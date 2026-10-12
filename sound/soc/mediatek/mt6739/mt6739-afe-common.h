/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _MT6739_AFE_COMMON_H_
#define _MT6739_AFE_COMMON_H_

#include <sound/soc.h>
#include "../common/mtk-base-afe.h"

enum {
	MT6739_MEMIF_DL1,
	MT6739_MEMIF_DL2,
	MT6739_MEMIF_VUL,
	MT6739_MEMIF_AWB,
	MT6739_MEMIF_NUM,
	MT6739_DAI_ADDA = MT6739_MEMIF_NUM,
	MT6739_DAI_NUM,
};

enum {
	MT6739_IRQ_0,
	MT6739_IRQ_1,
	MT6739_IRQ_2,
	MT6739_IRQ_3,
	MT6739_IRQ_4,
	MT6739_IRQ_5,
	MT6739_IRQ_6,
	MT6739_IRQ_7,
	MT6739_IRQ_NUM,
};

/* AFE interconnect inputs */
#define I_ADDA_UL_CH1	3
#define I_ADDA_UL_CH2	4
#define I_DL1_CH1	5
#define I_DL1_CH2	6
#define I_DL2_CH1	7
#define I_DL2_CH2	8

struct clk;

struct mt6739_afe_private {
	struct clk **clk;
	int pm_runtime_bypass_reg_ctl;
};

int mt6739_dai_adda_register(struct mtk_base_afe *afe);

#endif
