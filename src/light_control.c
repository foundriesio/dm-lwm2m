/*
 * Copyright (c) 2016-2017 Linaro Limited
 * Copyright (c) 2017-2018 Open Source Foundries Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define SYS_LOG_DOMAIN "fota/light"
#define SYS_LOG_LEVEL CONFIG_SYS_LOG_FOTA_LEVEL
#include <logging/sys_log.h>

#include <zephyr.h>
#include <board.h>
#include <gpio.h>
#include <net/lwm2m.h>

/* Defines for the IPSO light-control elements */
#define LED_GPIO_PIN		LED0_GPIO_PIN
#if defined(LED0_GPIO_PORT)
#define LED_GPIO_PORT		LED0_GPIO_PORT
#else
#define LED_GPIO_PORT		LED0_GPIO_CONTROLLER
#endif

static struct device *led_dev;
static u8_t led_current;

/* TODO: Move to a pre write hook that can handle ret codes once available */
static int on_off_cb(u16_t obj_inst_id, u8_t *data, u16_t data_len,
		     bool last_block, size_t total_size)
{
	int ret = 0;
	u8_t led_val;

	if (data_len != 1) {
		SYS_LOG_ERR("Length of on_off callback data is incorrect! (%u)",
			    data_len);
		return -EINVAL;
	}

	led_val = *data;
	if (led_val != led_current) {
		ret = gpio_pin_write(led_dev, LED_GPIO_PIN,
				     IS_ENABLED(CONFIG_FOTA_LED_GPIO_INVERTED) ?
					!led_val : led_val);
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

int init_light_control(void)
{
	int ret;

	led_dev = device_get_binding(LED_GPIO_PORT);
	SYS_LOG_INF("%s LED GPIO port %s",
			led_dev ? "Found" : "Did not find",
			LED_GPIO_PORT);

	if (!led_dev) {
		SYS_LOG_ERR("No LED device found.");
		ret = -ENODEV;
		goto fail;
	}

	ret = gpio_pin_configure(led_dev, LED_GPIO_PIN, GPIO_DIR_OUT);
	if (ret) {
		SYS_LOG_ERR("Error configuring LED GPIO.");
		goto fail;
	}

	ret = gpio_pin_write(led_dev, LED_GPIO_PIN,
			     IS_ENABLED(CONFIG_FOTA_LED_GPIO_INVERTED) ? 1 : 0);
	if (ret) {
		SYS_LOG_ERR("Error setting LED GPIO.");
		goto fail;
	}

	ret = lwm2m_engine_create_obj_inst("3311/0");
	if (ret < 0) {
		goto fail;
	}

	ret = lwm2m_engine_register_post_write_callback("3311/0/5850",
							on_off_cb);
	if (ret < 0) {
		goto fail;
	}

	return 0;

fail:
	return ret;
}
