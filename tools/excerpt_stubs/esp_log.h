/* SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：日志桩。W/E/I 级调用计数留存（验收要求：失败不得刷屏，也不得无声）。
 * I 级从 E-1 起也计数——`flushed after N retries (T ms)` 是验收行，必须能被断言，不能只靠眼睛。
 * English: logging stub; W/E/I counts are kept (acceptance: failures must neither spam nor
 * pass in silence). I-level counts since E-1, whose acceptance line must be assertable.
 */
#pragma once
#include <stdio.h>

extern int test_log_w, test_log_e, test_log_i;

#define ESP_LOGW(tag, fmt, ...) do { (void)(tag); ++test_log_w; printf("W " fmt "\n", ##__VA_ARGS__); } while (0)
#define ESP_LOGE(tag, fmt, ...) do { (void)(tag); ++test_log_e; printf("E " fmt "\n", ##__VA_ARGS__); } while (0)
#define ESP_LOGI(tag, fmt, ...) do { (void)(tag); ++test_log_i; printf("I " fmt "\n", ##__VA_ARGS__); } while (0)
#define ESP_LOGD(tag, fmt, ...) do { (void)(tag); } while (0)
