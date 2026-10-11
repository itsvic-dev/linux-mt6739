/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __SOC_MEDIATEK_MT6739_PM_DOMAINS_H
#define __SOC_MEDIATEK_MT6739_PM_DOMAINS_H

#include "mtk-pm-domains.h"
#include <dt-bindings/power/mediatek,mt6739-power-controller.h>

#define MT6739_SPM_CONN_PWR_CON		0x032c
#define MT6739_SPM_PWR_STATUS		0x0180
#define MT6739_SPM_PWR_STATUS_2ND	0x0184

#define MT6739_INFRA_TOPAXI_PROTECTEN_SET	0x02a0
#define MT6739_INFRA_TOPAXI_PROTECTEN_CLR	0x02a4
#define MT6739_INFRA_TOPAXI_PROTECTEN_STA1	0x0228
#define MT6739_TOP_AXI_PROT_EN_CONN		(BIT(13) | BIT(14))

static const struct scpsys_domain_data scpsys_domain_data_mt6739[] = {
	[MT6739_POWER_DOMAIN_CONN] = {
		.name = "conn",
		.sta_mask = BIT(1),
		.ctl_offs = MT6739_SPM_CONN_PWR_CON,
		.pwr_sta_offs = MT6739_SPM_PWR_STATUS,
		.pwr_sta2nd_offs = MT6739_SPM_PWR_STATUS_2ND,
		.caps = MTK_SCPD_NO_STAY_ON,
		.bp_cfg = {
			BUS_PROT_WR(INFRA, MT6739_TOP_AXI_PROT_EN_CONN,
				    MT6739_INFRA_TOPAXI_PROTECTEN_SET,
				    MT6739_INFRA_TOPAXI_PROTECTEN_CLR,
				    MT6739_INFRA_TOPAXI_PROTECTEN_STA1),
		},
	},
};

static const struct scpsys_soc_data mt6739_scpsys_data = {
	.domains_data = scpsys_domain_data_mt6739,
	.num_domains = ARRAY_SIZE(scpsys_domain_data_mt6739),
};

#endif
