/* SPDX-License-Identifier: Apache-2.0
 * esp_err_t 桩：值抄自 ESP-IDF v6.1 的 esp_err.h，避免宿主与真机错误码漂移。
 * esp_err_t shim; the values mirror ESP-IDF v6.1 so host and device cannot drift.
 */
#pragma once
#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_INVALID_CRC 0x109
#define ESP_ERR_INVALID_VERSION 0x10A
#define ESP_ERR_INVALID_MAC 0x10B
#define ESP_ERR_NOT_FINISHED 0x10C
#define ESP_ERR_NOT_ALLOWED 0x10D

const char *esp_err_to_name(esp_err_t code);
