/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：0x08 释义分帧的重组器（纯逻辑、无 ESP-IDF 依赖 → 宿主可测）。
 * English: reassembler for the 0x08 segmented definition (pure logic, no ESP-IDF,
 * so the host can test it).
 *
 * 语义钉死 / Semantics (frozen):
 *   1. **seq == 0 = 新条开始**（与 STATUS 0x19「seq==0 即新页」同款设计点）：
 *      无条件重置并以本帧起点；手机重发整条时设备不会把新旧两版拼在一起。
 *      **seq == 0 starts a new item**: always resets and takes this frame as the
 *      head, so a re-sent definition never splices onto the previous one.
 *   2. 非 0 帧必须「序号连得上 + 同一个词 + total 一致」，否则 REJECT 并重置——
 *      宁可整条作废也不出半截释义（诚实优于假成功，沿用 gen26 既有纪律）。
 *      A later frame must continue the sequence on the same word with the same
 *      total; otherwise REJECT and reset - a whole item is dropped rather than
 *      surfacing a half definition (honesty over fake success, as in gen26).
 *   3. 超出 AI_SEGASM_MAX 即 OVERFLOW 并重置（不截断：截断 = 假成功）。
 *      Overflowing AI_SEGASM_MAX yields OVERFLOW and a reset (never truncate:
 *      truncation would be a fake success).
 *   4. 重组按**字节流顺序拼接**，拼完整体当 UTF-8 用；单帧（total==1）立即完成，
 *      语义等价 0x07。Frames concatenate as a byte stream decoded as a whole;
 *      a single frame (total==1) completes at once, equivalent to 0x07.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "push_proto.h" // PUSH_AI_WORD_MAX

#ifdef __cplusplus
extern "C" {
#endif

#define AI_SEGASM_MAX 8192u ///< 重组缓冲上限（字节）= 传输槽上界，两者必须同数 / reassembly cap, must equal the transport slot
// 26.11.3-C2（门梯第四道）：重组器是链路上**最后一个**能无声吃掉答案的地方——
// 低于传输槽就会在 4096 处把 8 KB 的段落截在句子中间，而且没有任何告示，比 1152
// 时代的「带省略号截断」更糟。故它不是「够用就行」的缓冲，而是门梯上的一档：
// 任何一档低于 8192，8192 就是假门。
// 26.11.3-C2 (the fourth gate in the ladder): the reassembler is the last place that can
// silently eat an answer. Below the transport slot it would cut an 8 KB passage mid
// sentence with no notice at all - worse than the 1152-era ellipsis. It is a rung, not
// a "big enough" buffer: any rung under 8192 makes 8192 a fake ceiling.
_Static_assert(AI_SEGASM_MAX >= PUSH_AI_DEF_MAX,
               "26.11.3-C2 gate ladder: the reassembler must not be narrower than the slot");

typedef struct {
    char word[PUSH_AI_WORD_MAX]; ///< 正在重组的词 / word being reassembled
    char buf[AI_SEGASM_MAX];     ///< 拼接缓冲 / concatenation buffer
    size_t len;                  ///< 已拼字节数 / bytes joined so far
    uint16_t total;              ///< 期望总帧数；0 = 无在建条目 / expected total; 0 = idle
    uint16_t next;               ///< 期望的下一个 seq / the next seq expected
    bool active;                 ///< 是否有在建条目 / an item is in flight
} ai_segasm_t;

typedef enum {
    AI_SEGASM_PENDING = 0, ///< 还缺帧 / still missing frames
    AI_SEGASM_DONE,        ///< 收齐，可取全文 / complete, take the text
    AI_SEGASM_REJECT,      ///< 乱序/词变了/total 变了（已重置）/ out of order (reset)
    AI_SEGASM_OVERFLOW,    ///< 超上限（已重置）/ over the cap (reset)
} ai_segasm_status_t;

/// 清空重组器（断开、开新条、出错后都用它）。/ Clear it (disconnect, new item, error).
void ai_segasm_reset(ai_segasm_t *a);

/// 喂一帧。完成/出错时 out_text/out_len 描述结果；PENDING 时不要读。
/// Feed a frame. On DONE, out_text/out_len describe the text; do not read on PENDING.
ai_segasm_status_t ai_segasm_feed(ai_segasm_t *a, const push_ai_seg_t *s,
                                  const char **out_text, size_t *out_len);

#ifdef __cplusplus
}
#endif
