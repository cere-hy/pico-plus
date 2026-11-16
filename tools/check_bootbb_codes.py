#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
"""
门禁体检批 ④ —「事件码三元」自检（§13.21 第 ④ 项）。

31b 第 1 条给黑匣子加了 `PICO_BOOTBB_NOTE_PUSH_FREEZE = 9`：枚举加了、调用点接了、
日志也打了——**就是没进 `note_name()`**。于是重放时它打成 `?`。

这是「留痕」家族的读回侧变体：**写了但读不出来 = 没写**。与「失败而不留痕」互为补集，
两者合起来才是完整的：事件必须在**写入端存在**、在**读取端可命名**、在**容量压力下不被挤掉**。

三元判据（缺一条这张网就有洞）：
  ① 枚举 ↔ 登记簿 `tools/bootbb_codes.txt` 双向完备：新增码必须在这里决定级别，
     登记簿也不许留下已经不存在的码（陈旧条目会让下一个人以为那个码还在用）。
  ② 每个码在 `note_name()` 里有 case：否则环里存的是数字，人读到的是 `?`。
  ③ 级别与 `note_is_critical()` 一致：登记 critical 的必须真在判定里，
     登记 normal 的不许混进去（混进去 = 环满时普通事件挤掉强信号）。

弱牙三型自检（§13.21a）：
  · 谓词防短路：`note_name` 必须真被调用（只定义不调用，等于没有名字表）；
  · count >= 2 防单岔：不只查「有没有 case」，按**每个码**分别查；
  · 拒死代码形态：`note_is_critical` 若被 `if (0) return true` 短路，本牙报红。

用法 / usage: python3 tools/check_bootbb_codes.py   （0 = 通过）
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

HDR = ROOT / "components/pico_heapdiag/include/pico_bootbb.h"
SRC = ROOT / "components/pico_heapdiag/pico_bootbb.c"
LEDGER = ROOT / "tools/bootbb_codes.txt"

failures = []


def bad(msg):
    failures.append(msg)


def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return "\n".join(line.split("//")[0] for line in src.splitlines())


def func_body(src, name):
    """取 C 函数体（按花括号配平）。"""
    m = re.search(r"\b%s\s*\([^;{]*\)\s*\{" % re.escape(name), src)
    if not m:
        return ""
    depth, i = 0, m.end() - 1
    while i < len(src):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[m.end():i]
        i += 1
    return src[m.end():]


def main():
    for p in (HDR, SRC, LEDGER):
        if not p.exists():
            bad("missing %s: the event-code triangle cannot be judged"
                % p.relative_to(ROOT).as_posix())
    if failures:
        for f in failures:
            print("FAIL: " + f)
        return 1

    hdr = strip_comments(HDR.read_text(errors="replace"))
    src = strip_comments(SRC.read_text(errors="replace"))

    # ① 枚举：名 → 值。值必须唯一——复制粘贴后忘改数字，两个事件在环里长得一模一样。
    enum = {}
    for name, val in re.findall(r"\b(PICO_BOOTBB_NOTE_\w+)\s*=\s*(\d+)", hdr):
        enum[name] = int(val)
    if not enum:
        bad("no PICO_BOOTBB_NOTE_* enumerator found in pico_bootbb.h: the schema moved "
            "or the parse broke - either way this tooth is now blind")
        for f in failures:
            print("FAIL: " + f)
        return 1
    seen = {}
    for name, val in sorted(enum.items(), key=lambda kv: kv[1]):
        if val in seen:
            bad("① %s and %s share value %d: two different events would land in the "
                "blackbox indistinguishable, and every replay would silently merge them"
                % (seen[val], name, val))
        seen[val] = name

    # 登记簿
    ledger = {}
    order = []
    for lineno, raw in enumerate(LEDGER.read_text(errors="replace").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = raw.split("\t")
        parts = [p.strip() for p in parts if p.strip() != ""]
        if len(parts) < 2:
            bad("① %s:%d is not `code<TAB>level<TAB>note`: an entry without a level "
                "decides nothing, and deciding nothing is exactly what this ledger "
                "exists to stop" % (LEDGER.name, lineno))
            continue
        name, level = parts[0], parts[1]
        if level not in ("critical", "normal"):
            bad("① %s:%d has level %r: only `critical` or `normal` are accepted - a typo "
                "here would read as neither and silently drop the code from both sets"
                % (LEDGER.name, lineno, level))
            continue
        ledger[name] = level
        order.append(name)

    for name in sorted(set(enum) - set(ledger)):
        bad("① %s (=%d) is not in %s: a new event code must declare its level. "
            "Skipping the decision means it inherits 'normal' by accident, and the one "
            "event that had to survive a full ring is the one that gets evicted first"
            % (name, enum[name], LEDGER.name))
    for name in sorted(set(ledger) - set(enum)):
        bad("① %s is in %s but not in the enum: a stale ledger row makes the next "
            "reader believe that code still exists" % (name, LEDGER.name))

    # ② 名字表：逐码查 case
    name_body = func_body(src, "note_name")
    if not name_body:
        bad("② note_name() is gone: every replayed event would print as a bare number")
    else:
        cases = set(re.findall(r"case\s+(PICO_BOOTBB_NOTE_\w+)\s*:", name_body))
        for name in sorted(enum):
            if name not in cases:
                bad("② %s has no case in note_name(): it gets written to the ring and "
                    "reads back as `?` - written but unreadable is the same as never "
                    "written, this is the 31b item-1 escape verbatim" % name)
        if not re.search(r"note_name\s*\(", strip_comments(src).replace(
                "static const char *note_name", "", 1)):
            bad("② note_name() is defined but never called: a name table nobody reads is "
                "decoration (weak-tooth type 1: symbol exists, wiring does not)")

    # ③ 级别 ↔ note_is_critical
    crit_body = func_body(src, "note_is_critical")
    if not crit_body:
        bad("③ note_is_critical() is gone: every event becomes equal, and a full ring "
            "evicts data-loss events at the same rate as a routine poll tick")
    else:
        if re.search(r"if\s*\(\s*(0|false)\s*\)", crit_body):
            bad("③ note_is_critical() is short-circuited by a never-true guard: the "
                "level declared here would never take effect (weak-tooth type 1)")
        listed = set(re.findall(r"\b(PICO_BOOTBB_NOTE_\w+)\b", crit_body))
        for name, level in sorted(ledger.items()):
            if level == "critical" and name not in listed:
                bad("③ %s is registered critical but absent from note_is_critical(): on a "
                    "full ring it is evicted like any routine event - the strongest "
                    "signal loses to the noisiest one" % name)
            if level == "normal" and name in listed:
                bad("③ %s is registered normal but listed in note_is_critical(): it now "
                    "competes for the ring on equal terms with data-loss events, which "
                    "is how a real fault gets pushed out by chatter" % name)

    if failures:
        for f in failures:
            print("FAIL: " + f)
        print("\nbootbb event codes: %d failure(s)" % len(failures))
        return 1
    print("bootbb event codes: %d code(s) - enum / note_name / critical all agree "
          "(ledger %s) - PASS" % (len(enum), LEDGER.name))
    return 0


if __name__ == "__main__":
    sys.exit(main())
