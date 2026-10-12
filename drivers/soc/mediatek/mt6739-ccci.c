// SPDX-License-Identifier: GPL-2.0-only
/* MT6739 MD1 modem power-on and CCCI handshake. */

#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/miscdevice.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/random.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/sizes.h>
#include <linux/skbuff.h>
#include <linux/timekeeping.h>
#include <linux/unaligned.h>
#include <linux/wwan.h>

#define CCIF_BUSY		0x04
#define CCIF_TCHNUM		0x0c
#define CCIF_RCHNUM		0x10
#define CCIF_ACK		0x14
#define CCIF_SRAM		0x100
#define CCIF_SRAM_SIZE		0x200
#define CCIF_SRAM_DL_HEADER	0x100
#define CCIF_SRAM_MD_FEATURE	0x110
#define CCIF_SRAM_UP_HEADER	0x1f0
#define CCIF_SRAM_AP_FEATURE	0x200
#define CCIF_CH_SRAM		15
#define CCIF_CH_EXCEPTION	16
#define CCIF_CH_EX_CLEARQ	17
#define CCIF_CH_EX_CLEARQ_DONE	18
#define CCIF_RINGQ_NUM		8

#define MD_FEATURE_PATTERN	0x49434343
#define AP_FEATURE_PATTERN	0x43434349
#define MD_FEATURE_COUNT	64
#define MD_INIT_CHK_ID		0x5555ffff
#define CCCI_CONTROL_RX		0
#define CCCI_CONTROL_TX		1
#define CCCI_AT_RX		10
#define CCCI_AT_TX		12
#define CCCI_AT_QUEUE		5
#define CCCI_RPC_RX		32
#define CCCI_RPC_TX		33

#define MD_INIT_START_BOOT	0
#define MD_EX			4
#define MD_EX_REC_OK		6
#define MD_EX_PASS		8

#define RX_PKT_MAX		SZ_8K
#define PORT_RX_MAX		64
#define RPC_MAX_ARGS		6
#define RPC_MAX_BUF		2048
#define RPC_RESP_ID		0xffff0000
#define RPC_GET_TDD_EINT_NUM	0x4001
#define RPC_GET_GPIO_NUM	0x4002
#define RPC_GET_ADC_NUM		0x4003
#define RPC_GET_EMI_CLK_TYPE	0x4004
#define RPC_GET_EINT_ATTR	0x4005
#define RPC_GET_GPIO_VAL	0x4006
#define RPC_GET_ADC_VAL		0x4007
#define RPC_GET_GPIO_ADC	0x4009
#define RPC_USIM2NFC		0x400a
#define RPC_LHIF_MAPPING	0x400d
#define RPC_DTSI_QUERY		0x400e
#define RPC_GPIO_ADC_IN_V2	425
#define RPC_GPIO_ADC_OUT	32
#define RPC_GPIO_ADC_OUT_V2	88
#define RPC_DTSI_OUT		68
#define FS_NO_OP		(-1)
#define FS_FUNC_FAIL		(-5)
#define ERR_SIM_HOT_PLUG_QUERY_STRING	(-11)

#define FEATURE_NOT_EXIST	0
#define FEATURE_NOT_SUPPORT	1
#define FEATURE_MUST		2
#define FEATURE_OPTIONAL	3
#define FEATURE_BACKWARD	4

enum {
	F_BOOT_INFO = 0,
	F_EXCEPTION_SHM = 1,
	F_CCIF_SHM = 2,
	F_HIF_DMA_REMAP = 5,
	F_RTC_32K_LESS = 6,
	F_RANDOM_SEED = 7,
	F_GPS_COCLOCK = 8,
	F_SBP_ID = 9,
	F_MISC_CCCI = 10,
	F_CLIB_TIME = 11,
	F_C2K = 12,
	F_MD_IMAGE_START = 13,
	F_CCB_SHM = 15,
	F_DHL_RAW_SHM = 16,
	F_NETD_SHM = 17,
	F_USB_SHM = 18,
	F_EE_AFTER_EPOF = 19,
	F_CCMNI_MTU = 20,
	F_LWA_SHM = 25,
	F_AUDIO_RAW_SHM = 26,
	F_MULTI_MD_MPU = 27,
	F_CCISM_EXP_SHM = 28,
	F_PHY_CAPTURE = 29,
	F_MD_CONSYS_SHM = 30,
};

#define SMEM_MD_VIEW		0x40000000
#define SMEM_EXCEPTION_SIZE	SZ_64K
#define SMEM_RUNTIME		0xe800
#define SMEM_RUNTIME_SIZE	0x800
#define SMEM_DBM		0xff50
#define SMEM_DBM_SIZE		160
#define SMEM_DBM_GUARD		0x44444444
#define SMEM_CCB_CTRL		0x18000
#define SMEM_NETD		0x19000
#define SMEM_USB		0x1a000
#define SMEM_AUDIO		0x1b000
#define SMEM_AUDIO_SIZE		(52 * SZ_1K)
#define SMEM_CCISM		0x28000
#define SMEM_CCISM_EXP		0xd8400
#define SMEM_CCISM_EXP_SIZE	(121 * SZ_1K)
#define SMEM_SIB		SZ_1M
#define CCB_DATA_SIZE		SZ_2M
#define CCMNI_MTU		(3584 - 16)

#define RING_HEADER		0xee0000ee
#define RING_FOOTER		0xff0000ff
#define RING_CTL_LEN		40
#define PKT_HEADER		0xaabbaabb
#define PKT_FOOTER		0xccddeeff

#define INFRA_CCIF_CG_CLR	0x08c
#define INFRA_RST0_SET		0x140
#define INFRA_RST0_CLR		0x144
#define INFRA_RST0_CLDMA_AO	BIT(6)
#define INFRA_RST1_SET		0x150
#define INFRA_RST1_CLR		0x154
#define INFRA_RST1_CLDMA_PD	BIT(2)
#define INFRA_RST1_CCIF		BIT(8)
#define INFRA_PROT_STA1		0x228
#define INFRA_PROT_EN_1_STA1	0x258
#define INFRA_PROT_SET		0x2a0
#define INFRA_PROT_CLR		0x2a4
#define INFRA_PROT_1_SET	0x2a8
#define INFRA_PROT_1_CLR	0x2ac
#define INFRA_PROT_MD1		(BIT(3) | BIT(4) | BIT(7))
#define INFRA_PROT_AP2MD	BIT(7)
#define INFRA_PROT_1_MD2PERI	BIT(6)
#define INFRA_AP2MD_DUMMY	0x370
#define INFRA_CLDMA_WAKEUP	0xc00
#define INFRA_SRCLKEN		0xf0c

#define SPM_PWR_STATUS		0x180
#define SPM_PWR_STATUS_2ND	0x184
#define SPM_MD1_PWR_CON		0x320
#define SPM_MD_SRAM_ISO_CON	0x394
#define SPM_MD_EXTRA_PWR_CON	0x398
#define PWR_RST_B		BIT(0)
#define PWR_ISO			BIT(1)
#define PWR_ON			BIT(2)
#define PWR_ON_2ND		BIT(3)
#define MD_PWR_CLK_DIS		BIT(5)
#define MD_SRAM_PDN		BIT(6)
#define PWR_STATUS_MD1		BIT(0)

#define TOPCKGEN_MD_CLK_MODE	GENMASK(9, 8)
#define AP_PLL_CON0_LPF		BIT(1)
#define DVFSRC_MD_SRCLKENA	0x18
#define DVFSRC_MD_SRCLKENA_EN	(3 << 26)

#define CLDMA_SO_CFG		0x400
#define CLDMA_SO_CFG_EN		BIT(0)
#define CLDMA_DL_MTU		0x418
#define CLDMA_MTU		3584

#define MD_CFG_BOOT_SLAVE	0x24
#define MD_RGU_WDT		0x100
#define MD_RGU_WDT_OFF		0x55000030

#define MD_PLL_DONE		0x62930000

#define LK_TAG_NAME_LEN		64

struct ccci_lk_info {
	u64 base;
	u32 size;
	s32 err;
	s32 version;
	s32 tag_num;
	u32 ld_flag;
	s32 ld_errno[4];
} __packed;

struct ccci_lk_tag {
	char name[LK_TAG_NAME_LEN];
	u32 data_offset;
	u32 data_size;
	u32 next_offset;
} __packed;

struct ccci_smem_layout {
	u64 base;
	u32 ap_md1_offset;
	u32 ap_md1_size;
	u32 ap_md3_offset;
	u32 ap_md3_size;
	u32 md1_md3_offset;
	u32 md1_md3_size;
	u32 total_size;
} __packed;

struct ccci_header {
	u32 data[2];
	u16 channel;
	u16 seq;
	u32 reserved;
} __packed;

struct ccci_runtime_feature {
	u8 id;
	u8 support;
	u8 reserved[2];
	u32 data_len;
} __packed;

struct ap_query_md_feature {
	u32 head;
	u8 feature[MD_FEATURE_COUNT];
	u32 share_memory_support;
	u32 ap_runtime_addr;
	u32 ap_runtime_size;
	u32 md_runtime_addr;
	u32 md_runtime_size;
	u32 noncached_mpu_addr;
	u32 noncached_mpu_size;
	u32 cached_mpu_addr;
	u32 cached_mpu_size;
	u32 reserved[12];
	u32 tail;
} __packed;

struct ccci_ring_ctl {
	u32 read;
	u32 write;
	u32 length;
};

struct mt6739_ccci_ring {
	struct ccci_ring_ctl *rx;
	struct ccci_ring_ctl *tx;
	u8 *rx_buf;
	u8 *tx_buf;
};

static const u32 ring_rx_size[CCIF_RINGQ_NUM] = {
	80 * SZ_1K, 80 * SZ_1K, 40 * SZ_1K, 80 * SZ_1K, 20 * SZ_1K, 20 * SZ_1K, 64 * SZ_1K, 0,
};
static const u32 ring_tx_size[CCIF_RINGQ_NUM] = {
	128 * SZ_1K, 40 * SZ_1K, 8 * SZ_1K, 40 * SZ_1K, 20 * SZ_1K, 20 * SZ_1K, 64 * SZ_1K, 0,
};
static const u32 ring_exp_size[CCIF_RINGQ_NUM] = {
	12 * SZ_1K, 32 * SZ_1K, 8 * SZ_1K, 0, 0, 0, 8 * SZ_1K, 0,
};

/* The AP side of the feature negotiation, as in the vendor driver for MT6739. */
static const u8 ap_feature_mask[MD_FEATURE_COUNT] = {
	[F_BOOT_INFO] = FEATURE_MUST,
	[F_EXCEPTION_SHM] = FEATURE_MUST,
	[F_CCIF_SHM] = FEATURE_MUST,
	[F_HIF_DMA_REMAP] = FEATURE_MUST,
	/* The board has no 32 kHz crystal (MT6357 RTC spare 0x5b8 bit 6 is 0). */
	[F_RTC_32K_LESS] = FEATURE_MUST,
	[F_RANDOM_SEED] = FEATURE_MUST,
	[F_GPS_COCLOCK] = FEATURE_MUST,
	[F_SBP_ID] = FEATURE_MUST,
	[F_MISC_CCCI] = FEATURE_MUST,
	[F_CLIB_TIME] = FEATURE_MUST,
	[F_C2K] = FEATURE_MUST,
	[F_MD_IMAGE_START] = FEATURE_OPTIONAL,
	/* CCB and DHL raw need the CCB control setup that ccci_mdinit does; not offered yet. */
	[F_NETD_SHM] = FEATURE_MUST,
	[F_USB_SHM] = FEATURE_MUST,
	[F_EE_AFTER_EPOF] = FEATURE_MUST,
	[F_CCMNI_MTU] = FEATURE_MUST,
	[F_LWA_SHM] = FEATURE_MUST,
	[F_AUDIO_RAW_SHM] = FEATURE_MUST,
	[F_MULTI_MD_MPU] = FEATURE_MUST,
	[F_CCISM_EXP_SHM] = FEATURE_MUST,
	[F_MD_CONSYS_SHM] = FEATURE_MUST,
};

struct mt6739_ccci;

struct mt6739_ccci_port {
	const char *name;
	u16 rx_ch;
	u16 tx_ch;
	u8 queue;
	struct mt6739_ccci *md;
	struct miscdevice misc;
	struct sk_buff_head rx;
	wait_queue_head_t wait;
};

static struct mt6739_ccci_port mt6739_ccci_ports[] = {
	{ .name = "ccci_fs", .rx_ch = 14, .tx_ch = 15, .queue = 4 },
};

struct mt6739_ccci {
	struct device *dev;
	void __iomem *cldma_ao;
	void __iomem *ap_ccif;
	void __iomem *md_ccif;
	void __iomem *boot_stats;
	void __iomem *md_cfg;
	void __iomem *md_rgu;
	void __iomem *md_pll;
	void __iomem *md_clksw;
	void __iomem *topckgen;
	void __iomem *apmixed;
	void __iomem *dvfsrc;
	struct regmap *infracfg;
	struct regmap *scpsys;
	struct clk_bulk_data *clks;
	int num_clks;
	struct regulator *vmodem;
	struct ccci_smem_layout smem_layout;
	u64 ccb_addr;
	u32 ccb_size;
	u32 cache_offset;
	u32 c2k_lte_mode;
	void *smem;
	struct mt6739_ccci_ring rings[CCIF_RINGQ_NUM];
	struct mt6739_ccci_ring exp_rings[CCIF_RINGQ_NUM];
	bool in_exception;
	struct debugfs_blob_wrapper smem_blob;
	struct debugfs_blob_wrapper ccb_blob;
	u32 ccism_size;
	spinlock_t tx_lock;
	u8 *rx_pkt;
	u8 *tx_pkt;
	struct mutex lock;
	bool started;
	bool ready;
	struct wwan_port *at_port;
	struct dentry *debugfs;
};

static int mt6739_ccci_parse_lk_info(struct mt6739_ccci *md)
{
	struct device *dev = md->dev;
	struct ccci_lk_info info;
	struct ccci_lk_tag tag;
	struct device_node *chosen;
	bool have_layout = false;
	void *area;
	u32 off = 0;
	int i, ret;

	chosen = of_find_node_by_path("/chosen");
	if (!chosen)
		return -ENODEV;
	ret = of_property_read_u8_array(chosen, "ccci,modem_info_v2", (u8 *)&info, sizeof(info));
	of_node_put(chosen);
	if (ret)
		return dev_err_probe(dev, ret, "no ccci,modem_info_v2 (modem not loaded by LK)\n");
	if (!info.base || info.err || info.version != 2 || info.size > SZ_64K)
		return dev_err_probe(dev, -ENODEV, "LK did not load the modem: base %llx err %d ver %d\n",
				     info.base, info.err, info.version);

	area = memremap(info.base, info.size, MEMREMAP_WC);
	if (!area)
		return -ENOMEM;

	for (i = 0; i < info.tag_num && off + sizeof(tag) <= info.size; i++) {
		void *data;

		memcpy(&tag, area + off, sizeof(tag));
		tag.name[LK_TAG_NAME_LEN - 1] = '\0';
		off = tag.next_offset;
		if (tag.data_offset + tag.data_size > info.size)
			continue;
		data = area + tag.data_offset;
		dev_dbg(dev, "LK tag %s: size %#x first word %#x\n", tag.name, tag.data_size,
			tag.data_size >= 4 ? get_unaligned_le32(data) : 0);

		if (!strcmp(tag.name, "smem_layout") && tag.data_size >= sizeof(md->smem_layout)) {
			memcpy(&md->smem_layout, data, sizeof(md->smem_layout));
			have_layout = true;
		} else if (!strcmp(tag.name, "ccb_info") && tag.data_size >= 12) {
			md->ccb_addr = get_unaligned_le64(data);
			md->ccb_size = get_unaligned_le32(data + 8);
		} else if (!strcmp(tag.name, "md1_smem_cahce_offset") && tag.data_size >= 4) {
			md->cache_offset = get_unaligned_le32(data);
		} else if (!strcmp(tag.name, "opt_c2k_lte_mode") && tag.data_size >= 4) {
			md->c2k_lte_mode = get_unaligned_le32(data);
		}
	}
	memunmap(area);

	if (!have_layout || !md->ccb_addr || !md->cache_offset)
		return dev_err_probe(dev, -ENODEV, "LK tags incomplete\n");
	dev_info(dev, "shared memory %#llx size %#x, AP-MD1 %#x+%#x, CCB %#llx+%#x, C2K mode %u\n",
		 md->smem_layout.base, md->smem_layout.total_size,
		 md->smem_layout.ap_md1_offset, md->smem_layout.ap_md1_size,
		 md->ccb_addr, md->ccb_size, md->c2k_lte_mode);
	return 0;
}

/* The CCIF SRAM takes only 32-bit accesses: a byte write clears the rest of the word. */
static void mt6739_ccci_sram_clear(void __iomem *dst, size_t len)
{
	size_t i;

	for (i = 0; i < len; i += 4)
		writel(0, dst + i);
}

static u32 mt6739_ccci_smem_md(struct mt6739_ccci *md, u32 off)
{
	return SMEM_MD_VIEW + ((md->smem_layout.base + md->smem_layout.ap_md1_offset) &
			       (SZ_32M - 1)) + off;
}

static u32 mt6739_ccci_ccb_md(struct mt6739_ccci *md)
{
	return SMEM_MD_VIEW + md->cache_offset + (md->ccb_addr & (SZ_1M - 1));
}

static u32 mt6739_ccci_create_rings(struct mt6739_ccci *md, u32 off, const u32 *rx_size,
				    const u32 *tx_size, struct mt6739_ccci_ring *rings)
{
	u32 start = off;
	int i;

	for (i = 0; i < CCIF_RINGQ_NUM; i++) {
		u32 len = RING_CTL_LEN + rx_size[i] + tx_size[i];
		u32 *buf = md->smem + off;
		struct ccci_ring_ctl *ctl = (void *)(buf + 2);

		memset(buf, 0, len);
		buf[0] = RING_HEADER;
		buf[1] = RING_HEADER;
		buf[len / 4 - 2] = RING_FOOTER;
		buf[len / 4 - 1] = RING_FOOTER;
		ctl[0].length = rx_size[i];
		ctl[1].length = tx_size[i];
		if (rings) {
			rings[i].rx = &ctl[0];
			rings[i].tx = &ctl[1];
			rings[i].rx_buf = (u8 *)&ctl[2];
			rings[i].tx_buf = rings[i].rx_buf + rx_size[i];
		}
		off += len;
	}
	return off - start;
}

static void mt6739_ccci_ring_copy(void *dst, const u8 *buf, u32 pos, u32 n, u32 len)
{
	u32 first = min(n, len - pos);

	memcpy(dst, buf + pos, first);
	memcpy(dst + first, buf, n - first);
}

static void mt6739_ccci_ring_put(u8 *buf, u32 pos, const void *src, u32 n, u32 len)
{
	u32 first = min(n, len - pos);

	memcpy(buf + pos, src, first);
	memcpy(buf, src + first, n - first);
}

static int mt6739_ccci_ring_tx(struct mt6739_ccci *md, int q, const void *data, u32 n)
{
	struct mt6739_ccci_ring *ring = md->in_exception ? &md->exp_rings[q] : &md->rings[q];
	u32 len = ring->tx->length, read, write, space, total = ALIGN(16 + n, 8);
	u32 head[2] = { PKT_HEADER, n }, foot[2] = { PKT_FOOTER, PKT_FOOTER };
	unsigned long flags;

	spin_lock_irqsave(&md->tx_lock, flags);
	read = READ_ONCE(ring->tx->read);
	write = READ_ONCE(ring->tx->write);
	space = (read + len - write - 1) % len;
	if (!len || total > space) {
		spin_unlock_irqrestore(&md->tx_lock, flags);
		return -EBUSY;
	}
	mt6739_ccci_ring_put(ring->tx_buf, write, head, sizeof(head), len);
	mt6739_ccci_ring_put(ring->tx_buf, (write + 8) % len, data, n, len);
	mt6739_ccci_ring_put(ring->tx_buf, (write + total - 8) % len, foot, sizeof(foot), len);
	dma_wmb();
	WRITE_ONCE(ring->tx->write, (write + total) % len);
	wmb();
	writel(BIT(q), md->ap_ccif + CCIF_BUSY);
	writel(q, md->ap_ccif + CCIF_TCHNUM);
	spin_unlock_irqrestore(&md->tx_lock, flags);
	return 0;
}

static void mt6739_ccci_rpc_put(u8 **p, u32 *argc, const void *data, u32 n)
{
	put_unaligned_le32(n, *p);
	memcpy(*p + 4, data, n);
	memset(*p + 4 + n, 0, ALIGN(n, 4) - n);
	*p += 4 + ALIGN(n, 4);
	(*argc)++;
}

static void mt6739_ccci_rpc(struct mt6739_ccci *md, int q, u8 *pkt, u32 n)
{
	struct ccci_header *hdr = (void *)pkt;
	u32 op = get_unaligned_le32(pkt + 16), argc = get_unaligned_le32(pkt + 20);
	const u8 *arg[RPC_MAX_ARGS] = {};
	u32 arg_len[RPC_MAX_ARGS] = {};
	u8 *reply = md->tx_pkt, *p = reply + 24;
	u32 off = 24, nout = 0, i;
	s32 err[3];
	u8 out[96];

	for (i = 0; i < argc && i < RPC_MAX_ARGS && off + 4 <= n; i++) {
		arg_len[i] = get_unaligned_le32(pkt + off);
		arg[i] = pkt + off + 4;
		off += 4 + ALIGN(arg_len[i], 4);
	}
	if (argc > RPC_MAX_ARGS || off > n) {
		dev_err(md->dev, "RPC %#x: bad arguments\n", op);
		return;
	}

	switch (op) {
	case RPC_GET_TDD_EINT_NUM:
	case RPC_GET_GPIO_NUM:
	case RPC_GET_ADC_NUM:
	case RPC_GET_GPIO_VAL:
	case RPC_GET_ADC_VAL:
		err[0] = FS_FUNC_FAIL;
		mt6739_ccci_rpc_put(&p, &nout, err, 4);
		mt6739_ccci_rpc_put(&p, &nout, err, 4);
		dev_info(md->dev, "RPC %#x %.*s: not available\n", op,
			 arg_len[0] ? (int)arg_len[0] : 0, arg[0] ? (const char *)arg[0] : "");
		break;
	case RPC_GET_EMI_CLK_TYPE:
		err[0] = err[1] = err[2] = FS_FUNC_FAIL;
		for (i = 0; i < 3; i++)
			mt6739_ccci_rpc_put(&p, &nout, &err[i], 4);
		break;
	case RPC_GET_EINT_ATTR:
		/* No SIM hot-plug EINT nodes, as in the stock DT. */
		err[0] = ERR_SIM_HOT_PLUG_QUERY_STRING;
		mt6739_ccci_rpc_put(&p, &nout, err, 4);
		mt6739_ccci_rpc_put(&p, &nout, err, 4);
		dev_info(md->dev, "RPC EINT attr %.*s type %u: none\n",
			 arg_len[0] ? (int)arg_len[0] : 0, arg[0] ? (const char *)arg[0] : "",
			 arg[2] && arg_len[2] >= 4 ? get_unaligned_le32(arg[2]) : 0);
		break;
	case RPC_GET_GPIO_ADC:
		err[0] = 0;
		i = arg_len[0] == RPC_GPIO_ADC_IN_V2 ? RPC_GPIO_ADC_OUT_V2 : RPC_GPIO_ADC_OUT;
		memset(out, 0xf, i);
		mt6739_ccci_rpc_put(&p, &nout, err, 4);
		mt6739_ccci_rpc_put(&p, &nout, out, i);
		dev_info(md->dev, "RPC GPIO/ADC (input %u bytes): not available\n", arg_len[0]);
		break;
	case RPC_LHIF_MAPPING:
	case RPC_USIM2NFC:
		err[0] = 0;
		mt6739_ccci_rpc_put(&p, &nout, err, 4);
		mt6739_ccci_rpc_put(&p, &nout, err, 4);
		dev_info(md->dev, "RPC %#x: %*ph\n", op, min_t(u32, arg_len[0], 16), arg[0]);
		break;
	case RPC_DTSI_QUERY:
		err[0] = 0;
		memset(out, 0xf, RPC_DTSI_OUT);
		mt6739_ccci_rpc_put(&p, &nout, err, 4);
		mt6739_ccci_rpc_put(&p, &nout, out, RPC_DTSI_OUT);
		dev_info(md->dev, "RPC DT query %.64s: none\n",
			 arg_len[0] >= 66 ? (const char *)arg[0] + 2 : "");
		break;
	default:
		err[0] = FS_NO_OP;
		mt6739_ccci_rpc_put(&p, &nout, err, 4);
		dev_warn(md->dev, "RPC %#x with %u arguments: not handled\n", op, argc);
		break;
	}

	memcpy(reply, hdr, sizeof(*hdr));
	hdr = (void *)reply;
	hdr->channel = CCCI_RPC_TX;
	hdr->data[1] = p - reply;
	put_unaligned_le32(op | RPC_RESP_ID, reply + 16);
	put_unaligned_le32(nout, reply + 20);
	if (mt6739_ccci_ring_tx(md, q, reply, p - reply))
		dev_err(md->dev, "RPC %#x: no room for the reply\n", op);
}

static void mt6739_ccci_at_rx(struct mt6739_ccci *md, const u8 *data, u32 n)
{
	struct sk_buff *skb;

	if (!md->at_port || !n)
		return;
	skb = alloc_skb(n, GFP_KERNEL);
	if (!skb)
		return;
	skb_put_data(skb, data, n);
	wwan_port_rx(md->at_port, skb);
}

static int mt6739_ccci_at_start(struct wwan_port *port)
{
	return 0;
}

static void mt6739_ccci_at_stop(struct wwan_port *port)
{
}

static int mt6739_ccci_at_tx(struct wwan_port *port, struct sk_buff *skb)
{
	struct mt6739_ccci *md = wwan_port_get_drvdata(port);
	struct ccci_header *hdr;
	int ret;

	if (!md->ready)
		return -ENODEV;
	if (skb_headroom(skb) < sizeof(*hdr) &&
	    pskb_expand_head(skb, sizeof(*hdr), 0, GFP_KERNEL))
		return -ENOMEM;
	hdr = skb_push(skb, sizeof(*hdr));
	memset(hdr, 0, sizeof(*hdr));
	hdr->channel = CCCI_AT_TX;
	hdr->data[1] = skb->len;
	ret = mt6739_ccci_ring_tx(md, CCCI_AT_QUEUE, skb->data, skb->len);
	if (ret) {
		skb_pull(skb, sizeof(*hdr));
		return ret;
	}
	consume_skb(skb);
	return 0;
}

static const struct wwan_port_ops mt6739_ccci_at_ops = {
	.start = mt6739_ccci_at_start,
	.stop = mt6739_ccci_at_stop,
	.tx = mt6739_ccci_at_tx,
};

static void mt6739_ccci_ctrl(struct mt6739_ccci *md, const struct ccci_header *hdr)
{
	switch (hdr->data[1]) {
	case MD_INIT_START_BOOT:
		if (hdr->reserved == MD_INIT_CHK_ID)
			break;
		dev_info(md->dev, "HS2: MD is ready\n");
		md->ready = true;
		if (!md->at_port) {
			md->at_port = wwan_create_port(md->dev, WWAN_PORT_AT, &mt6739_ccci_at_ops,
						       NULL, md);
			if (IS_ERR(md->at_port)) {
				dev_err(md->dev, "AT port: %pe\n", md->at_port);
				md->at_port = NULL;
			}
		}
		break;
	case MD_EX:
	case MD_EX_REC_OK:
	case MD_EX_PASS:
		dev_err(md->dev, "MD exception message %u\n", hdr->data[1]);
		break;
	default:
		dev_info(md->dev, "control message %#x %#x\n", hdr->data[0], hdr->data[1]);
		break;
	}
}

static bool mt6739_ccci_port_rx(struct mt6739_ccci *md, const u8 *pkt, u32 n)
{
	const struct ccci_header *hdr = (const void *)pkt;
	struct mt6739_ccci_port *port;
	struct sk_buff *skb;
	int i;

	for (i = 0; i < ARRAY_SIZE(mt6739_ccci_ports); i++) {
		port = &mt6739_ccci_ports[i];
		if (port->rx_ch != hdr->channel)
			continue;
		if (skb_queue_len(&port->rx) >= PORT_RX_MAX) {
			dev_warn_ratelimited(md->dev, "%s: queue full, drop\n", port->name);
			return true;
		}
		skb = alloc_skb(n, GFP_KERNEL);
		if (skb) {
			skb_put_data(skb, pkt, n);
			skb_queue_tail(&port->rx, skb);
			wake_up_interruptible(&port->wait);
		}
		return true;
	}
	return false;
}

static struct mt6739_ccci_port *mt6739_ccci_file_port(struct file *file)
{
	return container_of(file->private_data, struct mt6739_ccci_port, misc);
}

static ssize_t mt6739_ccci_port_read(struct file *file, char __user *buf, size_t len,
				     loff_t *ppos)
{
	struct mt6739_ccci_port *port = mt6739_ccci_file_port(file);
	struct sk_buff *skb;
	ssize_t ret;

	for (;;) {
		skb = skb_dequeue(&port->rx);
		if (skb)
			break;
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(port->wait, !skb_queue_empty(&port->rx));
		if (ret)
			return ret;
	}
	if (len < skb->len) {
		skb_queue_head(&port->rx, skb);
		return -EMSGSIZE;
	}
	ret = copy_to_user(buf, skb->data, skb->len) ? -EFAULT : skb->len;
	kfree_skb(skb);
	return ret;
}

static ssize_t mt6739_ccci_port_write(struct file *file, const char __user *buf, size_t len,
				      loff_t *ppos)
{
	struct mt6739_ccci_port *port = mt6739_ccci_file_port(file);
	struct ccci_header *hdr;
	void *pkt;
	int ret;

	if (len < sizeof(*hdr) || len > RX_PKT_MAX)
		return -EINVAL;
	if (!port->md->started)
		return -ENODEV;
	pkt = memdup_user(buf, len);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);
	hdr = pkt;
	hdr->channel = port->tx_ch;
	hdr->data[1] = len;
	ret = mt6739_ccci_ring_tx(port->md, port->queue, pkt, len);
	kfree(pkt);
	return ret ?: len;
}

static __poll_t mt6739_ccci_port_poll(struct file *file, poll_table *wait)
{
	struct mt6739_ccci_port *port = mt6739_ccci_file_port(file);

	poll_wait(file, &port->wait, wait);
	return (skb_queue_empty(&port->rx) ? 0 : EPOLLIN | EPOLLRDNORM) | EPOLLOUT | EPOLLWRNORM;
}

static const struct file_operations mt6739_ccci_port_fops = {
	.owner = THIS_MODULE,
	.read = mt6739_ccci_port_read,
	.write = mt6739_ccci_port_write,
	.poll = mt6739_ccci_port_poll,
	.llseek = noop_llseek,
};

static int mt6739_ccci_ports_register(struct mt6739_ccci *md)
{
	struct mt6739_ccci_port *port;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(mt6739_ccci_ports); i++) {
		port = &mt6739_ccci_ports[i];
		port->md = md;
		skb_queue_head_init(&port->rx);
		init_waitqueue_head(&port->wait);
		port->misc.minor = MISC_DYNAMIC_MINOR;
		port->misc.name = port->name;
		port->misc.fops = &mt6739_ccci_port_fops;
		port->misc.mode = 0660;
		ret = misc_register(&port->misc);
		if (ret) {
			while (--i >= 0)
				misc_deregister(&mt6739_ccci_ports[i].misc);
			return ret;
		}
	}
	return 0;
}

static void mt6739_ccci_ports_unregister(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(mt6739_ccci_ports); i++) {
		misc_deregister(&mt6739_ccci_ports[i].misc);
		skb_queue_purge(&mt6739_ccci_ports[i].rx);
	}
}

static void mt6739_ccci_ring_rx(struct mt6739_ccci *md, int q)
{
	struct mt6739_ccci_ring *ring = md->in_exception ? &md->exp_rings[q] : &md->rings[q];
	u32 len = ring->rx->length;
	struct ccci_header *hdr = (void *)md->rx_pkt;
	u32 read, write, size, pkt[2], total;

	while (len) {
		read = READ_ONCE(ring->rx->read);
		write = READ_ONCE(ring->rx->write);
		size = (write + len - read) % len;
		if (size < 16 + sizeof(*hdr))
			break;
		dma_rmb();
		mt6739_ccci_ring_copy(pkt, ring->rx_buf, read, sizeof(pkt), len);
		total = ALIGN(16 + pkt[1], 8);
		if (pkt[0] != PKT_HEADER || pkt[1] < sizeof(*hdr) || pkt[1] > RX_PKT_MAX ||
		    total > size) {
			dev_err(md->dev, "queue %d: bad packet %#x len %u (read %u write %u)\n",
				q, pkt[0], pkt[1], read, write);
			break;
		}
		mt6739_ccci_ring_copy(md->rx_pkt, ring->rx_buf, (read + 8) % len, pkt[1], len);
		WRITE_ONCE(ring->rx->read, (read + total) % len);

		switch (hdr->channel) {
		case CCCI_CONTROL_RX:
			mt6739_ccci_ctrl(md, hdr);
			break;
		case CCCI_RPC_RX:
			mt6739_ccci_rpc(md, q, md->rx_pkt, pkt[1]);
			break;
		case CCCI_AT_RX:
			mt6739_ccci_at_rx(md, md->rx_pkt + sizeof(*hdr), pkt[1] - sizeof(*hdr));
			break;
		default:
			if (mt6739_ccci_port_rx(md, md->rx_pkt, pkt[1]))
				break;
			dev_info(md->dev, "queue %d: channel %u data %#x %#x reserved %#x len %u\n",
				 q, hdr->channel, hdr->data[0], hdr->data[1], hdr->reserved, pkt[1]);
			print_hex_dump(KERN_INFO, "md pkt: ", DUMP_PREFIX_OFFSET, 16, 1,
				       md->rx_pkt + sizeof(*hdr),
				       min_t(u32, pkt[1] - sizeof(*hdr), md->in_exception ? 512 : 128),
				       true);
			break;
		}
	}
}

static void mt6739_ccci_add_feature(void **p, u8 id, u8 support, const void *data, u32 len)
{
	struct ccci_runtime_feature f = { .id = id, .support = support, .data_len = len };

	memcpy(*p, &f, sizeof(f));
	*p += sizeof(f);
	memcpy(*p, data, len);
	*p += len;
}

static int mt6739_ccci_runtime_data(struct mt6739_ccci *md, const u8 *md_feature)
{
	void *p = md->smem + SMEM_RUNTIME;
	u32 d[4];
	int i;

	for (i = 0; i < MD_FEATURE_COUNT; i++) {
		u8 md_mask = md_feature[i] & 0xf, md_ver = md_feature[i] >> 4;
		u8 ap_mask = ap_feature_mask[i];
		u8 support = 0;
		u32 len = 0;

		if (md_mask == FEATURE_MUST && ap_mask < FEATURE_MUST) {
			dev_err(md->dev, "MD needs feature %d, AP does not have it\n", i);
			return -EINVAL;
		}
		if (md_mask == FEATURE_NOT_EXIST || md_mask == FEATURE_MUST)
			support = md_feature[i];
		else if (md_mask == FEATURE_OPTIONAL)
			support = md_ver == 0 && ap_mask >= FEATURE_MUST ? FEATURE_MUST :
									   FEATURE_NOT_SUPPORT;
		else if (md_mask == FEATURE_BACKWARD)
			support = ap_mask >= FEATURE_MUST ? FEATURE_MUST : FEATURE_NOT_SUPPORT;

		memset(d, 0, sizeof(d));
		if ((support & 0xf) == FEATURE_MUST) {
			len = 2 * sizeof(u32);
			switch (i) {
			case F_BOOT_INFO:
				d[0] = CCCI_CONTROL_RX;
				len = sizeof(d);
				break;
			case F_EXCEPTION_SHM:
				d[0] = mt6739_ccci_smem_md(md, 0);
				d[1] = SMEM_EXCEPTION_SIZE;
				break;
			case F_CCIF_SHM:
				d[0] = mt6739_ccci_smem_md(md, SMEM_CCISM);
				d[1] = md->ccism_size;
				break;
			case F_CCB_SHM:
				d[0] = mt6739_ccci_smem_md(md, SMEM_CCB_CTRL);
				d[1] = SZ_4K;
				d[2] = mt6739_ccci_ccb_md(md);
				d[3] = CCB_DATA_SIZE;
				len = sizeof(d);
				break;
			case F_DHL_RAW_SHM:
				d[0] = mt6739_ccci_ccb_md(md) + CCB_DATA_SIZE;
				d[1] = md->ccb_size - CCB_DATA_SIZE;
				break;
			case F_NETD_SHM:
				d[0] = mt6739_ccci_smem_md(md, SMEM_NETD);
				d[1] = SZ_4K;
				break;
			case F_USB_SHM:
				d[0] = mt6739_ccci_smem_md(md, SMEM_USB);
				d[1] = SZ_4K;
				break;
			case F_AUDIO_RAW_SHM:
				d[0] = mt6739_ccci_smem_md(md, SMEM_AUDIO);
				d[1] = SMEM_AUDIO_SIZE;
				break;
			case F_CCISM_EXP_SHM:
				d[0] = mt6739_ccci_smem_md(md, SMEM_CCISM_EXP);
				d[1] = SMEM_CCISM_EXP_SIZE;
				break;
			case F_LWA_SHM:
			case F_MD_CONSYS_SHM:
				d[0] = mt6739_ccci_smem_md(md, SMEM_SIB);
				break;
			case F_MULTI_MD_MPU:
				len = 0;
				break;
			case F_RANDOM_SEED:
				d[0] = get_random_u32();
				len = sizeof(d);
				break;
			case F_MISC_CCCI:
				d[0] = 0;
				len = sizeof(d);
				break;
			case F_CLIB_TIME:
				d[0] = lower_32_bits(ktime_get_real_seconds());
				d[1] = upper_32_bits(ktime_get_real_seconds());
				len = sizeof(d);
				break;
			case F_C2K:
				if (md->c2k_lte_mode == 1)
					d[0] = BIT(1);
				else if (md->c2k_lte_mode == 2)
					d[0] = BIT(2);
				len = sizeof(d);
				break;
			case F_CCMNI_MTU:
				d[0] = CCMNI_MTU;
				len = sizeof(u32);
				break;
			default:
				len = sizeof(d);
				break;
			}
		}
		mt6739_ccci_add_feature(&p, i, support, d, len);
	}
	if (p - (md->smem + SMEM_RUNTIME) > SMEM_RUNTIME_SIZE)
		dev_warn(md->dev, "runtime data overflows its region\n");
	return 0;
}

static void mt6739_ccci_send_runtime_data(struct mt6739_ccci *md)
{
	struct ap_query_md_feature ap = {
		.head = AP_FEATURE_PATTERN,
		.feature[1] = 0x10,
		.share_memory_support = 2,
		.ap_runtime_addr = mt6739_ccci_smem_md(md, SMEM_RUNTIME),
		.ap_runtime_size = SMEM_RUNTIME_SIZE,
		.md_runtime_addr = mt6739_ccci_smem_md(md, SMEM_RUNTIME + SMEM_RUNTIME_SIZE),
		.md_runtime_size = SMEM_RUNTIME_SIZE,
		.noncached_mpu_addr = mt6739_ccci_smem_md(md, 0),
		.noncached_mpu_size = md->smem_layout.ap_md1_size,
		.cached_mpu_addr = mt6739_ccci_ccb_md(md),
		.cached_mpu_size = md->ccb_size,
		.tail = AP_FEATURE_PATTERN,
	};
	struct ccci_header hdr = {
		.data[1] = sizeof(hdr) + sizeof(ap),
		.channel = CCCI_CONTROL_TX,
		.reserved = MD_INIT_CHK_ID,
	};
	u32 *dbm = md->smem + SMEM_DBM;

	__iowrite32_copy(md->ap_ccif + CCIF_SRAM_UP_HEADER, &hdr, sizeof(hdr) / 4);
	__iowrite32_copy(md->ap_ccif + CCIF_SRAM_AP_FEATURE, &ap, sizeof(ap) / 4);

	memset(dbm, 0, SMEM_DBM_SIZE + 16);
	dbm[0] = SMEM_DBM_GUARD;
	dbm[1] = SMEM_DBM_GUARD;
	dbm[SMEM_DBM_SIZE / 4 + 2] = SMEM_DBM_GUARD;
	dbm[SMEM_DBM_SIZE / 4 + 3] = SMEM_DBM_GUARD;

	wmb();
	writel(BIT(CCIF_CH_SRAM), md->ap_ccif + CCIF_BUSY);
	writel(CCIF_CH_SRAM, md->ap_ccif + CCIF_TCHNUM);
}

static void mt6739_ccci_hs1(struct mt6739_ccci *md)
{
	u8 feature[MD_FEATURE_COUNT] __aligned(4);
	struct ccci_header hdr;
	u32 head, tail;

	__ioread32_copy(&hdr, md->ap_ccif + CCIF_SRAM_DL_HEADER, sizeof(hdr) / 4);
	head = readl(md->ap_ccif + CCIF_SRAM_MD_FEATURE);
	__ioread32_copy(feature, md->ap_ccif + CCIF_SRAM_MD_FEATURE + 4, sizeof(feature) / 4);
	tail = readl(md->ap_ccif + CCIF_SRAM_MD_FEATURE + 4 + MD_FEATURE_COUNT);
	dev_info(md->dev, "HS1: data %#x %#x channel %u reserved %#x, boot stats %#x %#x\n",
		 hdr.data[0], hdr.data[1], hdr.channel, hdr.reserved,
		 readl(md->boot_stats), readl(md->boot_stats + 4));

	if (hdr.reserved != MD_INIT_CHK_ID || head != MD_FEATURE_PATTERN ||
	    tail != MD_FEATURE_PATTERN) {
		dev_err(md->dev, "bad HS1 pattern %#x/%#x\n", head, tail);
		return;
	}
	if (mt6739_ccci_runtime_data(md, feature))
		return;
	mt6739_ccci_send_runtime_data(md);
	dev_info(md->dev, "runtime data sent\n");
}

static irqreturn_t mt6739_ccci_irq(int irq, void *data)
{
	struct mt6739_ccci *md = data;
	u32 ch = readl(md->ap_ccif + CCIF_RCHNUM);
	int q;

	writel(ch, md->ap_ccif + CCIF_ACK);
	dev_dbg(md->dev, "CCIF irq %d channels %#x\n", irq, ch);

	if (ch & BIT(CCIF_CH_SRAM))
		mt6739_ccci_hs1(md);
	for (q = 0; q < CCIF_RINGQ_NUM; q++)
		if (ch & BIT(q))
			mt6739_ccci_ring_rx(md, q);
	if (ch & BIT(CCIF_CH_EXCEPTION)) {
		dev_err(md->dev, "MD exception, boot stats %#x %#x, seq error %#x\n",
			readl(md->boot_stats), readl(md->boot_stats + 4),
			*(u32 *)(md->smem + 0x34));
		writel(BIT(CCIF_CH_EXCEPTION), md->ap_ccif + CCIF_BUSY);
		writel(CCIF_CH_EXCEPTION, md->ap_ccif + CCIF_TCHNUM);
	}
	if (ch & BIT(CCIF_CH_EX_CLEARQ_DONE)) {
		md->in_exception = true;
		writel(BIT(CCIF_CH_EX_CLEARQ), md->ap_ccif + CCIF_BUSY);
		writel(CCIF_CH_EX_CLEARQ, md->ap_ccif + CCIF_TCHNUM);
	}
	if (ch & GENMASK(23, CCIF_CH_EXCEPTION + 1))
		dev_warn(md->dev, "CCIF exception channels %#x\n", ch);
	return IRQ_HANDLED;
}

static int mt6739_ccci_mtcmos_on(struct mt6739_ccci *md)
{
	struct regmap *spm = md->scpsys;
	u32 val, val2;
	int ret;

	regmap_clear_bits(spm, SPM_MD1_PWR_CON, PWR_RST_B);
	regmap_set_bits(spm, SPM_MD1_PWR_CON, PWR_ON);
	regmap_set_bits(spm, SPM_MD1_PWR_CON, PWR_ON_2ND);
	ret = regmap_read_poll_timeout(spm, SPM_PWR_STATUS, val, val & PWR_STATUS_MD1, 10, 10000);
	if (!ret)
		ret = regmap_read_poll_timeout(spm, SPM_PWR_STATUS_2ND, val2,
					       val2 & PWR_STATUS_MD1, 10, 10000);
	if (ret)
		return ret;

	regmap_clear_bits(spm, SPM_MD1_PWR_CON, MD_SRAM_PDN);
	regmap_set_bits(spm, SPM_MD_SRAM_ISO_CON, BIT(0));
	regmap_clear_bits(spm, SPM_MD1_PWR_CON, PWR_ISO);
	regmap_clear_bits(spm, SPM_MD1_PWR_CON, MD_PWR_CLK_DIS);
	regmap_set_bits(spm, SPM_MD1_PWR_CON, PWR_RST_B);
	regmap_clear_bits(spm, SPM_MD_EXTRA_PWR_CON, BIT(0));

	regmap_write(md->infracfg, INFRA_PROT_1_CLR, INFRA_PROT_1_MD2PERI);
	regmap_write(md->infracfg, INFRA_PROT_CLR, INFRA_PROT_MD1);
	udelay(250);
	return 0;
}

static int mt6739_ccci_pll_init(struct mt6739_ccci *md)
{
	void __iomem *pll = md->md_pll, *clksw = md->md_clksw;
	u32 val;
	int ret, i;

	ret = readl_poll_timeout(pll, val, val, 20000, 200000);
	if (ret)
		return dev_err_probe(md->dev, ret, "MD PLL does not answer\n");

	writel(readl(md->apmixed) | AP_PLL_CON0_LPF, md->apmixed);
	udelay(100);

	writel(0x80118000, pll + 0x60);
	writel(0x800b8a00, pll + 0x58);
	writel(0x80114e00, pll + 0x50);
	writel(0x80114e00, pll + 0x48);
	writel(0x800b8a00, pll + 0x40);
	ret = readl_poll_timeout(pll + 0xc00, val, !(val & BIT(14)), 10, 10000);
	if (ret)
		return dev_err_probe(md->dev, ret, "MD PLL busy\n");

	writel(readl(pll + 0x108) & ~0x10000, pll + 0x108);
	writel(0x10000, pll + 0x14);
	writel(0x10001, pll + 0x14);
	udelay(100);
	writel(0x10000, pll + 0x14);
	writel(0x0, pll + 0x14);
	writel(readl(pll + 0x108) | 0x10000, pll + 0x108);
	udelay(1);
	writel(readl(pll + 0x104) & ~0x4000000, pll + 0x104);
	udelay(1);

	writel(0x100010, pll + 0x10);
	for (i = 0; readl(pll + 0x10) != 0x100010; i++) {
		if (i == 50)
			return dev_err_probe(md->dev, -ETIMEDOUT, "MD PLL switch\n");
		msleep(20);
		if (i % 5 == 4)
			writel(0x100010, pll + 0x10);
	}

	ret = readl_poll_timeout(clksw + 0x84, val, val & BIT(15), 20000, 1000000);
	if (ret)
		return dev_err_probe(md->dev, ret, "MD clock switch\n");
	writel(readl(clksw + 0x24) | 0x3, clksw + 0x24);
	writel(readl(clksw + 0x24) | 0x058103fc, clksw + 0x24);
	writel(readl(clksw + 0x28) | 0x10, clksw + 0x28);
	writel(0x1, clksw + 0x20);

	writel(0xffff, pll + 0x314);
	writel(0xffff, pll + 0x318);
	writel(MD_PLL_DONE, pll + 0xf00);
	return 0;
}

static void mt6739_ccci_clear_smem(struct mt6739_ccci *md)
{
	memset(md->smem, 0, md->smem_layout.ap_md1_size);
}

static int mt6739_ccci_start(struct mt6739_ccci *md)
{
	struct regmap *infra = md->infracfg;
	u32 val, pwr, sta;
	int ret;

	regmap_read(md->scpsys, SPM_MD1_PWR_CON, &pwr);
	regmap_read(md->scpsys, SPM_PWR_STATUS, &sta);
	dev_info(md->dev, "before start: MD1_PWR_CON %#x PWR_STATUS %#x\n", pwr, sta);

	ret = clk_bulk_prepare_enable(md->num_clks, md->clks);
	if (ret)
		return ret;

	mt6739_ccci_clear_smem(md);
	md->ccism_size = mt6739_ccci_create_rings(md, SMEM_CCISM, ring_rx_size, ring_tx_size,
						  md->rings);
	mt6739_ccci_create_rings(md, SMEM_CCISM_EXP, ring_exp_size, ring_exp_size, md->exp_rings);
	wmb();

	regmap_write(infra, INFRA_RST1_SET, INFRA_RST1_CCIF);
	regmap_write(infra, INFRA_RST1_CLR, INFRA_RST1_CCIF);
	mt6739_ccci_sram_clear(md->ap_ccif + CCIF_SRAM, CCIF_SRAM_SIZE);
	mt6739_ccci_sram_clear(md->md_ccif + CCIF_SRAM, CCIF_SRAM_SIZE);
	writel(~0, md->ap_ccif + CCIF_ACK);

	regmap_write(infra, INFRA_RST0_SET, INFRA_RST0_CLDMA_AO);
	regmap_write(infra, INFRA_RST0_CLR, INFRA_RST0_CLDMA_AO);
	regmap_write(infra, INFRA_RST1_SET, INFRA_RST1_CLDMA_PD);
	regmap_write(infra, INFRA_RST1_CLR, INFRA_RST1_CLDMA_PD);
	regmap_set_bits(infra, INFRA_CLDMA_WAKEUP, BIT(1));

	writel(readl(md->topckgen) & ~TOPCKGEN_MD_CLK_MODE, md->topckgen);

	ret = regulator_enable(md->vmodem);
	if (ret)
		goto err_clk;

	ret = mt6739_ccci_mtcmos_on(md);
	if (ret) {
		dev_err(md->dev, "MD1 power domain does not turn on\n");
		goto err_reg;
	}

	regmap_clear_bits(infra, INFRA_AP2MD_DUMMY, BIT(0));
	regmap_write(infra, INFRA_PROT_1_SET, INFRA_PROT_1_MD2PERI);
	ret = regmap_read_poll_timeout(infra, INFRA_PROT_EN_1_STA1, val,
				       val & INFRA_PROT_1_MD2PERI, 10, 10000);
	if (ret)
		dev_warn(md->dev, "MD to peri protect does not set\n");

	writel(readl(md->dvfsrc + DVFSRC_MD_SRCLKENA) | DVFSRC_MD_SRCLKENA_EN,
	       md->dvfsrc + DVFSRC_MD_SRCLKENA);
	regmap_update_bits(infra, INFRA_SRCLKEN, 0xff, 0x21);

	ret = mt6739_ccci_pll_init(md);
	if (ret)
		goto err_reg;

	writel(CLDMA_MTU, md->cldma_ao + CLDMA_DL_MTU);
	writel(readl(md->cldma_ao + CLDMA_SO_CFG) | CLDMA_SO_CFG_EN, md->cldma_ao + CLDMA_SO_CFG);

	writel(MD_RGU_WDT_OFF, md->md_rgu + MD_RGU_WDT);
	writel(1, md->md_cfg + MD_CFG_BOOT_SLAVE);

	regmap_write(infra, INFRA_PROT_SET, INFRA_PROT_AP2MD);
	if (regmap_read_poll_timeout(infra, INFRA_PROT_STA1, val, val & INFRA_PROT_AP2MD, 10, 10000))
		dev_warn(md->dev, "AP to MD protect does not set\n");
	regmap_write(infra, INFRA_PROT_1_CLR, INFRA_PROT_1_MD2PERI);
	if (regmap_read_poll_timeout(infra, INFRA_PROT_EN_1_STA1, val,
				     !(val & INFRA_PROT_1_MD2PERI), 10, 10000))
		dev_warn(md->dev, "MD to peri protect does not clear\n");
	regmap_set_bits(infra, INFRA_AP2MD_DUMMY, BIT(0));

	dev_info(md->dev, "MD1 released, boot stats %#x %#x\n",
		 readl(md->boot_stats), readl(md->boot_stats + 4));
	return 0;

err_reg:
	regulator_disable(md->vmodem);
err_clk:
	clk_bulk_disable_unprepare(md->num_clks, md->clks);
	return ret;
}

static ssize_t mt6739_ccci_start_write(struct file *file, const char __user *buf,
				       size_t len, loff_t *ppos)
{
	struct mt6739_ccci *md = file->private_data;
	int ret = 0;

	mutex_lock(&md->lock);
	if (!md->started) {
		ret = mt6739_ccci_start(md);
		md->started = !ret;
	}
	mutex_unlock(&md->lock);
	return ret ?: len;
}

static const struct file_operations mt6739_ccci_start_fops = {
	.open = simple_open,
	.write = mt6739_ccci_start_write,
};

static int mt6739_ccci_status_show(struct seq_file *s, void *unused)
{
	struct mt6739_ccci *md = s->private;
	u32 pwr, sta;

	regmap_read(md->scpsys, SPM_MD1_PWR_CON, &pwr);
	regmap_read(md->scpsys, SPM_PWR_STATUS, &sta);
	seq_printf(s, "MD1_PWR_CON %#x PWR_STATUS %#x\n", pwr, sta);
	seq_printf(s, "boot stats %#x %#x\n", readl(md->boot_stats), readl(md->boot_stats + 4));
	if (md->started)
		seq_printf(s, "CCIF RCHNUM %#x BUSY %#x\n",
			   readl(md->ap_ccif + CCIF_RCHNUM), readl(md->ap_ccif + CCIF_BUSY));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mt6739_ccci_status);

static void __iomem *mt6739_ccci_map_shared(struct platform_device *pdev, const char *name)
{
	struct resource *res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);

	if (!res)
		return IOMEM_ERR_PTR(-EINVAL);
	return devm_ioremap(&pdev->dev, res->start, resource_size(res)) ?: IOMEM_ERR_PTR(-ENOMEM);
}

static int mt6739_ccci_probe(struct platform_device *pdev)
{
	static const char * const own[] = { "cldma-ao", "ap-ccif", "md-ccif", "boot-stats", "md-cfg",
					     "md-rgu", "md-pll", "md-clksw" };
	static const char * const shared[] = { "topckgen", "apmixed", "dvfsrc" };
	struct device *dev = &pdev->dev;
	void __iomem **own_base[ARRAY_SIZE(own)];
	void __iomem **shared_base[ARRAY_SIZE(shared)];
	struct mt6739_ccci *md;
	struct dentry *dir;
	int i, irq, ret;

	md = devm_kzalloc(dev, sizeof(*md), GFP_KERNEL);
	if (!md)
		return -ENOMEM;
	md->dev = dev;
	mutex_init(&md->lock);
	spin_lock_init(&md->tx_lock);
	md->rx_pkt = devm_kmalloc(dev, RX_PKT_MAX, GFP_KERNEL);
	md->tx_pkt = devm_kmalloc(dev, RPC_MAX_BUF + 32, GFP_KERNEL);
	if (!md->rx_pkt || !md->tx_pkt)
		return -ENOMEM;

	ret = mt6739_ccci_parse_lk_info(md);
	if (ret)
		return ret;

	own_base[0] = &md->cldma_ao;
	own_base[1] = &md->ap_ccif;
	own_base[2] = &md->md_ccif;
	own_base[3] = &md->boot_stats;
	own_base[4] = &md->md_cfg;
	own_base[5] = &md->md_rgu;
	own_base[6] = &md->md_pll;
	own_base[7] = &md->md_clksw;
	for (i = 0; i < ARRAY_SIZE(own); i++) {
		*own_base[i] = devm_platform_ioremap_resource_byname(pdev, own[i]);
		if (IS_ERR(*own_base[i]))
			return PTR_ERR(*own_base[i]);
	}

	/* The clock drivers own these blocks; the modem needs one register in each. */
	shared_base[0] = &md->topckgen;
	shared_base[1] = &md->apmixed;
	shared_base[2] = &md->dvfsrc;
	for (i = 0; i < ARRAY_SIZE(shared); i++) {
		*shared_base[i] = mt6739_ccci_map_shared(pdev, shared[i]);
		if (IS_ERR(*shared_base[i]))
			return PTR_ERR(*shared_base[i]);
	}

	md->infracfg = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,infracfg");
	if (IS_ERR(md->infracfg))
		return dev_err_probe(dev, PTR_ERR(md->infracfg), "no infracfg\n");
	md->scpsys = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,scpsys");
	if (IS_ERR(md->scpsys))
		return dev_err_probe(dev, PTR_ERR(md->scpsys), "no scpsys\n");

	md->num_clks = devm_clk_bulk_get_all(dev, &md->clks);
	if (md->num_clks < 0)
		return md->num_clks;
	md->vmodem = devm_regulator_get(dev, "vmodem");
	if (IS_ERR(md->vmodem))
		return PTR_ERR(md->vmodem);

	if (md->smem_layout.ap_md1_size < 0x28000)
		return dev_err_probe(dev, -EINVAL, "AP-MD1 shared memory too small\n");
	md->smem = devm_memremap(dev, md->smem_layout.base + md->smem_layout.ap_md1_offset,
				 md->smem_layout.ap_md1_size, MEMREMAP_WC);
	if (IS_ERR(md->smem))
		return PTR_ERR(md->smem);

	for (i = 0; i < 2; i++) {
		irq = platform_get_irq(pdev, i);
		if (irq < 0)
			return irq;
		ret = devm_request_threaded_irq(dev, irq, NULL, mt6739_ccci_irq, IRQF_ONESHOT,
						dev_name(dev), md);
		if (ret)
			return ret;
	}

	ret = mt6739_ccci_ports_register(md);
	if (ret)
		return ret;

	dir = debugfs_create_dir("mt6739-ccci", NULL);
	debugfs_create_file("start", 0200, dir, md, &mt6739_ccci_start_fops);
	debugfs_create_file("status", 0400, dir, md, &mt6739_ccci_status_fops);
	md->smem_blob.data = md->smem;
	md->smem_blob.size = md->smem_layout.ap_md1_size;
	debugfs_create_blob("smem", 0400, dir, &md->smem_blob);
	md->ccb_blob.data = devm_memremap(dev, md->ccb_addr, md->ccb_size, MEMREMAP_WC);
	if (!IS_ERR(md->ccb_blob.data)) {
		md->ccb_blob.size = md->ccb_size;
		debugfs_create_blob("ccb", 0400, dir, &md->ccb_blob);
	}
	md->debugfs = dir;
	platform_set_drvdata(pdev, md);
	return 0;
}

static void mt6739_ccci_remove(struct platform_device *pdev)
{
	struct mt6739_ccci *md = dev_get_drvdata(&pdev->dev);

	debugfs_remove_recursive(md->debugfs);
	if (md->at_port)
		wwan_remove_port(md->at_port);
	mt6739_ccci_ports_unregister();
}

static const struct of_device_id mt6739_ccci_of_match[] = {
	{ .compatible = "mediatek,mt6739-ccci" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6739_ccci_of_match);

static struct platform_driver mt6739_ccci_driver = {
	.probe = mt6739_ccci_probe,
	.remove = mt6739_ccci_remove,
	.driver = {
		.name = "mt6739-ccci",
		.of_match_table = mt6739_ccci_of_match,
	},
};
module_platform_driver(mt6739_ccci_driver);

MODULE_DESCRIPTION("MediaTek MT6739 MD1 modem CCCI");
MODULE_LICENSE("GPL");
