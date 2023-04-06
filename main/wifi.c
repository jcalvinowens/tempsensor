/*
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

#include "wifi.h"
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

extern const char *TAG;

static EventGroupHandle_t wifi_eg;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT	   BIT1

static void event_handler(void *arg, esp_event_base_t event_base,
			  int32_t event_id, void *event_data)
{
	if (event_base == WIFI_EVENT) {
		switch (event_id) {
		case WIFI_EVENT_STA_START:
			esp_wifi_connect();
			return;

		case WIFI_EVENT_STA_DISCONNECTED:
			xEventGroupSetBits(wifi_eg, WIFI_FAIL_BIT);
			return;

		case WIFI_EVENT_STA_BEACON_TIMEOUT:
			ESP_LOGE(TAG, "Beacon timeout!");
			return;

		default:
			ESP_LOGI(TAG, "Unexpected WIFI event_id=%ld", event_id);
			return;
		}
	}

	if (event_base == IP_EVENT) {
		switch (event_id) {
		case IP_EVENT_STA_GOT_IP:
			ip_event_got_ip_t* evt = (ip_event_got_ip_t*) event_data;
			ESP_LOGI(TAG, "ip=" IPSTR, IP2STR(&evt->ip_info.ip));
			xEventGroupSetBits(wifi_eg, WIFI_CONNECTED_BIT);
			return;

		case IP_EVENT_GOT_IP6:
			return;

		case IP_EVENT_STA_LOST_IP:
			ESP_LOGE(TAG, "Lost IP!");
			return;

		default:
			ESP_LOGI(TAG, "Unexpected IP event_id=%ld", event_id);
			return;
		}
	}
}

esp_err_t wifi_connect(void)
{
	EventBits_t bits = xEventGroupGetBits(wifi_eg);

	if (bits & WIFI_CONNECTED_BIT)
		return ESP_OK;

	if (bits & WIFI_FAIL_BIT) {
		xEventGroupClearBits(wifi_eg, WIFI_FAIL_BIT);
		esp_wifi_connect();
	}

	bits = xEventGroupWaitBits(wifi_eg, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
				   pdFALSE, pdFALSE, portMAX_DELAY);

	if (bits & WIFI_CONNECTED_BIT)
		return ESP_OK;

	if (bits & WIFI_FAIL_BIT)
		return ESP_FAIL;

	return ESP_ERR_INVALID_STATE;
}

void wifi_init(void)
{
	wifi_config_t wifi_config = {
		.sta = {
			.ssid = CONFIG_WIFI_SSID,
			.password = CONFIG_WIFI_PSK,
			.threshold.authmode = WIFI_AUTH_WPA3_PSK,
			.sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
		},
	};
	esp_event_handler_instance_t instance_any_id;
	esp_event_handler_instance_t instance_got_ip;
	wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
	wifi_eg = xEventGroupCreate();

	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_create_default_wifi_sta();
	ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_config));
	ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
							    ESP_EVENT_ANY_ID,
							    &event_handler, NULL,
							    &instance_any_id));
	ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
							    IP_EVENT_STA_GOT_IP,
							    &event_handler, NULL,
							    &instance_got_ip));
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
	ESP_ERROR_CHECK(esp_wifi_start());
}
