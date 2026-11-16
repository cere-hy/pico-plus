/* 中文：调度测试替身。/ English: Scheduler test shim. */
#pragma once
#include "common.h"
// E-2（§19.4）：挂载代号。桩头只声明，值由 runner 按拍查表给出。
uint32_t read_pico_sd_mount_epoch(void);
// P1-1：掉盘的第二个信源。桩只复刻契约（每拍被调、判死后 get_info 落成「卡在、卷不可用」）。
int read_pico_sd_watch_io(uint8_t *fail_count, uint16_t *elapsed_ms);
