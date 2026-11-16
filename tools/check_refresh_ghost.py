#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
"""
B79/B80 结构牙：阅读页刷新必须是「翻页局刷 / 周期清底 / 大动作全刷」三层。

历史（必须先读，否则会把它改回错误的方向）：

26.11.4 真机报残影，定性为**路径结构**问题——阅读页整页出口走的是 26.9.26 之前就在的
整屏差分，旧墨没人擦。26.11.5 的刀 B 把正文翻页改成「区域全像素 GC16」修掉了残影，
但同一份字段账只量了时间（GL16 37 帧 467 ms vs GC16 36 帧 452 ms），漏了视觉：
全像素让整块正文区每页走一遍「黑→白→目标」，用户看到的是每翻一页闪黑闪白。
真机两代日志的翻页耗时是 466 ms 对 468 ms，差 2 ms——**全刷的代价不在秒表上**。

所以判据从「时间」改成「闪账 + 灰账」，牙也从「翻页必须全刷」反转成「翻页必须局刷」：

B79a  阅读视图的整页出口（进入 / 跳章 / 跳页 / 换书）必须 `update_display_full(`，
      且必须有一条 `s_reader_body_turn` 的差分旁路（AI 解析页翻页走那条）。
B79b  （已由 B82a 取代，26.11.8）正文翻页的判据随 ② 摘取反转：
      26.11.7 曾要求 GC16 + 差分，真机三档同炸（闪 / 左缘竖带 / 反色停留），
      26.11.8 改回「柔和波形 × 全覆盖」——见下方 B82a。
B79c  跨视图进出阅读必须升到 `APP_REDRAW_FULL`（视图边沿判据，五条入口一次覆盖）。
B79d  反向判据：页脚走 DU、时钟走 GL16 差分——小面积路径不许借整页节拍。
B80a  周期清底必须存在：`s_reader_cleanup` 与 `APP_REDRAW_FULL` 同一出口 `update_display_full`。
B80b  跨章必须清底并归零：`turn_page()` 里 `chapter_changed` 进入 `s_reader_cleanup`，
      且 `s_turns = 0`（不变式：任何整屏清底后计数归零）。
B80c  AI 解析页翻页必须过账：`reader_ai_turn()` 置 `s_reader_body_turn` 且调
      `reader_note_turn()`——否则「连看 20 页解析」是零清底死角。
B82a  正文翻页必须 `_area_full_with(display_reader_soft_turn_waveform(), MODE_GL16, …)`；
      **禁止** `_diff_with`、`MODE_GC16`、直接点名 `E0470_WAVEFORM`。
B82c  `main/display_reader_waveform.c` 与 kiko `ae015054` **除两处声明偏离外逐字节相同**
      （归一化 blob `63a955a8`），且 `READER_SOFT_TURN_SETTLE_EXTRA == 0`——旧版复活、
      手工微调、顺手优化、把 12 相改回来，全部咬死。

全部在切出来的函数体内判，不是整文件子串：`app_book.c` 里 `update_display_mode_diff(`
在灰度插图分支上还有一处**合法**调用，整文件匹配会把它误判成缺陷，也会让「有人把它
挪进阅读整页出口」这类改动查不出来。

用法：python3 tools/check_refresh_ghost.py   （0 = 通过）
"""
import hashlib
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
APP_BOOK = ROOT / "main/apps/app_book.c"

fails = []


def bad(msg):
    fails.append(msg)
    print("FAIL: " + msg)


def ok(msg):
    print("ok   " + msg)


def strip_comments(src):
    """把注释替换成等长空格（保留换行），字符串/字符字面量原样保留。

    两个理由：① 本仓库注释极密，且注释里常出现 `{`/`}`（坐标、集合写法），
    按大括号切片会切歪；② 判据只该认**代码**，注释里写着 `update_display_full(`
    不算修好了——「在场 ≠ 生效」在牙自己身上同样成立。
    Two reasons: comments here are dense and carry braces (coordinates, set notation),
    which derails brace slicing; and only code counts - a comment mentioning
    `update_display_full(` is not a fix. "Present is not effective" applies to teeth too.
    """
    out = []
    i, n = 0, len(src)
    state = None  # None | 'line' | 'block' | 'str' | 'chr'
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if state is None:
            if c == "/" and nxt == "/":
                state = "line"
                out.append("  ")
                i += 2
                continue
            if c == "/" and nxt == "*":
                state = "block"
                out.append("  ")
                i += 2
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
                state = None
                out.append("  ")
                i += 2
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


def slice_function(text, name):
    """按大括号配对切出一个函数体（含签名）。找不到返回空串。

    `text` 必须是去注释后的代码：注释里的 `{`/`}` 会把配对算歪，切出一段鬼东西
    ——那正是「靶子是假的」。
    `text` must have comments blanked: braces inside comments skew the pairing and
    the slice becomes a fake target.
    """
    for m in re.finditer(r"^(?:static\s+)?[^\n;{}]*\b" + re.escape(name) + r"\s*\([^;{}]*?\)\s*\{",
                         text, re.M):
        at, depth, quote, escape = m.end(), 1, None, False
        while at < len(text) and depth:
            c = text[at]
            if quote:
                if escape:
                    escape = False
                elif c == "\\":
                    escape = True
                elif c == quote:
                    quote = None
            elif c in "\"'":
                quote = c
            elif c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
            at += 1
        return text[m.start():at]
    return ""


def block_after(src, anchor):
    """从 anchor 起取到与之配对的 `}`，用于判一个 else-if 分支体。"""
    i = src.find(anchor)
    if i < 0:
        return ""
    at = src.index("{", i) + 1
    depth, quote, escape = 1, None, False
    while at < len(src) and depth:
        c = src[at]
        if quote:
            if escape:
                escape = False
            elif c == "\\":
                escape = True
            elif c == quote:
                quote = None
        elif c in "\"'":
            quote = c
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        at += 1
    return src[i:at]


src = strip_comments(APP_BOOK.read_text(encoding="utf-8"))
present = slice_function(src, "present")
turn_page = slice_function(src, "turn_page")
reader_ai_turn = slice_function(src, "reader_ai_turn")
if not present:
    bad("present() not found in main/apps/app_book.c - the tooth has no target")
    print()
    print("check_refresh_ghost: 1 FAILED")
    sys.exit(1)

# ---- B79a：阅读视图整页出口 = 整屏 GC16 + 一条 body_turn 差分旁路 ----------
anchor = "else if (s_view == READING)"
body = block_after(present, anchor)
body_turn_branch = block_after(present, "} else if (s_reader_body_turn) {")
if not body:
    bad("B79a the `else if (s_view == READING)` whole-page exit is gone - re-audit before "
        "trusting the rest (this is the branch that carries open/chapter/jump)")
elif "update_display_full(" not in body:
    bad("B79a the reader's whole-page exit does not call update_display_full(): a page is "
        "presented without a screen-wide GC16, so the previous page's ink stays")
elif re.search(r"update_display_mode_diff\s*\(", body):
    bad("B79a the reader's whole-page exit is back on update_display_mode_diff(): a "
        "screen-wide differential keeps every unchanged pixel, i.e. every bit of the ghost")
elif not body_turn_branch:
    bad("B79a the `else if (s_reader_body_turn)` bypass is gone: AI detail page turns would "
        "fall into the whole-page GC16 and flash once per page - the second flash source")
elif not re.search(r"update_display_area_(full_with|diff_with|with)\s*\(", body_turn_branch):
    bad("B79a the body-turn bypass no longer refreshes anything: AI page turns fall through "
        "to the whole-page GC16 and flash once per page")
else:
    ok("B79a reader whole-page exit is a screen-wide GC16; the body-turn bypass now carries "
       "the same soft full-pixel GL16 as the body (judged by B82b)")

# ---- B82a：正文翻页 = 柔和波形 + 全像素 GL16（26.11.8 ② 摘取）-------------
#
# 26.11.7 这里是 GC16 + 差分（36 相裁剪表），真机三档同时炸：整块闪、左缘竖带、
# 反色停留。三者同源——裁剪表在差分的单次「深→白」推进里给不出足够的白。
# 26.11.8 按 kiko rc104 同款改回「柔和波形 × 全覆盖」：48 相（12 静止 + 4 黑推 +
# 尾部收），白目标优先、黑相靠后，全像素刷整个正文区。
#
# 判据四缺一不可：
#   _area_full_with          -> 全像素；差分拿不到足够的白推
#   display_reader_soft_turn -> 必须用恢复的 48 相模块，不是裁剪表
#   MODE_GL16                -> 柔和序列按 GL16 派生
#   禁 _diff_with            -> 它是 allow_gl16_diff=true 的入口（26.11.6 的错法）
#   禁 E0470_WAVEFORM/MODE_GC16 -> 回到 26.11.7 的那一行
turn = block_after(present, "} else if (s_reader_turn_pending) {")
if not turn:
    bad("B82a no `else if (s_reader_turn_pending)` branch found in present() - the body turn "
        "has no dedicated refresh path any more")
elif "update_display_area_diff_with(" in turn:
    bad("B82a the body turn is back on _diff_with: that entry sets allow_gl16_diff, which is "
        "how 26.11.6 bypassed the driver's rule that GL16 must run full-pixel to restore "
        "paper white")
elif "update_display_area_full_with(" not in turn:
    bad("B82a the body turn must call _area_full_with(): a differential cannot supply the "
        "white push, and that is what produced the 26.11.7 left-edge band")
elif "display_reader_soft_turn_waveform(" not in turn:
    bad("B82a the body turn must use display_reader_soft_turn_waveform(): the trimmed "
        "E0470_WAVEFORM table is what stopped white targets mid-transition (inverted text)")
elif "MODE_GC16" in turn:
    bad("B82a the body turn is back on MODE_GC16: GC16 orders the black phases first, i.e. "
        "one whole-block flash per page (26.11.5 / 26.11.7)")
elif "MODE_GL16" not in turn:
    bad("B82a the body turn lost MODE_GL16: the soft sequence is derived from the GL16 LUT")
elif "E0470_WAVEFORM" in turn:
    bad("B82a the body turn still names the trimmed table E0470_WAVEFORM directly - use the "
        "soft module, whose ordering is what the panel was validated against")
else:
    ok("B82a body turn is a soft-waveform full-pixel GL16 refresh (no _diff_with, no GC16, "
       "no trimmed table)")

# ---- B82c：柔和波形模块必须与 kiko ae015054 逐字节相同 ----------------------
#
# 这是本批最强的一条牙：旧版替换、手工微调、顺手优化，全部被咬。
# 26.11.7 退休掉的是 3,664 B 的旧版（水波纹用），git 历史里躺着；把它复活是这个
# 批次最容易犯的错——哈希断言防住它，段账（64*16*4 = 4,096 B；运行期 37+12+4 = 53 相，
# 不是早前口述的 48 相 / 3,072 B）防第二道。
#
# 【x0 已转正（2026-11-03）：归一化哈希 + 值断言】
#
# 本文件现在偏离 kiko，且**只允许**两处：
#   1) `#define READER_SOFT_TURN_SETTLE_EXTRA 0`（kiko 是 12）；
#   2) 一段 `// >>> 26.11.8 x0 NOTE` 注释块（来历写在代码里，门禁不许它夹带代码）。
# 归一化 = 剥掉该注释块 + 把宏值抹回 12，仍要求 blob 等于 WAVEFORM_BLOB。
# 牙的含义从「逐字节相同」收紧为「除这两处偏离外逐字节相同」：旧版复活、顺手优化、
# 换了实现但输出"看起来一样"，一样咬。
#
# 为什么还要一条值断言：归一化只抹值、不判值——少了它，把宏改回 12 反而全绿，
# 而那正是 x0 要根除的那个东西。两条合起来才闭环：**结构只能差这两处，其中值必须是 0**。
WAVEFORM_C = ROOT / "main/display_reader_waveform.c"
WAVEFORM_BLOB = "63a955a8a5aaeaaf5ac0b10d592c22ed9b7401b4"
SETTLE_DEF_RE = re.compile(rb"(#define\s+READER_SOFT_TURN_SETTLE_EXTRA\s+)(\d+)")
X0_NOTE_BEGIN = b"// >>> 26.11.8 x0 NOTE"
X0_NOTE_END = b"// <<< 26.11.8 x0 NOTE"


def waveform_normalised(data):
    """剥掉 x0 注释块、把宏值抹回 12。返回 (字节, 错误说明)。"""
    out, inside, blocks = [], False, 0
    for line in data.split(b"\n"):
        s = line.strip()
        if s.startswith(X0_NOTE_BEGIN):
            if inside:
                return None, "nested x0 note block"
            inside = True
            blocks += 1
            continue
        if s.startswith(X0_NOTE_END):
            if not inside:
                return None, "x0 note end without a begin"
            inside = False
            continue
        if inside:
            if s and not s.startswith(b"//"):
                return None, "non-comment line inside the x0 note block"
            continue
        out.append(line)
    if inside:
        return None, "unterminated x0 note block"
    if blocks != 1:
        return None, "x0 note block must appear exactly once (found %d)" % blocks
    return SETTLE_DEF_RE.sub(lambda m: m.group(1) + b"12", b"\n".join(out)), None


if not WAVEFORM_C.exists():
    bad("B82c main/display_reader_waveform.c is missing - the soft turn waveform module was "
        "retired again")
else:
    data = WAVEFORM_C.read_bytes()
    settle = SETTLE_DEF_RE.search(data)
    norm, err = waveform_normalised(data)
    if not settle:
        bad("B82c the READER_SOFT_TURN_SETTLE_EXTRA definition is gone from "
            "display_reader_waveform.c - the waveform is no longer the declared kiko derivation")
    elif err is not None:
        bad("B82c display_reader_waveform.c is no longer kiko ae015054 plus the two declared "
            "deviations: %s" % err)
    else:
        h = hashlib.sha1()
        h.update(b"blob %d\0" % len(norm))
        h.update(norm)
        got = h.hexdigest()
        if got != WAVEFORM_BLOB:
            bad("B82c display_reader_waveform.c drifted from kiko ae015054 outside the declared "
                "deviations (normalised blob %s, %d B; expected %s) - an old revision, a hand "
                "tweak or an 'optimization' crept in" % (got[:12], len(norm), WAVEFORM_BLOB[:12]))
        elif int(settle.group(2)) != 0:
            bad("B82c READER_SOFT_TURN_SETTLE_EXTRA is %s, must be 0 - the twelve white phases "
                "are back, and with them the idle grey veil they were convicted of"
                % settle.group(2).decode())
        else:
            ok("B82c soft turn waveform is kiko ae015054 with exactly the two declared "
               "deviations (settle 12 -> 0, one comment block): normalised blob %s, %d phases"
               % (got[:12], 37 + 0 + 4))

# ---- B79c：跨视图进出阅读 = APP_REDRAW_FULL -------------------------------
edge = re.search(
    r"if\s*\(\s*s_presented_view\s*!=\s*\(int\)s_view\s*&&\s*\n?\s*"
    r"\(\s*s_view\s*==\s*READING\s*\|\|\s*s_presented_view\s*==\s*READING\s*\)\s*\)\s*"
    r"\n?\s*redraw\s*=\s*APP_REDRAW_FULL\s*;", present)
if not edge:
    bad("B79c the view-edge rule (s_presented_view != s_view AND READING on either side -> "
        "APP_REDRAW_FULL) is gone: entering or leaving the reader falls back to a "
        "differential and the first page keeps the shelf's ink")
else:
    ok("B79c crossing the reader's view edge forces APP_REDRAW_FULL")

# ---- B79d：反向判据——小面积路径必须保持快刷 ------------------------------
footer = block_after(present, "if (s_reader_footer_pending)")
if not footer:
    bad("B79d the footer strip branch is gone - the turn's progress band is unhandled")
elif not re.search(r"MODE_DU", footer) or "progress_rect()" not in footer:
    bad("B79d the footer strip no longer uses the DU fast waveform over progress_rect(): "
        "a 42 px band must not borrow the whole-page cadence")
else:
    ok("B79d footer strip keeps its DU fast path over progress_rect()")

clock = block_after(present, "if (s_reader_clock_pending)")
if not clock:
    bad("B79d the clock strip branch is gone")
elif not re.search(r"update_display_area_diff_with\s*\([^;]*MODE_GL16", clock):
    bad("B79d the clock no longer repaints as a GL16 differential strip: a minute tick must "
        "never pay for a full refresh")
else:
    ok("B79d clock keeps its GL16 differential strip")

# ---- B80a：周期清底必须存在 ----------------------------------------------
if not re.search(r"redraw\s*==\s*APP_REDRAW_FULL\s*\|\|\s*s_reader_cleanup", present):
    bad("B80a the periodic cleanup no longer shares the full-refresh exit: s_reader_cleanup "
        "exists but nothing consumes it, so 'every N pages' is a dead setting again")
elif not re.search(r"s_reader_cleanup\s*\)\s*\{\s*\n?\s*s_turns\s*=\s*0", present):
    bad("B80a the cleanup branch does not reset s_turns: the invariant 'after any screen-wide "
        "cleanup the counter is zero' would break, and N would drift")
elif "reader_note_turn()" not in present:
    bad("B80a present() no longer books page advances: the counter is written somewhere else "
        "than where the frame is decided, so 'every N pages' counts intent instead of screens")
elif not re.search(r"if\s*\(\s*due\s*\)\s*s_reader_cleanup\s*=\s*true", present):
    bad("B80a present() books the turn but never turns it into a cleanup: the counter climbs "
        "and nothing consumes it - the 26.11.6 zero-cleanup shape")
else:
    ok("B80a periodic cleanup shares the full-refresh exit, resets the counter, and is booked "
       "in present() only")

# ---- B80b：跨章清底并归零 -------------------------------------------------
if not turn_page:
    bad("B80b turn_page() not found - chapter crossings cannot be audited")
elif "chapter_changed" not in turn_page or "s_chapter != chapter_before" not in turn_page:
    bad("B80b turn_page() no longer detects a chapter change: crossing a chapter must force a "
        "screen-wide cleanup (the ruled 'new chapter = full refresh')")
elif not re.search(r"s_reader_cleanup\s*=\s*chapter_changed\s*;", turn_page):
    bad("B80b the chapter change is not wired into s_reader_cleanup")
elif "reader_note_turn(" in turn_page:
    bad("B80b turn_page() books the turn itself again: the count left present() and went back "
        "to counting intent (S2 put it in one place on purpose)")
else:
    # 归零不需要单独一行：跨章置位 s_reader_cleanup，整屏分支的 `s_turns = 0` 就是归零点。
    # The reset needs no separate line: the crossing sets s_reader_cleanup and the
    # full-screen branch's `s_turns = 0` is the reset point (B80a asserts it exists).
    ok("B80b a chapter crossing forces cleanup; the counter resets at the shared exit")

# ---- B80c：AI 页翻页必须过账 ----------------------------------------------
if not reader_ai_turn:
    bad("B80c reader_ai_turn() not found - AI page turns would fall into the whole-page "
        "exit and flash once per page")
elif "s_reader_body_turn = true" not in reader_ai_turn:
    bad("B80c reader_ai_turn() does not mark the frame as a page advance: present() would "
        "take the whole-page GC16 branch (per-page flash)")
elif "s_reader_page_advance = true" not in reader_ai_turn:
    bad("B80c reader_ai_turn() does not mark the page advance: reading 20 AI pages would "
        "never hit the periodic cleanup - a zero-cleanup blind spot")
elif "reader_note_turn(" in reader_ai_turn:
    bad("B80c reader_ai_turn() books the turn itself: the count must settle in present(), "
        "where it can see whether a frame was actually presented")
else:
    ok("B80c AI page turns mark the frame; present() books it into the shared counter")

# ---- B80d：水波纹收尾不得整屏（26.11.7 W1）--------------------------------
ripple = block_after(present, "if (s_water_turn_pending)")
if not ripple:
    bad("B80d the water-turn branch is gone - the ripple's closing wipe cannot be audited")
elif "update_display_full(" in ripple:
    bad("B80d the ripple's closing wipe is screen-wide again: the animation only dirties the "
        "body, so wiping the whole panel is what made every turn flash (the 448 ms tail in "
        "the 26.11.6 log, 1091 - 643)")
elif not re.search(r"update_display_area_full_with\s*\([^;]*MODE_GC16\s*,\s*reader_area\(\)", ripple):
    bad("B80d the ripple's closing wipe is not a full-pixel GC16 over reader_area(): the "
        "swept region must be rebuilt to paper white, and a differential cannot do that")
else:
    ok("B80d ripple closing wipe is a full-pixel GC16 confined to reader_area()")

# ---- B82b：AI 解析页翻页 = 与正文同款（26.11.9 收编）----------------------
#
# 26.11.7 让 AI 页走 GC16 + 差分（s_reader_body_turn 分支），与正文当年同型错法：
# 差分的单次「深→白」推进 + 36 相裁剪表给不出足够的白。用户一进 AI 解析页翻几页
# 就会撞上——这是 26.11.8 收编正文时留下的唯一漏项，判据与 B82a 一字不差。
ai_turn = body_turn_branch
if not ai_turn:
    bad("B82b the `else if (s_reader_body_turn)` branch is gone: AI detail turns have no "
        "dedicated path and will flash through the whole-page GC16")
elif "update_display_area_diff_with(" in ai_turn:
    bad("B82b AI detail turns are back on _diff_with: allow_gl16_diff bypasses the driver's "
        "GL16-implies-full rule, which is how the white never reaches the rail")
elif "update_display_area_full_with(" not in ai_turn:
    bad("B82b AI detail turns must call _area_full_with(): a differential cannot supply the "
        "white push (the 26.11.7 band-making combination)")
elif "display_reader_soft_turn_waveform(" not in ai_turn:
    bad("B82b AI detail turns must use display_reader_soft_turn_waveform(): the trimmed "
        "E0470_WAVEFORM table is what stopped white targets mid-transition")
elif "MODE_GC16" in ai_turn:
    bad("B82b AI detail turns are back on MODE_GC16: black phases first, one whole-block "
        "flash per AI page")
elif "MODE_GL16" not in ai_turn:
    bad("B82b AI detail turns lost MODE_GL16: the soft sequence is derived from the GL16 LUT")
elif "E0470_WAVEFORM" in ai_turn:
    bad("B82b AI detail turns still name the trimmed table E0470_WAVEFORM directly")
else:
    ok("B82b AI detail turns use the same soft-waveform full-pixel GL16 refresh as the body")

# ---- B83：差分裁剪的右边界掩码必须按 max_x 取 -------------------------------
#
# render.c 的 `epd_difference_image_base` 用两个循环找左右边界，各取半字节掩码：
# 左循环按 min_x 的奇偶、右循环按 **max_x** 的奇偶。原版右循环写的是 min_x（官方
# 移植带来的 bug，kiko 修了）——当最左脏列与最右脏列奇偶不同时，掩码查的是另一半
# 字节，右边界最多少判 1 列，产物在右缘。x0 之后差分路径只服务局部小面积，症状更小，
# 但这是正确性问题，判据是「与 kiko 行为一致」，不是「肉眼可见」。
render_c = ROOT / "components/epdiy/src/render.c"
if not render_c.exists():
    bad("B83 components/epdiy/src/render.c is gone - the differential crop has no boundary "
        "scan at all")
else:
    src = render_c.read_text(encoding="utf-8", errors="replace")
    left = re.search(r"for \(min_x = [^)]*; min_x < x_end; min_x\+\+\) \{\s*\n\s*"
                     r"uint8_t mask = (\w+) % 2", src)
    right = re.search(r"for \(max_x = [^)]*; max_x >= crop_to\.x; max_x--\) \{\s*\n\s*"
                      r"uint8_t mask = (\w+) % 2", src)
    if not left:
        bad("B83 the left-boundary scan in epd_difference_image_base is gone or reshaped - "
            "re-audit the crop before trusting the right edge")
    elif left.group(1) != "min_x":
        bad("B83 the left-boundary scan masks with %s instead of min_x: the left edge will "
            "drop a column the same way the right edge used to" % left.group(1))
    elif not right:
        bad("B83 the right-boundary scan in epd_difference_image_base is gone or reshaped - "
            "re-audit the crop before trusting the right edge")
    elif right.group(1) != "max_x":
        bad("B83 the right-boundary scan masks with %s instead of max_x: when the leftmost "
            "and rightmost dirty columns have different parity, the wrong nibble is tested "
            "and the crop loses a column on the right (the upstream port bug)"
            % right.group(1))
    else:
        ok("B83 differential crop masks the left scan with min_x and the right scan with max_x")

print()
if fails:
    print("check_refresh_ghost: %d FAILED" % len(fails))
    sys.exit(1)
print("check_refresh_ghost: all 12 checks passed (B79a-d, B80a-d, B82a, B82b, B82c, B83)")
