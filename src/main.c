/*
 * Copyright (c) 2016-2017 Linaro Limited
 * Copyright (c) 2018 Open Source Foundries Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define SYS_LOG_DOMAIN "fota/main"
#define SYS_LOG_LEVEL CONFIG_SYS_LOG_FOTA_LEVEL
#include <logging/sys_log.h>

#include <zephyr.h>
#include <sensor.h>
#include <board.h>
#include <gpio.h>
#include <net/lwm2m.h>
#include <tc_util.h>

/* Local helpers and functions */
#include "tstamp_log.h"
#include "app_work_queue.h"
#include "product_id.h"
#include "lwm2m.h"

/* Defines and configs for the IPSO elements */
#define MCU_TEMP_DEV		"fota-temp"
#define LED_GPIO_PIN		LED0_GPIO_PIN
#define LED_GPIO_PORT		LED0_GPIO_PORT

static struct device *mcu_dev;
static struct device *led_dev;
static u32_t led_current;
static struct float32_value temp_float;

static int read_temperature(struct device *temp_dev,
			    struct float32_value *float_val)
{
	__unused const char *name = temp_dev->config->name;
	struct sensor_value temp_val;
	int ret;

	ret = sensor_sample_fetch(temp_dev);
	if (ret) {
		SYS_LOG_ERR("%s: I/O error: %d", name, ret);
		return ret;
	}

	ret = sensor_channel_get(temp_dev, SENSOR_CHAN_TEMP, &temp_val);
	if (ret) {
		SYS_LOG_ERR("%s: can't get data: %d", name, ret);
		return ret;
	}

	SYS_LOG_DBG("%s: read %d.%d C",
			name, temp_val.val1, temp_val.val2);
	float_val->val1 = temp_val.val1;
	float_val->val2 = temp_val.val2;

	return 0;
}

static void *temp_read_cb(u16_t obj_inst_id, size_t *data_len)
{
	/* Only object instance 0 is currently used */
	if (obj_inst_id != 0) {
		*data_len = 0;
		return NULL;
	}

	/*
	 * No need to check if read was successful, just reuse the
	 * previous value which is already stored at temp_float.
	 * This is because there is currently no way to report read_cb
	 * failures to the LWM2M engine.
	 */
	read_temperature(mcu_dev, &temp_float);
	lwm2m_engine_set_float32("3303/0/5700", &temp_float);
	*data_len = sizeof(temp_float);

	return &temp_float;
}

/* TODO: Move to a pre write hook that can handle ret codes once available */
static int led_on_off_cb(u16_t obj_inst_id, u8_t *data, u16_t data_len,
			 bool last_block, size_t total_size)
{
	int ret = 0;
	u32_t led_val;

	led_val = *(u8_t *) data;
	if (led_val != led_current) {
		ret = gpio_pin_write(led_dev, LED_GPIO_PIN, led_val);
		if (ret) {
			/*
			 * We need an extra hook in LWM2M to better handle
			 * failures before writing the data value and not in
			 * post_write_cb, as there is not much that can be
			 * done here.
			 */
			SYS_LOG_ERR("Fail to write to GPIO %d", LED_GPIO_PIN);
			return ret;
		}
		led_current = led_val;
		/* TODO: Move to be set by the IPSO object itself */
		lwm2m_engine_set_s32("3311/0/5852", 0);
	}

	return ret;
}

static int init_temp_device(void)
{
	mcu_dev = device_get_binding(MCU_TEMP_DEV);
	SYS_LOG_INF("%s MCU temperature sensor %s",
			mcu_dev ? "Found" : "Did not find",
			MCU_TEMP_DEV);

	if (!mcu_dev) {
		SYS_LOG_ERR("No temperature device found.");
		return -ENODEV;
	}

	return 0;
}

static int init_led_device(void)
{
	int ret;

	led_dev = device_get_binding(LED_GPIO_PORT);
	SYS_LOG_INF("%s LED GPIO port %s",
			led_dev ? "Found" : "Did not find",
			LED_GPIO_PORT);

	if (!led_dev) {
		SYS_LOG_ERR("No LED device found.");
		return -ENODEV;
	}

	ret = gpio_pin_configure(led_dev, LED_GPIO_PIN, GPIO_DIR_OUT);
	if (ret) {
		SYS_LOG_ERR("Error configuring LED GPIO.");
		return ret;
	}

	ret = gpio_pin_write(led_dev, LED_GPIO_PIN, 0);
	if (ret) {
		SYS_LOG_ERR("Error setting LED GPIO.");
		return ret;
	}

	return 0;
}

void main(void)
{
	tstamp_hook_install();
	app_wq_init();

	SYS_LOG_INF("Open Source Foundries FOTA LWM2M example application");
	SYS_LOG_INF("Device: %s, Serial: %x",
		    product_id_get()->name, product_id_get()->number);

	TC_START("Running Built in Self Test (BIST)");

	TC_PRINT("Initializing LWM2M IPSO Temperature Sensor\n");
	if (init_temp_device()) {
		_TC_END_RESULT(TC_FAIL, "init_temp_device");
		TC_END_REPORT(TC_FAIL);
		return;
	}
	lwm2m_engine_create_obj_inst("3303/0");
	lwm2m_engine_register_read_callback("3303/0/5700", temp_read_cb);
	lwm2m_engine_set_string("3303/0/5701", "Cel");
	_TC_END_RESULT(TC_PASS, "init_temp_device");

	TC_PRINT("Initializing LWM2M IPSO Light Control\n");
	if (init_led_device()) {
		_TC_END_RESULT(TC_FAIL, "init_led_device");
		TC_END_REPORT(TC_FAIL);
		return;
	}
	lwm2m_engine_create_obj_inst("3311/0");
	lwm2m_engine_register_post_write_callback("3311/0/5850",
			led_on_off_cb);
	_TC_END_RESULT(TC_PASS, "init_led_device");
	TC_END_REPORT(TC_PASS);

	if (lwm2m_init()) {
		return;
	}

	/*
	 * From this point on, just handle work.
	 */
	app_wq_run();
}
