/*
 * Datasheet: https://www.ti.com/lit/ds/symlink/hdc1080.pdf
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
extern const int gpio_sda;
extern const int gpio_scl;

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

/*
 * Initialize I2C, configure the HDC1080, and write its serial to @s[1-3].
 */
void hdc1080_configure(uint16_t *s1, uint16_t *s2, uint16_t *s3)
{
	uint16_t reg;

	/*
	 * Because the I2C pulls aren't applied while we are asleep, the first
	 * I2c read will fail: just ignore it.
	 */

	ESP_ERROR_CHECK(i2c_master_init());
	dummy_sensor_read(0xFB);
	*s1 = sensor_read(0xFB);
	*s2 = sensor_read(0xFC);
	*s3 = sensor_read(0xFD);

	reg = sensor_read(HDC1080_REG_CFG);

	if (reg & HDC1080_CFG_BTST)
		ESP_LOGE(TAG, "HDC1080 reports voltage <= 2.8V!");

	reg &= ~(HDC1080_CFG_MODE_SEQ | HDC1080_CFG_HTR_ON | HDC1080_CFG_HRES |
		 HDC1080_CFG_TRES);

	sensor_write(HDC1080_REG_CFG, reg);
}

/*
 * Read temperature/humidity from the configured HDC1080. It might be worth
 * adding external I2C pulls in a future rev so we can enter light sleep here.
 */
void hdc1080_read_both(uint16_t *temp, uint16_t *humi)
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
