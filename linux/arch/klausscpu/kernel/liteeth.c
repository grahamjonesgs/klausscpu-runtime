// SPDX-License-Identifier: GPL-2.0
/*
 * The board's LiteEth MAC (CSRs at MMIO 0xF006_0000, 32-bit, 4 bytes
 * apart; slot SRAM at 0xF008_0000: RX0/RX1 then TX0/TX1, 2 KiB each) and its
 * LAN8720A PHY (bit-banged MDIO), as eth0.  The register layout is
 * mainline LiteEth's, but that driver needs a device tree; the board
 * details follow the Zephyr port's eth_klausscpu.c:
 *  - TX only from slot 0 (frames in slot 1 do not leave), padded to 60
 *    bytes (the MAC does not pad runts and switches drop them);
 *  - RX raises INT_SRC_ETH (level) while RX_EV_PENDING is set; writing 1
 *    to it releases the slot.  RX runs in NAPI with the event disabled.
 * The probe checks the SoC controller's scratch register (reset value
 * 0x12345678): an emulator without the MAC reads 0 and gets no eth0.
 *
 * With CPU_FEATURES[24] (klaussCPU HW_PERF_PLAN.md §3) the MAC has more RX
 * slots, opt-in through ETH_RX_SLOTS (0xF007_0000: [3:0] the count in use,
 * reset 2; [11:8] the maximum): the probe asks for them all before RX is
 * enabled.  A write to ETH_RX_SLOTS flushes the MAC's RX state (CTRL_RESET
 * is not connected in this core; the probe's pulse of it does nothing).
 * The 0xF007 window exists only with the feature bit.  Slots 0 and 1 stay
 * at 0xF008_0000/0800 and TX0 at
 * 0xF008_1000; slot k >= 2 is at 0xF008_1000 + k * 0x800.  Two slots
 * overflow while the CPU drains one at line rate (2192 TCP retransmits in
 * 15 s of iperf3 into the board).  RX_ERRORS counts the frames the MAC
 * dropped, as rx_missed_errors.
 */
#include <linux/etherdevice.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/moduleparam.h>
#include <linux/io.h>
#include <linux/netdevice.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include <asm/klausscpu.h>
#include <asm/setup.h>

#define ETH_CSR		(MMIO_BASE + 0x00060000UL)
#define ETH_SRAM	(MMIO_BASE + 0x00080000UL)
#define CTRL_RESET	0x0000
#define CTRL_SCRATCH	0x0004
#define PHY_RESET	0x0800
#define MDIO_W		0x0804
#define MDIO_R		0x0808
#define RX_SLOT		0x1000
#define RX_LENGTH	0x1004
#define RX_EV_PENDING	0x1010
#define RX_EV_ENABLE	0x1014
#define TX_START	0x1018
#define TX_READY	0x101c
#define TX_SLOT		0x1024
#define TX_LENGTH	0x1028
#define TX_EV_PENDING	0x1030
#define TX_EV_ENABLE	0x1034
#define RX_ERRORS	0x1008		/* frames dropped by the MAC, free-running */
#define ETH_RX_SLOTS	(MMIO_BASE + 0x00070000UL)
#define CPU_FEAT_ETH_SLOTS	(1UL << 24)

#define SLOT_SIZE	2048
#define TX_SLOT0	(ETH_SRAM + 2 * SLOT_SIZE)
#define SCRATCH_RESET	0x12345678

#define MDIO_MDC	1
#define MDIO_OE		2
#define MDIO_OUT	4
#define PHY_ADDR	1
#define PHY_BMCR	0
#define PHY_BMSR	1
#define BMSR_LINK	(1 << 2)

struct kl_eth {
	struct net_device *ndev;
	struct napi_struct napi;
	struct delayed_work link_work;
	spinlock_t tx_lock;
	unsigned int rx_slots;
	u32 rx_errors_base;
};

/* liteeth.rx_slots=N: at most N RX slots (2..8; 0, the default, = all). */
static unsigned int rx_slots_max;
module_param_named(rx_slots, rx_slots_max, uint, 0444);

/* RX slot k's SRAM: 0 and 1 below TX0, the rest above it. */
static unsigned long rx_slot_addr(unsigned int k)
{
	return ETH_SRAM + (k < 2 ? k * SLOT_SIZE : 0x1000 + k * SLOT_SIZE);
}

static void __iomem *csr(unsigned int off)
{
	return (void __iomem *)(ETH_CSR + off);
}

/* ── MDIO (bit-banged) ── */

static void mdio_bit(int bit)
{
	u32 w = MDIO_OE | (bit ? MDIO_OUT : 0);

	writel(w, csr(MDIO_W));
	ndelay(400);
	writel(w | MDIO_MDC, csr(MDIO_W));
	ndelay(400);
	writel(w, csr(MDIO_W));
	ndelay(400);
}

static int mdio_in(void)
{
	int bit;

	writel(0, csr(MDIO_W));
	ndelay(400);
	writel(MDIO_MDC, csr(MDIO_W));
	ndelay(400);
	bit = readl(csr(MDIO_R)) & 1;
	writel(0, csr(MDIO_W));
	ndelay(400);
	return bit;
}

static void mdio_head(int op, int reg)
{
	int i;

	for (i = 0; i < 32; i++)
		mdio_bit(1);
	mdio_bit(0);
	mdio_bit(1);		/* start */
	mdio_bit(op == 2);	/* 10 read, 01 write */
	mdio_bit(op == 1);
	for (i = 4; i >= 0; i--)
		mdio_bit((PHY_ADDR >> i) & 1);
	for (i = 4; i >= 0; i--)
		mdio_bit((reg >> i) & 1);
}

static u16 mdio_read(int reg)
{
	u16 v = 0;
	int i;

	mdio_head(2, reg);
	mdio_in();		/* turnaround */
	for (i = 15; i >= 0; i--)
		v |= mdio_in() << i;
	writel(0, csr(MDIO_W));
	return v;
}

static void mdio_write(int reg, u16 val)
{
	int i;

	mdio_head(1, reg);
	mdio_bit(1);
	mdio_bit(0);		/* turnaround */
	for (i = 15; i >= 0; i--)
		mdio_bit((val >> i) & 1);
	writel(0, csr(MDIO_W));
}

/* ── slot SRAM copies, 32 bits at a time ── */

static void sram_read(u8 *dst, unsigned long src, unsigned int n)
{
	unsigned int i = 0;

	for (; i + 4 <= n; i += 4)
		put_unaligned_le32(__raw_readl((void __iomem *)(src + i)), dst + i);
	for (; i < n; i++)
		dst[i] = __raw_readb((void __iomem *)(src + i));
}

static void sram_write(unsigned long dst, const u8 *src, unsigned int n)
{
	unsigned int i = 0;

	for (; i + 4 <= n; i += 4)
		__raw_writel(get_unaligned_le32(src + i), (void __iomem *)(dst + i));
	for (; i < n; i++)
		__raw_writeb(src[i], (void __iomem *)(dst + i));
}

/* ── data path ── */

static netdev_tx_t kl_eth_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct kl_eth *priv = netdev_priv(ndev);
	unsigned int len = skb->len;
	unsigned long flags;
	int spins = 0;

	if (len > SLOT_SIZE) {
		ndev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	if (skb_put_padto(skb, ETH_ZLEN)) {	/* frees skb on failure */
		ndev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}
	spin_lock_irqsave(&priv->tx_lock, flags);
	/* One slot: wait for the previous frame (~120 us at 100 Mb/s). */
	while (!(readl(csr(TX_READY)) & 1)) {
		if (++spins > 100000) {
			spin_unlock_irqrestore(&priv->tx_lock, flags);
			ndev->stats.tx_errors++;
			dev_kfree_skb_any(skb);
			return NETDEV_TX_OK;
		}
		cpu_relax();
	}
	sram_write(TX_SLOT0, skb->data, skb->len);
	writel(0, csr(TX_SLOT));
	writel(skb->len, csr(TX_LENGTH));
	writel(1, csr(TX_START));
	spin_unlock_irqrestore(&priv->tx_lock, flags);

	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += len;
	dev_consume_skb_any(skb);
	return NETDEV_TX_OK;
}

static int kl_eth_poll(struct napi_struct *napi, int budget)
{
	struct kl_eth *priv = container_of(napi, struct kl_eth, napi);
	struct net_device *ndev = priv->ndev;
	int done = 0;

	while (done < budget && (readl(csr(RX_EV_PENDING)) & 1)) {
		unsigned int slot = readl(csr(RX_SLOT)) % priv->rx_slots;
		unsigned int len = readl(csr(RX_LENGTH));
		struct sk_buff *skb;

		if (len < ETH_HLEN || len > SLOT_SIZE) {
			ndev->stats.rx_length_errors++;
			writel(1, csr(RX_EV_PENDING));
			continue;
		}
		skb = netdev_alloc_skb_ip_align(ndev, len);
		if (!skb) {
			ndev->stats.rx_dropped++;
			writel(1, csr(RX_EV_PENDING));
			continue;
		}
		sram_read(skb_put(skb, len), rx_slot_addr(slot), len);
		writel(1, csr(RX_EV_PENDING));		/* release the slot */
		skb->protocol = eth_type_trans(skb, ndev);
		ndev->stats.rx_packets++;
		ndev->stats.rx_bytes += len;
		netif_receive_skb(skb);
		done++;
	}
	if (done < budget && napi_complete_done(napi, done))
		writel(1, csr(RX_EV_ENABLE));
	return done;
}

static irqreturn_t kl_eth_irq(int irq, void *dev_id)
{
	struct kl_eth *priv = netdev_priv(dev_id);

	if (!(readl(csr(RX_EV_PENDING)) & 1))
		return IRQ_NONE;
	writel(0, csr(RX_EV_ENABLE));	/* the level drops; NAPI takes over */
	napi_schedule(&priv->napi);
	return IRQ_HANDLED;
}

static void kl_eth_link(struct work_struct *work)
{
	struct kl_eth *priv = container_of(to_delayed_work(work), struct kl_eth, link_work);
	bool up = mdio_read(PHY_BMSR) & BMSR_LINK;

	if (up != netif_carrier_ok(priv->ndev)) {
		if (up)
			netif_carrier_on(priv->ndev);
		else
			netif_carrier_off(priv->ndev);
		netdev_info(priv->ndev, "link %s\n", up ? "up" : "down");
	}
	schedule_delayed_work(&priv->link_work, HZ);
}

static int kl_eth_open(struct net_device *ndev)
{
	struct kl_eth *priv = netdev_priv(ndev);
	int err;

	writel(0, csr(TX_EV_ENABLE));
	writel(1, csr(TX_EV_PENDING));
	err = request_irq(ndev->irq, kl_eth_irq, 0, ndev->name, ndev);
	if (err)
		return err;
	napi_enable(&priv->napi);
	netif_carrier_off(ndev);
	schedule_delayed_work(&priv->link_work, 0);
	writel(1, csr(RX_EV_ENABLE));
	netif_start_queue(ndev);
	return 0;
}

static int kl_eth_stop(struct net_device *ndev)
{
	struct kl_eth *priv = netdev_priv(ndev);

	netif_stop_queue(ndev);
	writel(0, csr(RX_EV_ENABLE));
	cancel_delayed_work_sync(&priv->link_work);
	napi_disable(&priv->napi);
	free_irq(ndev->irq, ndev);
	return 0;
}

static void kl_eth_get_stats64(struct net_device *ndev, struct rtnl_link_stats64 *stats)
{
	struct kl_eth *priv = netdev_priv(ndev);

	netdev_stats_to_stats64(stats, &ndev->stats);
	stats->rx_missed_errors = readl(csr(RX_ERRORS)) - priv->rx_errors_base;
}

static const struct net_device_ops kl_eth_ops = {
	.ndo_open		= kl_eth_open,
	.ndo_stop		= kl_eth_stop,
	.ndo_start_xmit		= kl_eth_xmit,
	.ndo_get_stats64	= kl_eth_get_stats64,
	.ndo_set_mac_address	= eth_mac_addr,
	.ndo_validate_addr	= eth_validate_addr,
};

static int __init kl_eth_init(void)
{
	static const u8 mac[ETH_ALEN] = { 0x00, 0xab, 0xcd, 0x00, 0x00, 0x01 };
	struct net_device *ndev;
	struct kl_eth *priv;
	u32 scratch = readl(csr(CTRL_SCRATCH));
	unsigned int slots = 2;
	int err;

	if (scratch != SCRATCH_RESET) {
		pr_info("klausscpu-eth: no LiteEth (scratch %08x)\n", scratch);
		return 0;
	}

	/* All the RX slots there are, before RX is enabled (the write flushes RX). */
	if (klausscpu_cpu_features & CPU_FEAT_ETH_SLOTS) {
		unsigned int want = (readl((void __iomem *)ETH_RX_SLOTS) >> 8) & 0xf;

		if (rx_slots_max && rx_slots_max < want)
			want = rx_slots_max;
		writel(want, (void __iomem *)ETH_RX_SLOTS);
		slots = readl((void __iomem *)ETH_RX_SLOTS) & 0xf;
		if (slots < 2)
			slots = 2;
	}
	writel(1, csr(CTRL_RESET));
	mdelay(10);
	writel(0, csr(CTRL_RESET));
	writel(1, csr(PHY_RESET));
	mdelay(30);
	writel(0, csr(PHY_RESET));
	mdelay(50);
	mdio_write(PHY_BMCR, 0x3300);	/* 100 Mb/s, autonegotiate, restart */
	writel(0, csr(RX_EV_ENABLE));
	writel(1, csr(RX_EV_PENDING));

	ndev = alloc_etherdev(sizeof(*priv));
	if (!ndev)
		return -ENOMEM;
	priv = netdev_priv(ndev);
	priv->ndev = ndev;
	priv->rx_slots = slots;
	priv->rx_errors_base = readl(csr(RX_ERRORS));
	spin_lock_init(&priv->tx_lock);
	INIT_DELAYED_WORK(&priv->link_work, kl_eth_link);
	netif_napi_add(ndev, &priv->napi, kl_eth_poll);
	ndev->netdev_ops = &kl_eth_ops;
	ndev->irq = klausscpu_int2_irq(KL_INT2_ETH);	/* source 2, or its cascade */
	ndev->max_mtu = ETH_DATA_LEN;
	eth_hw_addr_set(ndev, mac);
	err = register_netdev(ndev);
	if (err) {
		free_netdev(ndev);
		return err;
	}
	netdev_info(ndev, "LiteEth at %08lx, MAC %pM, PHY BMSR %04x, %u RX slots\n", ETH_CSR, mac,
		    mdio_read(PHY_BMSR), slots);
	return 0;
}
device_initcall(kl_eth_init);
