/* SPDX-License-Identifier: Apache-2.0
 * 计时桩：单调毫秒计数，只用于日志里的耗时字段，不参与任何判据。
 * Timer shim: monotonic milliseconds, used only for log fields, never for a criterion.
 */
#pragma once
#include <stdint.h>
#include <time.h>

static inline int64_t esp_timer_get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
