#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
"""掉盘往返的结构牙：插回卡必须重开书源，且必须在 load_chapter 之前。

为什么要有这条静态检查：26.11.1 真机两个样本（t=139446 / t=300679）同形——
挂载成功（`err=ESP_OK present=1 mounted=1`）紧接
`book: chapter load failed: ESP_ERR_INVALID_ARG`，然后 `shelf books=18`，
用户看到的是「插回卡的瞬间被踢回书架」。

根因不在挂载，在顺序：`book_enter_offline()` 为了不留下悬空 `FILE*` 调了
`book_close()`（同时清空 `s_book.count`）；`book_leave_offline()` 若不先
`book_open(s_path)`，`book_chapter_load()` 的 `!s_book.file` 分支立刻返回
`ESP_ERR_INVALID_ARG`，失败路径把 view 打回 SHELF。

行为牙在 `tools/book_source_host_test.c`（book_close 后 load 必 INVALID_ARG、
重开后同章可加载），但它测不到 app 层「有没有调 book_open」这一句。这条静态牙
补的正是那一句：删掉它，真机重现，而所有宿主测试仍然全绿——这正是必须用结构
断言兜住的原因（同 §13.37「顺序不是实现细节，是语义本身」）。

只做两件事，不做正则万能匹配：函数体用大括号配对取出，再查两处标记与先后。
"""

import re
import sys
from pathlib import Path

SRC = Path("main/apps/app_book.c")


def strip_comments(body: str) -> str:
    """去掉 // 与 /* */ 注释，避免注释里提到 load_chapter 就先被匹配到。"""
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
    """取出 `name(...) {` 之后到配对 `}` 的函数体，返回 (body, body_start_line, line_no)。"""
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
                body = text[open_at:i + 1]
                return body, open_at, text.count("\n", 0, open_at) + 1
    return None, 0, 0


def main() -> int:
    if not SRC.exists():
        print("offline-reopen: FAIL — %s not found" % SRC, file=sys.stderr)
        return 1
    text = SRC.read_text(encoding="utf-8")
    problems = []

    enter, enter_at, enter_line = function_body(text, "book_enter_offline")
    leave, leave_at, leave_line = function_body(text, "book_leave_offline")
    if enter is None:
        problems.append("book_enter_offline() not found (renamed? update this check)")
    if leave is None:
        problems.append("book_leave_offline() not found (renamed? update this check)")

    if enter and "book_close(" not in enter:
        problems.append(
            "book_enter_offline(): no book_close() — the FILE* would outlive the volume "
            "unmount (dangling handle)")

    if leave:
        code = strip_comments(leave)
        open_at = code.find("book_open(")
        load_at = code.find("load_chapter(")
        if open_at < 0:
            problems.append(
                "book_leave_offline(): no book_open() — the source was closed at removal, so "
                "load_chapter() returns ESP_ERR_INVALID_ARG and the reader is bounced to the "
                "shelf (26.11.1 field signature)")
        elif load_at >= 0 and open_at > load_at:
            problems.append(
                "book_leave_offline(): book_open() comes AFTER load_chapter() — order is the "
                "semantics here, not an optimisation")

    if problems:
        for p in problems:
            print("offline-reopen: FAIL — " + p, file=sys.stderr)
        return 1

    print("offline-reopen: PASS — book_enter_offline:{} closes, "
          "book_leave_offline:{} reopens before load".format(enter_line, leave_line))
    return 0


if __name__ == "__main__":
    sys.exit(main())
