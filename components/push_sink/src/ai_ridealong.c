/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：ai_ridealong.h 的实现。纯 C11，小端助手本地化（不依赖 push_proto.c 的
 * 静态函数），宿主可全边覆盖。去重入队、紧凑存储、出队前移，逻辑全在扁平缓冲
 * 上，不碰文件/堆/射频。
 * English: implementation of ai_ridealong.h. Pure C11 with local little-endian
 * helpers (independent of push_proto.c's static functions) so the host test
 * covers every path. Deduped enqueue, compact storage, forward-shift on dequeue;
 * everything lives on the flat buffer - no files, no heap, no radio.
 */

#include "ai_ridealong.h"

#include <string.h>

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

size_t ai_raloc_buf_size(uint16_t cap) {
    return (size_t)6 + (size_t)cap * (1u + (size_t)PUSH_AI_WORD_MAX);
}

bool ai_raloc_init(uint8_t *buf, size_t buf_len, uint16_t cap) {
    if (!buf) return false;
    if (buf_len < ai_raloc_buf_size(cap)) return false;
    if (get_u16(buf) == AI_RALOC_MAGIC) {
        // 续载：cap 必须一致，count 不得越界。/ Resume: cap must match, count in range.
        if (get_u16(buf + 2) != cap) return false;
        if (get_u16(buf + 4) > cap) return false;
        return true;
    }
    memset(buf, 0, buf_len);
    put_u16(buf, AI_RALOC_MAGIC);
    put_u16(buf + 2, cap);
    put_u16(buf + 4, 0);
    return true;
}

uint16_t ai_raloc_cap(const uint8_t *buf) {
    return (buf && get_u16(buf) == AI_RALOC_MAGIC) ? get_u16(buf + 2) : 0;
}

uint16_t ai_raloc_count(const uint8_t *buf) {
    return (buf && get_u16(buf) == AI_RALOC_MAGIC) ? get_u16(buf + 4) : 0;
}

// 跳过已有条目，找队尾写入偏移。/ Skip existing entries to find the tail offset.
static size_t raloc_tail(const uint8_t *buf) {
    size_t off = 6;
    const uint16_t count = get_u16(buf + 4);
    for (uint16_t i = 0; i < count; ++i) off += 1u + (size_t)buf[off];
    return off;
}

// 条目 i 的偏移。/ Offset of entry i.
static size_t raloc_entry_off(const uint8_t *buf, uint16_t i) {
    size_t off = 6;
    for (uint16_t k = 0; k < i; ++k) off += 1u + (size_t)buf[off];
    return off;
}

bool ai_raloc_enqueue(uint8_t *buf, size_t buf_len, const char *word) {
    if (!buf || !word) return false;
    if (get_u16(buf) != AI_RALOC_MAGIC) return false;
    const uint16_t cap = get_u16(buf + 2);
    const uint16_t count = get_u16(buf + 4);
    if (count >= cap) return false; // 满 / full
    // 去重：遍历已有条目。/ Dedup: scan existing entries.
    for (uint16_t i = 0; i < count; ++i) {
        const size_t eo = raloc_entry_off(buf, i);
        const uint8_t wlen = buf[eo];
        if ((size_t)wlen == strlen(word) && memcmp(buf + eo + 1, word, wlen) == 0) return false;
    }
    size_t wlen = strlen(word);
    if (wlen >= PUSH_AI_WORD_MAX) wlen = PUSH_AI_WORD_MAX - 1; // 截断保 0 / truncate, keep NUL
    const size_t tail = raloc_tail(buf);
    if (tail + 1u + wlen > buf_len) return false;
    buf[tail] = (uint8_t)wlen;
    memcpy(buf + tail + 1u, word, wlen);
    // 槽位补 0（比上一条短时清残留）。/ Zero-fill the slot (clears leftovers).
    memset(buf + tail + 1u + wlen, 0, (size_t)(PUSH_AI_WORD_MAX - wlen));
    put_u16(buf + 4, count + 1u);
    return true;
}

bool ai_raloc_peek(const uint8_t *buf, char *word_out, size_t word_cap) {
    if (!buf || get_u16(buf) != AI_RALOC_MAGIC) return false;
    if (get_u16(buf + 4) == 0) return false;
    const size_t eo = raloc_entry_off(buf, 0);
    const uint8_t wlen = buf[eo];
    if (word_out && word_cap) {
        const size_t cp = wlen < word_cap - 1u ? (size_t)wlen : word_cap - 1u;
        memcpy(word_out, buf + eo + 1u, cp);
        word_out[cp] = 0;
    }
    return true;
}

bool ai_raloc_dequeue(uint8_t *buf, char *word_out, size_t word_cap) {
    if (!buf || get_u16(buf) != AI_RALOC_MAGIC) return false;
    const uint16_t count = get_u16(buf + 4);
    if (count == 0) return false;
    const size_t eo = raloc_entry_off(buf, 0);
    const uint8_t wlen = buf[eo];
    if (word_out && word_cap) {
        const size_t cp = wlen < word_cap - 1u ? (size_t)wlen : word_cap - 1u;
        memcpy(word_out, buf + eo + 1u, cp);
        word_out[cp] = 0;
    }
    // 把队首之后的条目整体前移，保持紧凑。/ Shift the rest forward to stay compact.
    const size_t entry_len = 1u + (size_t)wlen;
    const size_t tail = raloc_tail(buf);
    const size_t rest = tail - (eo + entry_len);
    memmove(buf + 6, buf + eo + entry_len, rest);
    memset(buf + 6u + rest, 0, tail - (6u + rest));
    put_u16(buf + 4, count - 1u);
    return true;
}

void ai_raloc_clear(uint8_t *buf) {
    if (!buf || get_u16(buf) != AI_RALOC_MAGIC) return;
    put_u16(buf + 4, 0);
    const size_t total = ai_raloc_buf_size(get_u16(buf + 2));
    if (total > 6u) memset(buf + 6, 0, total - 6u);
}
