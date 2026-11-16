/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * PMU 总线门的唯一谓词源（26.11.19 解耦批）。
 *
 * The single source of truth for the PMU bus gate (26.11.19 decoupling batch).
 *
 * 为什么要有这个头：采样支和取键支共用一条 I2C 总线，这件事以前是隐式的——
 * 两侧各自把「ready && !holds_pmu && !usb_storage_active()」抄一遍，任一侧改动
 * 另一侧不知道。解耦批把采样从取键里摘出来之后，两侧变成**并列**的两个调用点，
 * 各写一份就不再是笔误而是设计缺陷（§5.2 守卫按分裂红）。
 *
 * Why this header: sampling and key-taking share one I2C bus. That used to be
 * implicit - each side spelled the three conditions out itself, so a change on
 * one side left the other stale. Once sampling stops riding on key-taking the
 * two become **parallel** call sites; a second copy is no longer a typo but a
 * design defect, and the §5.2 guard goes red on the split.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app.h"

/// 三条件：PMU 在总线上 → 页面没独占总线 → USB 存储没让出总线。
/// 顺序就是历史形态，不改——真机日志与判读仍按这个顺序读。
/// / Three conditions: PMU present, page does not own the bus, USB storage has not yielded it.
/// The order is historical and stays: logs and hardware reads still follow it.
static inline bool pmu_bus_static_ok(bool ready, bool holds_pmu, bool usb_active) {
    return ready && !holds_pmu && !usb_active;
}

/// 到点判断。周期两侧共用 `APP_LOCK_POLL_MS`（基线 200 ms，不是待优化项）。
/// 戳由调用方给：采样侧 `last_sample_ms`、取键侧 `last_key_ms`——**两戳不串写**。
/// / Due check. Both sides share `APP_LOCK_POLL_MS` (200 ms baseline, not a tuning knob).
/// The stamp comes from the caller: `last_sample_ms` for sampling, `last_key_ms` for keys.
static inline bool pmu_bus_due(int64_t now_ms, int64_t last_ms) {
    return now_ms - last_ms >= APP_LOCK_POLL_MS;
}
