// SPDX-License-Identifier: GPL-2.0-or-later
/* drivers/gpu/drm/exynos/exynos9_drm_decon.c
 *
 * Copyright (C) 2014 Samsung Electronics Co.Ltd
 * Authors:
 *	Akshu Agarwal <akshua@gmail.com>
 *	Ajay Kumar <ajaykumar.rs@samsung.com>
 */

#include <linux/clk.h>
#include <linux/component.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include <video/of_display_timing.h>
#include <video/of_videomode.h>

#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_print.h>
#include <drm/drm_vblank.h>
#include <drm/exynos_drm.h>

#include "exynos_drm_crtc.h"
#include "exynos_drm_drv.h"
#include "exynos_drm_fb.h"
#include "exynos_drm_plane.h"
#include "regs-decon9.h"

/*
 * DECON stands for Display and Enhancement controller.
 */

#define MIN_FB_WIDTH_FOR_16WORD_BURST 128

#define WINDOWS_NR	2

struct decon_data {
	unsigned int vidw_buf_start_base;
	unsigned int shadowcon_win_protect_shift;
	unsigned int wincon_burstlen_shift;
	u32 hw_trig_sel;
	bool use_frame_start_irq;
	bool use_dpu_irq_regs;
	bool use_shd_reg_up_req;
	bool command_mode;
	bool sw_trigger;
};

static const struct decon_data exynos9_decon_data = {
	.vidw_buf_start_base = 0x80,
	.shadowcon_win_protect_shift = 10,
	.wincon_burstlen_shift = 11,
};

static const struct decon_data exynos7870_decon_data = {
	.vidw_buf_start_base = 0x880,
	.shadowcon_win_protect_shift = 8,
	.wincon_burstlen_shift = 10,
};

static const struct decon_data zuma_decon_data = {
	.vidw_buf_start_base = 0x880,
	.shadowcon_win_protect_shift = 8,
	.wincon_burstlen_shift = 10,
	.hw_trig_sel = HW_TRIG_SEL_FROM_DDI0,
	.use_frame_start_irq = true,
	.use_dpu_irq_regs = true,
	.use_shd_reg_up_req = true,
	.command_mode = true,
	.sw_trigger = false,
};

struct decon_context {
	struct device			*dev;
	struct drm_device		*drm_dev;
	void				*dma_priv;
	struct exynos_drm_crtc		*crtc;
	struct exynos_drm_plane		planes[WINDOWS_NR];
	struct exynos_drm_plane_config	configs[WINDOWS_NR];
	struct clk			*pclk;
	struct clk			*aclk;
	struct clk			*eclk;
	struct clk			*vclk;
	void __iomem			*regs;
	void __iomem			*win_regs;
	void __iomem			*sub_regs;
	void __iomem			*wincon_regs;
	unsigned long			irq_flags;
	int				irq;
	int				irq_fs;
	int				irq_fd;
	int				irq_ext;
	bool				i80_if;
	wait_queue_head_t		wait_vsync_queue;
	atomic_t			wait_vsync_event;

	const struct decon_data *data;
	struct drm_encoder *encoder;
};

static void decon_te_handler(struct exynos_drm_crtc *crtc);

static inline u32 decon_read(struct decon_context *ctx, u32 reg)
{
	return readl(ctx->regs + reg);
}

static inline void decon_write(struct decon_context *ctx, u32 reg, u32 val)
{
	writel(val, ctx->regs + reg);
}

static inline u32 decon_win_read(struct decon_context *ctx, u32 reg)
{
	return readl(ctx->win_regs + reg);
}

static inline void decon_win_write(struct decon_context *ctx, u32 reg, u32 val)
{
	writel(val, ctx->win_regs + reg);
}

static inline u32 decon_wincon_read(struct decon_context *ctx, u32 reg)
{
	return readl(ctx->wincon_regs + reg);
}

static inline void decon_wincon_write(struct decon_context *ctx, u32 reg, u32 val)
{
	writel(val, ctx->wincon_regs + reg);
}

static int decon_get_irq(struct platform_device *pdev, struct decon_context *ctx)
{
	int ret;

	if (ctx->data->use_dpu_irq_regs)
		return 0;

	if (ctx->data->use_frame_start_irq && !ctx->i80_if) {
		ret = platform_get_irq_byname_optional(pdev, "frame_start");
		if (ret >= 0)
			return ret;
	}

	return platform_get_irq_byname(pdev, ctx->i80_if ? "lcd_sys" : "vsync");
}

static void decon_irq_clear_all(struct decon_context *ctx)
{
	u32 val;

	val = decon_read(ctx, DECON_INT_PEND);
	if (val)
		decon_write(ctx, DECON_INT_PEND, val);

	val = decon_read(ctx, DECON_INT_PEND_EXTRA);
	if (val)
		decon_write(ctx, DECON_INT_PEND_EXTRA, val);
}

static void decon_dpu_set_irqs(struct decon_context *ctx, bool enable)
{
	u32 val;

	decon_irq_clear_all(ctx);

	if (enable) {
		val = INT_EN_FRAME_DONE | INT_EN_FRAME_START |
		      INT_EN_EXTRA | INT_EN;
		decon_write(ctx, DECON_INT_EN, val);
		decon_write(ctx, DECON_INT_EN_EXTRA,
			    INT_EN_RESOURCE_CONFLICT | INT_EN_TIME_OUT);
	} else {
		decon_write(ctx, DECON_INT_EN_EXTRA, 0);
		decon_write(ctx, DECON_INT_EN, 0);
	}
}

static void decon_trigger_shadow_update(struct decon_context *ctx,
					unsigned int win)
{
	u32 mask;
	u32 val;

	if (!ctx->data->use_shd_reg_up_req) {
		val = decon_read(ctx, DECON_UPDATE);
		val |= DECON_UPDATE_STANDALONE_F;
		decon_write(ctx, DECON_UPDATE, val);
		return;
	}

	mask = SHD_REG_UP_REQ_GLOBAL | SHD_REG_UP_REQ_CMP;
	if (win < WINDOWS_NR)
		mask |= SHD_REG_UP_REQ_WIN(win);
	else
		mask |= SHD_REG_UP_REQ_FOR_DECON;

	val = decon_read(ctx, SHD_REG_UP_REQ);
	val |= mask;
	decon_write(ctx, SHD_REG_UP_REQ, val);
}

static void decon_set_operation_mode(struct decon_context *ctx, bool command_mode)
{
	u32 val;

	val = decon_read(ctx, GLOBAL_CON);
	val &= ~GLOBAL_CON_OPERATION_MODE_F;
	val |= command_mode ? GLOBAL_CON_OPERATION_MODE_CMD_F :
			      GLOBAL_CON_OPERATION_MODE_VIDEO_F;
	decon_write(ctx, GLOBAL_CON, val);
}

static void decon_init_trigger(struct decon_context *ctx, bool command_mode,
			       bool sw_trigger)
{
	u32 val;
	u32 mask;

	if (!command_mode)
		val = HW_TRIG_EN;
	else if (sw_trigger)
		val = 0;
	else
		val = HW_TRIG_EN;

	mask = HW_TRIG_EN | HW_TRIG_SEL_MASK | HW_TRIG_MASK_DECON;
	val |= ctx->data->hw_trig_sel;
	val |= HW_TRIG_MASK_DECON;
	decon_write(ctx, TRIG_CON,
		    (decon_read(ctx, TRIG_CON) & ~mask) | (val & mask));
}

static void decon_set_trigger_mask(struct decon_context *ctx, bool masked)
{
	u32 val;

	if (!ctx->data->command_mode)
		return;

	val = decon_read(ctx, TRIG_CON);
	if (masked)
		val |= HW_TRIG_MASK_DECON;
	else
		val &= ~HW_TRIG_MASK_DECON;

	decon_write(ctx, TRIG_CON, val);
}

static void decon_unmask_trigger(struct decon_context *ctx)
{
	if (!ctx->data->command_mode)
		return;

	if (ctx->data->sw_trigger) {
		u32 val = decon_read(ctx, TRIG_CON);

		val |= SW_TRIG_EN | SW_TRIG_DET_EN;
		decon_write(ctx, TRIG_CON, val);
		return;
	}

	decon_set_trigger_mask(ctx, false);
}

static void decon_set_color_map(struct decon_context *ctx, unsigned int win,
				unsigned int width, unsigned int height)
{
	u32 val;

	/* Solid black window as a bring-up fallback until DPP planes are wired. */
	decon_win_write(ctx, WIN_START_POSITION(win),
			WIN_STRPTR_Y_F(0) | WIN_STRPTR_X_F(0));
	decon_win_write(ctx, WIN_END_POSITION(win),
			WIN_ENDPTR_Y_F(height - 1) | WIN_ENDPTR_X_F(width - 1));
	decon_win_write(ctx, WIN_START_TIME_CON(win), 0);

	decon_win_write(ctx, WIN_COLORMAP_0(win),
			WIN_MAPCOLOR_A_F(0xff) | WIN_MAPCOLOR_R_F(0x0));
	decon_win_write(ctx, WIN_COLORMAP_1(win),
			WIN_MAPCOLOR_G_F(0x0) | WIN_MAPCOLOR_B_F(0x0));

	val = decon_wincon_read(ctx, DECON_CON_WIN(win));
	val &= ~WIN_CHMAP_MASK;
	val |= WIN_CHMAP_F(0);
	val |= WIN_MAPCOLOR_EN_F | _WIN_EN_F;
	decon_wincon_write(ctx, DECON_CON_WIN(win), val);
}

static void decon_init_dpu(struct decon_context *ctx)
{
	u32 val;

	decon_write(ctx, GLOBAL_CON, GLOBAL_CON_SRESET);

	val = decon_read(ctx, CLOCK_CON(0));
	val &= ~(CLOCK_CON_AUTO_CG_MASK | CLOCK_CON_QACTIVE_MASK);
	decon_write(ctx, CLOCK_CON(0), val);

	decon_set_operation_mode(ctx, ctx->data->command_mode);
	decon_init_trigger(ctx, ctx->data->command_mode, ctx->data->sw_trigger);
	decon_irq_clear_all(ctx);
}

static void decon_commit_dpu(struct decon_context *ctx,
			     const struct drm_display_mode *mode)
{
	u32 val;
	u32 status;
	int ret;

	decon_set_color_map(ctx, 0, mode->hdisplay, mode->vdisplay);

	decon_write(ctx, DATA_PATH_CON_0, COMP_OUTIF_PATH_F(0x001));

	val = BLENDER_BG_HEIGHT_F(mode->vdisplay) |
	      BLENDER_BG_WIDTH_F(mode->hdisplay);
	decon_write(ctx, BLD_BG_IMG_SIZE_PRI, val);

	val = OUTFIFO_HEIGHT_F(mode->vdisplay) |
	      OUTFIFO_WIDTH_F(mode->hdisplay);
	decon_write(ctx, OF_SIZE_0, val);

	decon_write(ctx, DECON_INT_PEND, INT_PEND_FRAME_START);
	val = decon_read(ctx, GLOBAL_CON) |
	      GLOBAL_CON_DECON_EN | GLOBAL_CON_DECON_EN_F;
	decon_write(ctx, GLOBAL_CON, val);

	decon_trigger_shadow_update(ctx, WINDOWS_NR);

	ret = readl_poll_timeout_atomic(ctx->regs + GLOBAL_CON, status,
					status & GLOBAL_CON_RUN_STATUS,
					10, 2000);
	if (ret)
		dev_warn(ctx->dev, "timed out waiting for DECON run status\n");

	decon_unmask_trigger(ctx);
}

static const struct of_device_id decon_driver_dt_match[] = {
	{
		.compatible = "google,zuma-decon",
		.data = &zuma_decon_data,
	},
	{},
};
MODULE_DEVICE_TABLE(of, decon_driver_dt_match);

static const uint32_t decon_formats[] = {
	DRM_FORMAT_RGB565,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_RGBX8888,
	DRM_FORMAT_BGRX8888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_ABGR8888,
	DRM_FORMAT_RGBA8888,
	DRM_FORMAT_BGRA8888,
};

static const enum drm_plane_type decon_win_types[WINDOWS_NR] = {
	DRM_PLANE_TYPE_PRIMARY,
	DRM_PLANE_TYPE_CURSOR,
};

/**
 * decon_shadow_protect_win() - disable updating values from shadow registers at vsync
 *
 * @ctx: display and enhancement controller context
 * @win: window to protect registers for
 * @protect: 1 to protect (disable updates)
 */
static void decon_shadow_protect_win(struct decon_context *ctx,
				     unsigned int win, bool protect)
{
	u32 bits, val;
	unsigned int shift = ctx->data->shadowcon_win_protect_shift;

	bits = SHADOWCON_WINx_PROTECT(shift, win);

	val = readl(ctx->regs + SHADOWCON);
	if (protect)
		val |= bits;
	else
		val &= ~bits;
	writel(val, ctx->regs + SHADOWCON);
}

static void decon_wait_for_vblank(struct decon_context *ctx)
{
	atomic_set(&ctx->wait_vsync_event, 1);

	/*
	 * wait for DECON to signal VSYNC interrupt or return after
	 * timeout which is set to 50ms (refresh rate of 20).
	 */
	if (!wait_event_timeout(ctx->wait_vsync_queue,
				!atomic_read(&ctx->wait_vsync_event),
				HZ/20))
		DRM_DEV_DEBUG_KMS(ctx->dev, "vblank wait timed out.\n");
}

static void decon_clear_channels(struct decon_context *ctx)
{
	unsigned int win, ch_enabled = 0;
	u32 val;

	if (ctx->data->use_dpu_irq_regs)
		return;

	/* Check if any channel is enabled. */
	for (win = 0; win < WINDOWS_NR; win++) {
		val = readl(ctx->regs + WINCON(win));

		if (val & WINCONx_ENWIN) {
			decon_shadow_protect_win(ctx, win, true);

			val &= ~WINCONx_ENWIN;
			writel(val, ctx->regs + WINCON(win));
			ch_enabled = 1;

			decon_shadow_protect_win(ctx, win, false);
		}
	}

	decon_trigger_shadow_update(ctx, WINDOWS_NR);

	/* Wait for vsync, as disable channel takes effect at next vsync */
	if (ch_enabled)
		decon_wait_for_vblank(ctx);
}

static int decon_ctx_initialize(struct decon_context *ctx,
			struct drm_device *drm_dev)
{
	ctx->drm_dev = drm_dev;

	decon_clear_channels(ctx);

	return exynos_drm_register_dma(drm_dev, ctx->dev, &ctx->dma_priv);
}

static void decon_ctx_remove(struct decon_context *ctx)
{
	/* detach this sub driver from iommu mapping if supported. */
	exynos_drm_unregister_dma(ctx->drm_dev, ctx->dev, &ctx->dma_priv);
}

static u32 decon_calc_clkdiv(struct decon_context *ctx,
		const struct drm_display_mode *mode)
{
	unsigned long ideal_clk = mode->clock * 1000;
	u32 clkdiv;

	/* Find the clock divider value that gets us closest to ideal_clk */
	clkdiv = DIV_ROUND_UP(clk_get_rate(ctx->vclk), ideal_clk);

	return (clkdiv < 0x100) ? clkdiv : 0xff;
}

static void decon_commit(struct exynos_drm_crtc *crtc)
{
	struct decon_context *ctx = crtc->ctx;
	struct drm_display_mode *mode = &crtc->base.state->adjusted_mode;
	u32 val, clkdiv;

	/* nothing to do if we haven't set the mode yet */
	if (mode->htotal == 0 || mode->vtotal == 0)
		return;

	if (ctx->data->use_dpu_irq_regs) {
		decon_commit_dpu(ctx, mode);
		return;
	}

	if (!ctx->i80_if) {
		int vsync_len, vbpd, vfpd, hsync_len, hbpd, hfpd;
	      /* setup vertical timing values. */
		vsync_len = mode->crtc_vsync_end - mode->crtc_vsync_start;
		vbpd = mode->crtc_vtotal - mode->crtc_vsync_end;
		vfpd = mode->crtc_vsync_start - mode->crtc_vdisplay;

		val = VIDTCON0_VBPD(vbpd - 1) | VIDTCON0_VFPD(vfpd - 1);
		writel(val, ctx->regs + VIDTCON0);

		val = VIDTCON1_VSPW(vsync_len - 1);
		writel(val, ctx->regs + VIDTCON1);

		/* setup horizontal timing values.  */
		hsync_len = mode->crtc_hsync_end - mode->crtc_hsync_start;
		hbpd = mode->crtc_htotal - mode->crtc_hsync_end;
		hfpd = mode->crtc_hsync_start - mode->crtc_hdisplay;

		/* setup horizontal timing values.  */
		val = VIDTCON2_HBPD(hbpd - 1) | VIDTCON2_HFPD(hfpd - 1);
		writel(val, ctx->regs + VIDTCON2);

		val = VIDTCON3_HSPW(hsync_len - 1);
		writel(val, ctx->regs + VIDTCON3);
	}

	/* setup horizontal and vertical display size. */
	val = VIDTCON4_LINEVAL(mode->vdisplay - 1) |
	       VIDTCON4_HOZVAL(mode->hdisplay - 1);
	writel(val, ctx->regs + VIDTCON4);

	writel(mode->vdisplay - 1, ctx->regs + LINECNT_OP_THRESHOLD);

	/*
	 * fields of register with prefix '_F' would be updated
	 * at vsync(same as dma start)
	 */
	val = VIDCON0_ENVID | VIDCON0_ENVID_F;
	writel(val, ctx->regs + VIDCON0);

	clkdiv = decon_calc_clkdiv(ctx, mode);
	if (clkdiv > 1) {
		val = VCLKCON1_CLKVAL_NUM_VCLK(clkdiv - 1);
		writel(val, ctx->regs + VCLKCON1);
		writel(val, ctx->regs + VCLKCON2);
	}

	if (ctx->data->use_dpu_irq_regs)
		writel(INT_PEND_FRAME_START, ctx->regs + DECON_INT_PEND);

	decon_trigger_shadow_update(ctx, WINDOWS_NR);
}

static int decon_enable_vblank(struct exynos_drm_crtc *crtc)
{
	struct decon_context *ctx = crtc->ctx;
	u32 val;

	if (!test_and_set_bit(0, &ctx->irq_flags)) {
	if (ctx->data->use_dpu_irq_regs) {
		enable_irq(ctx->irq_fs);
		return 0;
	}

		val = readl(ctx->regs + VIDINTCON0);

		val |= VIDINTCON0_INT_ENABLE;

		if (!ctx->i80_if) {
			val |= VIDINTCON0_INT_FRAME;
			val &= ~VIDINTCON0_FRAMESEL0_MASK;
			val |= VIDINTCON0_FRAMESEL0_VSYNC;
		}

		writel(val, ctx->regs + VIDINTCON0);
	}

	return 0;
}

static void decon_disable_vblank(struct exynos_drm_crtc *crtc)
{
	struct decon_context *ctx = crtc->ctx;
	u32 val;

	if (test_and_clear_bit(0, &ctx->irq_flags)) {
		if (ctx->data->use_dpu_irq_regs) {
			disable_irq_nosync(ctx->irq_fs);
			return;
		}

		val = readl(ctx->regs + VIDINTCON0);

		val &= ~VIDINTCON0_INT_ENABLE;
		if (!ctx->i80_if)
			val &= ~VIDINTCON0_INT_FRAME;

		writel(val, ctx->regs + VIDINTCON0);
	}
}

static void decon_win_set_pixfmt(struct decon_context *ctx, unsigned int win,
				 struct drm_framebuffer *fb)
{
	unsigned long val;
	int padding;
	unsigned int shift = ctx->data->wincon_burstlen_shift;

	val = readl(ctx->regs + WINCON(win));
	val &= ~WINCONx_BPPMODE_MASK;

	switch (fb->format->format) {
	case DRM_FORMAT_RGB565:
		val |= WINCONx_BPPMODE_16BPP_565;
		val |= WINCONx_BURSTLEN_16WORD(shift);
		break;
	case DRM_FORMAT_XRGB8888:
		val |= WINCONx_BPPMODE_24BPP_xRGB;
		val |= WINCONx_BURSTLEN_16WORD(shift);
		break;
	case DRM_FORMAT_XBGR8888:
		val |= WINCONx_BPPMODE_24BPP_xBGR;
		val |= WINCONx_BURSTLEN_16WORD(shift);
		break;
	case DRM_FORMAT_RGBX8888:
		val |= WINCONx_BPPMODE_24BPP_RGBx;
		val |= WINCONx_BURSTLEN_16WORD(shift);
		break;
	case DRM_FORMAT_BGRX8888:
		val |= WINCONx_BPPMODE_24BPP_BGRx;
		val |= WINCONx_BURSTLEN_16WORD(shift);
		break;
	case DRM_FORMAT_ARGB8888:
		val |= WINCONx_BPPMODE_32BPP_ARGB | WINCONx_BLD_PIX |
			WINCONx_ALPHA_SEL;
		val |= WINCONx_BURSTLEN_16WORD(shift);
		break;
	case DRM_FORMAT_ABGR8888:
		val |= WINCONx_BPPMODE_32BPP_ABGR | WINCONx_BLD_PIX |
			WINCONx_ALPHA_SEL;
		val |= WINCONx_BURSTLEN_16WORD(shift);
		break;
	case DRM_FORMAT_RGBA8888:
		val |= WINCONx_BPPMODE_32BPP_RGBA | WINCONx_BLD_PIX |
			WINCONx_ALPHA_SEL;
		val |= WINCONx_BURSTLEN_16WORD(shift);
		break;
	case DRM_FORMAT_BGRA8888:
	default:
		val |= WINCONx_BPPMODE_32BPP_BGRA | WINCONx_BLD_PIX |
			WINCONx_ALPHA_SEL;
		val |= WINCONx_BURSTLEN_16WORD(shift);
		break;
	}

	DRM_DEV_DEBUG_KMS(ctx->dev, "cpp = %d\n", fb->format->cpp[0]);

	/*
	 * In case of exynos, setting dma-burst to 16Word causes permanent
	 * tearing for very small buffers, e.g. cursor buffer. Burst Mode
	 * switching which is based on plane size is not recommended as
	 * plane size varies a lot towards the end of the screen and rapid
	 * movement causes unstable DMA which results into iommu crash/tear.
	 */

	padding = (fb->pitches[0] / fb->format->cpp[0]) - fb->width;
	if (fb->width + padding < MIN_FB_WIDTH_FOR_16WORD_BURST) {
		val &= ~WINCONx_BURSTLEN_MASK(shift);
		val |= WINCONx_BURSTLEN_8WORD(shift);
	}

	writel(val, ctx->regs + WINCON(win));
}

static void decon_win_set_colkey(struct decon_context *ctx, unsigned int win)
{
	unsigned int keycon0 = 0, keycon1 = 0;

	keycon0 = ~(WxKEYCON0_KEYBL_EN | WxKEYCON0_KEYEN_F |
			WxKEYCON0_DIRCON) | WxKEYCON0_COMPKEY(0);

	keycon1 = WxKEYCON1_COLVAL(0xffffffff);

	writel(keycon0, ctx->regs + WKEYCON0_BASE(win));
	writel(keycon1, ctx->regs + WKEYCON1_BASE(win));
}

static void decon_atomic_begin(struct exynos_drm_crtc *crtc)
{
	struct decon_context *ctx = crtc->ctx;
	int i;

	if (ctx->data->use_dpu_irq_regs)
		return;

	for (i = 0; i < WINDOWS_NR; i++)
		decon_shadow_protect_win(ctx, i, true);
}

static void decon_update_plane(struct exynos_drm_crtc *crtc,
			       struct exynos_drm_plane *plane)
{
	struct exynos_drm_plane_state *state =
				to_exynos_plane_state(plane->base.state);
	struct decon_context *ctx = crtc->ctx;
	struct drm_framebuffer *fb = state->base.fb;
	int padding;
	unsigned long val, alpha;
	unsigned int last_x;
	unsigned int last_y;
	unsigned int win = plane->index;
	unsigned int cpp = fb->format->cpp[0];
	unsigned int pitch = fb->pitches[0];
	unsigned int vidw_addr0_base = ctx->data->vidw_buf_start_base;

	if (ctx->data->use_dpu_irq_regs)
		return;

	/*
	 * SHADOWCON/PRTCON register is used for enabling timing.
	 *
	 * for example, once only width value of a register is set,
	 * if the dma is started then decon hardware could malfunction so
	 * with protect window setting, the register fields with prefix '_F'
	 * wouldn't be updated at vsync also but updated once unprotect window
	 * is set.
	 */

	/* buffer start address */
	val = (unsigned long)exynos_drm_fb_dma_addr(fb, 0);
	writel(val, ctx->regs + VIDW_BUF_START(vidw_addr0_base, win));

	padding = (pitch / cpp) - fb->width;

	/* buffer size */
	writel(fb->width + padding, ctx->regs + VIDW_WHOLE_X(win));
	writel(fb->height, ctx->regs + VIDW_WHOLE_Y(win));

	/* offset from the start of the buffer to read */
	writel(state->src.x, ctx->regs + VIDW_OFFSET_X(win));
	writel(state->src.y, ctx->regs + VIDW_OFFSET_Y(win));

	DRM_DEV_DEBUG_KMS(ctx->dev, "start addr = 0x%lx\n",
			(unsigned long)val);
	DRM_DEV_DEBUG_KMS(ctx->dev, "ovl_width = %d, ovl_height = %d\n",
			state->crtc.w, state->crtc.h);

	val = VIDOSDxA_TOPLEFT_X(state->crtc.x) |
		VIDOSDxA_TOPLEFT_Y(state->crtc.y);
	writel(val, ctx->regs + VIDOSD_A(win));

	last_x = state->crtc.x + state->crtc.w;
	if (last_x)
		last_x--;
	last_y = state->crtc.y + state->crtc.h;
	if (last_y)
		last_y--;

	val = VIDOSDxB_BOTRIGHT_X(last_x) | VIDOSDxB_BOTRIGHT_Y(last_y);

	writel(val, ctx->regs + VIDOSD_B(win));

	DRM_DEV_DEBUG_KMS(ctx->dev, "osd pos: tx = %d, ty = %d, bx = %d, by = %d\n",
			state->crtc.x, state->crtc.y, last_x, last_y);

	/* OSD alpha */
	alpha = VIDOSDxC_ALPHA0_R_F(0x0) |
			VIDOSDxC_ALPHA0_G_F(0x0) |
			VIDOSDxC_ALPHA0_B_F(0x0);

	writel(alpha, ctx->regs + VIDOSD_C(win));

	alpha = VIDOSDxD_ALPHA1_R_F(0xff) |
			VIDOSDxD_ALPHA1_G_F(0xff) |
			VIDOSDxD_ALPHA1_B_F(0xff);

	writel(alpha, ctx->regs + VIDOSD_D(win));

	decon_win_set_pixfmt(ctx, win, fb);

	/* hardware window 0 doesn't support color key. */
	if (win != 0)
		decon_win_set_colkey(ctx, win);

	/* wincon */
	val = readl(ctx->regs + WINCON(win));
	val |= WINCONx_TRIPLE_BUF_MODE;
	val |= WINCONx_ENWIN;
	writel(val, ctx->regs + WINCON(win));

	/* Enable DMA channel and unprotect windows */
	decon_shadow_protect_win(ctx, win, false);

	decon_trigger_shadow_update(ctx, win);
}

static void decon_disable_plane(struct exynos_drm_crtc *crtc,
				struct exynos_drm_plane *plane)
{
	struct decon_context *ctx = crtc->ctx;
	unsigned int win = plane->index;
	u32 val;

	if (ctx->data->use_dpu_irq_regs)
		return;

	/* protect windows */
	decon_shadow_protect_win(ctx, win, true);

	/* wincon */
	val = readl(ctx->regs + WINCON(win));
	val &= ~WINCONx_ENWIN;
	writel(val, ctx->regs + WINCON(win));

	decon_trigger_shadow_update(ctx, win);
}

static void decon_atomic_flush(struct exynos_drm_crtc *crtc)
{
	struct decon_context *ctx = crtc->ctx;
	struct drm_display_mode *mode = &crtc->base.state->adjusted_mode;
	int i;

	if (!ctx->data->use_dpu_irq_regs)
		for (i = 0; i < WINDOWS_NR; i++)
			decon_shadow_protect_win(ctx, i, false);

	/*
	 * Arm the vblank event before triggering command-mode hardware so the
	 * first TE/frame start after unmasking cannot race past the event arm.
	 */
	if (ctx->data->use_dpu_irq_regs)
		exynos_crtc_handle_event(crtc);

	if (ctx->data->use_dpu_irq_regs)
		decon_commit_dpu(ctx, mode);
	else
		exynos_crtc_handle_event(crtc);
}

static void decon_init(struct decon_context *ctx)
{
	u32 val;

	if (ctx->data->use_dpu_irq_regs) {
		decon_init_dpu(ctx);
		return;
	}

	writel(VIDCON0_SWRESET, ctx->regs + VIDCON0);

	val = VIDOUTCON0_DISP_IF_0_ON;
	if (!ctx->i80_if)
		val |= VIDOUTCON0_RGBIF;
	writel(val, ctx->regs + VIDOUTCON0);

	writel(VCLKCON0_CLKVALUP | VCLKCON0_VCLKFREE, ctx->regs + VCLKCON0);

	if (!ctx->i80_if)
		writel(VIDCON1_VCLK_HOLD, ctx->regs + VIDCON1(0));
}

static void decon_atomic_enable(struct exynos_drm_crtc *crtc)
{
	struct decon_context *ctx = crtc->ctx;
	int ret;

	ret = pm_runtime_resume_and_get(ctx->dev);
	if (ret < 0) {
		DRM_DEV_ERROR(ctx->dev, "failed to enable DECON device.\n");
		return;
	}

	decon_init(ctx);

	if (ctx->data->use_dpu_irq_regs) {
		decon_dpu_set_irqs(ctx, true);
		enable_irq(ctx->irq_fd);
		if (ctx->irq_ext >= 0)
			enable_irq(ctx->irq_ext);
	}

	/* if vblank was enabled status, enable it again. */
	if (test_and_clear_bit(0, &ctx->irq_flags))
		decon_enable_vblank(ctx->crtc);

}

static void decon_atomic_disable(struct exynos_drm_crtc *crtc)
{
	struct decon_context *ctx = crtc->ctx;
	int i;

	/*
	 * We need to make sure that all windows are disabled before we
	 * suspend that connector. Otherwise we might try to scan from
	 * a destroyed buffer later.
	 */
	for (i = 0; i < WINDOWS_NR; i++)
		decon_disable_plane(crtc, &ctx->planes[i]);

	if (ctx->data->use_dpu_irq_regs) {
		disable_irq_nosync(ctx->irq_fd);
		if (ctx->irq_ext >= 0)
			disable_irq_nosync(ctx->irq_ext);
		if (test_bit(0, &ctx->irq_flags))
			disable_irq_nosync(ctx->irq_fs);
		decon_dpu_set_irqs(ctx, false);
	}

	pm_runtime_put_sync(ctx->dev);
}

static const struct exynos_drm_crtc_ops decon_crtc_ops = {
	.atomic_enable = decon_atomic_enable,
	.atomic_disable = decon_atomic_disable,
	.enable_vblank = decon_enable_vblank,
	.disable_vblank = decon_disable_vblank,
	.atomic_begin = decon_atomic_begin,
	.update_plane = decon_update_plane,
	.disable_plane = decon_disable_plane,
	.atomic_flush = decon_atomic_flush,
	.te_handler = decon_te_handler,
};


static irqreturn_t decon_irq_handler(int irq, void *dev_id)
{
	struct decon_context *ctx = (struct decon_context *)dev_id;
	u32 val, clear_bit;

	if (ctx->data->use_dpu_irq_regs) {
		val = decon_read(ctx, DECON_INT_PEND);
		if (val)
			decon_write(ctx, DECON_INT_PEND, val);

		if ((val & INT_PEND_FRAME_DONE) && ctx->data->command_mode) {
			if (ctx->drm_dev && drm_dev_has_vblank(ctx->drm_dev)) {
				drm_crtc_handle_vblank(&ctx->crtc->base);
				if (atomic_read(&ctx->wait_vsync_event)) {
					atomic_set(&ctx->wait_vsync_event, 0);
					wake_up(&ctx->wait_vsync_queue);
				}
			}
			decon_set_trigger_mask(ctx, true);
		}

		if (val & INT_PEND_EXTRA) {
			clear_bit = decon_read(ctx, DECON_INT_PEND_EXTRA);
			if (clear_bit)
				decon_write(ctx, DECON_INT_PEND_EXTRA, clear_bit);
		}

		return IRQ_HANDLED;
	}

	val = readl(ctx->regs + VIDINTCON1);

	clear_bit = ctx->i80_if ? VIDINTCON1_INT_I80 : VIDINTCON1_INT_FRAME;
	if (val & clear_bit)
		writel(clear_bit, ctx->regs + VIDINTCON1);

	/* check the crtc is detached already from encoder */
	if (!ctx->drm_dev)
		goto out;

	/* check if crtc and vblank have been initialized properly */
	if (!drm_dev_has_vblank(ctx->drm_dev))
		goto out;

	if (!ctx->i80_if) {
		drm_crtc_handle_vblank(&ctx->crtc->base);

		/* set wait vsync event to zero and wake up queue. */
		if (atomic_read(&ctx->wait_vsync_event)) {
			atomic_set(&ctx->wait_vsync_event, 0);
			wake_up(&ctx->wait_vsync_queue);
		}
	}
out:
	return IRQ_HANDLED;
}

static irqreturn_t decon_frame_start_irq_handler(int irq, void *dev_id)
{
	struct decon_context *ctx = dev_id;
	u32 val;

	if (ctx->data->use_dpu_irq_regs) {
		val = decon_read(ctx, DECON_INT_PEND);
		if (val & INT_PEND_FRAME_START)
			decon_write(ctx, DECON_INT_PEND, INT_PEND_FRAME_START);
	}

	if (!ctx->drm_dev)
		return IRQ_HANDLED;

	if (!drm_dev_has_vblank(ctx->drm_dev))
		return IRQ_HANDLED;

	drm_crtc_handle_vblank(&ctx->crtc->base);

	if (atomic_read(&ctx->wait_vsync_event)) {
		atomic_set(&ctx->wait_vsync_event, 0);
		wake_up(&ctx->wait_vsync_queue);
	}

	return IRQ_HANDLED;
}

static void decon_te_handler(struct exynos_drm_crtc *crtc)
{
	struct decon_context *ctx = crtc->ctx;

	if (!ctx->data->command_mode)
		return;

	if (!ctx->drm_dev)
		return;

	if (!drm_dev_has_vblank(ctx->drm_dev))
		return;

	dev_info(ctx->dev, "TE vblank\n");
	drm_crtc_handle_vblank(&ctx->crtc->base);

	if (atomic_read(&ctx->wait_vsync_event)) {
		atomic_set(&ctx->wait_vsync_event, 0);
		wake_up(&ctx->wait_vsync_queue);
	}
}

static int decon_request_irqs(struct platform_device *pdev,
			      struct decon_context *ctx)
{
	struct device *dev = &pdev->dev;
	int ret;

	if (!ctx->data->use_dpu_irq_regs) {
		ret = decon_get_irq(pdev, ctx);
		if (ret < 0)
			return ret;

		ctx->irq = ret;
		return devm_request_irq(dev, ctx->irq,
					ctx->data->use_frame_start_irq && !ctx->i80_if ?
					decon_frame_start_irq_handler :
					decon_irq_handler,
					0, "drm_decon", ctx);
	}

	ctx->irq_fs = platform_get_irq_byname(pdev, "frame_start");
	if (ctx->irq_fs < 0)
		return ctx->irq_fs;

	ret = devm_request_irq(dev, ctx->irq_fs, decon_frame_start_irq_handler,
			       0, "drm_decon_fs", ctx);
	if (ret)
		return ret;

	ctx->irq_fd = platform_get_irq_byname(pdev, "frame_done");
	if (ctx->irq_fd < 0)
		return ctx->irq_fd;

	ret = devm_request_irq(dev, ctx->irq_fd, decon_irq_handler,
			       0, "drm_decon_fd", ctx);
	if (ret)
		return ret;

	ctx->irq_ext = platform_get_irq_byname_optional(pdev, "extra");
	if (ctx->irq_ext >= 0) {
		ret = devm_request_irq(dev, ctx->irq_ext, decon_irq_handler,
				       0, "drm_decon_ext", ctx);
		if (ret)
			return ret;
	}

	return 0;
}

static int decon_bind(struct device *dev, struct device *master, void *data)
{
	struct decon_context *ctx = dev_get_drvdata(dev);
	struct drm_device *drm_dev = data;
	struct exynos_drm_plane *exynos_plane;
	unsigned int i;
	int ret;

	ret = decon_ctx_initialize(ctx, drm_dev);
	if (ret) {
		DRM_DEV_ERROR(dev, "decon_ctx_initialize failed.\n");
		return ret;
	}

	for (i = 0; i < WINDOWS_NR; i++) {
		ctx->configs[i].pixel_formats = decon_formats;
		ctx->configs[i].num_pixel_formats = ARRAY_SIZE(decon_formats);
		ctx->configs[i].zpos = i;
		ctx->configs[i].type = decon_win_types[i];

		ret = exynos_plane_init(drm_dev, &ctx->planes[i], i,
					&ctx->configs[i]);
		if (ret)
			return ret;
	}

	exynos_plane = &ctx->planes[DEFAULT_WIN];
	ctx->crtc = exynos_drm_crtc_create(drm_dev, &exynos_plane->base,
			EXYNOS_DISPLAY_TYPE_LCD, &decon_crtc_ops, ctx);
	if (IS_ERR(ctx->crtc)) {
		decon_ctx_remove(ctx);
		return PTR_ERR(ctx->crtc);
	}

	if (ctx->encoder)
		exynos_dpi_bind(drm_dev, ctx->encoder);

	return 0;

}

static void decon_unbind(struct device *dev, struct device *master,
			void *data)
{
	struct decon_context *ctx = dev_get_drvdata(dev);

	decon_atomic_disable(ctx->crtc);

	if (ctx->encoder)
		exynos_dpi_remove(ctx->encoder);

	decon_ctx_remove(ctx);
}

static const struct component_ops decon_component_ops = {
	.bind	= decon_bind,
	.unbind = decon_unbind,
};

static int decon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct decon_context *ctx;
	struct device_node *i80_if_timings;
	int ret;

	if (!dev->of_node)
		return -ENODEV;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dev = dev;
	ctx->data = of_device_get_match_data(dev);

	i80_if_timings = of_get_child_by_name(dev->of_node, "i80-if-timings");
	if (i80_if_timings)
		ctx->i80_if = true;
	of_node_put(i80_if_timings);

	ctx->regs = devm_platform_ioremap_resource_byname(pdev, "main");
	if (IS_ERR(ctx->regs)) {
		if (PTR_ERR(ctx->regs) != -EINVAL)
			return PTR_ERR(ctx->regs);
		ctx->regs = of_iomap(dev->of_node, 0);
		if (!ctx->regs)
			return -ENOMEM;
	}

	ctx->win_regs = devm_platform_ioremap_resource_byname(pdev, "win");
	if (IS_ERR(ctx->win_regs)) {
		if (PTR_ERR(ctx->win_regs) != -EINVAL)
			return PTR_ERR(ctx->win_regs);
		ctx->win_regs = ctx->regs;
	}

	ctx->sub_regs = devm_platform_ioremap_resource_byname(pdev, "sub");
	if (IS_ERR(ctx->sub_regs)) {
		if (PTR_ERR(ctx->sub_regs) != -EINVAL)
			return PTR_ERR(ctx->sub_regs);
		ctx->sub_regs = ctx->regs;
	}

	ctx->wincon_regs = devm_platform_ioremap_resource_byname(pdev, "wincon");
	if (IS_ERR(ctx->wincon_regs)) {
		if (PTR_ERR(ctx->wincon_regs) != -EINVAL)
			return PTR_ERR(ctx->wincon_regs);
		ctx->wincon_regs = ctx->regs;
	}

	if (!ctx->regs || !ctx->win_regs || !ctx->wincon_regs)
		return -ENOMEM;

	ctx->pclk = devm_clk_get(dev, "pclk_decon0");
	if (IS_ERR(ctx->pclk)) {
		dev_err(dev, "failed to get bus clock pclk\n");
		ret = PTR_ERR(ctx->pclk);
		goto err_iounmap;
	}

	ctx->aclk = devm_clk_get(dev, "aclk_decon0");
	if (IS_ERR(ctx->aclk)) {
		dev_err(dev, "failed to get bus clock aclk\n");
		ret = PTR_ERR(ctx->aclk);
		goto err_iounmap;
	}

	ctx->eclk = devm_clk_get(dev, "decon0_eclk");
	if (IS_ERR(ctx->eclk)) {
		dev_err(dev, "failed to get eclock\n");
		ret = PTR_ERR(ctx->eclk);
		goto err_iounmap;
	}

	ctx->vclk = devm_clk_get(dev, "decon0_vclk");
	if (IS_ERR(ctx->vclk)) {
		dev_err(dev, "failed to get vclock\n");
		ret = PTR_ERR(ctx->vclk);
		goto err_iounmap;
	}

	ret = decon_request_irqs(pdev, ctx);
	if (ret) {
		dev_err(dev, "irq request failed.\n");
		goto err_iounmap;
	}

	if (ctx->data->use_dpu_irq_regs) {
		disable_irq(ctx->irq_fs);
		disable_irq(ctx->irq_fd);
		if (ctx->irq_ext >= 0)
			disable_irq(ctx->irq_ext);
	}

	init_waitqueue_head(&ctx->wait_vsync_queue);
	atomic_set(&ctx->wait_vsync_event, 0);

	platform_set_drvdata(pdev, ctx);

	ctx->encoder = exynos_dpi_probe(dev);
	if (IS_ERR(ctx->encoder)) {
		ret = PTR_ERR(ctx->encoder);
		goto err_iounmap;
	}

	pm_runtime_enable(dev);

	ret = component_add(dev, &decon_component_ops);
	if (ret)
		goto err_disable_pm_runtime;

	return ret;

err_disable_pm_runtime:
	pm_runtime_disable(dev);

err_iounmap:
	iounmap(ctx->regs);

	return ret;
}

static void decon_remove(struct platform_device *pdev)
{
	struct decon_context *ctx = dev_get_drvdata(&pdev->dev);

	pm_runtime_disable(&pdev->dev);

	iounmap(ctx->regs);

	component_del(&pdev->dev, &decon_component_ops);
}

static int exynos9_decon_suspend(struct device *dev)
{
	struct decon_context *ctx = dev_get_drvdata(dev);

	clk_disable_unprepare(ctx->vclk);
	clk_disable_unprepare(ctx->eclk);
	clk_disable_unprepare(ctx->aclk);
	clk_disable_unprepare(ctx->pclk);

	return 0;
}

static int exynos9_decon_resume(struct device *dev)
{
	struct decon_context *ctx = dev_get_drvdata(dev);
	int ret;

	ret = clk_prepare_enable(ctx->pclk);
	if (ret < 0) {
		DRM_DEV_ERROR(dev, "Failed to prepare_enable the pclk [%d]\n",
			      ret);
		goto err_pclk_enable;
	}

	ret = clk_prepare_enable(ctx->aclk);
	if (ret < 0) {
		DRM_DEV_ERROR(dev, "Failed to prepare_enable the aclk [%d]\n",
			      ret);
		goto err_aclk_enable;
	}

	ret = clk_prepare_enable(ctx->eclk);
	if  (ret < 0) {
		DRM_DEV_ERROR(dev, "Failed to prepare_enable the eclk [%d]\n",
			      ret);
		goto err_eclk_enable;
	}

	ret = clk_prepare_enable(ctx->vclk);
	if  (ret < 0) {
		DRM_DEV_ERROR(dev, "Failed to prepare_enable the vclk [%d]\n",
			      ret);
		goto err_vclk_enable;
	}

	return 0;

err_vclk_enable:
	clk_disable_unprepare(ctx->eclk);
err_eclk_enable:
	clk_disable_unprepare(ctx->aclk);
err_aclk_enable:
	clk_disable_unprepare(ctx->pclk);
err_pclk_enable:
	return ret;
}

static DEFINE_RUNTIME_DEV_PM_OPS(exynos9_decon_pm_ops, exynos9_decon_suspend,
				 exynos9_decon_resume, NULL);

struct platform_driver exynos9_decon_driver = {
	.probe		= decon_probe,
	.remove		= decon_remove,
	.driver		= {
		.name	= "exynos9-decon",
		.pm	= pm_ptr(&exynos9_decon_pm_ops),
		.of_match_table = decon_driver_dt_match,
	},
};
