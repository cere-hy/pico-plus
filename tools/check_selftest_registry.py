#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
"""
门禁体检批 ② —「注册区定义数 == 调用数」自检（§13.18 修法落地）。

现场（31b 第 1 条）：给 `check_excerpt.py` 加了两颗牙 B55/B56，函数写好了、注释写好了、
连失败文案都写好了，**只有 main() 里那行注册被 `\\n` 转义吃掉**。十一向先红时十个
全部 MISSED——牙定义了、门禁全 PASS，`GATE_RC=0` 亮得像没事。

这和「没跑 ≠ 绿了」是同一枚硬币：**定义了没调用，等于没定义**，而且比没定义更坏——
文件里躺着一段看起来在守护你的代码，谁都不会再去补。

判据：脚本里每个 `def check_xxx(` 都必须从 `main()` 出发**可达**。
间接调用（check_a 里调 check_b）算可达，因为门禁跑得到它。
不是 check_ 前缀的函数（helper、`_body_of` 之类）不参与——它们的可达性由调用者承担。

用法 / usage: python3 tools/check_selftest_registry.py   （0 = 通过）
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

failures = []


def bad(msg):
    failures.append(msg)


DEF_RE = re.compile(r"^def\s+([A-Za-z_]\w*)\s*\(", re.M)
CALL_RE = re.compile(r"\b([A-Za-z_]\w*)\s*\(")


def function_bodies(src):
    """把源码切成 {函数名: 函数体}。顶格 def 之间的文本即函数体。"""
    marks = [(m.start(), m.group(1)) for m in DEF_RE.finditer(src)]
    bodies = {}
    for i, (pos, name) in enumerate(marks):
        end = marks[i + 1][0] if i + 1 < len(marks) else len(src)
        bodies[name] = src[pos:end]
    return bodies


def reachable(bodies, entry="main"):
    if entry not in bodies:
        return set()
    seen = set()
    stack = [entry]
    while stack:
        name = stack.pop()
        if name in seen:
            continue
        seen.add(name)
        for callee in CALL_RE.findall(bodies[name]):
            if callee in bodies and callee not in seen:
                stack.append(callee)
    return seen


def main():
    scripts = sorted(p for p in (ROOT / "tools").glob("check_*.py"))
    # 本脚本自己也在 tools/check_*.py 里，跳过它：它的 "check" 是文件级语义。
    scripts = [p for p in scripts if p.name != "check_selftest_registry.py"]

    checked = 0
    for path in scripts:
        rel = path.relative_to(ROOT).as_posix()
        src = path.read_text(errors="replace")
        bodies = function_bodies(src)
        defined = {n for n in bodies if n.startswith("check_")}
        if not defined:
            continue  # 不是「check 函数集合」结构的脚本，不适用本条
        if "main" not in bodies:
            bad("%s defines %d check_* function(s) but has no main(): nothing can call "
                "them, and the gate would still print a green line"
                % (rel, len(defined)))
            continue
        checked += 1
        live = reachable(bodies)
        dead = sorted(defined - live)
        if dead:
            bad("%s defines %s but main() never reaches %s: a registered-but-uncalled "
                "tooth is worse than a missing one - the file looks guarded, so nobody "
                "goes back to fix it. (This is the 31b item-1 escape: a `\\n` in the "
                "registration line silently ate the call.)"
                % (rel, ", ".join(sorted(defined)), ", ".join(dead)))

    if checked == 0:
        bad("no gate script with a check_* registry was found: the self-check has "
            "nothing to inspect, which is not the same as everything passing")

    if failures:
        for f in failures:
            print("FAIL: " + f)
        print("\nself-test registry: %d failure(s)" % len(failures))
        return 1
    print("self-test registry: %d script(s), every check_* definition is reachable "
          "from main() - PASS" % checked)
    return 0


if __name__ == "__main__":
    sys.exit(main())
