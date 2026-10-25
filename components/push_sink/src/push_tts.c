/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：TTS 页文本帧切分计划（gen26.5 STATUS 0x19）。实现见 push_tts.h 的契约：
 * 只切不编、不截断 UTF-8、空文本一帧。宿主对表在 tools/push_sink_host_test.c
 * （ASCII / 中文 / 恰好贴预算 / 续字节处收刀全钉）。
 * English: the TTS page-text frame plan (gen26.5 STATUS 0x19). See push_tts.h for
 * the contract: split only, never encode, never cut UTF-8, empty text is one
 * frame. Host vectors in tools/push_sink_host_test.c.
 */

#include "push_tts.h"

size_t push_tts_utf8_advance(const char *s, size_t len, size_t budget) {
    if (!s || len == 0 || budget == 0) return 0;
    size_t n = 0;
    while (n < len && n < budget) {
        // 续字节（10xxxxxx）不是字符起点：只可能在字符内部被跨过，不能在此起刀。
        // A continuation byte (10xxxxxx) never starts a character: it may only be
        // crossed inside a character, never cut at.
        const unsigned char c = (unsigned char)s[n];
        if ((c & 0xc0) == 0x80) break; // 落点为续字节：到此为止（上一個字符已收尾）
        size_t char_len = 1;
        if ((c & 0x80u) == 0x00u) char_len = 1;        // 0xxxxxxx ASCII
        else if ((c & 0xe0u) == 0xc0u) char_len = 2;   // 110xxxxx
        else if ((c & 0xf0u) == 0xe0u) char_len = 3;   // 1110xxxx
        else if ((c & 0xf8u) == 0xf0u) char_len = 4;   // 11110xxx
        else break;                                     // 非法前导字节：不当字符起点
        if (n + char_len > len || n + char_len > budget) break; // 整字符放不下：收刀
        n += char_len;
    }
    return n;
}

size_t push_tts_frame_count(const char *text, size_t len) {
    if (!text || len == 0) return 1; // 空文本一帧（调用方决定发不发）/ empty = one frame
    size_t count = 0, off = 0;
    while (off < len) {
        const size_t step = push_tts_utf8_advance(text + off, len - off, PUSH_TTS_FRAME_BUDGET);
        // step 恒 ≥1（首字节合法时至少吃掉一个字符；非法前导字节也会被 break 成 0——
        // 此时强行前进 1 字节保进度，帧内坏字节由对端按 UTF-8 规则处理）。
        // step is >= 1 for a legal lead byte; an illegal lead byte breaks to 0, so
        // force 1 byte of progress (the peer handles the bad byte per UTF-8 rules).
        off += step ? step : 1;
        ++count;
    }
    return count;
}

bool push_tts_frame_at(const char *text, size_t len, size_t i,
                       const char **slice, size_t *slice_len) {
    if (!text || len == 0) {
        // 空文本只有第 0 帧，且是空切片。/ Empty text has only frame 0, an empty slice.
        if (i != 0) return false;
        if (slice) *slice = text;
        if (slice_len) *slice_len = 0;
        return true;
    }
    size_t off = 0, idx = 0;
    while (off < len) {
        const size_t step = push_tts_utf8_advance(text + off, len - off, PUSH_TTS_FRAME_BUDGET);
        const size_t take = step ? step : 1;
        if (idx == i) {
            if (slice) *slice = text + off;
            if (slice_len) *slice_len = take;
            return true;
        }
        off += take;
        ++idx;
    }
    return false; // i 越界 / out of range
}
