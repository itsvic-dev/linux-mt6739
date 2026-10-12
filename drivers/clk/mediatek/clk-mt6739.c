// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2017 MediaTek Inc.

#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "clk-gate.h"
#include "clk-mtk.h"
#include "clk-mux.h"
#include "clk-pll.h"
#include "reset.h"

#include <dt-bindings/clock/mediatek,mt6739-clk.h>

static DEFINE_SPINLOCK(mt6739_clk_lock);

#define CLK_CFG_UPDATE		0x004
#define CLK_CFG_UPDATE1		0x008
#define CLK_CFG_0		0x040
#define CLK_CFG_0_SET		0x044
#define CLK_CFG_0_CLR		0x048
#define CLK_CFG_1		0x050
#define CLK_CFG_2		0x060
#define CLK_CFG_3		0x070
#define CLK_CFG_4		0x080
#define CLK_CFG_6		0x0a0
#define CLK_CFG_7		0x0b0
#define CLK_CFG_8		0x0c0
#define CLK_CFG_8_SET		0x0c4
#define CLK_CFG_8_CLR		0x0c8
#define CLK_CFG_9		0x0d0
#define CLK_CFG_10		0x0e0

/* mainpll children do not forward rate requests: mainpll clocks the AXI bus. */
static const struct mtk_fixed_factor top_divs[] = {
	FACTOR_FLAGS(CLK_TOP_SYSPLL, "syspll_ck", "mainpll", 1, 1, 0),
	FACTOR(CLK_TOP_SYSPLL_D2, "syspll_d2", "syspll_ck", 1, 2),
	FACTOR(CLK_TOP_SYSPLL1_D2, "syspll1_d2", "syspll_d2", 1, 2),
	FACTOR(CLK_TOP_SYSPLL1_D4, "syspll1_d4", "syspll_d2", 1, 4),
	FACTOR(CLK_TOP_SYSPLL1_D8, "syspll1_d8", "syspll_d2", 1, 8),
	FACTOR(CLK_TOP_SYSPLL1_D16, "syspll1_d16", "syspll_d2", 1, 16),
	FACTOR_FLAGS(CLK_TOP_SYSPLL_D3, "syspll_d3", "mainpll", 1, 3, 0),
	FACTOR(CLK_TOP_SYSPLL2_D2, "syspll2_d2", "syspll_d3", 1, 2),
	FACTOR(CLK_TOP_SYSPLL2_D4, "syspll2_d4", "syspll_d3", 1, 4),
	FACTOR_FLAGS(CLK_TOP_SYSPLL_D5, "syspll_d5", "mainpll", 1, 5, 0),
	FACTOR(CLK_TOP_SYSPLL3_D2, "syspll3_d2", "syspll_d5", 1, 2),
	FACTOR(CLK_TOP_SYSPLL3_D4, "syspll3_d4", "syspll_d5", 1, 4),
	FACTOR_FLAGS(CLK_TOP_SYSPLL_D7, "syspll_d7", "mainpll", 1, 7, 0),
	FACTOR(CLK_TOP_SYSPLL4_D2, "syspll4_d2", "syspll_d7", 1, 2),
	FACTOR(CLK_TOP_SYSPLL4_D4, "syspll4_d4", "syspll_d7", 1, 4),
	FACTOR(CLK_TOP_UNIVPLL, "univpll_ck", "univpll", 1, 1),
	FACTOR(CLK_TOP_UNIVPLL_D26, "univpll_d26", "univpll_ck", 1, 26),
	FACTOR(CLK_TOP_UNIVPLL_48M_D2, "univpll_48m_d2", "univpll_d26", 1, 2),
	FACTOR(CLK_TOP_UNIVPLL_48M_D4, "univpll_48m_d4", "univpll_d26", 1, 4),
	FACTOR(CLK_TOP_UNIVPLL_48M_D8, "univpll_48m_d8", "univpll_d26", 1, 8),
	FACTOR(CLK_TOP_UNIVPLL_D2, "univpll_d2", "univpll", 1, 2),
	FACTOR(CLK_TOP_UNIVPLL1_D2, "univpll1_d2", "univpll_d2", 1, 2),
	FACTOR(CLK_TOP_UNIVPLL1_D4, "univpll1_d4", "univpll_d2", 1, 4),
	FACTOR(CLK_TOP_UNIVPLL_D3, "univpll_d3", "univpll", 1, 3),
	FACTOR(CLK_TOP_UNIVPLL2_D2, "univpll2_d2", "univpll_d3", 1, 2),
	FACTOR(CLK_TOP_UNIVPLL2_D4, "univpll2_d4", "univpll_d3", 1, 4),
	FACTOR(CLK_TOP_UNIVPLL2_D8, "univpll2_d8", "univpll_d3", 1, 8),
	FACTOR(CLK_TOP_UNIVPLL2_D32, "univpll2_d32", "univpll_d3", 1, 32),
	FACTOR(CLK_TOP_UNIVPLL_D5, "univpll_d5", "univpll", 1, 5),
	FACTOR(CLK_TOP_UNIVPLL3_D2, "univpll3_d2", "univpll_d5", 1, 2),
	FACTOR(CLK_TOP_UNIVPLL3_D4, "univpll3_d4", "univpll_d5", 1, 4),
	FACTOR(CLK_TOP_UNIVPLL3_D8, "univpll3_d8", "univpll_d5", 1, 8),
	FACTOR(CLK_TOP_MMPLL, "mmpll_ck", "mfgpll", 1, 1),
	FACTOR(CLK_TOP_VENCPLL, "vencpll_ck", "mmpll", 1, 1),
	FACTOR(CLK_TOP_MSDCPLL, "msdcpll_ck", "msdcpll", 1, 1),
	FACTOR(CLK_TOP_MSDCPLL_D2, "msdcpll_d2", "msdcpll", 1, 2),
	FACTOR(CLK_TOP_APLL1, "apll1_ck", "apll1", 1, 1),
	FACTOR(CLK_TOP_APLL1_D2, "apll1_d2", "apll1", 1, 2),
	FACTOR(CLK_TOP_APLL1_D4, "apll1_d4", "apll1", 1, 4),
	FACTOR(CLK_TOP_APLL1_D8, "apll1_d8", "apll1", 1, 8),
};

static const char * const axi_parents[] = {
	"clk26m",
	"syspll_d7",
	"syspll1_d4",
	"syspll3_d2"
};

static const char * const mm_parents[] = {
	"clk26m",
	"vencpll_ck",
	"syspll1_d2",
	"syspll_d5",
	"syspll1_d4",
	"univpll_d5",
	"syspll2_d2",
	"univpll2_d2"
};

static const char * const mfg_parents[] = {
	"clk26m",
	"mmpll_ck",
	"syspll_d3",
	"syspll_d5"
};

static const char * const camtg_parents[] = {
	"clk26m",
	"univpll_48m_d2",
	"univpll2_d8",
	"univpll_d26",
	"univpll2_d32",
	"univpll_48m_d4",
	"univpll_48m_d8"
};

static const char * const uart_parents[] = {
	"clk26m",
	"univpll2_d8"
};

static const char * const spi_parents[] = {
	"clk26m",
	"syspll3_d2",
	"syspll4_d2",
	"syspll2_d4"
};

static const char * const msdc5hclk_parents[] = {
	"clk26m",
	"syspll1_d2",
	"syspll2_d2"
};

static const char * const msdc50_0_parents[] = {
	"clk26m",
	"msdcpll_ck",
	"syspll2_d2",
	"syspll4_d2",
	"univpll1_d2",
	"syspll1_d2",
	"univpll_d5",
	"univpll1_d4"
};

static const char * const msdc30_1_parents[] = {
	"clk26m",
	"msdcpll_d2",
	"univpll2_d2",
	"syspll2_d2",
	"syspll1_d4",
	"univpll1_d4",
	"univpll_d26",
	"syspll2_d4"
};

static const char * const audio_parents[] = {
	"clk26m",
	"syspll3_d4",
	"syspll4_d4",
	"syspll1_d16"
};

static const char * const aud_intbus_parents[] = {
	"clk26m",
	"syspll1_d4",
	"syspll4_d2"
};

static const char * const dbi0_parents[] = {
	"clk26m",
	"univpll3_d2",
	"univpll2_d4",
	"syspll4_d2",
	"univpll2_d8"
};

static const char * const scam_parents[] = {
	"clk26m",
	"syspll3_d2",
	"univpll2_d4"
};

static const char * const aud_1_parents[] = {
	"clk26m",
	"apll1_ck"
};

static const char * const disp_pwm_parents[] = {
	"clk26m",
	"univpll2_d4",
	"univpll2_d8",
	"univpll3_d8"
};

static const char * const nfi2x_parents[] = {
	"clk26m",
	"syspll2_d2",
	"syspll_d7",
	"syspll_d3",
	"syspll2_d4",
	"msdcpll_d2",
	"univpll1_d2",
	"univpll_d5"
};

static const char * const nfiecc_parents[] = {
	"clk26m",
	"syspll4_d2",
	"univpll2_d4",
	"syspll_d7",
	"univpll1_d2",
	"syspll1_d2",
	"univpll2_d2",
	"syspll_d5"
};

static const char * const usb_top_parents[] = {
	"clk26m",
	"univpll3_d4"
};

static const char * const spm_parents[] = {
	"clk26m",
	"syspll1_d8"
};

static const char * const i2c_parents[] = {
	"clk26m",
	"univpll_d26",
	"univpll3_d4"
};

static const char * const senif_parents[] = {
	"clk26m",
	"univpll1_d4",
	"univpll2_d2"
};

static const char * const dxcc_parents[] = {
	"clk26m",
	"syspll1_d2",
	"syspll1_d4",
	"syspll1_d8"
};

static const char * const aud_engen1_parents[] = {
	"clk26m",
	"apll1_d2",
	"apll1_d4",
	"apll1_d8"
};

#define TOP_MUX_CRIT(_id, _name, _parents, _reg, _shift, _width, _gate, \
		     _upd_ofs, _upd)					\
	MUX_GATE_CLR_SET_UPD_FLAGS(_id, _name, _parents, _reg,		\
			(_reg) + 0x4, (_reg) + 0x8, _shift, _width, _gate,	\
			_upd_ofs, _upd,					\
			CLK_IS_CRITICAL | CLK_SET_RATE_PARENT)

#define TOP_MUX(_id, _name, _parents, _reg, _shift, _width, _gate,	\
		_upd_ofs, _upd)						\
	MUX_GATE_CLR_SET_UPD(_id, _name, _parents, _reg, (_reg) + 0x4, \
			(_reg) + 0x8, _shift, _width, _gate, _upd_ofs, _upd)

/* No mem_sel or ddrphycfg_sel: DRAM clocking belongs to DRAMC and SPM. */
static const struct mtk_mux top_muxes[] = {
	GATE_CLR_SET_UPD_FLAGS(CLK_TOP_AXI_SEL, "axi_sel", axi_parents,
			CLK_CFG_0, CLK_CFG_0_SET, CLK_CFG_0_CLR, 0, 2,
			0, 0, -1, CLK_IS_CRITICAL, mtk_mux_clr_set_upd_ops),
	TOP_MUX_CRIT(CLK_TOP_MM_SEL, "mm_sel", mm_parents,
		     CLK_CFG_0, 24, 3, 31, CLK_CFG_UPDATE, 3),
	TOP_MUX(CLK_TOP_MFG_SEL, "mfg_sel", mfg_parents,
		CLK_CFG_1, 24, 2, 31, CLK_CFG_UPDATE, 7),
	TOP_MUX(CLK_TOP_CAMTG_SEL, "camtg_sel", camtg_parents,
		CLK_CFG_2, 0, 3, 7, CLK_CFG_UPDATE, 8),
	TOP_MUX(CLK_TOP_UART_SEL, "uart_sel", uart_parents,
		CLK_CFG_2, 8, 1, 15, CLK_CFG_UPDATE, 9),
	TOP_MUX(CLK_TOP_SPI_SEL, "spi_sel", spi_parents,
		CLK_CFG_2, 16, 2, 23, CLK_CFG_UPDATE, 10),
	TOP_MUX(CLK_TOP_MSDC50_0_HCLK_SEL, "msdc5hclk", msdc5hclk_parents,
		CLK_CFG_3, 8, 2, 15, CLK_CFG_UPDATE, 12),
	TOP_MUX(CLK_TOP_MSDC50_0_SEL, "msdc50_0_sel", msdc50_0_parents,
		CLK_CFG_3, 16, 3, 23, CLK_CFG_UPDATE, 13),
	TOP_MUX(CLK_TOP_MSDC30_1_SEL, "msdc30_1_sel", msdc30_1_parents,
		CLK_CFG_3, 24, 3, 31, CLK_CFG_UPDATE, 14),
	TOP_MUX(CLK_TOP_AUDIO_SEL, "audio_sel", audio_parents,
		CLK_CFG_4, 16, 2, 23, CLK_CFG_UPDATE, 17),
	TOP_MUX(CLK_TOP_AUD_INTBUS_SEL, "aud_intbus_sel", aud_intbus_parents,
		CLK_CFG_4, 24, 2, 31, CLK_CFG_UPDATE, 18),
	TOP_MUX(CLK_TOP_DBI0_SEL, "dbi0_sel", dbi0_parents,
		CLK_CFG_6, 0, 3, 7, CLK_CFG_UPDATE, 23),
	TOP_MUX(CLK_TOP_SCAM_SEL, "scam_sel", scam_parents,
		CLK_CFG_6, 8, 2, 15, CLK_CFG_UPDATE, 24),
	TOP_MUX(CLK_TOP_AUD_1_SEL, "aud_1_sel", aud_1_parents,
		CLK_CFG_6, 16, 1, 23, CLK_CFG_UPDATE, 25),
	TOP_MUX(CLK_TOP_DISP_PWM_SEL, "disp_pwm_sel", disp_pwm_parents,
		CLK_CFG_7, 0, 2, 7, CLK_CFG_UPDATE, 27),
	TOP_MUX(CLK_TOP_NFI2X_SEL, "nfi2x_sel", nfi2x_parents,
		CLK_CFG_7, 8, 3, 15, CLK_CFG_UPDATE, 28),
	TOP_MUX(CLK_TOP_NFIECC_SEL, "nfiecc_sel", nfiecc_parents,
		CLK_CFG_7, 16, 3, 23, CLK_CFG_UPDATE, 29),
	TOP_MUX(CLK_TOP_USB_TOP_SEL, "usb_top_sel", usb_top_parents,
		CLK_CFG_7, 24, 1, 31, CLK_CFG_UPDATE, 30),
	TOP_MUX_CRIT(CLK_TOP_SPM_SEL, "spm_sel", spm_parents,
		     CLK_CFG_8, 0, 1, 7, CLK_CFG_UPDATE, 31),
	MUX_CLR_SET_UPD(CLK_TOP_I2C_SEL, "i2c_sel", i2c_parents,
			CLK_CFG_8, CLK_CFG_8_SET, CLK_CFG_8_CLR, 16, 2,
			CLK_CFG_UPDATE1, 1),
	TOP_MUX(CLK_TOP_SENIF_SEL, "senif_sel", senif_parents,
		CLK_CFG_9, 8, 2, 15, CLK_CFG_UPDATE1, 3),
	TOP_MUX_CRIT(CLK_TOP_DXCC_SEL, "dxcc_sel", dxcc_parents,
		     CLK_CFG_9, 16, 2, 23, CLK_CFG_UPDATE1, 4),
	TOP_MUX(CLK_TOP_CAMTG2_SEL, "camtg2_sel", camtg_parents,
		CLK_CFG_9, 24, 3, 31, CLK_CFG_UPDATE1, 9),
	TOP_MUX(CLK_TOP_AUD_ENGEN1_SEL, "aud_engen1_sel", aud_engen1_parents,
		CLK_CFG_10, 0, 2, 7, CLK_CFG_UPDATE1, 5),
};

static const struct mtk_gate_regs infra0_cg_regs = {
	.set_ofs = 0x80,
	.clr_ofs = 0x84,
	.sta_ofs = 0x90,
};

static const struct mtk_gate_regs infra1_cg_regs = {
	.set_ofs = 0x88,
	.clr_ofs = 0x8c,
	.sta_ofs = 0x94,
};

static const struct mtk_gate_regs infra2_cg_regs = {
	.set_ofs = 0xa4,
	.clr_ofs = 0xa8,
	.sta_ofs = 0xac,
};

static const struct mtk_gate_regs infra3_cg_regs = {
	.set_ofs = 0xc0,
	.clr_ofs = 0xc4,
	.sta_ofs = 0xc8,
};

#define GATE_INFRA_FLAGS(_id, _name, _parent, _regs, _shift, _flags)	\
	GATE_MTK_FLAGS(_id, _name, _parent, _regs, _shift,		\
		       &mtk_clk_gate_ops_setclr, _flags)

#define GATE_INFRA0(_id, _name, _parent, _shift)			\
	GATE_INFRA_FLAGS(_id, _name, _parent, &infra0_cg_regs, _shift, 0)
#define GATE_INFRA1(_id, _name, _parent, _shift)			\
	GATE_INFRA_FLAGS(_id, _name, _parent, &infra1_cg_regs, _shift, 0)
#define GATE_INFRA2(_id, _name, _parent, _shift)			\
	GATE_INFRA_FLAGS(_id, _name, _parent, &infra2_cg_regs, _shift, 0)
#define GATE_INFRA3(_id, _name, _parent, _shift)			\
	GATE_INFRA_FLAGS(_id, _name, _parent, &infra3_cg_regs, _shift, 0)

/* The vendor kernel gates all infra clocks at boot except these. */
#define GATE_INFRA0_CRIT(_id, _name, _parent, _shift)			\
	GATE_INFRA_FLAGS(_id, _name, _parent, &infra0_cg_regs, _shift,	\
			 CLK_IS_CRITICAL)
#define GATE_INFRA1_CRIT(_id, _name, _parent, _shift)			\
	GATE_INFRA_FLAGS(_id, _name, _parent, &infra1_cg_regs, _shift,	\
			 CLK_IS_CRITICAL)
#define GATE_INFRA3_CRIT(_id, _name, _parent, _shift)			\
	GATE_INFRA_FLAGS(_id, _name, _parent, &infra3_cg_regs, _shift,	\
			 CLK_IS_CRITICAL)

static const struct mtk_gate infra_clks[] = {
	GATE_INFRA0_CRIT(CLK_INFRA_PMIC_TMR, "infra_pmic_tmr", "clk26m", 0),
	GATE_INFRA0_CRIT(CLK_INFRA_PMIC_AP, "infra_pmic_ap", "clk26m", 1),
	GATE_INFRA0_CRIT(CLK_INFRA_PMIC_MD, "infra_pmic_md", "clk26m", 2),
	GATE_INFRA0_CRIT(CLK_INFRA_PMIC_CONN, "infra_pmic_conn", "clk26m", 3),
	GATE_INFRA0(CLK_INFRA_SEJ, "infra_sej", "clk26m", 5),
	GATE_INFRA0_CRIT(CLK_INFRA_APXGPT, "infra_apxgpt", "axi_sel", 6),
	GATE_INFRA0(CLK_INFRA_ICUSB, "infra_icusb", "axi_sel", 8),
	GATE_INFRA0(CLK_INFRA_GCE, "infra_gce", "axi_sel", 9),
	GATE_INFRA0(CLK_INFRA_THERM, "infra_therm", "axi_sel", 10),
	GATE_INFRA0(CLK_INFRA_I2C0, "infra_i2c0", "i2c_sel", 11),
	GATE_INFRA0(CLK_INFRA_I2C1, "infra_i2c1", "i2c_sel", 12),
	GATE_INFRA0(CLK_INFRA_I2C2, "infra_i2c2", "i2c_sel", 13),
	GATE_INFRA0(CLK_INFRA_I2C3, "infra_i2c3", "i2c_sel", 14),
	GATE_INFRA0(CLK_INFRA_PWM_HCLK, "infra_pwm_hclk", "axi_sel", 15),
	GATE_INFRA0(CLK_INFRA_PWM1, "infra_pwm1", "i2c_sel", 16),
	GATE_INFRA0(CLK_INFRA_PWM2, "infra_pwm2", "i2c_sel", 17),
	GATE_INFRA0(CLK_INFRA_PWM3, "infra_pwm3", "i2c_sel", 18),
	GATE_INFRA0(CLK_INFRA_PWM4, "infra_pwm4", "i2c_sel", 19),
	GATE_INFRA0(CLK_INFRA_PWM5, "infra_pwm5", "i2c_sel", 20),
	GATE_INFRA0(CLK_INFRA_PWM, "infra_pwm", "i2c_sel", 21),
	GATE_INFRA0_CRIT(CLK_INFRA_UART0, "infra_uart0", "uart_sel", 22),
	GATE_INFRA0(CLK_INFRA_UART1, "infra_uart1", "uart_sel", 23),
	GATE_INFRA0(CLK_INFRA_UART2, "infra_uart2", "uart_sel", 24),
	GATE_INFRA0(CLK_INFRA_UART3, "infra_uart3", "uart_sel", 25),
	GATE_INFRA0(CLK_INFRA_GCE_26M, "infra_gce_26m", "axi_sel", 27),
	GATE_INFRA0(CLK_INFRA_CQ_DMA_FPC, "infra_dma", "axi_sel", 28),
	GATE_INFRA0(CLK_INFRA_BTIF, "infra_btif", "axi_sel", 31),
	GATE_INFRA1(CLK_INFRA_SPI0, "infra_spi0", "spi_sel", 1),
	GATE_INFRA1(CLK_INFRA_MSDC0, "infra_msdc0", "axi_sel", 2),
	GATE_INFRA1(CLK_INFRA_MSDC1, "infra_msdc1", "axi_sel", 4),
	GATE_INFRA1(CLK_INFRA_NFIECC_312M, "infra_nfiecc", "nfiecc_sel", 6),
	GATE_INFRA1_CRIT(CLK_INFRA_DVFSRC, "infra_dvfsrc", "clk26m", 7),
	GATE_INFRA1(CLK_INFRA_GCPU, "infra_gcpu", "axi_sel", 8),
	GATE_INFRA1(CLK_INFRA_TRNG, "infra_trng", "axi_sel", 9),
	GATE_INFRA1(CLK_INFRA_AUXADC, "infra_auxadc", "clk26m", 10),
	GATE_INFRA1(CLK_INFRA_CPUM, "infra_cpum", "axi_sel", 11),
	GATE_INFRA1(CLK_INFRA_CCIF1_AP, "infra_ccif1_ap", "axi_sel", 12),
	GATE_INFRA1(CLK_INFRA_CCIF1_MD, "infra_ccif1_md", "axi_sel", 13),
	GATE_INFRA1(CLK_INFRA_AUXADC_MD, "infra_auxadc_md", "clk26m", 14),
	GATE_INFRA1(CLK_INFRA_NFI, "infra_nfi", "nfi2x_sel", 16),
	GATE_INFRA1(CLK_INFRA_NFI_1X, "infra_nfi_1x", "nfi2x_sel", 17),
	GATE_INFRA1(CLK_INFRA_AP_DMA, "infra_ap_dma", "axi_sel", 18),
	GATE_INFRA1(CLK_INFRA_XIU, "infra_xiu", "axi_sel", 19),
	GATE_INFRA1_CRIT(CLK_INFRA_DEVICE_APC, "infra_dapc", "axi_sel", 20),
	GATE_INFRA1(CLK_INFRA_CCIF_AP, "infra_ccif_ap", "axi_sel", 23),
	GATE_INFRA1_CRIT(CLK_INFRA_DEBUGSYS, "infra_debugsys", "axi_sel", 24),
	GATE_INFRA1(CLK_INFRA_AUDIO, "infra_audio", "axi_sel", 25),
	GATE_INFRA1(CLK_INFRA_CCIF_MD, "infra_ccif_md", "axi_sel", 26),
	GATE_INFRA1(CLK_INFRA_DXCC_SEC_CORE, "infra_secore", "dxcc_sel", 27),
	GATE_INFRA1(CLK_INFRA_DXCC_AO, "infra_dxcc_ao", "dxcc_sel", 28),
	GATE_INFRA1_CRIT(CLK_INFRA_DRAMC_F26M, "infra_dramc26", "clk26m", 31),
	GATE_INFRA2(CLK_INFRA_RG_PWM_FBCLK6, "infra_pwmfb", "clk26m", 0),
	GATE_INFRA2(CLK_INFRA_DISP_PWM, "infra_disp_pwm", "disp_pwm_sel", 2),
	GATE_INFRA2(CLK_INFRA_CLDMA_BCLK, "infra_cldmabclk", "axi_sel", 3),
	GATE_INFRA2(CLK_INFRA_AUDIO_26M_BCLK, "infra_audio26m", "clk26m", 4),
	GATE_INFRA2(CLK_INFRA_SPI1, "infra_spi1", "spi_sel", 6),
	GATE_INFRA2(CLK_INFRA_I2C4, "infra_i2c4", "i2c_sel", 7),
	GATE_INFRA2(CLK_INFRA_MODEM_TEMP_SHARE, "infra_mdtemp", "clk26m", 8),
	GATE_INFRA2(CLK_INFRA_SPI2, "infra_spi2", "spi_sel", 9),
	GATE_INFRA2(CLK_INFRA_SPI3, "infra_spi3", "spi_sel", 10),
	GATE_INFRA2(CLK_INFRA_I2C5, "infra_i2c5", "i2c_sel", 18),
	GATE_INFRA2(CLK_INFRA_I2C5_ARBITER, "infra_i2c5a", "i2c_sel", 19),
	GATE_INFRA2(CLK_INFRA_I2C5_IMM, "infra_i2c5_imm", "i2c_sel", 20),
	GATE_INFRA2(CLK_INFRA_I2C1_ARBITER, "infra_i2c1a", "i2c_sel", 21),
	GATE_INFRA2(CLK_INFRA_I2C1_IMM, "infra_i2c1_imm", "i2c_sel", 22),
	GATE_INFRA2(CLK_INFRA_I2C2_ARBITER, "infra_i2c2a", "i2c_sel", 23),
	GATE_INFRA2(CLK_INFRA_I2C2_IMM, "infra_i2c2_imm", "i2c_sel", 24),
	GATE_INFRA2(CLK_INFRA_SPI4, "infra_spi4", "spi_sel", 25),
	GATE_INFRA2(CLK_INFRA_SPI5, "infra_spi5", "spi_sel", 26),
	GATE_INFRA2(CLK_INFRA_CQ_DMA, "infra_cq_dma", "axi_sel", 27),
	GATE_INFRA3(CLK_INFRA_MSDC0_SELF, "infra_msdc0sf", "msdc50_0_sel", 0),
	GATE_INFRA3(CLK_INFRA_MSDC1_SELF, "infra_msdc1sf", "msdc50_0_sel", 1),
	GATE_INFRA3(CLK_INFRA_MSDC2_SELF, "infra_msdc2sf", "msdc50_0_sel", 2),
	GATE_INFRA3(CLK_INFRA_I2C6, "infra_i2c6", "i2c_sel", 6),
	GATE_INFRA3(CLK_INFRA_AP_MSDC0, "infra_ap_msdc0", "msdc50_0_sel", 7),
	GATE_INFRA3(CLK_INFRA_MD_MSDC0, "infra_md_msdc0", "msdc50_0_sel", 8),
	GATE_INFRA3_CRIT(CLK_INFRA_MSDC0_SRC, "infra_msdc0_clk", "msdc50_0_sel", 31),
	GATE_INFRA3(CLK_INFRA_MSDC1_SRC, "infra_msdc1_clk", "msdc30_1_sel", 10),
	GATE_INFRA3(CLK_INFRA_MSDC2_SRC, "infra_msdc2_clk", "msdc50_0_sel", 11),
};

#define MT6739_PLL_FMAX		(3000UL * MHZ)
#define MT6739_PLL_FMIN		(1000UL * MHZ)

#define CON0_MT6739_RST_BAR	BIT(24)

#define PLL(_id, _name, _reg, _pwr_reg, _en_mask, _flags, _pcwbits,	\
	    _pcwibits, _pd_reg, _pd_shift, _pcw_reg, _pcw_shift,	\
	    _pcw_chg_reg) {						\
		.id = _id,						\
		.name = _name,						\
		.reg = _reg,						\
		.pwr_reg = _pwr_reg,					\
		.en_mask = _en_mask,					\
		.flags = _flags,					\
		.rst_bar_mask = CON0_MT6739_RST_BAR,			\
		.fmax = MT6739_PLL_FMAX,				\
		.fmin = MT6739_PLL_FMIN,				\
		.pcwbits = _pcwbits,					\
		.pcwibits = _pcwibits,					\
		.pd_reg = _pd_reg,					\
		.pd_shift = _pd_shift,					\
		.pcw_reg = _pcw_reg,					\
		.pcw_shift = _pcw_shift,				\
		.pcw_chg_reg = _pcw_chg_reg,				\
	}

static const struct mtk_pll_data plls[] = {
	PLL(CLK_APMIXED_ARMPLL_LL, "armpll_ll", 0x0200, 0x020c, 0x00000001,
	    PLL_AO, 22, 8, 0x0204, 24, 0x0204, 0, 0),
	PLL(CLK_APMIXED_MAINPLL, "mainpll", 0x0220, 0x022c, 0x80000111,
	    HAVE_RST_BAR | PLL_AO, 21, 7, 0x0224, 24, 0x0224, 0, 0),
	PLL(CLK_APMIXED_MFGPLL, "mfgpll", 0x0240, 0x024c, 0x40000111,
	    0, 21, 7, 0x0244, 24, 0x0244, 0, 0),
	PLL(CLK_APMIXED_MMPLL, "mmpll", 0x0270, 0x027c, 0x40000111,
	    0, 21, 7, 0x0274, 24, 0x0274, 0, 0),
	PLL(CLK_APMIXED_UNIVPLL, "univpll", 0x0230, 0x023c, 0xf0000111,
	    HAVE_RST_BAR, 21, 7, 0x0234, 24, 0x0234, 0, 0),
	PLL(CLK_APMIXED_MSDCPLL, "msdcpll", 0x0250, 0x025c, 0x40000111,
	    0, 21, 7, 0x0254, 24, 0x0254, 0, 0),
	PLL(CLK_APMIXED_APLL1, "apll1", 0x02a0, 0x02b0, 0x40000111,
	    0, 32, 8, 0x02a0, 1, 0x02a4, 0, 0x02a0),
};

static int clk_mt6739_apmixed_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct clk_hw_onecell_data *clk_data;
	int r;

	clk_data = mtk_alloc_clk_data(CLK_APMIXED_NR_CLK);
	if (!clk_data)
		return -ENOMEM;

	r = mtk_clk_register_plls(&pdev->dev, plls, ARRAY_SIZE(plls), clk_data);
	if (r)
		goto free_data;

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (r)
		goto unregister_plls;

	return 0;

unregister_plls:
	mtk_clk_unregister_plls(plls, ARRAY_SIZE(plls), clk_data);
free_data:
	mtk_free_clk_data(clk_data);
	return r;
}

static int clk_mt6739_top_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct clk_hw_onecell_data *clk_data;
	int r;

	clk_data = mtk_alloc_clk_data(CLK_TOP_NR_CLK);
	if (!clk_data)
		return -ENOMEM;

	r = mtk_clk_register_factors(top_divs, ARRAY_SIZE(top_divs), clk_data);
	if (r)
		goto free_data;

	r = mtk_clk_register_muxes(&pdev->dev, top_muxes,
				   ARRAY_SIZE(top_muxes), node,
				   &mt6739_clk_lock, clk_data);
	if (r)
		goto unregister_factors;

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (r)
		goto unregister_muxes;

	return 0;

unregister_muxes:
	mtk_clk_unregister_muxes(top_muxes, ARRAY_SIZE(top_muxes), clk_data);
unregister_factors:
	mtk_clk_unregister_factors(top_divs, ARRAY_SIZE(top_divs), clk_data);
free_data:
	mtk_free_clk_data(clk_data);
	return r;
}

static const char * const mcu_parents[] = {
	"clk26m",
	"armpll_ll",
};

static int clk_mt6739_top_ao_probe(struct platform_device *pdev)
{
	struct clk_hw_onecell_data *clk_data;
	void __iomem *base;
	struct clk_hw *hw;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	clk_data = devm_kzalloc(&pdev->dev, struct_size(clk_data, hws, CLK_TOP_AO_NR_CLK),
				GFP_KERNEL);
	if (!clk_data)
		return -ENOMEM;
	clk_data->num = CLK_TOP_AO_NR_CLK;

	hw = devm_clk_hw_register_mux(&pdev->dev, "mcu_sel", mcu_parents,
				      ARRAY_SIZE(mcu_parents),
				      CLK_IS_CRITICAL | CLK_SET_RATE_PARENT,
				      base, 8, 2, 0, &mt6739_clk_lock);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	clk_data->hws[CLK_TOP_AO_MCU_SEL] = hw;

	return devm_of_clk_add_hw_provider(&pdev->dev, of_clk_hw_onecell_get, clk_data);
}

static u16 infra_rst_ofs[] = { 0x140 };

static const struct mtk_clk_rst_desc infra_rst_desc = {
	.version = MTK_RST_SET_CLR,
	.rst_bank_ofs = infra_rst_ofs,
	.rst_bank_nr = ARRAY_SIZE(infra_rst_ofs),
};

static int clk_mt6739_infra_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct clk_hw_onecell_data *clk_data;
	int r;

	clk_data = mtk_alloc_clk_data(CLK_INFRA_NR_CLK);
	if (!clk_data)
		return -ENOMEM;

	r = mtk_clk_register_gates(&pdev->dev, node, infra_clks,
				   ARRAY_SIZE(infra_clks), clk_data);
	if (r)
		goto free_data;

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (r)
		goto unregister_gates;

	r = mtk_register_reset_controller_with_dev(&pdev->dev, &infra_rst_desc);
	if (r)
		goto del_provider;

	return 0;

del_provider:
	of_clk_del_provider(node);
unregister_gates:
	mtk_clk_unregister_gates(infra_clks, ARRAY_SIZE(infra_clks), clk_data);
free_data:
	mtk_free_clk_data(clk_data);
	return r;
}

static const struct of_device_id of_match_clk_mt6739[] = {
	{
		.compatible = "mediatek,mt6739-apmixedsys",
		.data = clk_mt6739_apmixed_probe,
	}, {
		.compatible = "mediatek,mt6739-topckgen",
		.data = clk_mt6739_top_probe,
	}, {
		.compatible = "mediatek,mt6739-topckgen-ao",
		.data = clk_mt6739_top_ao_probe,
	}, {
		.compatible = "mediatek,mt6739-infracfg",
		.data = clk_mt6739_infra_probe,
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6739);

static int clk_mt6739_probe(struct platform_device *pdev)
{
	int (*clk_probe)(struct platform_device *pdev);
	int r;

	clk_probe = of_device_get_match_data(&pdev->dev);
	if (!clk_probe)
		return -EINVAL;

	r = clk_probe(pdev);
	if (r)
		dev_err(&pdev->dev, "could not register clock provider: %d\n", r);

	return r;
}

static struct platform_driver clk_mt6739_drv = {
	.probe = clk_mt6739_probe,
	.driver = {
		.name = "clk-mt6739",
		.of_match_table = of_match_clk_mt6739,
		.suppress_bind_attrs = true,
	},
};

static int __init clk_mt6739_init(void)
{
	return platform_driver_register(&clk_mt6739_drv);
}
arch_initcall(clk_mt6739_init);

MODULE_DESCRIPTION("MediaTek MT6739 main clocks driver");
MODULE_LICENSE("GPL");
