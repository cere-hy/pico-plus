/* SPDX-License-Identifier: Apache-2.0
 * 宿主端替身：esp_timer。生词本 AI 批的「5s 合批」要一个可推进的时钟，
 * 测试里直接改 vocab_test_now_us，不用真等 5 秒。
 * Host stub: esp_timer. The vocabulary AI batch's "5s batching" needs a clock the
 * test can advance - set vocab_test_now_us instead of really waiting 5 seconds. */
#pragma once
#include <stdint.h>

extern int64_t vocab_test_now_us; /* 测试推进它 / the test advances this */

static inline int64_t esp_timer_get_time(void) { return vocab_test_now_us; }
