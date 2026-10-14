// SPDX-License-Identifier: GPL-2.0-only
/* MT6739 DBI command-mode display path, as left on by the bootloader. */

#include <linux/backlight.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_modes.h>
#include <drm/drm_module.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>

#define OVL_SRC_CON		0x02c
#define OVL_SRC_CON_L3_EN	BIT(3)
#define OVL_L3_CON		0x090
#define OVL_L3_PITCH		0x0a4
#define OVL_L3_ADDR		0xfa0
#define OVL_CON_CLRFMT		GENMASK(15, 12)
#define OVL_CON_CLRFMT_ARGB8888	2
#define OVL_CON_AEN		BIT(8)
#define OVL_CON_BYTE_SWAP	BIT(24)

#define MUTEX0_EN		0x020

#define DBI_STA			0x000
#define DBI_STA_BUSY		BIT(4)
#define DBI_INT_STATUS		0x008
#define DBI_START		0x00c
#define DBI_START_START		BIT(15)
#define DBI_ROI_SIZE		0x06c
#define DBI_ROI_WIDTH		GENMASK(10, 0)
#define DBI_ROI_HEIGHT		GENMASK(26, 16)
#define DBI_SCMD0		0xf80

#define MIPI_DCS_ENTER_SLEEP_MODE	0x10
#define MIPI_DCS_EXIT_SLEEP_MODE	0x11
#define MIPI_DCS_SET_DISPLAY_OFF	0x28
#define MIPI_DCS_SET_DISPLAY_ON		0x29
#define MIPI_DCS_WRITE_MEMORY_START	0x2c

struct mt6739_dbi {
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;
	struct drm_display_mode mode;
	void __iomem *ovl;
	void __iomem *mutex;
	void __iomem *dbi;
	struct backlight_device *backlight;
	bool asleep;
};

static struct mt6739_dbi *to_mt6739_dbi(struct drm_device *drm)
{
	return container_of(drm, struct mt6739_dbi, drm);
}

static void mt6739_dbi_wait_idle(struct mt6739_dbi *dbi)
{
	u32 sta;

	if (readl_poll_timeout(dbi->dbi + DBI_STA, sta, !(sta & DBI_STA_BUSY),
			       100, 100000))
		drm_err_once(&dbi->drm, "DBI stays busy\n");
}

static u32 mt6739_dbi_ovl_con(u32 format)
{
	u32 con = FIELD_PREP(OVL_CON_CLRFMT, OVL_CON_CLRFMT_ARGB8888);

	if (format == DRM_FORMAT_XBGR8888 || format == DRM_FORMAT_ABGR8888)
		con |= OVL_CON_BYTE_SWAP;

	return con;
}

static void mt6739_dbi_flush(struct mt6739_dbi *dbi, struct drm_framebuffer *fb)
{
	struct drm_gem_dma_object *gem = drm_fb_dma_get_gem_obj(fb, 0);
	u32 con;

	mt6739_dbi_wait_idle(dbi);

	con = readl(dbi->ovl + OVL_L3_CON);
	con &= ~(OVL_CON_CLRFMT | OVL_CON_AEN | OVL_CON_BYTE_SWAP);
	con |= mt6739_dbi_ovl_con(fb->format->format);
	writel(con, dbi->ovl + OVL_L3_CON);
	writel(OVL_SRC_CON_L3_EN, dbi->ovl + OVL_SRC_CON);
	writel(fb->pitches[0], dbi->ovl + OVL_L3_PITCH);
	writel(gem->dma_addr + fb->offsets[0], dbi->ovl + OVL_L3_ADDR);

	writel(0, dbi->dbi + DBI_INT_STATUS);
	writel(MIPI_DCS_WRITE_MEMORY_START, dbi->dbi + DBI_SCMD0);
	writel(0, dbi->dbi + DBI_START);
	writel(DBI_START_START, dbi->dbi + DBI_START);
	writel(1, dbi->mutex + MUTEX0_EN);
}

static void mt6739_dbi_enable(struct drm_simple_display_pipe *pipe,
			      struct drm_crtc_state *crtc_state,
			      struct drm_plane_state *plane_state)
{
	struct mt6739_dbi *dbi = to_mt6739_dbi(pipe->crtc.dev);

	if (dbi->asleep) {
		mt6739_dbi_wait_idle(dbi);
		writel(MIPI_DCS_EXIT_SLEEP_MODE, dbi->dbi + DBI_SCMD0);
		msleep(120);
		writel(MIPI_DCS_SET_DISPLAY_ON, dbi->dbi + DBI_SCMD0);
		dbi->asleep = false;
	}

	mt6739_dbi_flush(dbi, plane_state->fb);
	backlight_enable(dbi->backlight);
}

static void mt6739_dbi_disable(struct drm_simple_display_pipe *pipe)
{
	struct mt6739_dbi *dbi = to_mt6739_dbi(pipe->crtc.dev);

	backlight_disable(dbi->backlight);
	mt6739_dbi_wait_idle(dbi);
	writel(MIPI_DCS_SET_DISPLAY_OFF, dbi->dbi + DBI_SCMD0);
	writel(MIPI_DCS_ENTER_SLEEP_MODE, dbi->dbi + DBI_SCMD0);
	dbi->asleep = true;
}

static void mt6739_dbi_update(struct drm_simple_display_pipe *pipe,
			      struct drm_plane_state *old_state)
{
	struct drm_plane_state *state = pipe->plane.state;

	if (!pipe->crtc.state->active || !state->fb)
		return;

	mt6739_dbi_flush(to_mt6739_dbi(pipe->crtc.dev), state->fb);
}

static const struct drm_simple_display_pipe_funcs mt6739_dbi_pipe_funcs = {
	.enable = mt6739_dbi_enable,
	.disable = mt6739_dbi_disable,
	.update = mt6739_dbi_update,
};

static int mt6739_dbi_get_modes(struct drm_connector *connector)
{
	struct mt6739_dbi *dbi = to_mt6739_dbi(connector->dev);

	return drm_connector_helper_get_modes_fixed(connector, &dbi->mode);
}

static const struct drm_connector_helper_funcs mt6739_dbi_connector_helper_funcs = {
	.get_modes = mt6739_dbi_get_modes,
};

static const struct drm_connector_funcs mt6739_dbi_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_mode_config_funcs mt6739_dbi_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static const u32 mt6739_dbi_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_ABGR8888,
};

DEFINE_DRM_GEM_DMA_FOPS(mt6739_dbi_fops);

static const struct drm_driver mt6739_dbi_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.name = "mt6739-dbi",
	.desc = "MediaTek MT6739 DBI display",
	.major = 1,
	.minor = 0,
	.fops = &mt6739_dbi_fops,
	DRM_GEM_DMA_DRIVER_OPS,
	DRM_FBDEV_DMA_DRIVER_OPS,
};

static int mt6739_dbi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6739_dbi *dbi;
	struct drm_device *drm;
	u32 width_mm = 0, height_mm = 0;
	u32 size, width, height;
	int ret;

	dbi = devm_drm_dev_alloc(dev, &mt6739_dbi_driver, struct mt6739_dbi, drm);
	if (IS_ERR(dbi))
		return PTR_ERR(dbi);
	drm = &dbi->drm;

	dbi->ovl = devm_platform_ioremap_resource_byname(pdev, "ovl");
	if (IS_ERR(dbi->ovl))
		return PTR_ERR(dbi->ovl);
	dbi->mutex = devm_platform_ioremap_resource_byname(pdev, "mutex");
	if (IS_ERR(dbi->mutex))
		return PTR_ERR(dbi->mutex);
	dbi->dbi = devm_platform_ioremap_resource_byname(pdev, "dbi");
	if (IS_ERR(dbi->dbi))
		return PTR_ERR(dbi->dbi);

	dbi->backlight = devm_of_find_backlight(dev);
	if (IS_ERR(dbi->backlight))
		return PTR_ERR(dbi->backlight);

	size = readl(dbi->dbi + DBI_ROI_SIZE);
	width = FIELD_GET(DBI_ROI_WIDTH, size);
	height = FIELD_GET(DBI_ROI_HEIGHT, size);
	if (!width || !height)
		return dev_err_probe(dev, -ENODEV, "display not set up by the bootloader\n");

	device_property_read_u32(dev, "width-mm", &width_mm);
	device_property_read_u32(dev, "height-mm", &height_mm);
	dbi->mode = (struct drm_display_mode) {
		DRM_SIMPLE_MODE(width, height, width_mm, height_mm),
	};
	/* Userspace paces frames by the mode's refresh rate; report 60 Hz. */
	dbi->mode.clock = DIV_ROUND_UP(60 * width * height, 1000);

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	drm->mode_config.min_width = width;
	drm->mode_config.max_width = width;
	drm->mode_config.min_height = height;
	drm->mode_config.max_height = height;
	drm->mode_config.preferred_depth = 24;
	drm->mode_config.funcs = &mt6739_dbi_mode_config_funcs;

	drm_connector_helper_add(&dbi->connector, &mt6739_dbi_connector_helper_funcs);
	ret = drm_connector_init(drm, &dbi->connector, &mt6739_dbi_connector_funcs,
				 DRM_MODE_CONNECTOR_DPI);
	if (ret)
		return ret;

	ret = drm_simple_display_pipe_init(drm, &dbi->pipe, &mt6739_dbi_pipe_funcs,
					   mt6739_dbi_formats,
					   ARRAY_SIZE(mt6739_dbi_formats),
					   NULL, &dbi->connector);
	if (ret)
		return ret;

	drm_plane_enable_fb_damage_clips(&dbi->pipe.plane);
	drm_mode_config_reset(drm);
	platform_set_drvdata(pdev, drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	drm_client_setup(drm, NULL);

	return 0;
}

static void mt6739_dbi_remove(struct platform_device *pdev)
{
	struct drm_device *drm = platform_get_drvdata(pdev);

	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);
}

static void mt6739_dbi_shutdown(struct platform_device *pdev)
{
	drm_atomic_helper_shutdown(platform_get_drvdata(pdev));
}

static const struct of_device_id mt6739_dbi_of_match[] = {
	{ .compatible = "mediatek,mt6739-dbi-display" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6739_dbi_of_match);

static struct platform_driver mt6739_dbi_platform_driver = {
	.probe = mt6739_dbi_probe,
	.remove = mt6739_dbi_remove,
	.shutdown = mt6739_dbi_shutdown,
	.driver = {
		.name = "mt6739-dbi",
		.of_match_table = mt6739_dbi_of_match,
	},
};
drm_module_platform_driver(mt6739_dbi_platform_driver);

MODULE_DESCRIPTION("MediaTek MT6739 DBI display driver");
MODULE_LICENSE("GPL");
