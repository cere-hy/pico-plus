/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 英文音节断点：Liang 算法 + TeX `hyph-en-us` 模式表。
 * English hyphenation: Liang's algorithm over the TeX `hyph-en-us` pattern table.
 *
 * 为什么不在排版器里现算：断点是**词的性质**，与字号、版心、字体都无关。把它做成
 * 独立的一层，宿主能直接测（`tools/hyphen_host_test.c`），排版器只负责「哪个断点
 * 装得下」。同理，表是数据不是代码——生成器与校验器都在 tools/ 下。
 * Break points are a property of the word, not of the face or the measure, so this is
 * its own layer: host-testable, with the layout only choosing which point fits.
 *
 * 复杂度：逐位置查最长 8 字母的子串，每个子串一次二分。只在**行尾那个装不下的词**
 * 上调用（一行至多一次），不在建页时遍历全页单词。
 * Cost: binary search per candidate substring, invoked at most once per line - only for
 * the word that does not fit - never for every word on the page.
 */

#include "hyphen.h"

#include <string.h>

#include "hyph_en_us_data.h"

// 表里的切长是生成时写进数据的；与对外常量必须一致，否则断点会在两个层上打架。
// The table carries its own minima; they must agree with the public constants.
_Static_assert(HYPH_EN_US_LEFT_MIN == HYPH_LEFT_MIN, "left min drift");
_Static_assert(HYPH_EN_US_RIGHT_MIN == HYPH_RIGHT_MIN, "right min drift");

// 小写化只对 ASCII A-Z 生效：表是 ASCII 的，非 ASCII 词直接拒绝而不是猜。
// Lowercase ASCII only: the table is ASCII, so a non-ASCII word is refused, not guessed.
static bool ascii_lower(const char *word, size_t len, char *out, size_t cap) {
    if (len + 3 > cap) return false;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)word[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        if (!((c >= 'a' && c <= 'z') || c == '\'')) return false;
        out[i + 1] = (char)c; // 前面留一个 '.' 占位（词首边界）
    }
    out[0] = '.';
    out[len + 1] = '.';
    out[len + 2] = 0;
    return true;
}

// 与表中第 idx 条模式比较：先按长度，再按字节。表按字典序排好（生成器保证），
// '.' 的字节值小于字母，与 Python 的排序一致。
// Compare against pattern #idx: length first, then bytes. The table is sorted by the
// generator and '.' sorts before letters, matching Python's ordering.
static int pattern_cmp(const char *s, size_t len, uint32_t idx) {
    const uint32_t off = hyph_en_us_off[idx];
    const uint32_t end = hyph_en_us_off[idx + 1];
    const size_t plen = (size_t)(end - off);
    const size_t n = len < plen ? len : plen;
    const int c = memcmp(s, hyph_en_us_blob + off, n);
    if (c) return c;
    if (len == plen) return 0;
    return len < plen ? -1 : 1;
}

// 精确查一条模式，返回其下标或 -1。二分，不建运行时索引：表在 flash 里只读，
// 建索引要多一份 RAM，而调用频率是「每行至多一次」。
// Exact lookup, -1 when absent. Binary search over the read-only table: an index would
// cost RAM for a call rate of at most once per line.
static int32_t pattern_find(const char *s, size_t len) {
    uint32_t lo = 0, hi = HYPH_EN_US_PATTERN_COUNT;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        const int c = pattern_cmp(s, len, mid);
        if (c == 0) return (int32_t)mid;
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return -1;
}

// 例外表：14 条，线性比对比任何索引都便宜，且不必排序。
// Exceptions: 14 entries, linear is cheaper than any index and needs no ordering.
// 返回例外表下标；-1 表示「不是例外」。**不能用 mask==0 代表缺席**：`present`
// 就是一条 mask 为 0 的例外（TeX 用它禁止 pre-sent 这种断法），把它当缺席会掉回
// 模式表并断出模式允许的位置——正好是这条例外要防的。
// Index into the exception table, -1 when absent. A zero mask is a *valid* exception
// (`present` forbids pre-sent); treating it as absent falls back to the patterns and
// produces exactly the break the exception exists to prevent.
static int32_t exception_index(const char *word, size_t len) {
    for (uint32_t i = 0; i < HYPH_EN_US_EXCEPTION_COUNT; ++i) {
        const uint32_t off = hyph_en_us_exc_off[i];
        const uint32_t end = hyph_en_us_exc_off[i + 1];
        if ((size_t)(end - off) == len && memcmp(hyph_en_us_exc_blob + off, word, len) == 0)
            return (int32_t)i;
    }
    return -1;
}

bool hyph_points(const char *word, size_t len, hyph_points_t *out) {
    if (!out) return false;
    out->count = 0;
    if (!word || len < HYPH_EN_US_LEFT_MIN + HYPH_EN_US_RIGHT_MIN) return false;
    if (len > 63) return false; // 63 字节 = 两头加点后的栈缓冲上限 / stack buffer bound

    char buf[67];
    if (!ascii_lower(word, len, buf, sizeof(buf))) return false;

    // 例外优先，且**完全替代**模式：TeX 的 \hyphenation 就是这么定义的。
    // Exceptions win and replace the patterns entirely, as \hyphenation is defined to do.
    uint8_t score[66];
    memset(score, 0, sizeof(score));
    const int32_t exc = exception_index(buf + 1, len);
    if (exc >= 0) {
        const uint32_t mask = hyph_en_us_exc_mask[exc];
        for (size_t p = 1; p < len; ++p)
            if ((mask >> (p - 1)) & 1u) score[p + 1] = 1; // 奇数 = 允许 / odd allows
    } else {
        const size_t n = len + 2; // ".word." 的长度
        for (size_t i = 0; i + 1 < n; ++i) {
            const size_t maxl = n - i < HYPH_EN_US_MAX_PATTERN ? n - i : HYPH_EN_US_MAX_PATTERN;
            for (size_t l = 1; l <= maxl; ++l) {
                const int32_t idx = pattern_find(buf + i, l);
                if (idx < 0) continue;
                const uint32_t packed = hyph_en_us_dig[idx];
                for (size_t k = 0; k <= l; ++k) {
                    const uint8_t d = (uint8_t)((packed >> (4 * k)) & 0xF);
                    if (d > score[i + k]) score[i + k] = d;
                }
            }
        }
    }

    // 断点条件：该位置分为奇数，且左右都满足最小切长。score 的下标比词内位置多 1
    // （词首那个 '.' 占了 0 号位）。
    // A break needs an odd score at the position plus the left/right minima. The score
    // index is shifted by one because of the leading '.'.
    for (size_t p = HYPH_EN_US_LEFT_MIN; p + HYPH_EN_US_RIGHT_MIN <= len; ++p) {
        if ((score[p + 1] & 1u) == 0) continue;
        if (out->count == HYPH_MAX_POINTS) break;
        out->pos[out->count++] = (uint8_t)p;
    }
    return out->count > 0;
}
