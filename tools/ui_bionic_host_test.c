/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 仿生阅读 · 纯逻辑宿主测试（零 stub、零平台依赖）。
 *
 * 覆盖两件事：
 *  1. 50% 公式（v2，2026-11-16 备案）的边界用例：1/2/3/4/5/6/7/10/11/12/15/16/100；
 *  2. 切词状态机（英文词、撇号、连字符、弯撇号、数字、CJK 直通、词段计数）。
 *
 * Bionic reading host test: the v2 50% boundaries (ceil(len/2), uncapped) plus the
 * tokenizer state machine. Pure logic, no stubs.
 */
#include <stdio.h>
#include <string.h>

#include "ui_bionic.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (cond) {                                                             \
            ++g_pass;                                                           \
        } else {                                                                \
            ++g_fail;                                                           \
            printf("FAIL: %s  (line %d)\n", (msg), __LINE__);                   \
        }                                                                       \
    } while (0)

static size_t cp_step(const char *p) {
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

static uint32_t cp_at(const char *p, size_t step) {
    const unsigned char *s = (const unsigned char *)p;
    if (step == 1) return s[0];
    if (step == 2) return (uint32_t)((s[0] & 0x1Fu) << 6) | (s[1] & 0x3Fu);
    if (step == 3) return (uint32_t)((s[0] & 0x0Fu) << 12)
                          | ((uint32_t)(s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu);
    return (uint32_t)((s[0] & 0x07u) << 18)
         | ((uint32_t)((s[1] & 0x3Fu) << 12)
         | ((uint32_t)((s[2] & 0x3Fu) << 6) | (s[3] & 0x3Fu)));
}

/* 把整段文本跑一遍状态机，输出每个码点的 B / . 序列，并统计词段数。 */
static void run_bold(const char *text, char *out, size_t cap, int *segments) {
    br_bold_state_t st;
    br_bold_reset(&st);
    size_t n = 0;
    int segs = 0;
    const char *p = text;
    while (*p && n + 1 < cap) {
        const size_t step = cp_step(p);
        const uint32_t cp = cp_at(p, step);
        bool fresh = false;
        const bool bold = br_bold_feed(&st, cp, p + step, &fresh);
        if (fresh) ++segs;
        out[n++] = bold ? 'B' : '.';
        p += step;
    }
    out[n] = 0;
    if (segments) *segments = segs;
}

static void expect_bold(const char *text, const char *want, int want_segments) {
    char got[256];
    int segs = -1;
    run_bold(text, got, sizeof(got), &segs);
    if (strcmp(got, want) || segs != want_segments) {
        printf("FAIL: \"%s\" want %s/%d got %s/%d\n", text, want, want_segments, got, segs);
        ++g_fail;
        return;
    }
    ++g_pass;
}

int main(void) {
    /* ---- 一、50% 公式边界（v2：ceil(len/2)，无上限）---- */
    CHECK(br_fixation_len(0) == 0, "len=0 -> 0");
    CHECK(br_fixation_len(-3) == 0, "len=-3 -> 0");
    CHECK(br_fixation_len(1) == 1, "len=1 -> 1 (唯一 n==len)");
    CHECK(br_fixation_len(2) == 1, "len=2 -> 1");
    CHECK(br_fixation_len(3) == 2, "len=3 -> 2");
    CHECK(br_fixation_len(4) == 2, "len=4 -> 2");
    CHECK(br_fixation_len(5) == 3, "len=5 -> 3");
    CHECK(br_fixation_len(6) == 3, "len=6 -> 3");
    CHECK(br_fixation_len(7) == 4, "len=7 -> 4");
    CHECK(br_fixation_len(10) == 5, "len=10 -> 5");
    CHECK(br_fixation_len(11) == 6, "len=11 -> 6");
    CHECK(br_fixation_len(12) == 6, "len=12 -> 6");
    CHECK(br_fixation_len(15) == 8, "len=15 -> 8");
    CHECK(br_fixation_len(16) == 8, "len=16 -> 8 (无上限，不夹取)");
    CHECK(br_fixation_len(100) == 50, "len=100 -> 50 (无上限)");

    /* 逐长度扫一遍：n 恒在 [1,len]，len>=2 时恒小于词长（后半截必留白）。 */
    for (int len = 1; len <= 64; ++len) {
        const int n = br_fixation_len(len);
        CHECK(n >= 1 && n <= len, "n in 1..len");
        if (len >= 2) CHECK(n < len, "n < len (back half kept regular)");
        CHECK(n == (len + 1) / 2, "n == ceil(len/2)");
    }

    /* ---- 二、切词状态机 ---- */
    expect_bold("Hello", "BBB..", 1);                 /* len 5 -> 3 */
    expect_bold("reading", "BBBB...", 1);             /* len 7 -> 4 */
    expect_bold("state-of-the-art", "BBBBBBBB........", 1); /* len 16 -> 8，整词一段 */
    expect_bold("reader's", "BBBB....", 1);           /* 撇号词内，len 8 -> 4 */
    expect_bold("reader\xE2\x80\x99s", "BBBB....", 1);/* U+2019 弯撇号，len 8 -> 4 */
    expect_bold("don't", "BBB..", 1);                 /* len 5 -> 3 */
    expect_bold("A", "B", 1);                         /* 单字母全粗 */
    expect_bold("a1b2c", "BBB..", 1);                 /* 数字算词字符，len 5 -> 3 */
    expect_bold("Hello world", "BBB...BBB..", 2);     /* 空格切词，两段 */
    expect_bold("ab-cd", "BBB..", 1);                 /* 连字符词内，len 5 -> 3 */
    expect_bold("end- start", "BB...BBB..", 2);       /* 词尾连字符后接空格：不成词内 */
    expect_bold("-lead", ".BB..", 1);                 /* 行首连字符不是词首 */
    expect_bold("中文abc", "..BB.", 1);               /* CJK 直通，不参与加粗 */
    expect_bold("中文", "..", 0);                     /* 纯 CJK 零加粗零词段 */
    expect_bold("...", "...", 0);                     /* 纯标点 */
    expect_bold("", "", 0);                           /* 空串 */

    /* 备案件点名的两词实际落点（len 10 -> 5）：conditions -> condi，associated -> assoc。 */
    expect_bold("conditions", "BBBBB.....", 1);
    expect_bold("associated", "BBBBB.....", 1);

    printf("%s: %d passed, %d failed\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
