/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：内部堆取证（26.9.13 P0-d）。把「内部堆的块分布」在**事件发生的那一刻**
 * 打到串口，而不是等到事后猜。
 * English: Internal-heap evidence (26.9.13 P0-d). Prints the internal heap's block
 * layout at the instant the event happens instead of guessing about it later.
 *
 * 为什么要有这个组件：26.9.12 的取证函数埋在 ble_page_turner.c 里且是 static，
 * 只在「翻页器 BLE 起栈被拒」那条路上触发；而真机上失败的是手机推送那条路
 * （push_sink），两条路两套闸门。结果就是：面板说红（14.0 K）、闸门一行输出都
 * 没有、广播却起来了——手里没有一个数字能回答「谁放行的」。
 * 于是把同一份实现提成公共组件，所有相关站点共用一份口径、一份限额。
 * Why this component exists: 26.9.12's dump lived as a static function inside
 * ble_page_turner.c and fired only on the page-turner's start-gate refusal - while
 * the field failure was on the phone-push path (push_sink), which has its own gate.
 * The result: the panel read red (14.0 K), the gate printed nothing, and the radio
 * came up anyway - not one number on the wire could answer "who let it through".
 * One implementation, shared by every relevant site, one format, one budget.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/// 每个站点最多打几份（含逐块清单的那一份）。退避重试可能 30 s 一次，不限额会
/// 把串口刷满、把真正的上下文冲掉——取证的价值在于「第一次」，不在于「每一次」。
/// Per-site dump budget (including the one carrying the block list). Retries back
/// off 30 s at a time; without a cap the dumps would bury the surrounding context,
/// and the value of evidence is in the FIRST occurrence, not in every one.
#define PICO_HEAP_DUMP_PER_SITE 2

/// 全系统最多几份。26.9.15 E0：8 → 24。原先「四个站点各两份正好 8」的算法漏算了两件事：
/// 实际站点是 **9 个**（不是 4 个），而且 `push-*` 那四个会随用户反复开关蓝牙持续触发，
/// 8 份在真实使用里连一轮「开关两次蓝牙 + 进出两次传书页」都撑不过，之后最关键的
/// `push-bypass` 一句都不剩。24 份 = 12 个站点 × 2，留足余量。
/// System-wide cap, 8 -> 24. The old "four sites at two each" arithmetic missed two things:
/// there are NINE sites, and the `push-*` set keeps firing whenever the user toggles the
/// radio - eight dumps do not survive one round of "toggle twice + enter the transfer page
/// twice", after which the crucial `push-bypass` line is gone. 24 = 12 sites x 2.
#define PICO_HEAP_DUMP_TOTAL 24

/// 打一份内部堆证据。
///
/// @param site 站点名，稳定不改（离线统计按它分组）：`ble-start` / `push-precheck`
///             / `push-bypass` / `push-adv` / `push-connect` / `push-disc` /
///             `transfer-enter`。
/// @param who  调用方路径，形如 `app_transfer:push_button_action`。**这是 26.9.13
///             要抓的那个字段**：闸门被跳过时，只有它能说明「谁放行的」。
///
/// 输出两段：
///   1) 一行摘要（永远打）：站点、调用方、内部 free/largest/碎片率、PSRAM free/largest；
///   2) 逐块清单（只用 `heap_caps_print_heap_info`，每站点第一份打，且受总限额约束）。
///
/// @param site Stable site name used by offline grouping.
/// @param who  Caller path, e.g. `app_transfer:push_button_action` - the field this
///             round exists for: when a gate is skipped, only this says who skipped it.
void pico_heap_dump(const char *site, const char *who);

/// 本次开机已经打了几份（含被限额挡掉的调用？不含——只数真正打出去的）。
/// How many dumps actually went out this boot (blocked calls are not counted).
unsigned pico_heap_dump_count(void);

/// 逐块清单（26.9.14 D2）：把内部堆里**每一个块**的地址、大小、空闲与否打到串口。
///
/// 与 `pico_heap_dump` 的分工：那份只给每个池的**汇总**（容量/空闲/最大块/块数），
/// 足以判断「病没病」，但判断不了「谁把池子切开的」——26.9.13 的现场就是
/// `len 32767 free 30935 largest 18432 blocks 11/5/16`：一眼看出有 11 个残块，
/// 但残块多大、挨着谁，汇总里没有。定责需要逐块。
///
/// **只在病发时打印**：`only_if_largest_below` 是病发判据，通常填调用方的闸门值
/// （BLE 是 20 * 1024）。健康时一行不出——431 个块的清单在正常使用里只是噪声，
/// 在「11 颗石子」的现场却是唯一能点名凶手的东西。
///
/// Per-block inventory (26.9.14 D2): address, size and free/used for EVERY block in the
/// internal heap. `pico_heap_dump` gives per-pool aggregates - enough to tell "is it
/// sick", not enough to tell "who chopped the pool". The 26.9.13 scene was
/// `len 32767 free 30935 largest 18432 blocks 11/5/16`: eleven remnants visible, their
/// sizes and neighbours not. Naming the culprit needs the block walk.
///
/// Printed ONLY when ill: `only_if_largest_below` is the illness predicate, normally the
/// caller's gate value (20 * 1024 for BLE). Silent when healthy - 431 lines are noise in
/// normal use and the only thing that names the culprit on a "11 pebbles" scene.
///
/// @param site                 站点名，与 `pico_heap_dump` 同表；只用于日志定位。
/// @param only_if_largest_below 仅当内部堆最大空闲块**小于**该值时打印。
void pico_heap_dump_blocks(const char *site, unsigned only_if_largest_below);

/// 一行「路标」（26.9.20 S10）。与 `pico_heap_dump` 的分工：那份是重武器（汇总 + 逐块
/// 清单 + 限额），这份只回答一件事——**这一步让内部最大块掉了多少**。
///
/// 26.9.19 真机发现「每开一次书 largest 永久少 1,024 B」，位置已缩到 `open start` 与
/// `close before` 之间那 1 ms（三个候选：`pending_reserve`、`save_progress`、
/// `book_progress_watch_create`），但没有打点就分不出是谁。五个路标按顺序摆开，
/// 相邻两行的 `delta=` 直接点名。
///
/// One-line landmark (26.9.20 S10). The dump above is the heavy weapon; this answers a
/// single question: how much did this step cost the largest internal block. 26.9.19 found
/// a permanent -1,024 B per book open, narrowed to the 1 ms between `open start` and
/// `close before`, with three candidates and no instrumentation to tell them apart.
///
/// @param step 稳定短名（`reserve1` / `save` / `free` / `font` / `reserve2`），离线按它排序。
void pico_heap_mark(const char *step);

/// 把「大块是谁的」问出来（26.9.20 S13）。
///
/// 背景：`boot-post` 的块表里有个 8,192 B 的已分配块，后面紧跟着 100 B 块，把 21,564 B
/// 的空闲块一分为二——它才是「合并杀手」。**只要它在，largest 就上不了 20,480**，面板
/// 也就永远转不了绿。块表能说「多大、挨着谁」，说不出「谁申请的」。
///
/// 办法：列出所有任务，用 `heap_caps_get_allocated_size(pxStackBase)` 反查每个任务栈
/// 在堆里的真实块大小。任务栈是「栈 + 紧邻的 TCB」这种成对结构，和块表里的
/// `8192` 紧跟 `100` 一模一样；对上了就等于点名。
///
/// 只在开机打一次。`TaskStatus_t` 需要 `CONFIG_FREERTOS_USE_TRACE_FACILITY=y`；关掉时
/// 只打一行说明，绝不让取证工具自己变成故障源。
///
/// Ask "whose block is it" (26.9.20 S13). The boot-post block table holds an allocated
/// 8,192 B block with a 100 B block right behind it, splitting the 21,564 B free block in
/// two - that is the merge killer, and while it exists largest cannot reach 20,480.
/// The table says how big and who its neighbour is, never who asked for it. This walks the
/// task list and reverse-looks-up each task's stack block via
/// `heap_caps_get_allocated_size(pxStackBase)`; a stack plus its adjacent TCB is exactly
/// the "8192 then 100" pattern in the table. Printed once at boot.
void pico_heap_attr(const char *site);

#ifdef __cplusplus
}
#endif
