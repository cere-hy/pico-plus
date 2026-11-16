/* SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 仿生阅读 · 纯逻辑层实现（不引用任何绘制/字体/RTOS 符号）。
 * 判据 v2（2026-11-16 备案）：加粗前半截，ceil(len/2) 整数实现 `(len + 1) / 2`，无上限。
 *
 * Bionic reading: pure logic implementation. Rule v2 bolds the leading half,
 * ceil(len / 2) as the integer form (len + 1) / 2, with no cap.
 */
#include "ui_bionic.h"

/* UTF-8 首字节 -> 字节数；非法首字节按 1 字节处理（与断行器同判据）。
 * Lead byte -> sequence length; illegal leads read as one byte, as in the wrapper. */
static size_t utf8_step(const char *p) {
    const unsigned char c = (unsigned char)p[0];
    size_t step = c < 0x80u ? 1u
                : (c & 0xe0u) == 0xc0u ? 2u
                : (c & 0xf0u) == 0xe0u ? 3u
                : (c & 0xf8u) == 0xf0u ? 4u : 1u;
    size_t k = 1;
    while (k < step && p[k] && ((unsigned char)p[k] & 0xc0u) == 0x80u) ++k;
    if (k < step) step = 1u;
    return step;
}

static uint32_t utf8_cp(const char *p, size_t step) {
    const unsigned char *s = (const unsigned char *)p;
    if (step == 1) return s[0];
    if (step == 2) return (uint32_t)((s[0] & 0x1Fu) << 6) | (s[1] & 0x3Fu);
    if (step == 3) return (uint32_t)((s[0] & 0x0Fu) << 12)
                          | ((uint32_t)(s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu);
    return (uint32_t)((s[0] & 0x07u) << 18)
         | ((uint32_t)(s[1] & 0x3Fu) << 12)
         | ((uint32_t)(s[2] & 0x3Fu) << 6) | (s[3] & 0x3Fu);
}

/* 词字符：仅 ASCII 字母数字（定版：只作用英文词，CJK 直通）。
 * Word characters: ASCII letters and digits only (English words only; CJK passes through). */
static bool alnum_cp(uint32_t cp) {
    return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')
        || (cp >= '0' && cp <= '9');
}

/* 词内标点：ASCII 撇号、连字符与弯撇号 U+2019（reader's / reader’s / state-of-the-art）。
 * Intra-word punctuation: ASCII apostrophe, hyphen and U+2019. */
static bool intra_cp(uint32_t cp) {
    return cp == '\'' || cp == '-' || cp == 0x2019u;
}

int br_fixation_len(int len) {
    if (len <= 0) return 0;
    /* 前半截：ceil(len / 2)，C 整数除法直接成立。len=1 时 n=1（唯一 n==len 情形），
     * len>=2 恒有后半截留白，旧「短词留 1 字符」条款被公式天然覆盖。 */
    return (len + 1) / 2;
}

void br_bold_reset(br_bold_state_t *st) {
    if (!st) return;
    st->left = 0;
    st->pos = 0;
}

/* 从 p 起向前看，数出以首个词字符开头的整个词的字符数。
 * Count the whole word starting at p (first character already consumed by caller). */
static int word_length_from(const char *p) {
    int len = 0;
    while (p && *p) {
        const size_t step = utf8_step(p);
        const uint32_t cp = utf8_cp(p, step);
        if (alnum_cp(cp)) {
            ++len;
            p += step;
            continue;
        }
        if (intra_cp(cp)) {
            /* 词内标点只在紧跟字母数字时计入词长。/ Intra punctuation must lead to a word char. */
            const char *next = p + step;
            if (!*next) break;
            const size_t next_step = utf8_step(next);
            if (!alnum_cp(utf8_cp(next, next_step))) break;
            ++len;
            p += step;
            continue;
        }
        break;
    }
    return len;
}

bool br_bold_feed(br_bold_state_t *st, uint32_t cp, const char *text_after,
                  bool *new_word) {
    if (new_word) *new_word = false;
    if (!st) return false;

    if (alnum_cp(cp)) {
        if (st->pos == 0) {
            /* 新词：先看完整词长再定加粗数，保证同一词内判定不漂移。 */
            const int word_len = 1 + word_length_from(text_after);
            st->left = br_fixation_len(word_len);
            st->pos = 1;
            if (new_word) *new_word = true;
            return st->left >= 1;
        }
        ++st->pos;
        return st->pos <= st->left;
    }

    if (st->pos > 0 && intra_cp(cp) && text_after && *text_after) {
        const size_t step = utf8_step(text_after);
        if (alnum_cp(utf8_cp(text_after, step))) {
            ++st->pos;
            return st->pos <= st->left;
        }
    }

    /* 空白、标点、CJK：终止当前词。/ Whitespace, punctuation, CJK: end the word. */
    st->pos = 0;
    st->left = 0;
    return false;
}
