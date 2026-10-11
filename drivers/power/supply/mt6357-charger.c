// SPDX-License-Identifier: GPL-2.0-only
/* MT6357 PMIC charger input detection and fuel gauge. */

#include <linux/iio/consumer.h>
#include <linux/interrupt.h>
#include <linux/math64.h>
#include <linux/mfd/mt6357/registers.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#define CHR_TOP_CON0_RGS_CHRDET		BIT(4)

#define FGADC_CON1_SW_CTRL		GENMASK(3, 0)
#define FGADC_CON1_SW_READ_PRE		1
#define FGADC_CON1_SW_CLEAR		8
#define FGADC_CON1_LATCHDATA_ST		BIT(15)

/* nA per FG_CURRENT LSB with a 10 mOhm sense resistor. */
#define FG_CURRENT_UNIT_NA		305176
/* Stock CAR_TUNE_VALUE. */
#define FG_CAR_TUNE_PERMILLE		930
/* Coulomb counter: 10.85 uAh per 2^11 LSB with a 10 mOhm sense resistor. */
#define FG_CAR_UNIT_NAH			10850
#define FG_CAR_SHIFT			11
/* Battery and sense resistance, to estimate OCV under load. */
#define BAT_R_MOHM			150

#define POLL_INTERVAL			(10 * HZ)
#define FULL_CURRENT_UA			50000

/* NTC 10 kOhm with 16.9 kOhm pull-up to 1.8 V. */
#define NTC_PULL_UP_OHM			16900
#define NTC_PULL_UP_MV			1800

static const struct {
	int decic;
	int ohm;
} ntc_table[] = {
	{ -400, 195652 }, { -350, 148171 }, { -300, 113347 }, { -250, 87559 },
	{ -200, 68237 }, { -150, 53650 }, { -100, 42506 }, { -50, 33892 },
	{ 0, 27219 }, { 50, 22021 }, { 100, 17926 }, { 150, 14674 },
	{ 200, 12081 }, { 250, 10000 }, { 300, 8315 }, { 350, 6948 },
	{ 400, 5834 }, { 450, 4917 }, { 500, 4161 }, { 550, 3535 },
	{ 600, 3014 },
};

struct mt6357_charger {
	struct device *dev;
	struct regmap *regmap;
	struct power_supply *psy;
	struct power_supply *bat;
	struct power_supply_battery_info *info;
	struct iio_channel *vbat;
	struct iio_channel *temp;
	struct delayed_work work;
	struct mutex lock;
	bool online;
	int vbat_uv;
	int current_ua;
	int temp_decic;
	bool gauge_ready;
	s64 car0_uah;
	int charge0_uah;
	int charge_uah;
};

static const enum power_supply_property mt6357_charger_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static const enum power_supply_property mt6357_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
};

static bool mt6357_charger_online(struct mt6357_charger *chg)
{
	unsigned int con;

	if (regmap_read(chg->regmap, MT6357_CHR_TOP_CON0, &con))
		return false;
	return con & CHR_TOP_CON0_RGS_CHRDET;
}

static int mt6357_read_temp(struct mt6357_charger *chg, int *decic)
{
	int mv, ohm, i, ret;

	ret = iio_read_channel_processed(chg->temp, &mv);
	if (ret < 0)
		return ret;
	if (mv <= 0 || mv >= NTC_PULL_UP_MV)
		return -ERANGE;

	ohm = NTC_PULL_UP_OHM * mv / (NTC_PULL_UP_MV - mv);
	if (ohm >= ntc_table[0].ohm || ohm <= ntc_table[ARRAY_SIZE(ntc_table) - 1].ohm)
		return -ERANGE;

	for (i = 1; ohm < ntc_table[i].ohm; i++)
		;
	*decic = ntc_table[i].decic - (ohm - ntc_table[i].ohm) *
		 (ntc_table[i].decic - ntc_table[i - 1].decic) /
		 (ntc_table[i - 1].ohm - ntc_table[i].ohm);
	return 0;
}

static int mt6357_fg_latch_read(struct mt6357_charger *chg, unsigned int reg,
				unsigned int count, unsigned int *val)
{
	unsigned int sta, i;
	int ret;

	ret = regmap_update_bits(chg->regmap, MT6357_FGADC_CON1, FGADC_CON1_SW_CTRL,
				 FGADC_CON1_SW_READ_PRE);
	if (ret)
		return ret;
	ret = regmap_read_poll_timeout(chg->regmap, MT6357_FGADC_CON1, sta,
				       sta & FGADC_CON1_LATCHDATA_ST, 100, 10000);
	for (i = 0; !ret && i < count; i++)
		ret = regmap_read(chg->regmap, reg + 2 * i, &val[i]);

	regmap_update_bits(chg->regmap, MT6357_FGADC_CON1, FGADC_CON1_SW_CTRL,
			   FGADC_CON1_SW_CLEAR);
	regmap_read_poll_timeout(chg->regmap, MT6357_FGADC_CON1, sta,
				 !(sta & FGADC_CON1_LATCHDATA_ST), 100, 10000);
	regmap_update_bits(chg->regmap, MT6357_FGADC_CON1, FGADC_CON1_SW_CTRL, 0);
	return ret;
}

static int mt6357_read_fg_current(struct mt6357_charger *chg, int *ua)
{
	unsigned int val;
	int ret;

	ret = mt6357_fg_latch_read(chg, MT6357_FGADC_CUR_CON0, 1, &val);
	if (ret)
		return ret;

	*ua = div_s64((s64)(s16)val * FG_CURRENT_UNIT_NA * FG_CAR_TUNE_PERMILLE,
		      1000 * 1000);
	return 0;
}

static int mt6357_read_fg_charge(struct mt6357_charger *chg, s64 *uah)
{
	unsigned int val[2];
	s32 car;
	int ret;

	ret = mt6357_fg_latch_read(chg, MT6357_FGADC_CAR_CON0, 2, val);
	if (ret)
		return ret;

	car = (s32)(val[1] << 16 | val[0]);
	*uah = div_s64((s64)car * FG_CAR_UNIT_NAH * FG_CAR_TUNE_PERMILLE,
		       (1000LL * 1000) << FG_CAR_SHIFT);
	return 0;
}

static int mt6357_battery_status(struct mt6357_charger *chg)
{
	if (!chg->online)
		return POWER_SUPPLY_STATUS_DISCHARGING;
	if (chg->current_ua > FULL_CURRENT_UA)
		return POWER_SUPPLY_STATUS_CHARGING;
	if (chg->current_ua > -FULL_CURRENT_UA &&
	    chg->vbat_uv >= chg->info->constant_charge_voltage_max_uv - 50000)
		return POWER_SUPPLY_STATUS_FULL;
	return POWER_SUPPLY_STATUS_NOT_CHARGING;
}

static void mt6357_gauge_update(struct mt6357_charger *chg)
{
	int full = chg->info->charge_full_design_uah;
	int ocv, cap;
	s64 car;

	if (mt6357_read_fg_charge(chg, &car))
		return;

	if (!chg->gauge_ready) {
		ocv = chg->vbat_uv - chg->current_ua / 1000 * BAT_R_MOHM;
		cap = power_supply_batinfo_ocv2cap(chg->info, ocv, 25);
		if (cap < 0)
			return;
		chg->charge0_uah = div_s64((s64)full * cap, 100);
		chg->car0_uah = car;
		chg->gauge_ready = true;
	}

	if (mt6357_battery_status(chg) == POWER_SUPPLY_STATUS_FULL) {
		chg->charge0_uah = full;
		chg->car0_uah = car;
	}

	chg->charge_uah = clamp_t(s64, chg->charge0_uah + car - chg->car0_uah, 0, full);
}

static void mt6357_charger_work(struct work_struct *work)
{
	struct mt6357_charger *chg = container_of(to_delayed_work(work),
						  struct mt6357_charger, work);
	int uv;

	mutex_lock(&chg->lock);
	chg->online = mt6357_charger_online(chg);
	if (mt6357_read_fg_current(chg, &chg->current_ua))
		chg->current_ua = 0;
	if (mt6357_read_temp(chg, &chg->temp_decic))
		chg->temp_decic = INT_MIN;
	if (iio_read_channel_processed(chg->vbat, &uv) >= 0) {
		chg->vbat_uv = uv * 1000;
		mt6357_gauge_update(chg);
	}
	mutex_unlock(&chg->lock);

	schedule_delayed_work(&chg->work, POLL_INTERVAL);
}

static int mt6357_charger_get_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       union power_supply_propval *val)
{
	struct mt6357_charger *chg = power_supply_get_drvdata(psy);

	if (psp != POWER_SUPPLY_PROP_ONLINE)
		return -EINVAL;

	val->intval = mt6357_charger_online(chg);
	return 0;
}

static int mt6357_battery_get_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       union power_supply_propval *val)
{
	struct mt6357_charger *chg = power_supply_get_drvdata(psy);
	int full = chg->info->charge_full_design_uah;
	int ret = 0;

	mutex_lock(&chg->lock);
	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = mt6357_battery_status(chg);
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = chg->info->technology;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = chg->vbat_uv;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = chg->current_ua;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		if (chg->temp_decic == INT_MIN)
			ret = -ENODATA;
		else
			val->intval = chg->temp_decic;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		if (!chg->gauge_ready)
			ret = -ENODATA;
		else
			val->intval = DIV_ROUND_CLOSEST_ULL((u64)chg->charge_uah * 100, full);
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		if (!chg->gauge_ready)
			ret = -ENODATA;
		else
			val->intval = chg->charge_uah;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = full;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		val->intval = chg->info->voltage_max_design_uv;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN:
		val->intval = chg->info->voltage_min_design_uv;
		break;
	default:
		ret = -EINVAL;
	}
	mutex_unlock(&chg->lock);
	return ret;
}

static const struct power_supply_desc mt6357_charger_desc = {
	.name = "mt6357-charger",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = mt6357_charger_props,
	.num_properties = ARRAY_SIZE(mt6357_charger_props),
	.get_property = mt6357_charger_get_property,
};

static const struct power_supply_desc mt6357_battery_desc = {
	.name = "battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = mt6357_battery_props,
	.num_properties = ARRAY_SIZE(mt6357_battery_props),
	.get_property = mt6357_battery_get_property,
};

static irqreturn_t mt6357_charger_irq(int irq, void *data)
{
	struct mt6357_charger *chg = data;
	bool online = mt6357_charger_online(chg);

	if (online != chg->online) {
		chg->online = online;
		mod_delayed_work(system_wq, &chg->work, 0);
		power_supply_changed(chg->psy);
		power_supply_changed(chg->bat);
	}
	return IRQ_HANDLED;
}

static void mt6357_charger_cancel(void *data)
{
	struct mt6357_charger *chg = data;

	cancel_delayed_work_sync(&chg->work);
}

static int mt6357_charger_probe(struct platform_device *pdev)
{
	struct mt6397_chip *pmic = dev_get_drvdata(pdev->dev.parent);
	struct power_supply_config cfg = {};
	struct device *dev = &pdev->dev;
	struct mt6357_charger *chg;
	int irq, ret;

	chg = devm_kzalloc(dev, sizeof(*chg), GFP_KERNEL);
	if (!chg)
		return -ENOMEM;
	chg->dev = dev;
	chg->regmap = pmic->regmap;
	chg->temp_decic = INT_MIN;
	ret = devm_mutex_init(dev, &chg->lock);
	if (ret)
		return ret;
	INIT_DELAYED_WORK(&chg->work, mt6357_charger_work);

	chg->vbat = devm_iio_channel_get(dev, "vbat");
	if (IS_ERR(chg->vbat))
		return dev_err_probe(dev, PTR_ERR(chg->vbat), "no vbat channel\n");
	chg->temp = devm_iio_channel_get(dev, "temp");
	if (IS_ERR(chg->temp))
		return dev_err_probe(dev, PTR_ERR(chg->temp), "no temp channel\n");

	cfg.drv_data = chg;
	cfg.fwnode = dev_fwnode(dev);
	chg->psy = devm_power_supply_register(dev, &mt6357_charger_desc, &cfg);
	if (IS_ERR(chg->psy))
		return dev_err_probe(dev, PTR_ERR(chg->psy),
				     "Failed to register power supply\n");

	ret = power_supply_get_battery_info(chg->psy, &chg->info);
	if (ret)
		return dev_err_probe(dev, ret, "no monitored-battery\n");
	if (chg->info->charge_full_design_uah <= 0 ||
	    chg->info->constant_charge_voltage_max_uv <= 0)
		return dev_err_probe(dev, -EINVAL,
				     "battery needs capacity and charge voltage\n");

	cfg.fwnode = NULL;
	chg->bat = devm_power_supply_register(dev, &mt6357_battery_desc, &cfg);
	if (IS_ERR(chg->bat))
		return dev_err_probe(dev, PTR_ERR(chg->bat),
				     "Failed to register battery\n");

	ret = devm_add_action_or_reset(dev, mt6357_charger_cancel, chg);
	if (ret)
		return ret;

	irq = platform_get_irq_byname(pdev, "chrdet");
	if (irq < 0)
		return irq;
	ret = devm_request_threaded_irq(dev, irq, NULL, mt6357_charger_irq,
					IRQF_ONESHOT, "mt6357-chrdet", chg);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to request IRQ\n");

	chg->online = mt6357_charger_online(chg);
	schedule_delayed_work(&chg->work, 0);
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

MODULE_DESCRIPTION("MediaTek MT6357 charger detection and fuel gauge");
MODULE_LICENSE("GPL");
