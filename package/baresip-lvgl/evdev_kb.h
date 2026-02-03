/**
 * @file evdev_kb.h
 *
 */

#ifndef EVDEV_KB_H
#define EVDEV_KB_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#ifndef LV_DRV_NO_CONF
#ifdef LV_CONF_INCLUDE_SIMPLE
#include "lv_drv_conf.h"
#else
#include "../../lv_drv_conf.h"
#endif
#endif

#if USE_EVDEV || USE_BSD_EVDEV

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

/*********************
 *      DEFINES
 *********************/
#ifndef EVDEV_KB_NAME
#define EVDEV_KB_NAME "/dev/input/event0"
#endif

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * Initialize the evdev interface for keyboard
 */
void evdev_kb_init(void);

/**
 * reconfigure the device file for evdev keyboard
 * @param dev_name set the evdev device filename
 * @return true: the device file set complete
 *         false: the device file doesn't exist current system
 */
bool evdev_kb_set_file(const char *dev_name);

/**
 * Get the current position and state of the evdev
 * @param data store the evdev data here
 */
void evdev_kb_read(lv_indev_drv_t *drv, lv_indev_data_t *data);

#endif /* USE_EVDEV */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* EVDEV_KB_H */
