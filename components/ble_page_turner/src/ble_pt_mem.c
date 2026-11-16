/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：BLE 启动内存闸门的单一真源（gen24）。纯函数、零 IO、零 IDF 依赖——
 * ble_page_turner.c 的 start_locked() 预检与传书页「一键自愈」按钮共用本判定，
 * 两边数字永远一致；宿主测试直接 include 本文件覆盖边界。
 * English: the single source of truth for the BLE start memory gate (gen24). A
 * pure function with no IO and no IDF dependency - start_locked()'s precheck and
 * the transfer page's one-tap self-heal button share this verdict so the numbers
 * can never disagree; host tests include this file directly to cover the bounds.
 *
 * 冻结：只做数值判定，不读堆、不打日志、不碰 NVS。/ Frozen: numeric verdict only -
 * no heap reads, no logging, no NVS.
 */
#include "ble_page_turner.h"

// 26.9.24（审计批复"B2 主攻"）两层判定：内部 RAM 只给「控制块基线」——控制器与协议
// 栈任务跑在硬件路径上只能吃内部 RAM，free 56K 与 largest 16,384（26.9.23 真机实证的
// 控制器成功值：二次 BLE_INIT + 20 连开全程无恙）是它们的底线；数据大块（host mbuf、
// 传书文件写）本就活在 PSRAM，改判 PSRAM 余量——free 256K + largest 64K 两道下限。
// largest_internal 只差一步时会卡「栈起得来但运行中可能崩」的老话——所以 16,384 不是
// 拍脑袋：低于实证成功值的任何一格都要有真机日志背书才许再降。
// Since 26.9.24 (audit ruling "B2") a two-layer verdict: internal RAM reserves only the
// control-block baseline - the controller and stack tasks sit on the hardware path and can
// only use internal RAM, so 56K free and 16,384 largest (26.9.23 field-proven controller
// success value: second BLE_INIT + 20 book opens unharmed) guard them; the data bulk
// (host mbufs, transfer file writes) already lives in PSRAM and is judged on PSRAM
// headroom - 256K free + 64K largest. 16,384 is not a guess: going one notch below the
// proven value requires field-log evidence first.
bool ble_pt_mem_gate(size_t free_internal, size_t largest_internal,
                     size_t free_psram, size_t largest_psram) {
    return free_internal >= BLE_PT_MIN_FREE_INTERNAL &&
           largest_internal >= BLE_PT_MIN_LARGEST_INTERNAL &&
           free_psram >= BLE_PT_MIN_FREE_PSRAM &&
           largest_psram >= BLE_PT_MIN_LARGEST_PSRAM;
}
