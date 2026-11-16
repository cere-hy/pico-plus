/* SPDX-License-Identifier: Apache-2.0
 * SD 探测主机测试的 esp_timer 替身。
 * 26.9.18 S3 起 `read_pico_sd.c` 在 `mount_card()` 里用 `esp_timer_get_time()` 给
 * 三个阶段打点。宿主测试只需要它可链接、单调递增；真正的时间轴由真机黑匣子负责。
 *
 * 31b P1-1 起它还必须**可控**：掉盘判据是 `now - t0 >= 3 s`，而单次超时实测 117 ms、
 * 最坏 2 s——测试不可能真等 3 秒，也不可能把 16 s 的最坏路径跑一遍。所以这里暴露
 * 一个可写的 `sd_test_now_us`，由测试自己推进时间轴，一次一档。
 * 用变量而不是宏，是为了让 `read_pico_sd.c` 里所有取值都走同一个源（宏会在不同
 * 编译单元里展开成不同副本，改一处忘一处就是又一个「靶子是假的」）。
 * Host esp_timer stub for the SD-probe tests. Since 31b P1-1 it must also be
 * CONTROLLABLE: the removal predicate is `now - t0 >= 3 s`, and a single timeout
 * measures 117 ms (worst case 2 s) - a test can neither wait 3 s nor replay the
 * 16 s worst path. The test drives `sd_test_now_us` itself.
 */
#pragma once

#include <stdint.h>

extern int64_t sd_test_now_us;

static inline int64_t esp_timer_get_time(void) { return sd_test_now_us; }
