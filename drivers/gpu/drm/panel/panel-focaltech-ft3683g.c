// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 Arseniy Velikanov <me@adomerle.pw>
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct ft3683g_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct drm_dsc_config dsc;
	struct regulator_bulk_data supplies[3];
	struct gpio_desc *reset_gpio;
};

static inline struct ft3683g_panel *to_ft3683g_panel(struct drm_panel *panel)
{
	return container_of(panel, struct ft3683g_panel, panel);
}

static void ft3683g_reset(struct ft3683g_panel *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(10000, 11000);
}

static void ft3683g_emerald_init_sequence(struct mipi_dsi_multi_context *dsi_ctx)
{
	dsi_ctx->dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0x03, 0x01);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x28);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0x6f, 0x01);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, MIPI_DCS_SET_TEAR_ON, 0x00);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, MIPI_DCS_WRITE_POWER_SAVE, 0x10);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0x59, 0x09);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0x6c, 0x02);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0x6d, 0x00);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0x70, 0x11, 0x00, 0x00, 0x89, 0x30, 0x80, 0x09,
					0x60, 0x04, 0x38, 0x00, 0x0c, 0x02, 0x1c, 0x02, 0x1c,
					0x02, 0x00, 0x02, 0x0e, 0x00, 0x20, 0x01, 0x1f, 0x00,
					0x07, 0x00, 0x0c, 0x08, 0xbb, 0x08, 0x7a, 0x18, 0x00,
					0x10, 0xf0, 0x03, 0x0c, 0x20, 0x00, 0x06, 0x0b, 0x0b,
					0x33, 0x0e, 0x1c, 0x2a, 0x38, 0x46, 0x54, 0x62, 0x69,
					0x70, 0x77, 0x79, 0x7b, 0x7d, 0x7e, 0x01, 0x02, 0x01,
					0x00, 0x09, 0x40, 0x09, 0xbe, 0x19, 0xfc, 0x19, 0xfa,
					0x19, 0xf8, 0x1a, 0x38, 0x1a, 0x78, 0x1a, 0xb6, 0x2a,
					0xf6, 0x2b, 0x34, 0x2b, 0x74, 0x3b, 0x74, 0x6b, 0xf4,
					0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0xf0, 0xaa, 0x10);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0xd0, 0x84, 0x25, 0x50, 0x14, 0x14, 0x00, 0x39, 
					0x0d, 0x16, 0x19, 0x00, 0x00, 0x0d, 0x35, 0x19, 0x00,
					0x00, 0x0b, 0x05, 0x05, 0x16, 0x16);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0x65, 0x04);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0xcf, 0x7f);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0xf0, 0xaa, 0x13);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0xc6, 0x01);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0xc8, 0x04);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0xd0, 0x04);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0xe0, 0x04);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0xff, 0x5a, 0x80);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0x65, 0x0e);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0xf9, 0xb9);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0x65, 0x0a);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0xf9, 0xa8);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0xf0, 0xaa, 0x11);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0xd5, 0x73);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0xf0, 0xaa, 0x18);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0xb0, 0x13);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0xb2, 0x13);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0xff, 0x5a, 0x81);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0x65, 0x08);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0xf6, 0x51, 0x44);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0x65, 0x03);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0xf4, 0x03);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0xff, 0x5a, 0x00);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0xf0, 0xaa, 0x00);

	mipi_dsi_dcs_exit_sleep_mode_multi(dsi_ctx);
	mipi_dsi_msleep(dsi_ctx, 120);
	mipi_dsi_dcs_set_display_on_multi(dsi_ctx);
}

static int ft3683g_disable(struct drm_panel *panel)
{
	struct ft3683g_panel *ctx = to_ft3683g_panel(panel);
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);

	mipi_dsi_msleep(&dsi_ctx, 50);

	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);

	mipi_dsi_msleep(&dsi_ctx, 100);

	return dsi_ctx.accum_err;
}

static int ft3683g_prepare(struct drm_panel *panel)
{
	struct ft3683g_panel *ctx = to_ft3683g_panel(panel);
	struct drm_dsc_picture_parameter_set pps;
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	dsi_ctx.accum_err = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (dsi_ctx.accum_err)
		return dsi_ctx.accum_err;

	ft3683g_reset(ctx);

	ft3683g_emerald_init_sequence(&dsi_ctx);

	drm_dsc_pps_payload_pack(&pps, &ctx->dsc);

	mipi_dsi_picture_parameter_set_multi(&dsi_ctx, &pps);
	mipi_dsi_compression_mode_multi(&dsi_ctx, true);
	mipi_dsi_msleep(&dsi_ctx, 28);

	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xfe, 0x40);

	if (dsi_ctx.accum_err) {
		gpiod_set_value_cansleep(ctx->reset_gpio, 0);
		regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	}

	return dsi_ctx.accum_err;
}

static int ft3683g_unprepare(struct drm_panel *panel)
{
	struct ft3683g_panel *ctx = to_ft3683g_panel(panel);

	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);

	return 0;
}

static const struct drm_display_mode emerald_mode = {
	.clock = (1080 + 116 + 8 + 16) * (2400 + 20 + 4 + 8) * 120 / 1000,
	.hdisplay = 1080,
	.hsync_start = 1080 + 116,
	.hsync_end = 1080 + 116 + 8,
	.htotal = 1080 + 116 + 8 + 16,
	.vdisplay = 2400,
	.vsync_start = 2400 + 20,
	.vsync_end = 2400 + 20 + 4,
	.vtotal = 2400 + 20 + 4 + 8,
	.width_mm = 70,
	.height_mm = 155,
};

static int ft3683g_get_modes(struct drm_panel *panel,
					struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &emerald_mode);
	if (!mode)
		return -ENOMEM;

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs ft3683g_panel_funcs = {
	.prepare = ft3683g_prepare,
	.unprepare = ft3683g_unprepare,
	.disable = ft3683g_disable,
	.get_modes = ft3683g_get_modes,
};

static int ft3683g_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness = backlight_get_brightness(bl);
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_brightness_large(dsi, brightness);
	if (ret < 0)
		return ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return 0;
}

static int ft3683g_bl_get_brightness(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness;
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_get_display_brightness_large(dsi, &brightness);
	if (ret < 0)
		return ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return brightness;
}

static const struct backlight_ops ft3683g_bl_ops = {
	.update_status = ft3683g_bl_update_status,
	.get_brightness = ft3683g_bl_get_brightness,
};

static struct backlight_device *
ft3683g_create_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 4095,
		.max_brightness = 4095,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
					      &ft3683g_bl_ops, &props);
}

static int ft3683g_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct ft3683g_panel *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct ft3683g_panel, panel,
				   &ft3683g_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->supplies[0].supply = "vdd";
	ctx->supplies[1].supply = "vddi";
	ctx->supplies[2].supply = "vci";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ctx->supplies),
				      ctx->supplies);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to get regulators\n");

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE
			 | MIPI_DSI_MODE_LPM | MIPI_DSI_MODE_NO_EOT_PACKET
			 | MIPI_DSI_CLOCK_NON_CONTINUOUS;

	ctx->panel.prepare_prev_first = true;

	ctx->panel.backlight = ft3683g_create_backlight(dsi);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "Failed to create backlight\n");

	drm_panel_add(&ctx->panel);

	/* This panel only supports DSC; unconditionally enable it */
	dsi->dsc = &ctx->dsc;

	ctx->dsc.dsc_version_major = 1;
	ctx->dsc.dsc_version_minor = 1;
	ctx->dsc.slice_height = 12;
	ctx->dsc.slice_width = 540;

	ctx->dsc.slice_count = 540 / ctx->dsc.slice_width;
	ctx->dsc.bits_per_component = 8;
	ctx->dsc.bits_per_pixel = 8;
	ctx->dsc.block_pred_enable = true;

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to attach to DSI host: %d\n", ret);
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	return 0;
}

static void ft3683g_remove(struct mipi_dsi_device *dsi)
{
	struct ft3683g_panel *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id ft3683g_of_match[] = {
	{ .compatible = "xiaomi,emerald-ft3683g-csot" },
	{ }
};
MODULE_DEVICE_TABLE(of, ft3683g_of_match);

static struct mipi_dsi_driver ft3683g_driver = {
	.probe = ft3683g_probe,
	.remove = ft3683g_remove,
	.driver = {
		.name = "panel-ft3683g",
		.of_match_table = ft3683g_of_match,
	},
};
module_mipi_dsi_driver(ft3683g_driver);

MODULE_DESCRIPTION("DRM driver for ft3683g-equipped DSI panels");
MODULE_LICENSE("GPL");
