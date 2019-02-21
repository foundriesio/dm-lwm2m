/*
 * Copyright (c) 2019 Foundries.io
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME fota_storage
#define LOG_LEVEL CONFIG_FOTA_LOG_LEVEL

#include <logging/log.h>
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

#include <zephyr.h>
#include <settings/settings.h>

#include "settings.h"

static struct update_counter uc;

int fota_update_counter_read(struct update_counter *update_counter)
{
	memcpy(update_counter, &uc, sizeof(uc));
	return 0;
}

int fota_update_counter_update(update_counter_t type, u32_t new_value)
{
	if (type == COUNTER_UPDATE) {
		uc.update = new_value;
	} else {
		uc.current = new_value;
	}

	return settings_save_one("fota/counter", &uc, sizeof(uc));
}

static int set(int argc, char **argv, void *val_ctx)
{
	int len;

	if (argc != 1) {
		return -ENOENT;
	}

	if (!strcmp(argv[0], "counter")) {
		len = settings_val_read_cb(val_ctx, &uc, sizeof(uc));
		if (len < sizeof(uc)) {
			LOG_ERR("Unable to read update counter.  Resetting.");
			memset(&uc, 0, sizeof(uc));
		}

		return 0;
	}

	return -ENOENT;
}

static struct settings_handler fota_settings = {
	.name = "fota",
	.h_set = set,
};

int fota_settings_init(void)
{
	int err;

	err = settings_subsys_init();
	if (err) {
		LOG_ERR("settings_subsys_init failed (err %d)", err);
		return err;
	}

	err = settings_register(&fota_settings);
	if (err) {
		LOG_ERR("settings_register failed (err %d)", err);
		return err;
	}

	return 0;
}
