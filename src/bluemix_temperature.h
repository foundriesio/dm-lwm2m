/*
 * Copyright (c) 2017 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Example Bluemix client, which periodically publishes
 *        temperature data to the cloud.
 *
 * Temperature data can come from up to two sources:
 *
 * 1. One on-chip temperature sensor, named "fota-mcu-temp"
 * 2. One off-chip temperature sensor, named "fota-offchip-temp"
 *
 * The target configuration can ensure that a Zephyr temperature
 * sensor device has one of the given names, then configure the
 * Bluemix Kconfig parameters, to use this file. Examples are in the
 * board-level files in boards/.
 */

#ifndef __FOTA_BLUEMIX_TEMPERATURE_H__
#define __FOTA_BLUEMIX_TEMPERATURE_H__

/**
 * @brief Start the background Bluemix thread
 *
 * This thread will periodically attempt to publish temperature data
 * to an IBM Bluemix MQTT broker.
 *
 * @return 0 if the thread is started successfully, and a negative
 *         errno on error.
 */
int bluemix_temperature_start(void);

#endif	/* __FOTA_BLUEMIX_TEMPERATURE_H__ */
