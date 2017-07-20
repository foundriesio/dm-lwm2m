/*
 * Copyright (c) 2017 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _FOTA_FLASH_BLOCK_H_
#define _FOTA_FLASH_BLOCK_H_

#ifdef __cplusplus
extern "C" {
#endif

/*
 * TODO:
 * create flash_block lifecycle struct which contains:
 * flash buffer
 * verify buffer
 * tracking indexes
 * status
 *
 * add flash_block_init()
 * add flash_block_destroy()
 */

/**
 *  @brief  Process input buffers to be written to flash memory
 *  in single blocks.  Will store remainder between calls.
 *
 *  A final call to this function with finished set to true
 *  will write out the remaining block buffer to flash.
 *
 *  @param  dev             : flash device
 *  @param  offset          : starting offset for the write
 *  @param  bytes_written   : actual count of bytes written to flash
 *  @param  data            : data to write
 *  @param  len             : Number of bytes to write
 *  @param  finished        : when true this forces any buffered
 *  data to be written to flash
 *
 *  @return  0 on success, negative errno code on fail.
 */
int flash_block_write(struct device *dev,
		      off_t offset, int *bytes_written,
		      u8_t *data, int len,
		      bool finished);

#ifdef __cplusplus
}
#endif

#endif /* _FOTA_FLASH_BLOCK_H_ */
