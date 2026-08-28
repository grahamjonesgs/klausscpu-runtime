/*
 * main.c — AMP core 2, P3 image: LiteEth + lwIP bring-up to a ping.
 *
 * Runs on the second pipeline_core (effective 50 MHz) from the DDR text
 * window, data in local BRAM.  Core 1 (programs/core1_amp_host.c) loads and
 * starts this image, hands it LiteEth (C2_ETH_OWNER=1) and forwards this
 * console (the log FIFO, UART-compatible at 0xF001) to the real UART.
 *
 * lwIP NO_SYS=1 polling loop, same shape as baremetal/programs/lwip_demo.c.
 * P4: + the VNC server (vnc_c2.c) serving core 1's framebuffer (amp_proto.h).  sys_now() reads the clock_ms
 * mirror at 0xF00F_0040.
 */
#include <stdio.h>
#include <stdint.h>

#include "../mmio.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/timeouts.h"
#include "lwip/ip4_addr.h"
#include "netif/ethernet.h"
#include "../lwip_port/ethernetif.h"
#include "../src/eth.h"
#include "../amp/amp_proto.h"
#include "vnc_c2.h"

#define DHCP_TIMEOUT_MS   15000u
#define HEARTBEAT_MS      5000u

static struct netif g_netif;
static volatile int g_ip_assigned;

static void netif_status_cb(struct netif *nif)
{
    if (netif_is_up(nif) && !ip4_addr_isany_val(*netif_ip4_addr(nif))) {
        g_ip_assigned = 1;
        printf("core2: IP %s\n", ip4addr_ntoa(netif_ip4_addr(nif)));
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("core2: lwIP bring-up (AMP P3), clock_ms=%lu\n",
           (unsigned long)REG_CLOCK_MS);

    eth_init();                       /* PHY reset + MDIO + event enables */
    lwip_init();

    ip4_addr_t ipaddr, netmask, gw;
    IP4_ADDR(&ipaddr,  0, 0, 0, 0);
    IP4_ADDR(&netmask, 0, 0, 0, 0);
    IP4_ADDR(&gw,      0, 0, 0, 0);
    netif_add(&g_netif, &ipaddr, &netmask, &gw,
              NULL, ethernetif_init, ethernet_input);
    netif_set_default(&g_netif);
    netif_set_status_callback(&g_netif, netif_status_cb);
    netif_set_up(&g_netif);

    printf("core2: DHCP...\n");
    dhcp_start(&g_netif);
    uint64_t deadline = REG_CLOCK_MS + DHCP_TIMEOUT_MS;
    while (!g_ip_assigned && REG_CLOCK_MS < deadline) {
        ethernetif_input(&g_netif);
        sys_check_timeouts();
    }
    if (!g_ip_assigned) {
        printf("core2: DHCP timeout — static 192.168.68.60\n");
        dhcp_stop(&g_netif);
        IP4_ADDR(&ipaddr,  192, 168, 68, 60);
        IP4_ADDR(&netmask, 255, 255, 255, 0);
        IP4_ADDR(&gw,      192, 168, 68, 1);
        netif_set_addr(&g_netif, &ipaddr, &netmask, &gw);
    }
    printf("core2: up — ping me\n");
    /* P4: wait for core 1 to publish the framebuffer descriptor, then serve. */
    while (AMP_FB_DESC->magic != AMP_FB_MAGIC) {
        ethernetif_input(&g_netif);
        sys_check_timeouts();
    }
    /* P4 diagnostic: cost of uncached DDR loads through the window. */
    {
        const volatile uint64_t *p64 = (const volatile uint64_t *)(uintptr_t)AMP_FB_DESC->fb_base;
        const volatile uint16_t *p16 = (const volatile uint16_t *)(uintptr_t)AMP_FB_DESC->fb_base;
        uint64_t acc = 0, t0 = REG_CLOCK_MS;
        for (int i = 0; i < 16384; i++) acc += p64[i];          /* 128 KB */
        uint64_t t1 = REG_CLOCK_MS;
        for (int i = 0; i < 16384; i++) acc += p16[i];          /* 32 KB  */
        uint64_t t2 = REG_CLOCK_MS;
        printf("core2: ddr 16384 x u64 loads = %u ms\n", (unsigned)(t1 - t0));
        printf("core2: ddr 16384 x u16 loads = %u ms (acc %u)\n", (unsigned)(t2 - t1), (unsigned)acc);
        t0 = REG_CLOCK_MS;
        for (int i = 0; i < 16384; i++) acc += ((volatile uint64_t *)0x10000)[i & 1023];  /* BRAM */
        printf("core2: bram 16384 x u64 loads = %u ms\n", (unsigned)(REG_CLOCK_MS - t0));
    }
    printf("core2: fb %ux%u @0x%08x stride %u\n", AMP_FB_DESC->width,
           AMP_FB_DESC->height, (unsigned)AMP_FB_DESC->fb_base,
           (unsigned)AMP_FB_DESC->stride);
    vnc_c2_init();

    uint64_t next_beat = REG_CLOCK_MS + HEARTBEAT_MS;
    unsigned beats = 0;
    for (;;) {
        uint64_t t0 = REG_CLOCK_MS;
        ethernetif_input(&g_netif);
        sys_check_timeouts();
        AMP_FB_DESC->prof_rx_ms += (uint32_t)(REG_CLOCK_MS - t0);
        vnc_c2_poll();
        if (REG_CLOCK_MS >= next_beat) {
            amp_fb_desc_t *d = AMP_FB_DESC;
            next_beat += HEARTBEAT_MS;
            /* <=2 args per printf: multi-arg lines garble on this platform */
            printf("core2: alive %u upd=%u\n", ++beats, (unsigned)d->updates);
            printf("core2: boot=%u vnc=%d\n", (unsigned)d->pad1, vnc_c2_status());
            printf("core2: prof enc=%u tx=%u\n", (unsigned)d->prof_enc_ms,
                   (unsigned)d->prof_tx_ms);
            printf("core2: prof rx=%u (ms per 5 s)\n", (unsigned)d->prof_rx_ms);
            d->prof_enc_ms = 0; d->prof_tx_ms = 0; d->prof_rx_ms = 0;
        }
    }
    return 0;
}
