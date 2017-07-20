/*
 * Copyright (c) 2016 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define SYS_LOG_DOMAIN "fota/hawkbit"
#define SYS_LOG_LEVEL CONFIG_SYS_LOG_FOTA_LEVEL
#include <logging/sys_log.h>

#include <zephyr/types.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <misc/byteorder.h>
#include <flash.h>
#include <zephyr.h>
#include <misc/reboot.h>
#include <misc/stack.h>
#include <net/net_pkt.h>
#include <net/net_event.h>
#include <net/net_mgmt.h>

#include <soc.h>
#include <net/http_parser.h>

#include "jsmn.h"
#include "hawkbit.h"
#include "mcuboot.h"
#include "flash_block.h"
#include "product_id.h"
#include "tcp.h"

#define HAWKBIT_MAX_SERVER_FAIL	5

/*
 * TODO:
 * create a transfer lifecylce structure
 * to contain the following vars:
 * TCP receive buffer
 * tracking indexes
 * status
 *
 */
u8_t tcp_buf[TCP_RECV_BUF_SIZE];

struct hawkbit_download {
	size_t http_header_size;
	size_t http_header_read;
	size_t http_content_size;
	int header_status;
	size_t downloaded_size;
	size_t download_progress;
	int download_status;
};

struct json_data_t {
	char *data;
	size_t len;
};

struct http_download_t {
	size_t header_size;
	size_t content_length;
};

typedef enum {
	HAWKBIT_UPDATE_SKIP = 0,
	HAWKBIT_UPDATE_ATTEMPT,
	HAWKBIT_UPDATE_FORCED
} hawkbit_update_action_t;

typedef enum {
	HAWKBIT_RESULT_SUCCESS = 0,
	HAWKBIT_RESULT_FAILURE,
	HAWKBIT_RESULT_NONE,
} hawkbit_result_status_t;

typedef enum {
	HAWKBIT_EXEC_CLOSED = 0,
	HAWKBIT_EXEC_PROCEEDING,
	HAWKBIT_EXEC_CANCELED,
	HAWKBIT_EXEC_SCHEDULED,
	HAWKBIT_EXEC_REJECTED,
	HAWKBIT_EXEC_RESUMED,
} hawkbit_exec_status_t;

typedef enum {
	HAWKBIT_ACID_CURRENT = 0,
	HAWKBIT_ACID_UPDATE,
} hawkbit_dev_acid_t;

#define HAWKBIT_RX_TIMEOUT	K_SECONDS(3)

#define HAWKBIT_STACK_SIZE 3840
static K_THREAD_STACK_DEFINE(hawkbit_thread_stack, HAWKBIT_STACK_SIZE);
static struct k_thread hawkbit_thread_data;

int poll_sleep = K_SECONDS(30);
static bool connection_ready;
#if defined(CONFIG_NET_MGMT_EVENT)
static struct net_mgmt_event_callback cb;
#endif

/* Utils */
static int atoi_n(const char *s, int len)
{
        int i, val = 0;

	for (i = 0; i < len; i++) {
		if (*s < '0' || *s > '9')
			return val;
		val = (val * 10) + (*s - '0');
		s++;
	}

        return val;
}

static int jsoneq(const char *json, jsmntok_t *tok, const char *s)
{
	if (tok->type == JSMN_STRING &&
		(int) strlen(s) == tok->end - tok->start &&
		strncmp(json + tok->start, s, tok->end - tok->start) == 0) {
		return 1;
	}
	return 0;
}

static int json_parser(struct json_data_t *json, jsmn_parser *parser,
		       jsmntok_t *tks, u16_t num_tokens)
{
	int ret = 0;

	SYS_LOG_DBG("JSON: max tokens supported %d", num_tokens);

	jsmn_init(parser);
	ret = jsmn_parse(parser, json->data, json->len, tks, num_tokens);
	if (ret < 0) {
		switch (ret) {
		case JSMN_ERROR_NOMEM:
			SYS_LOG_ERR("JSON: Not enough tokens");
			break;
		case JSMN_ERROR_INVAL:
			SYS_LOG_ERR("JSON: Invalid character found");
			break;
		case JSMN_ERROR_PART:
			SYS_LOG_ERR("JSON: Incomplete JSON");
			break;
		}
		return ret;
	} else if (ret == 0 || tks[0].type != JSMN_OBJECT) {
		SYS_LOG_ERR("JSON: First token is not an object");
		return 0;
	}

	SYS_LOG_DBG("JSON: %d tokens found", ret);

	return ret;
}

static int hawkbit_time2sec(const char *s)
{
        int sec = 0;

	/* Time: HH:MM:SS */
	sec = atoi_n(s, 2) * 60 * 60;
	sec += atoi_n(s + 3, 2) * 60;
	sec += atoi_n(s + 6, 2);

	if (sec < 0) {
		return -1;
	} else {
		return sec;
	}
}

/* HTTP parser callbacks */
static int handle_headers_complete(struct http_parser *parser)
{
	/* Check if our buffer is enough for a valid body */
	if (parser->nread + parser->content_length >= TCP_RECV_BUF_SIZE) {
		SYS_LOG_ERR("header + body larger than buffer %d",
			    TCP_RECV_BUF_SIZE);
		return -1;
	}
	return 0;
}

static int handle_http_body(struct http_parser* parser, const char *at, size_t len)
{
	struct json_data_t *json = parser->data;

	if (json) {
		json->data = (char *) at;
		json->len = len;
	}

	return 0;
}

static int handle_headers_complete_download(struct http_parser *parser)
{
	struct http_download_t *http_data = parser->data;

	if (parser->status_code == 200) {
		http_data->header_size = parser->nread;
		http_data->content_length = parser->content_length;
	}

	return 1;
}

void hawkbit_device_acid_read(struct hawkbit_device_acid *device_acid)
{
	flash_read(flash_dev, FLASH_AREA_APPLICATION_STATE_OFFSET, device_acid,
		   sizeof(*device_acid));
}

/**
 * @brief Update an ACID of a given type on flash.
 *
 * @param type ACID type to update
 * @param acid New ACID value
 * @return 0 on success, negative on error.
 */
static int hawkbit_device_acid_update(hawkbit_dev_acid_t type,
				      u32_t new_value)
{
	struct hawkbit_device_acid device_acid;
	int ret;

	flash_read(flash_dev, FLASH_AREA_APPLICATION_STATE_OFFSET, &device_acid,
		   sizeof(device_acid));
	if (type == HAWKBIT_ACID_UPDATE) {
		device_acid.update = new_value;
	} else {
		device_acid.current = new_value;
	}

	flash_write_protection_set(flash_dev, false);
	ret = flash_erase(flash_dev, FLASH_AREA_APPLICATION_STATE_OFFSET,
			  FLASH_AREA_APPLICATION_STATE_SIZE);
	flash_write_protection_set(flash_dev, true);
	if (ret) {
		return ret;
	}

	flash_write_protection_set(flash_dev, false);
	ret = flash_write(flash_dev, FLASH_AREA_APPLICATION_STATE_OFFSET,
			  &device_acid, sizeof(device_acid));
	flash_write_protection_set(flash_dev, true);
	return ret;
}

static int hawkbit_start(void)
{
	int ret = 0;
	struct hawkbit_device_acid init_acid;
	u8_t boot_status;

	/* Update boot status and acid */
	hawkbit_device_acid_read(&init_acid);
	SYS_LOG_INF("ACID: current %d, update %d",
		    init_acid.current, init_acid.update);
	boot_status = boot_status_read();
	SYS_LOG_INF("Current boot status %x", boot_status);
	if (boot_status == BOOT_STATUS_ONGOING) {
		boot_status_update();
		SYS_LOG_INF("Updated boot status to %x", boot_status_read());
		ret = boot_erase_flash_bank(FLASH_AREA_IMAGE_1_OFFSET);
		if (ret) {
			SYS_LOG_ERR("Flash bank erase at offset %x: error %d",
				    FLASH_AREA_IMAGE_1_OFFSET, ret);
			return ret;
		} else {
			SYS_LOG_DBG("Erased flash bank at offset %x",
				    FLASH_AREA_IMAGE_1_OFFSET);
		}
		if (init_acid.update != -1) {
			ret = hawkbit_device_acid_update(HAWKBIT_ACID_CURRENT,
						  init_acid.update);
		}
		if (!ret) {
			hawkbit_device_acid_read(&init_acid);
			SYS_LOG_INF("ACID updated, current %d, update %d",
				    init_acid.current, init_acid.update);
		} else {
			SYS_LOG_ERR("Failed to update ACID: %d", ret);
		}
	}
	return ret;
}

static void hawkbit_header_cb(struct net_context *context,
			      struct net_pkt *pkt,
			      int status, void *user_data)
{
	struct hawkbit_download *hbd = user_data;
	struct net_buf *rx_buf;
	u8_t *ptr;
	int len;
	struct http_parser_settings http_settings;
	struct http_download_t http_data = { 0 };
	struct http_parser parser;

	if (!pkt) {
		SYS_LOG_ERR("Download: EARLY end of buffer");
		hbd->header_status = -1;
		k_sem_give(tcp_get_recv_wait_sem(TCP_CTX_HAWKBIT));
		return;
	}

	if (net_pkt_appdatalen(pkt) > 0) {
		rx_buf = pkt->frags;
		ptr = net_pkt_appdata(pkt);
		len = rx_buf->len - (ptr - rx_buf->data);

		while (rx_buf) {
			/* handle data */
			memcpy(tcp_buf + hbd->http_header_read, ptr, len);
			hbd->http_header_read += len;
			tcp_buf[hbd->http_header_read] = 0;

			rx_buf = rx_buf->frags;
			if (!rx_buf) {
				break;
			}

			ptr = rx_buf->data;
			len = rx_buf->len;
		}
	}

	net_pkt_unref(pkt);

	http_parser_init(&parser, HTTP_RESPONSE);
	http_parser_settings_init(&http_settings);
	http_settings.on_headers_complete = handle_headers_complete_download;
	parser.data = &http_data;

	/* Parse the HTTP headers available from the first buffer */
	http_parser_execute(&parser, &http_settings, (const char *) tcp_buf,
				hbd->http_header_read);

	if (parser.status_code > 0 && parser.status_code != 200) {
		SYS_LOG_ERR("Download: http error %d", parser.status_code);
		hbd->header_status = -1;
	} else if (http_data.content_length > 0 &&
		   parser.http_errno != HPE_INVALID_HEADER_TOKEN) {
		hbd->http_header_size = http_data.header_size;
		hbd->http_content_size = http_data.content_length;
		hbd->header_status = 1;
		k_sem_give(tcp_get_recv_wait_sem(TCP_CTX_HAWKBIT));
	}
}

static void hawkbit_download_cb(struct net_context *context,
				struct net_pkt *pkt,
				int status, void *user_data)
{
	struct hawkbit_download *hbd = user_data;
	struct net_buf *rx_buf;
	u8_t *ptr;
	int len, downloaded;

	if (!pkt) {
		/* handle end of stream */
		k_sem_give(tcp_get_recv_wait_sem(TCP_CTX_HAWKBIT));
		hbd->download_status = 1;
		return;
	}

	rx_buf = pkt->frags;
	ptr = net_pkt_appdata(pkt);
	len = rx_buf->len - (ptr - rx_buf->data);

	while (rx_buf && hbd->download_status == 0) {
		/* handle data */
		if (flash_block_write(flash_dev, FLASH_AREA_IMAGE_1_OFFSET,
				      &hbd->downloaded_size, ptr,
				      len, false) < 0) {
			hbd->download_status = -1;
		}
		downloaded = hbd->downloaded_size * 100 /
				hbd->http_content_size;
		if (downloaded > hbd->download_progress) {
			hbd->download_progress = downloaded;
			SYS_LOG_DBG("%d%%", hbd->download_progress);
		}

		rx_buf = rx_buf->frags;
		if (!rx_buf) {
			break;
		}

		ptr = rx_buf->data;
		len = rx_buf->len;
	}
	net_pkt_unref(pkt);
}

static int hawkbit_install_update(u8_t *tcp_buffer, size_t size,
				  const char *download_http,
				  size_t file_size)
{
	struct hawkbit_download hbd;
	int ret, len;

	if (!tcp_buffer || !download_http || !file_size) {
		return -EINVAL;
	}

	flash_write_protection_set(flash_dev, false);
	ret = flash_erase(flash_dev, FLASH_AREA_IMAGE_1_OFFSET,
			  FLASH_BANK_SIZE);
	flash_write_protection_set(flash_dev, true);
	if (ret != 0) {
		SYS_LOG_ERR("Failed to erase flash at offset %x, size %d",
			    FLASH_AREA_IMAGE_1_OFFSET, FLASH_BANK_SIZE);
		return -EIO;
	}

	SYS_LOG_INF("Starting the download and flash process");

	/* Here we just proceed with a normal HTTP Download process */
	memset(tcp_buffer, 0, size);
	snprintf(tcp_buffer, size, "GET %s HTTP/1.1\r\n"
				"Host: %s\r\n"
				"Connection: close\r\n"
				"\r\n",
				download_http, HAWKBIT_HOST);

	ret = tcp_send(TCP_CTX_HAWKBIT, (const char *) tcp_buffer,
		       strlen(tcp_buffer));
	if (ret < 0) {
		SYS_LOG_ERR("Failed to send buffer, err %d", ret);
		return ret;
	}

	/* Receive is special for download, since it writes to flash */
	memset(tcp_buf, 0, TCP_RECV_BUF_SIZE);
	memset(&hbd, 0, sizeof(struct hawkbit_download));

	if (tcp_connect(TCP_CTX_HAWKBIT) < 0) {
		ret = -1;
		goto error_cleanup;
	}

	net_context_recv(tcp_get_net_context(TCP_CTX_HAWKBIT),
			 hawkbit_header_cb, K_NO_WAIT, (void *)&hbd);

	/* wait here for the connection to complete or timeout */
	k_sem_take(tcp_get_recv_wait_sem(TCP_CTX_HAWKBIT), K_SECONDS(3));
	if (hbd.header_status < 0) {
		SYS_LOG_ERR("Unable to start the download process %d", ret);
		ret = -1;
		goto error_cleanup;
	}

	if (hbd.http_content_size != file_size) {
		SYS_LOG_ERR("Download: file size not the same as reported, "
			    "found %d, expecting %d",
			    hbd.http_content_size, file_size);
		ret = -1;
		goto error_cleanup;
	}

	/* Everything looks good, so fetch and flash */
	if (hbd.http_header_read > hbd.http_header_size) {
		len = hbd.http_header_read - hbd.http_header_size;
		ret = flash_block_write(flash_dev, FLASH_AREA_IMAGE_1_OFFSET,
				      &hbd.downloaded_size,
				      tcp_buffer + hbd.http_header_size,
				      len, false);
		if (ret < 0) {
			goto error_cleanup;
		}
	}

	/* wait here for the connection to complete or timeout */
	do {
		net_context_recv(tcp_get_net_context(TCP_CTX_HAWKBIT),
				 hawkbit_download_cb, K_NO_WAIT,
				 (void *)&hbd);
	} while (hbd.download_status >= 0 &&
		 k_sem_take(tcp_get_recv_wait_sem(TCP_CTX_HAWKBIT),
			    K_SECONDS(1)) != 0);

	if (hbd.download_status < 0) {
		SYS_LOG_ERR("Unable to finish the download process %d",
			    hbd.download_status);
		ret = -1;
		goto error_cleanup;
	}

	/*
	 * Call flash_block_write() one last time to finish writing
	 * the flash buffer
	 *
	 * NOTE: Ignoring returned error here since done is done.
	 * downloaded_size will be incorrect and OTA will fail.
	 */
	flash_block_write(flash_dev, FLASH_AREA_IMAGE_1_OFFSET,
			  &hbd.downloaded_size, NULL, 0, true);

	tcp_cleanup(TCP_CTX_HAWKBIT, true);

	if (hbd.downloaded_size != hbd.http_content_size) {
		SYS_LOG_ERR("Download: downloaded image size mismatch, "
				"downloaded %d, expecting %d",
				hbd.downloaded_size, hbd.http_content_size);
		return -1;
	}

	SYS_LOG_INF("Download: downloaded bytes %d", hbd.downloaded_size);

	return 0;

error_cleanup:
	tcp_cleanup(TCP_CTX_HAWKBIT, true);
	return ret;
}

static int hawkbit_query(u8_t *tcp_buffer, size_t size,
			 struct json_data_t *json)
{
	struct http_parser_settings http_settings;
	struct http_parser parser;
	int ret;

	if (!tcp_buffer) {
		return -EINVAL;
	}

	SYS_LOG_DBG("\n\n%s", tcp_buffer);

	ret = tcp_send(TCP_CTX_HAWKBIT, (const char *) tcp_buffer,
		       strlen(tcp_buffer));
	if (ret < 0) {
		SYS_LOG_ERR("Failed to send buffer, err %d", ret);
		return ret;
	}
	ret = tcp_recv(TCP_CTX_HAWKBIT, (char *) tcp_buffer,
		       size, HAWKBIT_RX_TIMEOUT);
	if (ret <= 0) {
		SYS_LOG_ERR("No received data (ret=%d)", ret);
		tcp_cleanup(TCP_CTX_HAWKBIT, true);
		return -1;
	}

	http_parser_init(&parser, HTTP_RESPONSE);
	http_parser_settings_init(&http_settings);
	http_settings.on_body = handle_http_body;
	http_settings.on_headers_complete = handle_headers_complete;
	parser.data = json;

	http_parser_execute(&parser, &http_settings,
				(const char *) tcp_buffer, ret);
	if (parser.status_code != 200) {
		SYS_LOG_ERR("Invalid HTTP status code %d",
						parser.status_code);
		return -1;
	}

	if (json) {
		if (json->data == NULL) {
			SYS_LOG_ERR("JSON data not found");
			return -1;
		}
		/* FIXME: Each poll needs a new connection, this saves
		 * us from using content from a previous package.
		 */
		json->data[json->len] = '\0';
		SYS_LOG_DBG("JSON DATA:\n%s", json->data);
	}

	SYS_LOG_DBG("Hawkbit query completed");

	return 0;
}

static int hawkbit_report_config_data(u8_t *tcp_buffer, size_t size)
{
	const struct product_id_t *product_id = product_id_get();
	char *helper;

	SYS_LOG_INF("Reporting target config data to Hawkbit");

	/* Use half for the header and half for the json content */
	memset(tcp_buffer, 0, size);
	helper = tcp_buffer + size / 2;

	/* Start with JSON as we need to calculate the content length */
	snprintf(helper, size / 2, "{"
			"\"data\":{"
				"\"board\":\"%s\","
				"\"serial\":\"%x\"},"
			"\"status\":{"
				"\"result\":{\"finished\":\"success\"},"
				"\"execution\":\"closed\"}"
			"}", product_id->name, product_id->number);

	/* size / 2 should be enough for the header */
	snprintf(tcp_buffer, size, "PUT %s/%s-%x/configData HTTP/1.1\r\n"
			"Host: %s\r\n"
			"Content-Type: application/json\r\n"
			"Content-Length: %d\r\n"
			"Connection: close\r\n"
			"\r\n%s", HAWKBIT_JSON_URL,
			product_id->name, product_id->number,
			HAWKBIT_HOST, strlen(helper), helper);

	if (hawkbit_query(tcp_buffer, size, NULL) < 0) {
		SYS_LOG_ERR("Error when reporting config data to Hawkbit");
		return -1;
	}

	return 0;
}

static int hawkbit_report_update_status(int acid,
					u8_t *tcp_buffer, size_t size,
					hawkbit_result_status_t status,
					hawkbit_exec_status_t exec)
{
	char finished[8];	/* 'success', 'failure', 'none' */
	char execution[11];
	char *helper;

	switch (status) {
	case HAWKBIT_RESULT_SUCCESS:
		snprintf(finished, sizeof(finished), "success");
		break;
	case HAWKBIT_RESULT_FAILURE:
		snprintf(finished, sizeof(finished), "failure");
		break;
	case HAWKBIT_RESULT_NONE:
		snprintf(finished, sizeof(finished), "none");
		break;
	}

	/* 'closed', 'proceeding', 'canceled', 'scheduled',
	 * 'rejected', 'resumed'
	 */
	switch (exec) {
	case HAWKBIT_EXEC_CLOSED:
		snprintf(execution, sizeof(execution), "closed");
		break;
	case HAWKBIT_EXEC_PROCEEDING:
		snprintf(execution, sizeof(execution), "proceeding");
		break;
	case HAWKBIT_EXEC_CANCELED:
		snprintf(execution, sizeof(execution), "canceled");
		break;
	case HAWKBIT_EXEC_SCHEDULED:
		snprintf(execution, sizeof(execution), "scheduled");
		break;
	case HAWKBIT_EXEC_REJECTED:
		snprintf(execution, sizeof(execution), "rejected");
		break;
	case HAWKBIT_EXEC_RESUMED:
		snprintf(execution, sizeof(execution), "resumed");
		break;
	}

	SYS_LOG_INF("Reporting action ID feedback: %s", finished);

	/* Use half for the header and half for the json content */
	memset(tcp_buffer, 0, size);
	helper = tcp_buffer + size / 2;

	/* Start with JSON as we need to calculate the content length */
	snprintf(helper, size / 2, "{"
			"\"id\":\"%d\","
			"\"status\":{"
				"\"result\":{\"finished\":\"%s\"},"
				"\"execution\":\"%s\"}"
			"}", acid, finished, execution);

	/* size / 2 should be enough for the header */
	snprintf(tcp_buffer, size,
			"POST %s/%s-%x/deploymentBase/%d/feedback HTTP/1.1\r\n"
			"Host: %s\r\n"
			"Content-Type: application/json\r\n"
			"Content-Length: %d\r\n"
			"Connection: close\r\n"
			"\r\n%s", HAWKBIT_JSON_URL,
			product_id_get()->name, product_id_get()->number, acid,
			HAWKBIT_HOST, strlen(helper), helper);

	if (hawkbit_query(tcp_buffer, size, NULL) < 0) {
		SYS_LOG_ERR("Error when reporting acId feedback to Hawkbit");
		return -1;
	}

	return 0;
}

int hawkbit_ddi_poll(void)
{
	jsmn_parser jsmnp;
	jsmntok_t jtks[60];	/* Enough for one artifact per SM */
	int i, ret, len, ntk;
	static hawkbit_update_action_t hawkbit_update_action;
	static int json_acid;
	struct hawkbit_device_acid device_acid;
	struct json_data_t json = { NULL, 0 };
	char deployment_base[40];	/* TODO: Find a better value */
	char download_http[200];	/* TODO: Find a better value */
	bool update_config_data = false;
	int file_size = 0;
	char *helper;
	const struct product_id_t *product_id = product_id_get();

	SYS_LOG_DBG("Polling target data from Hawkbit");

	memset(tcp_buf, 0, TCP_RECV_BUF_SIZE);
	snprintf(tcp_buf, TCP_RECV_BUF_SIZE, "GET %s/%s-%x HTTP/1.1\r\n"
				"Host: %s\r\n"
				"Connection: close\r\n"
				"\r\n",
				HAWKBIT_JSON_URL,
				product_id->name,
				product_id->number,
				HAWKBIT_HOST);

	ret = hawkbit_query(tcp_buf, TCP_RECV_BUF_SIZE, &json);
	if (ret < 0) {
		SYS_LOG_ERR("Error when polling from Hawkbit");
		return ret;
	}

	ntk = json_parser(&json, &jsmnp, jtks,
			sizeof(jtks) / sizeof(jsmntok_t));
	if (ntk <= 0) {
		SYS_LOG_ERR("Error when parsing JSON from target");
		return -1;
	}

	/* Hawkbit DDI v1 targetid */
	memset(deployment_base, 0, sizeof(deployment_base));
	/* TODO: Implement cancel action logic */
	for (i = 1; i < ntk - 1; i++) {
		/* config -> polling -> sleep */
		if (jsoneq(json.data, &jtks[i], "config") &&
				(i + 5 < ntk) &&
				(jsoneq(json.data, &jtks[i + 4], "sleep"))) {
			/* Sleep format: HH:MM:SS */
			if (jtks[i + 5].end - jtks[i + 5].start > 8) {
				SYS_LOG_ERR("Invalid poll sleep string");
				continue;
			}
			len = hawkbit_time2sec(json.data + jtks[i + 5].start);
			if (len > 0 &&
				poll_sleep != K_SECONDS(len)) {
				SYS_LOG_INF("New poll sleep %d seconds", len);
				poll_sleep = K_SECONDS(len);
				i += 5;
			}
		} else if (jsoneq(json.data, &jtks[i], "deploymentBase") &&
				(i + 3 < ntk) &&
				(jsoneq(json.data, &jtks[i + 2], "href"))) {
			/* Just extract the deploymentBase piece */
			helper = strstr(json.data + jtks[i + 3].start,
							"deploymentBase/");
			if (helper == NULL ||
					helper > json.data + jtks[i + 3].end) {
				continue;
			}
			len = json.data + jtks[i + 3].end - helper;
			memcpy(&deployment_base, helper, len);
			deployment_base[len] = '\0';
			SYS_LOG_DBG("Deployment base %s", deployment_base);
			i += 3;
		} else if (jsoneq(json.data, &jtks[i], "configData") &&
				(i + 3 < ntk) &&
				(jsoneq(json.data, &jtks[i + 2], "href"))) {
			update_config_data = true;
			i += 3;
		}
	}

	/* Update config data if the server asked for it */
	if (update_config_data) {
		hawkbit_report_config_data(tcp_buf, TCP_RECV_BUF_SIZE);
	}

	if (strlen(deployment_base) == 0) {
		SYS_LOG_DBG("No deployment base found, no actions to take");
		return 0;
	}

	/* Hawkbit DDI v1 deploymentBase */
	memset(tcp_buf, 0, TCP_RECV_BUF_SIZE);
	snprintf(tcp_buf, TCP_RECV_BUF_SIZE, "GET %s/%s-%x/%s HTTP/1.1\r\n"
				"Host: %s\r\n"
				"Connection: close\r\n"
				"\r\n",
				HAWKBIT_JSON_URL,
				product_id->name,
				product_id->number,
				deployment_base, HAWKBIT_HOST);

	memset(&json, 0, sizeof(struct json_data_t));
	if (hawkbit_query(tcp_buf, TCP_RECV_BUF_SIZE, &json) < 0) {
		SYS_LOG_ERR("Error when querying from Hawkbit");
		return -1;
	}

	/* We have our own limit here, which is directly affected by the
	 * number of artifacts available as part of the software module
	 * assigned, so needs coordination with the deployment process.
	 */
	ntk = json_parser(&json, &jsmnp, jtks,
			sizeof(jtks) / sizeof(jsmntok_t));
	if (ntk <= 0) {
		SYS_LOG_ERR("Error when parsing JSON from deploymentBase");
		return -1;
	}

	ret = 0;
	memset(download_http, 0, sizeof(download_http));
	for (i = 1; i < ntk - 1; i++) {
		if (jsoneq(json.data, &jtks[i], "id")) {
			/* id -> id */
			json_acid = atoi_n(json.data + jtks[i + 1].start,
					jtks[i + 1].end - jtks[i + 1].start);
			SYS_LOG_DBG("Hawkbit ACTION ID %d", json_acid);
			i += 1;
		} else if (jsoneq(json.data, &jtks[i], "deployment")) {
			/* deployment -> download, update or chunks */
			if (i + 5 >= ntk) {
				continue;
			}
			/* Check just the first 2 keys, since chunk is [] */
			if (jsoneq(json.data, &jtks[i + 2], "update")) {
				i += 3;
			} else if (jsoneq(json.data, &jtks[i + 4], "update")) {
				i += 5;
			} else {
				continue;
			}
			/* Now just find the update action */
			if (jsoneq(json.data, &jtks[i], "skip")) {
				hawkbit_update_action = HAWKBIT_UPDATE_SKIP;
				SYS_LOG_DBG("Hawkbit update action: SKIP");
			} else if (jsoneq(json.data, &jtks[i], "attempt")) {
				hawkbit_update_action = HAWKBIT_UPDATE_ATTEMPT;
				SYS_LOG_DBG("Hawkbit update action: ATTEMPT");
			} else if (jsoneq(json.data, &jtks[i], "forced")) {
				hawkbit_update_action = HAWKBIT_UPDATE_FORCED;
				SYS_LOG_DBG("Hawkbit update action: FORCED");
			}
		} else if (jsoneq(json.data, &jtks[i], "chunks")) {
			if (jtks[i + 1].type != JSMN_ARRAY) {
				continue;
			}
			if (jtks[i + 1].size != 1) {
				SYS_LOG_ERR("Only one chunk is supported, %d",
							jtks[i + 1].size);
				ret = -1;
				break;
			}
			i += 1;
		} else if (jsoneq(json.data, &jtks[i], "part")) {
			if (!jsoneq(json.data, &jtks[i + 1], "os")) {
				SYS_LOG_ERR("Only part 'os' is supported");
				ret = -1;
				break;
			}
			i += 1;
		} else if (jsoneq(json.data, &jtks[i], "size")) {
			file_size = atoi_n(json.data + jtks[i + 1].start,
					jtks[i + 1].end - jtks[i + 1].start);
			SYS_LOG_DBG("Artifact file size: %d", file_size);
			i += 1;
		} else if (jsoneq(json.data, &jtks[i], "download-http")) {
			/* We just support DEFAULT tenant on the same server */
			if (i + 3 >= ntk ||
				!jsoneq(json.data, &jtks[i + 2], "href")) {
				SYS_LOG_ERR("No href entry for download-http");
				ret = -1;
				continue;
			}
			/* Extracting everying after server address */
			helper = strstr(json.data + jtks[i + 3].start,
						"/DEFAULT/controller/v1");
			if (helper == NULL ||
					helper > json.data + jtks[i + 3].end) {
				continue;
			}
			len = json.data + jtks[i + 3].end - helper;
			if (len >= sizeof(download_http)) {
				SYS_LOG_ERR("Download HREF too big (%d)", len);
				ret = - 1;
				continue;
			}
			memcpy(&download_http, helper, len);
			download_http[len] = '\0';
			SYS_LOG_DBG("Artifact address: %s", download_http);
			i += 3;
		}
	}

	hawkbit_device_acid_read(&device_acid);

	if (device_acid.current == json_acid) {
		/* We are coming from a successful flash, update the server */
		hawkbit_report_update_status(json_acid,
					     tcp_buf, TCP_RECV_BUF_SIZE,
					     HAWKBIT_RESULT_SUCCESS,
					     HAWKBIT_EXEC_CLOSED);
		return 0;
	} else if (device_acid.update == json_acid) {
		/* There was already an atempt, so announce a failure */
		hawkbit_report_update_status(json_acid,
					     tcp_buf, TCP_RECV_BUF_SIZE,
					     HAWKBIT_RESULT_FAILURE,
					     HAWKBIT_EXEC_CLOSED);
		return 0;
	}

	/* Perform the action */
	if (strlen(download_http) == 0) {
		SYS_LOG_DBG("No download http address found, no action");
		return 0;
	}
	/* Error detected when parsing the SM */
	if (ret == -1) {
		hawkbit_report_update_status(json_acid,
					     tcp_buf, TCP_RECV_BUF_SIZE,
					     HAWKBIT_RESULT_FAILURE,
					     HAWKBIT_EXEC_CLOSED);
		return -1;
	}
	if (file_size > FLASH_BANK_SIZE) {
		SYS_LOG_ERR("Artifact file size too big (%d)", file_size);
		hawkbit_report_update_status(json_acid,
					     tcp_buf, TCP_RECV_BUF_SIZE,
					     HAWKBIT_RESULT_FAILURE,
					     HAWKBIT_EXEC_CLOSED);
		return -1;
	}

	/* Here we should have everything we need to apply the action */
	SYS_LOG_INF("Valid action ID %d found, proceeding with the update",
					json_acid);
	hawkbit_report_update_status(json_acid,
				     tcp_buf, TCP_RECV_BUF_SIZE,
				     HAWKBIT_RESULT_SUCCESS,
				     HAWKBIT_EXEC_PROCEEDING);
	ret = hawkbit_install_update(tcp_buf, TCP_RECV_BUF_SIZE, download_http, file_size);
	if (ret != 0) {
		SYS_LOG_ERR("Failed to install the update for action ID %d",
					json_acid);
		return -1;
	}

	SYS_LOG_INF("Triggering OTA update.");
	boot_trigger_ota();
	ret = hawkbit_device_acid_update(HAWKBIT_ACID_UPDATE, json_acid);
	if (ret != 0) {
		SYS_LOG_ERR("Failed to update ACID: %d", ret);
		return -1;
	}
	SYS_LOG_INF("Image id %d flashed successfuly, rebooting now",
					json_acid);

	/* Reboot and let the bootloader take care of the swap process */
	sys_reboot(0);

	return 0;
}

/* Firmware OTA thread (Hawkbit) */
static void hawkbit_service(void)
{
	u32_t hawkbit_failures = 0;
	int ret;

	SYS_LOG_INF("Starting FOTA Service Thread");

	do {
		k_sleep(poll_sleep);

		if (!connection_ready) {
			SYS_LOG_DBG("Network interface is not ready");
			continue;
		}

		tcp_interface_lock();

		ret = hawkbit_ddi_poll();
		if (ret < 0) {
			hawkbit_failures++;
			if (hawkbit_failures == HAWKBIT_MAX_SERVER_FAIL) {
				SYS_LOG_ERR("Too many unsuccessful poll"
					    " attempts, rebooting!");
				sys_reboot(0);
			}
		} else {
			/* restart the failed attempt counter */
			hawkbit_failures = 0;
		}

		tcp_interface_unlock();

		stack_analyze("Hawkbit Thread", hawkbit_thread_stack,
			      K_THREAD_STACK_SIZEOF(hawkbit_thread_stack));
	} while (1);
}

static void event_iface_up(struct net_mgmt_event_callback *cb,
			   u32_t mgmt_event, struct net_if *iface)
{
	connection_ready = true;
}

int hawkbit_init(void)
{
	struct net_if *iface = net_if_get_default();
	int ret;

	ret = hawkbit_start();
	if (ret) {
		SYS_LOG_ERR("Hawkbit Client initialization generated "
			    "an error: %d", ret);
		return ret;
	}

	k_thread_create(&hawkbit_thread_data, &hawkbit_thread_stack[0],
			K_THREAD_STACK_SIZEOF(hawkbit_thread_stack),
			(k_thread_entry_t) hawkbit_service,
			NULL, NULL, NULL, K_PRIO_COOP(7), 0, K_NO_WAIT);

#if defined(CONFIG_NET_MGMT_EVENT)
	/* Subscribe to NET_IF_UP if interface is not ready */
	if (!atomic_test_bit(iface->flags, NET_IF_UP)) {
		net_mgmt_init_event_callback(&cb, event_iface_up,
					     NET_EVENT_IF_UP);
		net_mgmt_add_event_callback(&cb);
		return ret;
	}
#endif

	event_iface_up(NULL, NET_EVENT_IF_UP, iface);

	return ret;
}
