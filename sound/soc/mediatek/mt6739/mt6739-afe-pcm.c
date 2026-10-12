// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2018 MediaTek Inc.

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include "mt6739-afe-common.h"
#include "mt6739-reg.h"
#include "../common/mtk-afe-fe-dai.h"
#include "../common/mtk-afe-platform-driver.h"

enum {
	MTK_AFE_RATE_8K = 0,
	MTK_AFE_RATE_11K = 1,
	MTK_AFE_RATE_12K = 2,
	MTK_AFE_RATE_16K = 4,
	MTK_AFE_RATE_22K = 5,
	MTK_AFE_RATE_24K = 6,
	MTK_AFE_RATE_32K = 8,
	MTK_AFE_RATE_44K = 9,
	MTK_AFE_RATE_48K = 10,
};

static unsigned int mt6739_rate_transform(struct device *dev, unsigned int rate)
{
	switch (rate) {
	case 8000:
		return MTK_AFE_RATE_8K;
	case 11025:
		return MTK_AFE_RATE_11K;
	case 12000:
		return MTK_AFE_RATE_12K;
	case 16000:
		return MTK_AFE_RATE_16K;
	case 22050:
		return MTK_AFE_RATE_22K;
	case 24000:
		return MTK_AFE_RATE_24K;
	case 32000:
		return MTK_AFE_RATE_32K;
	case 44100:
		return MTK_AFE_RATE_44K;
	case 48000:
		return MTK_AFE_RATE_48K;
	default:
		dev_warn(dev, "rate %u invalid, using 48000\n", rate);
		return MTK_AFE_RATE_48K;
	}
}

static int mt6739_memif_fs(struct snd_pcm_substream *substream,
			   unsigned int rate)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_component *component =
		snd_soc_rtdcom_lookup(rtd, AFE_PCM_NAME);
	struct mtk_base_afe *afe = snd_soc_component_get_drvdata(component);

	return mt6739_rate_transform(afe->dev, rate);
}

static const struct snd_pcm_hardware mt6739_afe_hardware = {
	.info = SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_MMAP_VALID,
	.formats = SNDRV_PCM_FMTBIT_S16_LE |
		   SNDRV_PCM_FMTBIT_S24_LE |
		   SNDRV_PCM_FMTBIT_S32_LE,
	.period_bytes_min = 256,
	.period_bytes_max = 4 * 48 * 1024,
	.periods_min = 2,
	.periods_max = 256,
	.buffer_bytes_max = 8 * 48 * 1024,
	.fifo_size = 0,
};

#define MTK_PCM_RATES SNDRV_PCM_RATE_8000_48000

#define MTK_PCM_FORMATS (SNDRV_PCM_FMTBIT_S16_LE |\
			 SNDRV_PCM_FMTBIT_S24_LE |\
			 SNDRV_PCM_FMTBIT_S32_LE)

#define MT6739_FE_DAI(_name, _id, _dir)				\
	{							\
		.name = _name,					\
		.id = _id,					\
		._dir = {					\
			.stream_name = _name,			\
			.channels_min = 1,			\
			.channels_max = 2,			\
			.rates = MTK_PCM_RATES,			\
			.formats = MTK_PCM_FORMATS,		\
		},						\
		.ops = &mtk_afe_fe_ops,				\
	}

static struct snd_soc_dai_driver mt6739_memif_dai_driver[] = {
	MT6739_FE_DAI("DL1", MT6739_MEMIF_DL1, playback),
	MT6739_FE_DAI("DL2", MT6739_MEMIF_DL2, playback),
	MT6739_FE_DAI("UL1", MT6739_MEMIF_VUL, capture),
	MT6739_FE_DAI("UL2", MT6739_MEMIF_AWB, capture),
};

static const struct snd_kcontrol_new memif_ul1_ch1_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN9, I_ADDA_UL_CH1, 1, 0),
};

static const struct snd_kcontrol_new memif_ul1_ch2_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN10, I_ADDA_UL_CH2, 1, 0),
};

static const struct snd_kcontrol_new memif_ul2_ch1_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN5, I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL1_CH1", AFE_CONN5, I_DL1_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL2_CH1", AFE_CONN5, I_DL2_CH1, 1, 0),
};

static const struct snd_kcontrol_new memif_ul2_ch2_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN6, I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL1_CH2", AFE_CONN6, I_DL1_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL2_CH2", AFE_CONN6, I_DL2_CH2, 1, 0),
};

static const struct snd_soc_dapm_widget mt6739_memif_widgets[] = {
	SND_SOC_DAPM_MIXER("UL1_CH1", SND_SOC_NOPM, 0, 0,
			   memif_ul1_ch1_mix, ARRAY_SIZE(memif_ul1_ch1_mix)),
	SND_SOC_DAPM_MIXER("UL1_CH2", SND_SOC_NOPM, 0, 0,
			   memif_ul1_ch2_mix, ARRAY_SIZE(memif_ul1_ch2_mix)),
	SND_SOC_DAPM_MIXER("UL2_CH1", SND_SOC_NOPM, 0, 0,
			   memif_ul2_ch1_mix, ARRAY_SIZE(memif_ul2_ch1_mix)),
	SND_SOC_DAPM_MIXER("UL2_CH2", SND_SOC_NOPM, 0, 0,
			   memif_ul2_ch2_mix, ARRAY_SIZE(memif_ul2_ch2_mix)),
};

static const struct snd_soc_dapm_route mt6739_memif_routes[] = {
	{"UL1", NULL, "UL1_CH1"},
	{"UL1", NULL, "UL1_CH2"},
	{"UL1_CH1", "ADDA_UL_CH1", "ADDA Capture"},
	{"UL1_CH2", "ADDA_UL_CH2", "ADDA Capture"},

	{"UL2", NULL, "UL2_CH1"},
	{"UL2", NULL, "UL2_CH2"},
	{"UL2_CH1", "ADDA_UL_CH1", "ADDA Capture"},
	{"UL2_CH2", "ADDA_UL_CH2", "ADDA Capture"},
	{"UL2_CH1", "DL1_CH1", "DL1"},
	{"UL2_CH2", "DL1_CH2", "DL1"},
	{"UL2_CH1", "DL2_CH1", "DL2"},
	{"UL2_CH2", "DL2_CH2", "DL2"},
};

static const struct snd_soc_component_driver mt6739_afe_pcm_dai_component = {
	.name = "mt6739-afe-pcm-dai",
};

#define MT6739_MEMIF(_id, _en_shift)				\
	[MT6739_MEMIF_##_id] = {				\
		.name = #_id,					\
		.id = MT6739_MEMIF_##_id,			\
		.reg_ofs_base = AFE_##_id##_BASE,		\
		.reg_ofs_cur = AFE_##_id##_CUR,			\
		.reg_ofs_end = AFE_##_id##_END,			\
		.reg_ofs_base_msb = AFE_##_id##_BASE_MSB,	\
		.reg_ofs_cur_msb = AFE_##_id##_CUR_MSB,		\
		.reg_ofs_end_msb = AFE_##_id##_END_MSB,		\
		.fs_reg = AFE_DAC_CON1,				\
		.fs_shift = _id##_MODE_SFT,			\
		.fs_maskbit = MEMIF_MODE_MASK,			\
		.mono_reg = AFE_DAC_CON1,			\
		.mono_shift = _id##_DATA_SFT,			\
		.enable_reg = AFE_DAC_CON0,			\
		.enable_shift = _en_shift,			\
		.hd_reg = AFE_MEMIF_HD_MODE,			\
		.hd_shift = _id##_HD_SFT,			\
		.hd_align_reg = AFE_MEMIF_HDALIGN,		\
		.hd_align_mshift = _id##_HD_ALIGN_SFT,		\
		.agent_disable_reg = -1,			\
		.agent_disable_shift = -1,			\
		.msb_reg = -1,					\
		.msb_shift = -1,				\
	}

static const struct mtk_base_memif_data memif_data[MT6739_MEMIF_NUM] = {
	MT6739_MEMIF(DL1, DL1_ON_SFT),
	MT6739_MEMIF(DL2, DL2_ON_SFT),
	MT6739_MEMIF(VUL, VUL_ON_SFT),
	MT6739_MEMIF(AWB, AWB_ON_SFT),
};

#define MT6739_AFE_IRQ(_id)					\
	[MT6739_IRQ_##_id] = {					\
		.id = MT6739_IRQ_##_id,				\
		.irq_cnt_reg = AFE_IRQ_MCU_CNT##_id,		\
		.irq_cnt_shift = 0,				\
		.irq_cnt_maskbit = 0x3ffff,			\
		.irq_fs_reg = AFE_IRQ_MCU_CON1,			\
		.irq_fs_shift = (_id) * 4,			\
		.irq_fs_maskbit = 0xf,				\
		.irq_en_reg = AFE_IRQ_MCU_CON0,			\
		.irq_en_shift = _id,				\
		.irq_clr_reg = AFE_IRQ_MCU_CLR,			\
		.irq_clr_shift = _id,				\
	}

static const struct mtk_base_irq_data irq_data[MT6739_IRQ_NUM] = {
	MT6739_AFE_IRQ(0),
	MT6739_AFE_IRQ(1),
	MT6739_AFE_IRQ(2),
	MT6739_AFE_IRQ(3),
	MT6739_AFE_IRQ(4),
	MT6739_AFE_IRQ(5),
	MT6739_AFE_IRQ(6),
	MT6739_AFE_IRQ(7),
};

static bool mt6739_is_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case AUDIO_TOP_CON0 ... AUDIO_TOP_CON1:	/* bits owned by the clock driver */
	case AUDIO_TOP_CON3:
	case AFE_DL1_CUR ... AFE_DL1_END:
	case AFE_DL2_CUR ... AFE_DL2_END:
	case AFE_AWB_END ... AFE_AWB_CUR:
	case AFE_VUL_END ... AFE_VUL_CUR:
	case AFE_MEMIF_MON0 ... AFE_MEMIF_MON9:
	case AFE_ADDA_SRC_DEBUG_MON0 ... AFE_ADDA_SRC_DEBUG_MON1:
	case AFE_ADDA_UL_SRC_MON0 ... AFE_ADDA_UL_SRC_MON1:
	case AFE_DAC_MON:
	case AFE_IRQ0_MCU_CNT_MON ... AFE_IRQ6_MCU_CNT_MON:
	case AFE_IRQ3_MCU_CNT_MON ... AFE_IRQ4_MCU_CNT_MON:
	case AFE_IRQ_MCU_STATUS ... AFE_IRQ_MCU_CLR:
	case AFE_IRQ_MCU_MON2:
	case AFE_IRQ1_MCU_CNT_MON ... AFE_IRQ5_MCU_CNT_MON:
	case AFE_IRQ7_MCU_CNT_MON:
	case AFE_DL1_CUR_MSB:
	case AFE_DL2_CUR_MSB:
	case AFE_AWB_CUR_MSB:
	case AFE_VUL_CUR_MSB:
	case AFE_ADDA_DL_SDM_FIFO_MON ... AFE_ADDA_DL_SDM_OUT_MON:
	case AFE_ADDA_MTKAIF_MON0 ... AFE_ADDA_MTKAIF_MON1:
	case AFE_AUD_PAD_TOP:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config mt6739_afe_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.volatile_reg = mt6739_is_volatile_reg,
	.max_register = AFE_MAX_REGISTER,
	.num_reg_defaults_raw = AFE_MAX_REGISTER,
	.cache_type = REGCACHE_FLAT,
};

static irqreturn_t mt6739_afe_irq_handler(int irq_id, void *dev)
{
	struct mtk_base_afe *afe = dev;
	unsigned int status, mcu_en, status_mcu;
	int i;

	regmap_read(afe->regmap, AFE_IRQ_MCU_EN, &mcu_en);
	regmap_read(afe->regmap, AFE_IRQ_MCU_STATUS, &status);
	status_mcu = status & mcu_en & AFE_IRQ_STATUS_BITS;

	if (!status_mcu) {
		dev_err_ratelimited(afe->dev, "irq status 0x%x, mcu_en 0x%x\n",
				    status, mcu_en);
		regmap_write(afe->regmap, AFE_IRQ_MCU_CLR, status);
		return IRQ_NONE;
	}

	for (i = 0; i < MT6739_MEMIF_NUM; i++) {
		struct mtk_base_afe_memif *memif = &afe->memif[i];
		struct mtk_base_afe_irq *irq;

		if (!memif->substream || memif->irq_usage < 0)
			continue;

		irq = &afe->irqs[memif->irq_usage];
		if (status_mcu & BIT(irq->irq_data->irq_en_shift))
			snd_pcm_period_elapsed(memif->substream);
	}

	regmap_write(afe->regmap, AFE_IRQ_MCU_CLR, status_mcu);

	return IRQ_HANDLED;
}

enum {
	CLK_INFRA_SYS_AUDIO,
	CLK_MUX_AUDIO,
	CLK_MUX_AUDIOINTBUS,
	CLK_AFE,
	CLK_NUM
};

static const char *const aud_clks[CLK_NUM] = {
	[CLK_INFRA_SYS_AUDIO] = "aud_infra_clk",
	[CLK_MUX_AUDIO] = "top_mux_audio",
	[CLK_MUX_AUDIOINTBUS] = "top_mux_aud_intbus",
	[CLK_AFE] = "aud_afe_clk",
};

static int mt6739_afe_enable_clock(struct mtk_base_afe *afe)
{
	struct mt6739_afe_private *afe_priv = afe->platform_priv;
	int i, ret;

	for (i = 0; i < CLK_NUM; i++) {
		ret = clk_prepare_enable(afe_priv->clk[i]);
		if (ret) {
			dev_err(afe->dev, "failed to enable %s: %d\n",
				aud_clks[i], ret);
			while (--i >= 0)
				clk_disable_unprepare(afe_priv->clk[i]);
			return ret;
		}
	}

	return 0;
}

static void mt6739_afe_disable_clock(struct mtk_base_afe *afe)
{
	struct mt6739_afe_private *afe_priv = afe->platform_priv;
	int i;

	for (i = CLK_NUM - 1; i >= 0; i--)
		clk_disable_unprepare(afe_priv->clk[i]);
}

static int mt6739_afe_runtime_suspend(struct device *dev)
{
	struct mtk_base_afe *afe = dev_get_drvdata(dev);
	struct mt6739_afe_private *afe_priv = afe->platform_priv;
	unsigned int value;
	int ret;

	if (!afe->regmap || afe_priv->pm_runtime_bypass_reg_ctl)
		goto skip_regmap;

	regmap_update_bits(afe->regmap, AFE_DAC_CON0, AFE_ON_MASK_SFT, 0);

	ret = regmap_read_poll_timeout(afe->regmap, AFE_DAC_MON, value,
				       !(value & AFE_ON_RETM_MASK_SFT),
				       20, 1000 * 1000);
	if (ret)
		dev_warn(afe->dev, "AFE did not stop: %d\n", ret);

	/* the vendor driver clears the IRQ status twice */
	regmap_write(afe->regmap, AFE_IRQ_MCU_CLR, 0xffff);
	regmap_write(afe->regmap, AFE_IRQ_MCU_CLR, 0xffff);

skip_regmap:
	mt6739_afe_disable_clock(afe);

	if (afe->regmap && !afe_priv->pm_runtime_bypass_reg_ctl) {
		regcache_cache_only(afe->regmap, true);
		regcache_mark_dirty(afe->regmap);
	}

	return 0;
}

static int mt6739_afe_runtime_resume(struct device *dev)
{
	struct mtk_base_afe *afe = dev_get_drvdata(dev);
	struct mt6739_afe_private *afe_priv = afe->platform_priv;
	bool reg_ctl = afe->regmap && !afe_priv->pm_runtime_bypass_reg_ctl;
	int ret;

	/* the AUDIOSYS clock gates share this regmap */
	if (reg_ctl)
		regcache_cache_only(afe->regmap, false);

	ret = mt6739_afe_enable_clock(afe);
	if (ret) {
		if (reg_ctl)
			regcache_cache_only(afe->regmap, true);
		return ret;
	}

	if (!reg_ctl)
		return 0;

	regcache_sync(afe->regmap);

	regmap_update_bits(afe->regmap, AUDIO_TOP_CON0, AUD_TOP_DCM_EN,
			   AUD_TOP_DCM_EN);

	/* 32-bit samples from the CPU are 8_24 */
	regmap_update_bits(afe->regmap, AFE_MEMIF_MSB, CPU_HD_ALIGN_MASK_SFT, 0);

	regmap_write(afe->regmap, AFE_CONN_24BIT, 0xffffffff);
	regmap_write(afe->regmap, AFE_CONN_24BIT_1, 0xffffffff);

	/* signal the IRQs to the AP */
	regmap_update_bits(afe->regmap, AFE_IRQ_MCU_EN, AFE_IRQ_STATUS_BITS,
			   AFE_IRQ_STATUS_BITS);

	regmap_update_bits(afe->regmap, AFE_DAC_CON0, AFE_ON_MASK_SFT,
			   AFE_ON_MASK_SFT);

	return 0;
}

static int mt6739_dai_memif_register(struct mtk_base_afe *afe)
{
	struct mtk_base_afe_dai *dai;

	dai = devm_kzalloc(afe->dev, sizeof(*dai), GFP_KERNEL);
	if (!dai)
		return -ENOMEM;

	list_add(&dai->list, &afe->sub_dais);

	dai->dai_drivers = mt6739_memif_dai_driver;
	dai->num_dai_drivers = ARRAY_SIZE(mt6739_memif_dai_driver);
	dai->dapm_widgets = mt6739_memif_widgets;
	dai->num_dapm_widgets = ARRAY_SIZE(mt6739_memif_widgets);
	dai->dapm_routes = mt6739_memif_routes;
	dai->num_dapm_routes = ARRAY_SIZE(mt6739_memif_routes);

	return 0;
}

static int mt6739_afe_pcm_dev_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6739_afe_private *afe_priv;
	struct mtk_base_afe *afe;
	int i, irq_id, ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	afe = devm_kzalloc(dev, sizeof(*afe), GFP_KERNEL);
	if (!afe)
		return -ENOMEM;
	platform_set_drvdata(pdev, afe);
	afe->dev = dev;

	afe_priv = devm_kzalloc(dev, sizeof(*afe_priv), GFP_KERNEL);
	if (!afe_priv)
		return -ENOMEM;
	afe->platform_priv = afe_priv;

	afe_priv->clk = devm_kcalloc(dev, CLK_NUM, sizeof(*afe_priv->clk),
				     GFP_KERNEL);
	if (!afe_priv->clk)
		return -ENOMEM;

	for (i = 0; i < CLK_NUM; i++) {
		afe_priv->clk[i] = devm_clk_get(dev, aud_clks[i]);
		if (IS_ERR(afe_priv->clk[i]))
			return dev_err_probe(dev, PTR_ERR(afe_priv->clk[i]),
					     "failed to get %s\n", aud_clks[i]);
	}

	afe->preallocate_buffers = true;

	afe->regmap = syscon_node_to_regmap(dev->parent->of_node);
	if (IS_ERR(afe->regmap))
		return dev_err_probe(dev, PTR_ERR(afe->regmap),
				     "could not get regmap from parent\n");

	ret = regmap_attach_dev(dev, afe->regmap, &mt6739_afe_regmap_config);
	if (ret)
		return ret;

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	/* read the register defaults with the clocks on */
	afe_priv->pm_runtime_bypass_reg_ctl = true;
	ret = pm_runtime_resume_and_get(dev);
	if (ret) {
		afe_priv->pm_runtime_bypass_reg_ctl = false;
		return ret;
	}

	ret = regmap_reinit_cache(afe->regmap, &mt6739_afe_regmap_config);
	pm_runtime_put_sync(dev);
	afe_priv->pm_runtime_bypass_reg_ctl = false;
	if (ret)
		return dev_err_probe(dev, ret, "regmap_reinit_cache failed\n");

	regcache_cache_only(afe->regmap, true);
	regcache_mark_dirty(afe->regmap);

	afe->memif_size = MT6739_MEMIF_NUM;
	afe->memif = devm_kcalloc(dev, afe->memif_size, sizeof(*afe->memif),
				  GFP_KERNEL);
	if (!afe->memif)
		return -ENOMEM;

	for (i = 0; i < afe->memif_size; i++) {
		afe->memif[i].data = &memif_data[i];
		afe->memif[i].irq_usage = -1;
	}

	mutex_init(&afe->irq_alloc_lock);

	afe->irqs_size = MT6739_IRQ_NUM;
	afe->irqs = devm_kcalloc(dev, afe->irqs_size, sizeof(*afe->irqs),
				 GFP_KERNEL);
	if (!afe->irqs)
		return -ENOMEM;

	for (i = 0; i < afe->irqs_size; i++)
		afe->irqs[i].irq_data = &irq_data[i];

	irq_id = platform_get_irq(pdev, 0);
	if (irq_id < 0)
		return irq_id;

	ret = devm_request_irq(dev, irq_id, mt6739_afe_irq_handler,
			       IRQF_TRIGGER_NONE, "asys-isr", afe);
	if (ret)
		return dev_err_probe(dev, ret, "could not request IRQ\n");

	INIT_LIST_HEAD(&afe->sub_dais);

	ret = mt6739_dai_adda_register(afe);
	if (ret)
		return ret;

	ret = mt6739_dai_memif_register(afe);
	if (ret)
		return ret;

	ret = mtk_afe_combine_sub_dai(afe);
	if (ret)
		return ret;

	afe->mtk_afe_hardware = &mt6739_afe_hardware;
	afe->memif_fs = mt6739_memif_fs;
	afe->irq_fs = mt6739_memif_fs;
	afe->runtime_resume = mt6739_afe_runtime_resume;
	afe->runtime_suspend = mt6739_afe_runtime_suspend;

	ret = devm_snd_soc_register_component(dev, &mtk_afe_pcm_platform,
					      NULL, 0);
	if (ret)
		return ret;

	return devm_snd_soc_register_component(dev,
					       &mt6739_afe_pcm_dai_component,
					       afe->dai_drivers,
					       afe->num_dai_drivers);
}

static const struct of_device_id mt6739_afe_pcm_dt_match[] = {
	{ .compatible = "mediatek,mt6739-audio" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6739_afe_pcm_dt_match);

static const struct dev_pm_ops mt6739_afe_pm_ops = {
	RUNTIME_PM_OPS(mt6739_afe_runtime_suspend,
		       mt6739_afe_runtime_resume, NULL)
};

static struct platform_driver mt6739_afe_pcm_driver = {
	.driver = {
		.name = "mt6739-audio",
		.of_match_table = mt6739_afe_pcm_dt_match,
		.pm = pm_ptr(&mt6739_afe_pm_ops),
	},
	.probe = mt6739_afe_pcm_dev_probe,
};
module_platform_driver(mt6739_afe_pcm_driver);

MODULE_DESCRIPTION("MediaTek MT6739 ALSA SoC AFE platform driver");
MODULE_LICENSE("GPL");
