/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：0x08 释义分帧的重组器实现（纯逻辑，宿主可测）。
 * English: reassembler implementation for 0x08 segments (pure logic, host-testable).
 */
#include "ai_segasm.h"

#include <stdio.h>
#include <string.h>

void ai_segasm_reset(ai_segasm_t *a) {
    if (!a) return;
    memset(a, 0, sizeof(*a));
}

ai_segasm_status_t ai_segasm_feed(ai_segasm_t *a, const push_ai_seg_t *s,
                                  const char **out_text, size_t *out_len) {
    if (!a || !s) return AI_SEGASM_REJECT;

    // ① seq==0 永远开新条：手机重发整条时不会与上一版拼接。
    // seq==0 always starts a new item: a re-send never splices onto the old one.
    if (s->seg_idx == 0) {
        ai_segasm_reset(a);
        size_t n = strnlen(s->seg, PUSH_AI_SEG_MAX);
        if (n >= AI_SEGASM_MAX) {
            ai_segasm_reset(a);
            return AI_SEGASM_OVERFLOW;
        }
        memcpy(a->buf, s->seg, n);
        a->len = n;
        a->buf[n] = 0;
        snprintf(a->word, sizeof(a->word), "%s", s->word);
        a->total = s->seg_total;
        a->next = 1;
        a->active = true;
    } else {
        // ② 续帧必须连得上、同一个词、total 一致，否则整条作废。
        // A continuation must fit the sequence, the word and the total.
        if (!a->active || s->seg_idx != a->next || s->seg_total != a->total ||
            strncmp(a->word, s->word, PUSH_AI_WORD_MAX) != 0) {
            ai_segasm_reset(a);
            return AI_SEGASM_REJECT;
        }
        size_t n = strnlen(s->seg, PUSH_AI_SEG_MAX);
        if (a->len + n + 1u > AI_SEGASM_MAX) {
            ai_segasm_reset(a);
            return AI_SEGASM_OVERFLOW; // 不截断：截断就是假成功 / never truncate
        }
        memcpy(a->buf + a->len, s->seg, n);
        a->len += n;
        a->buf[a->len] = 0;
        a->next = (uint16_t)(s->seg_idx + 1u);
    }

    // ③ 收齐即完成（total==1 的帧在这里立即完成，语义等价 0x07）。
    // Complete as soon as the last frame lands (total==1 finishes at once).
    if (a->next >= a->total) {
        if (out_text) *out_text = a->buf;
        if (out_len) *out_len = a->len;
        return AI_SEGASM_DONE;
    }
    return AI_SEGASM_PENDING;
}
