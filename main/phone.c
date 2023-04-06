/*
 * ESP32-C3 + HDC1080 Wifi Temperature/Humidity Sensor
 *
 * Copyright (C) 2023 Calvin Owens <jcalvinowens@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "phone.h"
#include "util.h"
#include "ota.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/i2c.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include <sys/param.h>
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include <lwip/netdb.h>
#include "driver/rtc_io.h"
#include "soc/rtc.h"
#include "esp_sleep.h"
#include "esp_sntp.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "cJSON.h"

extern const char server_cert_pem_start[] asm("_binary_cert_pem_start");
extern const char server_cert_pem_end[] asm("_binary_cert_pem_end");
extern const char *TAG;

esp_err_t phone_home(const char *str, const char *serial_str,
		     time_t *wake_epoch)
{
	esp_http_client_config_t config = {
		.transport_type = HTTP_TRANSPORT_OVER_SSL,
		.cert_pem = server_cert_pem_start,
		.method = HTTP_METHOD_POST,
	};
	esp_http_client_handle_t handle;
	esp_ota_img_states_t otastate;
	char url[64] = {0}, response[256 + 1] = {0};
	cJSON *root, *tmp;
	esp_err_t err;
	int64_t len;
	int ret;

	config.url = url;
	snprintf(url, sizeof(url), "https://%s/data/%s", CONFIG_PHONE_HOME_TGT,
		 serial_str);

	handle = esp_http_client_init(&config);
	ESP_LOGI(TAG, "POSTing '%s' to %s", str, config.url);

	err = esp_http_client_open(handle, strlen(str));
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Bad POST open?");
		flash_led(1, 150);
		goto err;
	}

	ret = esp_http_client_write(handle, str, strlen(str));
	if (ret != strlen(str)) {
		ESP_LOGE(TAG, "Bad POST write?");
		flash_led(2, 150);
		goto err;
	}

	len = esp_http_client_fetch_headers(handle);
	if (len <= 0 || len > sizeof(response) - 1) {
		ESP_LOGE(TAG, "Unexpected Content-Length %lld", len);
		flash_led(3, 150);
		goto err;
	}

	ret = esp_http_client_read_response(handle, response,
					    len > sizeof(response) - 1 ?
					    sizeof(response) - 1 : len);

	if (ret < 0) {
		ESP_LOGE(TAG, "Error reading HTTP response");
		flash_led(4, 150);
		goto err;
	}

	if (ret != len) {
		ESP_LOGE(TAG, "Unexpected HTTP response length %d", ret);
		flash_led(5, 150);
		goto err;
	}

	ESP_LOGI(TAG, "Server responds: '%s'", response);

	root = cJSON_Parse(response);
	if (!root) {
		ESP_LOGE(TAG, "Response is not JSON?");
		flash_led(2, 300);
		goto err1;
	}

	tmp = cJSON_GetObjectItem(root, "new_fw");
	if (tmp) {
		ESP_LOGI(TAG, "Server specifies new OTA firmware!");

		err = do_https_ota(tmp->valuestring);
		ESP_LOGE(TAG, "OTA FAILED (%d)", err);
		flash_led(3, 300);
		goto err1;
	}

	tmp = cJSON_GetObjectItem(root, "next_epoch");
	if (tmp) {
		cJSON *tmp2, *tmp3;

		*wake_epoch = tmp->valueint;

		tmp2 = cJSON_GetObjectItem(root, "queue_interval");
		tmp3 = cJSON_GetObjectItem(root, "queue_count");
		if (tmp2 && tmp3) {
			struct nvsdata64 orders;
			nvs_handle_t nvsh;

			orders.epoch = tmp->valueint;
			orders.interval = tmp2->valueint;
			orders.count = tmp3->valueint;

			if (orders.interval > 300)
				orders.interval = 300;

			if (orders.count > 60)
				orders.count = 60;

			if (nvs_open("cfg", NVS_READWRITE, &nvsh) == ESP_OK) {
				nvs_set_u64(nvsh, "orders", orders.u64);
				nvs_commit(nvsh);
				nvs_close(nvsh);
			}
		}
	}

	tmp = cJSON_GetObjectItem(root, "next_send_delay_us");
	if (tmp) {
		nvs_handle_t nvsh;

		if (nvs_open("cfg", NVS_READWRITE, &nvsh) == ESP_OK) {
			nvs_set_u32(nvsh, "send_delay_us", tmp->valueint);
			nvs_commit(nvsh);
			nvs_close(nvsh);
		}
	}

	err = esp_ota_get_state_partition(esp_ota_get_running_partition(),
					  &otastate);

	if (err == ESP_OK && otastate == ESP_OTA_IMG_PENDING_VERIFY) {
		ESP_LOGI(TAG, "First OTA boot worked, cancel rollback");
		esp_ota_mark_app_valid_cancel_rollback();
	}

	tmp = cJSON_GetObjectItem(root, "wait_for_ntp_sync");
	if (tmp && cJSON_IsTrue(tmp)) {
		ESP_LOGI(TAG, "Will wait for NTP sync as commanded...");

		while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED)
			delay_ms(10);
	}

	cJSON_Delete(root);
	esp_http_client_cleanup(handle);
	return ESP_OK;

err1:
	cJSON_Delete(root);
err:
	esp_http_client_cleanup(handle);
	return ESP_FAIL;
}
