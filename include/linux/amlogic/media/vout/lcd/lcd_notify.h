/* SPDX-License-Identifier: GPL-2.0+ */
/*
 *
 * Copyright (C) 2019 Amlogic, Inc. All rights reserved.
 *
 */

#ifndef _INC_LCD_NOTIFY_H_
#define _INC_LCD_NOTIFY_H_

#include <linux/notifier.h>

#define LCD_PRIORITY_INIT_CONFIG       12
#define LCD_PRIORITY_INIT_VOUT         11

#define LCD_PRIORITY_SCREEN_BLACK      7
#define LCD_PRIORITY_POWER_BL_OFF      6
#define LCD_PRIORITY_POWER_IF_OFF      5
#define LCD_PRIORITY_POWER_ENCL_OFF    4
#define LCD_PRIORITY_POWER_ENCL_ON     3
#define LCD_PRIORITY_POWER_IF_ON       2
#define LCD_PRIORITY_POWER_BL_ON       1
#define LCD_PRIORITY_SCREEN_RESTORE    0

/* original event */
#define LCD_EVENT_SCREEN_BLACK      BIT(0)
#define LCD_EVENT_BL_OFF            BIT(1)
#define LCD_EVENT_IF_OFF            BIT(2)
#define LCD_EVENT_ENCL_OFF          BIT(3)
#define LCD_EVENT_ENCL_ON           BIT(4)
#define LCD_EVENT_IF_ON             BIT(5)
#define LCD_EVENT_BL_ON             BIT(6)
#define LCD_EVENT_SCREEN_RESTORE    BIT(7)

/* combined event */
#define LCD_EVENT_POWER_ON          (LCD_EVENT_BL_ON | LCD_EVENT_IF_ON | \
				LCD_EVENT_ENCL_ON | LCD_EVENT_SCREEN_RESTORE)
#define LCD_EVENT_IF_POWER_ON       (LCD_EVENT_IF_ON | LCD_EVENT_BL_ON | \
				LCD_EVENT_SCREEN_RESTORE)
#define LCD_EVENT_POWER_OFF         (LCD_EVENT_SCREEN_BLACK | \
				LCD_EVENT_BL_OFF | LCD_EVENT_IF_OFF | \
				LCD_EVENT_ENCL_OFF)
#define LCD_EVENT_IF_POWER_OFF      (LCD_EVENT_SCREEN_BLACK | \
				LCD_EVENT_BL_OFF | LCD_EVENT_IF_OFF)

#define LCD_EVENT_PREPARE           (LCD_EVENT_ENCL_ON)
#define LCD_EVENT_ENABLE            (LCD_EVENT_IF_ON | LCD_EVENT_BL_ON | \
				LCD_EVENT_SCREEN_RESTORE)
#define LCD_EVENT_DISABLE           (LCD_EVENT_SCREEN_BLACK | \
				LCD_EVENT_BL_OFF | LCD_EVENT_IF_OFF)
#define LCD_EVENT_UNPREPARE         (LCD_EVENT_ENCL_OFF)

#define LCD_EVENT_GAMMA_UPDATE      BIT(10)
#define LCD_EVENT_EXTERN_SEL        BIT(11)

/* lcd bist pattern test occurred */
#define LCD_EVENT_TEST_PATTERN      BIT(14)

#define LCD_VLOCK_PARAM_NUM         5
#define LCD_VLOCK_PARAM_BIT_UPDATE  BIT(4)
#define LCD_VLOCK_PARAM_BIT_VALID   BIT(0)
#define LCD_EVENT_VLOCK_PARAM       BIT(16)

/* lcd backlight index select */
#define LCD_EVENT_BACKLIGHT_SEL     BIT(24)
/* lcd backlight pwm_vs vfreq change occurred */
#define LCD_EVENT_BACKLIGHT_UPDATE  BIT(25)
/* lcd backlight brightness update by dimming */
#define LCD_EVENT_BACKLIGHT_DV_DIM  BIT(26)
/* lcd backlight brightness on/off by dolby vision */
#define LCD_EVENT_BACKLIGHT_DV_SEL  BIT(27)

/* blocking notify */
int aml_lcd_notifier_register(struct notifier_block *nb);
int aml_lcd_notifier_unregister(struct notifier_block *nb);
int aml_lcd_notifier_call_chain(unsigned long event, void *v);
/* atomic notify */
int aml_lcd_atomic_notifier_register(struct notifier_block *nb);
int aml_lcd_atomic_notifier_unregister(struct notifier_block *nb);
int aml_lcd_atomic_notifier_call_chain(unsigned long event, void *v);

#endif /* _INC_LCD_NOTIFY_H_ */
