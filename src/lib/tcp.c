/*
 * Copyright (c) 2016 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define SYS_LOG_DOMAIN "fota/tcp"
#define SYS_LOG_LEVEL CONFIG_SYS_LOG_FOTA_LEVEL
#include <logging/sys_log.h>

#include <zephyr/types.h>
#include <stddef.h>
#include <errno.h>
#include <zephyr.h>
#include <toolchain.h>

#include <net/net_core.h>
#include <net/net_context.h>
#include <net/net_pkt.h>
#include <net/net_if.h>

#include "tcp.h"
#include "hawkbit.h"
#include "bluemix.h"

#define SERVER_CONNECT_TIMEOUT		K_SECONDS(5)
#define SERVER_CONNECT_MAX_WAIT_COUNT	2
#define TCP_TX_TIMEOUT			K_MSEC(500)

/*
 * If we're depending on net samples IP address config options, we
 * need to make sure they are set. That's the purpose of these
 * build-time asserts that their lengths are longer than 1 (a null
 * character, the default Kconfig string value).
 *
 * Users can specify these values for their local environment using
 * local.conf or boards/$(BOARD)-local.conf, which aren't tracked in
 * Git.
 */
#if defined(CONFIG_NET_IPV4)
#if !defined(CONFIG_NET_DHCPV4)
BUILD_ASSERT_MSG(sizeof(CONFIG_NET_APP_MY_IPV4_ADDR) > 1,
		 "DHCPv4 must be enabled, or CONFIG_NET_APP_MY_IPV4_ADDR must be defined, in boards/$(BOARD)-local.conf");
#endif
BUILD_ASSERT_MSG(sizeof(CONFIG_NET_APP_PEER_IPV4_ADDR) > 1,
		 "CONFIG_NET_APP_PEER_IPV4_ADDR must be defined in boards/$(BOARD)-local.conf");
#endif

/* Network Config */
#if defined(CONFIG_NET_IPV6)
#define FOTA_AF_INET		AF_INET6
#define NET_SIN_FAMILY(s)	net_sin6(s)->sin6_family
#define NET_SIN_ADDR(s)		net_sin6(s)->sin6_addr
#define NET_SIN_PORT(s)		net_sin6(s)->sin6_port
#define NET_SIN_SIZE		sizeof(struct sockaddr_in6)
#define LOCAL_IPADDR		"::"
#ifdef CONFIG_NET_APP_PEER_IPV6_ADDR
#define PEER_IPADDR		CONFIG_NET_APP_PEER_IPV6_ADDR
#else
#define PEER_IPADDR		"fe80::d4e7:0:0:1" /* tinyproxy gateway */
#endif
#elif defined(CONFIG_NET_IPV4)
#define FOTA_AF_INET		AF_INET
#define NET_SIN_FAMILY(s)	net_sin(s)->sin_family
#define NET_SIN_ADDR(s)		net_sin(s)->sin_addr
#define NET_SIN_PORT(s)		net_sin(s)->sin_port
#define NET_SIN_SIZE		sizeof(struct sockaddr_in)
#define LOCAL_IPADDR		CONFIG_NET_APP_MY_IPV4_ADDR
#define PEER_IPADDR		CONFIG_NET_APP_PEER_IPV4_ADDR
#endif

#if defined(CONFIG_NET_CONTEXT_NET_PKT_POOL)
NET_PKT_TX_SLAB_DEFINE(fota_tx_tcp, 15);
NET_PKT_DATA_POOL_DEFINE(fota_data_tcp, 30);

static struct k_mem_slab *tx_tcp_slab(void)
{
	return &fota_tx_tcp;
}

static struct net_buf_pool *data_tcp_pool(void)
{
	return &fota_data_tcp;
}
#endif

/* Global address to be set from RA */
static struct sockaddr client_addr;

struct tcp_context {
	struct net_context *net_ctx;
	struct k_sem sem_recv_wait;
	struct k_sem sem_recv_mutex;
	u8_t read_buf[TCP_RECV_BUF_SIZE];
	u16_t read_bytes;
	u16_t peer_port;
};

static struct tcp_context contexts[TCP_CTX_MAX];
static struct k_sem interface_lock;

static inline int invalid_id(enum tcp_context_id id)
{
	return id < 0 || id >= TCP_CTX_MAX;
}

static void tcp_cleanup_context(struct tcp_context *ctx, bool put_net_context)
{
	if (!ctx) {
		SYS_LOG_ERR("NULL tcp_context!");
		return;
	}

	if (put_net_context && ctx->net_ctx) {
		net_context_put(ctx->net_ctx);
	}

	ctx->net_ctx = NULL;
}

void tcp_cleanup(enum tcp_context_id id, bool put_net_context)
{
	if (invalid_id(id)) {
		return;
	}
	tcp_cleanup_context(&contexts[id], put_net_context);
}

static void tcp_received_cb(struct net_context *context,
			    struct net_pkt *pkt, int status, void *user_data)
{
	ARG_UNUSED(context);
	struct tcp_context *ctx = user_data;
	u16_t pos = 0;

	/* handle FIN packet */
	if (!pkt) {
		SYS_LOG_DBG("FIN received, closing network context");
		/* clear out our reference to the network connection */
		tcp_cleanup_context(ctx, false);
		k_sem_give(&ctx->sem_recv_wait);
		return;
	} else {
		k_sem_take(&ctx->sem_recv_mutex, K_FOREVER);

		/*
		 * TODO: if overflow, return an error and save
		 * the net_pkt for later processing
		 */
		if (net_pkt_appdatalen(pkt) >=
				TCP_RECV_BUF_SIZE - ctx->read_bytes) {
			SYS_LOG_ERR("ERROR buffer overflow!"
				    " (read(%u)+bufflen(%u) >= %u)",
				    ctx->read_bytes, net_pkt_appdatalen(pkt),
				    TCP_RECV_BUF_SIZE);
			net_pkt_unref(pkt);
			tcp_cleanup_context(ctx, true);
			k_sem_give(&ctx->sem_recv_wait);
			return;
		}

		pos = net_pkt_appdata(pkt) - net_pkt_ip_data(pkt);
		if (!net_frag_read(pkt->frags, pos, &pos,
				   net_pkt_appdatalen(pkt),
				   ctx->read_buf + ctx->read_bytes))
			ctx->read_bytes += net_pkt_appdatalen(pkt);

		ctx->read_buf[ctx->read_bytes] = 0;
		net_pkt_unref(pkt);
		k_sem_give(&ctx->sem_recv_mutex);
	}
}

int tcp_init(void)
{
	struct net_if *iface;
	int i;

	iface = net_if_get_default();
	if (!iface) {
		SYS_LOG_ERR("Cannot find default network interface!");
		return -ENETDOWN;
	}

#if defined(CONFIG_NET_IPV6)
	net_addr_pton(FOTA_AF_INET, LOCAL_IPADDR,
		      (struct sockaddr *)&NET_SIN_ADDR(&client_addr));
	net_if_ipv6_addr_add(iface,
			     &NET_SIN_ADDR(&client_addr),
			     NET_ADDR_MANUAL, 0);
/*
 * For IPv6 via ethernet, Zephyr does not support an autoconfiguration
 * method such as DHCPv6.  Use IPv4 until it's implemented if this is
 * required.
 */
#elif defined(CONFIG_NET_IPV4)
#if defined(CONFIG_NET_DHCPV4)
	net_dhcpv4_start(iface);

	/* Add delays so DHCP can assign IP */
	/* TODO: add a timeout/retry */
	SYS_LOG_INF("Waiting for DHCP");
	do {
		k_sleep(K_SECONDS(1));
	} while (net_is_ipv4_addr_unspecified(&iface->dhcpv4.requested_ip));
	SYS_LOG_INF("Done!");

	/* TODO: add a timeout */
	SYS_LOG_INF("Waiting for IP assignment");
	do {
		k_sleep(K_SECONDS(1));
	} while (!net_is_my_ipv4_addr(&iface->dhcpv4.requested_ip));
	SYS_LOG_INF("Done!");

	net_ipaddr_copy(&NET_SIN_ADDR(&client_addr),
			&iface->dhcpv4.requested_ip);
#else
	net_addr_pton(FOTA_AF_INET, LOCAL_IPADDR,
		      (struct sockaddr *)&NET_SIN_ADDR(&client_addr));
	net_if_ipv4_addr_add(iface,
			     &NET_SIN_ADDR(&client_addr),
			     NET_ADDR_MANUAL, 0);
#endif
#endif

	memset(contexts, 0x00, sizeof(contexts));
	for (i = 0; i < TCP_CTX_MAX; i++) {
		k_sem_init(&contexts[i].sem_recv_wait, 0, 1);
		k_sem_init(&contexts[i].sem_recv_mutex, 1, 1);
		contexts[i].read_bytes = 0;
	}
#if defined(CONFIG_FOTA_DM_BACKEND_HAWKBIT)
	contexts[TCP_CTX_HAWKBIT].peer_port = HAWKBIT_PORT;
#endif
	contexts[TCP_CTX_BLUEMIX].peer_port = BLUEMIX_PORT;

	k_sem_init(&interface_lock, 1, 1);

	return 0;
}

void tcp_interface_lock(void)
{
	k_sem_take(&interface_lock, K_FOREVER);
}

void tcp_interface_unlock()
{
	k_sem_give(&interface_lock);
}

static int tcp_connect_context(struct tcp_context *ctx)
{
	struct sockaddr my_addr;
	struct sockaddr dst_addr;
	int rc;

	/* make sure we have a network context */
	if (!ctx->net_ctx) {
		rc = net_context_get(FOTA_AF_INET, SOCK_STREAM,
				     IPPROTO_TCP, &ctx->net_ctx);
		if (rc < 0) {
			SYS_LOG_ERR("Cannot get network context for TCP (%d)",
				    rc);
			tcp_cleanup_context(ctx, true);
			return -EIO;
		}

		net_ipaddr_copy(&NET_SIN_ADDR(&my_addr),
				&NET_SIN_ADDR(&client_addr));
		NET_SIN_FAMILY(&my_addr) = FOTA_AF_INET;
		NET_SIN_PORT(&my_addr) = 0;

/*
 * This configuration is used for 6lowpan TCP pools which require
 * extra save/restore buffers to be allocated during compress/uncompress
 * operations.
 */
#if defined(CONFIG_NET_CONTEXT_NET_PKT_POOL)
		net_context_setup_pools(ctx->net_ctx, tx_tcp_slab,
					data_tcp_pool);
#endif

		rc = net_context_bind(ctx->net_ctx, &my_addr, NET_SIN_SIZE);
		if (rc < 0) {
			SYS_LOG_ERR("Cannot bind IP addr (%d)", rc);
			tcp_cleanup_context(ctx, true);
			return -EINVAL;
		}
	}

	if (!ctx->net_ctx) {
		SYS_LOG_ERR("ERROR: No TCP network context!");
		return -EIO;
	}

	/* if we're already connected return */
	if (net_context_get_state(ctx->net_ctx) == NET_CONTEXT_CONNECTED) {
		return 0;
	}

	net_addr_pton(FOTA_AF_INET, PEER_IPADDR,
		      (struct sockaddr *)&NET_SIN_ADDR(&dst_addr));
	NET_SIN_FAMILY(&dst_addr) = FOTA_AF_INET;
	NET_SIN_PORT(&dst_addr) = htons(ctx->peer_port);

	/* triggering the connection starts the callback sequence */
	rc = net_context_connect(ctx->net_ctx, &dst_addr, NET_SIN_SIZE,
				 NULL, SERVER_CONNECT_TIMEOUT, NULL);
	if (rc < 0) {
		__unused char buf[NET_IPV6_ADDR_LEN];

		SYS_LOG_ERR("Cannot connect to server: %s:%d (%d)",
			    net_addr_ntop(FOTA_AF_INET,
					  &NET_SIN_ADDR(&dst_addr),
					  buf, sizeof(buf)),
			    ctx->peer_port, rc);
		tcp_cleanup_context(ctx, true);
		return -EIO;
	}
	return 0;
}

int tcp_connect(enum tcp_context_id id)
{
	if (invalid_id(id)) {
		return -EINVAL;
	}
	return tcp_connect_context(&contexts[id]);
}

static int tcp_send_context(struct tcp_context *ctx, const unsigned char *buf,
			    size_t size)
{
	struct net_pkt *send_pkt;
	int rc, len;

	/* make sure we're connected */
	rc = tcp_connect_context(ctx);
	if (rc < 0)
		return rc;

	send_pkt = net_pkt_get_tx(ctx->net_ctx, K_FOREVER);
	if (!send_pkt) {
		SYS_LOG_ERR("cannot create net_pkt");
		return -EIO;
	}

	rc = net_pkt_append(send_pkt, size, (u8_t *) buf, K_FOREVER);
	if (!rc) {
		SYS_LOG_ERR("cannot write buf");
		net_pkt_unref(send_pkt);
		return -EIO;
	}

	len = net_pkt_get_len(send_pkt);

	rc = net_context_send(send_pkt, NULL, TCP_TX_TIMEOUT, NULL, NULL);
	if (rc < 0) {
		SYS_LOG_ERR("Cannot send data to peer (%d)", rc);
		net_pkt_unref(send_pkt);

		if (rc == -ESHUTDOWN)
			tcp_cleanup_context(ctx, true);

		return -EIO;
	} else {
		return len;
	}
}

int tcp_send(enum tcp_context_id id, const unsigned char *buf, size_t size)
{
	if (invalid_id(id)) {
		return -EINVAL;
	}
	return tcp_send_context(&contexts[id], buf, size);
}

static int tcp_recv_context(struct tcp_context *ctx, unsigned char *buf,
			    size_t size, s32_t timeout)
{
	int rc;

	/* make sure we're connected */
	rc = tcp_connect_context(ctx);
	if (rc < 0)
		return rc;

	net_context_recv(ctx->net_ctx, tcp_received_cb, K_NO_WAIT, ctx);
	/* wait here for the connection to complete or timeout */
	rc = k_sem_take(&ctx->sem_recv_wait, timeout);
	if (rc < 0 && rc != -ETIMEDOUT) {
		SYS_LOG_ERR("recv_wait sem error = %d", rc);
		return rc;
	}

	/* take a mutex here so we don't process any more data */
	k_sem_take(&ctx->sem_recv_mutex, K_FOREVER);

	/* copy the receive buffer into the passed in buffer */
	if (ctx->read_bytes > 0) {
		if (ctx->read_bytes > size)
			ctx->read_bytes = size;
		memcpy(buf, ctx->read_buf, ctx->read_bytes);
	}
	buf[ctx->read_bytes] = 0;
	rc = ctx->read_bytes;
	ctx->read_bytes = 0;

	k_sem_give(&ctx->sem_recv_mutex);

	return rc;
}

int tcp_recv(enum tcp_context_id id, unsigned char *buf, size_t size,
	      s32_t timeout)
{
	if (invalid_id(id)) {
		return -EINVAL;
	}
	return tcp_recv_context(&contexts[id], buf, size, timeout);
}

struct net_context *tcp_get_net_context(enum tcp_context_id id)
{
	if (invalid_id(id)) {
		return NULL;
	}
	return contexts[id].net_ctx;
}

struct k_sem *tcp_get_recv_wait_sem(enum tcp_context_id id)
{
	if (invalid_id(id)) {
		return NULL;
	}
	return &contexts[id].sem_recv_wait;
}
