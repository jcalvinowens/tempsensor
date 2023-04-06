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

#include "util.h"
#include "hdc1080.h"
#include "wifi.h"
#include "phone.h"

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

#define UNREASONABLY_LONG_AGO	(1670653382)
#define MAX_QUEUE_COUNT		(60)
#define WIFI_RETRIES		(5)
#define HTTP_RETRIES		(5)

const char *TAG = "tempsensor";
const int gpio_sda = 3;
const int gpio_scl = 10;
const int gpio_led = 18;
const int gpio_swpwr_en = 19;

void app_main(void)
{
	char *tx, sha[16 + 1], ser[12 + 1];
	uint16_t s1, s2, s3, temp, humi;
	int64_t sleep_duration_us = 60000000;
	bool have_queued_samples = false;
	esp_ota_img_states_t otastate;
	wifi_ap_record_t apinfo;
	struct nvsdata64 orders;
	const char *wakereason;
	nvs_handle_t nvshandle;
	nvs_iterator_t it = NULL;
	uint32_t send_delay_us;
	cJSON *root, *arr, *d;
	time_t wake_epoch = 0;
	esp_err_t ret;
	int attempts;
	time_t now;

	gpio_reset_pin(gpio_swpwr_en);
	gpio_set_direction(gpio_swpwr_en, GPIO_MODE_OUTPUT);
	gpio_set_level(gpio_swpwr_en, 0);

	gpio_reset_pin(gpio_led);
	gpio_set_direction(gpio_led, GPIO_MODE_OUTPUT_OD);
	gpio_set_level(gpio_led, 1);

	/*
	 * Take the temperature/humidity measurement immediately, to minimize
	 * error due to heating of the power ICs and CPU.
	 */

	hdc1080_configure(&s1, &s2, &s3);
	hdc1080_read_both(&temp, &humi);
	time(&now);

	/*
	 * Initialize NVS (both for our use below, and for the wifi driver).
	 */

	ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		nvs_flash_erase();
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);

	/*
	 * Read our current orders and send_delay from NVS.
	 */

	orders.u64 = 0;
	send_delay_us = 0;
	if (nvs_open("cfg", NVS_READWRITE, &nvshandle) == ESP_OK) {
		nvs_get_u32(nvshandle, "send_delay_us", &send_delay_us);
		nvs_get_u64(nvshandle, "orders", &orders.u64);
		nvs_close(nvshandle);
	}

	if (orders.interval > 300)
		orders.interval = 300;

	if (orders.count > MAX_QUEUE_COUNT)
		orders.count = MAX_QUEUE_COUNT;

	/*
	 * If the RTC isn't set, don't try to report old results, just act as
	 * though we have no orders. Same if we just flashed a new version.
	 */

	ret = esp_ota_get_state_partition(esp_ota_get_running_partition(),
					  &otastate);

	if ((ret == ESP_OK && otastate == ESP_OTA_IMG_PENDING_VERIFY) ||
	    (now < UNREASONABLY_LONG_AGO)) {
		nvs_handle_t nvsh;

		/*
		 * Invalidate orders from the previous firmware, and immediately
		 * phone home to make sure the newly flashed version functions.
		 */

		orders.u64 = 0;

		/*
		 * Dicard any queued data (we don't want to worry about keeping
		 * the format stable between firmware versions).
		 */

		if (nvs_open("data", NVS_READWRITE, &nvsh) == ESP_OK) {
			nvs_erase_all(nvsh);
			nvs_commit(nvsh);
			nvs_close(nvsh);
		}
	}

	if (orders.u64 != 0) {
		if (now < orders.epoch + orders.interval * (orders.count)) {
			struct nvsdata64 sample;
			nvs_handle_t nvsh;
			time_t tmp, next;
			char key[16];

			/*
			 * If we have orders, and this isn't the final sample,
			 * queue it in NVS and go back to sleep without powering
			 * up the wifi.
			 */

			snprintf(key, sizeof(key), "%llu", now);
			sample.epoch = now;
			sample.temp = temp;
			sample.humi = humi;

			if (nvs_open("data", NVS_READWRITE, &nvsh) == ESP_OK) {
				nvs_set_u64(nvsh, key, sample.u64);
				nvs_commit(nvsh);
				nvs_close(nvsh);
			}

			next = (now - orders.epoch) / orders.interval;
			next = (next + 1) * orders.interval + orders.epoch;

			time(&tmp);
			sleep_duration_us = (next - tmp) * 1000000LL;
			goto out;
		}

		/*
		 * This is the final sample, it's time to dump the queue.
		 */

		have_queued_samples = true;
	}

	/*
	 * We're done interacting with I2C and don't need the pulls, so we can
	 * light sleep here to save power.
	 */

	if (send_delay_us != 0) {
		ESP_LOGI(TAG, "Sleep for %lu before TX", send_delay_us);
		sleep_us(send_delay_us);
	}

	/*
	 * Turn the switcher on, give it a moment to stabilize.
	 */

	gpio_set_level(gpio_swpwr_en, 1);
	delay_ms(1);

	/*
	 * Initialize the wifi, retrying after a random delay.
	 */

	wifi_init();
	attempts = 0;
	while (1) {
		if (wifi_connect() == ESP_OK)
			break;

		delay_ms(1000 + esp_random() % 1000);

		if (++attempts < WIFI_RETRIES)
			continue;

		flash_led(10, 50);
		goto out;
	}

	/*
	 * Initialize NTP, wait for a sync if the RTC hasn't been set yet.
	 */

	sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
	sntp_setoperatingmode(SNTP_OPMODE_POLL);
	sntp_setservername(0, CONFIG_NTP_SERVER_HOSTNAME);
	sntp_init();

	if (now < UNREASONABLY_LONG_AGO)
		while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED)
			delay_ms(10);

	/*
	 * Build the JSON for our HTTPS POST to the backend.
	 */

	ESP_ERROR_CHECK(esp_wifi_sta_get_ap_info(&apinfo));
	esp_app_get_elf_sha256(sha, sizeof(sha));
	snprintf(ser, sizeof(ser), "%04x%04x%04x", s1, s2, s3);
	ESP_LOGI(TAG, "sha=%s serial=%s", sha, ser);
	write_wake_reason(&wakereason);

	root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "fw_sha", sha);
	cJSON_AddStringToObject(root, "serial", ser);
	cJSON_AddNumberToObject(root, "ap_rssi", apinfo.rssi);
	cJSON_AddNumberToObject(root, "ap_channel", apinfo.primary);
	cJSON_AddStringToObject(root, "ap_ssid", (const char *)apinfo.ssid);
	cJSON_AddStringToObject(root, "wake_reason", wakereason);
	cJSON_AddNumberToObject(root, "send_delay_us", send_delay_us);
	cJSON_AddNumberToObject(root, "wifi_retries", attempts);

	switch (apinfo.second) {
	case WIFI_SECOND_CHAN_ABOVE:
		cJSON_AddStringToObject(root, "ap_bw", "40+");
		break;

	case WIFI_SECOND_CHAN_BELOW:
		cJSON_AddStringToObject(root, "ap_bw", "40-");
		break;

	case WIFI_SECOND_CHAN_NONE:
		cJSON_AddStringToObject(root, "ap_bw", "20");
		break;
	}

	arr = cJSON_AddArrayToObject(root, "data");

	d = cJSON_CreateObject();
	cJSON_AddNumberToObject(d, "epoch", now);
	cJSON_AddNumberToObject(d, "temperature", temp);
	cJSON_AddNumberToObject(d, "humidity", humi);
	cJSON_AddItemToArray(arr, d);

	if (have_queued_samples) {
		nvs_handle_t nvsh;

		if (nvs_open("data", NVS_READONLY, &nvsh) == ESP_OK) {
			ret = nvs_entry_find("nvs", "data", NVS_TYPE_U64, &it);
			while (ret == ESP_OK) {
				struct nvsdata64 sample;
				nvs_entry_info_t info;

				nvs_entry_info(it, &info);
				nvs_get_u64(nvsh, info.key, &sample.u64);

				d = cJSON_CreateObject();
				cJSON_AddNumberToObject(d, "epoch",
							sample.epoch);
				cJSON_AddNumberToObject(d, "temperature",
							sample.temp);
				cJSON_AddNumberToObject(d, "humidity",
							sample.humi);

				cJSON_AddItemToArray(arr, d);
				ret = nvs_entry_next(&it);
			}

			nvs_release_iterator(it);
			nvs_close(nvsh);
		}
	}

	/*
	 * Try to phone home, retry with a random delay.
	 */

	attempts = 0;
	while (++attempts < HTTP_RETRIES) {
		tx = cJSON_Print(root);
		ret = phone_home(tx, ser, &wake_epoch);
		free(tx);

		if (ret == ESP_OK)
			break;

		ESP_LOGE(TAG, "Can't phone home, retrying...");
		cJSON_AddNumberToObject(root, "retries", attempts);
		delay_ms(esp_random() % 100);
	}

	cJSON_Delete(root);
	esp_wifi_disconnect();
	esp_wifi_stop();
	esp_wifi_deinit();

	/*
	 * If we successfully submitted the queued data, delete it.
	 */

	if (have_queued_samples && ret == ESP_OK) {
		nvs_handle_t nvsh;

		if (nvs_open("data", NVS_READWRITE, &nvsh) == ESP_OK) {
			ret = nvs_entry_find("nvs", "data", NVS_TYPE_U64, &it);
			while (ret == ESP_OK) {
				nvs_entry_info_t info;

				nvs_entry_info(it, &info);
				nvs_erase_key(nvsh, info.key);
				ret = nvs_entry_next(&it);
			}

			nvs_release_iterator(it);
			nvs_commit(nvsh);
			nvs_close(nvsh);
		}
	}

	/*
	 * If we have a target wake time, compute how long we should sleep.
	 */

	if (wake_epoch) {
		time_t t;

		time(&t);
		sleep_duration_us = (wake_epoch - t) * 1000000L;

		if (sleep_duration_us > 300 * 1000000L)
			sleep_duration_us = 300 * 1000000L;

		if (sleep_duration_us < 0)
			sleep_duration_us = 0;
	}

out:
	/*
	 * Ensure the switcher and LED are turned off.
	 */

	gpio_set_level(gpio_swpwr_en, 0);
	gpio_set_level(gpio_led, 1);

	/*
	 * Go to sleep.
	 */

	ESP_LOGI(TAG, "Entering deep sleep for %lldus!", sleep_duration_us);
	esp_sleep_enable_timer_wakeup(sleep_duration_us);
	esp_deep_sleep_start();
}
