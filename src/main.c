/*
 * Copyright (c) 2016-2017 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define SYS_LOG_DOMAIN "fota/main"
#define SYS_LOG_LEVEL SYS_LOG_LEVEL_DEBUG
#include <logging/sys_log.h>

#include <misc/stack.h>
#include <gpio.h>
#include <tc_util.h>
#include <misc/reboot.h>
#include <board.h>

/* Local helpers and functions */
#include "tstamp_log.h"
#include "app_work_queue.h"
#include "mcuboot.h"
#include "product_id.h"
#include "lwm2m.h"
#if defined(CONFIG_BLUETOOTH)
#include <bluetooth/conn.h>
#include "bt_storage.h"
#include "bt_ipss.h"
#endif

/*
 * GPIOs. These can be customized by device if needed.
 */
#if defined(CONFIG_BOARD_96B_NITROGEN) || defined(CONFIG_BOARD_96B_CARBON)
#define BT_CONNECT_LED	BT_GPIO_PIN
#define GPIO_DRV_BT	BT_GPIO_PORT
#endif

struct device *flash_dev;

#if defined(CONFIG_BLUETOOTH)
/* BT LE Connect/Disconnect callbacks */
static void set_bluetooth_led(bool state)
{
#if defined(GPIO_DRV_BT) && defined(BT_CONNECT_LED)
	struct device *gpio;

	gpio = device_get_binding(GPIO_DRV_BT);
	gpio_pin_configure(gpio, BT_CONNECT_LED, GPIO_DIR_OUT);
	gpio_pin_write(gpio, BT_CONNECT_LED, state);
#endif
}

static void connected(struct bt_conn *conn, u8_t err)
{
	if (err) {
		SYS_LOG_ERR("BT LE Connection failed: %u", err);
	} else {
		SYS_LOG_INF("BT LE Connected");
		set_bluetooth_led(1);
		err = ipss_set_connected();
		if (err) {
			SYS_LOG_ERR("BT LE connection name change failed: %u",
				    err);
		}
	}
}

static void disconnected(struct bt_conn *conn, u8_t reason)
{
	SYS_LOG_ERR("BT LE Disconnected (reason %u), rebooting!", reason);
	set_bluetooth_led(0);
	sys_reboot(0);
}

static struct bt_conn_cb conn_callbacks = {
	.connected = connected,
	.disconnected = disconnected,
};
#endif

void main(void)
{
#if defined(CONFIG_BLUETOOTH)
	int err;
#endif

	tstamp_hook_install();
	app_wq_init();

	SYS_LOG_INF("Linaro FOTA LWM2M example application");
	SYS_LOG_INF("Device: %s, Serial: %x",
		    product_id_get()->name, product_id_get()->number);

	TC_START("Running Built in Self Test (BIST)");

#if defined(CONFIG_BLUETOOTH)
	/* Storage used to provide a BT MAC based on the serial number */
	TC_PRINT("Setting Bluetooth MAC\n");
	bt_storage_init();

	TC_PRINT("Enabling Bluetooth\n");
	err = bt_enable(NULL);
	if (err) {
		SYS_LOG_ERR("Bluetooth init failed: %d", err);
		_TC_END_RESULT(TC_FAIL, "bt_enable");
		TC_END_REPORT(TC_FAIL);
		return;
	}
	_TC_END_RESULT(TC_PASS, "bt_enable");

	/* Callbacks for BT LE connection state */
	TC_PRINT("Registering Bluetooth LE connection callbacks\n");
	err = ipss_init(&conn_callbacks);
	if (err) {
		SYS_LOG_ERR("BT GATT attributes failed to set: %d", err);
		_TC_END_RESULT(TC_FAIL, "ipss_init");
		TC_END_REPORT(TC_FAIL);
		return;
	}
	_TC_END_RESULT(TC_PASS, "ipss_init");

	TC_PRINT("Advertising Bluetooth IP Profile\n");
	err = ipss_advertise();
	if (err) {
		SYS_LOG_ERR("Advertising failed to start: %d", err);
		_TC_END_RESULT(TC_FAIL, "ipss_advertise");
		TC_END_REPORT(TC_FAIL);
		return;
	}
	_TC_END_RESULT(TC_PASS, "ipss_advertise");
#endif

	TC_PRINT("Initializing LWM2M\n");
	if (lwm2m_init()) {
		_TC_END_RESULT(TC_FAIL, "lwm2m_init");
		TC_END_REPORT(TC_FAIL);
		return;
	}
	_TC_END_RESULT(TC_PASS, "lwm2m_init");

	TC_END_REPORT(TC_PASS);

	/*
	 * From this point on, just handle work.
	 */
	app_wq_run();
}
