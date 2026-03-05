// SPDX-License-Identifier: GPL-2.0-only
/*
 * Google KM4 panel driver (ZumaPro Komodo)
 *
 * Downstream-adapted DRM panel implementation for the in-tree Exynos DSIM path.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#define KM4_DEFAULT_BRIGHTNESS 1209
#define KM4_MAX_BRIGHTNESS 4095

struct google_km4 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regulator_bulk_data *supplies;
	struct gpio_desc *reset_gpio;
	struct backlight_device *backlight;
	bool prepared;
	bool enabled;
};

static const struct regulator_bulk_data google_km4_supplies[] = {
	{ .supply = "vddio" },
	{ .supply = "vci" },
	{ .supply = "poc" },
};

static const struct drm_display_mode google_km4_mode_60 = {
	.clock = (1344 + 80 + 24 + 42) * (2992 + 12 + 4 + 22) * 60 / 1000,
	.hdisplay = 1344,
	.hsync_start = 1344 + 80,
	.hsync_end = 1344 + 80 + 24,
	.htotal = 1344 + 80 + 24 + 42,
	.vdisplay = 2992,
	.vsync_start = 2992 + 12,
	.vsync_end = 2992 + 12 + 4,
	.vtotal = 2992 + 12 + 4 + 22,
	.width_mm = 70,
	.height_mm = 156,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static const struct drm_display_mode google_km4_mode_120 = {
	.clock = (1344 + 80 + 24 + 42) * (2992 + 12 + 4 + 22) * 120 / 1000,
	.hdisplay = 1344,
	.hsync_start = 1344 + 80,
	.hsync_end = 1344 + 80 + 24,
	.htotal = 1344 + 80 + 24 + 42,
	.vdisplay = 2992,
	.vsync_start = 2992 + 12,
	.vsync_end = 2992 + 12 + 4,
	.vtotal = 2992 + 12 + 4 + 22,
	.width_mm = 70,
	.height_mm = 156,
	.type = DRM_MODE_TYPE_DRIVER,
};

static inline struct google_km4 *to_google_km4(struct drm_panel *panel)
{
	return container_of(panel, struct google_km4, panel);
}

static void google_km4_reset(struct google_km4 *ctx)
{
	/* Downstream reset timing: 1ms assert, 1ms deassert, 5ms settle */
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(1000, 2000);
	usleep_range(5000, 6000);
}

static int google_km4_set_brightness(struct google_km4 *ctx, u16 brightness)
{
	int ret;

	ret = mipi_dsi_dcs_set_display_brightness(ctx->dsi, brightness);
	if (ret < 0)
		dev_err(&ctx->dsi->dev, "failed to set brightness (%u): %d\n", brightness, ret);

	return ret;
}

static int google_km4_bl_update_status(struct backlight_device *bl)
{
	struct google_km4 *ctx = bl_get_data(bl);
	u16 brightness = backlight_get_brightness(bl);

	if (!ctx->enabled)
		return 0;

	return google_km4_set_brightness(ctx, brightness);
}

static const struct backlight_ops google_km4_bl_ops = {
	.update_status = google_km4_bl_update_status,
};

static int google_km4_on(struct google_km4 *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	/*
	 * Minimal downstream-derived command sequence:
	 * - unlock vendor page
	 * - sleep out
	 * - TE on
	 * - default control display state
	 */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xF0, 0x5A, 0x5A);
	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);
	mipi_dsi_dcs_set_tear_on_multi(&dsi_ctx, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xF0, 0xA5, 0xA5);

	if (dsi_ctx.accum_err < 0)
		return dsi_ctx.accum_err;

	return 0;
}

static int google_km4_off(struct google_km4 *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	return dsi_ctx.accum_err;
}

static int google_km4_prepare(struct drm_panel *panel)
{
	struct google_km4 *ctx = to_google_km4(panel);
	int ret;

	if (ctx->prepared)
		return 0;

	ret = regulator_bulk_enable(ARRAY_SIZE(google_km4_supplies), ctx->supplies);
	if (ret < 0)
		return ret;

	google_km4_reset(ctx);

	ret = google_km4_on(ctx);
	if (ret < 0) {
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		regulator_bulk_disable(ARRAY_SIZE(google_km4_supplies), ctx->supplies);
		return ret;
	}

	ctx->prepared = true;
	return 0;
}

static int google_km4_enable(struct drm_panel *panel)
{
	struct google_km4 *ctx = to_google_km4(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };
	int ret;

	if (ctx->enabled)
		return 0;

	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	ret = dsi_ctx.accum_err;
	if (ret < 0)
		return ret;

	if (ctx->backlight)
		ret = google_km4_set_brightness(ctx, ctx->backlight->props.brightness);
	if (ret < 0)
		return ret;

	ctx->enabled = true;
	return 0;
}

static int google_km4_disable(struct drm_panel *panel)
{
	struct google_km4 *ctx = to_google_km4(panel);

	if (!ctx->enabled)
		return 0;

	google_km4_off(ctx);
	ctx->enabled = false;

	return 0;
}

static int google_km4_unprepare(struct drm_panel *panel)
{
	struct google_km4 *ctx = to_google_km4(panel);

	if (!ctx->prepared)
		return 0;

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(google_km4_supplies), ctx->supplies);

	ctx->prepared = false;
	return 0;
}

static int google_km4_get_modes(struct drm_panel *panel,
				struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &google_km4_mode_60);
	if (!mode)
		return -ENOMEM;
	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	mode = drm_mode_duplicate(connector->dev, &google_km4_mode_120);
	if (!mode)
		return -ENOMEM;
	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = google_km4_mode_60.width_mm;
	connector->display_info.height_mm = google_km4_mode_60.height_mm;

	return 2;
}

static const struct drm_panel_funcs google_km4_panel_funcs = {
	.prepare = google_km4_prepare,
	.enable = google_km4_enable,
	.disable = google_km4_disable,
	.unprepare = google_km4_unprepare,
	.get_modes = google_km4_get_modes,
};

static int google_km4_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct backlight_properties bl_props = { 0 };
	struct google_km4 *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct google_km4, panel,
				   &google_km4_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ret = devm_regulator_bulk_get_const(dev,
					    ARRAY_SIZE(google_km4_supplies),
					    google_km4_supplies,
					    &ctx->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "failed to get reset GPIO\n");

	bl_props.type = BACKLIGHT_RAW;
	bl_props.max_brightness = KM4_MAX_BRIGHTNESS;
	bl_props.brightness = KM4_DEFAULT_BRIGHTNESS;
	ctx->backlight = devm_backlight_device_register(dev, dev_name(dev), dev, ctx,
						&google_km4_bl_ops, &bl_props);
	if (IS_ERR(ctx->backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->backlight),
				     "failed to register backlight\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS;

	ctx->panel.prepare_prev_first = true;
	ctx->panel.backlight = ctx->backlight;

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "failed to attach DSI\n");
	}

	return 0;
}

static void google_km4_remove(struct mipi_dsi_device *dsi)
{
	struct google_km4 *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "failed to detach DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id google_km4_of_match[] = {
	{ .compatible = "google,gs-km4" },
	{ .compatible = "google,km4" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, google_km4_of_match);

static struct mipi_dsi_driver google_km4_driver = {
	.probe = google_km4_probe,
	.remove = google_km4_remove,
	.driver = {
		.name = "panel-google-km4",
		.of_match_table = google_km4_of_match,
	},
};
module_mipi_dsi_driver(google_km4_driver);

MODULE_AUTHOR("OpenAI Codex");
MODULE_DESCRIPTION("DRM panel driver for Google KM4 (ZumaPro Komodo)");
MODULE_LICENSE("GPL");
