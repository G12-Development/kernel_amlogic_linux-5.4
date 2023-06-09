/* SPDX-License-Identifier: (GPL-2.0+ OR MIT) */
/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 */

#ifndef __MESON_HDMI_H__
#define __MESON_HDMI_H__

#include "meson_drv.h"
#include <drm/drm_connector.h>
#include <drm/drm_encoder.h>
#include <linux/amlogic/media/vout/hdmi_tx/meson_drm_hdmitx.h>
#include <media/cec-notifier.h>

enum {
	HDCP_STATE_START = 0,
	HDCP_STATE_SUCCESS,
	HDCP_STATE_FAIL,
	HDCP_STATE_STOP,
	HDCP_STATE_DISCONNECT,
};

struct hdmitx_color_attr {
	int colorformat;
	int bitdepth;
};

struct am_hdmi_tx {
	struct meson_connector base;
	struct drm_encoder encoder;

	/*drm current hdmitx attr color-subsample(format)/colordepth*/
	struct hdmitx_color_attr color_attr;

	/*drm request content type.*/
	int hdcp_request_content_type;
	int hdcp_request_content_protection;
	/*current hdcp running mode, HDCP_NULL means hdcp disabled.*/
	int hdcp_mode;
	/*hdcp auth result, HDCP_AUTH_UNKNOWN means havenot finished auth.*/
	int hdcp_state;

	/*TODO: android compatible, remove later*/
	bool android_path;

	/*amlogic property: force hdmitx update
	 *colorspace/colordepth from sysfs.
	 */
	struct drm_property *update_attr_prop;
#ifdef CONFIG_CEC_NOTIFIER
	struct cec_notifier	*cec_notifier;
#endif
};

struct am_hdmitx_connector_state {
	struct drm_connector_state base;
	bool update : 1;
};

#define to_am_hdmitx_connector_state(x)	container_of(x, struct am_hdmitx_connector_state, base)
#define meson_connector_to_am_hdmi(x)	container_of(x, struct am_hdmi_tx, base)
#define connector_to_am_hdmi(x) \
	container_of(connector_to_meson_connector(x), struct am_hdmi_tx, base)
#define encoder_to_am_hdmi(x)	container_of(x, struct am_hdmi_tx, encoder)
#endif
