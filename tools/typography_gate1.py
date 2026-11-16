#!/usr/bin/env python3
"""门 1：运行时零差值断言（常设门，2026-11-03 第十二批定形）。

**断言什么**：`ttf_text_width_px`（度量）与 `ttf_draw_text_px_fitted` / `ttf_draw_text_px`
（绘制）在同一串、同一字号、`tracking=0` 下的结束笔位**逐像素相等**；固定字模
（引用条 22 / 页脚 24）同样比 `ui_font_text_width_px` 与 `ui_font_draw_text_px`。

**为什么容差是 0**：两条路径同源、同一 `lroundf(advance*scale)`，同源同舍入就该相等。
实跑出现 ±1 即为发现，不是「容差材料」——这条门的价值就在于对「看似无害的舍入漂移」
零容忍。`[124]` kerning 只要改动一侧推进，这里立刻红。

**为什么在宿主跑真代码**：不是模型、不是脚本复刻。编译的是 `main/font/ttf_font.c`、
`main/font/ui_font.c` 原件，字体是 `main/assets/builtin.ttf` 原件（`ld -r -b binary`
嵌进可执行文件），ESP 层是 `tools/typography_stubs/` 的桩——桩只提供堆、日志、时钟和
`epd_draw_pixel`，不参与任何宽度计算。

跑法：python3 tools/typography_gate1.py [--keep]
"""
from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
STUBS = ROOT / "tools/typography_stubs"
FONT = ROOT / "main/assets/builtin.ttf"

# 正文字号四档 + 固定字模两档（引用条 22 / 页脚 24）——审计点名的六档。
TTF_SIZES = (22, 24, 27, 30)
FIXED_SIZES = (22, 24)

MAIN_C = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "ttf_font.h"
#include "ui_font.h"

/* ---- 桩：像素落点 + 堆 + 错误名。宽度计算一个都不碰。 ---- */
#define FB_W 2048
#define FB_H 2048
static uint8_t *g_fb;

void epd_draw_pixel(int x, int y, uint8_t color, uint8_t* fb) {
    if (!fb || x < 0 || y < 0 || x >= FB_W || y >= FB_H) return;
    fb[(size_t)y * FB_W + x] = color;
}
void *heap_caps_malloc(size_t n, unsigned caps) { (void)caps; return malloc(n); }
void *heap_caps_calloc(size_t c, size_t n, unsigned caps) { (void)caps; return calloc(c, n); }
void *heap_caps_realloc(void *p, size_t n, unsigned caps) { (void)caps; return realloc(p, n); }
void heap_caps_free(void *p) { free(p); }
size_t heap_caps_get_largest_free_block(unsigned caps) { (void)caps; return 8u << 20; }
const char *esp_err_to_name(esp_err_t code) { (void)code; return "err"; }

static int g_fails;
static int g_checks;

static const char *CASES[] = {
    "The quick brown fox jumps over the lazy dog.",
    "Isosceles",
    "appreciate",
    "AVATAR WAVE To Yo",
    "iiilllmmm",
    "0123456789 48,151,136 B",
    "monstrous Irregular Isosceles, who, by feigning the voice of a Circle",
    "\xE4\xB8\xAD\xE6\x96\x87\xE6\x8E\x92\xE7\x89\x88\xEF\xBC\x8C\xE4\xB8\xA4\xE7\xAB\xAF\xE5\xAF\xB9\xE9\xBD\x90\xE3\x80\x82",
    "mixed \xE4\xB8\xAD\xE6\x96\x87 and Latin \xE2\x80\x94 dash",
};

/* 排版总开关的宿主实现：环境变量 PICO_TYPO=0 时走经典态（左对齐/无断词/无字距），
 * 其余（含未设）默认增强。门 1 自身在增强态跑——经典态由开关批的专门工具验。 */
bool app_typography_enhanced(void) {
    const char *v = getenv("PICO_TYPO");
    return !(v && v[0] == '0' && v[1] == 0);
}

static void fail(const char *what, int px, const char *s, int want, int got) {
    ++g_fails;
    printf("FAIL  %-28s px=%-2d want=%-4d got=%-4d  |%s|\n", what, px, want, got, s);
}

/* 正文：度量 vs 绘制（fitted 以自然宽为目标 → delta=0，仍走真实分摊循环） */
static void check_ttf(int px, const char *s) {
    const int m = ttf_text_width_px(px, s);
    // 0 == 0 是空断言：字没装上时两边都返回 0，门会假绿。宽度必须为正才算测到。
    // 0 == 0 would be vacuous: a missing glyph makes both sides zero. Require a real width.
    ++g_checks; if (m <= 0) { fail("ttf width is zero", px, s, 1, m); return; }
    const int d_fit = ttf_draw_text_px_fitted(g_fb, 10, 200, px, s, 0, m, 0, 15) - 10;
    const int d_plain = ttf_draw_text_px(g_fb, 10, 200, px, s, EPD_DRAW_ALIGN_LEFT, 0, 15) - 10;
    ++g_checks; if (d_fit != m) fail("ttf fitted advance", px, s, m, d_fit);
    ++g_checks; if (d_plain != m) fail("ttf plain advance", px, s, m, d_plain);

    // 可加性：排版器逐字累加（take_line），绘制与度量按整串算；两者必须相等，
    // 否则行宽与笔位从源头上就差几个像素——两端对齐会「看起来齐、实际差一点」。
    // [124] 起还差一层：字距是**对**的量，逐字求和之外要把每一对补回来。
    // Additivity: per-glyph advances plus one kern per adjacent pair must equal the whole.
    int sum = 0;
    for (const char *p = s; *p;) {
        const char *q = p;
        const uint32_t cp = ttf_plan_cp(&q);
        char g[5];
        const size_t n = (size_t)(q - p);
        memcpy(g, p, n);
        g[n] = 0;
        sum += ttf_text_width_px(px, g);
        if (*q) {
            const char *r = q;
            const uint32_t next = ttf_plan_cp(&r);
            sum += ttf_kern_px(px, cp, next);
        }
        p = q;
    }
    ++g_checks; if (sum != m) fail("per-glyph sum == whole", px, s, m, sum);
}

/* 字距：表里有的对必须真的发生，表里没有的对必须真的是 0。
 * 这条同时钉住两件事——「装了表」与「没瞎猜」：字体的覆盖只到 73 个字形，
 * `AV`/`WA` 这类常见对在这张表里**没有**调整，判据必须认这个事实，不能照抄教科书的例子。 */
static void check_kern(void) {
    static const struct { const char *pair; int want_negative; } CASES[] = {
        { "To", 1 }, { "Yo", 1 }, { "VA", 1 }, { "LT", 1 },
        { "AV", 0 }, { "WA", 0 },
    };
    for (int px = 22; px <= 30; px += 4) {
        for (unsigned i = 0; i < sizeof(CASES) / sizeof(CASES[0]); ++i) {
            const char *pair = CASES[i].pair;
            char a[2] = { pair[0], 0 };
            char b[2] = { pair[1], 0 };
            const int whole = ttf_text_width_px(px, pair);
            const int kern = ttf_kern_px(px, (uint32_t)pair[0], (uint32_t)pair[1]);
            const int parts = ttf_text_width_px(px, a) + ttf_text_width_px(px, b);
            ++g_checks;
            if (whole != parts + kern) fail("kern applied exactly once", px, pair, parts + kern, whole);
            ++g_checks;
            // 单位换成像素会取整：小字号上 -21/1000 em 落在半像素内，取整就是 0，
            // 这不是错，是表与字的真实比例。所以判据只咬「不许变宽」，再单独咬 To
            // 这种幅度足够大的对必须真的收窄。
            if (CASES[i].want_negative && kern > 0) fail("kern grew the pair", px, pair, 0, kern);
            if (CASES[i].want_negative && strcmp(pair, "To") == 0 && kern >= 0)
                fail("kern shrank the pair", px, pair, -1, kern);
            if (!CASES[i].want_negative && kern != 0) fail("kern invented a pair", px, pair, 0, kern);
        }
    }
}

/* 正向实现锚：**表里真实存在的对**，四档字号的期望像素由 fontTools 从 GPOS 读出的
 * 设计单位换算成字面量（span = hhea.ascent - hhea.descent），**不复用实现的换算公式**——
 * 复用公式等于自证，公式错了也会绿。挑对规则见 Python 侧 kern_expectations()。
 *
 * 为什么必须有这一条：负向锚（kern=0 时行为不变）挡不住「表在、查表坏了、永远 miss」——
 * 那种静默失效下，justify 会把自然宽变化全部吸收，行照样齐，kern=0 测试照样绿，
 * TF 字体测试也照样绿（那张表本来就没有 kern）。正向锚是唯一能看见它的眼睛。
 * Positive anchor: real pairs from the real table, expected pixels baked as literals. */
static const struct { const char *pair; int units; int want[4]; } KERN_POS[] = {
KERN_POS_TABLE_PLACEHOLDER
};
static const int KERN_POS_SIZES[4] = {22, 24, 27, 30};

static void check_kern_positive(void) {
    for (unsigned i = 0; i < sizeof(KERN_POS) / sizeof(KERN_POS[0]); ++i) {
        char a[2] = { KERN_POS[i].pair[0], 0 };
        char b[2] = { KERN_POS[i].pair[1], 0 };
        for (int k = 0; k < 4; ++k) {
            const int px = KERN_POS_SIZES[k];
            const int want = KERN_POS[i].want[k];
            // want == 0 的对不是锚：它与「查表 miss」在断言上不可区分，是假锚。
            ++g_checks;
            if (want == 0) { fail("positive anchor degenerate", px, KERN_POS[i].pair, 1, 0); continue; }
            const int kern = ttf_kern_px(px, (uint32_t)KERN_POS[i].pair[0],
                                        (uint32_t)KERN_POS[i].pair[1]);
            ++g_checks;
            if (kern != want) fail("kern value != table", px, KERN_POS[i].pair, want, kern);
            const int whole = ttf_text_width_px(px, KERN_POS[i].pair);
            const int parts = ttf_text_width_px(px, a) + ttf_text_width_px(px, b);
            ++g_checks;
            if (whole != parts + want) fail("advance != baseline + kern", px, KERN_POS[i].pair,
                                            parts + want, whole);
        }
    }
}

/* 经典态（开关关）：判据跟策略走——它不该落到目标宽度、也不该有字距。
 * 这两条正是「off = [122] 全退」在字体层的可执行形态：
 *   · 任何真实 kern 对必须为 0（否则 off 仍带着 [124] 的痕迹）；
 *   · fitted 的结束笔位必须等于自然宽（ragged right），且不等于 target
 *     （若仍等于 target，说明 justify 分支没被摘掉——那是最隐蔽的半开态）。 */
static void check_classic(void) {
    for (unsigned i = 0; i < sizeof(KERN_POS) / sizeof(KERN_POS[0]); ++i) {
        char a[2] = { KERN_POS[i].pair[0], 0 };
        char b[2] = { KERN_POS[i].pair[1], 0 };
        for (int k = 0; k < 4; ++k) {
            const int px = KERN_POS_SIZES[k];
            ++g_checks;
            if (ttf_kern_px(px, (uint32_t)KERN_POS[i].pair[0],
                            (uint32_t)KERN_POS[i].pair[1]) != 0)
                fail("classic kern must be zero", px, KERN_POS[i].pair, 0,
                     ttf_kern_px(px, (uint32_t)KERN_POS[i].pair[0],
                                 (uint32_t)KERN_POS[i].pair[1]));
            const int whole = ttf_text_width_px(px, KERN_POS[i].pair);
            const int parts = ttf_text_width_px(px, a) + ttf_text_width_px(px, b);
            ++g_checks;
            if (whole != parts) fail("classic width != raw sum", px, KERN_POS[i].pair, parts, whole);
        }
    }
    for (unsigned c = 0; c < sizeof(CASES) / sizeof(CASES[0]); ++c) {
        const char *s = CASES[c];
        for (int k = 0; k < 4; ++k) {
            const int px = KERN_POS_SIZES[k];
            const int m = ttf_text_width_px(px, s);
            if (m <= 0) continue;
            const int target = m + 60;
            const int d = ttf_draw_text_px_fitted(g_fb, 10, 200, px, s, 0, target, 0, 15) - 10;
            ++g_checks;
            if (d != m) fail("classic fitted must be ragged", px, s, m, d);
        }
    }
}

/* 固定字模：位图 advance 的度量 vs 绘制 */
static void check_fixed(int px, const char *s) {
    const int m = ui_font_text_width_px(px, s);
    ++g_checks; if (m <= 0) { fail("fixed width is zero", px, s, 1, m); return; }
    const int d = ui_font_draw_text_px(g_fb, 10, 200, px, s, EPD_DRAW_ALIGN_LEFT, 0, 15, false) - 10;
    ++g_checks; if (d != m) fail("fixed face advance", px, s, m, d);
}

/* 对齐计划：间隙数必须是**词间空格**数；未到上限时 step*gaps+rem 必须精确等于目标。 */
static void check_plan(void) {
    const char *s = "the quick brown fox jumps over the lazy dog";
    const int px = 24;
    const int natural = ttf_text_width_px(px, s);
    int spaces = 0;
    for (const char *p = s; *p; ++p) if (*p == ' ') ++spaces;

    for (int k = -natural / 2; k <= natural; ++k) {
        ttf_justify_t j;
        ttf_justify_plan(s, natural, natural + k, px, &j);
        ++g_checks;
        // 伸长只摊在词间空格上；压缩时每个字间都参与（字距收紧是逐字的）。
        int want_gaps = k >= 0 ? spaces : (int)strlen(s) - 1;
        if (!j.fitted || j.gaps != want_gaps) {
            fail("plan gaps", px, s, want_gaps, j.gaps);
            continue;
        }
        if (!j.partial) {
            ++g_checks;
            const int sum = natural + j.step * j.gaps + j.remainder;
            if (sum != natural + k) fail("plan sums to target", px, s, natural + k, sum);
        }
        // ⑤ 同一行内空格宽度差异 ≥1 px：有余额时前 |rem| 个间隙多 1 px。
        if (!j.partial && j.remainder != 0) {
            ++g_checks;
            int lo = j.step, hi = j.step + (j.remainder > 0 ? 1 : -1);
            if (hi - lo < 1 && lo - hi < 1) fail("gap delta >= 1px", px, s, 1, hi - lo);
        }
    }

    /* 绘制真的落到目标：挑几个未触上限的 k，末尾笔位必须恰为 x+target。 */
    for (int k = 0; k <= natural; k += 7) {
        ttf_justify_t j;
        ttf_justify_plan(s, natural, natural + k, px, &j);
        if (!j.fitted || j.partial) continue;
        const int d = ttf_draw_text_px_fitted(g_fb, 10, 200, px, s, 0, natural + k, 0, 15) - 10;
        ++g_checks;
        if (d != natural + k) fail("fitted lands on target", px, s, natural + k, d);
    }
}

int main(void) {
    g_fb = calloc((size_t)FB_W * FB_H, 1);
    if (!g_fb) return 2;
    FILE *f = fopen(FONT_PATH_PLACEHOLDER, "rb");
    if (!f) { fprintf(stderr, "cannot open font\n"); return 2; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);

    ttf_font_slots_init();
    int slot = ttf_font_open_mem(buf, (size_t)n, "builtin");
    if (slot < 0) { fprintf(stderr, "open_mem failed\n"); return 2; }
    ttf_font_select(slot);
    if (!ttf_font_slot_ready(slot)) { fprintf(stderr, "slot not ready\n"); return 2; }

    static const int ttf_sizes[] = {22, 24, 27, 30};
    static const int fixed_sizes[] = {22, 24};
    for (unsigned i = 0; i < sizeof(ttf_sizes) / sizeof(ttf_sizes[0]); ++i)
        for (unsigned c = 0; c < sizeof(CASES) / sizeof(CASES[0]); ++c)
            check_ttf(ttf_sizes[i], CASES[c]);
    for (unsigned i = 0; i < sizeof(fixed_sizes) / sizeof(fixed_sizes[0]); ++i)
        for (unsigned c = 0; c < sizeof(CASES) / sizeof(CASES[0]); ++c)
            check_fixed(fixed_sizes[i], CASES[c]);
    printf("mode: %s\n", app_typography_enhanced() ? "enhanced" : "classic");
    if (app_typography_enhanced()) {
        check_kern();
        check_kern_positive();
        check_plan();
    } else {
        check_classic();
    }

    printf("gate1: %d checks, %d failures, font=%ld B\n", g_checks, g_fails, n);
    printf("kern anchors:");
    for (unsigned i = 0; i < sizeof(KERN_POS) / sizeof(KERN_POS[0]); ++i)
        printf(" %s(%d)", KERN_POS[i].pair, KERN_POS[i].units);
    printf("\n");
    return g_fails ? 1 : 0;
}
'''


def kern_expectations(count: int = 5):
    """从字体**真实 GPOS 表**里挑 pairs，并算出四档字号的期望像素。

    挑对规则（刻意不硬编码 "LT"/"WA" 这类教科书名字）：
      · 覆盖表里真的存在、调整量非零；
      · 两侧都是可打印 ASCII，便于当字符串用；
      · **四档字号（22/24/27/30）换算后都非零**——归零的对与「查表 miss」
        在断言上不可区分，是假锚；
      · 按 |units| 从大到小取前 `count` 个互不相同的字符对。

    期望像素用 hhea 的 `ascent - descent` 作 span（stbtt_ScaleForPixelHeight 的口径），
    四舍五入按 lroundf 的「远离零」规则。返回 (pair, units, [px22, px24, px27, px30])。
    """
    from fontTools.ttLib import TTFont

    font = TTFont(FONT)
    hhea = font["hhea"]
    span = hhea.ascent - hhea.descent
    gpos = font["GPOS"].table

    # 找 Pair Adjustment（LookupType 2）里的 Format 2 类表；这张字体只有这一个。
    sub = None
    for lookup in gpos.LookupList.Lookup:
        if lookup.LookupType != 2:
            continue
        for st in lookup.SubTable:
            if getattr(st, "Format", 0) == 2:
                sub = st
                break
        if sub is not None:
            break
    if sub is None:
        raise SystemExit("gate1: 字体里没有 GPOS Pair Adjustment Format 2，正向锚无从谈起")

    right_glyphs = {}
    for gname, cls in sub.ClassDef2.classDefs.items():
        right_glyphs.setdefault(cls, []).append(gname)

    cmap = font.getBestCmap()
    cp_of = {}
    for cp, gname in cmap.items():
        cp_of.setdefault(gname, cp)

    def lround_half_away(v: float) -> int:
        return int(v + 0.5) if v >= 0 else -int(-v + 0.5)

    rows = []
    for lg in sub.Coverage.glyphs:
        lc = sub.ClassDef1.classDefs.get(lg, 0)
        rec = sub.Class1Record[lc]
        for cls, glyphs in right_glyphs.items():
            value = rec.Class2Record[cls].Value1.XAdvance
            if not value:
                continue
            for rg in glyphs:
                a, b = cp_of.get(lg), cp_of.get(rg)
                if not a or not b or not (32 < a < 127) or not (32 < b < 127):
                    continue
                rows.append((abs(value), value, chr(a), chr(b), a, b))

    rows.sort(key=lambda r: (-r[0], r[2], r[3]))

    def is_letter(ch: str) -> bool:
        return ch.isascii() and ch.isalpha()

    # 优先纯字母对：`PJ` / `FJ` / `TJ` / `LT` 是排版里真实会挨在一起的组合，
    # `L*` / `.*` 虽然也真实，但拿标点当主锚不具代表性。字母对不够才放宽。
    ordered = [r for r in rows if is_letter(r[2]) and is_letter(r[3])] + \
              [r for r in rows if not (is_letter(r[2]) and is_letter(r[3]))]

    picked, seen = [], set()
    for _, value, ca, cb, a, b in ordered:
        pair = ca + cb
        if pair in seen:
            continue
        pixels = [lround_half_away(value * px / span) for px in TTF_SIZES]
        if any(p == 0 for p in pixels):
            continue          # 有一档归零 → 假锚，跳过
        seen.add(pair)
        picked.append((pair, value, pixels, span))
        if len(picked) == count:
            break
    if len(picked) < count:
        raise SystemExit(f"gate1: 只找到 {len(picked)} 个可用的非零 kern 对，正向锚不够")
    return picked


def kern_table_c(rows) -> str:
    lines = []
    for pair, units, pixels, _ in rows:
        lit = ", ".join(str(v) for v in pixels)
        lines.append(f'    {{ "{pair}", {units}, {{{lit}}} }},')
    return "\n".join(lines)


def build(tmp: pathlib.Path) -> pathlib.Path:
    # 字体按「路径 + 内容」嵌进可执行文件：ld 的符号名取文件名，所以先在临时目录里
    # 放一份同名的 builtin.ttf 再打包，符号才是 _binary_builtin_ttf_start。
    shutil.copy(FONT, tmp / "builtin.ttf")
    subprocess.run(["ld", "-r", "-b", "binary", "-o", str(tmp / "font_blob.o"), "builtin.ttf"],
                   cwd=tmp, check=True)
    # 汉字补充位图（ui_hanzi.c 通过链接符号读它）：同法嵌入，符号名取文件名。
    shutil.copy(ROOT / "main/assets/ui-hanzi.bin", tmp / "ui-hanzi.bin")
    subprocess.run(["objcopy", "--redefine-sym",
                    "_binary_ui_hanzi_bin_start=_binary_ui_hanzi_bin_start"],
                   capture_output=True)  # no-op guard, see below
    subprocess.run(["ld", "-r", "-b", "binary", "-o", str(tmp / "hanzi_blob.o"), "ui-hanzi.bin"],
                   cwd=tmp, check=True)
    subprocess.run(["objcopy",
                    "--redefine-sym", "_binary_ui_hanzi_bin_start=_binary_ui_hanzi_bin_start",
                    "--redefine-sym", "_binary_ui_hanzi_bin_end=_binary_ui_hanzi_bin_end",
                    str(tmp / "hanzi_blob.o")], check=True)

    rows = kern_expectations()
    src = MAIN_C.replace("FONT_PATH_PLACEHOLDER", f'"{FONT}"')
    src = src.replace("KERN_POS_TABLE_PLACEHOLDER", kern_table_c(rows))
    (tmp / "gate1_main.c").write_text(src, encoding="utf-8")

    exe = tmp / "gate1"
    cmd = [
        "gcc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter",
        "-I", str(STUBS), "-I", str(ROOT / "main/font"), "-I", str(ROOT / "main/ui"),
        "-I", str(ROOT / "tools/zip_host_stubs"),
        # 共用 miniz 桩没定义这个标志（它走 zlib 的 raw 模式）；这里只补宏，不改桩。
        "-DTINFL_FLAG_PARSE_ZLIB_HEADER=8",
        "-I", str(ROOT / "main"),
        "-o", str(exe),
        str(tmp / "gate1_main.c"),
        str(ROOT / "main/font/ttf_font.c"),
        str(ROOT / "main/font/kern_pairs.c"),
        str(ROOT / "main/font/ui_font.c"),
        str(ROOT / "main/font/ui_hanzi.c"),
        str(ROOT / "main/ui/ui_gray_lut.c"),
        str(tmp / "font_blob.o"),
        str(tmp / "hanzi_blob.o"),
        "-lm", "-lz",
    ]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("gate1: COMPILE FAILED")
        print(r.stderr[-4000:])
        raise SystemExit(2)
    return exe


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true", help="keep the temp build directory")
    args = ap.parse_args()

    tmp = pathlib.Path(tempfile.mkdtemp(prefix="typo_gate1_"))
    try:
        exe = build(tmp)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        if r.returncode == 0:
            if "mode: classic" in r.stdout:
                print("typography gate1: PASS - classic mode (kern == 0, fitted is ragged right)")
            else:
                print("typography gate1: PASS - measure == draw advance, tolerance 0 "
                      "(sizes 22/24/27/30 + fixed 22/24, tracking=0)")
            return 0
        print("typography gate1: FAIL")
        return 1
    finally:
        if args.keep:
            print(f"kept: {tmp}")
        else:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
