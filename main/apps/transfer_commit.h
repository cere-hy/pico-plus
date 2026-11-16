// SPDX-FileCopyrightText: 2026 mindreset
// SPDX-License-Identifier: Apache-2.0
//
// 26.11.1：提交泵的两拍从 `app_transfer.c` 抽出来，独立成可宿主编译的文件。
//
// 为什么要抽：26.9.30 出包门第 3 条的 7b（busy 路径）在真机上**没走到**
// （`dropped after` 0、`ERR(7)` 0，最慢一次 1474 ms 未越 1500 ms 死线），而宿主侧
// `app_loop_host_test.c` 的 `transfer_pump_commit()` 只是空桩——两头都没覆盖。
// 用户明确拒绝「连翻 20 次」这类非人类操作去够这条分支（见真机验收包络纪律）。
// 于是这条路只剩一条合法通道：让两拍状态机本身能被宿主跑。`app_transfer.c` 牵
// 着 BLE / 存储 / UI 一大片，编不进宿主；两拍只依赖「取请求 / 放读者 / 跑四拍 /
// 报错 / 读时钟 / 打日志」六件事，抽出来就自由了。
//
// 抽层不改语义：函数体、日志文本、死线数值、静态量的生命周期一字未动，`app_transfer.c`
// 保留同名薄封装 `transfer_pump_commit()`，调用点（`app_loop.c`）零改动。宿主跑的是
// **同一份代码**，不是为测试另写一套复制品——复制品会漂移，那是比没有覆盖更坏的事。
//
// Why this file exists: gate #3's 7b (the busy path) never fired on hardware - zero
// "dropped after", zero ERR(7), slowest frame 1474 ms against a 1500 ms deadline - and
// the host side only had an empty stub. The user refused to force it with "tap 20 times
// while pushing", which is not a human reading action (see the acceptance-envelope
// discipline). That leaves exactly one legitimate channel: make the two-beat machine
// itself runnable on the host. app_transfer.c drags in BLE, storage and UI and cannot be
// built there; the machine needs only six things (take request, release reader, run
// four beats, abort, read the clock, log). Pulling it out frees it. No semantic change:
// body, log strings, deadline value and static lifetimes are untouched, app_transfer.c
// keeps the same-named thin wrapper, and app_loop.c's call site does not move. The host
// runs the same code, never a copy - a copy drifts, and a drifting copy is worse than
// no coverage at all.

#ifndef TRANSFER_COMMIT_H
#define TRANSFER_COMMIT_H

#include <stdbool.h>
#include <stdint.h>

// 分拍等待上限：prep 一轮是百毫秒量级，给 1500ms（3 个 500ms tick）覆盖最大书
// 的最长 prep；到点未停即按失败处理（裁定 #3：不原地重试，等待 = 卡死观感）。
// 同一个数被两处共用——让路链（push_arm 等 prep 停机）与提交泵（等读者放手）是
// 同一种「等 prep 停下来」的语义，不许出现第二个数。
// Tick-wait ceiling: a prep round runs in the hundreds of milliseconds, so 1500 ms
// (three 500 ms ticks) covers the longest prep of the biggest book; on expiry it is
// treated as a failure (ruling #3: never retry in place - waiting reads as a freeze).
// The same number serves two places on purpose: the yield chain (push_arm waits for
// prep to stop) and the commit pump (waits for the reader to let go) are the same
// "wait for prep to stop" semantics; a second number is forbidden.
#define PUSH_YIELD_WAIT_MS 1500

/// 提交泵的一次推进（每帧调一次）。取请求 → 拍 1 → 等读者 → 拍 2 → 跑四拍。
/// One pump step (called once per frame): take the request, beat 1, wait for the
/// reader, beat 2, run the four beats.
void transfer_commit_pump(void);

/// 等待态查询（诊断/宿主断言用，不参与任何判决）。
/// Waiting-state query (diagnostics and host assertions; never a verdict).
bool transfer_commit_waiting(void);

/// 当前拍号（1 = 已取请求、尚未放开读者）。/ Current beat number.
uint8_t transfer_commit_beat(void);

#endif // TRANSFER_COMMIT_H
