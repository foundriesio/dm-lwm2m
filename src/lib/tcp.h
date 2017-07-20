/*
 * Copyright (c) 2016 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __FOTA_TCP_H__
#define __FOTA_TCP_H__

/*
 * TCP Buffer Logic:
 * HTTP Header (17) + 3x MTU Packets 640 = 1937
 */
#define TCP_RECV_BUF_SIZE 2048

enum tcp_context_id {
#if defined(CONFIG_FOTA_DM_BACKEND_HAWKBIT)
	TCP_CTX_HAWKBIT,
#endif
	TCP_CTX_BLUEMIX,
	TCP_CTX_MAX
};

int tcp_init(void);

/* Currently, tcp contexts must lock the network interface before
 * using it. */
void tcp_interface_lock(void);
void tcp_interface_unlock(void);

/* These must be called/used with the TCP interface lock held. */
void tcp_cleanup(enum tcp_context_id id, bool put_net_context);
int tcp_connect(enum tcp_context_id id);
int tcp_send(enum tcp_context_id id, const unsigned char *buf, size_t size);
int tcp_recv(enum tcp_context_id id, unsigned char *buf, size_t size,
	     s32_t timeout);
struct net_context *tcp_get_net_context(enum tcp_context_id id);
struct k_sem *tcp_get_recv_wait_sem(enum tcp_context_id id);

#endif	/* __FOTA_TCP_H__ */
