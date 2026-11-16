/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 两端对齐的**唯一一份**分配计划：绘制（ttf_font.c）、点词命中（book_layout.c）、
 * 宿主门 1（typography_gate1.py 编译真代码）三处共用。
 *
 * 为什么单独成头：宿主布局测试用的是 `tools/` 下各 `_stubs` 目录里的 `ttf_font.h`，桩头不能把真头
 * 整个拉进来（会连带 ESP 依赖），但**规则不能因此分家**——桩头直接 include 本文件，
 * 宿主跑的分配规则与固件就是同一份字节。门禁体检 H3「桩头副本与真实头不一致」那一课，
 * 这里是同一个道理。
 *
 * 纯算术 + 只读文本：不碰字体、不链接任何东西，所以能安全地被桩头复用。
 *
 * The single justification plan, shared by drawing, hit-testing and the host gate. Stub
 * headers include this file so host tests exercise the very same rule.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/// 汉字/全角标点相邻处才算「可调间隙」。
/// A gap counts as adjustable next to CJK glyphs or fullwidth punctuation.
static inline bool ttf_cjk_gap_point(uint32_t cp) {
    return (cp >= 0x2e80 && cp <= 0xffef) || (cp >= 0x2018 && cp <= 0x201d);
}

/// 西文的可调间隙是**词间空格**——这正是 [123] 修的根：从前只数汉字间隙，
/// 英文 `cjk_gaps==0`，两端对齐在英文上从未生效过。一个空格只算一次（算在空格之后），
/// 前后都算会让间隙数翻倍、每个间隙摊到的量减半。
/// Latin absorbs slack at **word spaces**; counting only CJK gaps is why justification
/// never applied to English. A space counts once (the gap after it), not twice.
static inline bool ttf_justify_gap(uint32_t cp, uint32_t next) {
    if (cp == ' ' || cp == '\t') return true;
    if (next == ' ' || next == '\t') return false;
    return ttf_cjk_gap_point(cp) || ttf_cjk_gap_point(next);
}

/// 最小 UTF-8 解码（计划函数自己扫串，不依赖 ttf_font.c 的静态实现）。
static inline uint32_t ttf_plan_cp(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t cp = *s++;
    if (cp >= 0x80) {
        int n = (cp >= 0xf0) ? 3 : (cp >= 0xe0) ? 2 : (cp >= 0xc0) ? 1 : 0;
        if (n) {
            cp &= (1u << (6 - n)) - 1;
            for (int i = 0; i < n; ++i) cp = (cp << 6) | (s[i] & 0x3f);
            s += n;
        }
    }
    *p = (const char *)s;
    return cp;
}

typedef struct {
    int gaps;      ///< 参与分配的间隙数 / gaps that absorb the slack
    int step;      ///< 每个间隙的基准增减 / base delta per gap
    int remainder; ///< 前 |remainder| 个间隙再各加/减 1 px / +-1 px on the first ones
    bool fitted;   ///< false：按自然字距绘制 / draw with natural advances
    bool compress; ///< 负差额（压缩）/ negative delta
    bool partial;  ///< 已到上限仍有差额：部分拉伸，行尾留白 / capped, line stays short
} ttf_justify_t;

/// 生成分配计划。`natural` 必须**已含 tracking**（kiko 同款 `measure + tracking*gaps`），
/// 否则用户一开字距，预算就系统性错算——那正是假齐的代码面。
/// `natural` must already include tracking, or a non-zero tracking setting makes the
/// budget systematically wrong - the code-side shape of "fake justification".
static inline void ttf_justify_plan(const char *text, int natural, int target, int px,
                                    ttf_justify_t *out) {
    out->gaps = 0;
    out->step = 0;
    out->remainder = 0;
    out->fitted = false;
    out->compress = false;
    out->partial = false;
    if (!text || !*text || target <= 0 || px <= 0) return;

    const int delta = target - natural;
    out->compress = delta < 0;

    int glyphs = 0, word_gaps = 0;
    uint32_t prev = 0;
    for (const char *p = text; *p;) {
        uint32_t cp = ttf_plan_cp(&p);
        if (glyphs && ttf_justify_gap(prev, cp)) ++word_gaps;
        prev = cp;
        ++glyphs;
    }
    const int all_gaps = glyphs - 1;
    if (all_gaps <= 0) return;
    out->gaps = out->compress ? all_gaps : word_gaps;
    if (out->gaps <= 0) return;

    // 上限按字号走（0.75 em 的伸长 / 0.25 em 的压缩），不再是一个与字号无关的 12 px：
    // 22 号字上 12 px 已是两倍词距，34 号字上却几乎看不出。
    // The cap scales with the face: 0.75 em of stretch, 0.25 em of squeeze.
    const int cap = out->compress ? (px / 4 > 3 ? px / 4 : 3)
                                  : (px * 3 / 4 > 8 ? px * 3 / 4 : 8);
    int per = delta / out->gaps;
    if (per > cap) { per = cap; out->partial = true; }
    if (per < -cap) { per = -cap; out->partial = true; }
    out->step = per;
    out->remainder = out->partial ? 0 : delta - per * out->gaps;
    out->fitted = true;
}

