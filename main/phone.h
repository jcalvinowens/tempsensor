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

#pragma once
#include <stdint.h>
#include "esp_system.h"

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

esp_err_t phone_home(const char *str, const char *serial_str,
		     time_t *wake_epoch);
