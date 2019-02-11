/*
 * Copyright (c) 2017 Linaro Limited
 * Copyright (c) 2018-2019 Foundries.io
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME fota_lwm2m
#define LOG_LEVEL CONFIG_FOTA_LOG_LEVEL

#include <logging/log.h>
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

#include <zephyr.h>
#include <dfu/mcuboot.h>
#include <dfu/flash_img.h>
#include <flash.h>
#include <logging/log_ctrl.h>
#include <misc/reboot.h>
#include <net/net_if.h>
#include <net/net_mgmt.h>
#include <net/lwm2m.h>
#include <ctype.h>
#include <stdio.h>
#include <version.h>
#include <tc_util.h>
#if defined(CONFIG_MODEM_RECEIVER)
#include <drivers/modem/modem_receiver.h>
#endif

#include "product_id.h"
#include "lwm2m_credentials.h"
#include "app_work_queue.h"
#ifdef CONFIG_NET_L2_BT
#include "bluetooth.h"
#endif

/* Network configuration checks */
#if defined(CONFIG_NET_IPV6)
BUILD_ASSERT_MSG(sizeof(CONFIG_NET_CONFIG_PEER_IPV6_ADDR) > 1,
		"CONFIG_NET_CONFIG_PEER_IPV6_ADDR must be defined in boards/$(BOARD)-local.conf");
#endif
#if defined(CONFIG_NET_IPV4)
#if !defined(CONFIG_NET_DHCPV4)
BUILD_ASSERT_MSG(sizeof(CONFIG_NET_CONFIG_MY_IPV4_ADDR) > 1,
		"DHCPv4 must be enabled, or CONFIG_NET_CONFIG_MY_IPV4_ADDR must be defined, in boards/$(BOARD)-local.conf");
#endif
BUILD_ASSERT_MSG(sizeof(CONFIG_NET_CONFIG_PEER_IPV4_ADDR) > 1,
		"CONFIG_NET_CONFIG_PEER_IPV4_ADDR must be defined in boards/$(BOARD)-local.conf");
#endif
#if defined(CONFIG_LWM2M_FIRMWARE_UPDATE_PULL_COAP_PROXY_SUPPORT)
BUILD_ASSERT_MSG(sizeof(CONFIG_LWM2M_FIRMWARE_UPDATE_PULL_COAP_PROXY_ADDR) > 1,
		"CONFIG_LWM2M_FIRMWARE_UPDATE_PULL_COAP_PROXY_ADDR must be defined in boards/$(BOARD)-local.conf");
#endif

#if defined(CONFIG_NET_IPV6)
#define SERVER_ADDR	CONFIG_NET_CONFIG_PEER_IPV6_ADDR
#elif defined(CONFIG_NET_IPV4)
#define SERVER_ADDR	CONFIG_NET_CONFIG_PEER_IPV4_ADDR
#else
#error "Please enable IPv6 or IPv4"
#endif

#define CLIENT_MANUFACTURER	"Zephyr"
#define CLIENT_DEVICE_TYPE	"Zephyr OMA-LWM2M Client"
#define CLIENT_HW_VER		CONFIG_SOC

#define NUM_TEST_RESULTS	5

#if defined(CONFIG_LWM2M_DTLS_SUPPORT)
#define TLS_TAG			1

#define DEFAULT_CLIENT_PSK	"000102030405060708090a0b0c0d0e0f"

/* DTLS information read from credential partition */
static char client_psk[LWM2M_DEVICE_TOKEN_SIZE];
static u8_t client_psk_bin[LWM2M_DEVICE_TOKEN_HEX_SIZE];
#endif /* CONFIG_LWM2M_DTLS_SUPPORT */

#define FLASH_BANK_SIZE FLASH_AREA_IMAGE_1_SIZE

struct update_lwm2m_data {
	int failures;

	/* For test reporting */
	struct k_work tc_work;
	u8_t tc_results[NUM_TEST_RESULTS];
	u8_t tc_count;
};

static struct update_lwm2m_data update_data;
static bool tc_logging;

static char ep_name[LWM2M_DEVICE_ID_SIZE];

static struct device *flash_dev;
static struct flash_img_context dfu_ctx;
static struct lwm2m_ctx client;

/* storage location for firmware package */
static u8_t firmware_buf[CONFIG_LWM2M_COAP_BLOCK_SIZE];
/* storage location for firmware version */
static char firmware_version[32];

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
static struct k_work net_event_work;
static struct k_work_q *net_event_work_q;

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

static void *firmware_read_cb(u16_t obj_inst_id, size_t *data_len)
{
	*data_len = strlen(firmware_version);

	return firmware_version;
}

static void reboot(struct k_work *work)
{
	LOG_INF("Rebooting device");
#ifdef CONFIG_NET_L2_BT
	bt_network_disable();
#endif
	LOG_PANIC();
	sys_reboot(0);
}

static int device_reboot_cb(u16_t obj_inst_id)
{
	LOG_INF("DEVICE: Reboot in progress");
	k_delayed_work_submit(&reboot_work, MSEC_PER_SEC);

	return 0;
}

#ifdef CONFIG_LWM2M_FIRMWARE_UPDATE_OBJ_SUPPORT
static int firmware_update_cb(u16_t obj_inst_id)
{
	struct update_counter update_counter;
	int ret = 0;

	LOG_DBG("Executing firmware update");

	/* Bump update counter so it can be verified on the next reboot */
	ret = lwm2m_update_counter_read(&update_counter);
	if (ret) {
		LOG_ERR("Failed read update counter");
		goto cleanup;
	}
	LOG_INF("Update Counter: current %d, update %d",
		update_counter.current, update_counter.update);
	ret = lwm2m_update_counter_update(COUNTER_UPDATE,
					  update_counter.current + 1);
	if (ret) {
		LOG_ERR("Failed to update the update counter: %d", ret);
		goto cleanup;
	}

	boot_request_upgrade(false);

	k_delayed_work_submit(&reboot_work, MSEC_PER_SEC);

	return 0;

cleanup:
	return ret;
}

static void *firmware_get_buf(u16_t obj_inst_id, size_t *data_len)
{
	*data_len = sizeof(firmware_buf);
	return firmware_buf;
}

static int firmware_block_received_cb(u16_t obj_inst_id,
				      u8_t *data, u16_t data_len,
				      bool last_block, size_t total_size)
{
#if defined(CONFIG_FOTA_ERASE_PROGRESSIVELY)
	static int last_offset = FLASH_AREA_IMAGE_1_OFFSET;
#endif
	static u8_t percent_downloaded;
	static u32_t bytes_downloaded;
	u8_t downloaded;
	int ret = 0;

	if (total_size > FLASH_BANK_SIZE) {
		LOG_ERR("Artifact file size too big (%d)", total_size);
		return -EINVAL;
	}

	if (!data_len) {
		LOG_ERR("Data len is zero, nothing to write.");
		return -EINVAL;
	}

	/* Erase bank 1 before starting the write process */
	if (bytes_downloaded == 0) {
		flash_img_init(&dfu_ctx, flash_dev);
#if defined(CONFIG_FOTA_ERASE_PROGRESSIVELY)
		LOG_INF("Download firmware started, erasing progressively.");
		/* reset image data */
		ret = boot_invalidate_slot1();
		if (ret != 0) {
			LOG_ERR("Failed to reset image data in bank 1");
			goto cleanup;
		}
#else
		LOG_INF("Download firmware started, erasing second bank");
		ret = boot_erase_img_bank(FLASH_AREA_IMAGE_1_OFFSET);
		if (ret != 0) {
			LOG_ERR("Failed to erase flash bank 1");
			goto cleanup;
		}
#endif
	}

	bytes_downloaded += data_len;

	/* display a % downloaded, if it's different */
	if (total_size) {
		downloaded = bytes_downloaded * 100 / total_size;
	} else {
		/* Total size is empty when there is only one block */
		downloaded = 100;
	}

	if (downloaded > percent_downloaded) {
		percent_downloaded = downloaded;
		LOG_INF("%d%%", percent_downloaded);
	}

#if defined(CONFIG_FOTA_ERASE_PROGRESSIVELY)
	/* Erase the sector that's going to be written to next */
	while (last_offset <
	       FLASH_AREA_IMAGE_1_OFFSET + dfu_ctx.bytes_written +
	       FLASH_ERASE_BLOCK_SIZE) {
		LOG_INF("Erasing sector at offset 0x%x", last_offset);
		flash_write_protection_set(flash_dev, false);
		ret = flash_erase(flash_dev, last_offset,
				  FLASH_ERASE_BLOCK_SIZE);
		flash_write_protection_set(flash_dev, true);
		last_offset += FLASH_ERASE_BLOCK_SIZE;
		if (ret) {
			LOG_ERR("Error %d while erasing sector", ret);
			goto cleanup;
		}
	}
#endif

	ret = flash_img_buffered_write(&dfu_ctx, data, data_len, last_block);
	if (ret < 0) {
		LOG_ERR("Failed to write flash block");
		goto cleanup;
	}

	if (!last_block) {
		/* Keep going */
		return ret;
	}

	if (total_size && (bytes_downloaded != total_size)) {
		LOG_ERR("Early last block, downloaded %d, expecting %d",
			bytes_downloaded, total_size);
		ret = -EIO;
	}

cleanup:
#if defined(CONFIG_FOTA_ERASE_PROGRESSIVELY)
	last_offset = FLASH_AREA_IMAGE_1_OFFSET;
#endif
	bytes_downloaded = 0;
	percent_downloaded = 0;

	return ret;
}
#endif

#if defined(CONFIG_LWM2M_DTLS_SUPPORT)
static int generate_hex(char *src, u8_t *dst, size_t dst_len)
{
	int src_len, i, j = 0;
	u8_t c = 0;

	src_len = strlen(src);
	for (i = 0; i < src_len; i++) {
		if (isdigit(src[i])) {
			c += src[i] - '0';
		} else if (isalpha(src[i])) {
			c += src[i] - (isupper(src[i]) ? 'A' - 10 : 'a' - 10);
		} else {
			return -EINVAL;
		}
		if (i % 2) {
			if (j >= dst_len) {
				return -E2BIG;
			}
			dst[j++] = c;
			c = 0;
		} else {
			c = c << 4;
		}
	}

	if (j != LWM2M_DEVICE_TOKEN_HEX_SIZE) {
		return -EINVAL;
	}

	return 0;
}
#endif /* CONFIG_LWM2M_DTLS_SUPPORT */

static int lwm2m_setup(void)
{
	const struct product_id_t *product_id = product_id_get();
	static char device_serial_no[10];
	char *server_url;
	u16_t server_url_len;
	u8_t server_url_flags;
	int ret;

	snprintk(device_serial_no, sizeof(device_serial_no), "%08x",
		 product_id->number);
	/* Check if there is a valid device id stored in the device */
	ret = lwm2m_get_device_id(flash_dev, ep_name);
	if (ret) {
		LOG_ERR("Fail to read LWM2M Device ID");
	}
#if defined(CONFIG_MODEM_RECEIVER)
	/* use IMEI */
	if (ret || ep_name[LWM2M_DEVICE_ID_SIZE - 1] != '\0') {
		struct mdm_receiver_context *mdm_ctx;

		mdm_ctx = mdm_receiver_context_from_id(0);
		if (mdm_ctx && mdm_ctx->data_imei) {
			memset(ep_name, 0, sizeof(ep_name));
			LOG_WRN("LWM2M Device ID not set, using IMEI");
			snprintk(ep_name, LWM2M_DEVICE_ID_SIZE, "zmp:imei:%s",
				 mdm_ctx->data_imei);
			ret = 0;
		}
	}
#endif /* CONFIG_MODEM_RECEIVER */
	if (ret || ep_name[LWM2M_DEVICE_ID_SIZE - 1] != '\0') {
		/* No UUID, use the serial number instead */
		LOG_WRN("LWM2M Device ID not set, using serial number");
		snprintk(ep_name, LWM2M_DEVICE_ID_SIZE, "zmp:sn:%s",
			 device_serial_no);
	}
	LOG_INF("LWM2M Endpoint Client Name: %s", ep_name);

#if defined(CONFIG_LWM2M_DTLS_SUPPORT)
	/* Check if there is a valid device token stored in the device */
	ret = lwm2m_get_device_token(flash_dev, client_psk);
	if (ret || client_psk[LWM2M_DEVICE_TOKEN_SIZE - 1] != '\0') {
		LOG_ERR("Fail to read LWM2M Device Token");

		/* No token, use the default key instead */
		strncpy(client_psk, DEFAULT_CLIENT_PSK,
			LWM2M_DEVICE_TOKEN_SIZE);
	}

	ret = generate_hex(client_psk, client_psk_bin,
			   LWM2M_DEVICE_TOKEN_HEX_SIZE);
	if (ret) {
		LOG_ERR("Failed to generate psk hex: %d", ret);
		return ret;
	}
#endif /* CONFIG_LWM2M_DTLS_SUPPORT */

	/* Server URL */
	ret = lwm2m_engine_get_res_data("0/0/0",
					(void **)&server_url, &server_url_len,
					&server_url_flags);
	if (ret < 0) {
		return ret;
	}

	snprintk(server_url, server_url_len, "coap%s//%s%s%s",
		 IS_ENABLED(CONFIG_LWM2M_DTLS_SUPPORT) ? "s:" : ":",
		 strchr(SERVER_ADDR, ':') ? "[" : "", SERVER_ADDR,
		 strchr(SERVER_ADDR, ':') ? "]" : "");

	/* Security Mode */
	lwm2m_engine_set_u8("0/0/2",
			    IS_ENABLED(CONFIG_LWM2M_DTLS_SUPPORT) ? 0 : 3);
#if defined(CONFIG_LWM2M_DTLS_SUPPORT)
	lwm2m_engine_set_string("0/0/3", (char *)ep_name);
	lwm2m_engine_set_opaque("0/0/5",
				(void *)client_psk_bin, sizeof(client_psk_bin));
#endif /* CONFIG_LWM2M_DTLS_SUPPORT */

	/* Device Object values and callbacks */
	lwm2m_engine_set_res_data("3/0/0", CLIENT_MANUFACTURER,
				  sizeof(CLIENT_MANUFACTURER),
				  LWM2M_RES_DATA_FLAG_RO);
	lwm2m_engine_set_res_data("3/0/1", (char *) product_id->name,
				  strlen(product_id->name) + 1,
				  LWM2M_RES_DATA_FLAG_RO);
	lwm2m_engine_set_res_data("3/0/2", device_serial_no,
				  sizeof(device_serial_no),
				  LWM2M_RES_DATA_FLAG_RO);
	lwm2m_engine_register_read_callback("3/0/3", firmware_read_cb);
	lwm2m_engine_register_exec_callback("3/0/4", device_reboot_cb);
	lwm2m_engine_set_res_data("3/0/17", CLIENT_DEVICE_TYPE,
				  sizeof(CLIENT_DEVICE_TYPE),
				  LWM2M_RES_DATA_FLAG_RO);
	lwm2m_engine_set_res_data("3/0/18", CLIENT_HW_VER,
				  sizeof(CLIENT_HW_VER),
				  LWM2M_RES_DATA_FLAG_RO);
	lwm2m_engine_set_res_data("3/0/19", KERNEL_VERSION_STRING,
				  sizeof(KERNEL_VERSION_STRING),
				  LWM2M_RES_DATA_FLAG_RO);
	lwm2m_engine_set_u32("3/0/21", (int) (FLASH_BANK_SIZE / 1024));

#ifdef CONFIG_LWM2M_FIRMWARE_UPDATE_OBJ_SUPPORT
	/* Firmware Object callbacks */
	/* setup data buffer for block-wise transfer */
	lwm2m_engine_register_pre_write_callback("5/0/0", firmware_get_buf);
	lwm2m_firmware_set_write_cb(firmware_block_received_cb);
	lwm2m_firmware_set_update_cb(firmware_update_cb);
#endif

	/* Reboot work, used when executing update */
	k_delayed_work_init(&reboot_work, reboot);

	return 0;
}

static void handle_test_result(struct update_lwm2m_data *data, u8_t result)
{
	if (!tc_logging || data->tc_count >= NUM_TEST_RESULTS) {
		return;
	}

	data->tc_results[data->tc_count++] = result;

	if (data->tc_count == NUM_TEST_RESULTS) {
		app_wq_submit(&data->tc_work);
	}
}

static void rd_client_event(struct lwm2m_ctx *client,
			    enum lwm2m_rd_client_event client_event)
{
	switch (client_event) {

	case LWM2M_RD_CLIENT_EVENT_NONE:
		/* do nothing */
		break;

	case LWM2M_RD_CLIENT_EVENT_BOOTSTRAP_REG_FAILURE:
		if (tc_logging) {
			_TC_END_RESULT(TC_FAIL, "lwm2m_registration");
			TC_END_REPORT(TC_FAIL);
			tc_logging = false;
		}
		break;

	case LWM2M_RD_CLIENT_EVENT_BOOTSTRAP_REG_COMPLETE:
		/* do nothing here and continue to registration */
		break;

	case LWM2M_RD_CLIENT_EVENT_BOOTSTRAP_TRANSFER_COMPLETE:
		/* do nothing here and continue to registration */
		break;

	case LWM2M_RD_CLIENT_EVENT_REGISTRATION_FAILURE:
		if (tc_logging) {
			_TC_END_RESULT(TC_FAIL, "lwm2m_registration");
			TC_END_REPORT(TC_FAIL);
			tc_logging = false;
		}
		break;

	case LWM2M_RD_CLIENT_EVENT_REGISTRATION_COMPLETE:
		if (tc_logging) {
			_TC_END_RESULT(TC_PASS, "lwm2m_registration");
		}
		break;

	case LWM2M_RD_CLIENT_EVENT_REG_UPDATE_FAILURE:
		handle_test_result(&update_data, TC_FAIL);
		break;

	case LWM2M_RD_CLIENT_EVENT_REG_UPDATE_COMPLETE:
		handle_test_result(&update_data, TC_PASS);
		break;

	case LWM2M_RD_CLIENT_EVENT_DEREGISTER_FAILURE:
		LOG_DBG("Deregister failure!");
		/* TODO: handle deregister? */
		break;

	case LWM2M_RD_CLIENT_EVENT_DISCONNECT:
		LOG_DBG("Disconnected");
		/* TODO: handle disconnect? */
		break;

	}
}

/* Log the semantic version number of the current image. */
static void log_img_ver(void)
{
	struct mcuboot_img_header header;
	struct mcuboot_img_sem_ver *ver;
	int ret;

	ret = boot_read_bank_header(FLASH_AREA_IMAGE_0_OFFSET,
				    &header, sizeof(header));
	if (ret) {
		LOG_ERR("can't read header: %d", ret);
		return;
	} else if (header.mcuboot_version != 1) {
		LOG_ERR("unsupported MCUboot version %u",
			header.mcuboot_version);
		return;
	}

	ver = &header.h.v1.sem_ver;
	snprintf(firmware_version, sizeof(firmware_version),
		 "%u.%u.%u build #%u", ver->major, ver->minor,
		 ver->revision, ver->build_num);
	LOG_INF("image version %s", firmware_version);
}

static int lwm2m_image_init(void)
{
	int ret = 0;
	struct update_counter counter;
	bool image_ok;

	/*
	 * Initialize the DFU context.
	 */
	flash_dev = device_get_binding(DT_FLASH_DEV_NAME);
	if (!flash_dev) {
		LOG_ERR("missing flash device %s", DT_FLASH_DEV_NAME);
		return -ENODEV;
	}

	log_img_ver();

	/* Update boot status and update counter */
	ret = lwm2m_update_counter_read(&counter);
	if (ret) {
		LOG_ERR("Failed read update counter");
		return ret;
	}
	LOG_INF("Update Counter: current %d, update %d",
		counter.current, counter.update);
	image_ok = boot_is_img_confirmed();
	LOG_INF("Image is%s confirmed OK", image_ok ? "" : " not");
	if (!image_ok) {
		ret = boot_write_img_confirmed();
		if (ret) {
			LOG_ERR("Couldn't confirm this image: %d", ret);
			return ret;
		}
		LOG_INF("Marked image as OK");
#if defined(CONFIG_FOTA_ERASE_PROGRESSIVELY)
		/* instead of erasing slot 1, reset image data */
		ret = boot_invalidate_slot1();
		if (ret) {
			LOG_ERR("Flash image 1 reset: error %d", ret);
			return ret;
		}

		LOG_DBG("Erased flash bank 1 at offset %x",
			FLASH_AREA_IMAGE_1_OFFSET);
#else
		ret = boot_erase_img_bank(FLASH_AREA_IMAGE_1_OFFSET);
		if (ret) {
			LOG_ERR("Flash bank erase at offset %x: error %d",
				FLASH_AREA_IMAGE_1_OFFSET, ret);
			return ret;
		}

		LOG_DBG("Erased flash bank 1 at offset %x",
			FLASH_AREA_IMAGE_1_OFFSET);
#endif

		if (counter.update != -1) {
			ret = lwm2m_update_counter_update(COUNTER_CURRENT,
						counter.update);
			if (ret) {
				LOG_ERR("Failed to update the update "
					"counter: %d", ret);
				return ret;
			}
			ret = lwm2m_update_counter_read(&counter);
			if (ret) {
				LOG_ERR("Failed to read update counter: %d",
					ret);
				return ret;
			}
			LOG_INF("Update Counter updated");
		}
	}

	/* Check if a firmware update status needs to be reported */
	if (counter.update != -1 &&
			counter.current == counter.update) {
		/* Successful update */
		LOG_INF("Firmware updated successfully");
		lwm2m_engine_set_u8("5/0/5", RESULT_SUCCESS);
	} else if (counter.update > counter.current) {
		/* Failed update */
		LOG_INF("Firmware failed to be updated");
		lwm2m_engine_set_u8("5/0/5", RESULT_UPDATE_FAILED);
	}

	return ret;
}

/*
 * This work handler prints the results for updating LwM2M registration.
 */
static void lwm2m_reg_update_result(struct k_work *work)
{
	struct update_lwm2m_data *data =
		CONTAINER_OF(work, struct update_lwm2m_data, tc_work);
	/*
	 * `result_name' is long enough for the function name, '_',
	 * two digits of test result, and '\0'. If NUM_TEST_RESULTS is
	 * 100 or more, space for more digits is needed.
	 */
	size_t result_len = strlen(__func__) + 1 + 2 + 1;
	char result_name[result_len];
	u8_t result, final_result = TC_PASS;
	size_t i;

	/* Ensure we have enough space to print the result name. */
	BUILD_ASSERT_MSG(NUM_TEST_RESULTS <= 99,
			 "result_len is too small to print test number");

	TC_PRINT("Update LwM2M registration\n");
	for (i = 0; i < data->tc_count; i++) {
		result = data->tc_results[i];
		snprintk(result_name, sizeof(result_name), "%s_%zu",
			 __func__, i);
		if (result == TC_FAIL) {
			final_result = TC_FAIL;
		}
		_TC_END_RESULT(result, result_name);
	}
	TC_END_REPORT(final_result);
	tc_logging = false;
}

static void lwm2m_start(struct k_work *work)
{
	int ret;

	TC_START("LwM2M tests");

	TC_PRINT("Initializing LWM2M Image\n");
	ret = lwm2m_image_init();
	if (ret < 0) {
		LOG_ERR("Failed to setup image properties (%d)", ret);
		_TC_END_RESULT(TC_FAIL, "lwm2m_image_init");
		TC_END_REPORT(TC_FAIL);
		return;
	}
	_TC_END_RESULT(TC_PASS, "lwm2m_image_init");

	TC_PRINT("Initializing LWM2M Engine\n");
	ret = lwm2m_setup();
	if (ret < 0) {
		LOG_ERR("Cannot setup LWM2M fields (%d)", ret);
		_TC_END_RESULT(TC_FAIL, "lwm2m_setup");
		TC_END_REPORT(TC_FAIL);
		return;
	}
	_TC_END_RESULT(TC_PASS, "lwm2m_setup");

	/* initialize test case data */
	update_data.failures = 0;
	k_work_init(&update_data.tc_work, lwm2m_reg_update_result);
	update_data.tc_count = 0;
	tc_logging = true;

	memset(&client, 0x0, sizeof(client));

#if defined(CONFIG_LWM2M_DTLS_SUPPORT)
	client.tls_tag = TLS_TAG;
#endif /* CONFIG_LWM2M_DTLS_SUPPORT */

	/* small delay to finalize networking */
	k_sleep(K_SECONDS(2));
	TC_PRINT("LwM2M registration\n");

	/* client.sec_obj_inst is 0 as a starting point */
	lwm2m_rd_client_start(&client, ep_name, rd_client_event);
	LOG_INF("setup complete.");
}

static void event_iface_up(struct net_mgmt_event_callback *cb,
		u32_t mgmt_event, struct net_if *iface)
{
	k_work_submit_to_queue(net_event_work_q, &net_event_work);
}

int lwm2m_init(struct k_work_q *work_q)
{
	struct net_if *iface;

	k_work_init(&net_event_work, lwm2m_start);
	net_event_work_q = work_q;

	iface = net_if_get_default();
	if (!iface) {
		LOG_ERR("Cannot find default network interface!");
		_TC_END_RESULT(TC_FAIL, "lwm2m_setup");
		TC_END_REPORT(TC_FAIL);
		return -ENETDOWN;
	}

	/* Subscribe to NET_EVENT_IF_UP if interface is not ready */
	if (!net_if_is_up(iface)) {
		net_mgmt_init_event_callback(&cb, event_iface_up,
				NET_EVENT_IF_UP);
		net_mgmt_add_event_callback(&cb);
	} else {
		event_iface_up(NULL, NET_EVENT_IF_UP, iface);
	}

	return 0;
}
