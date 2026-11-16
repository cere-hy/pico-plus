// SPDX-FileCopyrightText: 2026 mindreset
// SPDX-License-Identifier: Apache-2.0
//
// 26.9.30 commit 5：`.part` 存储层独立成文件。
//
// 为什么要抽出来：commit 4 把两条不变式写进了 `push_sink.c` 的静态函数里——
// 「账本起点在 ARM 时刻就位」「欠账先于新包」。但 `push_sink.c` 依赖 BLE / NVS /
// esp_timer，宿主测试编不进去，于是这两条只存在于注释和真机日志里。审计的原话是
// 「没有机器证明就不算修好」，抽层是让它们可被指认地咬住的唯一办法。
//
// 抽层不是为测试改语义：存储层本来就不依赖会话状态机（`s_part_len` / `s_owed`
// 与 `s_sess` 无关），搬家之后 `push_sink.c` 只少了一段代码，行为一字未改。
//
// Why this file exists: commit 4 put two invariants into static functions inside
// push_sink.c - "the ledger's origin is in place at ARM time" and "the owed packet
// goes first". That file needs BLE/NVS/esp_timer, so the host tests cannot build
// it and the invariants lived only in comments and field logs. Splitting the
// storage layer out is what makes them nameable by a test. This is not a semantic
// change for the sake of testing: the layer never depended on the session
// machine, so nothing moves but the code.

#ifndef PUSH_PART_H
#define PUSH_PART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    // 文件与账本已对齐，可以开始收包。/ File and ledger agree; receiving may start.
    PUSH_PART_ADOPT_OK = 0,
    // stat 失败（卡不在）：簿记原样留着，等下次武装再对账。
    // stat failed (no card): the bookkeeping stays; reconcile on the next arm.
    PUSH_PART_ADOPT_NO_CARD,
    // 文件比账本短，或截断失败：调用方必须清 NVS 并丢弃 .part。
    // The file is shorter than the ledger, or the cut failed: clear NVS, drop .part.
    PUSH_PART_ADOPT_CORRUPT,
} push_part_adopt_t;

/// 全新会话：截断新建 + 账本归零 + 欠账槽清空。
/// A fresh session: truncate open, ledger to zero, owed slot cleared.
bool push_part_open_fresh(void);

/// 续传：把文件对齐到 `received`，并让账本在同一时刻就位。
/// Resume: align the file to `received` and put the ledger in place at that instant.
push_part_adopt_t push_part_adopt(uint32_t received);

/// 写一包。先补欠账（顺序不变式在函数内部，不靠调用方记得）。
/// Write one packet; the owed packet goes first (the ordering invariant lives in
/// here, not in each caller's memory).
bool push_part_append(const uint8_t *data, size_t len);

/// 显式补欠账（END 之前唯一的调用点——那时没有新包会替它触发）。
/// Drain explicitly; END is the only caller, since no new packet will follow.
bool push_part_drain(void);

/// fflush + fsync（NVS 检查点用）。/ fflush + fsync for the NVS checkpoints.
bool push_part_sync(void);

/// `.part` 可写探针（BEGIN 前置闸门）：打开 append 句柄即算可建，不写字节。
/// A writability probe for BEGIN's gate: an append handle that opens is enough.
bool push_part_probe(void);

/// 句柄是否开着（do_verify 要判「曾打开过」才谈得上读回）。
/// Whether the handle is open (do_verify only reads back if it ever opened).
bool push_part_is_open(void);

void push_part_close(void);
void push_part_discard(void);
long push_part_size(void);

/// 当前账本值（只读）。日志/诊断用，不参与任何判决。
/// The current ledger value (read-only). Diagnostics only, never a verdict.
uint32_t push_part_len(void);

#ifdef PUSH_PART_HOST_TEST
// ---- 宿主专用钩子（生产编译里不存在，见 .c 的 #ifdef）----
// Host-only hooks. They do not exist in production builds (see the #ifdef in .c).

/// 指定目录并清空现场（每个用例开头调一次）。
/// Point at a directory and clear it (once per case).
void push_part_test_reset(const char *dir);

/// 下一次 fwrite 只落 `len - short_by` 字节，复刻「瞬时忙 / 卡满」的真短写。
/// Next fwrite lands `len - short_by` bytes: a real short write, not a fake rc.
void push_part_test_short_next_write(size_t short_by);

/// 连续 `times` 次 fwrite 各短写 `short_by` 字节——造「欠账」需要连败两次：
/// 第一次进重试，第二次才把整包推进欠账槽。
/// Short by `short_by` for `times` writes: parking a packet needs two failures
/// in a row - the first triggers the retry, the second parks it.
void push_part_test_short_writes(size_t short_by, int times);

/// 最近一次 truncate 的参数（`rewind` 的证据）。/ The last truncate argument.
long push_part_test_last_truncate(void);


#endif

#endif // PUSH_PART_H
