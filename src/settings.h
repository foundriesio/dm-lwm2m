/*
 * Copyright (c) 2019 Foundries.io
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FOTA_STORAGE_H__
#define FOTA_STORAGE_H__

struct update_counter {
	int current;
	int update;
};

typedef enum {
	COUNTER_CURRENT = 0,
	COUNTER_UPDATE,
} update_counter_t;

int fota_update_counter_read(struct update_counter *update_counter);
int fota_update_counter_update(update_counter_t type, u32_t new_value);
int fota_settings_init(void);

#endif	/* FOTA_STORAGE_H__ */
