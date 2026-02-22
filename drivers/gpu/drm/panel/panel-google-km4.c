// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026
 *
 * Based on panel-samsung-s6e3fc2x01.c and downstream Google KM4 panel data.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

struct google_km4 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct drm_dsc_config dsc;
	struct regulator_bulk_data *supplies;
	struct gpio_desc *reset_gpio;
};

static const struct regulator_bulk_data google_km4_supplies[] = {
	{ .supply = "vddio" },
	{ .supply = "vci" },
	{ .supply = "poc" },
};

static inline struct google_km4 *to_google_km4(struct drm_panel *panel)
{
	return container_of(panel, struct google_km4, panel);
}

#define km4_test_key_on_lvl2(ctx) \
	mipi_dsi_dcs_write_seq_multi(ctx, 0xf0, 0x5a, 0x5a)
#define km4_test_key_off_lvl2(ctx) \
	mipi_dsi_dcs_write_seq_multi(ctx, 0xf0, 0xa5, 0xa5)

/* DSCv1.2a 1344x2992 */
static const struct drm_dsc_config google_km4_wqhd_pps_config = {
	.line_buf_depth = 9,
	.bits_per_component = 8,
	.convert_rgb = true,
	.slice_width = 672,
	.slice_height = 34,
	.slice_count = 2,
	.simple_422 = false,
	.pic_width = 1344,
	.pic_height = 2992,
	.rc_tgt_offset_high = 3,
	.rc_tgt_offset_low = 3,
	.bits_per_pixel = 128,
	.rc_edge_factor = 6,
	.rc_quant_incr_limit1 = 11,
	.rc_quant_incr_limit0 = 11,
	.initial_xmit_delay = 512,
	.initial_dec_delay = 592,
	.block_pred_enable = true,
	.first_line_bpg_offset = 12,
	.initial_offset = 6144,
	.rc_buf_thresh = {
		14, 28, 42, 56,
		70, 84, 98, 105,
		112, 119, 121, 123,
		125, 126
	},
	.rc_range_params = {
		{ .range_min_qp = 0, .range_max_qp = 4, .range_bpg_offset = 2 },
		{ .range_min_qp = 0, .range_max_qp = 4, .range_bpg_offset = 0 },
		{ .range_min_qp = 1, .range_max_qp = 5, .range_bpg_offset = 0 },
		{ .range_min_qp = 1, .range_max_qp = 6, .range_bpg_offset = 62 },
		{ .range_min_qp = 3, .range_max_qp = 7, .range_bpg_offset = 60 },
		{ .range_min_qp = 3, .range_max_qp = 7, .range_bpg_offset = 58 },
		{ .range_min_qp = 3, .range_max_qp = 7, .range_bpg_offset = 56 },
		{ .range_min_qp = 3, .range_max_qp = 8, .range_bpg_offset = 56 },
		{ .range_min_qp = 3, .range_max_qp = 9, .range_bpg_offset = 56 },
		{ .range_min_qp = 3, .range_max_qp = 10, .range_bpg_offset = 54 },
		{ .range_min_qp = 5, .range_max_qp = 11, .range_bpg_offset = 54 },
		{ .range_min_qp = 5, .range_max_qp = 12, .range_bpg_offset = 52 },
		{ .range_min_qp = 5, .range_max_qp = 13, .range_bpg_offset = 52 },
		{ .range_min_qp = 7, .range_max_qp = 13, .range_bpg_offset = 52 },
		{ .range_min_qp = 13, .range_max_qp = 15, .range_bpg_offset = 52 }
	},
	.rc_model_size = 8192,
	.flatness_min_qp = 3,
	.flatness_max_qp = 12,
	.initial_scale_value = 32,
	.scale_decrement_interval = 9,
	.scale_increment_interval = 932,
	.nfl_bpg_offset = 745,
	.slice_bpg_offset = 616,
	.final_offset = 4336,
	.vbr_enable = false,
	.slice_chunk_size = 672,
	.dsc_version_minor = 2,
	.dsc_version_major = 1,
	.native_422 = false,
	.native_420 = false,
	.second_line_bpg_offset = 0,
	.nsl_bpg_offset = 0,
	.second_line_offset_adj = 0,
};

static void google_km4_reset(struct google_km4 *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(5000, 6000);
}

static int google_km4_on(struct google_km4 *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };
	struct drm_dsc_picture_parameter_set pps;

	/*
	 * Match downstream sequencing: enable DSC and send PPS before
	 * EXIT_SLEEP_MODE/init command stream.
	 */
	drm_dsc_pps_payload_pack(&pps, &ctx->dsc);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x9d, 0x01);
	mipi_dsi_picture_parameter_set_multi(&dsi_ctx, &pps);
	mipi_dsi_compression_mode_ext_multi(&dsi_ctx, true,
					    MIPI_DSI_COMPRESSION_DSC, 0);

	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	/* Enable TE */
	mipi_dsi_dcs_set_tear_on_multi(&dsi_ctx, MIPI_DSI_DCS_TEAR_MODE_VBLANK);

	km4_test_key_on_lvl2(&dsi_ctx);

	/* FFC: off, 165MHz, MIPI speed 1368 Mbps */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x36, 0xc5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc5,
				     0x10, 0x10, 0x50, 0x05, 0x4d, 0x31, 0x40,
				     0x00, 0x40, 0x00, 0x40, 0x00, 0x4d, 0x31,
				     0x40, 0x00, 0x40, 0x00, 0x40, 0x00, 0x4d,
				     0x31, 0x40, 0x00, 0x40, 0x00, 0x40, 0x00,
				     0x4d, 0x31, 0x40, 0x00, 0x40, 0x00, 0x40,
				     0x00);

	/* Enable OPEC (auto still image detect off) */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x1d, 0x63);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x63, 0x02, 0x18);

	/* PMIC fast discharge off */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x13, 0xb1);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb1, 0x80);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf7, 0x0f);

	km4_test_key_off_lvl2(&dsi_ctx);

	/* Enable brightness control from DCS, no CABC */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_POWER_SAVE, 0x00);

	return dsi_ctx.accum_err;
}

static int google_km4_enable(struct drm_panel *panel)
{
	struct google_km4 *ctx = to_google_km4(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);

	return dsi_ctx.accum_err;
}

static int google_km4_disable(struct drm_panel *panel)
{
	struct google_km4 *ctx = to_google_km4(panel);
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

	return 0;
}

static int google_km4_unprepare(struct drm_panel *panel)
{
	struct google_km4 *ctx = to_google_km4(panel);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(google_km4_supplies), ctx->supplies);

	return 0;
}

static const struct drm_display_mode google_km4_mode = {
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

static int google_km4_get_modes(struct drm_panel *panel,
				struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &google_km4_mode);
}

static const struct drm_panel_funcs google_km4_panel_funcs = {
	.prepare = google_km4_prepare,
	.enable = google_km4_enable,
	.disable = google_km4_disable,
	.unprepare = google_km4_unprepare,
	.get_modes = google_km4_get_modes,
};

static int google_km4_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness = backlight_get_brightness(bl);
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_brightness_large(dsi, brightness);

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return ret;
}

static const struct backlight_ops google_km4_bl_ops = {
	.update_status = google_km4_bl_update_status,
};

static struct backlight_device *google_km4_create_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_PLATFORM,
		.brightness = 1023,
		.max_brightness = 4095,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
					      &google_km4_bl_ops, &props);
}

static int google_km4_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
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
		return dev_err_probe(dev, ret, "Failed to get regulators\n");

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS;

	ctx->panel.prepare_prev_first = true;

	ctx->panel.backlight = google_km4_create_backlight(dsi);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "Failed to create backlight\n");

	/* This panel only supports DSC; unconditionally enable it. */
	ctx->dsc = google_km4_wqhd_pps_config;
	dsi->dsc = &ctx->dsc;

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to attach to DSI host: %d\n", ret);
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	return 0;
}

static void google_km4_remove(struct mipi_dsi_device *dsi)
{
	struct google_km4 *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id google_km4_of_match[] = {
	{ .compatible = "google,gs-km4" },
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

MODULE_AUTHOR("Codex");
MODULE_DESCRIPTION("DRM panel driver for Google KM4");
MODULE_LICENSE("GPL");
