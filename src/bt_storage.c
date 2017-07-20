/*
 * Copyright (c) 2016-2017 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define SYS_LOG_DOMAIN "fota/bt_storage"
#define SYS_LOG_LEVEL SYS_LOG_LEVEL_DEBUG
#include <logging/sys_log.h>

#include <zephyr/types.h>
#include <stddef.h>
#include <errno.h>
#include <zephyr.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/storage.h>

#include <soc.h>

#include "product_id.h"

/* Any by default, can change depending on the hardware implementation */
static bt_addr_le_t bt_addr;

static ssize_t storage_read(const bt_addr_le_t *addr, u16_t key, void *data,
			       size_t length)
{
	if (addr) {
		return -ENOENT;
	}

	if (key == BT_STORAGE_ID_ADDR && length == sizeof(bt_addr)) {
		bt_addr_le_copy(data, &bt_addr);
		return sizeof(bt_addr);
	}

	return -EIO;
}

static ssize_t storage_write(const bt_addr_le_t *addr, u16_t key,
				const void *data, size_t length)
{
	return -ENOSYS;
}

static ssize_t storage_clear(const bt_addr_le_t *addr)
{
	return -ENOSYS;
}

static void set_own_bt_addr(void)
{
	int i;
	u8_t tmp;

	/*
	 * Generate a static BT addr using the unique product number.
	 */
	for (i = 0; i < 4; i++) {
		tmp = (product_id_get()->number >> i * 8) & 0xff;
		bt_addr.a.val[i] = tmp;
	}

	bt_addr.a.val[4] = 0xe7;
	bt_addr.a.val[5] = 0xd6;
}

int bt_storage_init(void)
{
	static const struct bt_storage storage = {
		.read = storage_read,
		.write = storage_write,
		.clear = storage_clear,
	};

	bt_addr.type = BT_ADDR_LE_RANDOM;

	set_own_bt_addr();

	bt_storage_register(&storage);

	SYS_LOG_DBG("Bluetooth storage driver registered");

	return 0;
}
