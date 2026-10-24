/* 中文：dict_popup 宿主测试共享替身基座。/ English: Shared shim base for the dict_popup host test. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

/// 矩形与字体对齐枚举：与 components/epdiy/src/epdiy.h 逐值一致。
/// Rectangle and alignment enums, value-for-value identical to epdiy.h.
typedef struct { int x, y, width, height; } EpdRect;
enum EpdFontFlags { EPD_DRAW_ALIGN_LEFT, EPD_DRAW_ALIGN_CENTER, EPD_DRAW_ALIGN_RIGHT };

/// 可控时钟：宿主测试通过推进 dict_test_now_us 模拟查询态超时。
/// Controllable clock: the host test advances dict_test_now_us to simulate the
/// query-state timeout.
extern int64_t dict_test_now_us;
int64_t esp_timer_get_time(void);

/// 日志替身：真调用形态（参数照旧求值）、输出丢弃。这样 -Werror 下计时/状态变量
/// 仍算被使用，宿主与产品构建的警告面一致；plain ((void)0) 宏会把 open_us 之类
/// 变成 unused-but-set，正是上一轮宿主桩红过的同型坑。
/// Logging double: real call shape (arguments still evaluated), output discarded.
/// Under -Werror this keeps timing/state variables "used" so the host warning
/// surface matches the product build; a plain ((void)0) macro would turn open_us
/// and friends into unused-but-set variables - the same stub pitfall that went red
/// in an earlier round.
static inline void esp_log_noop(const char* tag, const char* fmt, ...) { (void)tag; (void)fmt; }
#define ESP_LOGI(tag, ...) esp_log_noop(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) esp_log_noop(tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...) esp_log_noop(tag, __VA_ARGS__)
