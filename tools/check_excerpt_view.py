#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
"""P0-4 / P0-5 / P1-2 的结构牙（26.11.2）。

为什么要有静态牙：宿主测试证明了「函数写得对」，这条牙证明「接线还在」——
渲染不直接写成品（必须 tmp → rename）、enqueue 必须过净化、peek 必须零出队、
书架三处命中矩形必须互相不打架且由编译期断言锁住。删掉任何一句，宿主测试
仍然全绿，但真机会重现各自的现场事故。

咬的是行为不是文本巧合：函数体用大括号配对取出、去注释后比对（§13.37 同族，
被注释字面量骗过的先例已有三起）。带四向负向自检：牙必须能被杀，杀不死的牙是装饰。
"""

import re
import shutil
import sys
import tempfile
from pathlib import Path

STORE = Path("main/book/excerpt_store.c")
APP = Path("main/apps/app_book.c")


def strip_comments(body: str) -> str:
    """去掉 // 与 /* */ 注释；字符串原样保留（避免字符串里的字面量骗过断言计数）。"""
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


def function_body(text: str, name: str):
    """取出 `name(...) {` 到配对 `}` 的函数体（已去注释），找不到返回 None。"""
    m = re.search(r"\b" + re.escape(name) + r"\s*\([^;{}]*\)\s*\{", text)
    if not m:
        return None
    i = m.end() - 1
    depth = 0
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return strip_comments(text[m.end() - 1:i + 1])
        i += 1
    return None


def check_all(store_src: str, app_src: str):
    """全部正向判据；返回报错列表（空 = 绿）。"""
    errs = []

    # ---- P0-4：enqueue 必须把书名与正文都过 fold_ws（净化只救写入端，一句都不能少）----
    enqueue = function_body(store_src, "excerpt_store_enqueue")
    if enqueue is None:
        errs.append("excerpt_store_enqueue 找不到")
    else:
        calls = enqueue.count("fold_ws(")
        if calls < 2:
            errs.append(f"P0-4: enqueue 里 fold_ws 只有 {calls} 处（书名+正文必须各一处）")

    # ---- P0-5：渲染必须 tmp → rename，成品路径绝不许直接 fopen 写 ----
    render = function_body(store_src, "excerpt_store_render_view")
    if render is None:
        errs.append("excerpt_store_render_view 找不到")
    else:
        if "rename(EXCERPT_VIEW_TMP, EXCERPT_VIEW)" not in render:
            errs.append("P0-5: 渲染必须以 rename(EXCERPT_VIEW_TMP, EXCERPT_VIEW) 收口")
        for bad in ('fopen(EXCERPT_VIEW,', 'fopen(EXCERPT_VIEW_'):
            if bad.replace("EXCERPT_VIEW_", "EXCERPT_VIEW_TMP_") in render:
                continue
        if re.search(r'fopen\s*\(\s*EXCERPT_VIEW\b\s*,\s*"w"', render):
            errs.append("P0-5: 成品路径不许直接 fopen 写（必须走 tmp → rename）")
        if "EXCERPT_VIEW_MAX_RECORDS" not in render:
            errs.append("P0-5: 渲染必须挂 4,000 条软顶（EXCERPT_VIEW_MAX_RECORDS）")

    # ---- P1-2：peek 必须零拷贝零出队 ----
    peek = function_body(store_src, "excerpt_store_peek_line")
    if peek is None:
        errs.append("excerpt_store_peek_line 找不到")
    else:
        # 前缀后缀都要咬：++s_head / s_head++ / s_head = 都算出队。
        # Both prefix and postfix count as dequeue.
        touch = r"(?:\+\+|--)\s*s_head|s_head\s*(?:\+\+|--|=[^=])"
        touch_count = r"(?:\+\+|--)\s*s_count|s_count\s*(?:\+\+|--|=[^=])"
        if "heap_caps_free" in peek or re.search(touch, peek) or re.search(touch_count, peek):
            errs.append("P1-2: peek 不许出队（不许碰 s_head/s_count、不许 free）")

    # ---- 入口甲：书架三处命中矩形 + 编译期互斥断言 ----
    rect = function_body(app_src, "shelf_excerpt_rect")
    if rect is None or "333, 94, 97, 54" not in rect:
        errs.append("P0-5: shelf_excerpt_rect 必须返回 {333, 94, 97, 54}")
    asserts = re.findall(r"_Static_assert\s*\(([^;]+)\)", strip_comments(app_src))
    p0_asserts = [a for a in asserts if "293" in a or "442" in a or "319" in a]
    if len(p0_asserts) < 3:
        errs.append(f"P0-5: 命中矩形互斥 _Static_assert 只有 {len(p0_asserts)} 条（须 ≥3）")
    if "fit_text(category_title,52,230)" not in strip_comments(app_src):
        errs.append("P0-5: 标题预算必须是 fit_text(category_title,52,230)")
    if "fit_text(category_title,52,326)" in strip_comments(app_src):
        errs.append("P0-5: 旧标题预算 326 还在——326 会顶进新按钮")

    # ---- 接线：action_at 必须调 open_excerpt_view；open_excerpt_view 必须渲染 + 走 open_book_impl ----
    action = function_body(app_src, "action_at")
    if action is None or "open_excerpt_view(" not in action:
        errs.append("P0-5: action_at 必须调 open_excerpt_view（入口甲接线）")
    opener = function_body(app_src, "open_excerpt_view")
    if opener is None:
        errs.append("open_excerpt_view 找不到")
    else:
        if "excerpt_store_render_view(" not in opener:
            errs.append("P0-5: open_excerpt_view 必须先 excerpt_store_render_view")
        if "open_book_impl(" not in opener:
            errs.append("P0-5: open_excerpt_view 必须走 open_book_impl（不写启动恢复记录）")

    # ---- 徽标：渲染处必须用 shelf_excerpt_label（P1-2 全局出口的可见面）----
    if "shelf_excerpt_label(" not in strip_comments(app_src):
        errs.append("P1-2: shelf_excerpt_label（摘抄徽标）不存在")
    return errs


def self_check_negative():
    """负向自检：每类牙都要能被杀。在临时副本上注入五种破坏，逐一验红。"""
    store_src = STORE.read_text()
    app_src = APP.read_text()
    mutations = [
        ("P0-4: 删正文净化", lambda s, a: (s.replace(
            "const size_t body = fold_ws(line + at, cap, text, &body_truncated);",
            "const size_t body = (size_t)snprintf(line + at, cap, \"%s\", text);"), a)),
        ("P0-5: 跳过 rename 直接落成品", lambda s, a: (s.replace(
            "if (rename(EXCERPT_VIEW_TMP, EXCERPT_VIEW) != 0) {",
            "if (0) {"), a)),
        ("P1-2: peek 偷偷出队", lambda s, a: (s.replace(
            "    if (index >= s_count) return NULL;\n    return s_q[(s_head + index) % EXCERPT_QUEUE_MAX].line;",
            "    if (index >= s_count) return NULL;\n    if (index == 0) { ++s_head; --s_count; }\n    return NULL;"), a)),
        ("P0-5: 矩形挪两像素", lambda s, a: (s, a.replace(
            "return (EpdRect){333, 94, 97, 54};", "return (EpdRect){331, 94, 97, 54};"))),
        ("P0-5: 标题预算回 326", lambda s, a: (s, a.replace(
            "fit_text(category_title,52,230)", "fit_text(category_title,52,326)"))),
    ]
    for name, mutate in mutations:
        s2, a2 = mutate(store_src, app_src)
        if s2 == store_src and a2 == app_src:
            print(f"  NEGATIVE-NOOP: {name}（变异没生效——先修变异再谈牙）")
            return False
        if not check_all(s2, a2):
            print(f"  NEGATIVE-SURVIVED: {name}（牙没咬住）")
            return False
    return True


def main() -> int:
    errs = check_all(STORE.read_text(), APP.read_text())
    if errs:
        for e in errs:
            print(f"RED: {e}")
        return 1
    print("PASS: P0-4 净化 / P0-5 tmp->rename+软顶 / P1-2 peek 零出队 / 入口甲矩形互斥 全部在位")
    if not self_check_negative():
        print("RED: 负向自检失败——牙杀不死等于没有牙")
        return 1
    print("PASS: 五向负向自检全红（牙能被杀，牙是真的）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
