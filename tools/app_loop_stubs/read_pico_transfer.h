/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
static inline void read_pico_transfer_service_poll(void) {}

#include "common.h"
/* 体检批 ③（2026-11-01）：这三行曾经把**两个不同的 enum 拍平成一个**，还把 STA/AP
 * 整体挤后一位：真实头是 `MODE_{AP=0,STA}` 与 `STATE_{STOPPED=0,STARTING,READY,
 * UPLOADING,ERROR}` 两组独立枚举，桩却写成 STOPPED=0 / STA=1 / AP=2。
 * 后果不是编译错，而是**宿主测的状态机跑在与固件不同的数字上**——绿也证明不了固件。
 * 由 check_api_surface.py 第一次运行抓获。
 * Gate-health ③ (2026-11-01): these three lines flattened TWO enums into one and pushed
 * STA/AP down by a slot. The real header has MODE_{AP=0,STA} and STATE_{STOPPED=0,
 * STARTING,READY,UPLOADING,ERROR}. The host therefore asserted on different numbers than
 * the firmware uses - green, and meaningless. Caught the first time the check ran. */
enum { READ_PICO_TRANSFER_STOPPED = 0, READ_PICO_TRANSFER_STARTING,
       READ_PICO_TRANSFER_READY, READ_PICO_TRANSFER_UPLOADING, READ_PICO_TRANSFER_ERROR };
enum { READ_PICO_TRANSFER_MODE_AP = 0, READ_PICO_TRANSFER_MODE_STA };
typedef struct {int state,mode;bool network_ready;} read_pico_transfer_status_t;
typedef struct {int mode;bool network_only;} read_pico_transfer_cfg_t;
void read_pico_transfer_get_status(read_pico_transfer_status_t *s);
int read_pico_transfer_get_saved_wifi(char *ssid,bool *configured);
int read_pico_transfer_start(const read_pico_transfer_cfg_t *cfg);
bool read_pico_transfer_try_stop_if_idle(void);
