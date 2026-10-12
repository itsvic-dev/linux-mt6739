// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>
#include <linux/platform_device.h>
#include <sound/soc.h>

#include "../common/mtk-soc-card.h"
#include "../common/mtk-soundcard-driver.h"

enum {
	DAI_LINK_DL1_PLAYBACK,
	DAI_LINK_DL2_PLAYBACK,
	DAI_LINK_UL1_CAPTURE,
	DAI_LINK_UL2_CAPTURE,
	DAI_LINK_ADDA_BE,
};

static const struct snd_soc_dapm_widget mt6739_mt6357_widgets[] = {
	SND_SOC_DAPM_SPK("Speaker", NULL),
	SND_SOC_DAPM_SPK("Earpiece", NULL),
	SND_SOC_DAPM_MIC("Main Mic", NULL),
};

static const struct snd_kcontrol_new mt6739_mt6357_controls[] = {
	SOC_DAPM_PIN_SWITCH("Speaker"),
	SOC_DAPM_PIN_SWITCH("Earpiece"),
	SOC_DAPM_PIN_SWITCH("Main Mic"),
};

SND_SOC_DAILINK_DEFS(playback1,
		     DAILINK_COMP_ARRAY(COMP_CPU("DL1")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(playback2,
		     DAILINK_COMP_ARRAY(COMP_CPU("DL2")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(capture1,
		     DAILINK_COMP_ARRAY(COMP_CPU("UL1")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(capture2,
		     DAILINK_COMP_ARRAY(COMP_CPU("UL2")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(adda,
		     DAILINK_COMP_ARRAY(COMP_CPU("ADDA")),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));

#define MT6739_FE_LINK(_id, _name, _dir, _link)			\
	[_id] = {							\
		.name = _name,						\
		.stream_name = _name,					\
		.id = _id,						\
		.trigger = {						\
			SND_SOC_DPCM_TRIGGER_POST,			\
			SND_SOC_DPCM_TRIGGER_POST			\
		},							\
		.dynamic = 1,						\
		._dir = 1,						\
		.dpcm_merged_rate = 1,					\
		SND_SOC_DAILINK_REG(_link),				\
	}

static struct snd_soc_dai_link mt6739_mt6357_dai_links[] = {
	MT6739_FE_LINK(DAI_LINK_DL1_PLAYBACK, "Playback_1", playback_only, playback1),
	MT6739_FE_LINK(DAI_LINK_DL2_PLAYBACK, "Playback_2", playback_only, playback2),
	MT6739_FE_LINK(DAI_LINK_UL1_CAPTURE, "Capture_1", capture_only, capture1),
	MT6739_FE_LINK(DAI_LINK_UL2_CAPTURE, "Capture_2", capture_only, capture2),
	[DAI_LINK_ADDA_BE] = {
		.name = "ADDA_BE",
		.id = DAI_LINK_ADDA_BE,
		.no_pcm = 1,
		SND_SOC_DAILINK_REG(adda),
	},
};

static struct snd_soc_card mt6739_mt6357_card = {
	.name = "mt6739-mt6357",
	.owner = THIS_MODULE,
	.dai_link = mt6739_mt6357_dai_links,
	.num_links = ARRAY_SIZE(mt6739_mt6357_dai_links),
	.controls = mt6739_mt6357_controls,
	.num_controls = ARRAY_SIZE(mt6739_mt6357_controls),
	.dapm_widgets = mt6739_mt6357_widgets,
	.num_dapm_widgets = ARRAY_SIZE(mt6739_mt6357_widgets),
};

static int mt6739_mt6357_soc_probe(struct mtk_soc_card_data *soc_card_data,
				   bool legacy)
{
	struct snd_soc_card *card = soc_card_data->card_data->card;

	if (legacy)
		return dev_err_probe(card->dev, -EINVAL,
				     "audio-routing is required\n");

	return snd_soc_of_parse_aux_devs(card, "aux-devs");
}

static const struct mtk_soundcard_pdata mt6739_mt6357_pdata = {
	.card_name = "mt6739-mt6357",
	.card_data = &(struct mtk_platform_card_data) {
		.card = &mt6739_mt6357_card,
	},
	.soc_probe = mt6739_mt6357_soc_probe,
};

static const struct of_device_id mt6739_mt6357_dt_match[] = {
	{ .compatible = "mediatek,mt6739-mt6357-sound", .data = &mt6739_mt6357_pdata },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6739_mt6357_dt_match);

static struct platform_driver mt6739_mt6357_driver = {
	.driver = {
		.name = "mt6739-mt6357",
		.of_match_table = mt6739_mt6357_dt_match,
		.pm = &snd_soc_pm_ops,
	},
	.probe = mtk_soundcard_common_probe,
};
module_platform_driver(mt6739_mt6357_driver);

MODULE_DESCRIPTION("MT6739 MT6357 ALSA SoC machine driver");
MODULE_LICENSE("GPL");
