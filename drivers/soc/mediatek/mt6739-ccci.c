// SPDX-License-Identifier: GPL-2.0-only
/* MT6739 MD1 modem power-on and CCCI handshake. */

#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/sizes.h>

#define CCIF_BUSY		0x04
#define CCIF_TCHNUM		0x0c
#define CCIF_RCHNUM		0x10
#define CCIF_ACK		0x14
#define CCIF_SRAM		0x100
#define CCIF_SRAM_SIZE		0x200
#define CCIF_SRAM_DL_HEADER	0x100
#define CCIF_SRAM_MD_FEATURE	0x110
#define CCIF_CH_SRAM		15

#define MD_FEATURE_PATTERN	0x49434343
#define MD_FEATURE_COUNT	64

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

struct mt6739_ccci {
	struct device *dev;
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
	void *smem;
	struct mutex lock;
	bool started;
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
		memcpy(&tag, area + off, sizeof(tag));
		tag.name[LK_TAG_NAME_LEN - 1] = '\0';
		dev_dbg(dev, "LK tag %s: offset %#x size %#x\n", tag.name, tag.data_offset, tag.data_size);
		if (!strcmp(tag.name, "smem_layout") && tag.data_size >= sizeof(md->smem_layout) &&
		    tag.data_offset + sizeof(md->smem_layout) <= info.size) {
			memcpy(&md->smem_layout, area + tag.data_offset, sizeof(md->smem_layout));
			have_layout = true;
		}
		off = tag.next_offset;
	}
	memunmap(area);

	if (!have_layout)
		return dev_err_probe(dev, -ENODEV, "no smem_layout tag\n");
	dev_info(dev, "shared memory %#llx size %#x, AP-MD1 %#x+%#x\n",
		 md->smem_layout.base, md->smem_layout.total_size,
		 md->smem_layout.ap_md1_offset, md->smem_layout.ap_md1_size);
	return 0;
}

static irqreturn_t mt6739_ccci_irq(int irq, void *data)
{
	struct mt6739_ccci *md = data;
	u32 ch = readl(md->ap_ccif + CCIF_RCHNUM);
	u8 feature[MD_FEATURE_COUNT];
	struct ccci_header hdr;
	u32 head, tail;

	writel(ch, md->ap_ccif + CCIF_ACK);
	dev_info(md->dev, "CCIF irq %d channels %#x\n", irq, ch);

	if (ch & BIT(CCIF_CH_SRAM)) {
		memcpy_fromio(&hdr, md->ap_ccif + CCIF_SRAM_DL_HEADER, sizeof(hdr));
		head = readl(md->ap_ccif + CCIF_SRAM_MD_FEATURE);
		memcpy_fromio(feature, md->ap_ccif + CCIF_SRAM_MD_FEATURE + 4, sizeof(feature));
		tail = readl(md->ap_ccif + CCIF_SRAM_MD_FEATURE + 4 + MD_FEATURE_COUNT);
		dev_info(md->dev, "HS1: data %#x %#x channel %u reserved %#x, pattern %#x/%#x\n",
			 hdr.data[0], hdr.data[1], hdr.channel, hdr.reserved, head, tail);
		print_hex_dump(KERN_INFO, "md feature: ", DUMP_PREFIX_OFFSET, 16, 1,
			       feature, sizeof(feature), false);
		dev_info(md->dev, "boot stats %#x %#x\n",
			 readl(md->boot_stats), readl(md->boot_stats + 4));
	}
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
	/* CCB control (0x18000) and the audio region (0x1b000..0x28000) keep their contents. */
	memset(md->smem, 0, 0x18000);
	memset(md->smem + 0x19000, 0, 0x2000);
	memset(md->smem + 0x28000, 0, md->smem_layout.ap_md1_size - 0x28000);
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

	regmap_write(infra, INFRA_RST1_SET, INFRA_RST1_CCIF);
	regmap_write(infra, INFRA_RST1_CLR, INFRA_RST1_CCIF);
	memset_io(md->ap_ccif + CCIF_SRAM, 0, CCIF_SRAM_SIZE);
	memset_io(md->md_ccif + CCIF_SRAM, 0, CCIF_SRAM_SIZE);
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
	static const char * const own[] = { "ap-ccif", "md-ccif", "boot-stats", "md-cfg",
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

	ret = mt6739_ccci_parse_lk_info(md);
	if (ret)
		return ret;

	own_base[0] = &md->ap_ccif;
	own_base[1] = &md->md_ccif;
	own_base[2] = &md->boot_stats;
	own_base[3] = &md->md_cfg;
	own_base[4] = &md->md_rgu;
	own_base[5] = &md->md_pll;
	own_base[6] = &md->md_clksw;
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

	dir = debugfs_create_dir("mt6739-ccci", NULL);
	debugfs_create_file("start", 0200, dir, md, &mt6739_ccci_start_fops);
	debugfs_create_file("status", 0400, dir, md, &mt6739_ccci_status_fops);
	platform_set_drvdata(pdev, dir);
	return 0;
}

static void mt6739_ccci_remove(struct platform_device *pdev)
{
	debugfs_remove_recursive(platform_get_drvdata(pdev));
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
