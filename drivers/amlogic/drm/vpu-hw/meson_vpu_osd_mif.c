// SPDX-License-Identifier: (GPL-2.0+ OR MIT)
/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

#ifdef CONFIG_AMLOGIC_MEDIA_CANVAS
#include <linux/amlogic/media/canvas/canvas.h>
#include <linux/amlogic/media/canvas/canvas_mgr.h>
#endif
#include "meson_vpu_pipeline.h"
#include "meson_crtc.h"
#include "meson_vpu_reg.h"
#include "meson_vpu_util.h"

static int osd_hold_line = 8;
module_param(osd_hold_line, int, 0664);
MODULE_PARM_DESC(osd_hold_line, "osd_hold_line");

static struct osd_mif_reg_s osd_mif_reg[HW_OSD_MIF_NUM] = {
	{
		VIU_OSD1_CTRL_STAT,
		VIU_OSD1_CTRL_STAT2,
		VIU_OSD1_COLOR_ADDR,
		VIU_OSD1_COLOR,
		VIU_OSD1_TCOLOR_AG0,
		VIU_OSD1_TCOLOR_AG1,
		VIU_OSD1_TCOLOR_AG2,
		VIU_OSD1_TCOLOR_AG3,
		VIU_OSD1_BLK0_CFG_W0,
		VIU_OSD1_BLK0_CFG_W1,
		VIU_OSD1_BLK0_CFG_W2,
		VIU_OSD1_BLK0_CFG_W3,
		VIU_OSD1_BLK0_CFG_W4,
		VIU_OSD1_BLK1_CFG_W4,
		VIU_OSD1_BLK2_CFG_W4,
		VIU_OSD1_FIFO_CTRL_STAT,
		VIU_OSD1_TEST_RDDATA,
		VIU_OSD1_PROT_CTRL,
		VIU_OSD1_MALI_UNPACK_CTRL,
		VIU_OSD1_DIMM_CTRL,
	},
	{
		VIU_OSD2_CTRL_STAT,
		VIU_OSD2_CTRL_STAT2,
		VIU_OSD2_COLOR_ADDR,
		VIU_OSD2_COLOR,
		VIU_OSD2_TCOLOR_AG0,
		VIU_OSD2_TCOLOR_AG1,
		VIU_OSD2_TCOLOR_AG2,
		VIU_OSD2_TCOLOR_AG3,
		VIU_OSD2_BLK0_CFG_W0,
		VIU_OSD2_BLK0_CFG_W1,
		VIU_OSD2_BLK0_CFG_W2,
		VIU_OSD2_BLK0_CFG_W3,
		VIU_OSD2_BLK0_CFG_W4,
		VIU_OSD2_BLK1_CFG_W4,
		VIU_OSD2_BLK2_CFG_W4,
		VIU_OSD2_FIFO_CTRL_STAT,
		VIU_OSD2_TEST_RDDATA,
		VIU_OSD2_PROT_CTRL,
		VIU_OSD2_MALI_UNPACK_CTRL,
		VIU_OSD2_DIMM_CTRL,
	},
	{
		VIU_OSD3_CTRL_STAT,
		VIU_OSD3_CTRL_STAT2,
		VIU_OSD3_COLOR_ADDR,
		VIU_OSD3_COLOR,
		VIU_OSD3_TCOLOR_AG0,
		VIU_OSD3_TCOLOR_AG1,
		VIU_OSD3_TCOLOR_AG2,
		VIU_OSD3_TCOLOR_AG3,
		VIU_OSD3_BLK0_CFG_W0,
		VIU_OSD3_BLK0_CFG_W1,
		VIU_OSD3_BLK0_CFG_W2,
		VIU_OSD3_BLK0_CFG_W3,
		VIU_OSD3_BLK0_CFG_W4,
		VIU_OSD3_BLK1_CFG_W4,
		VIU_OSD3_BLK2_CFG_W4,
		VIU_OSD3_FIFO_CTRL_STAT,
		VIU_OSD3_TEST_RDDATA,
		VIU_OSD3_PROT_CTRL,
		VIU_OSD3_MALI_UNPACK_CTRL,
		VIU_OSD3_DIMM_CTRL,
	}
};

static unsigned int osd_canvas[3][2];
static u32 osd_canvas_index[3] = {0, 0, 0};

/*
 * Internal function to query information for a given format. See
 * meson_drm_format_info() for the public API.
 */
const struct meson_drm_format_info *__meson_drm_format_info(u32 format)
{
	static const struct meson_drm_format_info formats[] = {
		{ .format = DRM_FORMAT_XRGB8888,
			.hw_blkmode = BLOCK_MODE_32BIT,
			.hw_colormat = COLOR_MATRIX_ARGB8888,
			.alpha_replace = 1 },
		{ .format = DRM_FORMAT_XBGR8888,
			.hw_blkmode = BLOCK_MODE_32BIT,
			.hw_colormat = COLOR_MATRIX_ABGR8888,
			.alpha_replace = 1 },
		{ .format = DRM_FORMAT_RGBX8888,
			.hw_blkmode = BLOCK_MODE_32BIT,
			.hw_colormat = COLOR_MATRIX_RGBA8888,
			.alpha_replace = 1 },
		{ .format = DRM_FORMAT_BGRX8888,
			.hw_blkmode = BLOCK_MODE_32BIT,
			.hw_colormat = COLOR_MATRIX_BGRA8888,
			.alpha_replace = 1 },
		{ .format = DRM_FORMAT_ARGB8888,
			.hw_blkmode = BLOCK_MODE_32BIT,
			.hw_colormat = COLOR_MATRIX_ARGB8888,
			.alpha_replace = 0 },
		{ .format = DRM_FORMAT_ABGR8888,
			.hw_blkmode = BLOCK_MODE_32BIT,
			.hw_colormat = COLOR_MATRIX_ABGR8888,
			.alpha_replace = 0 },
		{ .format = DRM_FORMAT_RGBA8888,
			.hw_blkmode = BLOCK_MODE_32BIT,
			.hw_colormat = COLOR_MATRIX_RGBA8888,
			.alpha_replace = 0 },
		{ .format = DRM_FORMAT_BGRA8888,
			.hw_blkmode = BLOCK_MODE_32BIT,
			.hw_colormat = COLOR_MATRIX_BGRA8888,
			.alpha_replace = 0 },
		{ .format = DRM_FORMAT_RGB888,
			.hw_blkmode = BLOCK_MODE_24BIT,
			.hw_colormat = COLOR_MATRIX_RGB888,
			.alpha_replace = 0 },
		{ .format = DRM_FORMAT_RGB565,
			.hw_blkmode = BLOCK_MODE_16BIT,
			.hw_colormat = COLOR_MATRIX_565,
			.alpha_replace = 0 },
	};

	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(formats); ++i) {
		if (formats[i].format == format)
			return &formats[i];
	}

	return NULL;
}

const struct meson_drm_format_info *__meson_drm_afbc_format_info(u32 format)
{
	static const struct meson_drm_format_info formats[] = {
		{ .format = DRM_FORMAT_XRGB8888,
			.hw_blkmode = BLOCK_MODE_RGBA8888,
			.alpha_replace = 1 },
		{ .format = DRM_FORMAT_XBGR8888,
			.hw_blkmode = BLOCK_MODE_RGBA8888,
			.alpha_replace = 1 },
		{ .format = DRM_FORMAT_RGBX8888,
			.hw_blkmode = BLOCK_MODE_RGBA8888,
			.alpha_replace = 1 },
		{ .format = DRM_FORMAT_BGRX8888,
			.hw_blkmode = BLOCK_MODE_RGBA8888,
			.alpha_replace = 1 },
		{ .format = DRM_FORMAT_ARGB8888,
			.hw_blkmode = BLOCK_MODE_RGBA8888,
			.alpha_replace = 0 },
		{ .format = DRM_FORMAT_ABGR8888,
			.hw_blkmode = BLOCK_MODE_RGBA8888,
			.alpha_replace = 0 },
		{ .format = DRM_FORMAT_RGBA8888,
			.hw_blkmode = BLOCK_MODE_RGBA8888,
			.alpha_replace = 0 },
		{ .format = DRM_FORMAT_BGRA8888,
			.hw_blkmode = BLOCK_MODE_RGBA8888,
			.alpha_replace = 0 },
		{ .format = DRM_FORMAT_RGB888,
			.hw_blkmode = BLOCK_MODE_RGB888,
			.alpha_replace = 0 },
		{ .format = DRM_FORMAT_RGB565,
			.hw_blkmode = BLOCK_MODE_RGB565,
			.alpha_replace = 0 },
		{ .format = DRM_FORMAT_RGBA1010102,
			.hw_blkmode = BLOCK_MODE_RGBA1010102,
			.alpha_replace = 0 },
	};

	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(formats); ++i) {
		if (formats[i].format == format)
			return &formats[i];
	}

	return NULL;
}

/**
 * meson_drm_format_info - query information for a given format
 * @format: pixel format (DRM_FORMAT_*)
 *
 * The caller should only pass a supported pixel format to this function.
 * Unsupported pixel formats will generate a warning in the kernel log.
 *
 * Returns:
 * The instance of struct meson_drm_format_info that describes the
 * pixel format, or NULL if the format is unsupported.
 */
const struct meson_drm_format_info *meson_drm_format_info(u32 format,
							  bool afbc_en)
{
	const struct meson_drm_format_info *info;

	if (afbc_en)
		info = __meson_drm_afbc_format_info(format);
	else
		info = __meson_drm_format_info(format);
	WARN_ON(!info);
	return info;
}

/**
 * meson_drm_format_hw_blkmode - get the hw_blkmode for format
 * @format: pixel format (DRM_FORMAT_*)
 *
 * Returns:
 * The hw_blkmode match the specified pixel format.
 */
static u8 meson_drm_format_hw_blkmode(u32 format, bool afbc_en)
{
	const struct meson_drm_format_info *info;

	info = meson_drm_format_info(format, afbc_en);
	return info ? info->hw_blkmode : 0;
}

/**
 * meson_drm_format_hw_colormat - get the hw_colormat for format
 * @format: pixel format (DRM_FORMAT_*)
 *
 * Returns:
 * The hw_colormat match the specified pixel format.
 */
static u8 meson_drm_format_hw_colormat(u32 format, bool afbc_en)
{
	const struct meson_drm_format_info *info;

	info = meson_drm_format_info(format, afbc_en);
	return info ? info->hw_colormat : 0;
}

/**
 * meson_drm_format_alpha_replace - get the alpha replace for format
 * @format: pixel format (DRM_FORMAT_*)
 *
 * Returns:
 * The alpha_replace match the specified pixel format.
 */
static u8 meson_drm_format_alpha_replace(u32 format, bool afbc_en)
{
	const struct meson_drm_format_info *info;

	info = meson_drm_format_info(format, afbc_en);
	return info ? info->alpha_replace : 0;
}

/*osd hold line config*/
void ods_hold_line_config(struct osd_mif_reg_s *reg, int hold_line)
{
	u32 data = 0, value = 0;

	data = meson_drm_read_reg(reg->viu_osd_fifo_ctrl_stat);
	value = (data >> 5) & 0x1f;
	if (value != hold_line)
		meson_vpu_write_reg_bits(reg->viu_osd_fifo_ctrl_stat,
					 hold_line & 0x1f, 5, 5);
}

/*osd input size config*/
void osd_input_size_config(struct osd_mif_reg_s *reg, struct osd_scope_s scope)
{
	meson_vpu_write_reg(reg->viu_osd_blk0_cfg_w1,
			    (scope.h_end << 16) | /*x_end pixels[13bits]*/
		scope.h_start/*x_start pixels[13bits]*/);
	meson_vpu_write_reg(reg->viu_osd_blk0_cfg_w2,
			    (scope.v_end << 16) | /*y_end pixels[13bits]*/
		scope.v_start/*y_start pixels[13bits]*/);
}

/*osd canvas config*/
void osd_canvas_config(struct osd_mif_reg_s *reg, u32 canvas_index)
{
	meson_vpu_write_reg_bits(reg->viu_osd_blk0_cfg_w0,
				 canvas_index, 16, 8);
}

/*osd mali afbc src en
 * 1: read data from mali afbcd;0: read data from DDR directly
 */
void osd_mali_src_en(struct osd_mif_reg_s *reg, u8 osd_index, bool flag)
{
	meson_vpu_write_reg_bits(reg->viu_osd_blk0_cfg_w0, flag, 30, 1);
	meson_vpu_write_reg_bits(OSD_PATH_MISC_CTRL, flag, (osd_index + 4), 1);
}

/*osd endian mode
 * 1: little endian;0: big endian[for mali afbc input]
 */
void osd_endian_mode(struct osd_mif_reg_s *reg, bool flag)
{
	meson_vpu_write_reg_bits(reg->viu_osd_blk0_cfg_w0, flag, 15, 1);
}

/*osd mif enable*/
void osd_block_enable(struct osd_mif_reg_s *reg, bool flag)
{
	meson_vpu_write_reg_bits(reg->viu_osd_ctrl_stat, flag, 0, 1);
}

/*osd mem mode
 * 0: canvas_addr;1:linear_addr[for mali-afbc-mode]
 */
void osd_mem_mode(struct osd_mif_reg_s *reg, bool mode)
{
	meson_vpu_write_reg_bits(reg->viu_osd_ctrl_stat, mode, 2, 1);
}

/*osd alpha_div en
 *if input is premult,alpha_div=1,else alpha_div=0
 */
void osd_global_alpha_set(struct osd_mif_reg_s *reg, u16 val)
{
	meson_vpu_write_reg_bits(reg->viu_osd_ctrl_stat, val, 12, 9);
}

/*osd alpha_div en
 *if input is premult,alpha_div=1,else alpha_div=0
 */
void osd_premult_enable(struct osd_mif_reg_s *reg, int flag)
{
	/*afbc*/
	meson_vpu_write_reg_bits(reg->viu_osd_mali_unpack_ctrl, flag, 28, 1);
	/*mif*/
	//meson_vpu_write_reg_bits(reg->viu_osd_ctrl_stat, flag, 1, 1);
}

/*osd x reverse en
 *reverse read in X direction
 */
void osd_reverse_x_enable(struct osd_mif_reg_s *reg, bool flag)
{
	meson_vpu_write_reg_bits(reg->viu_osd_blk0_cfg_w0, flag, 28, 1);
}

/*osd y reverse en
 *reverse read in Y direction
 */
void osd_reverse_y_enable(struct osd_mif_reg_s *reg, bool flag)
{
	meson_vpu_write_reg_bits(reg->viu_osd_blk0_cfg_w0, flag, 29, 1);
}

/*osd mali unpack en
 * 1: osd will unpack mali_afbc_src;0:osd will unpack normal src
 */
void osd_mali_unpack_enable(struct osd_mif_reg_s *reg, bool flag)
{
	meson_vpu_write_reg_bits(reg->viu_osd_mali_unpack_ctrl, flag, 31, 1);
}

void osd_ctrl_init(struct osd_mif_reg_s *reg)
{
	/*Need config follow crtc index.*/
	u8 holdline = VIU1_DEFAULT_HOLD_LINE;

	meson_vpu_write_reg(reg->viu_osd_fifo_ctrl_stat,
			    (1 << 31) | /*BURSET_LEN_SEL[2]*/
			    (0 << 30) | /*no swap*/
			    (0 << 29) | /*div swap*/
			    (2 << 24) | /*Fifo_lim 5bits*/
			    (2 << 22) | /*Fifo_ctrl 2bits*/
			    (0x20 << 12) | /*FIFO_DEPATH_VAL 7bits*/
			    (1 << 10) | /*BURSET_LEN_SEL[1:0]*/
			    (holdline << 5) | /*hold fifo lines 5bits*/
			    (0 << 4) | /*CLEAR_ERR*/
			    (0 << 3) | /*fifo_sync_rst*/
			    (0 << 1) | /*ENDIAN:no conversion*/
			    (1 << 0)/*urgent enable*/);
	meson_vpu_write_reg(reg->viu_osd_ctrl_stat,
			    (0 << 31) | /*osd_cfg_sync_en*/
			    (0 << 30) | /*Enable free_clk*/
			    (0x100 << 12) | /*global alpha*/
			    (0 << 11) | /*TEST_RD_EN*/
			    (0 << 2) | /*osd_mem_mode 0:canvas_addr*/
			    (0 << 1) | /*premult_en*/
			    (0 << 0)/*OSD_BLK_ENABLE*/);
}

static void osd_color_config(struct osd_mif_reg_s *reg,
			     u32 pixel_format, u32 pixel_blend, bool afbc_en)
{
	u8 blk_mode, colormat, alpha_replace;

	blk_mode = meson_drm_format_hw_blkmode(pixel_format, afbc_en);
	colormat = meson_drm_format_hw_colormat(pixel_format, afbc_en);
	alpha_replace = (pixel_blend == DRM_MODE_BLEND_PIXEL_NONE) ||
		meson_drm_format_alpha_replace(pixel_format, afbc_en);
	meson_vpu_write_reg_bits(reg->viu_osd_blk0_cfg_w0,
				 blk_mode, 8, 4);
	meson_vpu_write_reg_bits(reg->viu_osd_blk0_cfg_w0,
				 colormat, 2, 4);

	if (alpha_replace)
		/*replace alpha    : bit 14 enable, 6~13 alpha val.*/
		meson_vpu_write_reg_bits(reg->viu_osd_ctrl_stat2, 0x1ff, 6, 9);
	else
		meson_vpu_write_reg_bits(reg->viu_osd_ctrl_stat2, 0x0, 6, 9);
}

static void osd_afbc_config(struct osd_mif_reg_s *reg,
			    u8 osd_index, bool afbc_en)
{
	if (!afbc_en)
		meson_vpu_write_reg_bits(reg->viu_osd_ctrl_stat2, 0, 1, 1);
	else
		meson_vpu_write_reg_bits(reg->viu_osd_ctrl_stat2, 1, 1, 1);

	osd_mali_unpack_enable(reg, afbc_en);
	osd_mali_src_en(reg, osd_index, afbc_en);
	osd_endian_mode(reg, !afbc_en);
	osd_mem_mode(reg, afbc_en);
}

static void osd_scan_mode_config(struct osd_mif_reg_s *reg, int scan_mode)
{
	if (scan_mode)
		meson_vpu_write_reg_bits(reg->viu_osd_blk0_cfg_w0, 0, 1, 1);
}

static void meson_drm_osd_canvas_alloc(void)
{
	if (canvas_pool_alloc_canvas_table("osd_drm",
					   &osd_canvas[0][0],
					   sizeof(osd_canvas) /
					   sizeof(osd_canvas[0][0]),
					   CANVAS_MAP_TYPE_1)) {
		DRM_INFO("allocate drm osd canvas error.\n");
	}
}

static void meson_drm_osd_canvas_free(void)
{
	canvas_pool_free_canvas_table(&osd_canvas[0][0],
				      sizeof(osd_canvas) /
				      sizeof(osd_canvas[0][0]));
}

static int osd_check_state(struct meson_vpu_block *vblk,
			   struct meson_vpu_block_state *state,
		struct meson_vpu_pipeline_state *mvps)
{
	struct meson_vpu_osd_layer_info *plane_info;
	struct meson_vpu_osd *osd = to_osd_block(vblk);
	struct meson_vpu_osd_state *mvos = to_osd_state(state);

	if (state->checked)
		return 0;

	state->checked = true;

	if (!mvos || mvos->plane_index >= MESON_MAX_OSDS) {
		DRM_INFO("mvos is NULL!\n");
		return -1;
	}
	DRM_DEBUG("%s - %d check_state called.\n", osd->base.name, vblk->index);
	plane_info = &mvps->plane_info[vblk->index];
	mvos->src_x = plane_info->src_x;
	mvos->src_y = plane_info->src_y;
	mvos->src_w = plane_info->src_w;
	mvos->src_h = plane_info->src_h;
	mvos->byte_stride = plane_info->byte_stride;
	mvos->phy_addr = plane_info->phy_addr;
	mvos->pixel_format = plane_info->pixel_format;
	mvos->fb_size = plane_info->fb_size;
	mvos->pixel_blend = plane_info->pixel_blend;
	mvos->rotation = plane_info->rotation;
	mvos->afbc_en = plane_info->afbc_en;
	mvos->blend_bypass = plane_info->blend_bypass;
	mvos->plane_index = plane_info->plane_index;
	mvos->global_alpha = plane_info->global_alpha;
	return 0;
}

static void osd_set_state(struct meson_vpu_block *vblk,
			  struct meson_vpu_block_state *state)
{
	struct drm_crtc *crtc;
	struct am_meson_crtc *amc;
	struct meson_vpu_osd *osd;
	struct meson_vpu_osd_state *mvos;
	u32 pixel_format, canvas_index, src_h, byte_stride;
	struct osd_scope_s scope_src = {0, 1919, 0, 1079};
	struct osd_mif_reg_s *reg;
	bool alpha_div_en = 0, reverse_x, reverse_y, afbc_en;
	u64 phy_addr;
	u16 global_alpha = 256; /*range 0~256*/


	if (!vblk || !state) {
		DRM_DEBUG("set_state break for NULL.\n");
		return;
	}

	osd = to_osd_block(vblk);
	mvos = to_osd_state(state);

	reg = osd->reg;
	if (!reg) {
		DRM_DEBUG("set_state break for NULL OSD mixer reg.\n");
		return;
	}

	crtc = vblk->pipeline->crtc;
	if (!crtc) {
		DRM_DEBUG("set_state break for NULL crtc.\n");
		return;
	}

	amc = to_am_meson_crtc(crtc);

	DRM_DEBUG("%s - %d %s called.\n", osd->base.name, vblk->index, __func__);

	afbc_en = mvos->afbc_en ? 1 : 0;
	if (mvos->pixel_blend == DRM_MODE_BLEND_PREMULTI)
		alpha_div_en = 1;

	/*drm alaph 16bit, amlogic alpha 8bit*/
	global_alpha = mvos->global_alpha >> 8;
	if (global_alpha == 0xff)
		global_alpha = 0x100;

	src_h = mvos->src_h + mvos->src_y;
	byte_stride = mvos->byte_stride;
	phy_addr = mvos->phy_addr;
	scope_src.h_start = mvos->src_x;
	scope_src.h_end = mvos->src_x + mvos->src_w - 1;
	scope_src.v_start = mvos->src_y;
	scope_src.v_end = mvos->src_y + mvos->src_h - 1;
	pixel_format = mvos->pixel_format;
	canvas_index = osd_canvas[vblk->index][osd_canvas_index[vblk->index]];

	reverse_x = (mvos->rotation & DRM_MODE_REFLECT_X) ? 1 : 0;
	reverse_y = (mvos->rotation & DRM_MODE_REFLECT_Y) ? 1 : 0;
	osd_reverse_x_enable(reg, reverse_x);
	osd_reverse_y_enable(reg, reverse_y);
	canvas_config(canvas_index, phy_addr, byte_stride, src_h,
		      CANVAS_ADDR_NOWRAP, CANVAS_BLKMODE_LINEAR);
	osd_canvas_index[vblk->index] ^= 1;
	osd_canvas_config(reg, canvas_index);
	osd_input_size_config(reg, scope_src);
	osd_color_config(reg, pixel_format, mvos->pixel_blend, afbc_en);
	osd_afbc_config(reg, vblk->index, afbc_en);
	osd_premult_enable(reg, alpha_div_en);
	osd_global_alpha_set(reg, global_alpha);
	osd_scan_mode_config(reg, vblk->pipeline->mode.flags &
				 DRM_MODE_FLAG_INTERLACE);
	ods_hold_line_config(reg, osd_hold_line);

	DRM_DEBUG("plane_index=%d,HW-OSD=%d\n",
		  mvos->plane_index, vblk->index);
	DRM_DEBUG("canvas_index[%d]=0x%x,phy_addr=0x%pa\n",
		  osd_canvas_index[vblk->index], canvas_index, &phy_addr);
	DRM_DEBUG("scope h/v start/end:[%d/%d/%d/%d]\n",
		  scope_src.h_start, scope_src.h_end,
		scope_src.v_start, scope_src.v_end);
	DRM_DEBUG("%s set_state done.\n", osd->base.name);
}

static void osd_hw_enable(struct meson_vpu_block *vblk)
{
	struct meson_vpu_osd *osd = to_osd_block(vblk);
	struct osd_mif_reg_s *reg = osd->reg;

	if (!vblk) {
		DRM_DEBUG("enable break for NULL.\n");
		return;
	}
	osd_block_enable(reg, 1);
	DRM_DEBUG("%s enable done.\n", osd->base.name);
}

static void osd_hw_disable(struct meson_vpu_block *vblk)
{
	struct meson_vpu_osd *osd;
	struct osd_mif_reg_s *reg;
	u8 version;

	if (!vblk) {
		DRM_DEBUG("disable break for NULL.\n");
		return;
	}

	osd = to_osd_block(vblk);
	reg = osd->reg;
	version = vblk->pipeline->osd_version;

	/*G12B should always enable,avoid afbc decoder error*/
	if (version != OSD_V2 && version != OSD_V3)
		osd_block_enable(reg, 0);
	DRM_DEBUG("%s disable done.\n", osd->base.name);
}

static void osd_dump_register(struct meson_vpu_block *vblk,
			      struct seq_file *seq)
{
	int osd_index;
	u32 value;
	char buff[8];
	struct meson_vpu_osd *osd;
	struct osd_mif_reg_s *reg;

	osd_index = vblk->index;
	osd = to_osd_block(vblk);
	reg = osd->reg;

	snprintf(buff, 8, "OSD%d", osd_index + 1);

	value = meson_drm_read_reg(reg->viu_osd_fifo_ctrl_stat);
	seq_printf(seq, "%s_%-35s\t0x%08X\n", buff, "FIFO_CTRL_STAT:", value);

	value = meson_drm_read_reg(reg->viu_osd_ctrl_stat);
	seq_printf(seq, "%s_%-35s\t0x%08X\n", buff, "CTRL_STAT:", value);

	value = meson_drm_read_reg(reg->viu_osd_ctrl_stat2);
	seq_printf(seq, "%s_%-35s\t0x%08X\n", buff, "CTRL_STAT2:", value);

	value = meson_drm_read_reg(reg->viu_osd_blk0_cfg_w0);
	seq_printf(seq, "%s_%-35s\t0x%08X\n", buff, "BLK0_CFG_W0:", value);

	value = meson_drm_read_reg(reg->viu_osd_blk0_cfg_w1);
	seq_printf(seq, "%s_%-35s\t0x%08X\n", buff, "BLK0_CFG_W1:", value);

	value = meson_drm_read_reg(reg->viu_osd_blk0_cfg_w2);
	seq_printf(seq, "%s_%-35s\t0x%08X\n", buff, "BLK0_CFG_W2:", value);

	value = meson_drm_read_reg(reg->viu_osd_blk0_cfg_w3);
	seq_printf(seq, "%s_%-35s\t0x%08X\n", buff, "BLK0_CFG_W3:", value);

	value = meson_drm_read_reg(reg->viu_osd_blk0_cfg_w4);
	seq_printf(seq, "%s_%-35s\t0x%08X\n", buff, "BLK0_CFG_W4:", value);

	value = meson_drm_read_reg(reg->viu_osd_blk1_cfg_w4);
	seq_printf(seq, "%s_%-35s\t0x%08X\n", buff, "BLK1_CFG_W4:", value);

	value = meson_drm_read_reg(reg->viu_osd_blk2_cfg_w4);
	seq_printf(seq, "%s_%-35s\t0x%08X\n", buff, "BLK2_CFG_W4:", value);

	value = meson_drm_read_reg(reg->viu_osd_prot_ctrl);
	seq_printf(seq, "%s_%-35s\t0x%08X\n", buff, "PROT_CTRL:", value);

	value = meson_drm_read_reg(reg->viu_osd_mali_unpack_ctrl);
	seq_printf(seq, "%s_%-35s\t0x%08X\n", buff, "MALI_UNPACK_CTRL:", value);

	value = meson_drm_read_reg(reg->viu_osd_dimm_ctrl);
	seq_printf(seq, "%s_%-35s\t0x%08X\n", buff, "DIMM_CTRL:", value);
}

static void osd_hw_init(struct meson_vpu_block *vblk)
{
	struct meson_vpu_osd *osd = to_osd_block(vblk);

	if (!vblk || !osd) {
		DRM_DEBUG("hw_init break for NULL.\n");
		return;
	}

	meson_drm_osd_canvas_alloc();

	osd->reg = &osd_mif_reg[vblk->index];
	osd_ctrl_init(osd->reg);

	DRM_DEBUG("%s hw_init done.\n", osd->base.name);
}

static void osd_hw_fini(struct meson_vpu_block *vblk)
{
	meson_drm_osd_canvas_free();
}

struct meson_vpu_block_ops osd_ops = {
	.check_state = osd_check_state,
	.update_state = osd_set_state,
	.enable = osd_hw_enable,
	.disable = osd_hw_disable,
	.dump_register = osd_dump_register,
	.init = osd_hw_init,
	.fini = osd_hw_fini,
};
