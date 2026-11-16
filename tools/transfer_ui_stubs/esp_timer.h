/* 中文：调度测试替身（transfer_ui 专用）。
 *
 * 为什么需要它：26.9.30 commit 3 起 `app_transfer.c` 走 `esp_timer_get_time()` 判两拍死线；
 * commit 5 抽层把这 3 个调用点搬进 `transfer_commit.c`，但宿主编译单元列表**没跟上**——
 * `transfer_ui_host_test.c` 直接 include 产品源文件，缺这个头就编不过，
 * 整颗牙从 commit 3 起就是死的（2026-11-01 门禁首次跑到才现形）。
 *
 * 本桩只保证链接成立：transfer-ui 不判死线，时钟按每次调用 +1 ms 单调递增，
 * 绝不回退——回退会让"死线已过"这类断言在别的用例里误红。
 * English: scheduler shim for the transfer UI host test. Monotonic, +1 ms per call;
 * this test never judges the commit deadline, the clock only has to exist.
 */
#pragma once
#include <stdint.h>

static inline int64_t esp_timer_get_time(void)
{
    static int64_t s_now_us = 0;
    s_now_us += 1000;
    return s_now_us;
}
