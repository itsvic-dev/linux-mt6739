// SPDX-License-Identifier: GPL-2.0-only
/* MT6357 PMIC linear charger and fuel gauge. */

#include <linux/bitfield.h>
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
#define CHR_TOP_CON3_VCDT_HV_VTH	GENMASK(10, 6)
#define CHR_TOP_CON3_VBAT_OV_VTH	GENMASK(13, 11)
#define CHR_TOP_CON5_VBAT_CV_VTH	GENMASK(4, 0)
#define CHR_TOP_CON5_PCHR_FLAG_EN	BIT(9)

#define CHR_CON0_CS_VTH			GENMASK(3, 0)
#define CHR_CON0_CS_EN			BIT(4)
#define CHR_CON0_VBAT_OV_EN		BIT(5)
#define CHR_CON0_VBAT_CV_EN		BIT(7)
#define CHR_CON0_VCDT_HV_EN		BIT(11)
#define CHR_CON1_CSDAC_MODE		BIT(7)
#define CHR_CON1_HWCV_EN		BIT(9)
#define CHR_CON1_ULC_DET_EN		BIT(10)
#define CHR_CON1_CSDAC_EN		BIT(11)
#define CHR_CON1_CHR_EN			BIT(12)
#define CHR_CON9_CHRWDT_TD		GENMASK(5, 2)
#define CHR_CON9_CHRWDT_EN		BIT(6)
#define CHR_CON9_CHRWDT_WR		BIT(7)

#define FGADC_CON1_SW_CTRL		GENMASK(3, 0)
#define FGADC_CON1_SW_READ_PRE		1
#define FGADC_CON1_SW_CLEAR		8
#define FGADC_CON1_LATCHDATA_ST		BIT(15)

/* 7.0 V charger input and 4.45 V battery over-voltage, as in the vendor driver. */
#define VCDT_HV_VTH_7V0			0xd
#define VBAT_OV_VTH_4V45		4
/* About 32 s. */
#define CHRWDT_TD_32S			3

/* uA per FG_CURRENT LSB with a 10 mOhm sense resistor, times 1000. */
#define FG_CURRENT_UNIT_NA		305176
/* Stock CAR_TUNE_VALUE. */
#define FG_CAR_TUNE_PERMILLE		930
/* Coulomb counter: 10.85 uAh per 2^11 LSB with a 10 mOhm sense resistor. */
#define FG_CAR_UNIT_NAH			10850
#define FG_CAR_SHIFT			11
/* Battery and sense resistance, to estimate OCV under load. */
#define BAT_R_MOHM			150

#define POLL_INTERVAL			(10 * HZ)
#define FULL_TIME			(60 * HZ)
#define RECHARGE_DROP_UV		150000
#define FULL_MARGIN_UV			50000
#define TEMP_MIN_DECIC			0
#define TEMP_MAX_DECIC			450

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

static const u32 cs_vth_ua[] = {
	2000000, 1600000, 1500000, 1350000, 1200000, 1100000, 1000000, 900000,
	800000, 700000, 650000, 550000, 450000, 300000, 200000, 70000,
};

struct mt6357_charger {
	struct device *dev;
	struct regmap *regmap;
	struct power_supply *psy;
	struct iio_channel *vbat;
	struct iio_channel *temp;
	struct delayed_work work;
	struct mutex lock;
	u32 cv_uv;
	u32 cc_ua;
	u32 term_ua;
	int status;
	unsigned long full_since;
	int temp_decic;
	int current_ua;
	bool online;

	struct power_supply *bat;
	struct power_supply_battery_info *info;
	bool gauge_ready;
	s64 car0_uah;
	int charge0_uah;
	int charge_uah;
	int vbat_uv;
};

static const enum power_supply_property mt6357_charger_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
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

static int mt6357_cv_index(u32 uv)
{
	static const u32 low[] = { 3900000, 4000000, 4050000, 4100000, 4150000, 4200000 };
	int i;

	for (i = ARRAY_SIZE(low) - 1; i > 0; i--)
		if (uv >= low[i])
			break;
	if (uv < 4212500)
		return i;
	return min_t(int, 6 + (uv - 4212500) / 12500, 29);
}

static int mt6357_cs_index(u32 ua)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(cs_vth_ua); i++)
		if (cs_vth_ua[i] <= ua)
			return i;
	return ARRAY_SIZE(cs_vth_ua) - 1;
}

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

	if (chg->status == POWER_SUPPLY_STATUS_FULL) {
		chg->charge0_uah = full;
		chg->car0_uah = car;
	}

	chg->charge_uah = clamp_t(s64, chg->charge0_uah + car - chg->car0_uah, 0, full);
}

static void mt6357_charger_kick(struct mt6357_charger *chg)
{
	regmap_update_bits(chg->regmap, MT6357_CHR_CON9, CHR_CON9_CHRWDT_WR,
			   CHR_CON9_CHRWDT_WR);
}

static void mt6357_charger_enable(struct mt6357_charger *chg)
{
	struct regmap *map = chg->regmap;

	regmap_update_bits(map, MT6357_CHR_TOP_CON5, CHR_TOP_CON5_VBAT_CV_VTH,
			   FIELD_PREP(CHR_TOP_CON5_VBAT_CV_VTH, mt6357_cv_index(chg->cv_uv)));
	regmap_update_bits(map, MT6357_CHR_CON0, CHR_CON0_CS_VTH,
			   FIELD_PREP(CHR_CON0_CS_VTH, mt6357_cs_index(chg->cc_ua)));

	regmap_update_bits(map, MT6357_CHR_CON1, CHR_CON1_CSDAC_MODE, CHR_CON1_CSDAC_MODE);
	regmap_update_bits(map, MT6357_CHR_CON0, CHR_CON0_CS_EN, CHR_CON0_CS_EN);
	regmap_update_bits(map, MT6357_CHR_CON1, CHR_CON1_ULC_DET_EN, CHR_CON1_ULC_DET_EN);
	regmap_update_bits(map, MT6357_CHR_CON1, CHR_CON1_HWCV_EN, CHR_CON1_HWCV_EN);
	regmap_update_bits(map, MT6357_CHR_CON0, CHR_CON0_VBAT_CV_EN, CHR_CON0_VBAT_CV_EN);
	regmap_update_bits(map, MT6357_CHR_TOP_CON5, CHR_TOP_CON5_PCHR_FLAG_EN,
			   CHR_TOP_CON5_PCHR_FLAG_EN);
	regmap_update_bits(map, MT6357_CHR_CON1, CHR_CON1_CSDAC_EN, CHR_CON1_CSDAC_EN);
	regmap_update_bits(map, MT6357_CHR_CON1, CHR_CON1_CHR_EN, CHR_CON1_CHR_EN);
}

static void mt6357_charger_disable(struct mt6357_charger *chg)
{
	struct regmap *map = chg->regmap;

	regmap_update_bits(map, MT6357_CHR_CON1, CHR_CON1_CHR_EN, 0);
	regmap_update_bits(map, MT6357_CHR_CON1, CHR_CON1_CSDAC_EN, 0);
	regmap_update_bits(map, MT6357_CHR_TOP_CON5, CHR_TOP_CON5_PCHR_FLAG_EN, 0);
	regmap_update_bits(map, MT6357_CHR_CON0, CHR_CON0_VBAT_CV_EN, 0);
	regmap_update_bits(map, MT6357_CHR_CON1, CHR_CON1_HWCV_EN, 0);
	regmap_update_bits(map, MT6357_CHR_CON0, CHR_CON0_CS_EN, 0);
}

static void mt6357_charger_set_status(struct mt6357_charger *chg, int status)
{
	if (status == POWER_SUPPLY_STATUS_CHARGING) {
		mt6357_charger_kick(chg);
		if (chg->status != POWER_SUPPLY_STATUS_CHARGING) {
			chg->full_since = 0;
			mt6357_charger_enable(chg);
		}
	} else if (chg->status == POWER_SUPPLY_STATUS_CHARGING ||
		   chg->status == POWER_SUPPLY_STATUS_UNKNOWN) {
		mt6357_charger_disable(chg);
	}

	if (chg->status != status) {
		dev_info(chg->dev, "status %d -> %d (%d.%d C, %d uA)\n", chg->status,
			 status, chg->temp_decic / 10, abs(chg->temp_decic % 10),
			 chg->current_ua);
		chg->status = status;
		power_supply_changed(chg->psy);
		power_supply_changed(chg->bat);
	}
}

static void mt6357_charger_update(struct mt6357_charger *chg)
{
	int vbat_uv, ret;

	if (mt6357_read_fg_current(chg, &chg->current_ua))
		chg->current_ua = 0;
	if (mt6357_read_temp(chg, &chg->temp_decic))
		chg->temp_decic = INT_MIN;
	ret = iio_read_channel_processed(chg->vbat, &vbat_uv);
	vbat_uv = ret < 0 ? 0 : vbat_uv * 1000;
	chg->vbat_uv = vbat_uv;
	if (vbat_uv)
		mt6357_gauge_update(chg);

	if (!mt6357_charger_online(chg)) {
		mt6357_charger_set_status(chg, POWER_SUPPLY_STATUS_DISCHARGING);
		return;
	}

	ret = chg->temp_decic == INT_MIN ? -ERANGE : 0;
	if (ret || chg->temp_decic < TEMP_MIN_DECIC || chg->temp_decic > TEMP_MAX_DECIC) {
		if (ret)
			dev_warn_ratelimited(chg->dev, "no battery temperature: %d\n", ret);
		mt6357_charger_set_status(chg, POWER_SUPPLY_STATUS_NOT_CHARGING);
		return;
	}

	if (!vbat_uv) {
		mt6357_charger_set_status(chg, POWER_SUPPLY_STATUS_NOT_CHARGING);
		return;
	}

	switch (chg->status) {
	case POWER_SUPPLY_STATUS_FULL:
		if (vbat_uv < chg->cv_uv - RECHARGE_DROP_UV)
			mt6357_charger_set_status(chg, POWER_SUPPLY_STATUS_CHARGING);
		else
			mt6357_charger_set_status(chg, POWER_SUPPLY_STATUS_FULL);
		break;
	case POWER_SUPPLY_STATUS_CHARGING:
		if (vbat_uv < chg->cv_uv - FULL_MARGIN_UV || chg->current_ua >= chg->term_ua)
			chg->full_since = 0;
		else if (!chg->full_since)
			chg->full_since = jiffies ?: 1;
		if (chg->full_since && time_after(jiffies, chg->full_since + FULL_TIME))
			mt6357_charger_set_status(chg, POWER_SUPPLY_STATUS_FULL);
		else
			mt6357_charger_set_status(chg, POWER_SUPPLY_STATUS_CHARGING);
		break;
	default:
		mt6357_charger_set_status(chg, POWER_SUPPLY_STATUS_CHARGING);
		break;
	}
}

static void mt6357_charger_work(struct work_struct *work)
{
	struct mt6357_charger *chg = container_of(to_delayed_work(work),
						  struct mt6357_charger, work);

	mutex_lock(&chg->lock);
	mt6357_charger_update(chg);
	mutex_unlock(&chg->lock);

	schedule_delayed_work(&chg->work, POLL_INTERVAL);
}

static int mt6357_charger_get_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       union power_supply_propval *val)
{
	struct mt6357_charger *chg = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = mt6357_charger_online(chg);
		break;
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = chg->status;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
		val->intval = chg->cv_uv;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		val->intval = cs_vth_ua[mt6357_cs_index(chg->cc_ua)];
		break;
	default:
		return -EINVAL;
	}
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
		val->intval = chg->status == POWER_SUPPLY_STATUS_UNKNOWN ?
			      POWER_SUPPLY_STATUS_DISCHARGING : chg->status;
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

static const struct power_supply_desc mt6357_battery_desc = {
	.name = "battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = mt6357_battery_props,
	.num_properties = ARRAY_SIZE(mt6357_battery_props),
	.get_property = mt6357_battery_get_property,
};

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
	bool online = mt6357_charger_online(chg);

	if (online != chg->online) {
		chg->online = online;
		mod_delayed_work(system_wq, &chg->work, 0);
		power_supply_changed(chg->psy);
	}
	return IRQ_HANDLED;
}

static void mt6357_charger_stop(void *data)
{
	struct mt6357_charger *chg = data;

	cancel_delayed_work_sync(&chg->work);
	mt6357_charger_disable(chg);
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
	platform_set_drvdata(pdev, chg);
	chg->status = POWER_SUPPLY_STATUS_UNKNOWN;
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
	chg->cv_uv = chg->info->constant_charge_voltage_max_uv;
	chg->cc_ua = chg->info->constant_charge_current_max_ua;
	chg->term_ua = chg->info->charge_term_current_ua;
	if (chg->cv_uv <= 0 || chg->cc_ua <= 0 || chg->term_ua <= 0 ||
	    chg->info->charge_full_design_uah <= 0)
		return dev_err_probe(dev, -EINVAL,
				     "battery needs charge voltage, currents and capacity\n");

	cfg.fwnode = NULL;
	chg->bat = devm_power_supply_register(dev, &mt6357_battery_desc, &cfg);
	if (IS_ERR(chg->bat))
		return dev_err_probe(dev, PTR_ERR(chg->bat),
				     "Failed to register battery\n");

	regmap_update_bits(chg->regmap, MT6357_CHR_CON1, CHR_CON1_CHR_EN, 0);
	regmap_update_bits(chg->regmap, MT6357_CHR_TOP_CON3,
			   CHR_TOP_CON3_VCDT_HV_VTH | CHR_TOP_CON3_VBAT_OV_VTH,
			   FIELD_PREP(CHR_TOP_CON3_VCDT_HV_VTH, VCDT_HV_VTH_7V0) |
			   FIELD_PREP(CHR_TOP_CON3_VBAT_OV_VTH, VBAT_OV_VTH_4V45));
	regmap_update_bits(chg->regmap, MT6357_CHR_CON0,
			   CHR_CON0_VCDT_HV_EN | CHR_CON0_VBAT_OV_EN,
			   CHR_CON0_VCDT_HV_EN | CHR_CON0_VBAT_OV_EN);
	regmap_update_bits(chg->regmap, MT6357_CHR_CON9,
			   CHR_CON9_CHRWDT_TD | CHR_CON9_CHRWDT_EN | CHR_CON9_CHRWDT_WR,
			   FIELD_PREP(CHR_CON9_CHRWDT_TD, CHRWDT_TD_32S) |
			   CHR_CON9_CHRWDT_EN | CHR_CON9_CHRWDT_WR);

	ret = devm_add_action_or_reset(dev, mt6357_charger_stop, chg);
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

static void mt6357_charger_shutdown(struct platform_device *pdev)
{
	struct mt6357_charger *chg = platform_get_drvdata(pdev);

	if (chg)
		mt6357_charger_stop(chg);
}

static const struct of_device_id mt6357_charger_of_match[] = {
	{ .compatible = "mediatek,mt6357-charger" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6357_charger_of_match);

static struct platform_driver mt6357_charger_driver = {
	.probe = mt6357_charger_probe,
	.shutdown = mt6357_charger_shutdown,
	.driver = {
		.name = "mt6357-charger",
		.of_match_table = mt6357_charger_of_match,
	},
};
module_platform_driver(mt6357_charger_driver);

MODULE_DESCRIPTION("MediaTek MT6357 linear charger");
MODULE_LICENSE("GPL");
