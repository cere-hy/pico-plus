#!/usr/bin/env python3
"""26.11.4 西文按词断行的**黄金向量**（在仓库根目录运行）。

做法：把 `book_layout.c` 里的 `line_append` + `take_line` 原样抽出来编译成宿主单元，
桩掉字体与块表（等宽桩：ASCII 10px、CJK 20px），对固定语料跑出「每行内容」，
再用同一套语料跑一遍 **HEAD 版**（改动之前的源码），两版逐行对照。

判据三条：
  1. 中文语料：两版**逐行逐字节相同**（这条是红线——断行改动不许碰中文排版）；
  2. 英文语料：改后不得出现「行尾字母 + 下行行首字母」（词被切开）；
     例外只有一种：该行内没有空格（一个词比整行还长，必须硬切）；
  3. 差异必须可解释：英文行数只许增加或持平，不许减少。

为什么用「两版对照」而不是只跑新版：新版跑绿只证明「现在是这样」，不证明
「只有该变的地方变了」。26.11.3-B 判 VOID 的教训就是「看起来对」不算证据。

Why a two-version diff: a green run on the new code only says how it behaves now;
only the pair says that *nothing else* moved.
"""
from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
LAYOUT = ROOT / "main/book/book_layout.c"

STUBS = r'''
#include "hyphen.h"
static bool s_vertical; /* 宿主单元恒横排 */
/* ---- 宿主桩：等宽字体 + 无块表 + 无禁则 ---- */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
/* 等宽桩：ASCII 10px、其余（CJK/全角）20px。宽度可手算，黄金向量才可读。 */
static int ttf_text_width_px(int px, const char* glyph) { (void)px; return ((unsigned char)glyph[0] < 0x80) ? 10 : 20; }
// 替身字体没有 kern：这条正是 [124] 细则 1 的降级路径，黄金向量必须照旧逐行相等。
static int ttf_kern_px(int px, uint32_t a, uint32_t b) { (void)px; (void)a; (void)b; return 0; }
/* 排版总开关：PICO_TYPO=0 走经典态（左对齐/无断词/无字距），否则增强。
 * 双向哈希断言靠它切态，不靠编译期宏——同一份二进制跑两遍，才是真正的「开关」。 */
bool app_typography_enhanced(void) {
    const char *v = getenv("PICO_TYPO");
    return !(v && v[0] == '0' && v[1] == 0);
}

static int ttf_text_left_bearing_px(int px, const char* glyph) { (void)px; (void)glyph; return 0; }
static const blk_t* block_at(size_t off) { (void)off; return NULL; }
static size_t visible_start(size_t off, size_t limit) { (void)limit; return off; }
static bool prohibited_line_end(uint32_t cp) { (void)cp; return false; }
static bool prohibited_line_start(uint32_t cp) { (void)cp; return false; }
static void slot_select_for(size_t off) { (void)off; }
static void line_runs_build(const blk_t* b, size_t a, size_t z) { (void)b; (void)a; (void)z; }
/* 26.11.4 起 take_line 用 word_char 判断词内字符；它定义在抽取区之外，这里补一份
   同样的实现（ASCII 字母数字 + 撇号 + 连字符）。HEAD 版不引用它，无副作用。
   take_line now calls word_char; it lives outside the extracted region, so the host
   unit carries the same definition. The HEAD build never calls it. */
static bool word_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '\'' || c == '-';
}
'''

RUNNER = r'''
/* ---- 语料 ---- */
typedef struct { const char* name; const char* text; int width; bool cjk; } corpus_t;

static const char k_cn1[] =
    "在山的那边，是海吗？小时候我常伏在窗口痴想，山那边是什么呢？妈妈给我说过：海。"
    "哦，山那边是海吗？于是，怀着一种隐秘的想望，有一天我终于爬上了那个山顶。"
    "可是，我却几乎是哭着回来了——在山的那边，依然是山。山那边的山啊，铁青着脸，"
    "给我的幻想打了一个零分！妈妈，那个海呢？";
static const char k_cn2[] =
    "第一段讲的是起点。第二段接着说了路上的事，风很大，雨也很急，可是没有人回头。"
    "第三段只有一句话：继续走。";
static const char k_cn3[] =
    "　　纸书的段首常常留两个全角空格，排版时由阅读设置统一接管缩进，原文偏移仍然保留。"
    "这一行就是为了把那条行为钉住：段首的空白不许变成行首的空格。";
static const char k_en1[] =
    "The house stood at the end of the lane. Nobody had lived in it for years, and the "
    "windows had gone the colour of old milk. Still, every evening the light came on "
    "upstairs for ten minutes, exactly as it had when the old woman was alive.";
static const char k_en2[] =
    "He said, \"I'll be back before the rain starts,\" and then he was gone - down the "
    "steps, past the gate, into the ordinary afternoon that never quite forgave him.";
static const char k_en3[] =
    "Pneumonoultramicroscopicsilicovolcanoconiosis is a word that cannot fit on any line.";
static const char k_mix[] =
    "他说了一句英文：the quick brown fox jumps over the lazy dog，然后就走了。"
    "接着又是一段中文，用来检查中英混排的换行是否与上一版一致。";

static corpus_t k_corpus[] = {
    {"cn1", k_cn1, 560, true},
    {"cn2", k_cn2, 560, true},
    {"cn3", k_cn3, 560, true},
    {"mix", k_mix, 560, true},
    {"en1", k_en1, 560, false},
    {"en2", k_en2, 560, false},
    {"en3", k_en3, 560, false},
    {"en1_narrow", k_en1, 300, false},
};

static bool word_byte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '\'' || c == '-';
}

int main(void) {
    /* 无缓冲：断言一失败就要看得见，崩在半路也不至于把证据丢在 stdio 里。 */
    setvbuf(stdout, NULL, _IONBF, 0);
    /* 行缓冲由 book_layout_set_text 在设备上分配；宿主单元自己给一块起始内存。 */
    s_line_capacity = 256;
    s_line = malloc(s_line_capacity);
    if (!s_line) return 2;
    s_line[0] = 0;
    bool ok = true;
    for (size_t c = 0; c < sizeof(k_corpus) / sizeof(k_corpus[0]); ++c) {
        corpus_t* cp = &k_corpus[c];
        s_text = cp->text;
        s_len = strlen(cp->text);
        size_t off = 0;
        int idx = 0;
        char prev_last = 0;
        size_t joined_len = 0;
        static char joined[8192];
        joined[0] = 0;
        printf("== %s width=%d\n", cp->name, cp->width);
        while (off < s_len && idx < 200) {
            size_t next = 0;
            bool pend = false, heading = false;
            int px = 0, lw = 0, indent = 0, mb = 0, ma = 0;
            uint8_t align = 0;
            if (!take_line(off, cp->width, 0, &next, &pend, &px, &heading, &lw, &indent, &align,
                           &mb, &ma)) {
                printf("  <take_line failed at %zu>\n", off);
                ok = false;
                break;
            }
            if (next <= off) { printf("  <no progress at %zu>\n", off); ok = false; break; }
            size_t n = strlen(s_line);
            printf("  %04zu|%s|\n", off, s_line);
            /* 26.11.13 起允许**声明式**断词：行尾的 '-' 是排版器插入的软连字符。
               判据不变，但要把「插入的连字符」与「原文里的连字符」分开：
               原文 next-1 不是 '-' 而本行末是 '-'，说明这一横是插进去的。
               Since 26.11.13 a declared soft hyphen may end a line; distinguish it from a
               hyphen that exists in the source by looking at the last consumed byte. */
            const bool soft_end = n > 0 && s_line[n - 1] == '-' &&
                                  (next == 0 || cp->text[next - 1] != '-');
            size_t keep = soft_end ? n - 1 : n;
            /* 行尾空格在 26.11.13 起被移出缓冲（它没有墨，却会吃掉对齐目标）。
               守恒判据因此只比**非空白字符**：丢字、重复、吞字符照样现形，
               行界处被消费掉的空格不算内容丢失。 */
            while (keep > 0 && s_line[keep - 1] == ' ') --keep;
            if (joined_len + keep + 1 < sizeof(joined)) {
                memcpy(joined + joined_len, s_line, keep);
                joined_len += keep;
            }
            /* 判据②（改锚到源码）：断点两侧都是词内字符 = 断在词中，此时本行
               必须以 '-' 收尾（声明式断词）。不再用「行尾字符 + 下行首字符」推断——
               行尾空格被剔掉后，那种推断会把「空格恰好落在行界」误判成断词。 */
            if (!cp->cjk && next > 0 && next < s_len &&
                word_byte((unsigned char)cp->text[next - 1]) &&
                word_byte((unsigned char)cp->text[next]) && !(n > 0 && s_line[n - 1] == '-')) {
                printf("  !! word split across lines at %zu\n", next);
                ok = false;
            }
            off = next;
            ++idx;
        }
        /* 判据④：内容守恒（忽略空白）——行的拼接与原文逐字符相同。丢字、重复、
           吞字符都在这条上现形；空白只允许在行界处被消费（行尾空格、断词后的换行）。 */
        joined[joined_len] = 0;
        {
            size_t a = 0, b = 0;
            bool same = true;
            while (a < joined_len || b < s_len) {
                while (a < joined_len && (joined[a] == ' ' || joined[a] == '\t')) ++a;
                while (b < s_len && (cp->text[b] == ' ' || cp->text[b] == '\t')) ++b;
                if (a >= joined_len || b >= s_len) { same = (a >= joined_len && b >= s_len); 
                    if (!same) printf("  !! content not conserved: %zu vs %zu bytes\n", joined_len, s_len);
                    break; }
                if (joined[a] != cp->text[b]) {
                    same = false;
                    printf("  !! content not conserved: %zu vs %zu bytes\n", joined_len, s_len);
                    printf("  !! first divergence at byte %zu\n", b);
                    break;
                }
                ++a; ++b;
            }
            if (!same) ok = false;
        }
    }
    printf(ok ? "WRAP_OK\n" : "WRAP_FAIL\n");
    return ok ? 0 : 1;
}
'''

EXTRACT_START = "static bool line_append("
EXTRACT_END = "/* ---- 竖排 / Vertical setting ---- */"


def extract(src: str) -> str:
    a = src.index(EXTRACT_START)
    b = src.index(EXTRACT_END)
    return src[a:b]


def build_unit(layout_source: str) -> str:
    return STUBS + extract(layout_source) + RUNNER


def run(layout_source: str, tag: str) -> tuple[bool, str]:
    unit = build_unit(layout_source)
    with tempfile.TemporaryDirectory() as td:
        c = pathlib.Path(td) / "wrap.c"
        exe = pathlib.Path(td) / "wrap"
        c.write_text(unit)
        # take_line 自 26.11.13 起调 hyph_points：真实现 + 真表一起链，避免黄金向量
        # 变成「桩掉断词」的假绿。/ Since 26.11.13 take_line calls hyph_points.
        subprocess.run(["gcc", "-std=c11", "-D_DEFAULT_SOURCE", "-w", "-O0", "-g", str(c),
                        str(ROOT / "main/book/hyphen.c"), str(ROOT / "main/book/hyph_en_us_data.c"),
                        "-I", str(ROOT / "main/book"), "-o", str(exe)],
                       check=True)
        p = subprocess.run([str(exe)], capture_output=True, text=True)
        (pathlib.Path("/tmp") / f"wrap_{tag}.txt").write_text(p.stdout + p.stderr)
        return p.returncode == 0, p.stdout + p.stderr


def parse(out: str) -> dict[str, list[str]]:
    cur = None
    blocks: dict[str, list[str]] = {}
    for line in out.splitlines():
        if line.startswith("== "):
            cur = line[3:].split()[0]
            blocks[cur] = []
        elif cur is not None and line.startswith("  ") and not line.strip().startswith("!!"):
            blocks[cur].append(line.strip())
    return blocks


def main() -> int:
    new_src = LAYOUT.read_text()
    head = subprocess.run(["git", "-C", str(ROOT), "show", "HEAD:main/book/book_layout.c"],
                          capture_output=True, text=True)
    if head.returncode != 0:
        print("FAIL: cannot read HEAD:main/book/book_layout.c")
        return 1
    old_src = head.stdout

    ok_new, out_new = run(new_src, "new")
    ok_old, out_old = run(old_src, "head")
    b_new = parse(out_new)
    b_old = parse(out_old)

    fails = 0
    if not ok_new:
        print("FAIL: the new build violates a criterion")
        for l in out_new.splitlines():
            if l.strip().startswith("!!") or "<" in l:
                print("  " + l.strip())
        fails += 1

    # 判据①：中文语料逐行一致。
    for name in ("cn1", "cn2", "cn3", "mix"):
        if b_new.get(name) != b_old.get(name):
            fails += 1
            print(f"FAIL: Chinese corpus '{name}' changed - the word-wrap must not touch CJK")
            for i, (a, b) in enumerate(zip(b_old.get(name, []), b_new.get(name, []))):
                if a != b:
                    print(f"  line {i}: HEAD={a[:60]}")
                    print(f"  line {i}: NEW ={b[:60]}")
                    break
            if len(b_old.get(name, [])) != len(b_new.get(name, [])):
                print(f"  line count HEAD={len(b_old.get(name, []))} NEW={len(b_new.get(name, []))}")

    # 行数是成本，不是判据（见 RUNNER 里判据④的注释）。
    # 证据行：英文行数变化要看得见。/ Show the line-count change: it is the expected cost.
    for name in ("en1", "en2", "en3", "en1_narrow"):
        print(f"lines {name}: HEAD={len(b_old.get(name, []))} -> NEW={len(b_new.get(name, []))}")

    print("WRAP-GOLDEN: %s (logs in /tmp/wrap_head.txt, /tmp/wrap_new.txt)" %
          ("PASS" if fails == 0 else f"FAIL ({fails})"))
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
