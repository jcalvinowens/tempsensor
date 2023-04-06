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

/*
 * Configure the HDC1080, and write its serial to @s[1-3].
 */
void hdc1080_configure(uint16_t *s1, uint16_t *s2, uint16_t *s3);

/*
 * Read temperature/humidity from the configured HDC1080.
 */
void hdc1080_read_both(uint16_t *temp, uint16_t *humi);
