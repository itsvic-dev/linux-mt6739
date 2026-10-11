// SPDX-License-Identifier: GPL-2.0-only
/* Halo HL7005 switching charger (bq24157 register layout). */

#include <linux/bitfield.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#define HL7005_CON0			0x00
#define HL7005_CON0_TMR_RST		BIT(7)
#define HL7005_CON0_EN_STAT		BIT(6)
#define HL7005_CON0_STAT		GENMASK(5, 4)
#define HL7005_CON0_FAULT		GENMASK(2, 0)
#define HL7005_CON1			0x01
#define HL7005_CON1_LIN_LIMIT		GENMASK(7, 6)
#define HL7005_CON1_LOW_V		GENMASK(5, 4)
#define HL7005_CON1_TE			BIT(3)
#define HL7005_CON1_CE_N		BIT(2)
#define HL7005_CON1_HZ_MODE		BIT(1)
#define HL7005_CON1_OPA_MODE		BIT(0)
#define HL7005_CON2			0x02
#define HL7005_CON2_OREG		GENMASK(7, 2)
#define HL7005_CON4			0x04
#define HL7005_CON4_I_CHR		GENMASK(6, 4)
#define HL7005_CON4_I_TERM		GENMASK(2, 0)
#define HL7005_CON5			0x05
#define HL7005_CON6			0x06
#define HL7005_CON6_ISAFE		GENMASK(6, 4)
#define HL7005_CON6_VSAFE		GENMASK(3, 0)

#define HL7005_STAT_READY		0
#define HL7005_STAT_CHARGING		1
#define HL7005_STAT_DONE		2
#define HL7005_STAT_FAULT		3

#define HL7005_LIN_LIMIT_500MA		1
#define HL7005_LOW_V_3V7		3
#define HL7005_OREG_MIN_UV		3500000
#define HL7005_OREG_STEP_UV		20000
#define HL7005_OREG_MAX			47
#define HL7005_I_CHR_MIN_UA		550000
#define HL7005_I_CHR_STEP_UA		100000
#define HL7005_I_TERM_150MA		2
/* VSP 4.44 V and normal current level, as the vendor driver sets them. */
#define HL7005_CON5_INIT		0x03
#define HL7005_ISAFE_1250MA		7
#define HL7005_VSAFE_4V20		0

#define HL7005_KICK_INTERVAL		(10 * HZ)
#define HL7005_TEMP_MIN_DECIC		0
#define HL7005_TEMP_MAX_DECIC		450

struct hl7005 {
	struct device *dev;
	struct regmap *regmap;
	struct power_supply *psy;
	struct delayed_work work;
	u32 cv_uv;
	u32 cc_ua;
	bool temp_stop;
};

static const enum power_supply_property hl7005_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
};

static int hl7005_oreg(u32 uv)
{
	if (uv < HL7005_OREG_MIN_UV)
		return 0;
	return min_t(int, (uv - HL7005_OREG_MIN_UV) / HL7005_OREG_STEP_UV, HL7005_OREG_MAX);
}

static int hl7005_i_chr(u32 ua)
{
	if (ua < HL7005_I_CHR_MIN_UA)
		return 0;
	return min_t(int, (ua - HL7005_I_CHR_MIN_UA) / HL7005_I_CHR_STEP_UA, 7);
}

static bool hl7005_temp_ok(struct hl7005 *chg)
{
	union power_supply_propval val;
	struct power_supply *bat;
	int ret;

	bat = power_supply_get_by_name("battery");
	if (!bat)
		return false;
	ret = power_supply_get_property(bat, POWER_SUPPLY_PROP_TEMP, &val);
	power_supply_put(bat);

	return !ret && val.intval >= HL7005_TEMP_MIN_DECIC &&
	       val.intval <= HL7005_TEMP_MAX_DECIC;
}

static void hl7005_work(struct work_struct *work)
{
	struct hl7005 *chg = container_of(to_delayed_work(work), struct hl7005, work);
	bool stop = !hl7005_temp_ok(chg);

	regmap_write(chg->regmap, HL7005_CON0, HL7005_CON0_TMR_RST | HL7005_CON0_EN_STAT);

	if (stop != chg->temp_stop) {
		dev_info(chg->dev, "charging %s\n", stop ? "stopped: battery temperature" : "allowed");
		chg->temp_stop = stop;
	}
	regmap_update_bits(chg->regmap, HL7005_CON1, HL7005_CON1_CE_N,
			   stop ? HL7005_CON1_CE_N : 0);

	power_supply_changed(chg->psy);
	schedule_delayed_work(&chg->work, HL7005_KICK_INTERVAL);
}

static int hl7005_get_property(struct power_supply *psy, enum power_supply_property psp,
			       union power_supply_propval *val)
{
	struct hl7005 *chg = power_supply_get_drvdata(psy);
	unsigned int con0, stat;
	int ret;

	ret = regmap_read(chg->regmap, HL7005_CON0, &con0);
	if (ret)
		return ret;
	stat = FIELD_GET(HL7005_CON0_STAT, con0);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		if (stat == HL7005_STAT_CHARGING)
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		else if (stat == HL7005_STAT_DONE)
			val->intval = POWER_SUPPLY_STATUS_FULL;
		else
			val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		if (chg->temp_stop)
			val->intval = POWER_SUPPLY_HEALTH_OVERHEAT;
		else if (stat == HL7005_STAT_FAULT)
			val->intval = POWER_SUPPLY_HEALTH_UNSPEC_FAILURE;
		else
			val->intval = POWER_SUPPLY_HEALTH_GOOD;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
		val->intval = HL7005_OREG_MIN_UV + hl7005_oreg(chg->cv_uv) * HL7005_OREG_STEP_UV;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		val->intval = HL7005_I_CHR_MIN_UA + hl7005_i_chr(chg->cc_ua) * HL7005_I_CHR_STEP_UA;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static const struct power_supply_desc hl7005_desc = {
	.name = "hl7005",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = hl7005_props,
	.num_properties = ARRAY_SIZE(hl7005_props),
	.get_property = hl7005_get_property,
};

static const struct regmap_config hl7005_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = HL7005_CON6,
};

static void hl7005_stop(void *data)
{
	struct hl7005 *chg = data;

	cancel_delayed_work_sync(&chg->work);
}

static int hl7005_probe(struct i2c_client *client)
{
	struct power_supply_battery_info *info;
	struct power_supply_config cfg = {};
	struct device *dev = &client->dev;
	struct hl7005 *chg;
	int ret;

	chg = devm_kzalloc(dev, sizeof(*chg), GFP_KERNEL);
	if (!chg)
		return -ENOMEM;
	chg->dev = dev;
	INIT_DELAYED_WORK(&chg->work, hl7005_work);

	chg->regmap = devm_regmap_init_i2c(client, &hl7005_regmap_config);
	if (IS_ERR(chg->regmap))
		return PTR_ERR(chg->regmap);

	/* The safety limits only take effect as the first write after reset. */
	ret = regmap_write(chg->regmap, HL7005_CON6,
			   FIELD_PREP(HL7005_CON6_ISAFE, HL7005_ISAFE_1250MA) |
			   FIELD_PREP(HL7005_CON6_VSAFE, HL7005_VSAFE_4V20));
	if (ret)
		return dev_err_probe(dev, ret, "no response\n");

	cfg.drv_data = chg;
	cfg.fwnode = dev_fwnode(dev);
	chg->psy = devm_power_supply_register(dev, &hl7005_desc, &cfg);
	if (IS_ERR(chg->psy))
		return dev_err_probe(dev, PTR_ERR(chg->psy), "Failed to register power supply\n");

	ret = power_supply_get_battery_info(chg->psy, &info);
	if (ret)
		return dev_err_probe(dev, ret, "no monitored-battery\n");
	chg->cv_uv = info->constant_charge_voltage_max_uv;
	chg->cc_ua = info->constant_charge_current_max_ua;
	if (chg->cv_uv <= 0 || chg->cc_ua <= 0)
		return dev_err_probe(dev, -EINVAL, "battery needs charge voltage and current\n");

	regmap_write(chg->regmap, HL7005_CON0, HL7005_CON0_TMR_RST | HL7005_CON0_EN_STAT);
	regmap_write(chg->regmap, HL7005_CON2,
		     FIELD_PREP(HL7005_CON2_OREG, hl7005_oreg(chg->cv_uv)));
	regmap_write(chg->regmap, HL7005_CON4,
		     FIELD_PREP(HL7005_CON4_I_CHR, hl7005_i_chr(chg->cc_ua)) |
		     FIELD_PREP(HL7005_CON4_I_TERM, HL7005_I_TERM_150MA));
	regmap_write(chg->regmap, HL7005_CON5, HL7005_CON5_INIT);
	regmap_write(chg->regmap, HL7005_CON1,
		     FIELD_PREP(HL7005_CON1_LIN_LIMIT, HL7005_LIN_LIMIT_500MA) |
		     FIELD_PREP(HL7005_CON1_LOW_V, HL7005_LOW_V_3V7) | HL7005_CON1_TE);

	ret = devm_add_action_or_reset(dev, hl7005_stop, chg);
	if (ret)
		return ret;

	schedule_delayed_work(&chg->work, 0);
	return 0;
}

static const struct of_device_id hl7005_of_match[] = {
	{ .compatible = "halo,hl7005" },
	{ }
};
MODULE_DEVICE_TABLE(of, hl7005_of_match);

static struct i2c_driver hl7005_driver = {
	.probe = hl7005_probe,
	.driver = {
		.name = "hl7005",
		.of_match_table = hl7005_of_match,
	},
};
module_i2c_driver(hl7005_driver);

MODULE_DESCRIPTION("Halo HL7005 charger driver");
MODULE_LICENSE("GPL");
