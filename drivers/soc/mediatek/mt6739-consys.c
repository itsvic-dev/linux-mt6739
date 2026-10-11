// SPDX-License-Identifier: GPL-2.0-only
/* MT6739 connectivity subsystem (CONSYS) power control. */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/sizes.h>

#define CONSYS_HW_ID		0x008
#define CONSYS_HW_ID_MT6739	0x699
#define CONSYS_ACR		0x110
#define CONSYS_ACR_MBIST	BIT(18)
#define CONSYS_CPUPCR		0x160

#define INFRA_CONN_EMI_REMAP		0x380
#define INFRA_CONN_EMI_REMAP_EN		BIT(13)
#define INFRA_CONN_EMI_REMAP_BASE	GENMASK(12, 0)

#define TOPRGU_SWSYSRST		0x018
#define TOPRGU_KEY		0x88000000
#define TOPRGU_CONN_RST		BIT(12)

struct mt6739_consys {
	struct device *dev;
	void __iomem *base;
	void __iomem *rgu;
	struct regulator *vcn18;
	struct regulator *vcn28;
	struct regulator *vcn33_bt;
	struct regmap *infracfg;
	phys_addr_t emi_base;
};

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

	mt6739_consys_cpu_reset(cs, true);

	ret = pm_runtime_resume_and_get(cs->dev);
	if (ret)
		goto err_vcn28;

	udelay(10);
	for (i = 0; i < 10; i++) {
		id = readl(cs->base + CONSYS_HW_ID);
		if (id == CONSYS_HW_ID_MT6739)
			break;
		msleep(20);
	}
	dev_info(cs->dev, "hardware ID 0x%x after %d polls\n", id, i);
	if (id != CONSYS_HW_ID_MT6739)
		return -ENODEV;

	writel(readl(cs->base + CONSYS_ACR) | CONSYS_ACR_MBIST, cs->base + CONSYS_ACR);
	mt6739_consys_cpu_reset(cs, false);
	msleep(20);

	for (i = 0; i < 5; i++) {
		dev_info(cs->dev, "CPU PC 0x%08x\n", readl(cs->base + CONSYS_CPUPCR));
		usleep_range(1000, 2000);
	}
	return 0;

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
	mt6739_consys_cpu_reset(cs, true);
	pm_runtime_put_sync(cs->dev);
	regulator_disable(cs->vcn28);
	regulator_disable(cs->vcn33_bt);
	regulator_disable(cs->vcn18);
}

static int mt6739_consys_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6739_consys *cs;
	struct resource *res, mem;
	int ret;

	cs = devm_kzalloc(dev, sizeof(*cs), GFP_KERNEL);
	if (!cs)
		return -ENOMEM;
	cs->dev = dev;

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

	ret = mt6739_consys_power_on(cs);
	if (ret != -ENODEV && ret)
		return dev_err_probe(dev, ret, "power on failed\n");
	mt6739_consys_power_off(cs);

	return 0;
}

static const struct of_device_id mt6739_consys_of_match[] = {
	{ .compatible = "mediatek,mt6739-consys" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6739_consys_of_match);

static struct platform_driver mt6739_consys_driver = {
	.probe = mt6739_consys_probe,
	.driver = {
		.name = "mt6739-consys",
		.of_match_table = mt6739_consys_of_match,
	},
};
module_platform_driver(mt6739_consys_driver);

MODULE_DESCRIPTION("MediaTek MT6739 CONSYS power control");
MODULE_LICENSE("GPL");
