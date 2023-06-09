// SPDX-License-Identifier: (GPL-2.0+ OR MIT)
/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 */

#include <linux/module.h>
#include <linux/thermal.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/amlogic/ddr_cooling.h>
#include <linux/amlogic/meson_cooldev.h>
#include <linux/io.h>
#include "../../thermal/thermal_core.h"

static DEFINE_IDR(ddr_idr);
static DEFINE_MUTEX(cooling_ddr_lock);
static DEFINE_MUTEX(cooling_list_lock);
static LIST_HEAD(ddr_dev_list);

static void __iomem *ddr_reg0;

/**
 * get_idr - function to get a unique id.
 * @idr: struct idr * handle used to create a id.
 * @id: int * value generated by this function.
 *
 * This function will populate @id with an unique
 * id, using the idr API.
 *
 * Return: 0 on success, an error code on failure.
 */
static int get_idr(struct idr *idr)
{
	int ret;

	mutex_lock(&cooling_ddr_lock);
	ret = idr_alloc(idr, NULL, 0, 0, GFP_KERNEL);
	mutex_unlock(&cooling_ddr_lock);
	return ret;
}

/**
 * release_idr - function to free the unique id.
 * @idr: struct idr * handle used for creating the id.
 * @id: int value representing the unique id.
 */
static void release_idr(struct idr *idr, int id)
{
	mutex_lock(&cooling_ddr_lock);
	idr_remove(idr, id);
	mutex_unlock(&cooling_ddr_lock);
}

static int ddr_get_max_state(struct thermal_cooling_device *cdev,
			     unsigned long *state)
{
	struct ddr_cooling_device *ddr_device = cdev->devdata;

	*state = ddr_device->ddr_status - 1;
	return 0;
}

static int ddr_get_cur_state(struct thermal_cooling_device *cdev,
			     unsigned long *state)
{
	*state = 0;
	return 0;
}

static int ddr_set_cur_state(struct thermal_cooling_device *cdev,
			     unsigned long state)
{
	struct ddr_cooling_device *ddr_device = cdev->devdata;

	if (WARN_ON(state >= ddr_device->ddr_status))
		return -EINVAL;

	state = 0;
	return state;
}

static int ddr_get_requested_power(struct thermal_cooling_device *cdev,
				   struct thermal_zone_device *tz,
				   u32 *power)
{
	*power = 0;
	return 0;
}

static int ddr_state2power(struct thermal_cooling_device *cdev,
			   struct thermal_zone_device *tz,
			   unsigned long state, u32 *power)
{
	*power = 0;
	return 0;
}

static int ddr_power2state(struct thermal_cooling_device *cdev,
			   struct thermal_zone_device *tz, u32 power,
			   unsigned long *state)
{
	cdev->ops->get_cur_state(cdev, state);
	return 0;
}

static int ddr_notify_state(void *thermal_instance,
			    int trip,
			    enum thermal_trip_type type)
{
	struct thermal_instance *ins = thermal_instance;
	struct thermal_zone_device *tz;
	struct thermal_cooling_device *cdev;
	struct ddr_cooling_device *ddr_device;
	int i, hyst = 0, trip_temp;
	unsigned int val, val0, reg_val;

	if (!ins)
		return -EINVAL;

	tz = ins->tz;
	cdev = ins->cdev;

	if (!tz || !cdev)
		return -EINVAL;

	ddr_device = cdev->devdata;

	tz->ops->get_trip_hyst(tz, trip, &hyst);
	tz->ops->get_trip_temp(tz, trip, &trip_temp);

	reg_val = readl_relaxed(ddr_reg0);
	switch (type) {
	case THERMAL_TRIP_HOT:
		val = ddr_device->ddr_data[0];

		for (i = 1; i < ddr_device->ddr_status; i++) {
			if (tz->temperature >= (trip_temp + i * hyst))
				val = ddr_device->ddr_data[i];
			else
				break;
		}
		pr_debug("chip temp: %d, set ddr reg bit val: %x\n", tz->temperature, val);

		val0 = reg_val & ddr_device->ddr_bits_keep;
		val0 = val0 >> ddr_device->ddr_bits[0];

		if (val0 == val)
			break;

		reg_val &= ddr_device->ddr_bits_keep;
		reg_val |= (val << ddr_device->ddr_bits[0]);
		pr_debug("last set ddr reg val: %x\n", reg_val);

		writel_relaxed(reg_val, ddr_reg0);
	default:
		break;
	}
	return 0;
}

static struct thermal_cooling_device_ops const ddr_cooling_ops = {
	.get_max_state = ddr_get_max_state,
	.get_cur_state = ddr_get_cur_state,
	.set_cur_state = ddr_set_cur_state,
	.state2power   = ddr_state2power,
	.power2state   = ddr_power2state,
	.notify_state  = ddr_notify_state,
	.get_requested_power = ddr_get_requested_power,
};

struct thermal_cooling_device *
ddr_cooling_register(struct device_node *np, struct cool_dev *cool)
{
	struct thermal_cooling_device *cool_dev;
	struct ddr_cooling_device *ddr_dev = NULL;
	struct thermal_instance *pos = NULL;
	char dev_name[THERMAL_NAME_LENGTH];
	int i;

	ddr_dev = kmalloc(sizeof(*ddr_dev), GFP_KERNEL);
	if (!ddr_dev)
		return ERR_PTR(-ENOMEM);

	ddr_dev->id = get_idr(&ddr_idr);
	if (ddr_dev->id < 0) {
		kfree(ddr_dev);
		return ERR_PTR(-EINVAL);
	}

	ddr_reg0 = ioremap(cool->ddr_reg, 1);
	if (!ddr_reg0) {
		pr_err("thermal ddr cdev: ddr reg0 ioremap fail.\n");
		release_idr(&ddr_idr, ddr_dev->id);
		kfree(ddr_dev);
		return ERR_PTR(-EINVAL);
	}

	snprintf(dev_name, sizeof(dev_name), "thermal-ddr-%d",
		 ddr_dev->id);

	ddr_dev->ddr_reg = cool->ddr_reg;
	ddr_dev->ddr_status = cool->ddr_status;
	for (i = 0; i < 2; i++)
		ddr_dev->ddr_bits[i] = cool->ddr_bits[i];
	ddr_dev->ddr_bits_keep = ~(0xffffffff << (ddr_dev->ddr_bits[1] + 1));
	ddr_dev->ddr_bits_keep = ~((ddr_dev->ddr_bits_keep >> ddr_dev->ddr_bits[0])
				   << ddr_dev->ddr_bits[0]);
	for (i = 0; i < cool->ddr_status; i++)
		ddr_dev->ddr_data[i] = cool->ddr_data[i];

	cool_dev = thermal_of_cooling_device_register(np, dev_name, ddr_dev,
						      &ddr_cooling_ops);

	if (!cool_dev) {
		release_idr(&ddr_idr, ddr_dev->id);
		kfree(ddr_dev);
		return ERR_PTR(-EINVAL);
	}
	ddr_dev->cool_dev = cool_dev;

	list_for_each_entry(pos, &cool_dev->thermal_instances, cdev_node) {
		if (!strncmp(pos->cdev->type, dev_name, sizeof(dev_name))) {
			cool_dev->ops->notify_state(pos, pos->trip,
					THERMAL_TRIP_HOT);
			break;
		}
	}

	mutex_lock(&cooling_list_lock);
	list_add(&ddr_dev->node, &ddr_dev_list);
	mutex_unlock(&cooling_list_lock);

	return cool_dev;
}
EXPORT_SYMBOL_GPL(ddr_cooling_register);

/**
 * cpucore_cooling_unregister - function to remove cpucore cooling device.
 * @cdev: thermal cooling device pointer.
 *
 * This interface function unregisters the "thermal-cpucore-%x" cooling device.
 */
void ddr_cooling_unregister(struct thermal_cooling_device *cdev)
{
	struct ddr_cooling_device *ddr_dev;

	if (!cdev)
		return;

	iounmap(ddr_reg0);

	ddr_dev = cdev->devdata;

	mutex_lock(&cooling_list_lock);
	list_del(&ddr_dev->node);
	mutex_unlock(&cooling_list_lock);

	thermal_cooling_device_unregister(ddr_dev->cool_dev);
	release_idr(&ddr_idr, ddr_dev->id);
	kfree(ddr_dev);
}
EXPORT_SYMBOL_GPL(ddr_cooling_unregister);
