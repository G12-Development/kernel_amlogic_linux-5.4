// SPDX-License-Identifier: (GPL-2.0+ OR MIT)
/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 */

#include <linux/version.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/mm.h>
#include <linux/major.h>
#include <linux/platform_device.h>
#include <linux/pinctrl/devinfo.h>
#include <linux/pinctrl/consumer.h>
#include <linux/mutex.h>
#include <linux/cdev.h>
#include <linux/ctype.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/input.h>
#include <linux/irq.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/reboot.h>
#include <linux/i2c.h>
#include <linux/miscdevice.h>
//#include <linux/amlogic/cpu_version.h>
#include <linux/amlogic/media/vout/vinfo.h>
#include <linux/amlogic/media/vout/vout_notify.h>
#if IS_ENABLED(CONFIG_AMLOGIC_SND_SOC)
#include <linux/amlogic/media/sound/aout_notify.h>
#endif
#include <linux/amlogic/media/vout/hdmi_tx21/hdmi_info_global.h>
#include <linux/amlogic/media/vout/hdmi_tx21/hdmi_tx_ddc.h>
#include <linux/amlogic/media/vout/hdmi_tx21/hdmi_tx_module.h>
#include <linux/amlogic/media/vout/hdmi_tx21/hdmi_config.h>
#include <linux/amlogic/media/vout/hdmi_tx_ext.h>
#include "hdmi_tx_ext.h"
#include "hdmi_tx.h"

#include <linux/amlogic/gki_module.h>

#define HDMI_TX_COUNT 32
#define HDMI_TX_POOL_NUM  6
#define HDMI_TX_RESOURCE_NUM 4
#define HDMI_TX_PWR_CTRL_NUM	6

static struct class *hdmitx_class;
static int set_disp_mode_auto(void);
static void hdmitx_get_edid(struct hdmitx_dev *hdev);
static void hdmitx_set_drm_pkt(struct master_display_info_s *data);
static void hdmitx_set_vsif_pkt(enum eotf_type type, enum mode_type
	tunnel_mode, struct dv_vsif_para *data, bool signal_sdr);
static void hdmitx_set_hdr10plus_pkt(u32 flag,
				     struct hdr10plus_para *data);
static void hdmitx_set_emp_pkt(u8 *data,
			       u32 type,
			       u32 size);
static int check_fbc_special(u8 *edid_dat);
static void hdmitx_fmt_attr(struct hdmitx_dev *hdev);
static void clear_rx_vinfo(struct hdmitx_dev *hdev);
static void edidinfo_attach_to_vinfo(struct hdmitx_dev *hdev);
static void edidinfo_detach_to_vinfo(struct hdmitx_dev *hdev);
static void update_current_para(struct hdmitx_dev *hdev);

#ifdef CONFIG_AMLOGIC_VOUT_SERVE
static struct vinfo_s *hdmitx_get_current_vinfo(void *data);
#else
static struct vinfo_s *hdmitx_get_current_vinfo(void *data)
{
	return NULL;
}
#endif

#ifdef CONFIG_OF
static struct amhdmitx_data_s amhdmitx_data_t7 = {
	.chip_type = MESON_CPU_ID_T7,
	.chip_name = "t7",
};

static const struct of_device_id meson_amhdmitx_of_match[] = {
	{
		.compatible	 = "amlogic, amhdmitx-t7",
		.data = &amhdmitx_data_t7,
	},
	{},
};
#else
#define meson_amhdmitx_dt_match NULL
#endif

static DEFINE_MUTEX(setclk_mutex);
static DEFINE_MUTEX(getedid_mutex);

static struct hdmitx_dev hdmitx21_device = {
	.frac_rate_policy = 1,
};

struct hdmitx_dev *get_hdmitx21_device(void)
{
	return &hdmitx21_device;
}
EXPORT_SYMBOL(get_hdmitx21_device);

int get_hdmitx21_init(void)
{
	return hdmitx21_device.hdmi_init;
}

struct vsdb_phyaddr *get_hdmitx21_phy_addr(void)
{
	return &hdmitx21_device.hdmi_info.vsdb_phy_addr;
}

static const struct dv_info dv_dummy;
static int log21_level;

static struct vout_device_s hdmitx_vdev = {
	.dv_info = &hdmitx21_device.rxcap.dv_info,
	.fresh_tx_hdr_pkt = hdmitx_set_drm_pkt,
	.fresh_tx_vsif_pkt = hdmitx_set_vsif_pkt,
	.fresh_tx_hdr10plus_pkt = hdmitx_set_hdr10plus_pkt,
	.fresh_tx_emp_pkt = hdmitx_set_emp_pkt,
	.get_attr = get21_attr,
	.setup_attr = setup21_attr,
	.video_mute = hdmitx21_video_mute_op,
};

static struct hdmitx_uevent hdmi_events[] = {
	{
		.type = HDMITX_HPD_EVENT,
		.env = "hdmitx_hpd=",
	},
	{
		.type = HDMITX_HDCP_EVENT,
		.env = "hdmitx_hdcp=",
	},
	{
		.type = HDMITX_AUDIO_EVENT,
		.env = "hdmitx_audio=",
	},
	{
		.type = HDMITX_HDR_EVENT,
		.env = "hdmitx_hdr=",
	},
	{
		.type = HDMITX_RXSENSE_EVENT,
		.env = "hdmitx_rxsense=",
	},
	{
		.type = HDMITX_CEDST_EVENT,
		.env = "hdmitx_cedst=",
	},
	{ /* end of hdmi_events[] */
		.type = HDMITX_NONE_EVENT,
	},
};

int hdmitx21_set_uevent(enum hdmitx_event type, int val)
{
	char env[MAX_UEVENT_LEN];
	struct hdmitx_uevent *event = hdmi_events;
	struct hdmitx_dev *hdev = get_hdmitx21_device();
	char *envp[2];
	int ret = -1;

	for (event = hdmi_events; event->type != HDMITX_NONE_EVENT; event++) {
		if (type == event->type)
			break;
	}
	if (event->type == HDMITX_NONE_EVENT)
		return ret;
	if (event->state == val)
		return ret;

	event->state = val;
	memset(env, 0, sizeof(env));
	envp[0] = env;
	envp[1] = NULL;
	snprintf(env, MAX_UEVENT_LEN, "%s%d", event->env, val);

	ret = kobject_uevent_env(&hdev->hdtx_dev->kobj, KOBJ_CHANGE, envp);
	/* pr_info("%s[%d] %s %d\n", __func__, __LINE__, env, ret); */
	return ret;
}

/* There are 3 callback functions for front HDR/DV/HDR10+ modules to notify
 * hdmi drivers to send out related HDMI infoframe
 * hdmitx_set_drm_pkt() is for HDR 2084 SMPTE, HLG, etc.
 * hdmitx_set_vsif_pkt() is for DV
 * hdmitx_set_hdr10plus_pkt is for HDR10+
 * Front modules may call the 2nd, and next call the 1st, and the realted flags
 * are remained the same. So, add hdr_status_pos and place it in the above 3
 * functions to record the position.
 */
static int hdr_status_pos;

static inline void hdmitx_notify_hpd(int hpd)
{
	if (hpd)
		hdmitx21_event_notify(HDMITX_PLUG, NULL);
	else
		hdmitx21_event_notify(HDMITX_UNPLUG, NULL);
}

#ifdef CONFIG_AMLOGIC_LEGACY_EARLY_SUSPEND
#include <linux/amlogic/pm.h>
static void hdmitx_early_suspend(struct early_suspend *h)
{
	struct hdmitx_dev *hdev = (struct hdmitx_dev *)h->param;

	hdev->ready = 0;
	hdev->hpd_lock = 1;
	hdev->hwop.cntlmisc(hdev, MISC_SUSFLAG, 1);
	usleep_range(10000, 10010);
	hdev->hwop.cntlmisc(hdev, MISC_AVMUTE_OP, SET_AVMUTE);
	usleep_range(10000, 10010);
	pr_info("HDMITX: Early Suspend\n");
	hdev->hdcp_mode = 0;
	hdcp_mode_set(0);
	hdev->hwop.cntl(hdev, HDMITX_EARLY_SUSPEND_RESUME_CNTL,
		HDMITX_EARLY_SUSPEND);
	hdev->cur_VIC = HDMI_0_UNKNOWN;
	hdev->output_blank_flag = 0;
	hdmitx_set_vsif_pkt(0, 0, NULL, true);
	hdmitx_set_hdr10plus_pkt(0, NULL);
	clear_rx_vinfo(hdev);
	hdmitx21_edid_clear(hdev);
	hdmitx21_edid_ram_buffer_clear(hdev);
	edidinfo_detach_to_vinfo(hdev);
	hdev->hwop.cntlconfig(hdev, CONF_CLR_AVI_PACKET, 0);
	hdev->hwop.cntlconfig(hdev, CONF_CLR_VSDB_PACKET, 0);
}

static int hdmitx_is_hdmi_vmode(char *mode_name)
{
	enum hdmi_vic vic = hdmitx21_edid_vic_tab_map_vic(mode_name);

	if (vic == HDMI_0_UNKNOWN)
		return 0;

	return 1;
}

static void hdmitx_late_resume(struct early_suspend *h)
{
	const struct vinfo_s *info = hdmitx_get_current_vinfo(NULL);
	struct hdmitx_dev *hdev = (struct hdmitx_dev *)h->param;

	if (info && info->name && (hdmitx_is_hdmi_vmode(info->name) == 1))
		hdev->hwop.cntlmisc(hdev, MISC_HPLL_FAKE, 0);

	hdev->hpd_lock = 0;

	/* update status for hpd and switch/state */
	hdev->hpd_state = !!(hdev->hwop.cntlmisc(hdev, MISC_HPD_GPI_ST, 0));
	if (hdev->hpd_state)
		hdev->already_used = 1;

	pr_info("hdmitx hpd state: %d\n", hdev->hpd_state);
	hdmitx_notify_hpd(hdev->hpd_state);

	/*force to get EDID after resume for Amplifier Power case*/
	if (hdev->hpd_state)
		hdmitx_get_edid(hdev);

	hdev->hwop.cntlconfig(hdev, CONF_AUDIO_MUTE_OP, AUDIO_MUTE);
	set_disp_mode_auto();

	hdmitx21_set_uevent(HDMITX_HPD_EVENT, hdev->hpd_state);

	hdmitx21_set_uevent(HDMITX_AUDIO_EVENT, hdev->hpd_state);
	pr_info("%s: late resume module %d\n", DEVICE_NAME, __LINE__);
	hdev->hwop.cntl(hdev, HDMITX_EARLY_SUSPEND_RESUME_CNTL,
		HDMITX_LATE_RESUME);
	hdev->hwop.cntlmisc(hdev, MISC_SUSFLAG, 0);
	pr_info("HDMITX: Late Resume\n");
}

/* Set avmute_set signal to HDMIRX */
static int hdmitx_reboot_notifier(struct notifier_block *nb,
				  unsigned long action, void *data)
{
	struct hdmitx_dev *hdev = container_of(nb, struct hdmitx_dev, nb);

	hdev->ready = 0;
	hdev->hwop.cntlmisc(hdev, MISC_AVMUTE_OP, SET_AVMUTE);
	usleep_range(10000, 10010);
	hdcp_mode_set(0);
	hdev->hwop.cntlmisc(hdev, MISC_TMDS_PHY_OP, TMDS_PHY_DISABLE);
	hdev->hwop.cntl(hdev, HDMITX_EARLY_SUSPEND_RESUME_CNTL,
		HDMITX_EARLY_SUSPEND);

	return NOTIFY_OK;
}

static struct early_suspend hdmitx_early_suspend_handler = {
	.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN - 10,
	.suspend = hdmitx_early_suspend,
	.resume = hdmitx_late_resume,
	.param = &hdmitx21_device,
};
#endif

#define INIT_FLAG_VDACOFF		0x1
	/* unplug powerdown */
#define INIT_FLAG_POWERDOWN	 0x2

#define INIT_FLAG_NOT_LOAD 0x80

static u8 init_flag;
#undef DISABLE_AUDIO

int get21_cur_vout_index(void)
/*
 * return value: 1, vout; 2, vout2;
 */
{
	int vout_index = 1;
	return vout_index;
}

static  int  set_disp_mode(const char *mode)
{
	int ret =  -1;
	enum hdmi_vic vic;
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	vic = hdmitx21_edid_get_VIC(hdev, mode, 1);
	if (strncmp(mode, "2160p30hz", strlen("2160p30hz")) == 0)
		vic = HDMI_95_3840x2160p30_16x9;
	else if (strncmp(mode, "2160p25hz", strlen("2160p25hz")) == 0)
		vic = HDMI_94_3840x2160p25_16x9;
	else if (strncmp(mode, "2160p24hz", strlen("2160p24hz")) == 0)
		vic = HDMI_93_3840x2160p24_16x9;
	else if (strncmp(mode, "smpte24hz", strlen("smpte24hz")) == 0)
		vic = HDMI_98_4096x2160p24_256x135;
	else
		;/* nothing */

	if (strncmp(mode, "1080p60hz", strlen("1080p60hz")) == 0)
		vic = HDMI_16_1920x1080p60_16x9;
	if (strncmp(mode, "1080p50hz", strlen("1080p50hz")) == 0)
		vic = HDMI_31_1920x1080p50_16x9;

	if (vic != HDMI_0_UNKNOWN) {
		hdev->mux_hpd_if_pin_high_flag = 1;
		if (hdev->vic_count == 0) {
			if (hdev->unplug_powerdown)
				return 0;
		}
	}

	hdev->cur_VIC = HDMI_0_UNKNOWN;
	ret = hdmitx21_set_display(hdev, vic);
	if (ret >= 0) {
		hdev->hwop.cntl(hdev, HDMITX_AVMUTE_CNTL, AVMUTE_CLEAR);
		hdev->cur_VIC = vic;
		hdev->audio_param_update_flag = 1;
	}

	if (hdev->cur_VIC == HDMI_0_UNKNOWN) {
		if (hdev->hpdmode == 2) {
			/* edid will be read again when hpd is muxed and
			 * it is high
			 */
			hdmitx21_edid_clear(hdev);
			hdev->mux_hpd_if_pin_high_flag = 0;
		}
		if (hdev->hwop.cntl) {
			hdev->hwop.cntl(hdev,
				HDMITX_HWCMD_TURNOFF_HDMIHW,
				(hdev->hpdmode == 2) ? 1 : 0);
		}
	}
	return ret;
}

static void hdmi_physical_size_update(struct hdmitx_dev *hdev)
{
	u32 width, height;
	struct vinfo_s *info = NULL;

	info = hdmitx_get_current_vinfo(NULL);
	if (!info || !info->name) {
		pr_info("cann't get valid mode\n");
		return;
	}

	if (info->mode == VMODE_HDMI) {
		width = hdev->rxcap.physical_width;
		height = hdev->rxcap.physical_height;
		if (width == 0 || height == 0) {
			info->screen_real_width = info->aspect_ratio_num;
			info->screen_real_height = info->aspect_ratio_den;
		} else {
			info->screen_real_width = width;
			info->screen_real_height = height;
		}
		pr_info("update physical size: %d %d\n",
			info->screen_real_width, info->screen_real_height);
	}
}

static void hdrinfo_to_vinfo(struct vinfo_s *info, struct hdmitx_dev *hdev)
{
	u32 i, j;
	/*static hdr*/
	info->hdr_info.hdr_support = (hdev->rxcap.hdr_sup_eotf_sdr << 0)
			| (hdev->rxcap.hdr_sup_eotf_hdr << 1)
			| (hdev->rxcap.hdr_sup_eotf_smpte_st_2084 << 2)
			| (hdev->rxcap.hdr_sup_eotf_hlg << 3);
	memcpy(info->hdr_info.rawdata, hdev->rxcap.hdr_rawdata, 7);
	/*dynamic hdr*/
	for (i = 0; i < 4; i++) {
		if (hdev->rxcap.hdr_dynamic_info[i].type == 0) {
			memset(&info->hdr_info.dynamic_info[i],
			 0, sizeof(struct hdr_dynamic));
			continue;
		}
		info->hdr_info.dynamic_info[i].type =
			hdev->rxcap.hdr_dynamic_info[i].type;
		info->hdr_info.dynamic_info[i].of_len =
			hdev->rxcap.hdr_dynamic_info[i].hd_len - 3;
		info->hdr_info.dynamic_info[i].support_flags =
			hdev->rxcap.hdr_dynamic_info[i].support_flags;

		for (j = 0; j < hdev->rxcap.hdr_dynamic_info[i].hd_len - 3; j++)
			info->hdr_info.dynamic_info[i].optional_fields[j] =
			hdev->rxcap.hdr_dynamic_info[i].optional_fields[j];
	}
	/*hdr 10+*/
	memcpy(&info->hdr_info.hdr10plus_info,
	       &hdev->rxcap.hdr10plus_info, sizeof(struct hdr10_plus_info));

	info->hdr_info.colorimetry_support =
		hdev->rxcap.colorimetry_data;
	info->hdr_info.lumi_max = hdev->rxcap.hdr_lum_max;
	info->hdr_info.lumi_avg = hdev->rxcap.hdr_lum_avg;
	info->hdr_info.lumi_min = hdev->rxcap.hdr_lum_min;
	pr_info("update rx hdr info %x\n",
		info->hdr_info.hdr_support);
}

static void rxlatency_to_vinfo(struct vinfo_s *info, struct rx_cap *rx)
{
	if (!info || !rx)
		return;
	info->rx_latency.vLatency = rx->vLatency;
	info->rx_latency.aLatency = rx->aLatency;
	info->rx_latency.i_vLatency = rx->i_vLatency;
	info->rx_latency.i_aLatency = rx->i_aLatency;
}

static void edidinfo_attach_to_vinfo(struct hdmitx_dev *hdev)
{
	struct vinfo_s *info = NULL;

	/* get current vinfo */
	info = hdmitx_get_current_vinfo(NULL);
	if (!info || !info->name)
		return;

	if (strncmp(info->name, "480cvbs", 7) == 0 ||
	    strncmp(info->name, "576cvbs", 7) == 0 ||
	    strncmp(info->name, "null", 4) == 0)
		return;

	mutex_lock(&getedid_mutex);
	hdrinfo_to_vinfo(info, hdev);
	if (hdev->para && hdev->para->cd == COLORDEPTH_24B)
		memset(&info->hdr_info, 0, sizeof(struct hdr_info));
	rxlatency_to_vinfo(info, &hdev->rxcap);
	hdmitx_vdev.dv_info = &hdev->rxcap.dv_info;
	mutex_unlock(&getedid_mutex);
}

static void edidinfo_detach_to_vinfo(struct hdmitx_dev *hdev)
{
	struct vinfo_s *info = NULL;

	/* get current vinfo */
	info = hdmitx_get_current_vinfo(NULL);
	if (!info || !info->name)
		return;

	edidinfo_attach_to_vinfo(hdev);
	hdmitx_vdev.dv_info = &dv_dummy;
}

static int set_disp_mode_auto(void)
{
	int ret =  -1;

	struct vinfo_s *info = NULL;
	struct hdmitx_dev *hdev = get_hdmitx21_device();
	struct hdmi_format_para *para = NULL;
	u8 mode[32];
	enum hdmi_vic vic = HDMI_0_UNKNOWN;

	memset(mode, 0, sizeof(mode));
	hdev->ready = 0;

	/* get current vinfo */
	info = hdmitx_get_current_vinfo(NULL);
	if (!info || !info->name)
		return -1;
	pr_info("get current mode: %s\n", info->name);
	/*update hdmi checksum to vout*/
	memcpy(info->hdmichecksum, hdev->rxcap.chksum, 10);

	hdmi_physical_size_update(hdev);

	strncpy(mode, info->name, sizeof(mode));
	mode[31] = '\0';
	if (strstr(mode, "fp")) {
		int i = 0;

		for (; mode[i]; i++) {
			if ((mode[i] == 'f') && (mode[i + 1] == 'p')) {
				/* skip "f", 1080fp60hz -> 1080p60hz */
				do {
					mode[i] = mode[i + 1];
					i++;
				} while (mode[i]);
				break;
			}
		}
	}

	para = hdmitx21_get_fmtpara(mode, hdev->fmt_attr);
	if (!para) {
		pr_info("%s[%d] %s %s\n", __func__, __LINE__, mode,
			hdev->fmt_attr);
		return -1;
	}
	pr_info("setting hdmi mode %s %s\n", mode, hdev->fmt_attr);
	pr_info("cd/cs/cr: %d/%d/%d\n", para->cd, para->cs, para->cr);
	hdev->para = para;
	vic = hdmitx21_edid_get_VIC(hdev, mode, 1);
	if (strncmp(info->name, "2160p30hz", strlen("2160p30hz")) == 0) {
		vic = HDMI_95_3840x2160p30_16x9;
	} else if (strncmp(info->name, "2160p25hz",
		strlen("2160p25hz")) == 0) {
		vic = HDMI_94_3840x2160p25_16x9;
	} else if (strncmp(info->name, "2160p24hz",
		strlen("2160p24hz")) == 0) {
		vic = HDMI_93_3840x2160p24_16x9;
	} else if (strncmp(info->name, "smpte24hz",
		strlen("smpte24hz")) == 0) {
		vic = HDMI_98_4096x2160p24_256x135;
	} else {
	/* nothing */
	}

	hdev->cur_VIC = HDMI_0_UNKNOWN;
/* if vic is HDMI_0_UNKNOWN, hdmitx21_set_display will disable HDMI */
	ret = hdmitx21_set_display(hdev, vic);

	if (ret >= 0) {
		hdev->hwop.cntl(hdev, HDMITX_AVMUTE_CNTL, AVMUTE_CLEAR);
		hdev->cur_VIC = vic;
		hdev->audio_param_update_flag = 1;
	}
	if (hdev->cur_VIC == HDMI_0_UNKNOWN) {
		if (hdev->hpdmode == 2) {
			/* edid will be read again when hpd is muxed
			 * and it is high
			 */
			hdmitx21_edid_clear(hdev);
			hdev->mux_hpd_if_pin_high_flag = 0;
		}
		/* If current display is NOT panel, needn't TURNOFF_HDMIHW */
		if (strncmp(mode, "panel", 5) == 0) {
			hdev->hwop.cntl(hdev, HDMITX_HWCMD_TURNOFF_HDMIHW,
				(hdev->hpdmode == 2) ? 1 : 0);
		}
	}
	hdmitx21_set_audio(hdev, &hdev->cur_audio_param);
	if (hdev->cedst_policy) {
		cancel_delayed_work(&hdev->work_cedst);
		queue_delayed_work(hdev->cedst_wq, &hdev->work_cedst, 0);
	}
	hdev->output_blank_flag = 1;
	hdev->ready = 1;
	edidinfo_attach_to_vinfo(hdev);
	if (get_hdcp2_lstore() & is_rx_hdcp2ver()) {
		hdev->hdcp_mode = 2;
		hdcp_mode_set(2);
	} else {
		if (get_hdcp1_lstore()) {
			hdev->hdcp_mode = 1;
			hdcp_mode_set(1);
		} else {
			hdev->hdcp_mode = 0;
		}
	}
	return ret;
}

/*disp_mode attr*/
static ssize_t disp_mode_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	int pos = 0;
	int i = 0;
	struct hdmitx_dev *hdev = get_hdmitx21_device();
	struct hdmi_format_para *para = hdev->para;
	struct hdmi_timing *timing = &para->timing;
	struct vinfo_s *vinfo = &para->hdmitx_vinfo;

	if (!para)
		return pos;

	pos += snprintf(buf + pos, PAGE_SIZE, "cd/cs/cr: %d/%d/%d\n", para->cd,
		para->cs, para->cr);
	pos += snprintf(buf + pos, PAGE_SIZE, "scramble/tmds_clk_div40: %d/%d\n",
		para->scrambler_en, para->tmds_clk_div40);
	pos += snprintf(buf + pos, PAGE_SIZE, "tmds_clk: %d\n", para->tmds_clk);
	pos += snprintf(buf + pos, PAGE_SIZE, "vic: %d\n", timing->vic);
	pos += snprintf(buf + pos, PAGE_SIZE, "name: %s\n", timing->name);
	if (timing->sname)
		pos += snprintf(buf + pos, PAGE_SIZE, "sname: %s\n",
			timing->sname);
	pos += snprintf(buf + pos, PAGE_SIZE, "pi_mode: %c\n",
		timing->pi_mode ? 'P' : 'I');
	pos += snprintf(buf + pos, PAGE_SIZE, "h/v_freq: %d/%d\n",
		timing->h_freq, timing->v_freq);
	pos += snprintf(buf + pos, PAGE_SIZE, "pixel_freq: %d\n",
		timing->pixel_freq);
	pos += snprintf(buf + pos, PAGE_SIZE, "h_total: %d\n", timing->h_total);
	pos += snprintf(buf + pos, PAGE_SIZE, "h_blank: %d\n", timing->h_blank);
	pos += snprintf(buf + pos, PAGE_SIZE, "h_front: %d\n", timing->h_front);
	pos += snprintf(buf + pos, PAGE_SIZE, "h_sync: %d\n", timing->h_sync);
	pos += snprintf(buf + pos, PAGE_SIZE, "h_back: %d\n", timing->h_back);
	pos += snprintf(buf + pos, PAGE_SIZE, "h_active: %d\n", timing->h_active);
	pos += snprintf(buf + pos, PAGE_SIZE, "v_total: %d\n", timing->v_total);
	pos += snprintf(buf + pos, PAGE_SIZE, "v_blank: %d\n", timing->v_blank);
	pos += snprintf(buf + pos, PAGE_SIZE, "v_front: %d\n", timing->v_front);
	pos += snprintf(buf + pos, PAGE_SIZE, "v_sync: %d\n", timing->v_sync);
	pos += snprintf(buf + pos, PAGE_SIZE, "v_back: %d\n", timing->v_back);
	pos += snprintf(buf + pos, PAGE_SIZE, "v_active: %d\n", timing->v_active);
	pos += snprintf(buf + pos, PAGE_SIZE, "v_sync_ln: %d\n", timing->v_sync_ln);
	pos += snprintf(buf + pos, PAGE_SIZE, "h/v_pol: %d/%d\n", timing->h_pol, timing->v_pol);
	pos += snprintf(buf + pos, PAGE_SIZE, "h/v_pict: %d/%d\n", timing->h_pict, timing->v_pict);
	pos += snprintf(buf + pos, PAGE_SIZE, "h/v_pixel: %d/%d\n",
		timing->h_pixel, timing->v_pixel);
	pos += snprintf(buf + pos, PAGE_SIZE, "name: %s\n", vinfo->name);
	pos += snprintf(buf + pos, PAGE_SIZE, "mode: %d\n", vinfo->mode);
	pos += snprintf(buf + pos, PAGE_SIZE, "ext_name: %s\n", vinfo->ext_name);
	pos += snprintf(buf + pos, PAGE_SIZE, "frac: %d\n", vinfo->frac);
	pos += snprintf(buf + pos, PAGE_SIZE, "width/height: %d/%d\n", vinfo->width, vinfo->height);
	pos += snprintf(buf + pos, PAGE_SIZE, "field_height: %d\n", vinfo->field_height);
	pos += snprintf(buf + pos, PAGE_SIZE, "aspect_ratio_num/den: %d/%d\n",
		vinfo->aspect_ratio_num, vinfo->aspect_ratio_den);
	pos += snprintf(buf + pos, PAGE_SIZE, "screen_real_width/height: %d/%d\n",
		vinfo->screen_real_width, vinfo->screen_real_height);
	pos += snprintf(buf + pos, PAGE_SIZE, "sync_duration_num/den: %d/%d\n",
		vinfo->sync_duration_num, vinfo->sync_duration_den);
	pos += snprintf(buf + pos, PAGE_SIZE, "video_clk: %d\n", vinfo->video_clk);
	pos += snprintf(buf + pos, PAGE_SIZE, "h/vtotal: %d/%d\n", vinfo->htotal, vinfo->vtotal);
	pos += snprintf(buf + pos, PAGE_SIZE, "hdmichecksum:\n");
	for (i = 0; i < sizeof(vinfo->hdmichecksum); i++)
		pos += snprintf(buf + pos, PAGE_SIZE, "%02x", vinfo->hdmichecksum[i]);
	pos += snprintf(buf + pos, PAGE_SIZE, "\n");
	pos += snprintf(buf + pos, PAGE_SIZE, "info_3d: %d\n", vinfo->info_3d);
	pos += snprintf(buf + pos, PAGE_SIZE, "fr_adj_type: %d\n", vinfo->fr_adj_type);
	pos += snprintf(buf + pos, PAGE_SIZE, "viu_color_fmt: %d\n", vinfo->viu_color_fmt);
	pos += snprintf(buf + pos, PAGE_SIZE, "viu_mux: %d\n", vinfo->viu_mux);
	/* master_display_info / hdr_info / rx_latency */

	return pos;
}

static ssize_t disp_mode_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	set_disp_mode(buf);
	return count;
}

static ssize_t attr_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	int pos = 0;
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	if (!memcmp(hdev->fmt_attr, "default,", 7)) {
		memset(hdev->fmt_attr, 0,
		       sizeof(hdev->fmt_attr));
		hdmitx_fmt_attr(hdev);
	}
	pos += snprintf(buf + pos, PAGE_SIZE, "%s\n\r", hdev->fmt_attr);
	return pos;
}

static ssize_t attr_store(struct device *dev,
		   struct device_attribute *attr,
		   const char *buf, size_t count)
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	strncpy(hdev->fmt_attr, buf, sizeof(hdev->fmt_attr));
	hdev->fmt_attr[15] = '\0';
	if (!memcmp(hdev->fmt_attr, "rgb", 3))
		hdev->para->cs = HDMI_COLORSPACE_RGB;
	else if (!memcmp(hdev->fmt_attr, "422", 3))
		hdev->para->cs = HDMI_COLORSPACE_YUV422;
	else if (!memcmp(hdev->fmt_attr, "420", 3))
		hdev->para->cs = HDMI_COLORSPACE_YUV420;
	else
		hdev->para->cs = HDMI_COLORSPACE_YUV444;
	return count;
}

/*aud_mode attr*/

void setup21_attr(const char *buf)
{
	char attr[16] = {0};
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	memcpy(attr, buf, sizeof(attr));
	memcpy(hdev->fmt_attr, attr, sizeof(hdev->fmt_attr));
}

void get21_attr(char attr[16])
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	memcpy(attr, hdev->fmt_attr, sizeof(hdev->fmt_attr));
}

/*edid attr*/
static ssize_t edid_show(struct device *dev,
			 struct device_attribute *attr,
			 char *buf)
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	return hdmitx21_edid_dump(hdev, buf, PAGE_SIZE);
}

static int dump_edid_data(u32 type, char *path)
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();
	struct file *filp = NULL;
	loff_t pos = 0;
	char line[128] = {0};
	mm_segment_t old_fs = get_fs();
	u32 i = 0, j = 0, k = 0, size = 0, block_cnt = 0;
	u32 index = 0, tmp = 0;

	set_fs(KERNEL_DS);
	filp = filp_open(path, O_RDWR | O_CREAT, 0666);
	if (IS_ERR(filp)) {
		pr_info("[%s] failed to open/create file: |%s|\n",
			__func__, path);
		goto PROCESS_END;
	}

	block_cnt = hdev->EDID_buf[0x7e] + 1;
	if (hdev->EDID_buf[0x7e] != 0 &&
		hdev->EDID_buf[128 + 4] == 0xe2 &&
		hdev->EDID_buf[128 + 5] == 0x78)
		block_cnt = hdev->EDID_buf[128 + 6] + 1;
	if (type == 1) {
		/* dump as bin file*/
		size = vfs_write(filp, hdev->EDID_buf,
				 block_cnt * 128, &pos);
	} else if (type == 2) {
		/* dump as txt file*/

		for (i = 0; i < block_cnt; i++) {
			for (j = 0; j < 8; j++) {
				for (k = 0; k < 16; k++) {
					index = i * 128 + j * 16 + k;
					tmp = hdev->EDID_buf[index];
					snprintf((char *)&line[k * 6], 7,
						 "0x%02x, ",
						 tmp);
				}
				line[16 * 6 - 1] = '\n';
				line[16 * 6] = 0x0;
				pos = (i * 8 + j) * 16 * 6;
				size += vfs_write(filp, line, 16 * 6, &pos);
			}
		}
	}

	pr_info("[%s] write %d bytes to file %s\n", __func__, size, path);

	vfs_fsync(filp, 0);
	filp_close(filp, NULL);

PROCESS_END:
	set_fs(old_fs);
	return 0;
}

static int load_edid_data(u32 type, char *path)
{
	struct file *filp = NULL;
	loff_t pos = 0;
	mm_segment_t old_fs = get_fs();
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	struct kstat stat;
	u32 length = 0, max_len = EDID_MAX_BLOCK * 128;
	char *buf = NULL;

	set_fs(KERNEL_DS);

	filp = filp_open(path, O_RDONLY, 0444);
	if (IS_ERR(filp)) {
		pr_info("[%s] failed to open file: |%s|\n", __func__, path);
		goto PROCESS_END;
	}

	WARN_ON(vfs_stat(path, &stat));

	length = (stat.size > max_len) ? max_len : stat.size;

	buf = kmalloc(length, GFP_KERNEL);
	if (!buf)
		goto PROCESS_END;

	vfs_read(filp, buf, length, &pos);

	memcpy(hdev->EDID_buf, buf, length);

	kfree(buf);
	filp_close(filp, NULL);

	pr_info("[%s] %d bytes loaded from file %s\n", __func__, length, path);

	hdmitx21_edid_clear(hdev);
	hdmitx21_edid_parse(hdev);
	pr_info("[%s] new edid loaded!\n", __func__);

PROCESS_END:
	set_fs(old_fs);
	return 0;
}

static ssize_t edid_store(struct device *dev,
			  struct device_attribute *attr,
			  const char *buf, size_t count)
{
	u32 argn = 0;
	char *p = NULL, *para = NULL, *argv[8] = {NULL};
	u32 path_length = 0;
	u32 index = 0, tmp = 0;
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	p = kstrdup(buf, GFP_KERNEL);
	if (!p)
		return count;

	do {
		para = strsep(&p, " ");
		if (para) {
			argv[argn] = para;
			argn++;
			if (argn > 7)
				break;
		}
	} while (para);

	if (buf[0] == 'h') {
		int i;

		pr_info("EDID hash value:\n");
		for (i = 0; i < 20; i++)
			pr_info("%02x", hdev->EDID_hash[i]);
		pr_info("\n");
	}
	if (buf[0] == 'd') {
		int ii, jj;
		unsigned long block_idx;
		int ret;

		ret = kstrtoul(buf + 1, 16, &block_idx);
		if (block_idx < EDID_MAX_BLOCK) {
			for (ii = 0; ii < 8; ii++) {
				for (jj = 0; jj < 16; jj++) {
					index = block_idx * 128 + ii * 16 + jj;
					tmp = hdev->EDID_buf1[index];
					pr_info("%02x ", tmp);
				}
				pr_info("\n");
			}
		pr_info("\n");
	}
	}
	if (buf[0] == 'e') {
		int ii, jj;
		unsigned long block_idx;
		int ret;

		ret = kstrtoul(buf + 1, 16, &block_idx);
		if (block_idx < EDID_MAX_BLOCK) {
			for (ii = 0; ii < 8; ii++) {
				for (jj = 0; jj < 16; jj++) {
					index = block_idx * 128 + ii * 16 + jj;
					tmp = hdev->EDID_buf1[index];
					pr_info("%02x ", tmp);
				}
				pr_info("\n");
			}
			pr_info("\n");
		}
	}

	if (!strncmp(argv[0], "save", strlen("save"))) {
		u32 type = 0;

		if (argn != 3) {
			pr_info("[%s] cmd format: save bin/txt edid_file_path\n",
				__func__);
			goto PROCESS_END;
		}
		if (!strncmp(argv[1], "bin", strlen("bin")))
			type = 1;
		else if (!strncmp(argv[1], "txt", strlen("txt")))
			type = 2;

		if (type == 1 || type == 2) {
			/* clean '\n' from file path*/
			path_length = strlen(argv[2]);
			if (argv[2][path_length - 1] == '\n')
				argv[2][path_length - 1] = 0x0;

			dump_edid_data(type, argv[2]);
		}
	} else if (!strncmp(argv[0], "load", strlen("load"))) {
		if (argn != 2) {
			pr_info("[%s] cmd format: load edid_file_path\n",
				__func__);
			goto PROCESS_END;
		}

		/* clean '\n' from file path*/
		path_length = strlen(argv[1]);
		if (argv[1][path_length - 1] == '\n')
			argv[1][path_length - 1] = 0x0;
		load_edid_data(0, argv[1]);
	}

PROCESS_END:
	kfree(p);
	return 16;
}

/* rawedid attr */
static ssize_t rawedid_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	int pos = 0;
	int i;
	struct hdmitx_dev *hdev = get_hdmitx21_device();
	int num;
	int block_no = 0;

	/* prevent null prt */
	if (!hdev->edid_ptr)
		hdev->edid_ptr = hdev->EDID_buf;
	block_no = hdev->edid_ptr[126];
	if (block_no == 1)
		if (hdev->edid_ptr[128 + 4] == 0xe2 && hdev->edid_ptr[128 + 5] == 0x78)
			block_no = hdev->edid_ptr[128 + 6];
	if (block_no < 8)
		num = (block_no + 1) * 0x80;
	else
		num = 8 * 128;

	for (i = 0; i < num; i++)
		pos += snprintf(buf + pos, PAGE_SIZE, "%02x",
				hdev->edid_ptr[i]);

	pos += snprintf(buf + pos, PAGE_SIZE, "\n");

	return pos;
}

/*
 * edid_parsing attr
 * If RX edid data are all correct, HEAD(00ffffffffffff00), checksum,
 * version, etc), then return "ok". Otherwise, "ng"
 * Actually, in some old televisions, EDID is stored in EEPROM.
 * some bits in EEPROM may reverse with time.
 * But it does not affect  edid_parsing.
 * Therefore, we consider the RX edid data are all correct, return "OK"
 */
static ssize_t edid_parsing_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	int pos = 0;

	pos += snprintf(buf + pos, PAGE_SIZE, "ok\n");
	return pos;
}

/*
 * sink_type attr
 * sink, or repeater
 */
static ssize_t sink_type_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	int pos = 0;
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	if (!hdev->hpd_state) {
		pos += snprintf(buf + pos, PAGE_SIZE, "none\n");
		return pos;
	}

	if (hdev->hdmi_info.vsdb_phy_addr.b)
		pos += snprintf(buf + pos, PAGE_SIZE, "repeater\n");
	else
		pos += snprintf(buf + pos, PAGE_SIZE, "sink\n");

	return pos;
}

void hdmitx21_audio_mute_op(u32 flag)
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	hdev->tx_aud_cfg = flag;
	if (flag == 0)
		hdev->hwop.cntlconfig(hdev, CONF_AUDIO_MUTE_OP, AUDIO_MUTE);
	else
		hdev->hwop.cntlconfig(hdev, CONF_AUDIO_MUTE_OP, AUDIO_UNMUTE);
}

void hdmitx21_video_mute_op(u32 flag)
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	if (flag == 0)
		hdev->hwop.cntlconfig(hdev, CONF_VIDEO_MUTE_OP, VIDEO_MUTE);
	else
		hdev->hwop.cntlconfig(hdev, CONF_VIDEO_MUTE_OP, VIDEO_UNMUTE);
}

/*
 *  SDR/HDR uevent
 *  1: SDR to HDR
 * 0: HDR to SDR
 */
static void hdmitx_sdr_hdr_uevent(struct hdmitx_dev *hdev)
{
	if (hdev->hdmi_last_hdr_mode == 0 &&
	    hdev->hdmi_current_hdr_mode != 0) {
		/* SDR -> HDR*/
		hdev->hdmi_last_hdr_mode = hdev->hdmi_current_hdr_mode;
		hdmitx21_set_uevent(HDMITX_HDR_EVENT, 1);
	} else if ((hdev->hdmi_last_hdr_mode != 0) &&
			(hdev->hdmi_current_hdr_mode == 0)) {
		/* HDR -> SDR*/
		hdev->hdmi_last_hdr_mode = hdev->hdmi_current_hdr_mode;
		hdmitx21_set_uevent(HDMITX_HDR_EVENT, 0);
	}
}

static void hdr_work_func(struct work_struct *work)
{
	struct hdmitx_dev *hdev =
		container_of(work, struct hdmitx_dev, work_hdr);
	struct hdmi_drm_infoframe *info = &hdev->infoframes.drm.drm;

	if (hdev->hdr_transfer_feature == T_BT709 &&
	    hdev->hdr_color_feature == C_BT709) {
		pr_info("%s: send zero DRM\n", __func__);
		hdmi_drm_infoframe_init(info);
		hdmi_drm_infoframe_set(info);
		hdmi_avi_infoframe_config(CONF_AVI_BT2020, hdev->colormetry);

		msleep(1500);/*delay 1.5s*/
		/* disable DRM packets completely ONLY if hdr transfer
		 * feature and color feature still demand SDR.
		 */
		if (hdr_status_pos == 4) {
			/* zero hdr10+ VSIF being sent - disable it */
			pr_info("%s: disable hdr10+ vsif\n", __func__);
			hdmi_vend_infoframe_set(NULL);
			hdr_status_pos = 0;
		}
		if (hdev->hdr_transfer_feature == T_BT709 &&
		    hdev->hdr_color_feature == C_BT709) {
			pr_info("%s: disable DRM\n", __func__);
			hdmi_drm_infoframe_set(NULL);
			hdev->hdmi_current_hdr_mode = 0;
			hdmitx_sdr_hdr_uevent(hdev);
		}
	} else {
		hdmitx_sdr_hdr_uevent(hdev);
	}
}

#define hdmi_debug() \
	do { \
		if (log21_level == 0xff) \
			pr_info("%s[%d]\n", __func__, __LINE__); \
	} while (0)

/* Init drm_db[0] from Uboot status */
static void init_drm_db0(struct hdmitx_dev *hdev, u8 *dat)
{
	static int once_flag = 1;

	if (once_flag) {
		once_flag = 0;
		*dat = hdev->hwop.getstate(hdev, STAT_HDR_TYPE, 0);
	}
}

#define GET_LOW8BIT(a)	((a) & 0xff)
#define GET_HIGH8BIT(a)	(((a) >> 8) & 0xff)
static void hdmitx_set_drm_pkt(struct master_display_info_s *data)
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();
	u8 drm_hb[3] = {0x87, 0x1, 26};
	static u8 db[28] = {0x0};
	u8 *drm_db = &db[1]; /* db[0] is the checksum */

	hdmi_debug();
	init_drm_db0(hdev, &drm_db[0]);
	if (hdr_status_pos == 4) {
		/* zero hdr10+ VSIF being sent - disable it */
		pr_info("%s: disable hdr10+ zero vsif\n", __func__);
		hdmi_vend_infoframe_set(NULL);
		hdr_status_pos = 0;
	}

	/*
	 *hdr_color_feature: bit 23-16: color_primaries
	 *	1:bt709 0x9:bt2020
	 *hdr_transfer_feature: bit 15-8: transfer_characteristic
	 *	1:bt709 0xe:bt2020-10 0x10:smpte-st-2084 0x12:hlg(todo)
	 */
	if (data) {
		hdev->hdr_transfer_feature = (data->features >> 8) & 0xff;
		hdev->hdr_color_feature = (data->features >> 16) & 0xff;
		hdev->colormetry = (data->features >> 30) & 0x1;
	}

	if (hdr_status_pos != 1 && hdr_status_pos != 3)
		pr_info("%s: tf=%d, cf=%d, colormetry=%d\n",
			__func__,
			hdev->hdr_transfer_feature,
			hdev->hdr_color_feature,
			hdev->colormetry);
	hdr_status_pos = 1;
	/* if VSIF/DV or VSIF/HDR10P packet is enabled, disable it */
	if (hdmitx21_dv_en()) {
		update_current_para(hdev);
		hdmi_avi_infoframe_config(CONF_AVI_CS, hdev->para->cs);
/* if using VSIF/DOVI, then only clear DV_VS10_SIG, else disable VSIF */
		if (hdev->hwop.cntlconfig(hdev, CONF_CLR_DV_VS10_SIG, 0) == 0)
			hdmi_vend_infoframe_set(NULL);
	}

	/* hdr10+ content on a hdr10 sink case */
	if (hdev->hdr_transfer_feature == 0x30) {
		if (hdev->rxcap.hdr10plus_info.ieeeoui != 0x90848B ||
		    hdev->rxcap.hdr10plus_info.application_version != 1) {
			hdev->hdr_transfer_feature = T_SMPTE_ST_2084;
			pr_info("%s: HDR10+ not supported, treat as hdr10\n",
				__func__);
		}
	}

	if (!data || (!hdev->rxcap.hdr_sup_eotf_smpte_st_2084 &&
		      !hdev->rxcap.hdr_sup_eotf_hdr &&
		      !hdev->rxcap.hdr_sup_eotf_sdr &&
		      !hdev->rxcap.hdr_sup_eotf_hlg)) {
		drm_hb[1] = 0;
		drm_hb[2] = 0;
		drm_db[0] = 0;
		hdmi_drm_infoframe_set(NULL);
		hdmi_avi_infoframe_config(CONF_AVI_BT2020, hdev->colormetry);
		return;
	}

	/*SDR*/
	if (hdev->hdr_transfer_feature == T_BT709 &&
		hdev->hdr_color_feature == C_BT709) {
		/* send zero drm only for HDR->SDR transition */
		if (drm_db[0] == 0x02 || drm_db[0] == 0x03) {
			pr_info("%s: HDR->SDR, drm_db[0]=%d\n",
				__func__, drm_db[0]);
			hdev->colormetry = 0;
			hdmi_avi_infoframe_config(CONF_AVI_BT2020, 0);
			schedule_work(&hdev->work_hdr);
			drm_db[0] = 0;
		}
		return;
	}

	drm_db[1] = 0x0;
	drm_db[2] = GET_LOW8BIT(data->primaries[0][0]);
	drm_db[3] = GET_HIGH8BIT(data->primaries[0][0]);
	drm_db[4] = GET_LOW8BIT(data->primaries[0][1]);
	drm_db[5] = GET_HIGH8BIT(data->primaries[0][1]);
	drm_db[6] = GET_LOW8BIT(data->primaries[1][0]);
	drm_db[7] = GET_HIGH8BIT(data->primaries[1][0]);
	drm_db[8] = GET_LOW8BIT(data->primaries[1][1]);
	drm_db[9] = GET_HIGH8BIT(data->primaries[1][1]);
	drm_db[10] = GET_LOW8BIT(data->primaries[2][0]);
	drm_db[11] = GET_HIGH8BIT(data->primaries[2][0]);
	drm_db[12] = GET_LOW8BIT(data->primaries[2][1]);
	drm_db[13] = GET_HIGH8BIT(data->primaries[2][1]);
	drm_db[14] = GET_LOW8BIT(data->white_point[0]);
	drm_db[15] = GET_HIGH8BIT(data->white_point[0]);
	drm_db[16] = GET_LOW8BIT(data->white_point[1]);
	drm_db[17] = GET_HIGH8BIT(data->white_point[1]);
	drm_db[18] = GET_LOW8BIT(data->luminance[0]);
	drm_db[19] = GET_HIGH8BIT(data->luminance[0]);
	drm_db[20] = GET_LOW8BIT(data->luminance[1]);
	drm_db[21] = GET_HIGH8BIT(data->luminance[1]);
	drm_db[22] = GET_LOW8BIT(data->max_content);
	drm_db[23] = GET_HIGH8BIT(data->max_content);
	drm_db[24] = GET_LOW8BIT(data->max_frame_average);
	drm_db[25] = GET_HIGH8BIT(data->max_frame_average);

	/* bt2020 + gamma transfer */
	if (hdev->hdr_transfer_feature == T_BT709 &&
	    hdev->hdr_color_feature == C_BT2020) {
		if (hdev->sdr_hdr_feature == 0) {
			hdmi_drm_infoframe_set(NULL);
			hdmi_avi_infoframe_config(CONF_AVI_BT2020, SET_AVI_BT2020);
		} else if (hdev->sdr_hdr_feature == 1) {
			memset(db, 0, sizeof(db));
			hdmi_drm_infoframe_rawset(drm_hb, db);
			hdmi_avi_infoframe_config(CONF_AVI_BT2020, SET_AVI_BT2020);
		} else {
			drm_db[0] = 0x02; /* SMPTE ST 2084 */
			hdmi_drm_infoframe_rawset(drm_hb, db);
			hdmi_avi_infoframe_config(CONF_AVI_BT2020, SET_AVI_BT2020);
		}
		return;
	}

	/*must clear hdr mode*/
	hdev->hdmi_current_hdr_mode = 0;

	/* SMPTE ST 2084 and (BT2020 or NON_STANDARD) */
	if (hdev->rxcap.hdr_sup_eotf_smpte_st_2084) {
		if (hdev->hdr_transfer_feature == T_SMPTE_ST_2084 &&
		    hdev->hdr_color_feature == C_BT2020)
			hdev->hdmi_current_hdr_mode = 1;
		else if (hdev->hdr_transfer_feature == T_SMPTE_ST_2084 &&
			 hdev->hdr_color_feature != C_BT2020)
			hdev->hdmi_current_hdr_mode = 2;
	}

	/*HLG and BT2020*/
	if (hdev->rxcap.hdr_sup_eotf_hlg) {
		if (hdev->hdr_color_feature == C_BT2020 &&
		    (hdev->hdr_transfer_feature == T_BT2020_10 ||
		     hdev->hdr_transfer_feature == T_HLG))
			hdev->hdmi_current_hdr_mode = 3;
	}

	switch (hdev->hdmi_current_hdr_mode) {
	case 1:
		/*standard HDR*/
		drm_db[0] = 0x02; /* SMPTE ST 2084 */
		hdmi_drm_infoframe_rawset(drm_hb, db);
		hdmi_avi_infoframe_config(CONF_AVI_BT2020, SET_AVI_BT2020);
		break;
	case 2:
		/*non standard*/
		drm_db[0] = 0x02; /* no standard SMPTE ST 2084 */
		hdmi_drm_infoframe_rawset(drm_hb, db);
		hdmi_avi_infoframe_config(CONF_AVI_BT2020, CLR_AVI_BT2020);
		break;
	case 3:
		/*HLG*/
		drm_db[0] = 0x03;/* HLG is 0x03 */
		hdmi_drm_infoframe_rawset(drm_hb, db);
		hdmi_avi_infoframe_config(CONF_AVI_BT2020, SET_AVI_BT2020);
		break;
	case 0:
	default:
		/*other case*/
		hdmi_drm_infoframe_set(NULL);
		hdmi_avi_infoframe_config(CONF_AVI_BT2020, CLR_AVI_BT2020);
		break;
	}

	/* if sdr/hdr mode change ,notify uevent to userspace*/
	if (hdev->hdmi_current_hdr_mode != hdev->hdmi_last_hdr_mode)
		schedule_work(&hdev->work_hdr);
}

static void update_current_para(struct hdmitx_dev *hdev)
{
	struct vinfo_s *info = NULL;
	u8 mode[32];

	info = hdmitx_get_current_vinfo(NULL);
	if (!info)
		return;

	memset(mode, 0, sizeof(mode));
	strncpy(mode, info->name, sizeof(mode) - 1);
	if (strstr(hdev->fmt_attr, "420")) {
		if (!strstr(mode, "420"))
			strncat(mode, "420", sizeof(mode) - strlen("420") - 1);
	}
	hdev->para = hdmitx21_get_fmtpara(mode, hdev->fmt_attr);
}

static struct vsif_debug_save vsif_debug_info;
static void hdmitx_set_vsif_pkt(enum eotf_type type,
				enum mode_type tunnel_mode,
				struct dv_vsif_para *data,
				bool signal_sdr)
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();
	struct dv_vsif_para para = {0};
	u8 ven_hb[3] = {0x81, 0x01};
	u8 db1[28] = {0x00};
	u8 *ven_db1 = &db1[1];
	u8 db2[28] = {0x00};
	u8 *ven_db2 = &db2[1];
	u8 len = 0;
	u32 vic = hdev->cur_VIC;
	u32 hdmi_vic_4k_flag = 0;
	static enum eotf_type ltype = EOTF_T_NULL;
	static u8 ltmode = -1;
	enum hdmi_tf_type hdr_type = HDMI_NONE;

	hdmi_debug();
	if (hdev->bist_lock)
		return;

	if (!data)
		memcpy(&vsif_debug_info.data, &para,
		       sizeof(struct dv_vsif_para));
	else
		memcpy(&vsif_debug_info.data, data,
		       sizeof(struct dv_vsif_para));
	vsif_debug_info.type = type;
	vsif_debug_info.tunnel_mode = tunnel_mode;
	vsif_debug_info.signal_sdr = signal_sdr;

	if (hdev->ready == 0 || hdev->rxcap.dv_info.ieeeoui
		!= DV_IEEE_OUI) {
		ltype = EOTF_T_NULL;
		ltmode = -1;
		return;
	}

	if (hdr_status_pos != 2)
		pr_info("%s: type = %d\n", __func__, type);
	hdr_status_pos = 2;

	/* if DRM/HDR packet is enabled, disable it */
	hdr_type = hdmitx21_get_cur_hdr_st();
	if (hdr_type != HDMI_NONE && hdr_type != HDMI_HDR_SDR) {
		hdev->hdr_transfer_feature = T_BT709;
		hdev->hdr_color_feature = C_BT709;
		hdev->colormetry = 0;
		hdmi_avi_infoframe_config(CONF_AVI_BT2020, hdev->colormetry);
		schedule_work(&hdev->work_hdr);
	}

	hdev->hdmi_current_eotf_type = type;
	hdev->hdmi_current_tunnel_mode = tunnel_mode;
	/*ver0 and ver1_15 and ver1_12bit with ll= 0 use hdmi 1.4b VSIF*/
	if (hdev->rxcap.dv_info.ver == 0 ||
	    (hdev->rxcap.dv_info.ver == 1 &&
	    hdev->rxcap.dv_info.length == 0xE) ||
	    (hdev->rxcap.dv_info.ver == 1 &&
	    hdev->rxcap.dv_info.length == 0xB &&
	    hdev->rxcap.dv_info.low_latency == 0)) {
		if (vic == HDMI_95_3840x2160p30_16x9 ||
		    vic == HDMI_94_3840x2160p25_16x9 ||
		    vic == HDMI_93_3840x2160p24_16x9 ||
		    vic == HDMI_98_4096x2160p24_256x135)
			hdmi_vic_4k_flag = 1;

		switch (type) {
		case EOTF_T_DOLBYVISION:
			len = 0x18;
			hdev->dv_src_feature = 1;
			break;
		case EOTF_T_HDR10:
		case EOTF_T_SDR:
		case EOTF_T_NULL:
		default:
			len = 0x05;
			hdev->dv_src_feature = 0;
			break;
		}

		ven_hb[2] = len;
		ven_db1[0] = 0x03;
		ven_db1[1] = 0x0c;
		ven_db1[2] = 0x00;
		ven_db1[3] = 0x00;

		if (hdmi_vic_4k_flag) {
			ven_db1[3] = 0x20;
			if (vic == HDMI_95_3840x2160p30_16x9)
				ven_db1[4] = 0x1;
			else if (vic == HDMI_94_3840x2160p25_16x9)
				ven_db1[4] = 0x2;
			else if (vic == HDMI_93_3840x2160p24_16x9)
				ven_db1[4] = 0x3;
			else/*vic == HDMI_98_4096x2160p24_256x135*/
				ven_db1[4] = 0x4;
		}
		if (type == EOTF_T_DV_AHEAD) {
			hdmi_vend_infoframe_rawset(ven_hb, db1);
			return;
		}
		if (type == EOTF_T_DOLBYVISION) {
			/*first disable drm package*/
			hdmi_drm_infoframe_set(NULL);
			hdmi_vend_infoframe_rawset(ven_hb, db1);
			hdmi_avi_infoframe_config(CONF_AVI_BT2020, CLR_AVI_BT2020);/*BT709*/
			if (tunnel_mode == RGB_8BIT) {
				hdmi_avi_infoframe_config(CONF_AVI_CS, HDMI_COLORSPACE_RGB);
				hdmi_avi_infoframe_config(CONF_AVI_Q01, RGB_RANGE_FUL);
			} else {
				hdmi_avi_infoframe_config(CONF_AVI_CS, HDMI_COLORSPACE_YUV422);
				hdmi_avi_infoframe_config(CONF_AVI_YQ01, YCC_RANGE_FUL);
			}
		} else {
			if (hdmi_vic_4k_flag)
				hdmi_vend_infoframe_rawset(ven_hb, db1);
			else
				hdmi_vend_infoframe_set(NULL);
			if (signal_sdr) {
				pr_info("hdmitx: H14b VSIF, switching signal to SDR\n");
				update_current_para(hdev);
				hdmi_avi_infoframe_config(CONF_AVI_CS, hdev->para->cs);
				hdmi_avi_infoframe_config(CONF_AVI_Q01, RGB_RANGE_LIM);
				hdmi_avi_infoframe_config(CONF_AVI_YQ01, YCC_RANGE_LIM);
				hdmi_avi_infoframe_config(CONF_AVI_BT2020, CLR_AVI_BT2020);/*BT709*/
			}
		}
	}
	/*ver1_12  with low_latency = 1 and ver2 use Dolby VSIF*/
	if (hdev->rxcap.dv_info.ver == 2 ||
	    (hdev->rxcap.dv_info.ver == 1 &&
	     hdev->rxcap.dv_info.length == 0xB &&
	     hdev->rxcap.dv_info.low_latency == 1) ||
	     type == EOTF_T_LL_MODE) {
		if (!data)
			data = &para;
		len = 0x1b;

		switch (type) {
		case EOTF_T_DOLBYVISION:
		case EOTF_T_LL_MODE:
			hdev->dv_src_feature = 1;
			break;
		case EOTF_T_HDR10:
		case EOTF_T_SDR:
		case EOTF_T_NULL:
		default:
			hdev->dv_src_feature = 0;
			break;
		}
		ven_hb[2] = len;
		ven_db2[0] = 0x46;
		ven_db2[1] = 0xd0;
		ven_db2[2] = 0x00;
		if (data->ver2_l11_flag == 1) {
			ven_db2[3] = data->vers.ver2_l11.low_latency |
				     data->vers.ver2_l11.dobly_vision_signal << 1;
			ven_db2[4] = data->vers.ver2_l11.eff_tmax_PQ_hi
				     | data->vers.ver2_l11.auxiliary_MD_present << 6
				     | data->vers.ver2_l11.backlt_ctrl_MD_present << 7
				     | 0x20; /*L11_MD_Present*/
			ven_db2[5] = data->vers.ver2_l11.eff_tmax_PQ_low;
			ven_db2[6] = data->vers.ver2_l11.auxiliary_runmode;
			ven_db2[7] = data->vers.ver2_l11.auxiliary_runversion;
			ven_db2[8] = data->vers.ver2_l11.auxiliary_debug0;
			ven_db2[9] = (data->vers.ver2_l11.content_type)
				| (data->vers.ver2_l11.content_sub_type << 4);
			ven_db2[10] = (data->vers.ver2_l11.intended_white_point)
				| (data->vers.ver2_l11.crf << 4);
			ven_db2[11] = data->vers.ver2_l11.l11_byte2;
			ven_db2[12] = data->vers.ver2_l11.l11_byte3;
		} else {
			ven_db2[3] = (data->vers.ver2.low_latency) |
				(data->vers.ver2.dobly_vision_signal << 1);
			ven_db2[4] = (data->vers.ver2.eff_tmax_PQ_hi)
				| (data->vers.ver2.auxiliary_MD_present << 6)
				| (data->vers.ver2.backlt_ctrl_MD_present << 7);
			ven_db2[5] = data->vers.ver2.eff_tmax_PQ_low;
			ven_db2[6] = data->vers.ver2.auxiliary_runmode;
			ven_db2[7] = data->vers.ver2.auxiliary_runversion;
			ven_db2[8] = data->vers.ver2.auxiliary_debug0;
		}
		if (type == EOTF_T_DV_AHEAD) {
			hdmi_vend_infoframe_rawset(ven_hb, db2);
			return;
		}
		/*Dolby Vision standard case*/
		if (type == EOTF_T_DOLBYVISION) {
			/*first disable drm package*/
			hdmi_drm_infoframe_set(NULL);
			hdmi_vend_infoframe_rawset(ven_hb, db2);
			hdmi_avi_infoframe_config(CONF_AVI_BT2020, CLR_AVI_BT2020);/*BT709*/
			if (tunnel_mode == RGB_8BIT) {/*RGB444*/
				hdmi_avi_infoframe_config(CONF_AVI_CS, HDMI_COLORSPACE_RGB);
				hdmi_avi_infoframe_config(CONF_AVI_Q01, RGB_RANGE_FUL);
			} else {/*YUV422*/
				hdmi_avi_infoframe_config(CONF_AVI_CS, HDMI_COLORSPACE_YUV422);
				hdmi_avi_infoframe_config(CONF_AVI_YQ01, YCC_RANGE_FUL);
			}
		}
		/*Dolby Vision low-latency case*/
		else if  (type == EOTF_T_LL_MODE) {
			/*first disable drm package*/
			hdmi_drm_infoframe_set(NULL);
			hdmi_vend_infoframe_rawset(ven_hb, db2);
			if (hdev->rxcap.colorimetry_data & 0xe0)
				/*if RX support BT2020, then output BT2020*/
				hdmi_avi_infoframe_config(CONF_AVI_BT2020, SET_AVI_BT2020);
			else
				hdmi_avi_infoframe_config(CONF_AVI_BT2020, CLR_AVI_BT2020);
			if (tunnel_mode == RGB_10_12BIT) {/*10/12bit RGB444*/
				hdmi_avi_infoframe_config(CONF_AVI_CS, HDMI_COLORSPACE_RGB);
				hdmi_avi_infoframe_config(CONF_AVI_Q01, RGB_RANGE_LIM);
			} else if (tunnel_mode == YUV444_10_12BIT) {
				/*10/12bit YUV444*/
				hdmi_avi_infoframe_config(CONF_AVI_CS, HDMI_COLORSPACE_YUV444);
				hdmi_avi_infoframe_config(CONF_AVI_YQ01, YCC_RANGE_LIM);
			} else {/*YUV422*/
				hdmi_avi_infoframe_config(CONF_AVI_CS, HDMI_COLORSPACE_YUV422);
				hdmi_avi_infoframe_config(CONF_AVI_YQ01, YCC_RANGE_LIM);
			}
		} else { /*SDR case*/
			pr_info("hdmitx: Dolby VSIF, ven_db2[3]) = %d\n", ven_db2[3]);
			hdmi_vend_infoframe_rawset(ven_hb, db2);
			if (signal_sdr) {
				pr_info("hdmitx: Dolby VSIF, switching signal to SDR\n");
				update_current_para(hdev);
				pr_info("vic:%d, cd:%d, cs:%d, cr:%d\n",
					hdev->para->timing.vic, hdev->para->cd,
					hdev->para->cs, hdev->para->cr);
				hdmi_avi_infoframe_config(CONF_AVI_CS, hdev->para->cs);
				hdmi_avi_infoframe_config(CONF_AVI_Q01, RGB_RANGE_DEFAULT);
				hdmi_avi_infoframe_config(CONF_AVI_YQ01, YCC_RANGE_LIM);
				hdmi_avi_infoframe_config(CONF_AVI_BT2020, CLR_AVI_BT2020);/*BT709*/
			}
		}
	}
}

static void hdmitx_set_hdr10plus_pkt(u32 flag,
	struct hdr10plus_para *data)
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();
	u8 ven_hb[3] = {0x81, 0x01, 0x1b};
	u8 db[28] = {0x00};
	u8 *ven_db = &db[1];

	hdmi_debug();
	if (hdev->bist_lock)
		return;
	if (flag == HDR10_PLUS_ZERO_VSIF) {
		/* needed during hdr10+ to sdr transition */
		pr_info("%s: zero vsif\n", __func__);
		hdmi_vend_infoframe_rawset(ven_hb, db);
		hdmi_avi_infoframe_config(CONF_AVI_BT2020, CLR_AVI_BT2020);
		hdev->hdr10plus_feature = 0;
		hdr_status_pos = 4;
		return;
	}

	if (!data || !flag) {
		pr_info("%s: null vsif\n", __func__);
		hdmi_vend_infoframe_set(NULL);
		hdmi_avi_infoframe_config(CONF_AVI_BT2020, CLR_AVI_BT2020);
		hdev->hdr10plus_feature = 0;
		return;
	}

	if (hdev->hdr10plus_feature != 1)
		pr_info("%s: flag = %d\n", __func__, flag);
	hdev->hdr10plus_feature = 1;
	hdr_status_pos = 3;
	ven_db[0] = 0x8b;
	ven_db[1] = 0x84;
	ven_db[2] = 0x90;

	ven_db[3] = ((data->application_version & 0x3) << 6) |
		 ((data->targeted_max_lum & 0x1f) << 1);
	ven_db[4] = data->average_maxrgb;
	ven_db[5] = data->distribution_values[0];
	ven_db[6] = data->distribution_values[1];
	ven_db[7] = data->distribution_values[2];
	ven_db[8] = data->distribution_values[3];
	ven_db[9] = data->distribution_values[4];
	ven_db[10] = data->distribution_values[5];
	ven_db[11] = data->distribution_values[6];
	ven_db[12] = data->distribution_values[7];
	ven_db[13] = data->distribution_values[8];
	ven_db[14] = ((data->num_bezier_curve_anchors & 0xf) << 4) |
		((data->knee_point_x >> 6) & 0xf);
	ven_db[15] = ((data->knee_point_x & 0x3f) << 2) |
		((data->knee_point_y >> 8) & 0x3);
	ven_db[16] = data->knee_point_y  & 0xff;
	ven_db[17] = data->bezier_curve_anchors[0];
	ven_db[18] = data->bezier_curve_anchors[1];
	ven_db[19] = data->bezier_curve_anchors[2];
	ven_db[20] = data->bezier_curve_anchors[3];
	ven_db[21] = data->bezier_curve_anchors[4];
	ven_db[22] = data->bezier_curve_anchors[5];
	ven_db[23] = data->bezier_curve_anchors[6];
	ven_db[24] = data->bezier_curve_anchors[7];
	ven_db[25] = data->bezier_curve_anchors[8];
	ven_db[26] = ((data->graphics_overlay_flag & 0x1) << 7) |
		((data->no_delay_flag & 0x1) << 6);

	hdmi_vend_infoframe_rawset(ven_hb, db);
	hdmi_avi_infoframe_config(CONF_AVI_BT2020, SET_AVI_BT2020);
}

#define  EMP_FIRST 0x80
#define  EMP_LAST 0x40
static void hdmitx_set_emp_pkt(u8 *data, u32 type,
			       u32 size)
{
	u32 number;
	u32 remainder;
	u8 *virt_ptr;
	u8 *virt_ptr_align32bit;
	unsigned long phys_ptr;
	u32 i;
	struct hdmitx_dev *hdev = get_hdmitx21_device();
	u32 ds_type = 0;
	u8 AFR = 0;
	u8 VFR = 0;
	u8 sync = 0;
	u8  new = 0;
	u8  end = 0;
	u32 organzation_id = 0;
	u32 data_set_tag = 0;
	u32 data_set_length = 0;

	hdmi_debug();

	if (!data) {
		pr_info("the data is null\n");
		return;
	}

	if (size <= 21) {
		number = 1;
		remainder = size;
	} else {
		number = ((size - 21) / 28) + 2;
		remainder = (size - 21) % 28;
	}

	virt_ptr = kzalloc(sizeof(unsigned char) * (number + 0x1f),
			   GFP_KERNEL);
	if (!virt_ptr)
		return;
	pr_info("emp_pkt virt_ptr: %p\n", virt_ptr);
	virt_ptr_align32bit = (u8 *)
		((((unsigned long)virt_ptr) + 0x1f) & (~0x1f));
	pr_info("emp_pkt virt_ptr_align32bit: %p\n", virt_ptr_align32bit);

	memset(virt_ptr_align32bit, 0, sizeof(unsigned char) * (number + 0x1f));

	switch (type) {
	case VENDOR_SPECIFIC_EM_DATA:
		break;
	case COMPRESS_VIDEO_TRAMSPORT:
		break;
	case HDR_DYNAMIC_METADATA:
			ds_type = 1;
			sync = 1;
			VFR = 1;
			AFR = 0;
			new = 0x1; /*todo*/
			end = 0x1; /*todo*/
			organzation_id = 2;
		break;
	case VIDEO_TIMING_EXTENDED:
		break;
	default:
		break;
	}

	for (i = 0; i < number; i++) {
		/*HB[0]-[2]*/
		virt_ptr_align32bit[i * 32 + 0] = 0x7F;
		if (i == 0)
			virt_ptr_align32bit[i * 32 + 1] |=  EMP_FIRST;
		if (i == number)
			virt_ptr_align32bit[i * 32 + 1] |= EMP_LAST;
		virt_ptr_align32bit[i * 32 + 2] = number;
		/*PB[0]-[6]*/
		if (i == 0) {
			virt_ptr_align32bit[3] = (new << 7) | (end << 6) |
				(ds_type << 4) | (AFR << 3) |
				(VFR << 2) | (sync << 1);
			virt_ptr_align32bit[4] = 0;/*Rsvd*/
			virt_ptr_align32bit[5] = organzation_id;
			virt_ptr_align32bit[6] = (data_set_tag >> 8) & 0xFF;
			virt_ptr_align32bit[7] = data_set_tag & 0xFF;
			virt_ptr_align32bit[8] = (data_set_length >> 8)
				& 0xFF;
			virt_ptr_align32bit[9] = data_set_length & 0xFF;
		}
		if (number == 1) {
			memcpy(&virt_ptr_align32bit[10], &data[0],
			       sizeof(unsigned char) * remainder);
		} else {
			if (i == 0) {
			/*MD: first package need PB[7]-[27]*/
				memcpy(&virt_ptr_align32bit[10], &data[0],
				       sizeof(unsigned char) * 21);
			} else if (i != number) {
			/*MD: following package need PB[0]-[27]*/
				memcpy(&virt_ptr_align32bit[i * 32 + 10],
				       &data[(i - 1) * 28 + 21],
				       sizeof(unsigned char) * 28);
			} else {
			/*MD: the last package need PB[0] to end */
				memcpy(&virt_ptr_align32bit[0],
				       &data[(i - 1) * 28 + 21],
				       sizeof(unsigned char) * remainder);
			}
		}
			/*PB[28]*/
		virt_ptr_align32bit[i * 32 + 31] = 0;
	}

	phys_ptr = virt_to_phys(virt_ptr_align32bit);
	pr_info("emp_pkt phys_ptr: %lx\n", phys_ptr);

	hdev->hwop.cntlconfig(hdev, CONF_EMP_NUMBER, number);
	hdev->hwop.cntlconfig(hdev, CONF_EMP_PHY_ADDR, phys_ptr);
}

/*config attr*/
static ssize_t config_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	int pos = 0;
	u8 *conf;
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	pos += snprintf(buf + pos, PAGE_SIZE, "cur_VIC: %d\n", hdev->cur_VIC);

	if (hdev->para) {
		switch (hdev->para->cd) {
		case COLORDEPTH_24B:
			conf = "8bit";
			break;
		case COLORDEPTH_30B:
			conf = "10bit";
			break;
		case COLORDEPTH_36B:
			conf = "12bit";
			break;
		case COLORDEPTH_48B:
			conf = "16bit";
			break;
		default:
			conf = "reserved";
		}
		pos += snprintf(buf + pos, PAGE_SIZE, "colordepth: %s\n",
				conf);
		switch (hdev->para->cs) {
		case HDMI_COLORSPACE_RGB:
			conf = "RGB";
			break;
		case HDMI_COLORSPACE_YUV422:
			conf = "422";
			break;
		case HDMI_COLORSPACE_YUV444:
			conf = "444";
			break;
		case HDMI_COLORSPACE_YUV420:
			conf = "420";
			break;
		default:
			conf = "reserved";
		}
		pos += snprintf(buf + pos, PAGE_SIZE, "colorspace: %s\n",
				conf);
	}

	switch (hdev->tx_aud_cfg) {
	case 0:
		conf = "off";
		break;
	case 1:
		conf = "on";
		break;
	case 2:
		conf = "auto";
		break;
	default:
		conf = "none";
	}
	pos += snprintf(buf + pos, PAGE_SIZE, "audio config: %s\n", conf);

	switch (hdev->hdmi_audio_off_flag) {
	case 0:
		conf = "on";
		break;
	case 1:
		conf = "off";
		break;
	default:
		conf = "none";
	}
	pos += snprintf(buf + pos, PAGE_SIZE, "audio on/off: %s\n", conf);

	switch (hdev->tx_aud_src) {
	case 0:
		conf = "SPDIF";
		break;
	case 1:
		conf = "I2S";
		break;
	default:
		conf = "none";
	}
	pos += snprintf(buf + pos, PAGE_SIZE, "audio source: %s\n", conf);

	switch (hdev->cur_audio_param.type) {
	case CT_REFER_TO_STREAM:
		conf = "refer to stream header";
		break;
	case CT_PCM:
		conf = "L-PCM";
		break;
	case CT_AC_3:
		conf = "AC-3";
		break;
	case CT_MPEG1:
		conf = "MPEG1";
		break;
	case CT_MP3:
		conf = "MP3";
		break;
	case CT_MPEG2:
		conf = "MPEG2";
		break;
	case CT_AAC:
		conf = "AAC";
		break;
	case CT_DTS:
		conf = "DTS";
		break;
	case CT_ATRAC:
		conf = "ATRAC";
		break;
	case CT_ONE_BIT_AUDIO:
		conf = "One Bit Audio";
		break;
	case CT_DOLBY_D:
		conf = "Dobly Digital+";
		break;
	case CT_DTS_HD:
		conf = "DTS_HD";
		break;
	case CT_MAT:
		conf = "MAT";
		break;
	case CT_DST:
		conf = "DST";
		break;
	case CT_WMA:
		conf = "WMA";
		break;
	default:
		conf = "MAX";
	}
	pos += snprintf(buf + pos, PAGE_SIZE, "audio type: %s\n", conf);

	switch (hdev->cur_audio_param.channel_num) {
	case CC_REFER_TO_STREAM:
		conf = "refer to stream header";
		break;
	case CC_2CH:
		conf = "2 channels";
		break;
	case CC_3CH:
		conf = "3 channels";
		break;
	case CC_4CH:
		conf = "4 channels";
		break;
	case CC_5CH:
		conf = "5 channels";
		break;
	case CC_6CH:
		conf = "6 channels";
		break;
	case CC_7CH:
		conf = "7 channels";
		break;
	case CC_8CH:
		conf = "8 channels";
		break;
	default:
		conf = "MAX";
	}
	pos += snprintf(buf + pos, PAGE_SIZE, "audio channel num: %s\n", conf);

	switch (hdev->cur_audio_param.sample_rate) {
	case FS_REFER_TO_STREAM:
		conf = "refer to stream header";
		break;
	case FS_32K:
		conf = "32kHz";
		break;
	case FS_44K1:
		conf = "44.1kHz";
		break;
	case FS_48K:
		conf = "48kHz";
		break;
	case FS_88K2:
		conf = "88.2kHz";
		break;
	case FS_96K:
		conf = "96kHz";
		break;
	case FS_176K4:
		conf = "176.4kHz";
		break;
	case FS_192K:
		conf = "192kHz";
		break;
	case FS_768K:
		conf = "768kHz";
		break;
	default:
		conf = "MAX";
	}
	pos += snprintf(buf + pos, PAGE_SIZE, "audio sample rate: %s\n", conf);

	switch (hdev->cur_audio_param.sample_size) {
	case SS_REFER_TO_STREAM:
		conf = "refer to stream header";
		break;
	case SS_16BITS:
		conf = "16bit";
		break;
	case SS_20BITS:
		conf = "20bit";
		break;
	case SS_24BITS:
		conf = "24bit";
		break;
	default:
		conf = "MAX";
	}
	pos += snprintf(buf + pos, PAGE_SIZE, "audio sample size: %s\n", conf);

	if (hdev->flag_3dfp)
		conf = "FramePacking";
	else if (hdev->flag_3dss)
		conf = "SidebySide";
	else if (hdev->flag_3dtb)
		conf = "TopButtom";
	else
		conf = "off";
	pos += snprintf(buf + pos, PAGE_SIZE, "3D config: %s\n", conf);
	return pos;
}

static ssize_t config_store(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t count)
{
	int ret = 0;
	struct master_display_info_s data = {0};
	struct hdr10plus_para hdr_data = {0x1, 0x2, 0x3};
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	pr_info("hdmitx: config: %s\n", buf);

	if (strncmp(buf, "unplug_powerdown", 16) == 0) {
		if (buf[16] == '0')
			hdev->unplug_powerdown = 0;
		else
			hdev->unplug_powerdown = 1;
	} else if (strncmp(buf, "info", 4) == 0) {
		pr_info("%x %x %x %x %x %x\n",
			hdmitx21_get_cur_hdr_st(),
			hdmitx21_get_cur_dv_st(),
			hdmitx21_get_cur_hdr10p_st(),
			hdmitx21_hdr_en(),
			hdmitx21_dv_en(),
			hdmitx21_hdr10p_en()
			);
	} else if (strncmp(buf, "3d", 2) == 0) {
		/* Second, set 3D parameters */
		if (strncmp(buf + 2, "tb", 2) == 0) {
			hdev->flag_3dtb = 1;
			hdev->flag_3dss = 0;
			hdev->flag_3dfp = 0;
			hdmi21_set_3d(hdev, T3D_TAB, 0);
		} else if ((strncmp(buf + 2, "lr", 2) == 0) ||
			(strncmp(buf + 2, "ss", 2) == 0)) {
			unsigned long sub_sample_mode = 0;

			hdev->flag_3dtb = 0;
			hdev->flag_3dss = 1;
			hdev->flag_3dfp = 0;
			if (buf[2])
				ret = kstrtoul(buf + 2, 10,
					       &sub_sample_mode);
			/* side by side */
			hdmi21_set_3d(hdev, T3D_SBS_HALF,
				    sub_sample_mode);
		} else if (strncmp(buf + 2, "fp", 2) == 0) {
			hdev->flag_3dtb = 0;
			hdev->flag_3dss = 0;
			hdev->flag_3dfp = 1;
			hdmi21_set_3d(hdev, T3D_FRAME_PACKING, 0);
		} else if (strncmp(buf + 2, "off", 3) == 0) {
			hdev->flag_3dfp = 0;
			hdev->flag_3dtb = 0;
			hdev->flag_3dss = 0;
			hdmi21_set_3d(hdev, T3D_DISABLE, 0);
		}
	} else if (strncmp(buf, "sdr", 3) == 0) {
		data.features = 0x00010100;
		hdmitx_set_drm_pkt(&data);
	} else if (strncmp(buf, "hdr", 3) == 0) {
		data.features = 0x00091000;
		hdmitx_set_drm_pkt(&data);
	} else if (strncmp(buf, "hlg", 3) == 0) {
		data.features = 0x00091200;
		hdmitx_set_drm_pkt(&data);
	} else if (strncmp(buf, "vsif", 4) == 0) {
		hdmitx_set_vsif_pkt(buf[4] - '0', buf[5] == '1', NULL, true);
	} else if (strncmp(buf, "emp", 3) == 0) {
		hdmitx_set_emp_pkt(NULL, 1, 1);
	} else if (strncmp(buf, "hdr10+", 6) == 0) {
		hdmitx_set_hdr10plus_pkt(1, &hdr_data);
	}
	return count;
}

void hdmitx21_ext_set_audio_output(int enable)
{
	hdmitx21_audio_mute_op(enable);
}

int hdmitx21_ext_get_audio_status(void)
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	return !!hdev->tx_aud_cfg;
}

void hdmitx21_ext_set_i2s_mask(char ch_num, char ch_msk)
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();
	static u32 update_flag = -1;

	if (!(ch_num == 2 || ch_num == 4 ||
	      ch_num == 6 || ch_num == 8)) {
		pr_info("err chn setting, must be 2, 4, 6 or 8, Rst as def\n");
		hdev->aud_output_ch = 0;
		if (update_flag != hdev->aud_output_ch) {
			update_flag = hdev->aud_output_ch;
			hdev->hdmi_ch = 0;
			hdmitx21_set_audio(hdev, &hdev->cur_audio_param);
		}
	}
	if (ch_msk == 0) {
		pr_info("err chn msk, must larger than 0\n");
		return;
	}
	hdev->aud_output_ch = (ch_num << 4) + ch_msk;
	if (update_flag != hdev->aud_output_ch) {
		update_flag = hdev->aud_output_ch;
		hdev->hdmi_ch = 0;
		hdmitx21_set_audio(hdev, &hdev->cur_audio_param);
	}
}

char hdmitx21_ext_get_i2s_mask(void)
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	return hdev->aud_output_ch & 0xf;
}

static ssize_t vid_mute_show(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
	int pos = 0;
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	pos += snprintf(buf + pos, PAGE_SIZE, "%d\n",
		atomic_read(&hdev->kref_video_mute));
	return pos;
}

static ssize_t vid_mute_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();
	atomic_t kref_video_mute = hdev->kref_video_mute;

	if (buf[0] == '1') {
		atomic_inc(&kref_video_mute);
		if (atomic_read(&kref_video_mute) == 1)
			hdmitx21_video_mute_op(0);
	}
	if (buf[0] == '0') {
		if (!(atomic_sub_and_test(0, &kref_video_mute))) {
			atomic_dec(&kref_video_mute);
			if (atomic_sub_and_test(0, &kref_video_mute))
				hdmitx21_video_mute_op(1);
		}
	}

	hdev->kref_video_mute = kref_video_mute;

	return count;
}

static ssize_t debug_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	hdev->hwop.debugfun(hdev, buf);
	return count;
}

/**/
static ssize_t disp_cap_show(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
	int i, pos = 0;
	struct hdmitx_dev *hdev = get_hdmitx21_device();
	struct rx_cap *prxcap = &hdev->rxcap;
	const struct hdmi_timing *timing = NULL;
	enum hdmi_vic vic;

	for (i = 0; i < prxcap->VIC_count; i++) {
		vic = prxcap->VIC[i];
		timing = hdmitx21_gettiming_from_vic(vic);
		if (timing) {
			pos += snprintf(buf + pos, PAGE_SIZE, "%s",
				timing->sname ? timing->sname : timing->name);
			if (vic == prxcap->native_vic ||
				vic == prxcap->native_vic2)
				pos += snprintf(buf + pos, PAGE_SIZE, "*");
			pos += snprintf(buf + pos, PAGE_SIZE, "\n");
		}
	}
	return pos;
}

static ssize_t preferred_mode_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	int pos = 0;
	struct hdmitx_dev *hdev = get_hdmitx21_device();
	struct rx_cap *prxcap = &hdev->rxcap;

	pos += snprintf(buf + pos, PAGE_SIZE, "%s\n",
		hdmitx21_edid_vic_to_string(prxcap->preferred_mode));

	return pos;
}

/* cea_cap, a clone of disp_cap */
static ssize_t cea_cap_show(struct device *dev,
			    struct device_attribute *attr,
			    char *buf)
{
	return disp_cap_show(dev, attr, buf);
}

static ssize_t vesa_cap_show(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
	/* TODO */
	return 0;
}

static void _show_pcm_ch(struct rx_cap *prxcap, int i,
			 int *ppos, char *buf)
{
	const char * const aud_sample_size[] = {"ReferToStreamHeader",
		"16", "20", "24", NULL};
	int j = 0;

	for (j = 0; j < 3; j++) {
		if (prxcap->RxAudioCap[i].cc3 & (1 << j))
			*ppos += snprintf(buf + *ppos, PAGE_SIZE, "%s/",
				aud_sample_size[j + 1]);
	}
	*ppos += snprintf(buf + *ppos - 1, PAGE_SIZE, " bit\n");
}

/**/
static ssize_t aud_cap_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();
	int i, pos = 0, j;
	static const char * const aud_ct[] =  {
		"ReferToStreamHeader", "PCM", "AC-3", "MPEG1", "MP3",
		"MPEG2", "AAC", "DTS", "ATRAC",	"OneBitAudio",
		"Dobly_Digital+", "DTS-HD", "MAT", "DST", "WMA_Pro",
		"Reserved", NULL};
	static const char * const aud_sampling_frequency[] = {
		"ReferToStreamHeader", "32", "44.1", "48", "88.2", "96",
		"176.4", "192", NULL};
	struct rx_cap *prxcap = &hdev->rxcap;

	pos += snprintf(buf + pos, PAGE_SIZE,
		"CodingType MaxChannels SamplingFreq SampleSize\n");
	for (i = 0; i < prxcap->AUD_count; i++) {
		pos += snprintf(buf + pos, PAGE_SIZE, "%s",
			aud_ct[prxcap->RxAudioCap[i].audio_format_code]);
		if (prxcap->RxAudioCap[i].audio_format_code == CT_DOLBY_D &&
		    (prxcap->RxAudioCap[i].cc3 & 1))
			pos += snprintf(buf + pos, PAGE_SIZE, "/ATMOS");
		pos += snprintf(buf + pos, PAGE_SIZE, ", %d ch, ",
			prxcap->RxAudioCap[i].channel_num_max + 1);
		for (j = 0; j < 7; j++) {
			if (prxcap->RxAudioCap[i].freq_cc & (1 << j))
				pos += snprintf(buf + pos, PAGE_SIZE, "%s/",
					aud_sampling_frequency[j + 1]);
		}
		pos += snprintf(buf + pos - 1, PAGE_SIZE, " kHz, ");
		switch (prxcap->RxAudioCap[i].audio_format_code) {
		case CT_PCM:
			_show_pcm_ch(prxcap, i, &pos, buf);
			break;
		case CT_AC_3:
		case CT_MPEG1:
		case CT_MP3:
		case CT_MPEG2:
		case CT_AAC:
		case CT_DTS:
		case CT_ATRAC:
		case CT_ONE_BIT_AUDIO:
			pos += snprintf(buf + pos, PAGE_SIZE,
				"MaxBitRate %dkHz\n",
				prxcap->RxAudioCap[i].cc3 * 8);
			break;
		case CT_DOLBY_D:
		case CT_DTS_HD:
		case CT_MAT:
		case CT_DST:
			pos += snprintf(buf + pos, PAGE_SIZE, "DepVaule 0x%x\n",
				prxcap->RxAudioCap[i].cc3);
			break;
		case CT_WMA:
		default:
			break;
		}
	}
	return pos;
}

/**/
static ssize_t hdmi_hdr_status_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	int pos = 0;
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	/* pos = 3 */
	if (hdr_status_pos == 3 || hdev->hdr10plus_feature) {
		pos += snprintf(buf + pos, PAGE_SIZE, "HDR10Plus-VSIF");
		return pos;
	}

	/* pos = 2 */
	if (hdr_status_pos == 2) {
		if (hdev->hdmi_current_eotf_type == EOTF_T_DOLBYVISION) {
			pos += snprintf(buf + pos, PAGE_SIZE,
				"DolbyVision-Std");
			return pos;
		}
		if (hdev->hdmi_current_eotf_type == EOTF_T_LL_MODE) {
			pos += snprintf(buf + pos, PAGE_SIZE,
				"DolbyVision-Lowlatency");
			return pos;
		}
	}

	/* pos = 1 */
	if (hdr_status_pos == 1) {
		if (hdev->hdr_transfer_feature == T_SMPTE_ST_2084) {
			if (hdev->hdr_color_feature == C_BT2020) {
				pos += snprintf(buf + pos, PAGE_SIZE,
					"HDR10-GAMMA_ST2084");
				return pos;
			}
			pos += snprintf(buf + pos, PAGE_SIZE, "HDR10-others");
			return pos;
		}
		if (hdev->hdr_color_feature == C_BT2020 &&
		    (hdev->hdr_transfer_feature == T_BT2020_10 ||
		     hdev->hdr_transfer_feature == T_HLG)) {
			pos += snprintf(buf + pos, PAGE_SIZE,
				"HDR10-GAMMA_HLG");
			return pos;
		}
	}

	/* default is SDR */
	pos += snprintf(buf + pos, PAGE_SIZE, "SDR");

	return pos;
}

/**/
static ssize_t dc_cap_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	enum hdmi_vic vic = HDMI_0_UNKNOWN;
	int pos = 0;
	struct hdmitx_dev *hdev = get_hdmitx21_device();
	struct rx_cap *prxcap = &hdev->rxcap;
	const struct dv_info *dv = &hdev->rxcap.dv_info;
	const struct dv_info *dv2 = &hdev->rxcap.dv_info2;

	if (prxcap->dc_36bit_420)
		; // pos += snprintf(buf + pos, PAGE_SIZE, "420,12bit\n");
	if (prxcap->dc_30bit_420) {
		; // pos += snprintf(buf + pos, PAGE_SIZE, "420,10bit\n");
		; // pos += snprintf(buf + pos, PAGE_SIZE, "420,8bit\n");
	} else {
		vic = hdmitx21_edid_get_VIC(hdev, "2160p60hz420", 0);
		if (vic != HDMI_0_UNKNOWN) {
			;
			goto next444;
		}
		vic = hdmitx21_edid_get_VIC(hdev, "2160p50hz420", 0);
		if (vic != HDMI_0_UNKNOWN) {
			;
			goto next444;
		}
		vic = hdmitx21_edid_get_VIC(hdev, "smpte60hz420", 0);
		if (vic != HDMI_0_UNKNOWN) {
			;
			goto next444;
		}
		vic = hdmitx21_edid_get_VIC(hdev, "smpte50hz420", 0);
		if (vic != HDMI_0_UNKNOWN) {
			;
			goto next444;
		}
	}
next444:
	if (prxcap->dc_y444) {
		if (prxcap->dc_36bit || dv->sup_10b_12b_444 == 0x2 ||
		    dv2->sup_10b_12b_444 == 0x2)
			if (!hdev->vend_id_hit)
				pos += snprintf(buf + pos, PAGE_SIZE, "444,12bit\n");
		if (prxcap->dc_30bit || dv->sup_10b_12b_444 == 0x1 ||
		    dv2->sup_10b_12b_444 == 0x1) {
			if (!hdev->vend_id_hit)
				pos += snprintf(buf + pos, PAGE_SIZE, "444,10bit\n");
			pos += snprintf(buf + pos, PAGE_SIZE, "444,8bit\n");
		}
		if (prxcap->dc_36bit || dv->sup_yuv422_12bit ||
		    dv2->sup_yuv422_12bit)
			if (!hdev->vend_id_hit)
				pos += snprintf(buf + pos, PAGE_SIZE, "422,12bit\n");
		if (prxcap->dc_30bit) {
			if (!hdev->vend_id_hit)
				pos += snprintf(buf + pos, PAGE_SIZE, "422,10bit\n");
			pos += snprintf(buf + pos, PAGE_SIZE, "422,8bit\n");
			goto nextrgb;
		}
	} else {
		if (prxcap->native_Mode & (1 << 5))
			pos += snprintf(buf + pos, PAGE_SIZE, "444,8bit\n");
		if (prxcap->native_Mode & (1 << 4))
			pos += snprintf(buf + pos, PAGE_SIZE, "422,8bit\n");
	}
nextrgb:
	if (prxcap->dc_36bit || dv->sup_10b_12b_444 == 0x2 ||
	    dv2->sup_10b_12b_444 == 0x2)
		if (!hdev->vend_id_hit)
			;
	if (prxcap->dc_30bit || dv->sup_10b_12b_444 == 0x1 ||
	    dv2->sup_10b_12b_444 == 0x1)
		if (!hdev->vend_id_hit)
			;
	; // pos += snprintf(buf + pos, PAGE_SIZE, "rgb,8bit\n");
	return pos;
}

static bool valid_mode;
static char cvalid_mode[32];

static bool pre_process_str(char *name)
{
	int i;
	u32 flag = 0;
	char *color_format[4] = {"444", "422", "420", "rgb"};

	for (i = 0; i < 4 ; i++) {
		if (strstr(name, color_format[i]))
			flag++;
	}
	if (flag >= 2)
		return 0;
	else
		return 1;
}

static ssize_t valid_mode_show(struct device *dev,
			       struct device_attribute *attr,
			       char *buf)
{
	int pos = 0;
	struct hdmitx_dev *hdev = get_hdmitx21_device();
	struct hdmi_format_para *para = NULL;

	if (cvalid_mode[0]) {
		valid_mode = pre_process_str(cvalid_mode);
		if (valid_mode == 0) {
			pos += snprintf(buf + pos, PAGE_SIZE, "%d\n\r",
				valid_mode);
			return pos;
		}
		para = hdmitx21_tst_fmt_name(cvalid_mode, cvalid_mode);
		if (!para) {
			pos += snprintf(buf + pos, PAGE_SIZE, "0\n\r");
			return pos;
		}
	}

	valid_mode = hdmitx21_edid_check_valid_mode(hdev, para);

	pos += snprintf(buf + pos, PAGE_SIZE, "%d\n\r", valid_mode);

	return pos;
}

static ssize_t valid_mode_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	memset(cvalid_mode, 0, sizeof(cvalid_mode));
	strncpy(cvalid_mode, buf, sizeof(cvalid_mode));
	cvalid_mode[31] = '\0';
	return count;
}

static ssize_t allm_cap_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	int pos = 0;
	struct hdmitx_dev *hdev = get_hdmitx21_device();
	struct rx_cap *prxcap = &hdev->rxcap;

	pos += snprintf(buf + pos, PAGE_SIZE, "%d\n\r", prxcap->allm);
	return pos;
}

static ssize_t allm_mode_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	int pos = 0;
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	pos += snprintf(buf + pos, PAGE_SIZE, "%d\n\r", hdev->allm_mode);

	return pos;
}

static inline int com_str(const char *buf, const char *str)
{
	return strncmp(buf, str, strlen(str)) == 0;
}

static ssize_t allm_mode_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf,
			       size_t count)
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	pr_info("hdmitx: store allm_mode as %s\n", buf);

	if (com_str(buf, "0")) {
		// disable ALLM
		hdev->allm_mode = 0;
		hdmitx21_construct_vsif(hdev, VT_ALLM, 0, NULL);
		if (_is_hdmi14_4k(hdev->cur_VIC))
			hdmitx21_construct_vsif(hdev, VT_HDMI14_4K, 1, NULL);
	}
	if (com_str(buf, "1")) {
		hdev->allm_mode = 1;
		hdmitx21_construct_vsif(hdev, VT_ALLM, 1, NULL);
		hdev->hwop.cntlconfig(hdev, CONF_CT_MODE, SET_CT_OFF);
	}
	if (com_str(buf, "-1")) {
		if (hdev->allm_mode == 1) {
			hdev->allm_mode = 0;
			hdmi_vend_infoframe_set(NULL);
		}
	}
	return count;
}

static ssize_t contenttype_cap_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	int pos = 0;
	struct hdmitx_dev *hdev = get_hdmitx21_device();
	struct rx_cap *prxcap = &hdev->rxcap;

	if (prxcap->cnc0)
		pos += snprintf(buf + pos, PAGE_SIZE, "graphics\n\r");
	if (prxcap->cnc1)
		pos += snprintf(buf + pos, PAGE_SIZE, "photo\n\r");
	if (prxcap->cnc2)
		pos += snprintf(buf + pos, PAGE_SIZE, "cinema\n\r");
	if (prxcap->cnc3)
		pos += snprintf(buf + pos, PAGE_SIZE, "game\n\r");

	return pos;
}

static ssize_t contenttype_mode_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	int pos = 0;
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	if (hdev->ct_mode == 0)
		pos += snprintf(buf + pos, PAGE_SIZE, "off\n\r");
	if (hdev->ct_mode == 1)
		pos += snprintf(buf + pos, PAGE_SIZE, "game\n\r");
	if (hdev->ct_mode == 2)
		pos += snprintf(buf + pos, PAGE_SIZE, "graphics\n\r");
	if (hdev->ct_mode == 3)
		pos += snprintf(buf + pos, PAGE_SIZE, "photo\n\r");
	if (hdev->ct_mode == 4)
		pos += snprintf(buf + pos, PAGE_SIZE, "cinema\n\r");

	return pos;
}

static ssize_t contenttype_mode_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	pr_info("hdmitx: store contenttype_mode as %s\n", buf);

	if (hdev->allm_mode == 1) {
		hdev->allm_mode = 0;
		hdmitx21_construct_vsif(hdev, VT_ALLM, 0, NULL);
	}
	if (_is_hdmi14_4k(hdev->cur_VIC))
		hdmitx21_construct_vsif(hdev, VT_HDMI14_4K, 1, NULL);
	hdev->ct_mode = 0;
	hdev->hwop.cntlconfig(hdev, CONF_CT_MODE, SET_CT_OFF);

	if (com_str(buf, "1") || com_str(buf, "game")) {
		hdev->ct_mode = 1;
		hdev->hwop.cntlconfig(hdev, CONF_CT_MODE, SET_CT_GAME);
	}
	if (com_str(buf, "2") || com_str(buf, "graphics")) {
		hdev->ct_mode = 2;
		hdev->hwop.cntlconfig(hdev, CONF_CT_MODE, SET_CT_GRAPHICS);
	}
	if (com_str(buf, "3") || com_str(buf, "photo")) {
		hdev->ct_mode = 3;
		hdev->hwop.cntlconfig(hdev, CONF_CT_MODE, SET_CT_PHOTO);
	}
	if (com_str(buf, "4") || com_str(buf, "cinema")) {
		hdev->ct_mode = 4;
		hdev->hwop.cntlconfig(hdev, CONF_CT_MODE, SET_CT_CINEMA);
	}

	return count;
}

/**/
static ssize_t hdr_cap_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	int pos = 0;
	u32 i, j;
	struct hdmitx_dev *hdev = get_hdmitx21_device();
	struct rx_cap *prxcap = &hdev->rxcap;
	int hdr10plugsupported = 0;

	if (prxcap->hdr10plus_info.ieeeoui == HDR10_PLUS_IEEE_OUI &&
	    prxcap->hdr10plus_info.application_version != 0xFF)
		hdr10plugsupported = 1;
	pos += snprintf(buf + pos, PAGE_SIZE, "HDR10Plus Supported: %d\n",
		hdr10plugsupported);
	pos += snprintf(buf + pos, PAGE_SIZE, "HDR Static Metadata:\n");
	pos += snprintf(buf + pos, PAGE_SIZE, "    Supported EOTF:\n");
	pos += snprintf(buf + pos, PAGE_SIZE, "        Traditional SDR: %d\n",
		prxcap->hdr_sup_eotf_sdr);
	pos += snprintf(buf + pos, PAGE_SIZE, "        Traditional HDR: %d\n",
		prxcap->hdr_sup_eotf_hdr);
	pos += snprintf(buf + pos, PAGE_SIZE, "        SMPTE ST 2084: %d\n",
		prxcap->hdr_sup_eotf_smpte_st_2084);
	pos += snprintf(buf + pos, PAGE_SIZE, "        Hybrid Log-Gamma: %d\n",
		prxcap->hdr_sup_eotf_hlg);
	pos += snprintf(buf + pos, PAGE_SIZE, "    Supported SMD type1: %d\n",
		prxcap->hdr_sup_SMD_type1);
	pos += snprintf(buf + pos, PAGE_SIZE, "    Luminance Data\n");
	pos += snprintf(buf + pos, PAGE_SIZE, "        Max: %d\n",
		prxcap->hdr_lum_max);
	pos += snprintf(buf + pos, PAGE_SIZE, "        Avg: %d\n",
		prxcap->hdr_lum_avg);
	pos += snprintf(buf + pos, PAGE_SIZE, "        Min: %d\n\n",
		prxcap->hdr_lum_min);
	pos += snprintf(buf + pos, PAGE_SIZE, "HDR Dynamic Metadata:");

	for (i = 0; i < 4; i++) {
		if (prxcap->hdr_dynamic_info[i].type == 0)
			continue;
		pos += snprintf(buf + pos, PAGE_SIZE,
			"\n    metadata_version: %x\n",
			prxcap->hdr_dynamic_info[i].type);
		pos += snprintf(buf + pos, PAGE_SIZE,
			"        support_flags: %x\n",
			prxcap->hdr_dynamic_info[i].support_flags);
		pos += snprintf(buf + pos, PAGE_SIZE,
			"        optional_fields:");
		for (j = 0; j <
			(prxcap->hdr_dynamic_info[i].hd_len - 3); j++)
			pos += snprintf(buf + pos, PAGE_SIZE, " %x",
				prxcap->hdr_dynamic_info[i].optional_fields[j]);
	}

	pos += snprintf(buf + pos, PAGE_SIZE, "\n\ncolorimetry_data: %x\n",
		prxcap->colorimetry_data);

	return pos;
}

static ssize_t _show_dv_cap(struct device *dev,
			    struct device_attribute *attr,
			    char *buf,
			    const struct dv_info *dv)
{
	int pos = 0;
	int i;

	if (dv->ieeeoui != DV_IEEE_OUI) {
		pos += snprintf(buf + pos, PAGE_SIZE,
			"The Rx don't support DolbyVision\n");
		return pos;
	}
	pos += snprintf(buf + pos, PAGE_SIZE,
		"DolbyVision RX support list:\n");

	if (dv->ver == 0) {
		pos += snprintf(buf + pos, PAGE_SIZE,
			"VSVDB Version: V%d\n", dv->ver);
		pos += snprintf(buf + pos, PAGE_SIZE,
			"2160p%shz: 1\n",
			dv->sup_2160p60hz ? "60" : "30");
		pos += snprintf(buf + pos, PAGE_SIZE,
			"Support mode:\n");
		pos += snprintf(buf + pos, PAGE_SIZE,
			"  DV_RGB_444_8BIT\n");
		if (dv->sup_yuv422_12bit)
			pos += snprintf(buf + pos, PAGE_SIZE,
				"  DV_YCbCr_422_12BIT\n");
	}
	if (dv->ver == 1) {
		pos += snprintf(buf + pos, PAGE_SIZE,
			"VSVDB Version: V%d(%d-byte)\n",
			dv->ver, dv->length + 1);
		if (dv->length == 0xB) {
			pos += snprintf(buf + pos, PAGE_SIZE,
				"2160p%shz: 1\n",
				dv->sup_2160p60hz ? "60" : "30");
		pos += snprintf(buf + pos, PAGE_SIZE,
			"Support mode:\n");
		pos += snprintf(buf + pos, PAGE_SIZE,
			"  DV_RGB_444_8BIT\n");
		if (dv->sup_yuv422_12bit)
			pos += snprintf(buf + pos, PAGE_SIZE,
			"  DV_YCbCr_422_12BIT\n");
		if (dv->low_latency == 0x01)
			pos += snprintf(buf + pos, PAGE_SIZE,
				"  LL_YCbCr_422_12BIT\n");
		}

		if (dv->length == 0xE) {
			pos += snprintf(buf + pos, PAGE_SIZE,
				"2160p%shz: 1\n",
				dv->sup_2160p60hz ? "60" : "30");
			pos += snprintf(buf + pos, PAGE_SIZE,
				"Support mode:\n");
			pos += snprintf(buf + pos, PAGE_SIZE,
				"  DV_RGB_444_8BIT\n");
			if (dv->sup_yuv422_12bit)
				pos += snprintf(buf + pos, PAGE_SIZE,
				"  DV_YCbCr_422_12BIT\n");
		}
	}
	if (dv->ver == 2) {
		pos += snprintf(buf + pos, PAGE_SIZE,
			"VSVDB Version: V%d\n", dv->ver);
		pos += snprintf(buf + pos, PAGE_SIZE,
			"2160p%shz: 1\n",
			dv->sup_2160p60hz ? "60" : "30");
		pos += snprintf(buf + pos, PAGE_SIZE,
			"Support mode:\n");
		if (dv->Interface != 0x00 && dv->Interface != 0x01) {
			pos += snprintf(buf + pos, PAGE_SIZE,
				"  DV_RGB_444_8BIT\n");
			if (dv->sup_yuv422_12bit)
				pos += snprintf(buf + pos, PAGE_SIZE,
					"  DV_YCbCr_422_12BIT\n");
		}
		pos += snprintf(buf + pos, PAGE_SIZE,
			"  LL_YCbCr_422_12BIT\n");
		if (dv->Interface == 0x01 || dv->Interface == 0x03) {
			if (dv->sup_10b_12b_444 == 0x1) {
				pos += snprintf(buf + pos, PAGE_SIZE,
					"  LL_RGB_444_10BIT\n");
			}
			if (dv->sup_10b_12b_444 == 0x2) {
				pos += snprintf(buf + pos, PAGE_SIZE,
					"  LL_RGB_444_12BIT\n");
			}
		}
	}
	pos += snprintf(buf + pos, PAGE_SIZE,
		"IEEEOUI: 0x%06x\n", dv->ieeeoui);
	pos += snprintf(buf + pos, PAGE_SIZE,
		"EMP: %d\n", dv->dv_emp_cap);
	pos += snprintf(buf + pos, PAGE_SIZE, "VSVDB: ");
	for (i = 0; i < (dv->length + 1); i++)
		pos += snprintf(buf + pos, PAGE_SIZE, "%02x",
		dv->rawdata[i]);
	pos += snprintf(buf + pos, PAGE_SIZE, "\n");
	return pos;
}

static ssize_t dv_cap_show(struct device *dev,
			   struct device_attribute *attr,
			   char *buf)
{
	int pos = 0;
	struct hdmitx_dev *hdev = get_hdmitx21_device();
	const struct dv_info *dv = &hdev->rxcap.dv_info;

	if (dv->ieeeoui != DV_IEEE_OUI || hdev->hdr_priority) {
		pos += snprintf(buf + pos, PAGE_SIZE,
			"The Rx don't support DolbyVision\n");
		return pos;
	}
	return _show_dv_cap(dev, attr, buf, dv);
}

static ssize_t dv_cap2_show(struct device *dev,
			    struct device_attribute *attr,
			    char *buf)
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();
	const struct dv_info *dv2 = &hdev->rxcap.dv_info2;

	return _show_dv_cap(dev, attr, buf, dv2);
}

/*
 *  1: set avmute
 * -1: clear avmute
 * 0: off avmute
 */
static ssize_t avmute_store(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t count)
{
	int cmd = OFF_AVMUTE;
	struct hdmitx_dev *hdev = get_hdmitx21_device();
	static int mask0;
	static int mask1;
	static DEFINE_MUTEX(avmute_mutex);

	pr_info("%s %s\n", __func__, buf);
	mutex_lock(&avmute_mutex);
	if (strncmp(buf, "-1", 2) == 0) {
		cmd = CLR_AVMUTE;
		mask0 = -1;
	} else if (strncmp(buf, "0", 1) == 0) {
		cmd = OFF_AVMUTE;
		mask0 = 0;
	} else if (strncmp(buf, "1", 1) == 0) {
		cmd = SET_AVMUTE;
		mask0 = 1;
	}
	if (strncmp(buf, "r-1", 3) == 0) {
		cmd = CLR_AVMUTE;
		mask1 = -1;
	} else if (strncmp(buf, "r0", 2) == 0) {
		cmd = OFF_AVMUTE;
		mask1 = 0;
	} else if (strncmp(buf, "r1", 2) == 0) {
		cmd = SET_AVMUTE;
		mask1 = 1;
	}
	if (mask0 == 1 || mask1 == 1)
		cmd = SET_AVMUTE;
	else if ((mask0 == -1) && (mask1 == -1))
		cmd = CLR_AVMUTE;
	hdev->hwop.cntlmisc(hdev, MISC_AVMUTE_OP, cmd);
	mutex_unlock(&avmute_mutex);

	return count;
}

static ssize_t avmute_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();
	int ret = 0;
	int pos = 0;

	ret = hdev->hwop.cntlmisc(hdev, MISC_READ_AVMUTE_OP, 0);
	pos += snprintf(buf + pos, PAGE_SIZE, "%d", ret);

	return pos;
}

/*
 *  1: enable hdmitx phy
 * 0: disable hdmitx phy
 */
static ssize_t phy_store(struct device *dev,
			 struct device_attribute *attr,
			 const char *buf, size_t count)
{
	int cmd = TMDS_PHY_ENABLE;
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	pr_info("%s %s\n", __func__, buf);

	if (strncmp(buf, "0", 1) == 0)
		cmd = TMDS_PHY_DISABLE;
	else if (strncmp(buf, "1", 1) == 0)
		cmd = TMDS_PHY_ENABLE;
	else
		pr_info("set phy wrong: %s\n", buf);

	hdev->hwop.cntlmisc(hdev, MISC_TMDS_PHY_OP, cmd);
	return count;
}

static ssize_t phy_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	return 0;
}

static ssize_t rxsense_policy_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	int val = 0;
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	if (isdigit(buf[0])) {
		val = buf[0] - '0';
		pr_info("hdmitx: set rxsense_policy as %d\n", val);
		if (val == 0 || val == 1)
			hdev->rxsense_policy = val;
		else
			pr_info("only accept as 0 or 1\n");
	}
	if (hdev->rxsense_policy)
		queue_delayed_work(hdev->rxsense_wq,
				   &hdev->work_rxsense, 0);
	else
		cancel_delayed_work(&hdev->work_rxsense);

	return count;
}

static ssize_t rxsense_policy_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	int pos = 0;
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	pos += snprintf(buf + pos, PAGE_SIZE, "%d\n",
		hdev->rxsense_policy);

	return pos;
}

/* cedst_policy: 0, no CED feature
 *	       1, auto mode, depends on RX scdc_present
 *	       2, forced CED feature
 */
static ssize_t cedst_policy_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf,
				  size_t count)
{
	int val = 0;
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	if (isdigit(buf[0])) {
		val = buf[0] - '0';
		pr_info("hdmitx: set cedst_policy as %d\n", val);
		if (val == 0 || val == 1 || val == 2) {
			hdev->cedst_policy = val;
			if (val == 1) { /* Auto mode, depends on Rx */
				/* check RX scdc_present */
				if (hdev->rxcap.scdc_present)
					hdev->cedst_policy = 1;
				else
					hdev->cedst_policy = 0;
			}
			if (val == 2) /* Force mode */
				hdev->cedst_policy = 1;
			/* assgin cedst_en from dts or here */
			hdev->cedst_en = hdev->cedst_policy;
		} else {
			pr_info("only accept as 0, 1(auto), or 2(force)\n");
		}
	}
	if (hdev->cedst_policy)
		queue_delayed_work(hdev->cedst_wq, &hdev->work_cedst, 0);
	else
		cancel_delayed_work(&hdev->work_cedst);

	return count;
}

static ssize_t cedst_policy_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buf)
{
	int pos = 0;
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	pos += snprintf(buf + pos, PAGE_SIZE, "%d\n",
		hdev->cedst_policy);

	return pos;
}

static ssize_t cedst_count_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	int pos = 0;
	struct hdmitx_dev *hdev = get_hdmitx21_device();
	struct ced_cnt *ced = &hdev->ced_cnt;
	struct scdc_locked_st *ch_st = &hdev->chlocked_st;

	if (!ch_st->clock_detected)
		pos += snprintf(buf + pos, PAGE_SIZE, "clock undetected\n");
	if (!ch_st->ch0_locked)
		pos += snprintf(buf + pos, PAGE_SIZE, "CH0 unlocked\n");
	if (!ch_st->ch1_locked)
		pos += snprintf(buf + pos, PAGE_SIZE, "CH1 unlocked\n");
	if (!ch_st->ch2_locked)
		pos += snprintf(buf + pos, PAGE_SIZE, "CH2 unlocked\n");
	if (ced->ch0_valid && ced->ch0_cnt)
		pos += snprintf(buf + pos, PAGE_SIZE, "CH0 ErrCnt 0x%x\n",
			ced->ch0_cnt);
	if (ced->ch1_valid && ced->ch1_cnt)
		pos += snprintf(buf + pos, PAGE_SIZE, "CH1 ErrCnt 0x%x\n",
			ced->ch1_cnt);
	if (ced->ch2_valid && ced->ch2_cnt)
		pos += snprintf(buf + pos, PAGE_SIZE, "CH2 ErrCnt 0x%x\n",
			ced->ch2_cnt);
	memset(ced, 0, sizeof(*ced));

	return pos;
}

static ssize_t sspll_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf,
			   size_t count)
{
	int val = 0;
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	if (isdigit(buf[0])) {
		val = buf[0] - '0';
		pr_info("set sspll : %d\n", val);
		if (val == 0 || val == 1)
			hdev->sspll = val;
		else
			pr_info("sspll only accept as 0 or 1\n");
	}

	return count;
}

static ssize_t sspll_show(struct device *dev,
			  struct device_attribute *attr,
			  char *buf)
{
	int pos = 0;
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	pos += snprintf(buf + pos, PAGE_SIZE, "%d\n",
		hdev->sspll);

	return pos;
}

static ssize_t frac_rate_policy_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf,
				      size_t count)
{
	int val = 0;
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	if (isdigit(buf[0])) {
		val = buf[0] - '0';
		pr_info("set frac_rate_policy as %d\n", val);
		if (val == 0 || val == 1)
			hdev->frac_rate_policy = val;
		else
			pr_info("only accept as 0 or 1\n");
	}

	return count;
}

static ssize_t frac_rate_policy_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	int pos = 0;
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	pos += snprintf(buf + pos, PAGE_SIZE, "%d\n",
		hdev->frac_rate_policy);

	return pos;
}

static ssize_t hdcp_type_policy_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	return count;
}

static ssize_t hdcp_type_policy_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	int pos = 0;

	return pos;
}

static ssize_t hdcp_lstore_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	int pos = 0;
	int lstore = 0;

	if (get_hdcp2_lstore())
		lstore |= BIT(1);
	if (get_hdcp1_lstore())
		lstore |= BIT(0);

	if ((lstore & BIT(1)) && (lstore & BIT(0))) {
		pos += snprintf(buf + pos, PAGE_SIZE, "22+14\n");
		return pos;
	}
	if (lstore & BIT(1))
		pos += snprintf(buf + pos, PAGE_SIZE, "22\n");
	if (lstore & BIT(0))
		pos += snprintf(buf + pos, PAGE_SIZE, "14\n");

	return pos;
}

static ssize_t hdcp_lstore_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	return count;
}

static ssize_t div40_show(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	int pos = 0;
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	pos += snprintf(buf + pos, PAGE_SIZE, "%d\n", hdev->div40);

	return pos;
}

static ssize_t div40_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	hdev->hwop.cntlddc(hdev, DDC_SCDC_DIV40_SCRAMB, buf[0] == '1');
	hdev->div40 = (buf[0] == '1');

	return count;
}

static ssize_t hdcp_mode_show(struct device *dev,
			      struct device_attribute *attr,
			      char *buf)
{
	int pos = 0;
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	pos += snprintf(buf + pos, PAGE_SIZE, "%d\n", hdev->hdcp_mode);

	return pos;
}

static ssize_t hdcp_mode_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	if (strncmp(buf, "f1", 2) == 0) {
		hdev->hdcp_mode = 0x11;
		hdcp_mode_set(1);
	}
	if (strncmp(buf, "f2", 2) == 0) {
		hdev->hdcp_mode = 0x12;
		hdcp_mode_set(2);
	}
	if (buf[0] == '0') {
		hdev->hdcp_mode = 0x00;
		hdcp_mode_set(0);
	}

	return count;
}

/* Indicate whether a rptx under repeater */
static ssize_t hdmi_repeater_tx_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	int pos = 0;
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	pos += snprintf(buf + pos, PAGE_SIZE, "%d\n",
		!!hdev->repeater_tx);

	return pos;
}

#include <linux/amlogic/media/vout/hdmi_tx/hdmi_rptx.h>

void direct21_hdcptx14_opr(enum rptx_hdcp14_cmd cmd, void *args)
{
}
EXPORT_SYMBOL(direct21_hdcptx14_opr);

/* Special FBC check */
static int check_fbc_special(u8 *edid_dat)
{
	if (edid_dat[250] == 0xfb && edid_dat[251] == 0x0c)
		return 1;
	else
		return 0;
}

static ssize_t hpd_state_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	int pos = 0;
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	pos += snprintf(buf + pos, PAGE_SIZE, "%d",
		hdev->hpd_state);
	return pos;
}

static ssize_t hdmi_used_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	int pos = 0;
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	pos += snprintf(buf + pos, PAGE_SIZE, "%d",
		hdev->already_used);
	return pos;
}

static ssize_t fake_plug_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	return snprintf(buf, PAGE_SIZE, "%d", hdev->hpd_state);
}

static ssize_t fake_plug_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	pr_info("hdmitx: fake plug %s\n", buf);

	if (strncmp(buf, "1", 1) == 0)
		hdev->hpd_state = 1;

	if (strncmp(buf, "0", 1) == 0)
		hdev->hpd_state = 0;

	hdmitx21_set_uevent(HDMITX_HPD_EVENT, hdev->hpd_state);
	return count;
}

static ssize_t ready_show(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	int pos = 0;
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	pos += snprintf(buf + pos, PAGE_SIZE, "%d\r\n",
		hdev->ready);
	return pos;
}

static ssize_t ready_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	if (strncmp(buf, "0", 1) == 0)
		hdev->ready = 0;
	if (strncmp(buf, "1", 1) == 0)
		hdev->ready = 1;
	return count;
}

static ssize_t support_3d_show(struct device *dev,
			       struct device_attribute *attr,
			       char *buf)
{
	int pos = 0;
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	pos += snprintf(buf + pos, PAGE_SIZE, "%d\n",
			hdev->rxcap.threeD_present);
	return pos;
}

static ssize_t hdmitx21_show(struct device *dev,
			       struct device_attribute *attr,
			       char *buf)
{
	return snprintf(buf, PAGE_SIZE, "1\n");
}

static DEVICE_ATTR_RW(disp_mode);
static DEVICE_ATTR_RW(attr);
static DEVICE_ATTR_RW(vid_mute);
static DEVICE_ATTR_RW(edid);
static DEVICE_ATTR_RO(rawedid);
static DEVICE_ATTR_RO(sink_type);
static DEVICE_ATTR_RO(edid_parsing);
static DEVICE_ATTR_RW(config);
static DEVICE_ATTR_WO(debug);
static DEVICE_ATTR_RO(disp_cap);
static DEVICE_ATTR_RO(preferred_mode);
static DEVICE_ATTR_RO(cea_cap);
static DEVICE_ATTR_RO(vesa_cap);
static DEVICE_ATTR_RO(aud_cap);
static DEVICE_ATTR_RO(hdmi_hdr_status);
static DEVICE_ATTR_RO(hdr_cap);
static DEVICE_ATTR_RO(dv_cap);
static DEVICE_ATTR_RO(dv_cap2);
static DEVICE_ATTR_RO(dc_cap);
static DEVICE_ATTR_RW(valid_mode);
static DEVICE_ATTR_RO(allm_cap);
static DEVICE_ATTR_RW(allm_mode);
static DEVICE_ATTR_RO(contenttype_cap);
static DEVICE_ATTR_RW(contenttype_mode);
static DEVICE_ATTR_RW(avmute);
static DEVICE_ATTR_RW(phy);
static DEVICE_ATTR_RW(sspll);
static DEVICE_ATTR_RW(frac_rate_policy);
static DEVICE_ATTR_RW(rxsense_policy);
static DEVICE_ATTR_RW(cedst_policy);
static DEVICE_ATTR_RO(cedst_count);
static DEVICE_ATTR_RW(hdcp_mode);
static DEVICE_ATTR_RW(hdcp_type_policy);
static DEVICE_ATTR_RW(hdcp_lstore);
static DEVICE_ATTR_RO(hdmi_repeater_tx);
static DEVICE_ATTR_RW(div40);
static DEVICE_ATTR_RO(hpd_state);
static DEVICE_ATTR_RO(hdmi_used);
static DEVICE_ATTR_RW(fake_plug);
static DEVICE_ATTR_RW(ready);
static DEVICE_ATTR_RO(support_3d);
static DEVICE_ATTR_RO(hdmitx21);

#ifdef CONFIG_AMLOGIC_VOUT_SERVE
static struct vinfo_s *hdmitx_get_current_vinfo(void *data)
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	if (!hdev->para)
		return NULL;
	return &hdev->para->hdmitx_vinfo;
}

/* fr_tab[]
 * 1080p24hz, 24:1
 * 1080p23.976hz, 2997:125
 * 25/50/100/200hz, no change
 */
static struct frac_rate_table fr_tab[] = {
	{"24hz", 24, 1, 2997, 125},
	{"30hz", 30, 1, 2997, 100},
	{"60hz", 60, 1, 2997, 50},
	{"120hz", 120, 1, 2997, 25},
	{"240hz", 120, 1, 5994, 25},
	{NULL},
};

static void recalc_vinfo_sync_duration(struct vinfo_s *info, u32 frac)
{
	struct frac_rate_table *fr = &fr_tab[0];

	pr_info("hdmitx: recalc before %s %d %d, frac %d\n", info->name,
		info->sync_duration_num, info->sync_duration_den, info->frac);

	while (fr->hz) {
		if (strstr(info->name, fr->hz)) {
			if (frac) {
				info->sync_duration_num = fr->sync_num_dec;
				info->sync_duration_den = fr->sync_den_dec;
				info->frac = 1;
			} else {
				info->sync_duration_num = fr->sync_num_int;
				info->sync_duration_den = fr->sync_den_int;
				info->frac = 0;
			}
			break;
		}
		fr++;
	}

	pr_info("recalc after %s %d %d, frac %d\n", info->name,
		info->sync_duration_num, info->sync_duration_den, info->frac);
}

static int hdmitx_set_current_vmode(enum vmode_e mode, void *data)
{
	struct vinfo_s *vinfo;
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	/* get current vinfo and refesh */
	vinfo = hdmitx_get_current_vinfo(NULL);
	if (vinfo && vinfo->name)
		recalc_vinfo_sync_duration(vinfo,
					   hdev->frac_rate_policy);

	if (!(mode & VMODE_INIT_BIT_MASK)) {
		set_disp_mode_auto();
	} else {
		pr_info("alread display in uboot\n");
		update_current_para(hdev);
		edidinfo_attach_to_vinfo(hdev);
	}
	return 0;
}

static enum vmode_e hdmitx_validate_vmode(char *_mode, u32 frac, void *data)
{
	struct hdmi_format_para *para = NULL;
	struct hdmitx_dev *hdev = get_hdmitx21_device();
	char mode[32] = {0};

	strncpy(mode, _mode, sizeof(mode));
	mode[31] = 0;

	para = hdmitx21_get_fmtpara(mode, hdev->fmt_attr);

	if (para) {
		/* //remove frac support for vout api
		 *if (frac)
		 *	hdev->frac_rate_policy = 1;
		 *else
		 *	hdev->frac_rate_policy = 0;
		 */
		hdev->para->hdmitx_vinfo.info_3d = NON_3D;
		hdev->para->hdmitx_vinfo.vout_device = &hdmitx_vdev;
		return VMODE_HDMI;
	}
	return VMODE_MAX;
}

static int hdmitx_vmode_is_supported(enum vmode_e mode, void *data)
{
	if ((mode & VMODE_MODE_BIT_MASK) == VMODE_HDMI)
		return true;
	else
		return false;
}

static int hdmitx_module_disable(enum vmode_e cur_vmod, void *data)
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	hdev->hwop.cntlconfig(hdev, CONF_CLR_AVI_PACKET, 0);
	hdev->hwop.cntlconfig(hdev, CONF_CLR_VSDB_PACKET, 0);
	hdev->hwop.cntlmisc(hdev, MISC_TMDS_PHY_OP, TMDS_PHY_DISABLE);
	hdmitx21_disable_clk(hdev);
	hdev->para = hdmitx21_tst_fmt_name("invalid", hdev->fmt_attr);
	hdmitx_validate_vmode("null", 0, NULL);
	if (hdev->cedst_policy)
		cancel_delayed_work(&hdev->work_cedst);
	if (hdev->rxsense_policy)
		queue_delayed_work(hdev->rxsense_wq, &hdev->work_rxsense, 0);

	return 0;
}

static int hdmitx_vout_state;
static int hdmitx_vout_set_state(int index, void *data)
{
	hdmitx_vout_state |= (1 << index);
	return 0;
}

static int hdmitx_vout_clr_state(int index, void *data)
{
	hdmitx_vout_state &= ~(1 << index);
	return 0;
}

static int hdmitx_vout_get_state(void *data)
{
	return hdmitx_vout_state;
}

/* if cs/cd/frac_rate is changed, then return 0 */
static int hdmitx_check_same_vmodeattr(char *name, void *data)
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	if (memcmp(hdev->backup_fmt_attr, hdev->fmt_attr, 16) == 0 &&
	    hdev->backup_frac_rate_policy == hdev->frac_rate_policy)
		return 1;
	memcpy(hdev->backup_fmt_attr, hdev->fmt_attr, 16);
	hdev->backup_frac_rate_policy = hdev->frac_rate_policy;
	return 0;
}

static int hdmitx_vout_get_disp_cap(char *buf, void *data)
{
	return disp_cap_show(NULL, NULL, buf);
}

static void hdmitx_set_bist(u32 num, void *data)
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	if (hdev->hwop.debug_bist)
		hdev->hwop.debug_bist(hdev, num);
}

static struct vout_server_s hdmitx_vout_server = {
	.name = "hdmitx_vout_server",
	.op = {
		.get_vinfo = hdmitx_get_current_vinfo,
		.set_vmode = hdmitx_set_current_vmode,
		.validate_vmode = hdmitx_validate_vmode,
		.check_same_vmodeattr = hdmitx_check_same_vmodeattr,
		.vmode_is_supported = hdmitx_vmode_is_supported,
		.disable = hdmitx_module_disable,
		.set_state = hdmitx_vout_set_state,
		.clr_state = hdmitx_vout_clr_state,
		.get_state = hdmitx_vout_get_state,
		.get_disp_cap = hdmitx_vout_get_disp_cap,
		.set_vframe_rate_hint = NULL,
		.get_vframe_rate_hint = NULL,
		.set_bist = hdmitx_set_bist,
#ifdef CONFIG_PM
		.vout_suspend = NULL,
		.vout_resume = NULL,
#endif
	},
	.data = NULL,
};
#endif

#ifdef CONFIG_AMLOGIC_VOUT2_SERVE
static struct vout_server_s hdmitx_vout2_server = {
	.name = "hdmitx_vout2_server",
	.op = {
		.get_vinfo = hdmitx_get_current_vinfo,
		.set_vmode = hdmitx_set_current_vmode,
		.validate_vmode = hdmitx_validate_vmode,
		.check_same_vmodeattr = hdmitx_check_same_vmodeattr,
		.vmode_is_supported = hdmitx_vmode_is_supported,
		.disable = hdmitx_module_disable,
		.set_state = hdmitx_vout_set_state,
		.clr_state = hdmitx_vout_clr_state,
		.get_state = hdmitx_vout_get_state,
		.get_disp_cap = hdmitx_vout_get_disp_cap,
		.set_vframe_rate_hint = NULL,
		.get_vframe_rate_hint = NULL,
		.set_bist = hdmitx_set_bist,
#ifdef CONFIG_PM
		.vout_suspend = NULL,
		.vout_resume = NULL,
#endif
	},
	.data = NULL,
};
#endif

#if IS_ENABLED(CONFIG_AMLOGIC_SND_SOC)

#include <linux/soundcard.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/initval.h>
#include <sound/control.h>

static struct rate_map_fs map_fs[] = {
	{0,	  FS_REFER_TO_STREAM},
	{32000,  FS_32K},
	{44100,  FS_44K1},
	{48000,  FS_48K},
	{88200,  FS_88K2},
	{96000,  FS_96K},
	{176400, FS_176K4},
	{192000, FS_192K},
};

static enum hdmi_audio_fs aud_samp_rate_map(u32 rate)
{
	int i = 0;

	for (i = 0; i < ARRAY_SIZE(map_fs); i++) {
		if (map_fs[i].rate == rate)
			return map_fs[i].fs;
	}
	pr_info("get FS_MAX\n");
	return FS_MAX;
}

static u8 *aud_type_string[] = {
	"CT_REFER_TO_STREAM",
	"CT_PCM",
	"CT_AC_3",
	"CT_MPEG1",
	"CT_MP3",
	"CT_MPEG2",
	"CT_AAC",
	"CT_DTS",
	"CT_ATRAC",
	"CT_ONE_BIT_AUDIO",
	"CT_DOLBY_D",
	"CT_DTS_HD",
	"CT_MAT",
	"CT_DST",
	"CT_WMA",
	"CT_MAX",
};

static struct size_map aud_size_map_ss[] = {
	{0,	 SS_REFER_TO_STREAM},
	{16,	SS_16BITS},
	{20,	SS_20BITS},
	{24,	SS_24BITS},
	{32,	SS_MAX},
};

static enum hdmi_audio_sampsize aud_size_map(u32 bits)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(aud_size_map_ss); i++) {
		if (bits == aud_size_map_ss[i].sample_bits)
			return aud_size_map_ss[i].ss;
	}
	pr_info("get SS_MAX\n");
	return SS_MAX;
}

static int hdmitx_notify_callback_a(struct notifier_block *block,
				    unsigned long cmd, void *para);
static struct notifier_block hdmitx_notifier_nb_a = {
	.notifier_call	= hdmitx_notify_callback_a,
};

static int hdmitx_notify_callback_a(struct notifier_block *block,
				    unsigned long cmd, void *para)
{
	int i, audio_check = 0;
	struct hdmitx_dev *hdev = get_hdmitx21_device();
	struct rx_cap *prxcap = &hdev->rxcap;
	struct aud_para *aud_param = (struct aud_para *)para;
	struct hdmitx_audpara *audio_param = &hdev->cur_audio_param;
	enum hdmi_audio_fs n_rate = aud_samp_rate_map(aud_param->rate);
	enum hdmi_audio_sampsize n_size = aud_size_map(aud_param->size);

	hdev->audio_param_update_flag = 0;
	hdev->audio_notify_flag = 0;

	if (audio_param->sample_rate != n_rate) {
		audio_param->sample_rate = n_rate;
		hdev->audio_param_update_flag = 1;
	}

	if (audio_param->type != cmd) {
		audio_param->type = cmd;
		pr_info("aout notify format %s\n",
			aud_type_string[audio_param->type & 0xff]);
		hdev->audio_param_update_flag = 1;
	}

	if (audio_param->sample_size != n_size) {
		audio_param->sample_size = n_size;
		hdev->audio_param_update_flag = 1;
	}

	if (audio_param->channel_num != (aud_param->chs - 1)) {
		audio_param->channel_num = aud_param->chs - 1;
		hdev->audio_param_update_flag = 1;
	}
	if (hdev->tx_aud_cfg == 2) {
		pr_info("auto mode\n");
	/* Detect whether Rx is support current audio format */
	for (i = 0; i < prxcap->AUD_count; i++) {
		if (prxcap->RxAudioCap[i].audio_format_code == cmd)
			audio_check = 1;
	}
	/* sink don't support current audio mode */
	if (!audio_check && cmd != CT_PCM) {
		pr_info("Sink not support this audio format %lu\n",
			cmd);
		hdev->hwop.cntlconfig(hdev,
			CONF_AUDIO_MUTE_OP, AUDIO_MUTE);
		hdev->audio_param_update_flag = 0;
	}
	}
	if (hdev->audio_param_update_flag == 0)
		;
	else
		hdev->audio_notify_flag = 1;

	if ((!(hdev->hdmi_audio_off_flag)) && hdev->audio_param_update_flag) {
		/* plug-in & update audio param */
		if (hdev->hpd_state == 1) {
			hdmitx21_set_audio(hdev,
					 &hdev->cur_audio_param);
			if (hdev->audio_notify_flag == 1 || hdev->audio_step == 1) {
				hdev->audio_notify_flag = 0;
				hdev->audio_step = 0;
			}
			hdev->audio_param_update_flag = 0;
			pr_info("set audio param\n");
		}
	}
	if (aud_param->fifo_rst)
		; /* hdev->hwop.cntlmisc(hdev, MISC_AUDIO_RESET, 1); */

	return 0;
}

#endif
u32 hdmitx21_check_edid_all_zeros(u8 *buf)
{
	u32 i = 0, j = 0;
	u32 chksum = 0;

	for (j = 0; j < EDID_MAX_BLOCK; j++) {
		chksum = 0;
		for (i = 0; i < 128; i++)
			chksum += buf[i + j * 128];
		if (chksum != 0)
			return 0;
	}
	return 1;
}

static void hdmitx_get_edid(struct hdmitx_dev *hdev)
{
	mutex_lock(&getedid_mutex);
	/* TODO hdmitx21_edid_ram_buffer_clear(hdev); */
	hdev->hwop.cntlddc(hdev, DDC_RESET_EDID, 0);
	hdev->hwop.cntlddc(hdev, DDC_PIN_MUX_OP, PIN_MUX);
	/* start reading edid frist time */
	hdev->hwop.cntlddc(hdev, DDC_EDID_READ_DATA, 0);
	hdev->hwop.cntlddc(hdev, DDC_EDID_GET_DATA, 0);
	if (hdmitx21_check_edid_all_zeros(hdev->EDID_buf)) {
		hdev->hwop.cntlddc(hdev, DDC_GLITCH_FILTER_RESET, 0);
		hdev->hwop.cntlddc(hdev, DDC_EDID_READ_DATA, 0);
		hdev->hwop.cntlddc(hdev, DDC_EDID_GET_DATA, 0);
	}
	/* If EDID is not correct at first time, then retry */
	if (!check21_dvi_hdmi_edid_valid(hdev->EDID_buf)) {
		msleep(100);
		/* start reading edid second time */
		hdev->hwop.cntlddc(hdev, DDC_EDID_READ_DATA, 0);
		hdev->hwop.cntlddc(hdev, DDC_EDID_GET_DATA, 1);
		if (hdmitx21_check_edid_all_zeros(hdev->EDID_buf1)) {
			hdev->hwop.cntlddc(hdev, DDC_GLITCH_FILTER_RESET, 0);
			hdev->hwop.cntlddc(hdev, DDC_EDID_READ_DATA, 0);
			hdev->hwop.cntlddc(hdev, DDC_EDID_GET_DATA, 1);
		}
	}
	hdmitx21_edid_clear(hdev);
	hdmitx21_edid_parse(hdev);
	hdmitx21_edid_buf_compare_print(hdev);

	if (hdev->hdr_priority) { /* clear dv_info */
		struct dv_info *dv = &hdev->rxcap.dv_info;

		memset(dv, 0, sizeof(struct dv_info));
		pr_info("clear dv_info\n");
	}
	mutex_unlock(&getedid_mutex);
}

static void hdmitx_rxsense_process(struct work_struct *work)
{
	int sense;
	struct hdmitx_dev *hdev = container_of((struct delayed_work *)work,
		struct hdmitx_dev, work_rxsense);

	sense = hdev->hwop.cntlmisc(hdev, MISC_TMDS_RXSENSE, 0);
	hdmitx21_set_uevent(HDMITX_RXSENSE_EVENT, sense);
	queue_delayed_work(hdev->rxsense_wq, &hdev->work_rxsense, HZ);
}

static void hdmitx_cedst_process(struct work_struct *work)
{
	int ced;
	struct hdmitx_dev *hdev = container_of((struct delayed_work *)work,
		struct hdmitx_dev, work_cedst);

	ced = hdev->hwop.cntlmisc(hdev, MISC_TMDS_CEDST, 0);
	/* firstly send as 0, then real ced, A trigger signal */
	hdmitx21_set_uevent(HDMITX_CEDST_EVENT, 0);
	hdmitx21_set_uevent(HDMITX_CEDST_EVENT, ced);
	queue_delayed_work(hdev->cedst_wq, &hdev->work_cedst, HZ);
	queue_delayed_work(hdev->cedst_wq, &hdev->work_cedst, HZ);
}

static void hdmitx_hpd_plugin_handler(struct work_struct *work)
{
	struct vinfo_s *info = NULL;
	struct hdmitx_dev *hdev = container_of((struct delayed_work *)work,
		struct hdmitx_dev, work_hpd_plugin);

	mutex_lock(&setclk_mutex);
	hdev->already_used = 1;
	if (!(hdev->hdmitx_event & (HDMI_TX_HPD_PLUGIN))) {
		mutex_unlock(&setclk_mutex);
		return;
	}
	if (hdev->rxsense_policy) {
		cancel_delayed_work(&hdev->work_rxsense);
		queue_delayed_work(hdev->rxsense_wq, &hdev->work_rxsense, 0);
	}
	pr_info("plugin\n");
	hdev->hwop.cntlmisc(hdev, MISC_I2C_RESET, 0);
	hdev->hdmitx_event &= ~HDMI_TX_HPD_PLUGIN;
	/* start reading E-EDID */
	if (hdev->repeater_tx)
		rx_repeat_hpd_state(1);
	hdmitx_get_edid(hdev);
	hdev->cedst_policy = hdev->cedst_en & hdev->rxcap.scdc_present;
	hdmi_physical_size_update(hdev);
	if (hdev->rxcap.ieeeoui != HDMI_IEEE_OUI)
		hdev->hwop.cntlconfig(hdev,
			CONF_HDMI_DVI_MODE, DVI_MODE);
	else
		hdev->hwop.cntlconfig(hdev,
			CONF_HDMI_DVI_MODE, HDMI_MODE);
	mutex_lock(&getedid_mutex);
	mutex_unlock(&getedid_mutex);
	if (hdev->repeater_tx) {
		if (check_fbc_special(&hdev->EDID_buf[0]) ||
		    check_fbc_special(&hdev->EDID_buf1[0]))
			rx_set_repeater_support(0);
		else
			rx_set_repeater_support(1);
	}

	info = hdmitx_get_current_vinfo(NULL);
	if (info && info->mode == VMODE_HDMI)
		hdmitx21_set_audio(hdev, &hdev->cur_audio_param);
	hdev->hpd_state = 1;
	hdmitx_notify_hpd(hdev->hpd_state);

	hdmitx21_set_uevent(HDMITX_HPD_EVENT, 1);
	hdmitx21_set_uevent(HDMITX_AUDIO_EVENT, 1);
	/* Should be started at end of output */
	cancel_delayed_work(&hdev->work_cedst);
	if (hdev->cedst_policy)
		queue_delayed_work(hdev->cedst_wq, &hdev->work_cedst, 0);
	mutex_unlock(&setclk_mutex);
}

static void clear_rx_vinfo(struct hdmitx_dev *hdev)
{
	struct vinfo_s *info = hdmitx_get_current_vinfo(NULL);

	if (info) {
		memset(&info->hdr_info, 0, sizeof(info->hdr_info));
		memset(&info->rx_latency, 0, sizeof(info->rx_latency));
	}
}

static void hdmitx_aud_hpd_plug_handler(struct work_struct *work)
{
	int st;
	struct hdmitx_dev *hdev = container_of((struct delayed_work *)work,
		struct hdmitx_dev, work_aud_hpd_plug);

	st = hdev->hwop.cntlmisc(hdev, MISC_HPD_GPI_ST, 0);
	pr_info("%s state:%d\n", __func__, st);
	hdmitx21_set_uevent(HDMITX_AUDIO_EVENT, st);
}

static void hdmitx_hpd_plugout_handler(struct work_struct *work)
{
	struct hdmitx_dev *hdev = container_of((struct delayed_work *)work,
		struct hdmitx_dev, work_hpd_plugout);

	mutex_lock(&setclk_mutex);
	if (!(hdev->hdmitx_event & (HDMI_TX_HPD_PLUGOUT))) {
		mutex_unlock(&setclk_mutex);
		return;
	}
	if (hdev->cedst_policy)
		cancel_delayed_work(&hdev->work_cedst);
	edidinfo_detach_to_vinfo(hdev);
	pr_info("plugout\n");
	if (!!(hdev->hwop.cntlmisc(hdev, MISC_HPD_GPI_ST, 0))) {
		pr_info("hpd gpio high\n");
		hdev->hdmitx_event &= ~HDMI_TX_HPD_PLUGOUT;
		mutex_unlock(&setclk_mutex);
		return;
	}
	/*after plugout, DV mode can't be supported*/
	hdmitx_set_vsif_pkt(0, 0, NULL, true);
	hdmitx_set_hdr10plus_pkt(0, NULL);
	hdev->ready = 0;
	if (hdev->repeater_tx)
		rx_repeat_hpd_state(0);
	hdev->hwop.cntlconfig(hdev, CONF_CLR_AVI_PACKET, 0);
	hdev->hwop.cntlmisc(hdev, MISC_TMDS_PHY_OP, TMDS_PHY_DISABLE);
	hdev->hdmitx_event &= ~HDMI_TX_HPD_PLUGOUT;
	hdev->hdcp_mode = 0;
	hdcp_mode_set(0);
	clear_rx_vinfo(hdev);
	rx_edid_physical_addr(0, 0, 0, 0);
	hdmitx21_edid_clear(hdev);
	hdmi_physical_size_update(hdev);
	hdmitx21_edid_ram_buffer_clear(hdev);
	hdev->hpd_state = 0;
	hdmitx_notify_hpd(hdev->hpd_state);
	hdmitx21_set_uevent(HDMITX_HPD_EVENT, 0);
	hdmitx21_set_uevent(HDMITX_AUDIO_EVENT, 0);

	mutex_unlock(&setclk_mutex);
}

int get21_hpd_state(void)
{
	int ret;
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	mutex_lock(&setclk_mutex);
	ret = hdev->hpd_state;
	mutex_unlock(&setclk_mutex);

	return ret;
}

/******************************
 *  hdmitx kernel task
 *******************************/

static bool is_cur_tmds_div40(struct hdmitx_dev *hdev)
{
	return 0;
}

static void hdmitx_resend_div40(struct hdmitx_dev *hdev)
{
	hdev->hwop.cntlddc(hdev, DDC_SCDC_DIV40_SCRAMB, 1);
	hdev->div40 = 1;
}

/*****************************
 *	hdmitx driver file_operations
 *
 ******************************/
static int amhdmitx_open(struct inode *node, struct file *file)
{
	struct hdmitx_dev *hdmitx_in_devp;

	/* Get the per-device structure that contains this cdev */
	hdmitx_in_devp = container_of(node->i_cdev, struct hdmitx_dev, cdev);
	file->private_data = hdmitx_in_devp;

	return 0;
}

static int amhdmitx_release(struct inode *node, struct file *file)
{
	return 0;
}

static const struct file_operations amhdmitx_fops = {
	.owner	= THIS_MODULE,
	.open	 = amhdmitx_open,
	.release = amhdmitx_release,
};

static int get_dt_vend_init_data(struct device_node *np,
				 struct vendor_info_data *vend)
{
	int ret;

	ret = of_property_read_string(np, "vendor_name",
				      (const char **)&vend->vendor_name);
	if (ret)
		pr_info("not find vendor name\n");

	ret = of_property_read_u32(np, "vendor_id", &vend->vendor_id);
	if (ret)
		pr_info("not find vendor id\n");

	ret = of_property_read_string(np, "product_desc",
				      (const char **)&vend->product_desc);
	if (ret)
		pr_info("not find product desc\n");
	return 0;
}

static void hdmitx_fmt_attr(struct hdmitx_dev *hdev)
{
	if (strlen(hdev->fmt_attr) >= 8) {
		pr_info("fmt_attr %s\n", hdev->fmt_attr);
		return;
	}
	memset(hdev->fmt_attr, 0, sizeof(hdev->fmt_attr));
	switch (hdev->para->cs) {
	case HDMI_COLORSPACE_RGB:
		memcpy(hdev->fmt_attr, "rgb,", 5);
		break;
	case HDMI_COLORSPACE_YUV422:
		memcpy(hdev->fmt_attr, "422,", 5);
		break;
	case HDMI_COLORSPACE_YUV444:
		memcpy(hdev->fmt_attr, "444,", 5);
		break;
	case HDMI_COLORSPACE_YUV420:
		memcpy(hdev->fmt_attr, "420,", 5);
		break;
	default:
		break;
	}
	switch (hdev->para->cd) {
	case COLORDEPTH_24B:
		strcat(hdev->fmt_attr, "8bit");
		break;
	case COLORDEPTH_30B:
		strcat(hdev->fmt_attr, "10bit");
		break;
	case COLORDEPTH_36B:
		strcat(hdev->fmt_attr, "12bit");
		break;
	case COLORDEPTH_48B:
		strcat(hdev->fmt_attr, "16bit");
		break;
	default:
		break;
	}
	pr_info("fmt_attr %s\n", hdev->fmt_attr);
}

static void hdmitx_init_fmt_attr(struct hdmitx_dev *hdev)
{
	if (!hdev->para)
		return;
	if (strlen(hdev->fmt_attr) >= 8) {
		pr_info("fmt_attr %s\n", hdev->fmt_attr);
		return;
	}
	memset(hdev->fmt_attr, 0, sizeof(hdev->fmt_attr));
	switch (hdev->para->cs) {
	case HDMI_COLORSPACE_RGB:
		memcpy(hdev->fmt_attr, "rgb,", 5);
		break;
	case HDMI_COLORSPACE_YUV422:
		memcpy(hdev->fmt_attr, "422,", 5);
		break;
	case HDMI_COLORSPACE_YUV444:
		memcpy(hdev->fmt_attr, "444,", 5);
		break;
	case HDMI_COLORSPACE_YUV420:
		memcpy(hdev->fmt_attr, "420,", 5);
		break;
	default:
		break;
	}
	switch (hdev->para->cd) {
	case COLORDEPTH_24B:
		strcat(hdev->fmt_attr, "8bit");
		break;
	case COLORDEPTH_30B:
		strcat(hdev->fmt_attr, "10bit");
		break;
	case COLORDEPTH_36B:
		strcat(hdev->fmt_attr, "12bit");
		break;
	case COLORDEPTH_48B:
		strcat(hdev->fmt_attr, "16bit");
		break;
	default:
		break;
	}
	pr_info("fmt_attr %s\n", hdev->fmt_attr);
}

/* for notify to cec */
static BLOCKING_NOTIFIER_HEAD(hdmitx21_event_notify_list);
int hdmitx21_event_notifier_regist(struct notifier_block *nb)
{
	int ret = 0;
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	if (!nb)
		return ret;

	ret = blocking_notifier_chain_register(&hdmitx21_event_notify_list, nb);
	/* update status when register */
	if (!ret && nb->notifier_call) {
		hdmitx_notify_hpd(hdev->hpd_state);
		if (hdev->physical_addr != 0xffff)
			hdmitx21_event_notify(HDMITX_PHY_ADDR_VALID,
					    &hdev->physical_addr);
	}

	return ret;
}

int hdmitx21_event_notifier_unregist(struct notifier_block *nb)
{
	int ret;
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	if (hdev->hdmi_init != 1)
		return 1;

	ret = blocking_notifier_chain_unregister(&hdmitx21_event_notify_list,
						 nb);

	return ret;
}

void hdmitx21_event_notify(unsigned long state, void *arg)
{
	blocking_notifier_call_chain(&hdmitx21_event_notify_list, state, arg);
}

void hdmitx21_hdcp_status(int hdmi_authenticated)
{
	hdmitx21_set_uevent(HDMITX_HDCP_EVENT, hdmi_authenticated);
}

static void hdmitx_init_parameters(struct hdmitx_info *info)
{
	memset(info, 0, sizeof(struct hdmitx_info));

	info->video_out_changing_flag = 1;

	info->audio_flag = 1;
	info->audio_info.type = CT_REFER_TO_STREAM;
	info->audio_info.format = AF_I2S;
	info->audio_info.fs = FS_44K1;
	info->audio_info.ss = SS_16BITS;
	info->audio_info.channels = CC_2CH;
	info->audio_out_changing_flag = 1;
}

static int amhdmitx_device_init(struct hdmitx_dev *hdmi_dev)
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();
	static struct hdmi_format_para para;

	if (!hdmi_dev)
		return 1;

	memset(&para, 0, sizeof(para));
	pr_info("Ver: %s\n", HDMITX_VER);

	hdmi_dev->hdtx_dev = NULL;

	hdev->para = &para;
	hdev->physical_addr = 0xffff;
	hdev->hdmi_last_hdr_mode = 0;
	hdev->hdmi_current_hdr_mode = 0;
	hdev->unplug_powerdown = 0;
	hdev->vic_count = 0;
	hdev->force_audio_flag = 0;
	hdev->ready = 0;
	hdev->rxsense_policy = 0; /* no RxSense by default */
	/* enable or disable HDMITX SSPLL, enable by default */
	hdev->sspll = 1;
	/*
	 * 0, do not unmux hpd when off or unplug ;
	 * 1, unmux hpd when unplug;
	 * 2, unmux hpd when unplug  or off;
	 */
	hdev->hpdmode = 1;

	hdev->flag_3dfp = 0;
	hdev->flag_3dss = 0;
	hdev->flag_3dtb = 0;

	if ((init_flag & INIT_FLAG_POWERDOWN) &&
	    hdev->hpdmode == 2)
		hdev->mux_hpd_if_pin_high_flag = 0;
	else
		hdev->mux_hpd_if_pin_high_flag = 1;

	hdev->audio_param_update_flag = 0;
	/* 1: 2ch */
	hdev->hdmi_ch = 1;
	/* default audio configure is on */
	hdev->tx_aud_cfg = 1;
	hdmitx_init_parameters(&hdev->hdmi_info);

	return 0;
}

static void amhdmitx_get_drm_info(void)
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();
	struct device_node *drm_node;
	u8 *drm_status;
	int ret = 0;

	drm_node = of_find_node_by_path("/drm-amhdmitx");
	if (drm_node) {
		ret =
		of_property_read_string(drm_node, "status",
					(const char **)&drm_status);
		if (ret) {
			pr_info("not find drm_feature\n");
		} else {
			if (memcmp(drm_status, "okay", 4) == 0)
				hdev->drm_feature = 1;
			else
				hdev->drm_feature = 0;
			pr_info("hdev->drm_feature : %d\n",
				hdev->drm_feature);
		}
	} else {
		pr_info("not find drm_amhdmitx\n");
	}
}

static int amhdmitx_get_dt_info(struct platform_device *pdev)
{
	int ret = 0;
	struct hdmitx_dev *hdev = get_hdmitx21_device();

#ifdef CONFIG_OF
	int val;
	phandle phandler;
	struct device_node *init_data;
	const struct of_device_id *match;
#endif

	/* HDMITX pinctrl config for hdp and ddc*/
	if (pdev->dev.pins) {
		hdev->pdev = &pdev->dev;

		hdev->pinctrl_default =
			pinctrl_lookup_state(pdev->dev.pins->p, "default");
		if (IS_ERR(hdev->pinctrl_default))
			pr_info("no default of pinctrl state\n");

		hdev->pinctrl_i2c =
			pinctrl_lookup_state(pdev->dev.pins->p, "hdmitx_i2c");
		if (IS_ERR(hdev->pinctrl_i2c))
			pr_info("no hdmitx_i2c of pinctrl state\n");

		pinctrl_select_state(pdev->dev.pins->p,
				     hdev->pinctrl_default);
	}

#ifdef CONFIG_OF
	if (pdev->dev.of_node) {
		int dongle_mode = 0;

		memset(&hdev->config_data, 0,
		       sizeof(struct hdmi_config_platform_data));
		/* Get chip type and name information */
		match = of_match_device(meson_amhdmitx_of_match, &pdev->dev);

		if (!match) {
			pr_info("%s: no match table\n", __func__);
			return -1;
		}
		hdev->data = (struct amhdmitx_data_s *)match->data;

		pr_info("chip_type:%d chip_name:%s\n",
			hdev->data->chip_type,
			hdev->data->chip_name);

		/* Get dongle_mode information */
		ret = of_property_read_u32(pdev->dev.of_node, "dongle_mode",
					   &dongle_mode);
		hdev->dongle_mode = !!dongle_mode;
		if (!ret)
			pr_info("hdev->dongle_mode: %d\n",
				hdev->dongle_mode);
		/* Get repeater_tx information */
		ret = of_property_read_u32(pdev->dev.of_node,
					   "repeater_tx", &val);
		if (!ret)
			hdev->repeater_tx = val;

		ret = of_property_read_u32(pdev->dev.of_node,
					   "cedst_en", &val);
		if (!ret)
			hdev->cedst_en = !!val;

		ret = of_property_read_u32(pdev->dev.of_node,
					   "hdcp_type_policy", &val);
		if (!ret) {
			if (val == 2)
				;
			if (val == 1)
				;
		}

		/* Get vendor information */
		ret = of_property_read_u32(pdev->dev.of_node,
					   "vend-data", &val);
		if (ret)
			pr_info("not find match init-data\n");
		if (ret == 0) {
			phandler = val;
			init_data = of_find_node_by_phandle(phandler);
			if (!init_data)
				pr_info("not find device node\n");
			hdev->config_data.vend_data =
			kzalloc(sizeof(struct vendor_info_data), GFP_KERNEL);
			if (!(hdev->config_data.vend_data))
				pr_info("not allocate memory\n");
			ret = get_dt_vend_init_data
			(init_data, hdev->config_data.vend_data);
			if (ret)
				pr_info("not find vend_init_data\n");
		}
		/* Get power control */
		ret = of_property_read_u32(pdev->dev.of_node,
					   "pwr-ctrl", &val);
		if (ret)
			pr_info("not find match pwr-ctl\n");
		if (ret == 0) {
			phandler = val;
			init_data = of_find_node_by_phandle(phandler);
			if (!init_data)
				pr_info("not find device node\n");
			hdev->config_data.pwr_ctl = kzalloc((sizeof(struct hdmi_pwr_ctl)) *
				HDMI_TX_PWR_CTRL_NUM, GFP_KERNEL);
			if (!hdev->config_data.pwr_ctl)
				pr_info("can not get pwr_ctl mem\n");
			else
				memset(hdev->config_data.pwr_ctl, 0, sizeof(struct hdmi_pwr_ctl));
			if (ret)
				pr_info("not find pwr_ctl\n");
		}

		/* Get drm feature information */
		amhdmitx_get_drm_info();

		/* Get reg information */
		ret = hdmitx21_init_reg_map(pdev);
	}

#else
		hdmi_pdata = pdev->dev.platform_data;
		if (!hdmi_pdata) {
			pr_info("not get platform data\n");
			r = -ENOENT;
		} else {
			pr_info("get hdmi platform data\n");
		}
#endif
	hdev->irq_hpd = platform_get_irq_byname(pdev, "hdmitx_hpd");
	if (hdev->irq_hpd == -ENXIO) {
		pr_err("%s: ERROR: hdmitx hpd irq No not found\n",
		       __func__);
			return -ENXIO;
	}
	pr_info("hpd irq = %d\n", hdev->irq_hpd);

	return ret;
}

/*
 * amhdmitx_clktree_probe
 * get clktree info from dts
 */
static void amhdmitx_clktree_probe(struct device *hdmitx_dev)
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();
	struct clk *hdmi_clk_vapb, *hdmi_clk_vpu;
	struct clk *venci_top_gate, *venci_0_gate, *venci_1_gate;

	hdmi_clk_vapb = devm_clk_get(hdmitx_dev, "hdmi_vapb_clk");
	if (IS_ERR(hdmi_clk_vapb)) {
		pr_warn("vapb_clk failed to probe\n");
	} else {
		hdev->hdmitx_clk_tree.hdmi_clk_vapb = hdmi_clk_vapb;
		clk_prepare_enable(hdev->hdmitx_clk_tree.hdmi_clk_vapb);
	}

	hdmi_clk_vpu = devm_clk_get(hdmitx_dev, "hdmi_vpu_clk");
	if (IS_ERR(hdmi_clk_vpu)) {
		pr_warn("vpu_clk failed to probe\n");
	} else {
		hdev->hdmitx_clk_tree.hdmi_clk_vpu = hdmi_clk_vpu;
		clk_prepare_enable(hdev->hdmitx_clk_tree.hdmi_clk_vpu);
	}

	venci_top_gate = devm_clk_get(hdmitx_dev, "venci_top_gate");
	if (IS_ERR(venci_top_gate))
		pr_warn("venci_top_gate failed to probe\n");
	else
		hdev->hdmitx_clk_tree.venci_top_gate = venci_top_gate;

	venci_0_gate = devm_clk_get(hdmitx_dev, "venci_0_gate");
	if (IS_ERR(venci_0_gate))
		pr_warn("venci_0_gate failed to probe\n");
	else
		hdev->hdmitx_clk_tree.venci_0_gate = venci_0_gate;

	venci_1_gate = devm_clk_get(hdmitx_dev, "venci_1_gate");
	if (IS_ERR(venci_1_gate))
		pr_warn("venci_0_gate failed to probe\n");
	else
		hdev->hdmitx_clk_tree.venci_1_gate = venci_1_gate;
}

void amhdmitx21_vpu_dev_regiter(struct hdmitx_dev *hdev)
{
	hdev->hdmitx_vpu_clk_gate_dev =
	vpu_dev_register(VPU_VENCI, DEVICE_NAME);
}

static void amhdmitx_infoframe_init(struct hdmitx_dev *hdev)
{
	int ret = 0;

	ret = hdmi_vendor_infoframe_init(&hdev->infoframes.vend.vendor.hdmi);
	if (!ret)
		pr_info("%s[%d] init vendor infoframe failed %d\n", __func__, __LINE__, ret);
	ret = hdmi_avi_infoframe_init(&hdev->infoframes.avi.avi);
	if (ret)
		pr_info("init avi infoframe failed\n");
	// TODO, panic
	// hdmi_spd_infoframe_init(&hdev->infoframes.spd.spd,
	//	hdev->config_data.vend_data->vendor_name,
	//	hdev->config_data.vend_data->product_desc);
	hdmi_audio_infoframe_init(&hdev->infoframes.aud.audio);
	hdmi_drm_infoframe_init(&hdev->infoframes.drm.drm);
}

static int amhdmitx_probe(struct platform_device *pdev)
{
	int r, ret = 0;
	struct device *dev;
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	pr_debug("%s start\n", __func__);

	amhdmitx_device_init(hdev);
	amhdmitx_infoframe_init(hdev);

	ret = amhdmitx_get_dt_info(pdev);
	if (ret)
		return ret;

	amhdmitx_clktree_probe(&pdev->dev);

	amhdmitx21_vpu_dev_regiter(hdev);

	r = alloc_chrdev_region(&hdev->hdmitx_id, 0, HDMI_TX_COUNT,
				DEVICE_NAME);
	cdev_init(&hdev->cdev, &amhdmitx_fops);
	hdev->cdev.owner = THIS_MODULE;
	r = cdev_add(&hdev->cdev, hdev->hdmitx_id, HDMI_TX_COUNT);

	hdmitx_class = class_create(THIS_MODULE, "amhdmitx");
	if (IS_ERR(hdmitx_class)) {
		unregister_chrdev_region(hdev->hdmitx_id, HDMI_TX_COUNT);
		return -1;
	}

	dev = device_create(hdmitx_class, NULL, hdev->hdmitx_id, NULL,
			    "amhdmitx%d", 0); /* kernel>=2.6.27 */

	if (!dev) {
		pr_info("device_create create error\n");
		class_destroy(hdmitx_class);
		r = -EEXIST;
		return r;
	}
	hdev->hdtx_dev = dev;
	ret = device_create_file(dev, &dev_attr_disp_mode);
	ret = device_create_file(dev, &dev_attr_attr);
	ret = device_create_file(dev, &dev_attr_vid_mute);
	ret = device_create_file(dev, &dev_attr_edid);
	ret = device_create_file(dev, &dev_attr_rawedid);
	ret = device_create_file(dev, &dev_attr_sink_type);
	ret = device_create_file(dev, &dev_attr_edid_parsing);
	ret = device_create_file(dev, &dev_attr_config);
	ret = device_create_file(dev, &dev_attr_debug);
	ret = device_create_file(dev, &dev_attr_disp_cap);
	ret = device_create_file(dev, &dev_attr_preferred_mode);
	ret = device_create_file(dev, &dev_attr_cea_cap);
	ret = device_create_file(dev, &dev_attr_vesa_cap);
	ret = device_create_file(dev, &dev_attr_aud_cap);
	ret = device_create_file(dev, &dev_attr_hdmi_hdr_status);
	ret = device_create_file(dev, &dev_attr_hdr_cap);
	ret = device_create_file(dev, &dev_attr_dv_cap);
	ret = device_create_file(dev, &dev_attr_dv_cap2);
	ret = device_create_file(dev, &dev_attr_avmute);
	ret = device_create_file(dev, &dev_attr_phy);
	ret = device_create_file(dev, &dev_attr_frac_rate_policy);
	ret = device_create_file(dev, &dev_attr_sspll);
	ret = device_create_file(dev, &dev_attr_rxsense_policy);
	ret = device_create_file(dev, &dev_attr_cedst_policy);
	ret = device_create_file(dev, &dev_attr_cedst_count);
	ret = device_create_file(dev, &dev_attr_hdcp_mode);
	ret = device_create_file(dev, &dev_attr_hdcp_type_policy);
	ret = device_create_file(dev, &dev_attr_hdmi_repeater_tx);
	ret = device_create_file(dev, &dev_attr_hdcp_lstore);
	ret = device_create_file(dev, &dev_attr_div40);
	ret = device_create_file(dev, &dev_attr_hpd_state);
	ret = device_create_file(dev, &dev_attr_hdmi_used);
	ret = device_create_file(dev, &dev_attr_fake_plug);
	ret = device_create_file(dev, &dev_attr_ready);
	ret = device_create_file(dev, &dev_attr_support_3d);
	ret = device_create_file(dev, &dev_attr_dc_cap);
	ret = device_create_file(dev, &dev_attr_valid_mode);
	ret = device_create_file(dev, &dev_attr_allm_cap);
	ret = device_create_file(dev, &dev_attr_allm_mode);
	ret = device_create_file(dev, &dev_attr_contenttype_cap);
	ret = device_create_file(dev, &dev_attr_contenttype_mode);
	ret = device_create_file(dev, &dev_attr_hdmitx21);

#ifdef CONFIG_AMLOGIC_LEGACY_EARLY_SUSPEND
	register_early_suspend(&hdmitx_early_suspend_handler);
#endif
	hdev->nb.notifier_call = hdmitx_reboot_notifier;
	register_reboot_notifier(&hdev->nb);
	hdmitx21_meson_init(hdev);
	hdev->hpd_state = !!(hdev->hwop.cntlmisc(hdev, MISC_HPD_GPI_ST, 0));
#ifdef CONFIG_AMLOGIC_VOUT_SERVE
	vout_register_server(&hdmitx_vout_server);
#endif
#ifdef CONFIG_AMLOGIC_VOUT2_SERVE
	vout2_register_server(&hdmitx_vout2_server);
#endif
#if IS_ENABLED(CONFIG_AMLOGIC_SND_SOC)
	if (hdmitx21_uboot_audio_en()) {
		struct hdmitx_audpara *audpara = &hdev->cur_audio_param;

		audpara->sample_rate = FS_48K;
		audpara->type = CT_PCM;
		audpara->sample_size = SS_16BITS;
		audpara->channel_num = 2 - 1;
	}
	aout_register_client(&hdmitx_notifier_nb_a);
#endif
pr_info("%s[%d]\n", __func__, __LINE__);
	/* update fmt_attr */
	hdmitx_init_fmt_attr(hdev);
pr_info("%s[%d]\n", __func__, __LINE__);

	hdev->hpd_state = !!hdev->hwop.cntlmisc(hdev, MISC_HPD_GPI_ST, 0);
	hdmitx_notify_hpd(hdev->hpd_state);
	INIT_WORK(&hdev->work_hdr, hdr_work_func);
pr_info("%s[%d]\n", __func__, __LINE__);

/* When init hdmi, clear the hdmitx module edid ram and edid buffer. */
	hdmitx21_edid_clear(hdev);
	hdmitx21_edid_ram_buffer_clear(hdev);
pr_info("%s[%d]\n", __func__, __LINE__);
	hdev->hdmi_wq = alloc_workqueue(DEVICE_NAME,
					WQ_HIGHPRI | WQ_CPU_INTENSIVE, 0);
	INIT_DELAYED_WORK(&hdev->work_hpd_plugin, hdmitx_hpd_plugin_handler);
	INIT_DELAYED_WORK(&hdev->work_hpd_plugout, hdmitx_hpd_plugout_handler);
	INIT_DELAYED_WORK(&hdev->work_aud_hpd_plug,
			  hdmitx_aud_hpd_plug_handler);
	INIT_DELAYED_WORK(&hdev->work_internal_intr, hdmitx_top_intr_handler);
pr_info("%s[%d]\n", __func__, __LINE__);

	/* for rx sense feature */
	hdev->rxsense_wq = alloc_workqueue("hdmitx_rxsense",
					   WQ_SYSFS | WQ_FREEZABLE, 0);
	INIT_DELAYED_WORK(&hdev->work_rxsense, hdmitx_rxsense_process);
	/* for cedst feature */
	hdev->cedst_wq = alloc_workqueue("hdmitx_cedst",
					 WQ_SYSFS | WQ_FREEZABLE, 0);
	INIT_DELAYED_WORK(&hdev->work_cedst, hdmitx_cedst_process);
pr_info("%s[%d]\n", __func__, __LINE__);

	hdev->tx_aud_cfg = 1; /* default audio configure is on */
	/* When bootup mbox and TV simutanously, TV may not handle SCDC/DIV40 */
	if (hdev->hpd_state && is_cur_tmds_div40(hdev))
		hdmitx_resend_div40(hdev);

	hdmitx21_hdcp_init();

	/*Direct Rander Management use another irq*/
	if (hdev->drm_feature == 0)
		hdmitx_setupirqs(hdev);
pr_info("%s[%d]\n", __func__, __LINE__);

	if (hdev->hpd_state) {
		/* need to get edid before vout probe */
		hdev->already_used = 1;
		hdmitx_get_edid(hdev);
	}
pr_info("%s[%d]\n", __func__, __LINE__);
	/* Trigger HDMITX IRQ*/
	if (hdev->hwop.cntlmisc(hdev, MISC_HPD_GPI_ST, 0))
		hdev->hwop.cntlmisc(hdev, MISC_TRIGGER_HPD, 0);
pr_info("%s[%d]\n", __func__, __LINE__);

	hdev->hdmi_init = 1;

	pr_info("%s end\n", __func__);

	return r;
}

static int amhdmitx_remove(struct platform_device *pdev)
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();
	struct device *dev = hdev->hdtx_dev;

	cancel_work_sync(&hdev->work_hdr);

	if (hdev->hwop.uninit)
		hdev->hwop.uninit(hdev);
	hdev->hpd_event = 0xff;
	kthread_stop(hdev->task);
#ifdef CONFIG_AMLOGIC_VOUT_SERVE
	vout_unregister_server(&hdmitx_vout_server);
#endif
#ifdef CONFIG_AMLOGIC_VOUT2_SERVE
	vout2_unregister_server(&hdmitx_vout2_server);
#endif

#if IS_ENABLED(CONFIG_AMLOGIC_SND_SOC)
	aout_unregister_client(&hdmitx_notifier_nb_a);
#endif

	/* Remove the cdev */
	device_remove_file(dev, &dev_attr_disp_mode);
	device_remove_file(dev, &dev_attr_attr);
	device_remove_file(dev, &dev_attr_vid_mute);
	device_remove_file(dev, &dev_attr_edid);
	device_remove_file(dev, &dev_attr_rawedid);
	device_remove_file(dev, &dev_attr_sink_type);
	device_remove_file(dev, &dev_attr_edid_parsing);
	device_remove_file(dev, &dev_attr_config);
	device_remove_file(dev, &dev_attr_debug);
	device_remove_file(dev, &dev_attr_disp_cap);
	device_remove_file(dev, &dev_attr_preferred_mode);
	device_remove_file(dev, &dev_attr_cea_cap);
	device_remove_file(dev, &dev_attr_vesa_cap);
	device_remove_file(dev, &dev_attr_hdr_cap);
	device_remove_file(dev, &dev_attr_dv_cap);
	device_remove_file(dev, &dev_attr_dv_cap2);
	device_remove_file(dev, &dev_attr_dc_cap);
	device_remove_file(dev, &dev_attr_valid_mode);
	device_remove_file(dev, &dev_attr_allm_cap);
	device_remove_file(dev, &dev_attr_allm_mode);
	device_remove_file(dev, &dev_attr_contenttype_cap);
	device_remove_file(dev, &dev_attr_contenttype_mode);
	device_remove_file(dev, &dev_attr_hpd_state);
	device_remove_file(dev, &dev_attr_hdmi_used);
	device_remove_file(dev, &dev_attr_fake_plug);
	device_remove_file(dev, &dev_attr_ready);
	device_remove_file(dev, &dev_attr_support_3d);
	device_remove_file(dev, &dev_attr_avmute);
	device_remove_file(dev, &dev_attr_frac_rate_policy);
	device_remove_file(dev, &dev_attr_sspll);
	device_remove_file(dev, &dev_attr_rxsense_policy);
	device_remove_file(dev, &dev_attr_cedst_policy);
	device_remove_file(dev, &dev_attr_cedst_count);
	device_remove_file(dev, &dev_attr_div40);
	device_remove_file(dev, &dev_attr_hdcp_type_policy);
	device_remove_file(dev, &dev_attr_hdmi_repeater_tx);
	device_remove_file(dev, &dev_attr_hdmi_hdr_status);
	device_remove_file(dev, &dev_attr_hdmitx21);

	cdev_del(&hdev->cdev);

	device_destroy(hdmitx_class, hdev->hdmitx_id);

	class_destroy(hdmitx_class);

	unregister_chrdev_region(hdev->hdmitx_id, HDMI_TX_COUNT);
	return 0;
}

#ifdef CONFIG_PM
static int amhdmitx_suspend(struct platform_device *pdev,
			    pm_message_t state)
{
	return 0;
}

static int amhdmitx_resume(struct platform_device *pdev)
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	pr_info("%s: I2C_REACTIVE\n", DEVICE_NAME);
	hdev->hwop.cntlmisc(hdev, MISC_I2C_REACTIVE, 0);

	return 0;
}
#endif

static struct platform_driver amhdmitx_driver = {
	.probe	 = amhdmitx_probe,
	.remove	 = amhdmitx_remove,
#ifdef CONFIG_PM
	.suspend	= amhdmitx_suspend,
	.resume	 = amhdmitx_resume,
#endif
	.driver	 = {
		.name = DEVICE_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(meson_amhdmitx_of_match),
#ifdef CONFIG_HIBERNATION
		.pm	= &amhdmitx_pm,
#endif
	}
};

int  __init amhdmitx21_init(void)
{
	if (init_flag & INIT_FLAG_NOT_LOAD)
		return 0;

	return platform_driver_register(&amhdmitx_driver);
}

void __exit amhdmitx21_exit(void)
{
	pr_info("%s...\n", __func__);
	// TODO stop hdcp
	platform_driver_unregister(&amhdmitx_driver);
}

#ifndef MODULE
subsys_initcall(amhdmitx21_init);
module_exit(amhdmitx21_exit);
#endif

//MODULE_DESCRIPTION("AMLOGIC HDMI TX driver");
//MODULE_LICENSE("GPL");
//MODULE_VERSION("1.0.0");

/* besides characters defined in separator, '\"' are used as separator;
 * and any characters in '\"' will not act as separator
 */
static char *next_token_ex(char *separator, char *buf, u32 size,
			   u32 offset, u32 *token_len,
			   u32 *token_offset)
{
	char *ptoken = NULL;
	char last_separator = 0;
	char trans_char_flag = 0;

	if (buf) {
		for (; offset < size; offset++) {
			int ii = 0;
		char ch;

		if (buf[offset] == '\\') {
			trans_char_flag = 1;
			continue;
		}
		while (((ch = separator[ii++]) != buf[offset]) && (ch))
			;
		if (ch) {
			if (!ptoken) {
				continue;
		} else {
			if (last_separator != '"') {
				*token_len = (unsigned int)
					(buf + offset - ptoken);
				*token_offset = offset;
				return ptoken;
			}
		}
		} else if (!ptoken) {
			if (trans_char_flag && (buf[offset] == '"'))
				last_separator = buf[offset];
			ptoken = &buf[offset];
		} else if ((trans_char_flag && (buf[offset] == '"')) &&
			   (last_separator == '"')) {
			*token_len = (unsigned int)(buf + offset - ptoken - 2);
			*token_offset = offset + 1;
			return ptoken + 1;
		}
		trans_char_flag = 0;
	}
	if (ptoken) {
		*token_len = (unsigned int)(buf + offset - ptoken);
		*token_offset = offset;
	}
	}
	return ptoken;
}

/* check the colorattribute from uboot */
static void check_hdmiuboot_attr(char *token)
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();
	char attr[16] = {0};
	const char * const cs[] = {
		"444", "422", "rgb", "420", NULL};
	const char * const cd[] = {
		"8bit", "10bit", "12bit", "16bit", NULL};
	int i;

	if (hdev->fmt_attr[0] != 0)
		return;

	if (!token)
		return;

	for (i = 0; cs[i]; i++) {
		if (strstr(token, cs[i])) {
			if (strlen(cs[i]) < sizeof(attr))
				strcpy(attr, cs[i]);
			strcat(attr, ",");
			break;
		}
	}
	for (i = 0; cd[i]; i++) {
		if (strstr(token, cd[i])) {
			if (strlen(cd[i]) < sizeof(attr))
				if (strlen(cd[i]) <
					(sizeof(attr) - strlen(attr)))
					strcat(attr, cd[i]);
			strncpy(hdev->fmt_attr, attr,
				sizeof(hdev->fmt_attr));
			hdev->fmt_attr[15] = '\0';
			break;
		}
	}
	memcpy(hdev->backup_fmt_attr, hdev->fmt_attr,
	       sizeof(hdev->fmt_attr));
}

static int hdmitx21_boot_para_setup(char *s)
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();
	char separator[] = {' ', ',', ';', 0x0};
	char *token;
	u32 token_len = 0;
	u32 token_offset = 0;
	u32 offset = 0;
	int size = strlen(s);

	memset(hdev->fmt_attr, 0, sizeof(hdev->fmt_attr));
	memset(hdev->backup_fmt_attr, 0,
	       sizeof(hdev->backup_fmt_attr));

	do {
		token = next_token_ex(separator, s, size, offset,
				      &token_len, &token_offset);
		if (token) {
			if (token_len == 3 &&
			    strncmp(token, "off", token_len) == 0) {
				init_flag |= INIT_FLAG_NOT_LOAD;
			}
			check_hdmiuboot_attr(token);
		}
		offset = token_offset;
	} while (token);
	return 0;
}

__setup("hdmitx=", hdmitx21_boot_para_setup);

static int hdmitx21_boot_frac_rate(char *str)
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	if (strncmp("0", str, 1) == 0)
		hdev->frac_rate_policy = 0;
	else
		hdev->frac_rate_policy = 1;

	pr_info("hdmitx boot frac_rate_policy: %d",
		hdev->frac_rate_policy);

	hdev->backup_frac_rate_policy = hdev->frac_rate_policy;
	return 0;
}

__setup("frac_rate_policy=", hdmitx21_boot_frac_rate);

static int hdmitx21_boot_hdr_priority(char *str)
{
	struct hdmitx_dev *hdev = get_hdmitx21_device();

	if (strncmp("1", str, 1) == 0) {
		hdev->hdr_priority = 1;
		pr_info("hdmitx boot hdr_priority: 1\n");
	}
	return 0;
}

__setup("hdr_priority=", hdmitx21_boot_hdr_priority);

MODULE_PARM_DESC(log21_level, "\n log21_level\n");
module_param(log21_level, int, 0644);
