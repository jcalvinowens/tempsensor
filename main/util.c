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
extern const int gpio_led;

#define MAX_SLEEP_SECONDS (300L)

void delay_ms(int64_t ms)
{
	if (ms <= 0)
		return;

	if (ms > MAX_SLEEP_SECONDS * 1000L) {
		ESP_LOGE(TAG, "Refusing to block for over MAX...");
		ms = MAX_SLEEP_SECONDS * 1000L;
	}

	vTaskDelay(ms / portTICK_PERIOD_MS);
}

void sleep_us(int64_t us)
{
	if (us <= 0)
		return;

	if (us > MAX_SLEEP_SECONDS * 1000000L) {
		ESP_LOGE(TAG, "Refusing to light sleep for over MAX...");
		us = MAX_SLEEP_SECONDS * 1000000L;
	}

	esp_sleep_enable_timer_wakeup(us);
	if (esp_light_sleep_start() == ESP_OK)
		return;

	ESP_LOGE(TAG, "Light sleep didn't work?");
	delay_ms(us / 1000);
}

void flash_led(int count, int flash_ms)
{
	int i;

	for (i = 0; i < count; i++) {
		gpio_set_level(gpio_led, 0);
		delay_ms(flash_ms);
		gpio_set_level(gpio_led, 1);
		delay_ms(flash_ms * 2);
	}
}

void write_wake_reason(const char **wakereason)
{
	switch (esp_reset_reason()) {
	case ESP_RST_POWERON:
		*wakereason = "initial";
		break;

	case ESP_RST_DEEPSLEEP:
		*wakereason = "deepsleep";
		break;

	case ESP_RST_BROWNOUT:
		*wakereason = "brownout";
		break;

	case ESP_RST_PANIC:
		*wakereason = "panic";
		break;

	case ESP_RST_SW:
		*wakereason = "reset";
		break;

	case ESP_RST_INT_WDT:
	case ESP_RST_TASK_WDT:
	case ESP_RST_WDT:
		*wakereason = "watchdog";
		break;

	case ESP_RST_SDIO:
		*wakereason = "sdio";
		break;

	default:
		*wakereason = "unknown";
		break;
	}
}
