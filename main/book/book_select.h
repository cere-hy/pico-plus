/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：框选（M5-A）纯状态机：只认字节区间，不绘制、不排版、不读文件。
 * English: Box-selection (M5-A) pure state machine: byte ranges only, no drawing,
 * no layout calls, no file access.
 *
 * 冻结：单页内选定；两次点按定界（点首、点尾），禁止拖选；所有退出路径都只经
 *       box_select_cancel()。本模块不认识屏幕坐标，命中由调用方用
 *       book_layout_hit_test() 换成词区间后传入——因此可在宿主上无桩测试。
 * Frozen: single page only; two taps define the range (head then tail), dragging is
 * never a selection gesture; every exit path funnels through box_select_cancel().
 * The module knows nothing about screen coordinates: the caller converts a tap with
 * book_layout_hit_test() and passes word spans in, so it is host-testable without
 * any stub.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// 30 s 无有效点按即放弃本次框选。/ Abandon the selection after 30 s without a valid tap.
#define BOX_SELECT_TIMEOUT_MS 30000

typedef enum {
    BOX_STATE_OFF = 0,  ///< 常态：不吞事件、不绘制 / idle: swallows nothing, draws nothing
    BOX_STATE_ARM,      ///< 已进入，等首点 / entered, waiting for the head tap
    BOX_STATE_TAIL,     ///< 首点已定，等尾点 / head fixed, waiting for the tail tap
    BOX_STATE_READY,    ///< 选区完成，等动作栏 / range complete, action bar is live
} box_state_t;

typedef struct {
    box_state_t state;
    size_t page;        ///< 选区所在页（单页，跨页结构上不可能）/ page of the range (single page)
    size_t anchor_ws;   ///< 首点词区间（保持原样，供反向选交换）/ head word span, kept for reverse swaps
    size_t anchor_we;
    size_t off_start;   ///< 选区起（含）/ range start, inclusive
    size_t off_end;     ///< 选区止（不含）/ range end, exclusive
    int64_t deadline_ms;///< 超时时刻；0 表示未起算 / expiry instant; 0 means not armed yet
} box_selection_t;

/// 进入框选态（等待首点）。重复调用等价于重来一次。/ Enter the mode (waiting for a head tap); calling again restarts.
void box_select_begin(box_selection_t *sel, size_t page, int64_t now_ms);

/// 提交一次点按命中的整词区间 [ws, we)。返回 true 表示该次点按被接受。
/// 首点定锚；尾点在锚前则交换（含完全重叠），在锚后则取并集——选区是区间，与先后无关。
/// Submit a tap that hit the whole-word span [ws, we). True means the tap was accepted.
/// The first call sets the anchor; a tail before the anchor swaps (overlap included), a
/// tail after it unions — a range is an interval, order of taps does not matter.
bool box_select_tap(box_selection_t *sel, size_t ws, size_t we, int64_t now_ms);

/// 按句吸附（26.9.12）：把点中的词/字扩展成整句，**纯字节运算**，不碰排版与坐标。
/// 句界 = 中英文句末标点（。！？；…．以及 . ! ? ;）与换行；英文的 . ! ? ; 后面必须
/// 跟空白或页尾才算（否则 3.14、Mr. 会被切断，一期不做缩写特判，接受误扩）。
/// dir < 0：从 ws 向前找最近句界之后，作为句首（跳过行首缩进）；
/// dir > 0：从 we 向后找最近句界之后（含该标点），作为句末（换行本身不含）。
/// [page_start, page_end) 是本页范围：句子跨页时按页截断（冻结的不变量 R2 不破）。
/// 返回 false 表示无法吸附（参数非法或结果不含原区间），调用方保留原词区间即可。
/// Sentence snapping (26.9.12): widen the tapped word into a whole sentence, **byte
/// arithmetic only** - no layout, no coordinates. Boundaries are CJK/ASCII terminators
/// (。！？；…． . ! ? ;) and newlines; an ASCII terminator only counts when followed by
/// whitespace or the page end so 3.14 and Mr. are not cut (no abbreviation special-casing
/// in phase one - over-extension is accepted). dir < 0 walks back from ws to just after
/// the previous boundary (skipping leading indent); dir > 0 walks forward from we to just
/// after the next boundary, punctuation included, a newline itself excluded.
/// [page_start, page_end) is the page: a sentence crossing a page break is clipped there,
/// so frozen invariant R2 still holds. False means "cannot snap" (bad arguments, or the
/// result does not contain the tapped span); the caller then keeps the word span.
bool box_select_snap_sentence(const char *text, size_t text_len,
                              size_t page_start, size_t page_end,
                              size_t ws, size_t we, int dir,
                              size_t *out_s, size_t *out_e);

/// 超时判定：到点返回 true 并把状态打回 OFF（调用方随后整页重绘）。
/// Expiry: true when the deadline passed; the state drops to OFF and the caller repaints.
bool box_select_expired(box_selection_t *sel, int64_t now_ms);

/// 唯一的退出路径：清空整结构，幂等。/ The single exit path: clears everything, idempotent.
void box_select_cancel(box_selection_t *sel);

/// 是否处于框选态（吞事件的判据）。/ Whether the mode is active (the event-swallowing test).
bool box_select_active(const box_selection_t *sel);
/// 选区是否已完成（动作栏可用的判据）。/ Whether the range is complete (action bar may run).
bool box_select_ready(const box_selection_t *sel);

/// 把选区原文按 UTF-8 原样拷进 buf：不折行、不去空白、不截断词。
/// 返回写入字节数（不含 NUL）；cap 不足或区间非法时返回 0 且 buf 不写（不返回半截文本）。
/// Copy the selected source bytes verbatim: no re-wrapping, no whitespace trimming and no
/// word cutting. Returns bytes written without the NUL; a short cap or an invalid range
/// returns 0 and leaves buf untouched (never a half-written selection).
size_t box_select_copy_text(const box_selection_t *sel, const char *text, size_t text_len,
                            char *buf, size_t cap);

/// 选区覆盖的字节数（不含 NUL）；未完成时为 0。/ Bytes covered by the range; 0 unless complete.
size_t box_select_length(const box_selection_t *sel);
