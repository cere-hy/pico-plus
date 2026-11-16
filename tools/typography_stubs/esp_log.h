/* SPDX-License-Identifier: Apache-2.0
 * 日志桩：默认静默（`-DTYPO_VERBOSE_LOG=1` 才打印），门禁输出只留断言结果。
 * Log shim: silent by default so the gate output stays readable.
 */
#pragma once
#include <stdio.h>

#ifndef TYPO_VERBOSE_LOG
#define TYPO_VERBOSE_LOG 0
#endif

#define ESP_LOG_AT(level, tag, fmt, ...)                                        \
    do {                                                                        \
        if (TYPO_VERBOSE_LOG)                                                   \
            fprintf(stderr, "[%s] %s: " fmt "\n", level, tag, ##__VA_ARGS__);   \
    } while (0)
#define ESP_LOGE(tag, fmt, ...) ESP_LOG_AT("E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) ESP_LOG_AT("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) ESP_LOG_AT("I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) ESP_LOG_AT("D", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) ESP_LOG_AT("V", tag, fmt, ##__VA_ARGS__)
