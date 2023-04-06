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

#include "ota.h"
#include "util.h"

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

esp_err_t do_https_ota(const char *url)
{
	esp_http_client_config_t config = {
		.url = url,
		.transport_type = HTTP_TRANSPORT_OVER_SSL,
		.cert_pem = server_cert_pem_start,
	};
	esp_https_ota_config_t otaconfig = {
		.http_config = &config,
	};
	esp_https_ota_handle_t handle;
	const esp_app_desc_t *oldinfo;
	esp_app_desc_t newinfo;
	esp_err_t ret;

	ret = esp_https_ota_begin(&otaconfig, &handle);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Can't begin OTA? %d", ret);
		return ESP_FAIL;
	}

	oldinfo = esp_app_get_description();
	ret = esp_https_ota_get_img_desc(handle, &newinfo);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Can't read new OTA version? %d", ret);
		goto err;
	}

	/*
	 * No version checking, except that we won't reinstall the same firmware
	 * we're already running.
	 */

	if (!memcmp(oldinfo->app_elf_sha256, newinfo.app_elf_sha256, 32)) {
		ESP_LOGE(TAG, "Refusing to reinstall identical firmware");
		goto err;
	}

	ESP_LOGI(TAG, "Downloading/writing new firmware...");

	do {
		ret = esp_https_ota_perform(handle);
	} while (ret == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Can't download new OTA image? %d", ret);
		goto err;
	}

	ESP_LOGI(TAG, "New firmware donwload complete!");

	if (!esp_https_ota_is_complete_data_received(handle)) {
		ESP_LOGE(TAG, "Incomplete data received!");
		goto err;
	}

	ret = esp_https_ota_finish(handle);
	if (ret == ESP_OK) {
		nvs_handle_t nvsh;

		/*
		 * Clear any stale data before restarting.
		 */
		if (nvs_open("data", NVS_READWRITE, &nvsh) == ESP_OK) {
			nvs_erase_all(nvsh);
			nvs_commit(nvsh);
			nvs_close(nvsh);
		}

		ESP_LOGI(TAG, "Successful OTA, restarting...");
		esp_restart();

		__builtin_unreachable();
	}

	ESP_LOGE(TAG, "Unsuccessful OTA: %d", ret);
	return ret;

err:
	esp_https_ota_abort(handle);
	return ESP_FAIL;
}
