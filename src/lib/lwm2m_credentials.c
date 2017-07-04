/*
 * Copyright (c) 2017 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lwm2m_credentials.h"

#include <flash.h>

#define LWM2M_CREDENTIALS_BASE FLASH_AREA_LWM2M_CREDENTIALS_OFFSET

/**
 * @brief On-flash representation of lwm2m credentials data.
 */
struct lwm2m_credentials_data {
	/** Device's unique ID in LWM2M */
	char device_id[LWM2M_DEVICE_ID_SIZE];
	/** Device's DTLS token in LWM2M */
	char device_token[LWM2M_DEVICE_TOKEN_SIZE];
};

int lwm2m_get_device_id(struct device *flash,
			char device_id[LWM2M_DEVICE_ID_SIZE])
{
	off_t offset = LWM2M_CREDENTIALS_BASE +
		offsetof(struct lwm2m_credentials_data, device_id);
	return flash_read(flash, offset, device_id, LWM2M_DEVICE_ID_SIZE);
}

int lwm2m_get_device_token(struct device *flash,
			   char device_token[LWM2M_DEVICE_TOKEN_SIZE])
{
	off_t offset = LWM2M_CREDENTIALS_BASE +
		offsetof(struct lwm2m_credentials_data, device_token);
	return flash_read(flash, offset, device_token, LWM2M_DEVICE_TOKEN_SIZE);
}
