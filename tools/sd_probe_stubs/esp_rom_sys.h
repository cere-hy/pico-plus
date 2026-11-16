/* SPDX-License-Identifier: Apache-2.0
 * 26.9.31a：忙等桩。去抖窗必须用微秒级等待——100 Hz tick 下
 * `pdMS_TO_TICKS(2)` 等于 0，会把三连读退化成「一次采样」。
 * Host stub for esp_rom_delay_us; the CD debounce window cannot use vTaskDelay.
 */
#pragma once
#include <stdint.h>
static inline void esp_rom_delay_us(uint32_t us) { (void)us; }
