/*
 * Copyright (c) 2017 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __FOTA_LWM2M_CREDENTIALS_H__
#define __FOTA_LWM2M_CREDENTIALS_H__

#include <device.h>

#define LWM2M_DEVICE_ID_SIZE (32 + 1)
#define LWM2M_DEVICE_TOKEN_SIZE (32 + 1)

/**
 * @brief Get a copy of this device's unique ID.
 *
 * The argument array must be large enough to contain a
 * null-terminated string of size LWM2M_DEVICE_ID_SIZE.
 *
 * @param flash Flash device containing the data.
 * @param device_id Pointer to character array to copy the data into.
 * @return flash_read() return code
 */
int lwm2m_get_device_id(struct device *flash,
			char device_id[LWM2M_DEVICE_ID_SIZE]);

/**
 * @brief Get a copy of this device's DTLS login token.
 *
 * The argument array must be large enough to contain a
 * null-terminated string of size LWM2M_DEVICE_TOKEN_SIZE.
 *
 * @param flash Flash device containing the data.
 * @param device_token Pointer to character array to copy the data
 *                     into.
 * @return flash_read() return code
 */
int lwm2m_get_device_token(struct device *flash,
			   char device_token[LWM2M_DEVICE_TOKEN_SIZE]);

#endif	/* __FOTA_LWM2M_CREDENTIALS_H__ */
