#!/usr/bin/env python3
"""26.11.8 排版批的**现状留白基线**（只量数，不改产品码）。

做法：把 `book_layout.c` 的 `line_append` + `take_line` 原样抽出来编译成宿主单元，
字体桩换成 `main/assets/builtin.ttf` 的**真实 advance**（stb_truetype，取整规则照抄
`ttf_font.c:measure_width` 的 `lroundf(advance * scale)`），对 Flatland 那段原文跑一遍，
逐行报「行宽 / 版心宽 / 行尾留白（em）/ 该行空格数」。

为什么不能用等宽桩：留白是**字形前进宽度**的函数。等宽桩把 ASCII 一律当 10px，
窄字母（i、l、t）与宽字母（m、w）一样宽，行尾留白会被系统性抹平——量出来的数
不是这本书的数。`book_wrap_golden.py` 用等宽桩是对的（它比的是两版**相对**差异），
本脚本要比**绝对**留白，必须真字体。

几何口径：版心宽按 `book_layout_balanced_rect` 的「整字格」规则取整，即
`width = px + (cols-1) * (px + tracking)`，所以版心宽天然是整数个 em。脚本按 em
（`available_em`）给参数，与 px 解耦：留白以 em 计时，换字号不换结论。

跑法：python3 tools/measure_line_whitespace.py [--em 17] [--px 48]
"""
from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
LAYOUT = ROOT / "main/book/book_layout.c"
FONT = ROOT / "main/assets/builtin.ttf"
STB = ROOT / "main/font"

STUBS = r'''
/* ---- 宿主桩：真字体 + 无块表 + 无禁则 ---- */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "hyphen.h"
#include "kern_pairs.h"
static bool s_vertical; /* 宿主单元恒横排 */

#define PSRAM_CAPS 0
static void* heap_caps_realloc(void* p, size_t n, int caps) { (void)caps; return realloc(p, n); }

typedef struct {
    size_t offset, len;
    bool heading;
    uint8_t align;
    int margin_before_percent, margin_after_percent;
    bool line_break_after;
    int image;
} blk_t;

static const char* s_text;
static size_t s_len;
static char* s_line;
static size_t s_line_capacity;
static int s_px = 30;
static int s_tracking_px = 0;
static unsigned s_first_line_indent_em = 0;
static int s_first_line_indent_adjust_px = 0;

static size_t codepoint_size(const char* s, size_t n) {
    unsigned char c = (unsigned char)s[0];
    size_t want = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
    return want <= n ? want : 0;
}
static uint32_t codepoint_value(const char* s, size_t n) {
    unsigned char c = (unsigned char)s[0];
    if (n == 1) return c;
    if (n == 2) return ((uint32_t)(c & 0x1F) << 6) | ((unsigned char)s[1] & 0x3F);
    if (n == 3) return ((uint32_t)(c & 0x0F) << 12) | (((unsigned char)s[1] & 0x3F) << 6) |
                      ((unsigned char)s[2] & 0x3F);
    return ((uint32_t)(c & 0x07) << 18) | (((unsigned char)s[1] & 0x3F) << 12) |
           (((unsigned char)s[2] & 0x3F) << 6) | ((unsigned char)s[3] & 0x3F);
}

/* ---- 真字体：stb_truetype，取整照抄 ttf_font.c:measure_width ---- */
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
static stbtt_fontinfo g_font;
static float g_scale;
static int g_loaded, g_fallback_glyphs;

static int load_font(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 0; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); return 0; }
    fclose(f);
    if (!stbtt_InitFont(&g_font, buf, stbtt_GetFontOffsetForIndex(buf, 0))) return 0;
    return 1;
}
/* 与 measure_width 同一张表、同一条取整：lroundf(units * ScaleForPixelHeight(px))。
 * 产品实现以 ttf_font.c 的 ttf_kern_px 为准；这里只是让统计吃上真字距。 */
bool app_typography_enhanced(void);   /* 前向声明：ttf_kern_px 与断词都看它 */

/* 排版总开关：PICO_TYPO=0 经典态（无字距/无断词），否则增强。 */
bool app_typography_enhanced(void) {
    const char *v = getenv("PICO_TYPO");
    return !(v && v[0] == '0' && v[1] == 0);
}

static int ttf_kern_px(int px, uint32_t a, uint32_t b) {
    if (!app_typography_enhanced()) return 0;   /* 经典态无字距：与产品实现同一条判据 */
    if (!g_loaded) return 0;
    if (px != 0 && g_scale <= 0) g_scale = stbtt_ScaleForPixelHeight(&g_font, (float)px);
    int lg = stbtt_FindGlyphIndex(&g_font, (int)a);
    int rg = stbtt_FindGlyphIndex(&g_font, (int)b);
    if (lg <= 0 || rg <= 0) return 0;
    uint32_t key = ((uint32_t)lg << KERN_GID_BITS) | (uint32_t)rg;
    int lo = 0, hi = KERN_PAIR_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint32_t k = kern_pair_keys[mid];
        if (k == key) return (int)lroundf((float)kern_pair_values[mid] * g_scale);
        if (k < key) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

/* 与 measure_width 同一条取整：lroundf(advance * ScaleForPixelHeight(px))。 */
static int ttf_text_width_px(int px, const char* glyph) {
    if (!g_loaded) return 0;
    if (px != 0 && g_scale <= 0) g_scale = stbtt_ScaleForPixelHeight(&g_font, (float)px);
    const char* cursor = glyph;
    uint32_t cp = codepoint_value(cursor, strlen(cursor));
    int gid = stbtt_FindGlyphIndex(&g_font, (int)cp);
    if (gid == 0) { ++g_fallback_glyphs; return (int)lroundf((float)px * 0.5f); }
    int advance = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&g_font, gid, &advance, &lsb);
    return (int)lroundf((float)advance * g_scale);
}
static int ttf_text_left_bearing_px(int px, const char* glyph) { (void)px; (void)glyph; return 0; }
static const blk_t* block_at(size_t off) { (void)off; return NULL; }
static size_t visible_start(size_t off, size_t limit) { (void)limit; return off; }
static bool prohibited_line_end(uint32_t cp) { (void)cp; return false; }
static bool prohibited_line_start(uint32_t cp) { (void)cp; return false; }
static void slot_select_for(size_t off) { (void)off; }
static void line_runs_build(const blk_t* b, size_t a, size_t z) { (void)b; (void)a; (void)z; }
static bool word_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '\'' || c == '-';
}
'''

# 照片那一段（Flatland, Chapter 1）。`dePart ing` 是 26.11.6 的残影叠印，原文是 departing。
CORPUS = (
    "As soon as the sound of the Peace-cry of my departing Wife had died away, I began to "
    "approach the Stranger with the intention of taking a nearer view and of bidding him be "
    "seated; but his appearance struck me dumb and motionless with astonishment. Without the "
    "slightest symptoms of angularity he nevertheless varied every instant with gradations of "
    "size and brightness scarcely possible for any Figure within the scope of my experience. "
    "The thought flashed across me that I might have before me a burglar or cut-throat, some "
    "monstrous Irregular Isosceles, who, by feigning the voice of a Circle, had obtained "
    "admission somehow into the house, and was now preparing to stab me with his acute angle."
)

RUNNER = r'''
static const char k_text[] = %(text)s;

static int count_spaces(const char* s) {
    int n = 0;
    for (; *s; ++s) if (*s == ' ') ++n;
    return n;
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!load_font(argv[1])) return 2;
    g_loaded = 1;
    s_line_capacity = 256;
    s_line = malloc(s_line_capacity);
    if (!s_line) return 2;
    s_line[0] = 0;
    s_text = k_text;
    s_len = strlen(k_text);
    s_px = atoi(argv[2]);
    const int available = atoi(argv[3]);
    g_scale = stbtt_ScaleForPixelHeight(&g_font, (float)s_px);

    printf("px=%%d available=%%d (%%d em) glyphs_fallback=%%d\n", s_px, available, available / s_px, g_fallback_glyphs);
    size_t off = 0;
    int idx = 0;
    while (off < s_len && idx < 200) {
        size_t next = 0;
        bool pend = false, heading = false;
        int px = 0, lw = 0, indent = 0, mb = 0, ma = 0;
        uint8_t align = 0;
        if (!take_line(off, available, 0, &next, &pend, &px, &heading, &lw, &indent, &align,
                       &mb, &ma)) {
            printf("LINE %%02d TAKE_FAILED at %%zu\n", idx, off);
            return 3;
        }
        if (next <= off) { printf("LINE %%02d NO_PROGRESS at %%zu\n", idx, off); return 3; }
        int slack = available - indent - lw;
        int spaces = count_spaces(s_line);
        bool final_line = pend || next >= s_len;
        printf("LINE %%02d usable=%%d width=%%d slack=%%d slack_em=%%+.3f spaces=%%d final=%%d |%%s|\n",
               idx, available - indent, lw, slack, (double)slack / (double)s_px, spaces,
               final_line ? 1 : 0, s_line);
        off = next;
        ++idx;
    }
    return 0;
}
'''

EXTRACT_START = "static bool line_append("
EXTRACT_END = "/* ---- 竖排 / Vertical setting ---- */"


def extract(src: str) -> str:
    a = src.index(EXTRACT_START)
    b = src.index(EXTRACT_END)
    return src[a:b]


def build_unit(layout_source: str) -> str:
    # 语料以 C 字符串字面量塞进 RUNNER：逐行切段，避免超长单行撞编译器限制。
    chunks = [CORPUS[i:i + 72] for i in range(0, len(CORPUS), 72)]
    literal = "\n".join('    "%s"' % c.replace('\\', '\\\\').replace('"', '\\"') for c in chunks)
    runner = RUNNER % {"text": "\n" + literal}
    return STUBS + extract(layout_source) + runner


def run(layout_source: str, px: int, available: int) -> str:
    unit = build_unit(layout_source)
    with tempfile.TemporaryDirectory() as td:
        c = pathlib.Path(td) / "measure.c"
        exe = pathlib.Path(td) / "measure"
        c.write_text(unit)
        # 26.11.13 起 take_line 会调用 hyph_points：真实现 + 真表一起链进来，
        # 不用桩——这条脚本量的是「断词之后」的留白，桩会把 A 档的收益抹掉。
        # Since 26.11.13 take_line calls hyph_points: link the real implementation and table.
        subprocess.run(["gcc", "-std=c11", "-D_DEFAULT_SOURCE", "-w", "-O1", "-I", str(STB),
                        "-I", str(ROOT / "main/book"),
                        str(c), str(ROOT / "main/book/hyphen.c"),
                        str(ROOT / "main/book/hyph_en_us_data.c"),
                        str(ROOT / "main/font/kern_pairs.c"),
                        "-o", str(exe), "-lm"], check=True)
        p = subprocess.run([str(exe), str(FONT), str(px), str(available)],
                           capture_output=True, text=True)
        if p.returncode != 0:
            sys.stderr.write(p.stdout + p.stderr)
            raise SystemExit(f"measure unit failed (rc={p.returncode})")
        return p.stdout


def summarize(out: str, px: int, label: str) -> dict:
    rows = []
    fallback = 0
    for line in out.splitlines():
        if line.startswith("px="):
            for tok in line.split():
                if tok.startswith("glyphs_fallback="):
                    fallback = int(tok.split("=")[1])
        if not line.startswith("LINE "):
            continue
        head, text = line.split(" |", 1)
        fields = {}
        for tok in head.split():
            if "=" in tok:
                k, v = tok.split("=", 1)
                fields[k] = v
        rows.append({
            "slack_em": float(fields["slack_em"]),
            "spaces": int(fields["spaces"]),
            "final": fields["final"] == "1",
            "text": text.rstrip("|").strip(),
            "usable": int(fields["usable"]),
            "width": int(fields["width"]),
        })
    body = [r for r in rows if not r["final"]]
    slacks = sorted(r["slack_em"] for r in body)
    def pct(q):
        if not slacks:
            return 0.0
        i = min(len(slacks) - 1, max(0, int(round(q * (len(slacks) - 1)))))
        return slacks[i]
    over3 = [r for r in body if r["slack_em"] > 3.0]
    over2 = [r for r in body if r["slack_em"] > 2.0]
    stretch = [r["slack_em"] / r["spaces"] for r in body if r["spaces"] > 0]
    print(f"\n== {label}: px={px} 版心={rows[0]['usable']}px ({rows[0]['usable']/px:.2f} em) "
          f"行数={len(rows)} 正文行={len(body)} 缺字回退={fallback}")
    print(f"   留白 em：p50={pct(0.5):.2f}  p90={pct(0.9):.2f}  max={max(slacks) if slacks else 0:.2f}")
    print(f"   >3em 行 {len(over3)}/{len(body)} = {len(over3)/max(1,len(body))*100:.0f}%"
          f"   >2em 行 {len(over2)}/{len(body)} = {len(over2)/max(1,len(body))*100:.0f}%")
    if stretch:
        stretch.sort()
        print(f"   若两端对齐：每空格需拉伸 {stretch[len(stretch)//2]:.2f} em（中位）"
              f"，最大 {stretch[-1]:.2f} em")
    # 分配器上限 0.75 em/间隙：低于它的行能**精确**落到右边界，高于它的行只能部分拉伸。
    # 判据 ①（右缘方差 ≤2 px）等价于「部分拉伸行为 0」。
    CAP_EM = 0.75
    over_cap = [r for r in body if r["spaces"] > 0 and r["slack_em"] / r["spaces"] > CAP_EM]
    no_gap = [r for r in body if r["spaces"] == 0 and r["slack_em"] > 0]
    print(f"   对齐可达：{len(body) - len(over_cap) - len(no_gap)}/{len(body)} 行可精确落到右边界"
          f"（{CAP_EM} em/间隙上限）；部分拉伸 {len(over_cap)} 行；无间隙 {len(no_gap)} 行")
    for r in sorted(body, key=lambda x: -x["slack_em"])[:3]:
        print(f"   worst {r['slack_em']:+.2f} em ({r['spaces']} 空格) | {r['text'][:56]}")
    return {"rows": rows, "body": body, "p50": pct(0.5), "p90": pct(0.9),
            "max": max(slacks) if slacks else 0.0, "over3": len(over3),
            "over2": len(over2), "fallback": fallback,
            "lines": [(r["final"], r["text"], r["width"]) for r in rows]}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--px", type=int, default=48, help="字号（默认 48，取大一档以减小舍入噪声）")
    ap.add_argument("--ems", default="13,15,17,19,21", help="版心宽（em），逗号分隔")
    ap.add_argument("--rev", default="", help="从该 git 版本取 book_layout.c（如 HEAD）做对照")
    ap.add_argument("--fingerprint", action="store_true",
                    help="打印分页指纹（逐行 文本+宽度 的 sha256），供开关批做等价断言")
    args = ap.parse_args()

    if args.rev:
        src = subprocess.run(["git", "show", f"{args.rev}:main/book/book_layout.c"],
                             cwd=ROOT, capture_output=True, text=True, check=True).stdout
        print(f"== 对照：{args.rev} 版 book_layout.c + builtin.ttf 真实 advance ==")
    else:
        src = LAYOUT.read_text()
        print("== 现状：工作树 book_layout.c + builtin.ttf 真实 advance ==")
    results = {}
    for em in [int(x) for x in args.ems.split(",")]:
        out = run(src, args.px, em * args.px)
        results[em] = summarize(out, args.px, f"版心 {em} em")
    print("\n== 汇总（>3em 行占比 / max 留白 em）==")
    # 分页指纹：逐行（是否段末 + 文本 + 实宽）按版心顺序串起来取 sha256。
    # 它比「行数相等」硬：行数相同而断点不同，这里照样不同。
    import hashlib
    fp = hashlib.sha256()
    for em in sorted(results):
        for final, text, width in results[em]["lines"]:
            fp.update(f"{em}|{1 if final else 0}|{text}|{width}\n".encode("utf-8"))
    if args.fingerprint:
        print(f"FINGERPRINT {fp.hexdigest()}")
    for em, r in results.items():
        print(f"   {em:>2} em: p50={r['p50']:.2f} p90={r['p90']:.2f} max={r['max']:.2f} "
              f">3em={r['over3']}/{len(r['body'])} ({r['over3']/max(1,len(r['body']))*100:.0f}%)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
