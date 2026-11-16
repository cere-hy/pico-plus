#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
"""
排版牙（[123]，与刷新牙分开成套件——套件跟域走）：折行只有一份、对齐只有一份计划。

判据纪律（先把「为什么是这几条」说清，否则下一批会把它们当噪声删掉）：

T1  引用条**不许有第二套折行**。`scene_draw_quote` 曾自写「逐码位累加、超了就断」，
    把 `Isosceles` 断成 `I`/`sosceles`、`appreciate` 断成 `apprecia`。它必须调
    `wrap_line_len`，函数体内不得再出现宽度比较循环。
T2  折行规则**只有一处实现**（`used_at_space * 5 >= width_px * 3` 的 60% 版心门）。
    正文与引用条只差度量回调，规则不复制。
T3  两端对齐**只有一份计划**（`ttf_justify_plan`，在 ttf_font.h）：绘制与点词命中
    都调它。`cjk_gaps` 那个只数汉字间隙的变量正是「英文永远不齐」的根。
T4  同 T3，命中侧（`hit_line_fit_init`）不许再自己算一遍。
T5  间隙判据：一个空格只算**一次**（算在空格之后）。写成「前后都算」会让间隙数翻倍，
    每个间隙摊到的量减半，看起来齐、实际差一半。
T6  断词接线：`take_line` 必须真的调 `hyph_points` 并插入 `-`。
T7  行尾空格必须从行缓冲与宽度里去掉——它没有墨，却会吃掉对齐目标。
T8  natural 必须含 tracking（`tracking_px * (glyphs - 1)`）：少了它，用户一开字距，
    预算系统性错算，那正是「假齐」的代码面。
T9  `line_len_at` 退化成 `wrap_line_len` 的薄包装，不许再有第二份实现。
T10 门 1 必须在出包四步锁序里，且判据是**零容差**（`!=`），不是 `<= 1`。

用法：python3 tools/check_typography.py   （0 = 通过）
"""
from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
AI = ROOT / "main/apps/ai_reader.c"
TTF = ROOT / "main/font/ttf_font.c"
TTFH = ROOT / "main/font/ttf_font.h"
JUSTIFY = ROOT / "main/font/ttf_justify.h"
LAYOUT = ROOT / "main/book/book_layout.c"
GATE = ROOT / "tools/typography_gate1.py"
RELEASE = ROOT / "tools/release_gate.sh"

fails = 0


def bad(msg: str) -> None:
    global fails
    fails += 1
    print("FAIL: " + msg)


def ok(msg: str) -> None:
    print("ok   " + msg)


def strip_comments(src: str) -> str:
    """注释替换成空格（保留换行、字符串原样）。判据只认代码，注释不算数。"""
    out, i, n, state = [], 0, len(src), None
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if state is None:
            if c == "/" and nxt == "/":
                state, i = "line", i + 2
                out.append("  ")
                continue
            if c == "/" and nxt == "*":
                state, i = "block", i + 2
                out.append("  ")
                continue
            if c == '"':
                state = "str"
            elif c == "'":
                state = "chr"
            out.append(c)
            i += 1
            continue
        if state == "line":
            if c == "\n":
                state = None
                out.append(c)
            else:
                out.append(" ")
            i += 1
            continue
        if state == "block":
            if c == "*" and nxt == "/":
                state, i = None, i + 2
                out.append("  ")
                continue
            out.append(c if c == "\n" else " ")
            i += 1
            continue
        if c == "\\":
            out.append(c)
            if nxt:
                out.append(nxt)
            i += 2
            continue
        if (state == "str" and c == '"') or (state == "chr" and c == "'"):
            state = None
        out.append(c)
        i += 1
    return "".join(out)


def body(src: str, needle: str) -> str:
    """从 `needle` 起到配平的大括号为止（注释已剥掉，字符串里的大括号要跳过）。"""
    i = src.index(needle)
    j = src.index("{", i)
    depth = 0
    k = j
    while k < len(src):
        if src[k] == "{":
            depth += 1
        elif src[k] == "}":
            depth -= 1
            if depth == 0:
                return src[j:k + 1]
        k += 1
    raise ValueError(f"unbalanced braces after {needle}")


def main() -> int:
    ai = strip_comments(AI.read_text(encoding="utf-8"))
    ttf = strip_comments(TTF.read_text(encoding="utf-8"))
    ttfh = strip_comments(TTFH.read_text(encoding="utf-8"))
    just = strip_comments(JUSTIFY.read_text(encoding="utf-8"))
    lay = strip_comments(LAYOUT.read_text(encoding="utf-8"))

    # T1 引用条单源
    q = body(ai, "static void scene_draw_quote(")
    if "wrap_line_len(" not in q:
        bad("T1 scene_draw_quote 没有走 wrap_line_len（第二套折行又回来了）")
    elif re.search(r">\s*avail\b", q):
        bad("T1 scene_draw_quote 里仍有「累计宽度 > 版心」的判宽循环")
    else:
        ok("T1 引用条折行走单一实现（无自写判宽循环）")

    # T2 规则唯一
    if ai.count("used_at_space * 5 >= width_px * 3") != 1:
        bad("T2 60% 版心门不是恰好一处（折行规则被复制了）")
    elif ai.count("static size_t wrap_line_len(") != 1:
        bad("T2 wrap_line_len 定义不唯一")
    else:
        ok("T2 折行规则只有一份实现")

    # T3 绘制用计划
    f = body(ttf, "int ttf_draw_text_px_fitted(")
    if "ttf_justify_plan(" not in f:
        bad("T3 fitted 绘制没有用 ttf_justify_plan")
    elif "cjk_gaps" in ttf:
        bad("T3 ttf_font.c 仍存在 cjk_gaps（只数汉字间隙的旧判据）")
    elif "adjustable" in ttf:
        bad("T3 ttf_font.c 仍有 adjustable 旧变量")
    else:
        ok("T3 绘制使用唯一对齐计划（无 cjk_gaps / adjustable）")

    # T4 命中用计划
    h = body(lay, "static void hit_line_fit_init(")
    if "ttf_justify_plan(" not in h:
        bad("T4 点词命中没有复用 ttf_justify_plan")
    else:
        ok("T4 点词命中复用同一份计划")

    # T5 空格只算一次
    g = body(just, "static inline bool ttf_justify_gap(")
    if not re.search(r"if \(cp == ' ' \|\| cp == '\\t'\) return true;", g):
        bad("T5 间隙判据缺少「空格之后算一次」")
    elif not re.search(r"if \(next == ' ' \|\| next == '\\t'\) return false;", g):
        bad("T5 间隙判据没有排除「空格之前」的重复计数")
    else:
        ok("T5 一个空格只计一个间隙")

    # T6 断词接线
    t = body(lay, "static bool take_line(")
    if "hyph_points(" not in t:
        bad("T6 take_line 没有调用 hyph_points（断词没接线）")
    elif 'line_append(cut - visible_off, "-", 1)' not in t:
        bad("T6 take_line 没有把连字符插进行缓冲")
    else:
        ok("T6 断词已接线（断点 + 连字符）")

    # T7 行尾空格
    if "s_text[keep - 1] == ' '" not in t:
        bad("T7 take_line 没有把行尾空格移出对齐目标")
    else:
        ok("T7 行尾空格从宽度中剔除")

    # T8a 计划头必须被真头与桩头共用（单源，不许两边各抄一份）
    if '#include "ttf_justify.h"' not in ttfh:
        bad("T8a ttf_font.h 没有包含 ttf_justify.h（计划被抄成第二份）")
    else:
        ok("T8a 计划头与真头同源")

    # T8 natural 含 tracking
    if not re.search(r"tracking_px \* \(glyphs - 1\)", f):
        bad("T8 fitted 的 natural 未含 tracking 项")
    else:
        ok("T8 natural 含 tracking（预算口径正确）")

    # T9 line_len_at 是薄包装
    ll = body(ai, "static size_t line_len_at(")
    if "wrap_line_len(" not in ll or "last_space" in ll:
        bad("T9 line_len_at 不是薄包装（存在第二份折行实现）")
    else:
        ok("T9 line_len_at 只是 wrap_line_len 的包装")

    # T10 门 1 在锁序里 + 零容差
    rel = RELEASE.read_text(encoding="utf-8")
    gate = GATE.read_text(encoding="utf-8")
    if "typography_gate1.py" not in rel:
        bad("T10 出包四步锁序里没有门 1")
    elif "d_fit != m" not in gate or "d_plain != m" not in gate:
        bad("T10 门 1 的判据不是零容差（!=）")
    else:
        ok("T10 门 1 在锁序内且判据零容差")

    # T11 三端同尺：布局累加也必须带 kern（只改字体层，行宽会比画出来的窄）
    kern_in_layout = t.count("ttf_kern_px(")
    if "width + ttf_text_width_px(*px, glyph)" not in t.replace("\n", " ") and \
       "ttf_kern_px(*px, prev_cp, cp)" not in t:
        bad("T11 take_line 的候选宽度没有加 kern（布局与绘制不同源）")
    elif kern_in_layout < 2:
        bad("T11 布局侧 kern 只接了一处（正文累加与断词段宽都要接）")
    else:
        ok("T11 布局累加带 kern（与度量/绘制同尺）")

    # T12 降级守卫：非内置字体必须静默无 kern（[124] 细则 1）
    k = body(ttf, "int ttf_kern_px(")
    if "ttf_font_path_is_builtin(" not in k:
        bad("T12 ttf_kern_px 没有「非内置字体返回 0」的守卫（换字体会按错字形调字距）")
    elif "stbtt_FindGlyphIndex" not in k:
        bad("T12 ttf_kern_px 不查字形 id（键是字形 id，不是码位）")
    else:
        ok("T12 非内置字体静默无 kern（降级有个门）")

    # T13 排版开关：一个开关管三条路径，且默认开（半开态没有历史基线可比）
    sw = (ROOT / "main/font/typography_switch.h").read_text(encoding="utf-8")
    st = (ROOT / "main/settings.c").read_text(encoding="utf-8")
    if "bool app_typography_enhanced(void)" not in sw:
        bad("T13 排版开关头里没有 app_typography_enhanced 声明")
    elif "static bool s_typography_enhanced = true;" not in st:
        bad("T13 开关默认值不是 true（升级上来的设备就失去了增强排版）")
    elif "NVS_KEY_TYPO_ENHANCED" not in st or "typo_enh" not in st:
        bad("T13 开关没有 NVS 持久化（重启失效）")
    else:
        uses = (ttf.replace("\n", " ").count("app_typography_enhanced()"))
        lay = (t.replace("\n", " ").count("app_typography_enhanced()"))
        if uses < 2:
            bad(f"T13 字体层只接了 {uses} 处开关（justify 与 kern 都要接）")
        elif lay < 2:
            bad(f"T13 布局层只接了 {lay} 处开关（断词与行尾空格剔除都要接）")
        else:
            ok(f"T13 一个开关三路径（字体 {uses} 处 / 布局 {lay} 处）且默认开、可持久化")

    print(f"\ncheck_typography: {'PASS' if not fails else f'FAIL ({fails})'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
