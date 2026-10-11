// SPDX-License-Identifier: GPL-2.0-only
/* MT6739 connectivity subsystem (CONSYS) power control. */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kfifo.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/sizes.h>

#define CONSYS_HW_ID		0x008
#define CONSYS_HW_ID_MT6739	0x699
#define CONSYS_ACR		0x110
#define CONSYS_ACR_MBIST	BIT(18)
#define CONSYS_CPUPCR		0x160

#define INFRA_CONN_EMI_REMAP		0x380
#define INFRA_CONN_EMI_REMAP_EN		BIT(13)
#define INFRA_CONN_EMI_REMAP_BASE	GENMASK(12, 0)

#define BTIF_RBR		0x00
#define BTIF_THR		0x00
#define BTIF_IER		0x04
#define BTIF_IER_RXFEN		BIT(0)
#define BTIF_FIFOCTRL		0x08
#define BTIF_FIFOCTRL_CLR_RX	BIT(1)
#define BTIF_FIFOCTRL_CLR_TX	BIT(2)
#define BTIF_FAKELCR		0x0c
#define BTIF_LSR		0x14
#define BTIF_LSR_DR		BIT(0)
#define BTIF_LSR_THRE		BIT(5)
#define BTIF_DMA_EN		0x4c
#define BTIF_DMA_EN_AUTORST	BIT(2)
#define BTIF_TRI_LVL		0x60
#define BTIF_TRI_LVL_TX		GENMASK(3, 0)
#define BTIF_TRI_LVL_RX		GENMASK(6, 4)
#define BTIF_HANDSHAKE		0x6c
#define BTIF_HANDSHAKE_EN	BIT(0)

#define TOPRGU_SWSYSRST		0x018
#define TOPRGU_KEY		0x88000000
#define TOPRGU_CONN_RST		BIT(12)

struct mt6739_consys {
	struct device *dev;
	void __iomem *base;
	void __iomem *rgu;
	struct regulator *vcn18;
	struct regulator *vcn28;
	struct regulator *vcn33_bt;
	struct regmap *infracfg;
	phys_addr_t emi_base;
	void __iomem *btif;
	struct clk *btif_clk;
	DECLARE_KFIFO(rx, u8, 4096);
	spinlock_t rx_lock;
	struct mutex lock;
	bool on;
};

static irqreturn_t mt6739_consys_btif_irq(int irq, void *data)
{
	struct mt6739_consys *cs = data;
	irqreturn_t ret = IRQ_NONE;
	u8 c;

	spin_lock(&cs->rx_lock);
	while (readl(cs->btif + BTIF_LSR) & BTIF_LSR_DR) {
		c = readl(cs->btif + BTIF_RBR);
		kfifo_put(&cs->rx, c);
		ret = IRQ_HANDLED;
	}
	spin_unlock(&cs->rx_lock);
	return ret;
}

static void mt6739_consys_btif_init(struct mt6739_consys *cs)
{
	writel(0, cs->btif + BTIF_FAKELCR);
	writel(BTIF_HANDSHAKE_EN, cs->btif + BTIF_HANDSHAKE);
	writel(BTIF_FIFOCTRL_CLR_RX | BTIF_FIFOCTRL_CLR_TX, cs->btif + BTIF_FIFOCTRL);
	writel(0, cs->btif + BTIF_FIFOCTRL);
	writel(FIELD_PREP(BTIF_TRI_LVL_TX, 8) | FIELD_PREP(BTIF_TRI_LVL_RX, 1),
	       cs->btif + BTIF_TRI_LVL);
	writel(BTIF_DMA_EN_AUTORST, cs->btif + BTIF_DMA_EN);
	writel(BTIF_IER_RXFEN, cs->btif + BTIF_IER);
}

static int mt6739_consys_btif_put(struct mt6739_consys *cs, u8 c)
{
	u32 lsr;
	int ret;

	ret = readl_poll_timeout(cs->btif + BTIF_LSR, lsr, lsr & BTIF_LSR_THRE, 10, 100000);
	if (ret)
		return ret;
	writel(c, cs->btif + BTIF_THR);
	return 0;
}

static void mt6739_consys_cpu_reset(struct mt6739_consys *cs, bool assert)
{
	u32 val = readl(cs->rgu + TOPRGU_SWSYSRST);

	if (assert)
		val |= TOPRGU_CONN_RST;
	else
		val &= ~TOPRGU_CONN_RST;
	writel(val | TOPRGU_KEY, cs->rgu + TOPRGU_SWSYSRST);
}

static int mt6739_consys_power_on(struct mt6739_consys *cs)
{
	u32 id = 0;
	int i, ret;

	ret = regulator_enable(cs->vcn18);
	if (ret)
		return ret;
	ret = regulator_enable(cs->vcn33_bt);
	if (ret)
		goto err_vcn18;
	udelay(150);
	ret = regulator_enable(cs->vcn28);
	if (ret)
		goto err_vcn33;

	mt6739_consys_cpu_reset(cs, true);

	ret = pm_runtime_resume_and_get(cs->dev);
	if (ret)
		goto err_vcn28;

	udelay(10);
	for (i = 0; i < 10; i++) {
		id = readl(cs->base + CONSYS_HW_ID);
		if (id == CONSYS_HW_ID_MT6739)
			break;
		msleep(20);
	}
	dev_info(cs->dev, "hardware ID 0x%x after %d polls\n", id, i);
	if (id != CONSYS_HW_ID_MT6739) {
		ret = -ENODEV;
		goto err_pm;
	}

	ret = clk_prepare_enable(cs->btif_clk);
	if (ret)
		goto err_pm;
	mt6739_consys_btif_init(cs);

	writel(readl(cs->base + CONSYS_ACR) | CONSYS_ACR_MBIST, cs->base + CONSYS_ACR);
	mt6739_consys_cpu_reset(cs, false);
	msleep(20);

	dev_info(cs->dev, "CPU PC 0x%08x\n", readl(cs->base + CONSYS_CPUPCR));
	return 0;

err_pm:
	mt6739_consys_cpu_reset(cs, true);
	pm_runtime_put_sync(cs->dev);
err_vcn28:
	regulator_disable(cs->vcn28);
err_vcn33:
	regulator_disable(cs->vcn33_bt);
err_vcn18:
	regulator_disable(cs->vcn18);
	return ret;
}

static void mt6739_consys_power_off(struct mt6739_consys *cs)
{
	writel(0, cs->btif + BTIF_IER);
	clk_disable_unprepare(cs->btif_clk);
	mt6739_consys_cpu_reset(cs, true);
	pm_runtime_put_sync(cs->dev);
	regulator_disable(cs->vcn28);
	regulator_disable(cs->vcn33_bt);
	regulator_disable(cs->vcn18);
}

static ssize_t mt6739_consys_power_write(struct file *file, const char __user *buf,
					 size_t len, loff_t *ppos)
{
	struct mt6739_consys *cs = file->private_data;
	bool on;
	int ret;

	ret = kstrtobool_from_user(buf, len, &on);
	if (ret)
		return ret;

	mutex_lock(&cs->lock);
	if (on && !cs->on) {
		kfifo_reset(&cs->rx);
		ret = mt6739_consys_power_on(cs);
		cs->on = !ret;
	} else if (!on && cs->on) {
		mt6739_consys_power_off(cs);
		cs->on = false;
	}
	mutex_unlock(&cs->lock);
	return ret ?: len;
}

static const struct file_operations mt6739_consys_power_fops = {
	.open = simple_open,
	.write = mt6739_consys_power_write,
};

static ssize_t mt6739_consys_btif_write(struct file *file, const char __user *buf,
					size_t len, loff_t *ppos)
{
	struct mt6739_consys *cs = file->private_data;
	u8 data[256];
	size_t i, n = min(len, sizeof(data));
	int ret = 0;

	if (copy_from_user(data, buf, n))
		return -EFAULT;

	mutex_lock(&cs->lock);
	if (!cs->on)
		ret = -ENODEV;
	for (i = 0; !ret && i < n; i++)
		ret = mt6739_consys_btif_put(cs, data[i]);
	mutex_unlock(&cs->lock);
	return ret ?: n;
}

static ssize_t mt6739_consys_btif_read(struct file *file, char __user *buf,
				       size_t len, loff_t *ppos)
{
	struct mt6739_consys *cs = file->private_data;
	unsigned int copied;
	int ret;

	ret = kfifo_to_user(&cs->rx, buf, len, &copied);
	return ret ?: copied;
}

static const struct file_operations mt6739_consys_btif_fops = {
	.open = simple_open,
	.read = mt6739_consys_btif_read,
	.write = mt6739_consys_btif_write,
};

static void mt6739_consys_remove(struct platform_device *pdev)
{
	struct mt6739_consys *cs = platform_get_drvdata(pdev);

	debugfs_remove_recursive(debugfs_lookup("mt6739-consys", NULL));
	if (cs->on)
		mt6739_consys_power_off(cs);
}

static int mt6739_consys_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6739_consys *cs;
	struct resource *res, mem;
	struct dentry *dir;
	int irq, ret;

	cs = devm_kzalloc(dev, sizeof(*cs), GFP_KERNEL);
	if (!cs)
		return -ENOMEM;
	cs->dev = dev;
	INIT_KFIFO(cs->rx);
	spin_lock_init(&cs->rx_lock);
	mutex_init(&cs->lock);
	platform_set_drvdata(pdev, cs);

	cs->btif = devm_platform_ioremap_resource_byname(pdev, "btif");
	if (IS_ERR(cs->btif))
		return PTR_ERR(cs->btif);
	cs->btif_clk = devm_clk_get(dev, "btif");
	if (IS_ERR(cs->btif_clk))
		return dev_err_probe(dev, PTR_ERR(cs->btif_clk), "no btif clock\n");
	irq = platform_get_irq_byname(pdev, "btif");
	if (irq < 0)
		return irq;
	ret = devm_request_irq(dev, irq, mt6739_consys_btif_irq, 0, "mt6739-btif", cs);
	if (ret)
		return ret;

	cs->base = devm_platform_ioremap_resource_byname(pdev, "conn");
	if (IS_ERR(cs->base))
		return PTR_ERR(cs->base);

	/* The watchdog driver owns this region; only one reset register is used. */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "toprgu");
	if (!res)
		return -EINVAL;
	cs->rgu = devm_ioremap(dev, res->start, resource_size(res));
	if (!cs->rgu)
		return -ENOMEM;

	cs->vcn18 = devm_regulator_get(dev, "vcn18");
	if (IS_ERR(cs->vcn18))
		return PTR_ERR(cs->vcn18);
	cs->vcn28 = devm_regulator_get(dev, "vcn28");
	if (IS_ERR(cs->vcn28))
		return PTR_ERR(cs->vcn28);
	cs->vcn33_bt = devm_regulator_get(dev, "vcn33-bt");
	if (IS_ERR(cs->vcn33_bt))
		return PTR_ERR(cs->vcn33_bt);

	cs->infracfg = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,infracfg");
	if (IS_ERR(cs->infracfg))
		return dev_err_probe(dev, PTR_ERR(cs->infracfg), "no infracfg\n");

	ret = of_reserved_mem_region_to_resource(dev->of_node, 0, &mem);
	if (ret)
		return dev_err_probe(dev, ret, "no shared memory\n");
	if (resource_size(&mem) != SZ_2M || !IS_ALIGNED(mem.start, SZ_2M))
		return dev_err_probe(dev, -EINVAL, "shared memory must be 2 MiB, 2 MiB aligned\n");
	cs->emi_base = mem.start;
	regmap_update_bits(cs->infracfg, INFRA_CONN_EMI_REMAP,
			   INFRA_CONN_EMI_REMAP_EN | INFRA_CONN_EMI_REMAP_BASE,
			   INFRA_CONN_EMI_REMAP_EN |
			   FIELD_PREP(INFRA_CONN_EMI_REMAP_BASE, cs->emi_base >> 21));
	dev_info(dev, "shared memory at %pa\n", &cs->emi_base);

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	dir = debugfs_create_dir("mt6739-consys", NULL);
	debugfs_create_file("power", 0200, dir, cs, &mt6739_consys_power_fops);
	debugfs_create_file("btif", 0600, dir, cs, &mt6739_consys_btif_fops);

	return 0;
}

static const struct of_device_id mt6739_consys_of_match[] = {
	{ .compatible = "mediatek,mt6739-consys" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6739_consys_of_match);

static struct platform_driver mt6739_consys_driver = {
	.probe = mt6739_consys_probe,
	.remove = mt6739_consys_remove,
	.driver = {
		.name = "mt6739-consys",
		.of_match_table = mt6739_consys_of_match,
	},
};
module_platform_driver(mt6739_consys_driver);

MODULE_DESCRIPTION("MediaTek MT6739 CONSYS power control");
MODULE_LICENSE("GPL");
