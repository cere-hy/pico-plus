/* 中文：调度测试替身——日志不打印，但实参必须被求值一次。
 * English: scheduler test shim - nothing prints, but every argument must be evaluated. */
#pragma once
#include "common.h"

/* 中文：宏一律展开成 `((void)0)` 时，只出现在格式串里的局部变量会被
 * `-Werror=unused-variable` / `-Werror=unused-but-set-variable` 判成死代码
 * （26.9.18 的 `s_release_known` 就是这么红的，而它在真机上是好好用着的）。
 * 用一个 inline 函数接住实参：编译器看到的是普通调用，变量算被读过，
 * 运行时因为函数体是空的，一条指令都不产生。
 * 这里先 `#undef`，是因为 `common.h` 已经定义了 I/E 两条（给不 include 本头的
 * 测试文件用），不撤掉的话下面三个 `#ifndef` 只有 ESP_LOGW 会生效。
 * English: when the macros collapse to `((void)0)`, a local that only appears inside a
 * format call trips -Wunused-variable / -Wunused-but-set-variable (26.9.18's
 * `s_release_known` went red that way while being perfectly live on the device). An
 * inline sink takes the arguments, so the compiler sees an ordinary call and the variable
 * counts as read; the empty body emits no instructions. The `#undef`s are needed because
 * common.h already defines I/E for test files that do not include this header. */
static inline void host_log_sink(const char *tag, const char *fmt, ...) {
    (void)tag;
    (void)fmt;
}

#undef ESP_LOGW
#undef ESP_LOGI
#undef ESP_LOGE

#define ESP_LOGW(tag, fmt, ...) host_log_sink((tag), (fmt), ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) host_log_sink((tag), (fmt), ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) host_log_sink((tag), (fmt), ##__VA_ARGS__)
