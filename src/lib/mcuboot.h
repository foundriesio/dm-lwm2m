/*
 * Copyright (c) 2016 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __FOTA_MCUBOOT_H__
#define __FOTA_MCUBOOT_H__

/*
 * The image areas must have equal size. This define refers to it.
 */
#define FLASH_BANK_SIZE	FLASH_AREA_IMAGE_0_SIZE

typedef enum {
	BOOT_STATUS_DONE    = 0x01,
	BOOT_STATUS_ONGOING = 0xff,
} boot_status_t;

extern struct device *flash_dev;

boot_status_t boot_status_read(void);
void boot_status_update(void);
void boot_trigger_ota(void);

int boot_erase_flash_bank(u32_t bank_offset);

#endif	/* __FOTA_MCUBOOT_H__ */
