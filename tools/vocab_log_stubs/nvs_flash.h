/* SPDX-License-Identifier: Apache-2.0
 * 宿主端替身：nvs_flash。生词本 AI 自动落库开关（rp_vocab/autosave）的读取在宿主
 * 测试里必须可控——vocab_test_nvs_autosave 就是那个注入点：
 *   -1 = 键不存在（nvs_get_u8 返回 ESP_ERR_NOT_FOUND，走缺省开）
 *    0 = 读到 0（关）
 *    1 = 读到 1（开）
 *    2 = 怪值（按缺省开）
 * open 永远成功（namespace 不存在与 namespace 里没这个键是同一条退化路径，
 * 产品侧都落到缺省开，不需要第二套注入）。
 * Host stub: nvs_flash. The vocabulary AI auto-logging switch (rp_vocab/autosave)
 * must be controllable in host tests - vocab_test_nvs_autosave is the injection:
 *   -1 = key absent (ESP_ERR_NOT_FOUND -> the default-on path)
 *    0 = read 0 (off)
 *    1 = read 1 (on)
 *    2 = odd value (counts as the default on)
 * open always succeeds: a missing namespace and a namespace without the key take
 * the same degradation on the device (both land on default-on), so a second
 * injection would be redundant. */
#pragma once
#include <stdint.h>

#include "esp_err.h" /* ESP_OK / ESP_ERR_NOT_FOUND from tools/dict_hit_stubs */

typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY = 0, NVS_READWRITE = 1 } nvs_open_mode_t;

extern int vocab_test_nvs_autosave; /* 见文件头 / see the header */

static inline int nvs_open(const char* namespace_name, nvs_open_mode_t open_mode,
                           nvs_handle_t* out_handle) {
    (void)namespace_name; (void)open_mode;
    if (out_handle) *out_handle = 1;
    return ESP_OK;
}

static inline int nvs_get_u8(nvs_handle_t handle, const char* key, uint8_t* out_value) {
    (void)handle;
    if (!out_value || !key) return ESP_ERR_INVALID_ARG;
    if (vocab_test_nvs_autosave < 0) return ESP_ERR_NOT_FOUND;
    *out_value = (uint8_t)vocab_test_nvs_autosave;
    return ESP_OK;
}

static inline void nvs_close(nvs_handle_t handle) { (void)handle; }
