// SPDX-License-Identifier: GPL-2.0-only
/* MT6357 PMIC charger input detection. */

#include <linux/interrupt.h>
#include <linux/mfd/mt6357/registers.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regmap.h>

#define MT6357_RGS_CHRDET	BIT(4)

struct mt6357_charger {
	struct regmap *regmap;
	struct power_supply *psy;
};

static const enum power_supply_property mt6357_charger_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static int mt6357_charger_get_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       union power_supply_propval *val)
{
	struct mt6357_charger *chg = power_supply_get_drvdata(psy);
	unsigned int con;
	int ret;

	if (psp != POWER_SUPPLY_PROP_ONLINE)
		return -EINVAL;

	ret = regmap_read(chg->regmap, MT6357_CHR_TOP_CON0, &con);
	if (ret)
		return ret;

	val->intval = !!(con & MT6357_RGS_CHRDET);
	return 0;
}

static const struct power_supply_desc mt6357_charger_desc = {
	.name = "mt6357-charger",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = mt6357_charger_props,
	.num_properties = ARRAY_SIZE(mt6357_charger_props),
	.get_property = mt6357_charger_get_property,
};

static irqreturn_t mt6357_charger_irq(int irq, void *data)
{
	struct mt6357_charger *chg = data;

	power_supply_changed(chg->psy);
	return IRQ_HANDLED;
}

static int mt6357_charger_probe(struct platform_device *pdev)
{
	struct mt6397_chip *pmic = dev_get_drvdata(pdev->dev.parent);
	struct power_supply_config cfg = {};
	struct mt6357_charger *chg;
	int irq, ret;

	chg = devm_kzalloc(&pdev->dev, sizeof(*chg), GFP_KERNEL);
	if (!chg)
		return -ENOMEM;
	chg->regmap = pmic->regmap;

	cfg.drv_data = chg;
	cfg.fwnode = dev_fwnode(&pdev->dev);
	chg->psy = devm_power_supply_register(&pdev->dev, &mt6357_charger_desc, &cfg);
	if (IS_ERR(chg->psy))
		return dev_err_probe(&pdev->dev, PTR_ERR(chg->psy),
				     "Failed to register power supply\n");

	irq = platform_get_irq_byname(pdev, "chrdet");
	if (irq < 0)
		return irq;

	ret = devm_request_threaded_irq(&pdev->dev, irq, NULL, mt6357_charger_irq,
					IRQF_ONESHOT, "mt6357-chrdet", chg);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "Failed to request IRQ\n");

	return 0;
}

static const struct of_device_id mt6357_charger_of_match[] = {
	{ .compatible = "mediatek,mt6357-charger" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6357_charger_of_match);

static struct platform_driver mt6357_charger_driver = {
	.probe = mt6357_charger_probe,
	.driver = {
		.name = "mt6357-charger",
		.of_match_table = mt6357_charger_of_match,
	},
};
module_platform_driver(mt6357_charger_driver);

MODULE_DESCRIPTION("MediaTek MT6357 charger detection");
MODULE_LICENSE("GPL");
