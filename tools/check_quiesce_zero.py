#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
"""
B42 结构牙：`*_quiesce(0)` 零命中（31b 第 6 条，上线即绿）。

为什么是「传 0」而不是「调用本身」：quiesce 的语义是**收口**——停掉生产者、收齐
句柄，超时是失败。传 0 的含义是「宣布收口、一个 tick 都不等」，于是返回值必然是
false，而调用方紧接着一句 `(void)` 把失败扔掉。两个动作叠在一起，代码写着「我等了」，
实际一次没等。26.9.31 的 app_loop 插拔边沿就是这行：

    if (gained || lost) { ui_avatar_invalidate(); (void)book_cover_jobs_quiesce(0); ... }

卡已经拔了，它真正想要的是「别再给我上一页的封面」——那是**作废**（discard），不是
收口。真正的收口在 lost 分支的三拍里（`push_sink_quiesce(2000)` → `remount`），传的
是 2000。混用一次，拔卡路径上就有一次「假收口」。

三条判据，缺一不可：

B42a  `main/` + `components/`（vendor 除外）的**代码**里，`*_quiesce(0)` 零命中。
      去注释后匹配：注释里讨论 `quiesce(0)` 是在解释这条牙，不是调用。
      （与 §13.27 第三坑不冲突：盘点工具靠文本认「资产被引用」，这里认「调用形状」。）
B42b  正向锚：`book_cover_jobs_quiesce(` 的非零调用仍然存在（sleep / usb / 格式化 /
      阅读页）。没有它，「把 API 全删了」也会让 B42a 绿——那是牙被拆了，不是缺陷被修了。
B42c  修法落点：`poll_media()` 的插拔边沿必须调 `book_cover_jobs_discard()`。
      删掉它 = 边沿不再作废旧一代封面，旧书封面会短暂画到新书上（回归）。

用法：python3 tools/check_quiesce_zero.py   （0 = 通过）
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
APP_LOOP = ROOT / "main/app/app_loop.c"

# vendor 件不归本项目管；它们的 `quiesce(0)`（若有）也不该被这条牙判红。
VENDOR_PREFIXES = ("espressif__", "vendor")

fails = []


def bad(msg):
    fails.append(msg)
    print("FAIL: " + msg)


def ok(msg):
    print("ok   " + msg)


def strip_comments(src):
    """去掉 // 与 /* */，且不碰字符串/字符字面量。

    只为本牙服务：它要判**调用形状**，注释里出现 `quiesce(0)` 是在解释规则。
    写成一个显式状态机，是因为正则版会在 `"http://"` 上误删半行——那会让本牙
    自己变成「靶子是假的」（§13.12a）。
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
                i += 2
                continue
            if c == "/" and nxt == "*":
                state = "block"
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
            i += 1
            continue
        if state == "block":
            if c == "*" and nxt == "/":
                state = None
                i += 2
                # 保留一个空格，避免把前后两个 token 粘成一个
                out.append(" ")
                continue
            if c == "\n":
                out.append(c)  # 保行号
            i += 1
            continue
        # str / chr：原样搬运，遇到未转义的结束引号退出
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


def sources():
    for base in ("main", "components"):
        for p in sorted((ROOT / base).rglob("*")):
            if p.suffix not in (".c", ".h"):
                continue
            if base == "components":
                rel = p.relative_to(ROOT / "components")
                if rel.parts and rel.parts[0].startswith(VENDOR_PREFIXES):
                    continue
            yield p


# ---- B42a：代码里不许有 quiesce(0) ------------------------------------------
CALL = re.compile(r"\b[A-Za-z_]\w*_quiesce\s*\(\s*0[uUlL]*\s*\)")
hits = []
scanned = 0
for path in sources():
    scanned += 1
    code = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
    for m in CALL.finditer(code):
        line = code[: m.start()].count("\n") + 1
        hits.append("%s:%d  %s" % (path.relative_to(ROOT), line, m.group(0)))

if hits:
    for h in hits:
        bad("B42a quiesce(0) at " + h)
    print("     `quiesce(0)` announces a join and waits for nothing; use the discard "
          "path for 'drop stale results', or a real timeout for 'join handles'.")
else:
    ok("B42a no `*_quiesce(0)` in %d source files (vendor excluded)" % scanned)

# ---- B42b：正向锚——quiesce 的非零调用必须仍在 --------------------------------
nonzero = []
for path in sources():
    code = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
    for m in re.finditer(r"\bbook_cover_jobs_quiesce\s*\(([^)]*)\)", code):
        arg = m.group(1).strip()
        if arg and not re.fullmatch(r"0[uUlL]*", arg):
            line = code[: m.start()].count("\n") + 1
            nonzero.append("%s:%d  %s" % (path.relative_to(ROOT), line, arg))

if len(nonzero) < 2:
    bad("B42b only %d non-zero book_cover_jobs_quiesce() calls remain (expected >= 2: "
        "sleep / usb / format / reader). Deleting the API is not the fix." % len(nonzero))
else:
    ok("B42b %d real quiesce calls still carry a non-zero timeout" % len(nonzero))

# ---- B42c：插拔边沿必须走 discard ------------------------------------------
loop = strip_comments(APP_LOOP.read_text(encoding="utf-8", errors="replace"))
m = re.search(r"static\s+bool\s+poll_media\s*\([^)]*\)\s*\{", loop)
body = ""
if not m:
    bad("B42c poll_media() not found in main/app/app_loop.c")
else:
    tail = loop[m.end():]
    end = re.search(r"\n\}", tail)
    body = tail[: end.start()] if end else tail

if "book_cover_jobs_discard()" not in body:
    bad("B42c poll_media() no longer calls book_cover_jobs_discard() on the hotplug "
        "edge - stale covers survive the card swap")
else:
    ok("B42c poll_media() discards the stale cover generation on the hotplug edge")

print()
if fails:
    print("check_quiesce_zero: %d FAILED" % len(fails))
    sys.exit(1)
print("check_quiesce_zero: all 3 checks passed (B42)")
