/*
 * Copyright (c) 2017 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define SYS_LOG_DOMAIN "fota/lwm2m"
#define SYS_LOG_LEVEL CONFIG_SYS_LOG_FOTA_LEVEL
#include <logging/sys_log.h>

#include <zephyr.h>
#include <misc/reboot.h>
#include <net/net_core.h>
#include <net/net_pkt.h>
#include <net/net_app.h>
#include <net/net_mgmt.h>
#include <net/lwm2m.h>
#include <stdio.h>
#include <version.h>
#include <sensor.h>
#include <board.h>
#include <gpio.h>

#include "flash_block.h"
#include "mcuboot.h"
#include "product_id.h"

#define WAIT_TIME	K_SECONDS(10)

/* Network configuration checks */
#if defined(CONFIG_NET_IPV6)
BUILD_ASSERT_MSG(sizeof(CONFIG_NET_APP_PEER_IPV6_ADDR) > 1,
		"CONFIG_NET_APP_PEER_IPV6_ADDR must be defined in boards/$(BOARD)-local.conf");
#endif
#if defined(CONFIG_NET_IPV4)
#if !defined(CONFIG_NET_DHCPV4)
BUILD_ASSERT_MSG(sizeof(CONFIG_NET_APP_MY_IPV4_ADDR) > 1,
		"DHCPv4 must be enabled, or CONFIG_NET_APP_MY_IPV4_ADDR must be defined, in boards/$(BOARD)-local.conf");
#endif
BUILD_ASSERT_MSG(sizeof(CONFIG_NET_APP_PEER_IPV4_ADDR) > 1,
		"CONFIG_NET_APP_PEER_IPV4_ADDR must be defined in boards/$(BOARD)-local.conf");
#endif

#define CLIENT_MANUFACTURER	"Zephyr"
#define CLIENT_FIRMWARE_VER	"1.0"
#define CLIENT_DEVICE_TYPE	"Zephyr OMA-LWM2M Client"
#define CLIENT_HW_VER		CONFIG_SOC

#define MCU_TEMP_DEV		"fota-mcu-temp"

#define LED_GPIO_PIN		LED0_GPIO_PIN
#define LED_GPIO_PORT		LED0_GPIO_PORT

#define ENDPOINT_LEN		33
static char ep_name[ENDPOINT_LEN];

static struct net_app_ctx app_ctx;
static struct device *mcu_dev;
static struct device *led_dev;
static u32_t led_current;

/*
 * TODO: Find a better way to handle update markers, and if possible
 * identify a common solution that could also be shared with hawkbit
 * or other fota-implementations (e.g. common file-system structure).
 */
struct update_counter {
	int current;
	int update;
};

typedef enum {
	COUNTER_CURRENT = 0,
	COUNTER_UPDATE,
} update_counter_t;

static struct k_delayed_work reboot_work;
static struct net_mgmt_event_callback cb;

NET_PKT_TX_SLAB_DEFINE(lwm2m_tx_udp, 5);
NET_PKT_DATA_POOL_DEFINE(lwm2m_data_udp, 20);

static struct k_mem_slab *tx_udp_slab(void)
{
	return &lwm2m_tx_udp;
}

static struct net_buf_pool *data_udp_pool(void)
{
	return &lwm2m_data_udp;
}

extern struct device *flash_dev;
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

static int lwm2m_update_counter_read(struct update_counter *update_counter)
{
	return flash_read(flash_dev, FLASH_AREA_APPLICATION_STATE_OFFSET,
			update_counter, sizeof(*update_counter));
}

static int lwm2m_update_counter_update(update_counter_t type, u32_t new_value)
{
	struct update_counter update_counter;
	int ret;

	flash_read(flash_dev, FLASH_AREA_APPLICATION_STATE_OFFSET,
			&update_counter, sizeof(update_counter));
	if (type == COUNTER_UPDATE) {
		update_counter.update = new_value;
	} else {
		update_counter.current = new_value;
	}

	flash_write_protection_set(flash_dev, false);
	ret = flash_erase(flash_dev, FLASH_AREA_APPLICATION_STATE_OFFSET,
			  FLASH_AREA_APPLICATION_STATE_SIZE);
	flash_write_protection_set(flash_dev, true);
	if (ret) {
		return ret;
	}

	flash_write_protection_set(flash_dev, false);
	ret = flash_write(flash_dev, FLASH_AREA_APPLICATION_STATE_OFFSET,
			  &update_counter, sizeof(update_counter));
	flash_write_protection_set(flash_dev, true);

	return ret;
}

static void reboot(struct k_work *work)
{
	SYS_LOG_INF("Rebooting device");
	sys_reboot(0);
}

static int device_reboot_cb(u16_t obj_inst_id)
{
	SYS_LOG_INF("DEVICE: Reboot in progress");
	k_delayed_work_submit(&reboot_work, MSEC_PER_SEC);

	return 0;
}

static int firmware_update_cb(u16_t obj_inst_id)
{
	struct update_counter update_counter;
	u8_t state = lwm2m_engine_get_u8("5/0/3");
	int ret = 0;

	if (state != STATE_DOWNLOADED) {
		SYS_LOG_ERR("Cannot execute update, firmware not downloaded");
		return -EPERM;
	}

	SYS_LOG_DBG("Executing firmware update");
	lwm2m_engine_set_u8("5/0/3", STATE_UPDATING);
	lwm2m_engine_set_u8("5/0/5", RESULT_DEFAULT);

	/* Bump update counter so it can be verified on the next reboot */
	ret = lwm2m_update_counter_read(&update_counter);
	if (ret) {
		SYS_LOG_ERR("Failed read update counter");
		goto cleanup;
	}
	SYS_LOG_INF("Update Counter: current %d, update %d",
			update_counter.current, update_counter.update);
	ret = lwm2m_update_counter_update(COUNTER_UPDATE,
			update_counter.current + 1);
	if (ret) {
		SYS_LOG_ERR("Failed to update the update counter: %d", ret);
		goto cleanup;
	}

	boot_trigger_ota();

	k_delayed_work_submit(&reboot_work, MSEC_PER_SEC);

	return 0;

cleanup:
	lwm2m_engine_set_u8("5/0/3", STATE_DOWNLOADED);
	lwm2m_engine_set_u8("5/0/5", RESULT_UPDATE_FAILED);

	return ret;
}

static int firmware_block_received_cb(u16_t obj_inst_id,
				      u8_t *data, u16_t data_len,
				      bool last_block, size_t total_size)
{
	static int bytes_written;
	static u8_t percent_downloaded;
	u8_t downloaded;
	int ret = 0;

	if (total_size > FLASH_BANK_SIZE) {
		SYS_LOG_ERR("Artifact file size too big (%d)", total_size);
		lwm2m_engine_set_u8("5/0/3", STATE_IDLE);
		lwm2m_engine_set_u8("5/0/5", RESULT_NO_STORAGE);
		return -EINVAL;
	}

	/* Erase bank 1 before starting the write process */
	if (bytes_written == 0) {
		flash_write_protection_set(flash_dev, false);
		ret = flash_erase(flash_dev, FLASH_AREA_IMAGE_1_OFFSET,
					FLASH_BANK_SIZE);
		flash_write_protection_set(flash_dev, true);
		if (ret != 0) {
			SYS_LOG_ERR("Failed to erase flash bank 1");
			goto cleanup;
		}
	}

	if (data_len > 0) {
		ret = flash_block_write(flash_dev, FLASH_AREA_IMAGE_1_OFFSET,
					&bytes_written, data, data_len,
					last_block);
		if (ret < 0) {
			SYS_LOG_ERR("Failed to write flash block");
			goto cleanup;
		}
	}

	/* display a % downloaded, if it's different */
	if (total_size) {
		downloaded = bytes_written * 100 / total_size;
	} else {
		/* Total size is empty when there is only one block */
		downloaded = 100;
	}

	if (downloaded > percent_downloaded) {
		percent_downloaded = downloaded;
		SYS_LOG_INF("%d%%", percent_downloaded);
	}

	if (last_block) {
		bytes_written = 0;
		percent_downloaded = 0;
	}

	return ret;

cleanup:
	bytes_written = 0;
	percent_downloaded = 0;
	lwm2m_engine_set_u8("5/0/3", STATE_IDLE);
	lwm2m_engine_set_u8("5/0/5", RESULT_INTEGRITY_FAILED);

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


static int lwm2m_setup(void)
{
	const struct product_id_t *product_id = product_id_get();
	char device_serial_no[10];

	snprintf(device_serial_no, sizeof(device_serial_no), "%08x",
			product_id->number);
	strncpy(ep_name, device_serial_no, ENDPOINT_LEN);
	SYS_LOG_INF("LWM2M Endpoint Client Name: %s", ep_name);

	/* Device Object values and callbacks */
	lwm2m_engine_set_string("3/0/0", CLIENT_MANUFACTURER);
	lwm2m_engine_set_string("3/0/1", (char *) product_id->name);
	lwm2m_engine_set_string("3/0/2", device_serial_no);
	lwm2m_engine_set_string("3/0/3", CLIENT_FIRMWARE_VER);
	lwm2m_engine_register_exec_callback("3/0/4", device_reboot_cb);
	lwm2m_engine_set_string("3/0/17", CLIENT_DEVICE_TYPE);
	lwm2m_engine_set_string("3/0/18", CLIENT_HW_VER);
	lwm2m_engine_set_string("3/0/19", KERNEL_VERSION_STRING);
	lwm2m_engine_set_u32("3/0/21", (int) (FLASH_BANK_SIZE / 1024));

	/* Firmware Object callbacks */
	lwm2m_engine_register_post_write_callback("5/0/0",
					     firmware_block_received_cb);
	lwm2m_firmware_set_write_cb(firmware_block_received_cb);
	lwm2m_engine_register_exec_callback("5/0/2", firmware_update_cb);

	/* IPSO: Temperature Sensor object */
	if (init_temp_device() == 0) {
		lwm2m_engine_create_obj_inst("3303/0");
		lwm2m_engine_register_read_callback("3303/0/5700",
					temp_read_cb);
		lwm2m_engine_set_string("3303/0/5701", "Cel");
	}

	/* IPSO: Light Control object */
	if (init_led_device() == 0) {
		lwm2m_engine_create_obj_inst("3311/0");
		lwm2m_engine_register_post_write_callback("3311/0/5850",
					led_on_off_cb);
	}

	/* Reboot work, used when executing update */
	k_delayed_work_init(&reboot_work, reboot);

	return 0;
}

static int setup_net_app_ctx(struct net_app_ctx *ctx, const char *peer)
{
	int ret;

	net_app_set_net_pkt_pool(ctx, tx_udp_slab, data_udp_pool);

	ret = net_app_init_udp_client(ctx, NULL, NULL, peer,
				      CONFIG_LWM2M_PEER_PORT, WAIT_TIME, NULL);
	if (ret < 0) {
		SYS_LOG_ERR("Cannot init UDP client (%d)", ret);
		return ret;
	}

	return ret;
}

static void event_iface_up(struct net_mgmt_event_callback *cb,
		u32_t mgmt_event, struct net_if *iface)
{
	int ret;

	if (!iface) {
		SYS_LOG_ERR("No network interface specified!");
		return;
	}

#if defined(CONFIG_NET_IPV6)
	ret = setup_net_app_ctx(&app_ctx, CONFIG_NET_APP_PEER_IPV6_ADDR);
	if (ret < 0) {
		SYS_LOG_ERR("Fail to setup net_app ctx");
		return;
	}

	ret = lwm2m_engine_start(app_ctx.ipv6.ctx);
	if (ret < 0) {
		SYS_LOG_ERR("Cannot init LWM2M IPv6 engine (%d)", ret);
		goto cleanup;
	}

	ret = lwm2m_rd_client_start(app_ctx.ipv6.ctx, &app_ctx.ipv6.remote,
				    ep_name);
	if (ret < 0) {
		SYS_LOG_ERR("LWM2M init LWM2M IPv6 RD client error (%d)",
			ret);
		goto cleanup;
	}

	SYS_LOG_INF("IPv6 setup complete.");
#else /* CONFIG_NET_IPV4 */
	ret = setup_net_app_ctx(&app_ctx, CONFIG_NET_APP_PEER_IPV4_ADDR);
	if (ret < 0) {
		SYS_LOG_ERR("Fail to setup net_app ctx");
		return;
	}

	ret = lwm2m_engine_start(app_ctx.ipv4.ctx);
	if (ret < 0) {
		SYS_LOG_ERR("Cannot init LWM2M IPv4 engine (%d)", ret);
		goto cleanup;
	}

	ret = lwm2m_rd_client_start(app_ctx.ipv4.ctx, &app_ctx.ipv4.remote,
				    ep_name);
	if (ret < 0) {
		SYS_LOG_ERR("LWM2M init LWM2M IPv4 RD client error (%d)",
			ret);
		goto cleanup;
	}

	SYS_LOG_INF("IPv4 setup complete.");
#endif
	return;

cleanup:
	net_app_close(&app_ctx);
	net_app_release(&app_ctx);
}

/* TODO: Make it generic, and if possible sharing with hawkbit via library */
static int lwm2m_image_init(void)
{
	int ret = 0;
	struct update_counter counter;
	u8_t boot_status;

	/* Update boot status and update counter */
	ret = lwm2m_update_counter_read(&counter);
	if (ret) {
		SYS_LOG_ERR("Failed read update counter");
		return ret;
	}
	SYS_LOG_INF("Update Counter: current %d, update %d",
			counter.current, counter.update);
	boot_status = boot_status_read();
	SYS_LOG_INF("Current boot status %x", boot_status);
	if (boot_status == BOOT_STATUS_ONGOING) {
		boot_status_update();
		SYS_LOG_INF("Updated boot status to %x", boot_status_read());
		ret = boot_erase_flash_bank(FLASH_AREA_IMAGE_1_OFFSET);
		if (ret) {
			SYS_LOG_ERR("Flash bank erase at offset %x: error %d",
					FLASH_AREA_IMAGE_1_OFFSET, ret);
			return ret;
		}
		SYS_LOG_DBG("Erased flash bank at offset %x",
				FLASH_AREA_IMAGE_1_OFFSET);
		if (counter.update != -1) {
			ret = lwm2m_update_counter_update(COUNTER_CURRENT,
						counter.update);
			if (ret) {
				SYS_LOG_ERR("Failed to update the update "
					    "counter: %d", ret);
				return ret;
			}
			ret = lwm2m_update_counter_read(&counter);
			if (ret) {
				SYS_LOG_ERR("Failed read update counter");
				return ret;
			}
			SYS_LOG_INF("Update Counter updated");
		}
	}

	/* Check if a firmware update status needs to be reported */
	if (counter.update != -1 &&
			counter.current == counter.update) {
		/* Successful update */
		SYS_LOG_INF("Firmware updated successfully");
		lwm2m_engine_set_u8("5/0/5", RESULT_SUCCESS);
	} else if (counter.update > counter.current) {
		/* Failed update */
		SYS_LOG_INF("Firmware failed to be updated");
		lwm2m_engine_set_u8("5/0/5", RESULT_UPDATE_FAILED);
	}

	return ret;
}

int lwm2m_init(void)
{
	struct net_if *iface;
	int ret;

	ret = lwm2m_image_init();
	if (ret < 0) {
		SYS_LOG_ERR("Failed to setup image properties (%d)", ret);
		return ret;
	}

	ret = lwm2m_setup();
	if (ret < 0) {
		SYS_LOG_ERR("Cannot setup LWM2M fields (%d)", ret);
		return ret;
	}

	iface = net_if_get_default();
	if (!iface) {
		SYS_LOG_ERR("Cannot find default network interface!");
		return -ENETDOWN;
	}

	/* Subscribe to NET_IF_UP if interface is not ready */
	if (!atomic_test_bit(iface->flags, NET_IF_UP)) {
		net_mgmt_init_event_callback(&cb, event_iface_up,
				NET_EVENT_IF_UP);
		net_mgmt_add_event_callback(&cb);
	} else {
		event_iface_up(NULL, NET_EVENT_IF_UP, iface);
	}

	return 0;
}
