/* SPDX-License-Identifier: Apache-2.0
 * 宿主端替身：生词本 AI 批的 app_loop 侧接线。app_loop 宿主 harness 只编
 * app_loop.c（不链真 vocab_log.c），所以这里给 gen26.5 M2 Step8 新增的 5 个
 * 调用点一套空实现——harness 关心的是 UI 轮询时序，不是 TF 卡落盘。
 * Host stub: the vocabulary-book AI batch wiring on the app_loop side. The host
 * harness compiles app_loop.c alone (the real vocab_log.c is not linked), so the
 * five gen26.5 M2 Step8 entry points get no-ops here - the harness is about UI
 * poll timing, not card writes. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

static inline void vocab_log_ai_queue(const char* word) { (void)word; }
static inline void vocab_log_ai_set_def(const char* word, const char* def) {
    (void)word; (void)def;
}
static inline void vocab_log_ai_flush(void) {}
static inline bool vocab_log_autosave_enabled(void) { return true; }
static inline void vocab_log_autosave_init(void) {}
