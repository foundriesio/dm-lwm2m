/*
 * Copyright (c) 2017 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Much of the initial code here was pulled from
 * samples/net/mqtt_publisher
 */

#define SYS_LOG_DOMAIN "fota/bluemix"
#define SYS_LOG_LEVEL CONFIG_SYS_LOG_FOTA_LEVEL
#include <logging/sys_log.h>

#include <zephyr.h>

#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <misc/stack.h>
#include <net/net_context.h>
#include <net/net_event.h>
#include <net/net_mgmt.h>

#include "product_id.h"
#include "tcp.h"
#include "bluemix.h"

#define BLUEMIX_USERNAME	"use-token-auth"
#define APP_CONNECT_TRIES	10
#define APP_SLEEP_MSECS	K_MSEC(500)
#define APP_TX_RX_TIMEOUT	K_MSEC(300)
#define MQTT_DISCONNECT_WAIT	K_MSEC(1000)

#define BLUEMIX_STACK_SIZE 1024
static K_THREAD_STACK_DEFINE(bluemix_thread_stack, BLUEMIX_STACK_SIZE);
static struct k_thread bluemix_thread_data;

int bluemix_sleep = K_SECONDS(3);

static bool connection_ready;
#if defined(CONFIG_NET_MGMT_EVENT)
static struct net_mgmt_event_callback cb;
#endif

static inline struct bluemix_ctx* mqtt_to_bluemix(struct mqtt_ctx *mqtt)
{
	return CONTAINER_OF(mqtt, struct bluemix_ctx, mqtt_ctx);
}

static inline int wait_for_mqtt(struct bluemix_ctx *ctx, s32_t timeout)
{
	return k_sem_take(&ctx->wait_sem, timeout);
}

/*
 * MQTT helpers
 */

static void connect_cb(struct mqtt_ctx *ctx)
{
	SYS_LOG_DBG("MQTT connected");
	k_sem_give(&mqtt_to_bluemix(ctx)->wait_sem);
}

static void disconnect_cb(struct mqtt_ctx *ctx)
{
	SYS_LOG_DBG("MQTT disconnected");
	k_sem_give(&mqtt_to_bluemix(ctx)->wait_sem);
}

static void malformed_cb(struct mqtt_ctx *ctx, u16_t pkt_type)
{
	SYS_LOG_DBG("MQTT malformed CB");
}

/*
 * Try to connect to the MQTT broker. The Bluemix context must have
 * properly initialized mqtt_ctx and connect_msg fields.
 */
static int try_to_connect(struct bluemix_ctx *ctx)
{
	struct mqtt_ctx *mqtt = &ctx->mqtt_ctx;
	struct mqtt_connect_msg *msg = &ctx->connect_msg;
	int i = 0;
	int ret;

	for (i = 0; i < APP_CONNECT_TRIES; i++) {
		ret = mqtt_tx_connect(mqtt, msg);
		if (ret) {
			SYS_LOG_ERR("mqtt_tx_connect: %d", ret);
			continue;
		}
		ret = wait_for_mqtt(ctx, APP_SLEEP_MSECS);
		if (mqtt->connected) {
			return 0;
		}
	}

	return -ETIMEDOUT;
}

/*
 * Bluemix
 */

static int publish_message(struct bluemix_ctx *ctx)
{
	SYS_LOG_DBG("topic: %s", ctx->pub_msg.topic);
	SYS_LOG_DBG("message: %s", ctx->pub_msg.msg);
	return mqtt_tx_publish(&ctx->mqtt_ctx, &ctx->pub_msg);
}

static int bluemix_start(struct bluemix_ctx *ctx)
{
	int ret = 0;

	/* Set everything to 0 before assigning the required fields. */
	memset(ctx, 0x00, sizeof(*ctx));

	/*
	 * Initialize the IDs etc. before doing anything else.
	 */
	snprintf(ctx->bm_id, sizeof(ctx->bm_id), "%s-%08x",
		 CONFIG_FOTA_BLUEMIX_DEVICE_TYPE, product_id_get()->number);
	snprintf(ctx->client_id, sizeof(ctx->client_id),
		"d:%s:%s:%s", CONFIG_FOTA_BLUEMIX_ORG,
		CONFIG_FOTA_BLUEMIX_DEVICE_TYPE,
		ctx->bm_id);
	snprintf(ctx->bm_auth_token, sizeof(ctx->bm_auth_token),
		 "%08x", product_id_get()->number);

	k_sem_init(&ctx->wait_sem, 0, 1);

	/*
	 * try connecting here so that tcp_get_net_context()
	 * will return a valid net_ctx later
	 */
	ret = tcp_connect(TCP_CTX_BLUEMIX);
	if (ret < 0) {
		return ret;
	}

	ctx->mqtt_ctx.connect = connect_cb;
	ctx->mqtt_ctx.disconnect = disconnect_cb;
	ctx->mqtt_ctx.malformed = malformed_cb;
	ctx->mqtt_ctx.net_timeout = APP_TX_RX_TIMEOUT;
	ctx->mqtt_ctx.net_ctx = tcp_get_net_context(TCP_CTX_BLUEMIX);

	ret = mqtt_init(&ctx->mqtt_ctx, MQTT_APP_PUBLISHER);
	if (ret) {
		goto out;
	}

	/* The connect message will be sent to the MQTT server (broker).
	 * If clean_session here is 0, the mqtt_ctx clean_session variable
	 * will be set to 0 also. Please don't do that, set always to 1.
	 * Clean session = 0 is not yet supported.
	 */
	ctx->connect_msg.client_id = ctx->client_id;
	ctx->connect_msg.client_id_len = strlen(ctx->connect_msg.client_id);
	ctx->connect_msg.keep_alive = 0;
	ctx->connect_msg.user_name = BLUEMIX_USERNAME;
	ctx->connect_msg.user_name_len = strlen(ctx->connect_msg.user_name);
	ctx->connect_msg.password = ctx->bm_auth_token;
	ctx->connect_msg.password_len = strlen(ctx->connect_msg.password);
	ctx->connect_msg.clean_session = 1;

	ret = try_to_connect(ctx);
	if (ret) {
		goto out;
	}

	return 0;
 out:
	tcp_cleanup(TCP_CTX_BLUEMIX, true);
	return ret;
}

static int bluemix_fini(struct bluemix_ctx *ctx)
{
	int ret;

	ret = mqtt_tx_disconnect(&ctx->mqtt_ctx);
	if (ret) {
		SYS_LOG_ERR("%s: mqtt_tx_disconnect: %d", __func__, ret);
		goto cleanup;
	}
	ret = wait_for_mqtt(ctx, MQTT_DISCONNECT_WAIT);
 cleanup:
	tcp_cleanup(TCP_CTX_BLUEMIX, true);
	return ret;
}

int bluemix_pub_status_json(struct bluemix_ctx *ctx,
			    const char *fmt, ...)
{
	struct mqtt_publish_msg *pub_msg = &ctx->pub_msg;
	va_list vargs;
	int ret;

	snprintf(ctx->bm_topic, sizeof(ctx->bm_topic),
		 "iot-2/type/%s/id/%s/evt/status/fmt/json",
		 CONFIG_FOTA_BLUEMIX_DEVICE_TYPE, ctx->bm_id);

	/* Fill in the initial '{"d":'. */
	ret = snprintf(ctx->bm_message, sizeof(ctx->bm_message), "{\"d\":");
	if (ret == sizeof(ctx->bm_message) - 1) {
		return -ENOMEM;
	}
	/* Add the user data. */
	va_start(vargs, fmt);
	ret += vsnprintf(ctx->bm_message + ret, sizeof(ctx->bm_message) - ret,
			 fmt, vargs);
	va_end(vargs);
	if (ret > sizeof(ctx->bm_message) - 2) {
		/* Overflow check: 2 = (1 for '\0') + (1 for "}") */
		return -ENOMEM;
	}
	/* Append the closing brace. */
	snprintf(ctx->bm_message + ret, sizeof(ctx->bm_message) - ret, "}");

	/* Fill out the MQTT publication, and ship it.
	 *
	 * IMPORTANT: don't increase the level of QoS here, even if
	 *            Zephyr claims to support it, until the Zephyr
	 *            MQTT stack can correctly parse multiple MQTT
	 *            packets within a single struct net_pkt.
	 *
	 * Working around that issue implies never receiving multiple
	 * MQTT packets in the same net_pkt.
	 *
	 * Keep this at QoS 0 to avoid receiving PUBACK or PUBREC in
	 * response to this message. Since this app is a publisher
	 * only, the remaining possible incoming messages are CONNACK
	 * and PINGRESP (depending on nonzero keep_alive). Those will
	 * never be transmitted at the same time, as we ought to wait
	 * for CONNACK before sending any PINGREQs.
	 */
	pub_msg->msg = ctx->bm_message;
	pub_msg->msg_len = strlen(pub_msg->msg);
	pub_msg->qos = MQTT_QoS0;
	pub_msg->topic = ctx->bm_topic;
	pub_msg->topic_len = strlen(pub_msg->topic);
	return publish_message(ctx);
}

static void bluemix_service(void *bm_cbv, void *bm_cb_data, void *p3)
{
	static struct bluemix_ctx bluemix_context;
	bluemix_cb bm_cb = bm_cbv;
	static int bluemix_inited;
	int ret;

	ARG_UNUSED(p3);

	while (true) {
		k_sleep(bluemix_sleep);

		if (!connection_ready) {
			SYS_LOG_DBG("Network interface is not ready");
			continue;
		}

		tcp_interface_lock();

		if (!bluemix_inited) {
			ret = bluemix_start(&bluemix_context);
			if (ret) {
				SYS_LOG_ERR("connection failed: %d", ret);
				ret = bm_cb(&bluemix_context,
					    BLUEMIX_EVT_CONN_FAIL,
					    bm_cb_data);
				switch (ret) {
				case BLUEMIX_CB_OK:
				case BLUEMIX_CB_RECONNECT:
					tcp_interface_unlock();
					continue;
				default:
					goto out_unlock;
				}
			} else {
				bluemix_inited = 1;
			}
		}

		ret = bm_cb(&bluemix_context, BLUEMIX_EVT_POLL, bm_cb_data);
		switch (ret) {
		case BLUEMIX_CB_OK:
			break;
		case BLUEMIX_CB_RECONNECT:
			ret = bluemix_fini(&bluemix_context);
			if (ret) {
				SYS_LOG_ERR("bluemix_fini: %d", ret);
			}
			bluemix_inited = 0;
			break;
		case BLUEMIX_CB_HALT:
			goto out_close;
		default:
			SYS_LOG_ERR("callback returned %d", ret);
			goto out_close;
		}

		tcp_interface_unlock();

		stack_analyze("Bluemix Thread", bluemix_thread_stack,
			      K_THREAD_STACK_SIZEOF(bluemix_thread_stack));
	}

 out_close:
	ret = bluemix_fini(&bluemix_context);
	(void)ret;		/* Ignore the return value. */
 out_unlock:
	tcp_interface_unlock();
	return;
}

static void event_iface_up(struct net_mgmt_event_callback *cb,
			   u32_t mgmt_event, struct net_if *iface)
{
	connection_ready = true;
}

int bluemix_init(bluemix_cb bm_cb, void *bm_cb_data)
{
	struct net_if *iface = net_if_get_default();

	k_thread_create(&bluemix_thread_data, &bluemix_thread_stack[0],
			K_THREAD_STACK_SIZEOF(bluemix_thread_stack),
			(k_thread_entry_t) bluemix_service,
			bm_cb, bm_cb_data, NULL, K_PRIO_COOP(7), 0, K_NO_WAIT);

#if defined(CONFIG_NET_MGMT_EVENT)
	/* Subscribe to NET_IF_UP if interface is not ready */
	if (!atomic_test_bit(iface->flags, NET_IF_UP)) {
		net_mgmt_init_event_callback(&cb, event_iface_up,
					     NET_EVENT_IF_UP);
		net_mgmt_add_event_callback(&cb);
		return 0;
	}
#endif

	event_iface_up(NULL, NET_EVENT_IF_UP, iface);
	return 0;
}
