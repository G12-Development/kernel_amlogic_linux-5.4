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
#include <linux/amlogic/media/vfm/vframe.h>
#include "meson_vpu_pipeline.h"
#include "meson_crtc.h"
#include "meson_vpu_reg.h"
#include "meson_vpu_util.h"
#include "meson_drv.h"
#include "meson_plane.h"

static int video_axis_zoom = -1;
module_param(video_axis_zoom, int, 0664);
MODULE_PARM_DESC(video_axis_zoom, "video_axis_zoom");

static u32 video_type_get(u32 pixel_format)
{
	u32 vframe_type = 0;

	switch (pixel_format) {
	case DRM_FORMAT_NV12:
		vframe_type = VIDTYPE_VIU_NV12 | VIDTYPE_VIU_FIELD |
				VIDTYPE_PROGRESSIVE;
		break;
	case DRM_FORMAT_NV21:
		vframe_type = VIDTYPE_VIU_NV21 | VIDTYPE_VIU_FIELD |
				VIDTYPE_PROGRESSIVE;
		break;
	case DRM_FORMAT_YUYV:
	case DRM_FORMAT_YVYU:
		vframe_type = VIDTYPE_VIU_422 | VIDTYPE_VIU_FIELD |
				VIDTYPE_VIU_SINGLE_PLANE;
		break;
	default:
		DRM_INFO("no support pixel format:0x%x\n", pixel_format);
		break;
	}
	return vframe_type;
}

/* -----------------------------------------------------------------
 *           provider opeations
 * -----------------------------------------------------------------
 */
static struct vframe_s *vp_vf_peek(void *op_arg)
{
	struct meson_vpu_video *video = (struct meson_vpu_video *)op_arg;
	struct vframe_s *vf = NULL;

	if (kfifo_peek(&video->ready_q, &vf))
		return vf;
	else
		return NULL;
}

static struct vframe_s *vp_vf_get(void *op_arg)
{
	struct meson_vpu_video *video = (struct meson_vpu_video *)op_arg;
	struct vframe_s *vf = NULL;

	if (kfifo_get(&video->ready_q, &vf)) {
		if (!vf)
			return NULL;
		return vf;
	} else {
		return NULL;
	}
}

static void vp_vf_put(struct vframe_s *vf, void *op_arg)
{
	struct meson_vpu_video *video = (struct meson_vpu_video *)op_arg;

	if (!vf)
		return;

	kfifo_put(&video->free_q, vf);
}

static int vp_event_cb(int type, void *data, void *private_data)
{
	if (type & VFRAME_EVENT_RECEIVER_PUT)
		;
	else if (type & VFRAME_EVENT_RECEIVER_GET)
		;
	else if (type & VFRAME_EVENT_RECEIVER_FRAME_WAIT)
		;
	return 0;
}

static int vp_vf_states(struct vframe_states *states, void *op_arg)
{
	struct meson_vpu_video *video = (struct meson_vpu_video *)op_arg;

	states->vf_pool_size = BUFFER_NUM;
	states->buf_recycle_num = 0;
	states->buf_free_num = kfifo_len(&video->free_q);
	states->buf_avail_num = kfifo_len(&video->ready_q);
	return 0;
}

static const struct vframe_operations_s vp_vf_ops = {
	.peek = vp_vf_peek,
	.get  = vp_vf_get,
	.put  = vp_vf_put,
	.event_cb = vp_event_cb,
	.vf_states = vp_vf_states,
};

static int video_check_state(struct meson_vpu_block *vblk,
			     struct meson_vpu_block_state *state,
		struct meson_vpu_pipeline_state *mvps)
{
	struct meson_vpu_video_layer_info *plane_info;
	struct meson_vpu_video *video = to_video_block(vblk);
	struct meson_vpu_video_state *mvvs = to_video_state(state);
	int i;

	if (state->checked)
		return 0;

	state->checked = true;

	if (!mvvs || mvvs->plane_index >= MESON_MAX_VIDEO) {
		DRM_INFO("mvvs is NULL!\n");
		return -1;
	}
	DRM_DEBUG("%s check_state called.\n", video->base.name);
	plane_info = &mvps->video_plane_info[vblk->index];
	mvvs->src_x = plane_info->src_x;
	mvvs->src_y = plane_info->src_y;
	mvvs->src_w = plane_info->src_w;
	mvvs->src_h = plane_info->src_h;
	mvvs->dst_x = plane_info->dst_x;
	mvvs->dst_y = plane_info->dst_y;
	mvvs->dst_w = plane_info->dst_w;
	mvvs->dst_h = plane_info->dst_h;
	mvvs->byte_stride = plane_info->byte_stride;
	mvvs->phy_addr[0] = plane_info->phy_addr[0];
	mvvs->phy_addr[1] = plane_info->phy_addr[1];

	mvvs->pixel_format = plane_info->pixel_format;
	mvvs->fb_size[0] = plane_info->fb_size[0];
	mvvs->fb_size[1] = plane_info->fb_size[1];
	mvvs->vf = plane_info->vf;
	mvvs->is_uvm = plane_info->is_uvm;

	if (!video->video_path_reg) {
		kfifo_reset(&video->ready_q);
		kfifo_reset(&video->free_q);
		kfifo_reset(&video->display_q);
		for (i = 0; i < BUFFER_NUM; i++)
			kfifo_put(&video->free_q, &video->vframe[i]);
		vf_reg_provider(&video->vprov);
		vf_notify_receiver(video->base.name,
				   VFRAME_EVENT_PROVIDER_START, NULL);
		video->video_path_reg = 1;
	}
	return 0;
}

static void video_set_state(struct meson_vpu_block *vblk,
			    struct meson_vpu_block_state *state)
{
	struct vframe_s *vf = NULL, *dec_vf = NULL, *vfp = NULL;
	struct meson_vpu_video *video = to_video_block(vblk);
	struct meson_vpu_video_state *mvvs = to_video_state(state);
	u32 pixel_format, src_h, byte_stride, pic_w, pic_h;
	u32 recal_src_w, recal_src_h;
	u64 phy_addr, phy_addr2 = 0;

	DRM_DEBUG("%s", __func__);

	if (!vblk) {
		DRM_DEBUG("set_state break for NULL.\n");
		return;
	}

	src_h = mvvs->src_h;
	byte_stride = mvvs->byte_stride;
	phy_addr = mvvs->phy_addr[0];
	pixel_format = mvvs->pixel_format;
	DRM_DEBUG("%s %d-%d-%llx", __func__, src_h, pixel_format, phy_addr);

	if (mvvs->is_uvm) {
		dec_vf = mvvs->vf;
		vf = mvvs->vf;
		DRM_DEBUG("dec vf, %s, flag-%u, type-%u, %u, %u, %u, %u\n",
			  __func__, dec_vf->flag, dec_vf->type,
			  dec_vf->compWidth, dec_vf->compHeight,
			  dec_vf->width, dec_vf->height);
		if (vf->vf_ext && (vf->flag & VFRAME_FLAG_CONTAIN_POST_FRAME)) {
			vf = mvvs->vf->vf_ext;
			DRM_DEBUG("DI vf, %s, flag-%u, type-%u, %u,%u,%u,%u\n",
				  __func__, vf->flag, vf->type,
				  vf->compWidth, vf->compHeight,
				  vf->width, vf->height);
		}
		vf->axis[0] = mvvs->dst_x;
		vf->axis[1] = mvvs->dst_y;
		vf->axis[2] = mvvs->dst_x + mvvs->dst_w - 1;
		vf->axis[3] = mvvs->dst_y + mvvs->dst_h - 1;
		vf->crop[0] = mvvs->src_y;/*crop top*/
		vf->crop[1] = mvvs->src_x;/*crop left*/

		/*
		 *if video_axis_zoom = 1, means the video anix is
		 *set by westeros
		 */
		if (video_axis_zoom != -1) {
			if (dec_vf->type & VIDTYPE_COMPRESS) {
				pic_w = dec_vf->compWidth;
				pic_h = dec_vf->compHeight;
				recal_src_w = mvvs->src_w *
							pic_w / dec_vf->width;
				recal_src_h = mvvs->src_h *
							pic_h / dec_vf->height;
				vf->crop[0] = mvvs->src_y *
							pic_h / dec_vf->height;
				vf->crop[1] = mvvs->src_x *
							pic_w / dec_vf->width;
			} else {
				pic_w = dec_vf->width;
				pic_h = dec_vf->height;
				recal_src_w = mvvs->src_w;
				recal_src_h = mvvs->src_h;
			}
		} else {
			recal_src_w = mvvs->src_w;
			recal_src_h = mvvs->src_h;
			if (dec_vf->type & VIDTYPE_COMPRESS) {
				pic_w = dec_vf->compWidth;
				pic_h = dec_vf->compHeight;
			} else {
				pic_w = dec_vf->width;
				pic_h = dec_vf->height;
			}
		}

		if ((pic_w == 0 || pic_h == 0) && dec_vf->vf_ext) {
			vfp = dec_vf->vf_ext;
			pic_w = vfp->width;
			pic_h = vfp->height;
		}

		/*crop bottow*/
		if (pic_h > recal_src_h + vf->crop[0])
			vf->crop[2] = pic_h - recal_src_h - vf->crop[0];
		else
			vf->crop[2] = 0;

		/*crop right*/
		if (pic_w > recal_src_w + vf->crop[1])
			vf->crop[3] = pic_w - recal_src_w - vf->crop[1];
		else
			vf->crop[3] = 0;
		DRM_DEBUG("vf-crop:%u, %u, %u, %u\n",
				pic_w, pic_h, vf->crop[2], vf->crop[3]);
		vf->flag |= VFRAME_FLAG_VIDEO_DRM;
		if (!kfifo_put(&video->ready_q, vf))
			DRM_INFO("ready_q is full!\n");
	} else {
		if (pixel_format == DRM_FORMAT_NV12 ||
		    pixel_format == DRM_FORMAT_NV21) {
			if (!mvvs->phy_addr[1])
				phy_addr2 = phy_addr + byte_stride * src_h;
			else
				phy_addr2 = mvvs->phy_addr[1];
		}

		if (kfifo_get(&video->free_q, &vf) && vf) {
			memset(vf, 0, sizeof(struct vframe_s));
			vf->width = mvvs->src_w;
			vf->height = mvvs->src_h;
			vf->source_type = VFRAME_SOURCE_TYPE_OTHERS;
			vf->source_mode = VFRAME_SOURCE_MODE_OTHERS;
			vf->bitdepth = BITDEPTH_Y8 | BITDEPTH_U8 | BITDEPTH_V8;
			vf->type = video_type_get(pixel_format);
			vf->axis[0] = mvvs->dst_x;
			vf->axis[1] = mvvs->dst_y;
			vf->axis[2] = mvvs->dst_x + mvvs->dst_w - 1;
			vf->axis[3] = mvvs->dst_y + mvvs->dst_h - 1;
			vf->crop[0] = mvvs->src_y;/*crop top*/
			vf->crop[1] = mvvs->src_x;/*crop left*/
			/*vf->width is from mvvs->src_w which is the
			 *valid content so the crop of bottom and right
			 *could be 0
			 */
			vf->crop[2] = 0;/*crop bottow*/
			vf->crop[3] = 0;/*crop right*/
			vf->flag |= VFRAME_FLAG_VIDEO_DRM;
			/*need sync with vpp*/
			vf->canvas0Addr = (u32)-1;
			/*Todo: if canvas0_config.endian = 1
			 *supprot little endian is okay,could be removed.
			 */
			vf->flag |= VFRAME_FLAG_VIDEO_LINEAR;
			vf->plane_num = 1;
			vf->canvas0_config[0].phy_addr = phy_addr;
			vf->canvas0_config[0].width = byte_stride;
			vf->canvas0_config[0].height = src_h;
			vf->canvas0_config[0].block_mode =
				CANVAS_BLKMODE_LINEAR;
			/*big endian default support*/
			vf->canvas0_config[0].endian = 0;
			if (pixel_format == DRM_FORMAT_NV12 ||
			    pixel_format == DRM_FORMAT_NV21) {
				vf->plane_num = 2;
				vf->canvas0_config[1].phy_addr = phy_addr2;
				vf->canvas0_config[1].width = byte_stride;
				vf->canvas0_config[1].height = src_h / 2;
				vf->canvas0_config[1].block_mode =
					CANVAS_BLKMODE_LINEAR;
				/*big endian default support*/
				vf->canvas0_config[1].endian = 0;
			}
			DRM_DEBUG("vframe info:type(0x%x),plane_num=%d\n",
				  vf->type, vf->plane_num);
			if (!kfifo_put(&video->ready_q, vf))
				DRM_INFO("ready_q is full!\n");
		} else {
			DRM_INFO("free_q get fail!");
		}
	}
	DRM_DEBUG("plane_index=%d,HW-video=%d, byte_stride=%d\n",
		  mvvs->plane_index, vblk->index, byte_stride);
	DRM_DEBUG("phy_addr=0x%pa,phy_addr2=0x%pa\n",
		  &phy_addr, &phy_addr2);
	DRM_DEBUG("%s set_state done.\n", video->base.name);
}

static void video_hw_enable(struct meson_vpu_block *vblk)
{
	struct meson_vpu_video *video = to_video_block(vblk);

	if (!video) {
		DRM_DEBUG("enable break for NULL.\n");
		return;
	}
	if (!video->video_enabled) {
		set_video_enabled(1, vblk->index);
		video->video_enabled = 1;
	}
	DRM_DEBUG("%s enable done.\n", video->base.name);
}

static void video_hw_disable(struct meson_vpu_block *vblk)
{
	struct meson_vpu_video *video = to_video_block(vblk);

	if (!video) {
		DRM_DEBUG("disable break for NULL.\n");
		return;
	}

	if (video->video_enabled) {
		set_video_enabled(0, vblk->index);
		video->video_enabled = 0;
	}

	if (video->video_path_reg) {
		vf_unreg_provider(&video->vprov);
		video->video_path_reg = 0;
	}
	DRM_DEBUG("%s disable done.\n", video->base.name);
}

static void video_dump_register(struct meson_vpu_block *vblk,
				struct seq_file *seq)
{
}

static void video_hw_init(struct meson_vpu_block *vblk)
{
	struct meson_vpu_video *video = to_video_block(vblk);
	int i;

	if (!vblk || !video) {
		DRM_DEBUG("%s break for NULL.\n", __func__);
		return;
	}
	INIT_KFIFO(video->ready_q);
	INIT_KFIFO(video->free_q);
	INIT_KFIFO(video->display_q);
	kfifo_reset(&video->ready_q);
	kfifo_reset(&video->free_q);
	kfifo_reset(&video->display_q);
	for (i = 0; i < BUFFER_NUM; i++)
		kfifo_put(&video->free_q, &video->vframe[i]);
	if (vblk->id == VIDEO1_BLOCK)
		snprintf(video->vfm_map_chain, VP_MAP_STRUCT_SIZE,
			 "%s %s", video->base.name,
			 "amvideo");
	else if (vblk->id == VIDEO2_BLOCK)
		snprintf(video->vfm_map_chain, VP_MAP_STRUCT_SIZE,
			 "%s %s", video->base.name,
			 "videopip");
	else
		DRM_DEBUG("unsupported block id %d\n", vblk->id);
	snprintf(video->vfm_map_id, VP_MAP_STRUCT_SIZE,
		 "video-map-%d", vblk->index);
	vfm_map_add(video->vfm_map_id, video->vfm_map_chain);
	vf_provider_init(&video->vprov,
			 video->base.name,
			 &vp_vf_ops, video);
	DRM_DEBUG("%s:%s done.\n", __func__, video->base.name);
}

struct meson_vpu_block_ops video_ops = {
	.check_state = video_check_state,
	.update_state = video_set_state,
	.enable = video_hw_enable,
	.disable = video_hw_disable,
	.dump_register = video_dump_register,
	.init = video_hw_init,
};
