// SPDX-License-Identifier: GPL-2.0-only
/* MT6739 connectivity subsystem (CONSYS) power control and WMT firmware bring-up. */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/crc16.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kfifo.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/sizes.h>
#include <linux/soc/mediatek/mt6739-consys.h>
#include <linux/unaligned.h>
#include <linux/wait.h>

#define CONSYS_HW_ID		0x008
#define CONSYS_HW_ID_MT6739	0x699
#define CONSYS_ACR		0x110
#define CONSYS_ACR_MBIST	BIT(18)
#define CONSYS_CPUPCR		0x160

#define INFRA_CONN_EMI_REMAP		0x380
#define INFRA_CONN_EMI_REMAP_EN		BIT(13)
#define INFRA_CONN_EMI_REMAP_BASE	GENMASK(12, 0)

#define BTIF_RBR		0x00
#define BTIF_THR		0x00
#define BTIF_IER		0x04
#define BTIF_IER_RXFEN		BIT(0)
#define BTIF_FIFOCTRL		0x08
#define BTIF_FIFOCTRL_CLR_RX	BIT(1)
#define BTIF_FIFOCTRL_CLR_TX	BIT(2)
#define BTIF_FAKELCR		0x0c
#define BTIF_LSR		0x14
#define BTIF_LSR_DR		BIT(0)
#define BTIF_LSR_THRE		BIT(5)
#define BTIF_DMA_EN		0x4c
#define BTIF_DMA_EN_AUTORST	BIT(2)
#define BTIF_TRI_LVL		0x60
#define BTIF_TRI_LVL_TX		GENMASK(3, 0)
#define BTIF_TRI_LVL_RX		GENMASK(6, 4)
#define BTIF_HANDSHAKE		0x6c
#define BTIF_HANDSHAKE_EN	BIT(0)

#define TOPRGU_SWSYSRST		0x018
#define TOPRGU_KEY		0x88000000
#define TOPRGU_CONN_RST		BIT(12)

#define STP_TYPE_WMT		4
#define WMT_PATCH_HDR		28
#define WMT_PATCH_FRAG		1000
#define WMT_TIMEOUT_MS		2000

struct mt6739_consys {
	struct device *dev;
	void __iomem *base;
	void __iomem *rgu;
	struct regulator *vcn18;
	struct regulator *vcn28;
	struct regulator *vcn33_bt;
	struct regulator *vcn33_wifi;
	struct regmap *infracfg;
	phys_addr_t emi_base;
	void __iomem *btif;
	struct clk *btif_clk;
	DECLARE_KFIFO(rx, u8, 4096);
	spinlock_t rx_lock;
	wait_queue_head_t rx_wait;
	u8 rxbuf[8192];
	size_t rxlen;
	u8 evt[256];
	bool stp_full;
	u8 txseq, txack, rxseq;
	struct mutex lock;
	unsigned int users;
	bool debugfs_on;
};

static struct mt6739_consys *mt6739_consys;

static irqreturn_t mt6739_consys_btif_irq(int irq, void *data)
{
	struct mt6739_consys *cs = data;
	irqreturn_t ret = IRQ_NONE;
	u8 c;

	spin_lock(&cs->rx_lock);
	while (readl(cs->btif + BTIF_LSR) & BTIF_LSR_DR) {
		c = readl(cs->btif + BTIF_RBR);
		kfifo_put(&cs->rx, c);
		ret = IRQ_HANDLED;
	}
	spin_unlock(&cs->rx_lock);
	if (ret == IRQ_HANDLED)
		wake_up(&cs->rx_wait);
	return ret;
}

static void mt6739_consys_btif_init(struct mt6739_consys *cs)
{
	writel(0, cs->btif + BTIF_FAKELCR);
	writel(BTIF_HANDSHAKE_EN, cs->btif + BTIF_HANDSHAKE);
	writel(BTIF_FIFOCTRL_CLR_RX | BTIF_FIFOCTRL_CLR_TX, cs->btif + BTIF_FIFOCTRL);
	writel(0, cs->btif + BTIF_FIFOCTRL);
	writel(FIELD_PREP(BTIF_TRI_LVL_TX, 8) | FIELD_PREP(BTIF_TRI_LVL_RX, 1),
	       cs->btif + BTIF_TRI_LVL);
	writel(BTIF_DMA_EN_AUTORST, cs->btif + BTIF_DMA_EN);
	writel(BTIF_IER_RXFEN, cs->btif + BTIF_IER);
}

static int mt6739_consys_btif_put(struct mt6739_consys *cs, u8 c)
{
	u32 lsr;
	int ret;

	ret = readl_poll_timeout(cs->btif + BTIF_LSR, lsr, lsr & BTIF_LSR_THRE, 10, 100000);
	if (ret)
		return ret;
	writel(c, cs->btif + BTIF_THR);
	return 0;
}

static void mt6739_consys_cpu_reset(struct mt6739_consys *cs, bool assert)
{
	u32 val = readl(cs->rgu + TOPRGU_SWSYSRST);

	if (assert)
		val |= TOPRGU_CONN_RST;
	else
		val &= ~TOPRGU_CONN_RST;
	writel(val | TOPRGU_KEY, cs->rgu + TOPRGU_SWSYSRST);
}

static int mt6739_consys_power_on(struct mt6739_consys *cs)
{
	u32 id = 0;
	int i, ret;

	ret = regulator_enable(cs->vcn18);
	if (ret)
		return ret;
	ret = regulator_enable(cs->vcn33_bt);
	if (ret)
		goto err_vcn18;
	udelay(150);
	ret = regulator_enable(cs->vcn28);
	if (ret)
		goto err_vcn33;
	ret = regulator_enable(cs->vcn33_wifi);
	if (ret)
		goto err_vcn28;

	mt6739_consys_cpu_reset(cs, true);

	ret = pm_runtime_resume_and_get(cs->dev);
	if (ret)
		goto err_vcn33_wifi;

	udelay(10);
	for (i = 0; i < 10; i++) {
		id = readl(cs->base + CONSYS_HW_ID);
		if (id == CONSYS_HW_ID_MT6739)
			break;
		msleep(20);
	}
	dev_info(cs->dev, "hardware ID 0x%x after %d polls\n", id, i);
	if (id != CONSYS_HW_ID_MT6739) {
		ret = -ENODEV;
		goto err_pm;
	}

	ret = clk_prepare_enable(cs->btif_clk);
	if (ret)
		goto err_pm;
	mt6739_consys_btif_init(cs);

	writel(readl(cs->base + CONSYS_ACR) | CONSYS_ACR_MBIST, cs->base + CONSYS_ACR);
	mt6739_consys_cpu_reset(cs, false);
	msleep(20);

	dev_info(cs->dev, "CPU PC 0x%08x\n", readl(cs->base + CONSYS_CPUPCR));
	return 0;

err_pm:
	mt6739_consys_cpu_reset(cs, true);
	pm_runtime_put_sync(cs->dev);
err_vcn33_wifi:
	regulator_disable(cs->vcn33_wifi);
err_vcn28:
	regulator_disable(cs->vcn28);
err_vcn33:
	regulator_disable(cs->vcn33_bt);
err_vcn18:
	regulator_disable(cs->vcn18);
	return ret;
}

static void mt6739_consys_power_off(struct mt6739_consys *cs)
{
	writel(0, cs->btif + BTIF_IER);
	clk_disable_unprepare(cs->btif_clk);
	mt6739_consys_cpu_reset(cs, true);
	pm_runtime_put_sync(cs->dev);
	regulator_disable(cs->vcn33_wifi);
	regulator_disable(cs->vcn28);
	regulator_disable(cs->vcn33_bt);
	regulator_disable(cs->vcn18);
}

static int mt6739_consys_btif_send(struct mt6739_consys *cs, const u8 *p, size_t n)
{
	int ret = 0;

	while (!ret && n--)
		ret = mt6739_consys_btif_put(cs, *p++);
	return ret;
}

static int mt6739_consys_stp_send(struct mt6739_consys *cs, const u8 *payload, size_t len)
{
	u8 hdr[4], crc[2];
	int ret;

	if (cs->stp_full) {
		hdr[0] = 0x80 | cs->txseq << 3 | cs->txack;
		cs->txseq = (cs->txseq + 1) & 7;
		put_unaligned_le16(crc16(0, payload, len), crc);
	} else {
		hdr[0] = 0x80;
		put_unaligned_le16(0, crc);
	}
	hdr[1] = STP_TYPE_WMT << 4 | len >> 8;
	hdr[2] = len & 0xff;
	hdr[3] = cs->stp_full ? hdr[0] + hdr[1] + hdr[2] : 0;

	ret = mt6739_consys_btif_send(cs, hdr, sizeof(hdr));
	if (!ret)
		ret = mt6739_consys_btif_send(cs, payload, len);
	if (!ret)
		ret = mt6739_consys_btif_send(cs, crc, sizeof(crc));
	return ret;
}

static int mt6739_consys_stp_ack(struct mt6739_consys *cs)
{
	u8 hdr[4] = { 0x80 | cs->txack, 0, 0, 0x80 | cs->txack };

	return mt6739_consys_btif_send(cs, hdr, sizeof(hdr));
}

/* Returns the payload length of the next WMT packet, copied to cs->evt. */
static int mt6739_consys_wmt_recv(struct mt6739_consys *cs, unsigned int timeout_ms)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);
	u8 *b = cs->rxbuf;
	long left;
	int ret;

	for (;;) {
		while (cs->rxlen && b[0] != 0x80 && !(cs->stp_full && (b[0] & 0xc0) == 0x80))
			memmove(b, b + 1, --cs->rxlen);

		if (cs->rxlen >= 4) {
			size_t len = (b[1] & 0xf) << 8 | b[2];
			size_t copy = min(len, sizeof(cs->evt));
			unsigned int type = b[1] >> 4, seq = (b[0] >> 3) & 7;

			if (cs->stp_full && !len) {
				cs->rxlen -= 4;
				memmove(b, b + 4, cs->rxlen);
				continue;
			}
			if (cs->rxlen >= 6 + len) {
				memcpy(cs->evt, b + 4, copy);
				cs->rxlen -= 6 + len;
				memmove(b, b + 6 + len, cs->rxlen);
				if (cs->stp_full) {
					if (seq == cs->rxseq) {
						cs->txack = seq;
						cs->rxseq = (cs->rxseq + 1) & 7;
					}
					ret = mt6739_consys_stp_ack(cs);
					if (ret)
						return ret;
				}
				if (type == STP_TYPE_WMT)
					return copy;
				dev_dbg(cs->dev, "STP type %u: %*ph\n", type,
					(int)min_t(size_t, copy, 32), cs->evt);
				continue;
			}
		}

		ret = kfifo_out(&cs->rx, b + cs->rxlen, sizeof(cs->rxbuf) - cs->rxlen);
		if (ret) {
			cs->rxlen += ret;
			continue;
		}
		left = (long)(deadline - jiffies);
		if (left <= 0)
			return -ETIMEDOUT;
		wait_event_timeout(cs->rx_wait, !kfifo_is_empty(&cs->rx), left);
	}
}

static int mt6739_consys_wmt_cmd(struct mt6739_consys *cs, const char *what,
				 const u8 *cmd, size_t clen, const u8 *evt, size_t elen)
{
	int n;

	n = mt6739_consys_stp_send(cs, cmd, clen);
	if (n)
		return n;
	n = mt6739_consys_wmt_recv(cs, WMT_TIMEOUT_MS);
	if (n < 0) {
		dev_err(cs->dev, "WMT %s: %d\n", what, n);
		return n;
	}
	if (evt && (n != elen || memcmp(cs->evt, evt, elen))) {
		dev_err(cs->dev, "WMT %s: unexpected reply %*ph\n", what, min(n, 32), cs->evt);
		return -EIO;
	}
	return 0;
}

#define WMT_CMD(cs, what, c, e) mt6739_consys_wmt_cmd(cs, what, c, sizeof(c), e, sizeof(e))

static int mt6739_consys_wmt_read(struct mt6739_consys *cs, u32 addr, u32 *val)
{
	u8 c[20] = { 0x01, 0x08, 0x10, 0x00, 0x02, 0x01, 0x00, 0x01 };
	int ret;

	put_unaligned_le32(addr, c + 8);
	put_unaligned_le32(0xffffffff, c + 16);
	ret = mt6739_consys_stp_send(cs, c, sizeof(c));
	if (ret)
		return ret;
	ret = mt6739_consys_wmt_recv(cs, WMT_TIMEOUT_MS);
	if (ret < 0)
		return ret;
	if (ret != 16 || cs->evt[0] != 0x02 || cs->evt[1] != 0x08)
		return -EIO;
	*val = get_unaligned_le32(cs->evt + 12);
	return 0;
}

static int mt6739_consys_wmt_write(struct mt6739_consys *cs, u32 addr, u32 val, u32 mask)
{
	static const u8 evt[] = { 0x02, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x01 };
	u8 c[20] = { 0x01, 0x08, 0x10, 0x00, 0x01, 0x01, 0x00, 0x01 };

	put_unaligned_le32(addr, c + 8);
	put_unaligned_le32(val, c + 12);
	put_unaligned_le32(mask, c + 16);
	return WMT_CMD(cs, "write", c, evt);
}

static int mt6739_consys_wmt_patch(struct mt6739_consys *cs, const char *name)
{
	static const u8 evt[] = { 0x02, 0x01, 0x01, 0x00, 0x00 };
	static const u8 reset[] = { 0x01, 0x07, 0x01, 0x00, 0x04 };
	static const u8 reset_evt[] = { 0x02, 0x07, 0x01, 0x00, 0x00 };
	const struct firmware *fw;
	size_t off, n;
	u8 *frag;
	int ret;

	ret = request_firmware(&fw, name, cs->dev);
	if (ret)
		return ret;
	if (fw->size <= WMT_PATCH_HDR) {
		ret = -EINVAL;
		goto out_release;
	}
	frag = kmalloc(5 + WMT_PATCH_FRAG, GFP_KERNEL);
	if (!frag) {
		ret = -ENOMEM;
		goto out_release;
	}

	ret = mt6739_consys_wmt_write(cs, 0x02090240, 0, 0xffffffff);
	if (!ret)
		ret = mt6739_consys_wmt_write(cs, 0x020904c8,
					      fw->data[0x1a] << 16 | (u32)fw->data[0x1b] << 24,
					      0xffffffff);

	for (off = WMT_PATCH_HDR; !ret && off < fw->size; off += n) {
		n = min_t(size_t, fw->size - off, WMT_PATCH_FRAG);
		frag[0] = 0x01;
		frag[1] = 0x01;
		put_unaligned_le16(n + 1, frag + 2);
		frag[4] = off == WMT_PATCH_HDR ? 1 : off + n >= fw->size ? 3 : 2;
		memcpy(frag + 5, fw->data + off, n);
		ret = mt6739_consys_wmt_cmd(cs, name, frag, 5 + n, evt, sizeof(evt));
	}
	if (!ret)
		ret = WMT_CMD(cs, "reset", reset, reset_evt);

	kfree(frag);
out_release:
	release_firmware(fw);
	return ret;
}

struct mt6739_consys_wmt_step {
	const char *what;
	const u8 *cmd;
	size_t clen;
	const u8 *evt;
	size_t elen;
};

#define WMT_STEP(w, c, e) { w, c, sizeof(c), e, sizeof(e) }

static const u8 wmt_coex_evt[] = { 0x02, 0x10, 0x01, 0x00, 0x00 };
static const u8 wmt_coex_filter[] = {
	0x01, 0x10, 0x45, 0x00, 0x11, 0x00, 0x00, 0x01, 0x00, 0x11, 0x11, 0x16, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x63, 0x63, 0x63, 0x00, 0x39, 0x43, 0x63, 0x63, 0x02, 0x02, 0x03, 0x00, 0x01, 0x01, 0x01,
	0x01, 0x0e, 0x0e, 0x0e, 0x00, 0x0a, 0x0c, 0x0e, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const u8 wmt_coex_lte_freq[] = {
	0x01, 0x10, 0x21, 0x00, 0x12, 0xfc, 0x08, 0x15, 0x09, 0x2e, 0x09, 0x47, 0x09, 0xc4, 0x09, 0xd4,
	0x09, 0xe3, 0x09, 0x5a, 0x0a, 0x14, 0x09, 0x2d, 0x09, 0x46, 0x09, 0x60, 0x09, 0xd3, 0x09, 0xe2,
	0x09, 0x59, 0x0a, 0x8b, 0x0a,
};
static const u8 wmt_coex_lte_15[] = { 0x01, 0x10, 0x02, 0x00, 0x15, 0x01 };
static const u8 wmt_coex_ant[] = { 0x01, 0x10, 0x02, 0x00, 0x06, 0x01 };
static const u8 wmt_coex_wifi_path[] = { 0x01, 0x10, 0x03, 0x00, 0x1a, 0x00, 0x00 };
static const u8 wmt_coex_1b[] = { 0x01, 0x10, 0x12, 0x00, 0x1b, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
				  0, 0, 0, 0 };
static const u8 wmt_coex_1d[] = { 0x01, 0x10, 0x02, 0x00, 0x1d, 0x00 };
static const u8 wmt_co_clock[] = { 0x01, 0x0a, 0x02, 0x00, 0x08, 0x03 };
static const u8 wmt_co_clock_evt[] = { 0x02, 0x0a, 0x01, 0x00, 0x00 };
static const u8 wmt_fm_strap[] = { 0x01, 0x05, 0x02, 0x00, 0x02, 0x02 };
static const u8 wmt_fm_strap_evt[] = { 0x02, 0x05, 0x02, 0x00, 0x00, 0x02 };

static const struct mt6739_consys_wmt_step wmt_coex_lte[] = {
	WMT_STEP("coex filter", wmt_coex_filter, wmt_coex_evt),
	WMT_STEP("coex LTE frequencies", wmt_coex_lte_freq, wmt_coex_evt),
	WMT_STEP("coex LTE 0x15", wmt_coex_lte_15, wmt_coex_evt),
};

static const struct mt6739_consys_wmt_step wmt_coex[] = {
	WMT_STEP("coex antenna mode", wmt_coex_ant, wmt_coex_evt),
	WMT_STEP("coex Wi-Fi path", wmt_coex_wifi_path, wmt_coex_evt),
	WMT_STEP("coex 0x1b", wmt_coex_1b, wmt_coex_evt),
	WMT_STEP("coex 0x1d", wmt_coex_1d, wmt_coex_evt),
	WMT_STEP("co-clock", wmt_co_clock, wmt_co_clock_evt),
	WMT_STEP("FM strap", wmt_fm_strap, wmt_fm_strap_evt),
};

static int mt6739_consys_wmt_steps(struct mt6739_consys *cs,
				   const struct mt6739_consys_wmt_step *s, size_t n)
{
	int ret = 0;

	for (; !ret && n--; s++)
		ret = mt6739_consys_wmt_cmd(cs, s->what, s->cmd, s->clen, s->evt, s->elen);
	return ret;
}

/* The commands and values follow a trace of the stock WMT driver; see docs/WMT-PROTOCOL.md. */
static int mt6739_consys_wmt_init(struct mt6739_consys *cs)
{
	static const u8 query_stp[] = { 0x01, 0x04, 0x01, 0x00, 0x04 };
	static const u8 query_mand[] = { 0x02, 0x04, 0x06, 0x00, 0x00, 0x04, 0x11, 0x00, 0x00, 0x00 };
	static const u8 set_stp[] = { 0x01, 0x04, 0x05, 0x00, 0x03, 0xdf, 0x0e, 0x68, 0x01 };
	static const u8 set_stp_evt[] = { 0x02, 0x04, 0x02, 0x00, 0x00, 0x03 };
	static const u8 query_full[] = { 0x02, 0x04, 0x06, 0x00, 0x00, 0x04, 0xdf, 0x0e, 0x68, 0x01 };
	static const u8 cal[] = { 0x01, 0x14, 0x01, 0x00, 0x01 };
	u32 hw = 0, ver = 0, rom = 0;
	int ret;

	cs->rxlen = 0;
	cs->stp_full = false;
	cs->txseq = 0;
	cs->txack = 7;
	cs->rxseq = 0;

	ret = mt6739_consys_wmt_read(cs, 0x80000008, &hw);
	if (!ret)
		ret = mt6739_consys_wmt_read(cs, 0x80000000, &ver);
	if (!ret)
		ret = mt6739_consys_wmt_read(cs, 0x80000004, &rom);
	if (ret)
		return ret;
	dev_info(cs->dev, "WMT hardware 0x%x, version 0x%x, ROM 0x%x\n", hw, ver, rom);

	ret = WMT_CMD(cs, "query STP", query_stp, query_mand);
	if (!ret)
		ret = WMT_CMD(cs, "set STP", set_stp, set_stp_evt);
	if (ret)
		return ret;
	cs->stp_full = true;
	msleep(10);
	ret = WMT_CMD(cs, "query STP", query_stp, query_full);

	if (!ret)
		ret = mt6739_consys_wmt_write(cs, 0x80100060, 0, 0x00000f00);
	if (!ret)
		ret = mt6739_consys_wmt_write(cs, 0x80100060, 0, 0x000000f0);
	if (!ret)
		ret = mt6739_consys_wmt_write(cs, 0x80100060, 0, 0x00000008);
	if (!ret)
		ret = mt6739_consys_wmt_write(cs, 0x80000334, 0x00010000, 0xffffffff);
	if (!ret)
		ret = mt6739_consys_wmt_write(cs, 0x8000010c, 0x00844d59, 0xffffffff);
	if (!ret)
		ret = mt6739_consys_wmt_patch(cs, "ROMv2_lm_patch_1_1_hdr.bin");
	if (!ret)
		ret = mt6739_consys_wmt_patch(cs, "ROMv2_lm_patch_1_0_hdr.bin");
	if (!ret)
		ret = mt6739_consys_wmt_write(cs, 0x8000010c, 0x00844d00, 0xffffffff);
	if (!ret)
		ret = mt6739_consys_wmt_write(cs, 0x80000334, 0, 0xffffffff);
	if (ret)
		return ret;

	usleep_range(5000, 6000);
	ret = mt6739_consys_wmt_steps(cs, wmt_coex_lte, ARRAY_SIZE(wmt_coex_lte));
	if (ret)
		return ret;

	ret = mt6739_consys_stp_send(cs, cal, sizeof(cal));
	if (ret)
		return ret;
	ret = mt6739_consys_wmt_recv(cs, 3000);
	if (ret < 4 || cs->evt[0] != 0x02 || cs->evt[1] != 0x14) {
		dev_err(cs->dev, "WMT calibration failed: %d\n", ret);
		return ret < 0 ? ret : -EIO;
	}

	ret = mt6739_consys_wmt_steps(cs, wmt_coex, ARRAY_SIZE(wmt_coex));
	if (!ret)
		dev_info(cs->dev, "WMT firmware ready\n");
	return ret;
}

int mt6739_consys_get(void)
{
	struct mt6739_consys *cs = READ_ONCE(mt6739_consys);
	int ret = 0;

	if (!cs)
		return -EPROBE_DEFER;

	mutex_lock(&cs->lock);
	if (!cs->users) {
		kfifo_reset(&cs->rx);
		ret = mt6739_consys_power_on(cs);
		if (!ret) {
			ret = mt6739_consys_wmt_init(cs);
			if (ret)
				mt6739_consys_power_off(cs);
		}
	}
	if (!ret)
		cs->users++;
	mutex_unlock(&cs->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6739_consys_get);

void mt6739_consys_put(void)
{
	struct mt6739_consys *cs = READ_ONCE(mt6739_consys);

	if (!cs)
		return;

	mutex_lock(&cs->lock);
	if (cs->users && !--cs->users)
		mt6739_consys_power_off(cs);
	mutex_unlock(&cs->lock);
}
EXPORT_SYMBOL_GPL(mt6739_consys_put);

static ssize_t mt6739_consys_power_write(struct file *file, const char __user *buf,
					 size_t len, loff_t *ppos)
{
	struct mt6739_consys *cs = file->private_data;
	bool on;
	int ret;

	ret = kstrtobool_from_user(buf, len, &on);
	if (ret)
		return ret;

	if (on && !cs->debugfs_on) {
		ret = mt6739_consys_get();
		cs->debugfs_on = !ret;
	} else if (!on && cs->debugfs_on) {
		mt6739_consys_put();
		cs->debugfs_on = false;
	}
	return ret ?: len;
}

static const struct file_operations mt6739_consys_power_fops = {
	.open = simple_open,
	.write = mt6739_consys_power_write,
};

static ssize_t mt6739_consys_btif_write(struct file *file, const char __user *buf,
					size_t len, loff_t *ppos)
{
	struct mt6739_consys *cs = file->private_data;
	u8 data[256];
	size_t i, n = min(len, sizeof(data));
	int ret = 0;

	if (copy_from_user(data, buf, n))
		return -EFAULT;

	mutex_lock(&cs->lock);
	if (!cs->users)
		ret = -ENODEV;
	for (i = 0; !ret && i < n; i++)
		ret = mt6739_consys_btif_put(cs, data[i]);
	mutex_unlock(&cs->lock);
	return ret ?: n;
}

static ssize_t mt6739_consys_btif_read(struct file *file, char __user *buf,
				       size_t len, loff_t *ppos)
{
	struct mt6739_consys *cs = file->private_data;
	unsigned int copied;
	int ret;

	ret = kfifo_to_user(&cs->rx, buf, len, &copied);
	return ret ?: copied;
}

static const struct file_operations mt6739_consys_btif_fops = {
	.open = simple_open,
	.read = mt6739_consys_btif_read,
	.write = mt6739_consys_btif_write,
};

static void mt6739_consys_remove(struct platform_device *pdev)
{
	struct mt6739_consys *cs = platform_get_drvdata(pdev);

	debugfs_remove_recursive(debugfs_lookup("mt6739-consys", NULL));
	WRITE_ONCE(mt6739_consys, NULL);
	if (cs->users)
		mt6739_consys_power_off(cs);
}

static int mt6739_consys_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6739_consys *cs;
	struct resource *res, mem;
	struct dentry *dir;
	int irq, ret;

	cs = devm_kzalloc(dev, sizeof(*cs), GFP_KERNEL);
	if (!cs)
		return -ENOMEM;
	cs->dev = dev;
	INIT_KFIFO(cs->rx);
	spin_lock_init(&cs->rx_lock);
	init_waitqueue_head(&cs->rx_wait);
	mutex_init(&cs->lock);
	platform_set_drvdata(pdev, cs);

	cs->btif = devm_platform_ioremap_resource_byname(pdev, "btif");
	if (IS_ERR(cs->btif))
		return PTR_ERR(cs->btif);
	cs->btif_clk = devm_clk_get(dev, "btif");
	if (IS_ERR(cs->btif_clk))
		return dev_err_probe(dev, PTR_ERR(cs->btif_clk), "no btif clock\n");
	irq = platform_get_irq_byname(pdev, "btif");
	if (irq < 0)
		return irq;
	ret = devm_request_irq(dev, irq, mt6739_consys_btif_irq, 0, "mt6739-btif", cs);
	if (ret)
		return ret;

	cs->base = devm_platform_ioremap_resource_byname(pdev, "conn");
	if (IS_ERR(cs->base))
		return PTR_ERR(cs->base);

	/* The watchdog driver owns this region; only one reset register is used. */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "toprgu");
	if (!res)
		return -EINVAL;
	cs->rgu = devm_ioremap(dev, res->start, resource_size(res));
	if (!cs->rgu)
		return -ENOMEM;

	cs->vcn18 = devm_regulator_get(dev, "vcn18");
	if (IS_ERR(cs->vcn18))
		return PTR_ERR(cs->vcn18);
	cs->vcn28 = devm_regulator_get(dev, "vcn28");
	if (IS_ERR(cs->vcn28))
		return PTR_ERR(cs->vcn28);
	cs->vcn33_bt = devm_regulator_get(dev, "vcn33-bt");
	if (IS_ERR(cs->vcn33_bt))
		return PTR_ERR(cs->vcn33_bt);
	cs->vcn33_wifi = devm_regulator_get(dev, "vcn33-wifi");
	if (IS_ERR(cs->vcn33_wifi))
		return PTR_ERR(cs->vcn33_wifi);

	cs->infracfg = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,infracfg");
	if (IS_ERR(cs->infracfg))
		return dev_err_probe(dev, PTR_ERR(cs->infracfg), "no infracfg\n");

	ret = of_reserved_mem_region_to_resource(dev->of_node, 0, &mem);
	if (ret)
		return dev_err_probe(dev, ret, "no shared memory\n");
	if (resource_size(&mem) != SZ_2M || !IS_ALIGNED(mem.start, SZ_2M))
		return dev_err_probe(dev, -EINVAL, "shared memory must be 2 MiB, 2 MiB aligned\n");
	cs->emi_base = mem.start;
	regmap_update_bits(cs->infracfg, INFRA_CONN_EMI_REMAP,
			   INFRA_CONN_EMI_REMAP_EN | INFRA_CONN_EMI_REMAP_BASE,
			   INFRA_CONN_EMI_REMAP_EN |
			   FIELD_PREP(INFRA_CONN_EMI_REMAP_BASE, cs->emi_base >> 21));
	dev_info(dev, "shared memory at %pa\n", &cs->emi_base);

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	dir = debugfs_create_dir("mt6739-consys", NULL);
	debugfs_create_file("power", 0200, dir, cs, &mt6739_consys_power_fops);
	debugfs_create_file("btif", 0600, dir, cs, &mt6739_consys_btif_fops);

	WRITE_ONCE(mt6739_consys, cs);
	return 0;
}

static const struct of_device_id mt6739_consys_of_match[] = {
	{ .compatible = "mediatek,mt6739-consys" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6739_consys_of_match);

static struct platform_driver mt6739_consys_driver = {
	.probe = mt6739_consys_probe,
	.remove = mt6739_consys_remove,
	.driver = {
		.name = "mt6739-consys",
		.of_match_table = mt6739_consys_of_match,
	},
};
module_platform_driver(mt6739_consys_driver);

MODULE_DESCRIPTION("MediaTek MT6739 CONSYS power control and WMT bring-up");
MODULE_LICENSE("GPL");
