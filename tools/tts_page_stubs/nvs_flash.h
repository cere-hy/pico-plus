/* SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 宿主端替身：nvs_flash。TTS 朗读开关（rp_tts/enabled）的读写在宿主测试里必须
 * 可控——tts_test_nvs_enabled 就是那个注入点：
 *   -1 = 键不存在（nvs_get_u8 返回 ESP_ERR_NOT_FOUND，走缺省关）
 *    0 = 读到 0（关）
 *    1 = 读到 1（开）
 *    2 = 怪值（按缺省关——与 autosave 的「怪值按开」相反，理由见 tts_page.h）
 * 写路径（set/commit）记录到 tts_test_nvs_written / tts_test_nvs_commits，供断言
 * 「开关确实落了盘」。open 永远成功（namespace 不存在与没这个键同一条退化路径）。
 * Host stub: nvs_flash. The TTS read-aloud switch (rp_tts/enabled) must be
 * controllable in host tests - tts_test_nvs_enabled is the injection:
 *   -1 = key absent (ESP_ERR_NOT_FOUND -> default off)
 *    0 = read 0 (off)
 *    1 = read 1 (on)
 *    2 = odd value (counts as default off - the opposite of autosave's odd-is-on,
 *       see tts_page.h for why)
 * The write path (set/commit) records into tts_test_nvs_written /
 * tts_test_nvs_commits so tests can assert the switch really persisted. open
 * always succeeds: a missing namespace and a namespace without the key take the
 * same degradation path. */
#pragma once
#include <stdint.h>

#include "esp_err.h" /* ESP_OK / ESP_ERR_NOT_FOUND from tools/dict_hit_stubs */

typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY = 0, NVS_READWRITE = 1 } nvs_open_mode_t;

extern int tts_test_nvs_enabled;    /* 见文件头 / see the header */
extern int tts_test_nvs_written;    /* 最后一次写入值；-1 = 没写过 / last written value */
extern int tts_test_nvs_commits;    /* commit 次数 / commit count */
extern int tts_test_nvs_open_fails; /* >0 时 open 返回失败 / when >0, open fails */

static inline int nvs_open(const char *namespace_name, nvs_open_mode_t open_mode,
                           nvs_handle_t *out_handle) {
    (void)namespace_name; (void)open_mode;
    if (tts_test_nvs_open_fails > 0) {
        --tts_test_nvs_open_fails;
        return ESP_FAIL;
    }
    if (out_handle) *out_handle = 1;
    return ESP_OK;
}

static inline int nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out_value) {
    (void)handle;
    if (!out_value || !key) return ESP_ERR_INVALID_ARG;
    if (tts_test_nvs_enabled < 0) return ESP_ERR_NOT_FOUND;
    *out_value = (uint8_t)tts_test_nvs_enabled;
    return ESP_OK;
}

static inline int nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value) {
    (void)handle; (void)key;
    tts_test_nvs_written = (int)value;
    return ESP_OK;
}

static inline int nvs_commit(nvs_handle_t handle) {
    (void)handle;
    ++tts_test_nvs_commits;
    return ESP_OK;
}

static inline void nvs_close(nvs_handle_t handle) { (void)handle; }
