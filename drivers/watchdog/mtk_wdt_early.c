// SPDX-License-Identifier: GPL-2.0
/* Arm the TOPRGU watchdog early, never ping it: mtk_wdt_early=<phys>,<seconds> */

#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/minmax.h>
#include <asm/early_ioremap.h>

#define WDT_MODE		0x00
#define WDT_MODE_EN		BIT(0)
#define WDT_MODE_EXRST_EN	BIT(2)
#define WDT_MODE_IRQ_EN		BIT(3)
#define WDT_MODE_DUAL_EN	BIT(6)
#define WDT_MODE_DDR_RESERVE	BIT(7)
#define WDT_MODE_KEY		0x22000000

#define WDT_LENGTH		0x04
#define WDT_LENGTH_KEY		0x8

#define WDT_RESTART		0x08
#define WDT_RESTART_KEY		0x1971

static int __init mtk_wdt_early_setup(char *arg)
{
	unsigned long long phys;
	unsigned int timeout;
	void __iomem *base;
	char *sep;
	u32 mode;

	sep = strchr(arg ?: "", ',');
	if (!sep)
		return -EINVAL;
	*sep = '\0';
	if (kstrtoull(arg, 0, &phys) || kstrtouint(sep + 1, 0, &timeout))
		return -EINVAL;

	base = (void __iomem *)early_ioremap(phys, 0x20);
	if (!base)
		return -ENOMEM;

	timeout = clamp(timeout, 1U, 31U);
	writel((timeout << 6) << 5 | WDT_LENGTH_KEY, base + WDT_LENGTH);
	writel(WDT_RESTART_KEY, base + WDT_RESTART);

	mode = readl(base + WDT_MODE) & 0xff;
	mode &= ~(WDT_MODE_IRQ_EN | WDT_MODE_DUAL_EN);
	mode |= WDT_MODE_EN | WDT_MODE_EXRST_EN | WDT_MODE_DDR_RESERVE;
	writel(mode | WDT_MODE_KEY, base + WDT_MODE);

	early_iounmap((void *)base, 0x20);
	return 0;
}
early_param("mtk_wdt_early", mtk_wdt_early_setup);
