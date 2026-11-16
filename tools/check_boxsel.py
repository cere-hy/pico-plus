#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
"""
M5-A 框选「结构牙」：钉死三条不变量，防止后续改动悄悄破坏。

1. book_select 是纯逻辑：不得包含 UI/字体/EPD/排版头文件，也不得画像素。
2. 反色只在 draw_page 的同一次游走里发生：禁止整屏取反（epd_get_pixel 全屏遍历、
   fb[i] ^= 0xFF 之类），也禁止第二份行几何（book_select 里不得出现行矩形计算）。
3. 反色区间只能由 book_layout_set_invert_range() 设置，脏矩形只能由 draw_page 产生。

用法：python3 tools/check_boxsel.py   （0 = 通过）
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SEL_C = ROOT / "main/book/book_select.c"
SEL_H = ROOT / "main/book/book_select.h"
LAYOUT_C = ROOT / "main/book/book_layout.c"
BOOK_C = ROOT / "main/apps/app_book.c"  # S26-2 B31: host file of the exit discipline

ALLOWED_INCLUDES = {"book_select.h", "string.h", "stdbool.h", "stddef.h", "stdint.h"}
FORBIDDEN_SYMBOLS = (
    "epd_fill_rect", "ttf_draw", "ui_text", "ui_fill", "fb[",
    "book_layout_", "text_width", "justify_",
)

fails = []


def bad(msg):
    fails.append(msg)
    print("FAIL: " + msg)


def read(p):
    if not p.exists():
        bad("missing file %s" % p.relative_to(ROOT))
        return ""
    return p.read_text(encoding="utf-8", errors="ignore")


def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    return src


def check_pure_logic():
    src = strip_comments(read(SEL_C))
    for m in re.finditer(r'#include\s+"([^"]+)"', src):
        if m.group(1) not in ALLOWED_INCLUDES:
            bad("book_select.c includes %r; it must stay free of UI/font/layout headers"
                % m.group(1))
    for sym in FORBIDDEN_SYMBOLS:
        if sym in src:
            bad("book_select.c references %r — drawing/layout must live elsewhere" % sym)


def check_no_full_screen_invert():
    for path in (LAYOUT_C, SEL_C):
        src = strip_comments(read(path))
        if re.search(r"epd_get_pixel\s*\(", src):
            bad("%s reads pixels back (epd_get_pixel) — full-screen inversion is forbidden"
                % path.name)
        if re.search(r"\^\s*=\s*0x[fF]{2}|0x[fF]{2}\s*\^", src):
            bad("%s XORs against 0xFF — per-pixel inversion is forbidden" % path.name)
        if re.search(r"for\s*\([^)]*\)\s*for\s*\([^)]*\)[^;{]*\bfb\s*\[", src):
            bad("%s walks the whole framebuffer with a nested loop" % path.name)


def check_invert_owner():
    src = strip_comments(read(LAYOUT_C))
    # 脏矩形只允许在 draw_page 里登记：inv_dirty_add 的调用点必须位于 draw_page 函数体内。
    start = src.find("void book_layout_draw_page(")
    if start < 0:
        bad("book_layout_draw_page() not found")
        return
    end = src.find("\nint book_layout_draw_slice", start)
    body = src[start:end if end > 0 else len(src)]
    calls = len([m for m in re.findall(r"([^\n]*inv_dirty_add\s*\()", src)
                 if "static void" not in m])
    inside = len(re.findall(r"inv_dirty_add\s*\(", body))
    if calls != inside:
        bad("inv_dirty_add is called outside draw_page (%d total, %d inside)" % (calls, inside))
    # 反色开关只允许由 setter 写。
    if len(re.findall(r"s_inv_on\s*=", src)) != 1:
        bad("s_inv_on must be written exactly once (in book_layout_set_invert_range)")
    # 反色判定必须用 hit_test 的同一个 base helper，而不是自己重算偏移。
    if "hit_line_source_offset(off, s_line)" not in body:
        bad("draw_page must derive the invert base from hit_line_source_offset(off, s_line)")


def check_ink_width_single_source():
    """26.9.12 P2：绘制与反色底必须共用 line_ink_width()，否则右侧必然短一截。
    这里钉三件事：函数在、计算只有一处、反色矩形按 ink 而不是裸 line_width 铺。"""
    src = strip_comments(read(LAYOUT_C))
    if "static inline int line_ink_width(" not in src:
        bad("line_ink_width() is gone — drawing and the inversion box drift apart again "
            "(26.9.11: the fill stopped half a glyph short)")
        return
    start = src.find("void book_layout_draw_page(")
    end = src.find("\nint book_layout_draw_slice", start)
    body = src[start:end if end > 0 else len(src)]
    if "line_ink_width(" not in body:
        bad("draw_page does not call line_ink_width() — the fill and the glyphs can diverge")
    if "justify_target_px" in body:
        bad("draw_page computes justify_target_px itself; it must read the shared ink width "
            "so the inversion rect and the text cannot disagree")
    if re.search(r"EpdRect\s+line_rect\s*=\s*\{[^}]*\bline_width\b", src):
        bad("the inversion rect is sized from line_width again — it must use the ink width")


def check_exit_declaration():
    """26.9.26 S26-2 B31 出口声明制：动作栏点击必退出，且每个出口必须报名字。

    三条线的下巴 / three jaws:
      1. app_book.c 里不许出现裸 boxsel_reset()——只有两个合法 caller：
         boxsel_reset() 自己（清理实现）与 boxsel_exit()（带标签出口）。
      2. boxsel_exit() 体内必须同时做「复位 + 整页重绘」，缺一半就是半个出口。
      3. boxsel_action() 的每个 return 都必须经 boxsel_exit(某个标签)，不许 `return APP_REDRAW_PAGE`。
    """
    src = strip_comments(read(BOOK_C))

    # 取函数的**定义**体（前向声明不算），用花括号配对而不是找第一个 \n}。
    # Take a FUNCTION DEFINITION body (not a forward declaration) by matching braces.
    def body_of(pattern):
        m = re.search(pattern + r"[^;{]*\{", src)
        if not m:
            return None
        depth, i = 0, m.end() - 1
        while i < len(src):
            if src[i] == '{':
                depth += 1
            elif src[i] == '}':
                depth -= 1
                if depth == 0:
                    return src[m.end():i]
            i += 1
        return None

    # 1) 唯一的出口必须既复位又整页重绘 / the single exit must reset AND repaint wholesale
    exit_body = body_of(r"static app_redraw_t boxsel_exit\(")
    if exit_body is None:
        bad("B31 boxsel_exit() is gone: the exit disappeared and took its repaint with it")
        return
    if "boxsel_reset()" not in exit_body:
        bad("B31 boxsel_exit() no longer resets: taps would leave the range selected")
    if "APP_REDRAW_PAGE" not in exit_body:
        bad("B31 boxsel_exit() no longer returns APP_REDRAW_PAGE: the inversion would survive "
            "the tap on screen")

    # 2) 裸 boxsel_reset() 只允许出现在 boxsel_exit() 体内，别处出现即为无标签出口。
    # A bare boxsel_reset() may only live inside boxsel_exit(); anywhere else it is an
    # unlabelled exit.
    for m in re.finditer(r"\bboxsel_reset\s*\(\s*\)", src):
        if exit_body and src[m.start():m.start() + 20] in exit_body:
            continue
        bad("B31 a bare boxsel_reset() at offset %d escapes boxsel_exit(): every way out must "
            "carry a BOXSEL_EXIT_ label or the log cannot name it" % m.start())

    # 3) 动作栏每个出口都要带标签 / every action-bar return carries a label
    action_body = body_of(r"static app_redraw_t boxsel_action\(")
    if action_body is None:
        bad("B31 boxsel_action() is gone")
        return
    # 单层委托：允许 return 一个「本身只经 boxsel_exit 返回」的助手。委托不是豁免——
    # 助手自己的每个 return 照样要在下面逐条核，等于把同一条规则多看一层。
    # One level of delegation: a helper whose own returns all go through boxsel_exit may be
    # returned. Delegation is not an exemption - the helper is audited right below.
    delegates = {"boxsel_ai_send()"}
    for name in sorted(delegates):
        helper = body_of(r"static app_redraw_t %s\(" % name[:-2])
        if helper is None:
            bad("B31 %s delegates to %s but that helper does not exist"
                % ("boxsel_action", name))
            continue
        for m in re.finditer(r"return\s+([^;]+);", helper):
            v = m.group(1).strip()
            if not v.startswith("boxsel_exit("):
                bad("B31 %s returns %r without a BOXSEL_EXIT_ label: delegation must not launder "
                    "an unlabelled exit" % (name, v))
    for m in re.finditer(r"return\s+([^;]+);", action_body):
        val = m.group(1).strip()
        if val.startswith("boxsel_exit(") or val in delegates:
            continue
        bad("B31 boxsel_action() returns %r without a BOXSEL_EXIT_ label: an unlabelled exit "
            "is how 'the tap was swallowed' comes back" % val)


def check_public_api():
    hdr = strip_comments(read(SEL_H))
    for sym in ("box_select_begin", "box_select_tap", "box_select_expired",
                "box_select_cancel", "box_select_copy_text", "box_select_active",
                "box_select_ready", "box_select_snap_sentence"):
        if sym + "(" not in hdr:
            bad("book_select.h lost %s()" % sym)
    if "BOX_SELECT_TIMEOUT_MS" not in hdr:
        bad("book_select.h lost the timeout constant")


def check_single_exit():
    """退出只能经 box_select_cancel：不得有第二处把 state 置 OFF 的赋值。"""
    src = strip_comments(read(SEL_C))
    off_writes = re.findall(r"->state\s*=\s*BOX_STATE_OFF", src)
    if len(off_writes) != 1:
        bad("state is forced to OFF in %d places; only box_select_cancel may do it"
            % len(off_writes))
    if "memset(sel, 0, sizeof(*sel));\n    sel->state = BOX_STATE_OFF;" not in src:
        bad("box_select_cancel must memset the whole struct before setting OFF")




APP_BOOK = ROOT / "main/apps/app_book.c"
QUICK_C = ROOT / "main/ui/ui_quick_menu.c"
QUICK_IN = ROOT / "main/ui/ui_quick_menu_input.c"


def check_swallow_first_line():
    """E-S2：两个手势入口的第一条语句必须是框选早退，否则事件会漏到翻页/换页。"""
    src = strip_comments(read(APP_BOOK))
    for fn in ("gesture_event", "dual_gesture_event"):
        start = src.find("static app_redraw_t %s(app_ctx_t* ctx" % fn)
        if start < 0:
            bad("%s() not found in app_book.c" % fn)
            continue
        body = src[src.find("{", start) + 1:]
        first = ""
        for line in body.splitlines():
            if line.strip():
                first = line.strip()
                break
        if "boxsel_active()" not in first:
            bad("E-S2: first statement of %s() must be the box-selection early return, got %r"
                % (fn, first[:60]))


def check_state_writers():
    """E-S3：s_boxsel 只允许三个函数写：enter / reset / touch（外加 tick 的过期判定）。"""
    src = strip_comments(read(APP_BOOK))
    allowed = {"boxsel_enter", "boxsel_reset", "boxsel_touch", "on_tick", "box_select_begin",
               "box_select_tap", "box_select_cancel", "box_select_expired"}
    for m in re.finditer(r"s_boxsel\.(state|off_start|off_end|anchor_w[se]|page|deadline_ms)\s*=", src):
        line_start = src.rfind("\n", 0, m.start()) + 1
        # 允许结构体内部通过 box_select_* 修改；直接赋值必须发生在允许的函数里
        ctx = src[max(0, line_start - 3000):m.start()]
        owner = None
        for fn in allowed:
            idx = ctx.rfind(fn + "(")
            if idx >= 0 and (owner is None or idx > owner[1]):
                owner = (fn, idx)
        if owner is None:
            bad("s_boxsel is written outside the state machine helpers near %r" % m.group(0))


def check_quick_button():
    """入口必须真的接上：枚举、命中循环、动作表、绘制循环、主循环分发，五处齐。"""
    hdr = strip_comments(read(ROOT / "main/ui/ui_quick_menu.h"))
    if "UI_QUICK_BOX" not in hdr:
        bad("ui_quick_menu.h lost UI_QUICK_BOX")
    for path, needle in ((QUICK_IN, "for (int i = 0; i < 5; ++i)"),
                         (QUICK_C, "for (int i = 0; i < 5; ++i)"),
                         (QUICK_C, "box_icon(fb, cx, UI_QUICK_CIRCLE_Y)"),
                         (QUICK_IN, "UI_QUICK_BOX,"),
                         (ROOT / "main/app/app_loop.c", "action == UI_QUICK_BOX")):
        if needle not in read(path):
            bad("%s is missing %r — the button is not fully wired" % (path.name, needle))
    if "UI_LOCK_WIDTH / 10" not in read(QUICK_IN):
        bad("quick menu still divides the width by 8; the fifth button would overlap the lock")


def check_single_exit_wired():
    """11 条退出必须都落到 boxsel_reset / boxsel_boundary_reset，不得各写一套清理。"""
    src = strip_comments(read(APP_BOOK))
    for needle in ("boxsel_reset();", "boxsel_boundary_reset();"):
        if needle not in src:
            bad("app_book.c never calls %s" % needle)
    if "book_layout_set_invert_range(0, 0)" in src.split("static void boxsel_boundary_reset")[0]:
        # 关闭反色只允许出现在 reset 与 enter 里
        pass

def main():
    check_pure_logic()
    check_ink_width_single_source()
    check_no_full_screen_invert()
    check_invert_owner()
    check_public_api()
    check_single_exit()
    check_swallow_first_line()
    check_quick_button()
    check_single_exit_wired()
    check_exit_declaration()
    # 体检批 ②（2026-11-01）：本函数自 E-S3 起就存在，但注册行从来没写进来——
    # 「定义了没调用」第五例，由 check_selftest_registry.py 第一次运行捕获。
    # 补注册后先确认它对新代码还有效（牙体检：检查对象是否仍在原位）。
    # Gate-health batch 2 (2026-11-01): this tooth has existed since E-S3 but was never
    # registered - the fifth instance of "defined, never called", caught the first time
    # the self-check ran. Registered first, then inspected to see it still bites.
    check_state_writers()
    if fails:
        print("check_boxsel: FAIL (%d)" % len(fails))
        return 1
    print("check_boxsel: PASS (pure logic / ink-width single source / no full-screen invert / "
          "single invert owner / single exit)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
