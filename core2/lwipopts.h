// lwipopts.h — lwIP configuration for AMP CORE 2 (NO_SYS=1, bare-metal).
//
// Shadows lwip_port/lwipopts.h for the core-2 build (core2/ is first on the
// include path).  P4 sizing for the 128 KB local BRAM that holds core 2's
// data/bss/heap/stack: lwIP heap 64 KB (the TCP send buffer lives there —
// P4a measured the frame rate as window/RTT-bound, so the window is 46 KB)
// + 10 pool pbufs (16 KB) + TCP segments/pcbs; ~16 KB stack at the top.
// TCP is ON (VNC).  See AMP_CORE2_PLAN.md §4/§8.
#ifndef LWIP_LWIPOPTS_H
#define LWIP_LWIPOPTS_H

#define LWIP_NO_UNISTD_H            1
int rand(void);
#define LWIP_RAND() ((u32_t)rand())

#define NO_SYS                      1
#define SYS_LIGHTWEIGHT_PROT        0
#define LWIP_NETCONN                0
#define LWIP_SOCKET                 0

#define MEM_ALIGNMENT               8       // 64-bit pointer width on KlaussCPU
#define MEM_SIZE                    (64 * 1024)   // TCP send buffer lives here

#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ICMP                   1
#define LWIP_RAW                    0
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_DHCP                   1
#define LWIP_AUTOIP                 0
#define LWIP_IGMP                   0
#define LWIP_DNS                    0

#define MEMP_NUM_PBUF               16
#define MEMP_NUM_UDP_PCB            2
#define MEMP_NUM_TCP_PCB            2
#define MEMP_NUM_TCP_PCB_LISTEN     1
#define MEMP_NUM_TCP_SEG            96
#define MEMP_NUM_SYS_TIMEOUT        12
#define PBUF_POOL_SIZE              10
#define PBUF_POOL_BUFSIZE           1600

#define TCP_MSS                     1460
#define TCP_WND                     (4 * TCP_MSS)
#define TCP_SND_BUF                 (32 * TCP_MSS)  // 46 KB in flight per RTT
#define TCP_SND_QUEUELEN            80
#define TCP_QUEUE_OOSEQ             0
#define TCP_OVERSIZE                TCP_MSS

#define ARP_TABLE_SIZE              4
#define ARP_MAXAGE                  300

#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_LOOPBACK         0
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1

#define LWIP_CHKSUM_ALGORITHM       1
#define CHECKSUM_GEN_IP             1
#define CHECKSUM_GEN_UDP            1
#define CHECKSUM_GEN_TCP            1
#define CHECKSUM_GEN_ICMP           1
#define CHECKSUM_CHECK_IP           0
#define CHECKSUM_CHECK_UDP          0
#define CHECKSUM_CHECK_TCP          0

#define LWIP_STATS                  0
#define LWIP_STATS_DISPLAY          0
#define LWIP_DBG_MIN_LEVEL          LWIP_DBG_LEVEL_ALL
#define LWIP_DBG_TYPES_ON           LWIP_DBG_OFF

#endif // LWIP_LWIPOPTS_H
