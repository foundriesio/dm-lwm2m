/*
 * Copyright (c) 2016 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __FOTA_HAWKBIT_H__
#define __FOTA_HAWKBIT_H__

#define HAWKBIT_HOST	"gitci.com:8080"
#define HAWKBIT_PORT	8080
#define HAWKBIT_JSON_URL "/DEFAULT/controller/v1"

extern int poll_sleep;

struct hawkbit_device_acid {
	u32_t current;
	u32_t update;
};

int hawkbit_init(void);
void hawkbit_device_acid_read(struct hawkbit_device_acid *device_acid);

#endif	/* __FOTA_HAWKBIT_H__ */
