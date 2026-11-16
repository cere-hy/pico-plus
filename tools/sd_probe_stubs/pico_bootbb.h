/* SPDX-License-Identifier: Apache-2.0
 * SD 探测主机测试的开机黑匣子替身。
 * 26.9.18 S3 起 `mount_card()` 用 `pico_bootbb_note()` 记录三个阶段。宿主测试里没有
 * 黑匣子，全部退化成空操作——但**参数必须被求值一次**，否则 `-Werror` 会把只出现在
 * 调用里的局部量判成死代码。
 * Host blackbox stub for the SD-probe tests. Since 26.9.18 S3 `mount_card()` records three
 * stages through `pico_bootbb_note()`. There is no blackbox here, so everything collapses
 * to a no-op - but every argument must still be evaluated once, or `-Werror` flags a local
 * that only appears in the call.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
    PICO_BOOTBB_NOTE_SD_STATE = 1,
    PICO_BOOTBB_NOTE_SD_PROBE = 2,
    PICO_BOOTBB_NOTE_SD_DONE  = 3,
    PICO_BOOTBB_NOTE_SD_STAGE = 4,
    // 26.9.20 S9(b)：挂载发布之后的空间统计。桩里同样要有，否则调用点无法编译。
    PICO_BOOTBB_NOTE_SD_SPACE = 5,
    PICO_BOOTBB_NOTE_PUSH_FREEZE = 9,
    PICO_BOOTBB_NOTE_PUSH_HOLD = 10,
};

static inline void pico_bootbb_note(uint8_t code, uint8_t a, uint16_t b) {
    (void)code;
    (void)a;
    (void)b;
}

static inline bool pico_bootbb_active(void) { return false; }
static inline unsigned pico_bootbb_count(void) { return 0; }
static inline void pico_bootbb_start(void) {}
static inline void pico_bootbb_poll(void) {}
static inline void pico_bootbb_replay(void) {}
