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

#define ENDPOINT_LEN		33
static char ep_name[ENDPOINT_LEN];

static struct net_app_ctx app_ctx;
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

static int device_reboot_cb(u16_t obj_inst_id)
{
	SYS_LOG_INF("DEVICE: REBOOT");
	sys_reboot(0);
	return 1;
}

static int firmware_update_cb(u16_t obj_inst_id)
{
	SYS_LOG_DBG("UPDATE");
	boot_trigger_ota();
	sys_reboot(0);
	return 1;
}

static int firmware_block_received_cb(u16_t obj_inst_id,
				      u8_t *data, u16_t data_len,
				      bool last_block, size_t total_size)
{
	static int bytes_written;
	static u8_t percent_downloaded;
	u8_t downloaded;
	int ret = 0;

	/* first time in callback */
	if (bytes_written == 0) {
		/* update firmware state */
		lwm2m_engine_set_u8("5/0/3", STATE_DOWNLOADING);
	}

	if (data_len > 0) {
		ret = flash_block_write(flash_dev, FLASH_AREA_IMAGE_1_OFFSET,
					&bytes_written, data, data_len,
					last_block);
		if (ret < 0) {
			return ret;
		}
	}

	/* display a % downloaded, if it's different */
	downloaded = bytes_written * 100 / total_size;
	if (downloaded > percent_downloaded) {
		percent_downloaded = downloaded;
		SYS_LOG_INF("%d%%", percent_downloaded);
	}

	/* reset out state vars on the last block */
	if (last_block) {
		/* update firmware state */
		lwm2m_engine_set_u8("5/0/3", STATE_DOWNLOADED);
		bytes_written = 0;
		percent_downloaded = 0;
	}

	return 1;
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
	lwm2m_engine_create_obj_inst("3303/0");

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

int lwm2m_init(void)
{
	struct net_if *iface;
	int ret;

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
