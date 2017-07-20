/*
 * Copyright (c) 2016-2017 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __FOTA_BT_IPSS_H__
#define __FOTA_BT_IPSS_H__

/* these functions hidden in zephyr/subsys/bluetooth/host/hci_core.h */
extern struct net_buf *bt_hci_cmd_create(u16_t opcode, u8_t param_len);
extern int bt_hci_cmd_send_sync(u16_t opcode, struct net_buf *buf,
				struct net_buf **rsp);

int ipss_init(struct bt_conn_cb *conn_callbacks);
int ipss_set_connected(void);
int ipss_advertise(void);

#endif	/* __FOTA_BT_IPSS_H__ */
