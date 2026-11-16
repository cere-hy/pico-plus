#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
"""摘抄反馈的结构牙：入队必须当场说话，队列深度必须跨拍比对。

为什么要有这条静态检查：26.11.1 真机轮第 2 回执——无卡连摘 3 条，冲账成功（读卡见 3 行），
但屏幕上**一个字都没有**：没有「待落盘 N」，也没有「已存 ✓」。日志侧证据是干净的：

    443084 boxsel: excerpt enqueue -> 0 (pending=1)
    443645 excerpt: flush failed, keeping 1 queued      ← 每 1 s 一条，一直刷到插卡
    478349 excerpt: flushed, pending=0

两处独立缺陷叠在一起，都在 app 层，宿主测不到（切片 harness 已 KF-7.1）：

  ① 入队不请示刷新。`excerpt_footer_tick()` 的判据是「这一拍内前后是否相等」：
     `const unsigned before = excerpt_store_pending(); ... if (r != OK && before == pending)
     return false;`。而入队发生在**两次 tick 之间**，`before` 采的是入队之后的数——
     0→3 涨了三次，`before == after` 三次全成立，页脚一次没重画。
     深度变化的基线必须跨拍，谁改深度谁欠一次重绘。

  ② 反馈只挂在页脚上。页脚前缀在 `s_reader_fullscreen` 时整块不画（全屏阅读没有页脚），
     掉盘态由另一个分支接管，而离开 READING 视图后 tick 直接 `return false`。
     一个动作能不能被感知，不该取决于「用户会不会去看屏幕最下面那一行 24 px」。

所以：入队分支必须写 `s_reader_notice`（顶部气泡，全屏也画），tick 必须比对跨拍基线。
两条都是「删掉照样编译、行为牙照样全绿、真机照样是哑巴」的那种句子。
"""

import re
import sys
from pathlib import Path

SRC = Path("main/apps/app_book.c")


def strip_comments(body: str) -> str:
    """去掉 // 与 /* */ 注释，避免注释里写了「已入队」就先被匹配到。"""
    out, i, n = [], 0, len(body)
    while i < n:
        if body.startswith("//", i):
            j = body.find("\n", i)
            i = n if j < 0 else j
        elif body.startswith("/*", i):
            j = body.find("*/", i + 2)
            i = n if j < 0 else j + 2
        elif body[i] in "\"'":
            quote = body[i]
            out.append(body[i]); i += 1
            while i < n:
                if body[i] == "\\":
                    out.append(body[i:i + 2]); i += 2; continue
                out.append(body[i])
                if body[i] == quote:
                    i += 1
                    break
                i += 1
        else:
            out.append(body[i]); i += 1
    return "".join(out)


def function_body(text: str, name: str, start: int = 0):
    """取出 `name(...) {` 到配对 `}` 的函数体，返回 (body, body_start, line_no)。"""
    m = re.search(r"\b" + re.escape(name) + r"\s*\([^;{]*\)\s*\{", text[start:])
    if not m:
        return None, 0, 0
    open_at = start + m.end() - 1
    depth = 0
    for i in range(open_at, len(text)):
        ch = text[i]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return text[open_at:i + 1], open_at, text.count("\n", 0, open_at) + 1
    return None, 0, 0


def enclosing_function(text: str, at: int):
    """从 `at` 往前找最近的一层函数定义（粗略但够用：找 `类型 名字(` 且后面跟 `{`）。"""
    head = text[:at]
    for m in reversed(list(re.finditer(r"^[A-Za-z_][\w \*]*\b(\w+)\s*\([^;{]*\)\s*\{", head, re.M))):
        name = m.group(1)
        body, _, line = function_body(text, name, m.start())
        if body and m.start() <= at <= m.start() + len(body):
            return name, body, line
    return None, None, 0


def main() -> int:
    if not SRC.exists():
        print("excerpt-feedback: FAIL — %s not found" % SRC, file=sys.stderr)
        return 1
    text = SRC.read_text(encoding="utf-8")
    problems = []

    # ---- ① 入队分支必须当场给屏幕反馈（顶部气泡），不许只靠页脚 ----------
    enq = text.find("excerpt_store_enqueue(")
    if enq < 0:
        problems.append("excerpt_store_enqueue() call site not found (renamed? update this check)")
    else:
        name, body, line = enclosing_function(text, enq)
        if body is None:
            problems.append("cannot resolve the function that calls excerpt_store_enqueue()")
        else:
            code = strip_comments(body)
            m = re.search(r"case\s+EXCERPT_OK\s*:", code)
            if not m:
                problems.append("%s(): no `case EXCERPT_OK:` branch (renamed enum?)" % name)
            else:
                tail = code[m.end():]
                end = tail.find("break;")
                case_ok = tail[:end] if end >= 0 else tail
                # 26.11.4：气泡有了统一写入口 `reader_notice_set()`（它顺手打 show 日志），
                # 判据从「字面 copy_text」升到「两个入口二选一」——两者都不是 = 没写。
                # Since 26.11.4 the toast has a single writer; either form proves a write,
                # and neither form proves there is none.
                bubbles = ("reader_notice_set(" in case_ok) or ("copy_text(s_reader_notice" in case_ok)
                if not bubbles:
                    # 注意：判据必须是「写入调用」本身。只查 `s_reader_notice` 子串会
                    # 被 `s_reader_notice_until = ...` 骗过——过期时间戳还在，字没了。
                    problems.append(
                        "%s():%d EXCERPT_OK branch says nothing on screen — the only feedback is "
                        "the footer prefix, which is not drawn in full-screen reading and dies "
                        "with the view (26.11.1 field: 3 queued, 0 pixels)" % (name, line))

    # ---- ② 队列深度的基线必须跨拍（入队发生在两次 tick 之间）--------------
    tick, tick_at, tick_line = function_body(text, "excerpt_footer_tick")
    if tick is None:
        problems.append("excerpt_footer_tick() not found (renamed? update this check)")
    else:
        code = strip_comments(tick)
        if "s_excerpt_footer_seen" not in code:
            problems.append(
                "excerpt_footer_tick():%d compares a baseline sampled INSIDE the tick — an "
                "enqueue between two ticks makes before == after, so the depth can go 0 -> 3 "
                "with zero repaints. Compare against s_excerpt_footer_seen (cross-tick)."
                % tick_line)
        elif not re.search(r"excerpt_store_pending\(\)\s*[=!]=\s*s_excerpt_footer_seen"
                           r"|s_excerpt_footer_seen\s*[=!]=\s*excerpt_store_pending\(\)", code):
            problems.append(
                "excerpt_footer_tick():%d mentions s_excerpt_footer_seen but never compares the "
                "live depth against it" % tick_line)
        elif "s_excerpt_footer_seen = excerpt_store_pending();" not in code:
            problems.append(
                "excerpt_footer_tick():%d repaints but never records what it painted — the next "
                "tick would repaint forever (or never)" % tick_line)

    # ---- ③ 落盘回执必须上顶部气泡（hint 槽只在页脚带消费，全屏不可达）------
    # 26.11.1 toast 回执：全部开书入口都落全屏，「已存 ✓」挂在页脚 = 构造上不可见。
    # 判据按分支切块——OK 与 DROPPED 紧邻，整段窗口会被隔壁分支的调用骗过。
    if tick is not None:
        code = strip_comments(tick)
        m_ok = re.search(r"EXCERPT_FLUSH_OK\s*\)?\s*\{", code)
        if not m_ok:
            problems.append("excerpt_footer_tick(): no EXCERPT_FLUSH_OK branch (renamed enum?)")
        else:
            ok_block = code[m_ok.end():code.find("EXCERPT_FLUSH_DROPPED", m_ok.end())]
            if "excerpt_notice(" not in ok_block:
                problems.append(
                    "excerpt_footer_tick(): EXCERPT_FLUSH_OK does not fire the toast via "
                    "excerpt_notice() — the hint slot is consumed only by the footer band, "
                    "which no open path can reach (all opens land full-screen)")
        m_drop = re.search(r"EXCERPT_FLUSH_DROPPED\s*\)?\s*\{", code)
        if m_drop:
            drop_end = code.find("EXCERPT_FLUSH_FAILED", m_drop.end())
            drop_block = code[m_drop.end():drop_end if drop_end > 0 else len(code)]
            if "excerpt_notice(" not in drop_block:
                problems.append(
                    "excerpt_footer_tick(): EXCERPT_FLUSH_DROPPED does not fire the toast — "
                    "silently losing user data must not be quieter than saving it")

    # ---- ④ 气泡的擦除/回执几何必须走 reader_notice_area()，不许硬编码 --------
    # 擦除矩形曾是竖屏硬编码 {208,150,268,68}；横屏气泡中心随 ui_w() 右移，
    # 硬编码擦不到，气泡只能等翻页陪葬（26.11.1 toast 回执第 2 条）。
    # 判据咬去注释后的代码——注释里提到坐标不算（本牙自己的教训：文本巧合不是行为）。
    whole_code = strip_comments(text)
    n_area = len(re.findall(r"reader_notice_area\(\)", whole_code))
    hard = re.findall(r"\{\s*208\s*,\s*150\s*,\s*268\s*,\s*68\s*\}", whole_code)
    if n_area < 2:
        problems.append(
            "reader_notice_area() is used %d time(s) — paint/erase/receipt must share one "
            "geometry (need >=2 call sites); hardcoding coordinates again is how the "
            "landscape toast got stuck" % n_area)
    if hard:
        problems.append(
            "hardcoded notice rect {208,150,268,68} found in app_book.c — use "
            "reader_notice_area(); the constant is portrait-only and misses in landscape")

    if problems:
        for p in problems:
            print("excerpt-feedback: FAIL — " + p, file=sys.stderr)
        return 1

    print("excerpt-feedback: PASS — enqueue speaks via the toast writer; "
          "footer baseline is cross-tick; flush receipts hit the toast; "
          "notice geometry is computed, not hardcoded")
    return 0


if __name__ == "__main__":
    sys.exit(main())
