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
#define MAX_SLEEP_SECONDS	(300)
#define MAX_QUEUE_COUNT		(60)
#define WIFI_RETRIES		(5)
#define HTTP_RETRIES		(5)

extern const char server_cert_pem_start[] asm("_binary_cert_pem_start");
extern const char server_cert_pem_end[] asm("_binary_cert_pem_end");

static const char *TAG = "tempsensor";
static EventGroupHandle_t wifi_eg;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT	   BIT1

static const int gpio_sda = 3;
static const int gpio_scl = 10;
static const int gpio_led = 18;
static const int gpio_swpwr_en = 19;

static void delay_ms(int64_t ms)
{
	if (ms <= 0)
		return;

	if (ms > MAX_SLEEP_SECONDS * 1000L) {
		ESP_LOGE(TAG, "Refusing to block for over MAX...");
		ms = 60000L;
	}

	vTaskDelay(ms / portTICK_PERIOD_MS);
}

static void sleep_us(int64_t us)
{
	if (us <= 0)
		return;

	if (us > MAX_SLEEP_SECONDS * 1000000L) {
		ESP_LOGE(TAG, "Refusing to light sleep for over MAX...");
		us = 60000000L;
	}

	esp_sleep_enable_timer_wakeup(us);
	if (esp_light_sleep_start() == ESP_OK)
		return;

	ESP_LOGE(TAG, "Light sleep didn't work?");
	delay_ms(us / 1000);
}

static void flash_led(int count, int flash_ms)
{
	int i;

	for (i = 0; i < count; i++) {
		gpio_set_level(gpio_led, 0);
		delay_ms(flash_ms);
		gpio_set_level(gpio_led, 1);
		delay_ms(flash_ms * 2);
	}
}

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

static esp_err_t wifi_connect(void)
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

static void wifi_init(void)
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

/*
 * https://www.ti.com/lit/ds/symlink/hdc1080.pdf
 */

#define HDC1080_I2C_ADDR	0x40

#define HDC1080_REG_TEMP	0x00
#define HDC1080_REG_HUMI	0x01
#define HDC1080_REG_CFG		0x02

#define HDC1080_CFG_HRES	((uint16_t)3 << 8)
#define HDC1080_CFG_TRES	((uint16_t)1 << 10)
#define HDC1080_CFG_BTST	((uint16_t)1 << 11)
#define HDC1080_CFG_MODE_SEQ	((uint16_t)1 << 12)
#define HDC1080_CFG_HTR_ON	((uint16_t)1 << 13)
#define HDC1080_CFG_RST		((uint16_t)1 << 15)

static void __sensor_set_ptr(const uint8_t addr)
{
	ESP_ERROR_CHECK(i2c_master_write_to_device(0, HDC1080_I2C_ADDR,
						   &addr, sizeof(addr),
						   1000 / portTICK_PERIOD_MS));
}

static uint16_t __sensor_read(void)
{
	uint16_t ret;

	ESP_ERROR_CHECK(i2c_master_read_from_device(0, HDC1080_I2C_ADDR,
						    (void *)&ret, sizeof(ret),
						    1000 / portTICK_PERIOD_MS));

	return __builtin_bswap16(ret);
}

static void sensor_write(uint8_t reg, uint16_t data)
{
	const uint8_t buf[3] = {reg, data >> 8, data & 0xFF};

	ESP_ERROR_CHECK(i2c_master_write_to_device(0, HDC1080_I2C_ADDR,
						   buf, sizeof(buf),
						   1000 / portTICK_PERIOD_MS));
}

static void dummy_sensor_read(uint8_t reg)
{
	uint16_t ret __attribute__((unused));

	i2c_master_write_read_device(0, HDC1080_I2C_ADDR,
				     &reg, sizeof(reg),
				     (void *)&ret, sizeof(ret),
				     1000 / portTICK_PERIOD_MS);
}

static uint16_t sensor_read(uint8_t reg)
{
	uint16_t ret;

	ESP_ERROR_CHECK(i2c_master_write_read_device(0, HDC1080_I2C_ADDR,
						     &reg, sizeof(reg),
						     (void *)&ret, sizeof(ret),
						     1000 / portTICK_PERIOD_MS));

	return __builtin_bswap16(ret);
}

static esp_err_t i2c_master_init(void)
{
	i2c_config_t conf = {
		.mode = I2C_MODE_MASTER,
		.sda_io_num = gpio_sda,
		.scl_io_num = gpio_scl,
		.sda_pullup_en = GPIO_PULLUP_ENABLE,
		.scl_pullup_en = GPIO_PULLUP_ENABLE,
		.master.clk_speed = 400000,
	};

	gpio_reset_pin(gpio_sda);
	gpio_reset_pin(gpio_scl);
	i2c_param_config(0, &conf);
	return i2c_driver_install(0, conf.mode, 0, 0, 0);
}

static void hdc1080_configure(void)
{
	uint16_t reg = sensor_read(HDC1080_REG_CFG);

	if (reg & HDC1080_CFG_BTST)
		ESP_LOGE(TAG, "HDC1080 reports voltage <= 2.8V!");

	reg &= ~(HDC1080_CFG_MODE_SEQ | HDC1080_CFG_HTR_ON | HDC1080_CFG_HRES |
		 HDC1080_CFG_TRES);

	sensor_write(HDC1080_REG_CFG, reg);
}

/*
 * It might be worth adding external I2C pulls so we can enter light sleep here.
 */

static void hdc1080_read_both(uint16_t *temp, uint16_t *humi)
{
	__sensor_set_ptr(HDC1080_REG_TEMP);
	delay_ms(20);
	*temp = __sensor_read();

	__sensor_set_ptr(HDC1080_REG_HUMI);
	delay_ms(20);
	*humi = __sensor_read();

	ESP_LOGI(TAG, "Raw values: temp=%04x (%u), humi=%04x (%u)",
		 *temp, *temp, *humi, *humi);
}

static esp_err_t do_https_ota(const char *url)
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

struct nvsdata64 {
	union {
		uint64_t u64;
		struct {
			uint32_t epoch;
			union {
				struct {
					uint16_t interval;
					uint16_t count;
				};
				struct {
					uint16_t temp;
					uint16_t humi;
				};
			};
		};
	};
};

static esp_err_t phone_home(const char *str, const char *serial_str,
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

			if (orders.interval > MAX_SLEEP_SECONDS)
				orders.interval = MAX_SLEEP_SECONDS;

			if (orders.count > MAX_QUEUE_COUNT)
				orders.count = MAX_QUEUE_COUNT;

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

static void write_wake_reason(const char **wakereason)
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
	 *
	 * Because the I2C pulls aren't applied while we are asleep, the first
	 * I2c read will fail: just ignore it.
	 */

	ESP_ERROR_CHECK(i2c_master_init());
	dummy_sensor_read(0xFB);
	s1 = sensor_read(0xFB);
	s2 = sensor_read(0xFC);
	s3 = sensor_read(0xFD);
	hdc1080_configure();
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

	if (orders.interval > MAX_SLEEP_SECONDS)
		orders.interval = MAX_SLEEP_SECONDS;

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
	sntp_setservername(0, "adams.vkv.me");
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

		if (sleep_duration_us > MAX_SLEEP_SECONDS * 1000000L)
			sleep_duration_us = MAX_SLEEP_SECONDS * 1000000L;

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
