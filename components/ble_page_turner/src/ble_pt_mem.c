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

// 控制器跑在硬件路径上只能吃内部 RAM；host 的动态分配虽走 PSRAM，但控制器与协议栈任务的
// 大块分配仍落在内部堆。19K 边缘的最大块起得了栈，运行中也可能崩，所以这是硬闸而不是保守
// 值。阈值与 CrossMux 同板验证的理由见 ble_page_turner.h。/ The controller sits on the
// hardware path and can only use internal RAM; even though the host allocates from PSRAM,
// the controller and stack tasks still need large internal blocks. A 19K largest block
// might bring the stack up but can still crash mid-run, so this is a hard gate, not a
// conservative estimate. See ble_page_turner.h for the values and CrossMux's rationale.
bool ble_pt_mem_gate(size_t free_internal, size_t largest_internal, size_t free_psram) {
    return free_internal >= BLE_PT_MIN_FREE_INTERNAL &&
           largest_internal >= BLE_PT_MIN_LARGEST_INTERNAL &&
           free_psram >= BLE_PT_MIN_FREE_PSRAM;
}
