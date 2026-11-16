#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
"""
门禁体检批 ① —「门禁资产盘点」（§13.12 挂账落地）。

问题不是「有脚本没接进门禁」，而是**没人知道哪些没接**。§13.12 记的那四例静默豁免区
（`settings_entry_clock` 不在任何 runner、`reader_refresh_release`、`ai_detail_wiring`、
`book_ui` 有登记）就是这样被一个个偶然发现的：有人改到某个模块，才发现它的测试从来没跑过。

本脚本把「在不在总入口里」变成一次可重复的计算：
  从总入口出发，递归解析它调用的脚本，再解析这些脚本调用的脚本，得到**覆盖闭包**；
  `tools/` 下所有测试型文件按闭包分两类——**内** 与 **外**。

「外」不等于「错」。有些测试天生进不了总入口（要 APK、要真机、要 java 工具链）；
但**每一个「外」都必须有一句人写的理由**，落在 `tools/gate_scope_registry.txt`。
没有理由的「外」= 孤儿 = 红——它不是被决定留在外面的，只是被忘了。

三种登记状态：
  out-ok   有意留在外面，理由成立（外部依赖 / 一次性侦察 / 已退役但保留证据）
  out-gap  应当进总入口，但目前进不去 —— **这是缺口，不是豁免**，必须挂账
  （没有登记 = 孤儿，直接红）

反向也要查：登记表里写了、文件却不存在 → 陈旧条目，红。

用法 / usage:
  python3 tools/check_gate_inventory.py            # 判定（0 = 通过）
  python3 tools/check_gate_inventory.py --list     # 打印全表（含「内」的行）
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
REGISTRY = ROOT / "tools/gate_scope_registry.txt"

# 入口按优先级找。总入口历史上放在仓库外的工作目录里（出包脚本与仓库分离），
# 所以这里允许 `../run_26930_gates.sh`；找不到任何一个就红——没有入口就没有盘点。
ENTRY_CANDIDATES = (
    ROOT.parent / "run_26930_gates.sh",
    ROOT / "run_26930_gates.sh",
    ROOT / "tools/gate_entry.sh",
)

# 测试型文件的命名模式。只认这些，避免把文档生成器、词典构建工具也算进测试资产。
TEST_PATTERNS = (
    re.compile(r"^run_.*\.sh$"),
    re.compile(r"^run_.*\.py$"),
    re.compile(r"^check_.*\.py$"),
    re.compile(r"^.*_host_test\.(py|sh|c)$"),
    re.compile(r"^.*_mutations\.(py|sh)$"),
    re.compile(r"^test_.*\.py$"),
    re.compile(r"^.*_golden\.sh$"),
    re.compile(r"^.*_io_contract\.sh$"),
)

# ① 带仓库根前缀的路径：`tools/xxx.sh` / `tools/xxx.c`。
PATH_RE = re.compile(r"(tools/[A-Za-z0-9_./-]+\.(?:sh|py|c))\b")

# ② 调用形态的裸脚本名：`bash "$(dirname "$0")/run_foo.sh"`。
#    run_excerpt_host_tests.sh 就是这么调 run_excerpt_ai_golden.sh 的——只认 ① 的话，
#    这两个脚本明明在门禁里跑着，却会被报成孤儿。**低估覆盖比高估危险得多**：
#    孤儿可以补登记，被误判为「已覆盖」的洞没人会再去看。
#    只认 `bash/sh/python3` 后面的文件名，注释与字符串里提到名字不算调用。
CALL_START_RE = re.compile(r"(?:^|[\s;|&(])(?:bash|sh|python3?|source)\s", re.M)
SCRIPT_NAME_RE = re.compile(r"([A-Za-z0-9_][A-Za-z0-9_.\-]*\.(?:sh|py))\b")
# gcc/cc 命令行里的裸 `.c` 文件名：`gcc ... book_epub_host_test.c -o ...`。
# 这些同样是「被门禁编译并运行」的资产，漏了它们会把在跑的用例报成孤儿。
CC_LINE_RE = re.compile(r"(?:^|[\s;|&(])(?:gcc|cc|clang)\s")
C_NAME_RE = re.compile(r"([A-Za-z0-9_][A-Za-z0-9_.\-]*\.c)\b")

failures = []


def bad(msg):
    failures.append(msg)


def is_test_asset(name):
    return any(p.match(name) for p in TEST_PATTERNS)


def script_children(path):
    """脚本直接引用的 tools/ 内文件（相对仓库根）。"""
    try:
        text = path.read_text(errors="replace")
    except OSError:
        return set()
    out = set()
    for m in PATH_RE.findall(text):
        out.add(m)
    # 调用行里取**最后一个**脚本名：`bash "$(dirname "$0")/run_foo.sh"` 的最后一个
    # 才是被调的脚本，前面的 `$0` 之类只是路径碎片。
    for line in text.splitlines():
        if not CALL_START_RE.search(line):
            continue
        names = SCRIPT_NAME_RE.findall(line)
        if names:
            cand = "tools/" + names[-1]
            if (ROOT / cand).exists():
                out.add(cand)
    for line in text.splitlines():
        if not CC_LINE_RE.search(line):
            continue
        for name in C_NAME_RE.findall(line):
            cand = "tools/" + name
            if (ROOT / cand).exists():
                out.add(cand)
    return out


def closure(entry):
    """从入口出发的传递闭包（相对仓库根的 posix 路径集合）。

    踩过一次坑，记在这里：初版写成了「先展开 .sh，再补一轮展开其余」，而第一轮
    已经把 `.py` 塞进 seen——第二轮开头 `if cur in seen` 直接跳过它。结果
    `test_book_epub.py`（在门禁内）编译的 `book_epub_host_test.c` 被判成孤儿，
    而「在跑的东西被报成没跑」和「没跑的东西被报成在跑」一样坏。
    One pass, one queue, every existing file expanded exactly once.
    """
    seen = set()
    pending = [entry]
    while pending:
        cur = pending.pop()
        p = cur if isinstance(cur, pathlib.Path) else (ROOT / cur)
        key = p.resolve()
        if key in seen:
            continue
        seen.add(key)
        if not p.exists() or not p.is_file():
            continue
        for child in script_children(p):
            cp = (ROOT / child).resolve()
            if cp not in seen:
                pending.append(child)
    out = set()
    for p in seen:
        try:
            out.add(p.relative_to(ROOT).as_posix())
        except ValueError:
            pass  # 入口脚本本身在仓库外，不算资产
    return out


def build_owner_map():
    """tools/*.c → 引用它的脚本集合（相对仓库根）。

    为什么需要：一个 `.c` 不会自己跑，它是被某个 runner 的 gcc 行编出来的。
    所以 `.c` 的「在不在门禁内」不是一个独立事实——**它跟随它的 runner**。
    不建这张图，盘点会把「runner 在外」放大成「runner + 它编的 20 个 .c 全在外」，
    96 条红里 60 条是同一个事实的复读。
    """
    owners = {}
    for p in sorted((ROOT / "tools").iterdir()):
        if not p.is_file() or p.suffix not in (".sh", ".py"):
            continue
        try:
            text = p.read_text(errors="replace")
        except OSError:
            continue
        for m in PATH_RE.findall(text):
            if m.endswith(".c"):
                owners.setdefault(m, set()).add("tools/" + p.name)
    return owners


def load_registry():
    reg = {}
    if not REGISTRY.exists():
        return reg
    for lineno, raw in enumerate(REGISTRY.read_text(errors="replace").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = [p.strip() for p in raw.split("\t") if p.strip()]
        if len(parts) < 3:
            bad("① %s:%d is not `path<TAB>state<TAB>reason`: an entry without a reason "
                "is exactly the memory item this batch exists to delete"
                % (REGISTRY.name, lineno))
            continue
        path, state, reason = parts[0], parts[1], parts[2]
        if state not in ("out-ok", "out-gap"):
            bad("① %s:%d has state %r: only `out-ok` (deliberately outside) or `out-gap` "
                "(a known hole) are meaningful - anything else hides which of the two it is"
                % (REGISTRY.name, lineno, state))
            continue
        if len(reason) < 8:
            bad("① %s:%d reason %r is too short to be a decision: say WHY it is outside"
                % (REGISTRY.name, lineno, reason))
        reg[path] = (state, reason)
    return reg


def main():
    entry = None
    for cand in ENTRY_CANDIDATES:
        if cand.exists():
            entry = cand
            break
    if entry is None:
        bad("① no gate entry script found (looked for %s): with no entry there is no "
            "coverage to inventory, and an empty inventory is not a clean one"
            % ", ".join(str(c) for c in ENTRY_CANDIDATES))
        for f in failures:
            print("FAIL: " + f)
        return 1

    inside = closure(entry)
    reg = load_registry()

    owners = build_owner_map()
    followers = {}
    tools = ROOT / "tools"
    assets = sorted(
        "tools/" + p.name for p in tools.iterdir()
        if p.is_file() and is_test_asset(p.name)
    )

    orphans, gaps, oks, ins = [], [], [], []
    for rel in assets:
        if rel in inside:
            ins.append(rel)
            if rel in reg and reg[rel][0] == "out-ok":
                bad("① %s is registered `out-ok` but the entry actually runs it: the "
                    "registry contradicts the gate, and the next reader will trust the "
                    "registry" % rel)
            continue
        if rel not in reg:
            if rel.endswith(".c"):
                owner = sorted(o for o in owners.get(rel, ()) if o not in inside)
                if owner:
                    # 归属的 runner 也在外：这条不是新缺口，记在 runner 名下。
                    followers.setdefault(owner[0], []).append(rel)
                    continue
            orphans.append(rel)
            continue
        state, _ = reg[rel]
        (gaps if state == "out-gap" else oks).append(rel)

    for rel in orphans:
        bad("① %s is outside the gate and has no registry row: it is not 'deliberately "
            "excluded', it is forgotten. Add a line to %s saying why (out-ok) or admit "
            "it is a hole (out-gap)" % (rel, REGISTRY.name))

    for rel in sorted(reg):
        p = ROOT / rel
        if not p.exists():
            bad("① %s is in %s but the file is gone: a stale row keeps a retired test "
                "looking alive" % (rel, REGISTRY.name))
        elif not is_test_asset(p.name):
            bad("① %s is registered but does not look like a test asset: the registry "
                "is drifting away from what it claims to inventory" % rel)

    if "--list" in sys.argv:
        print("gate entry: %s" % entry)
        print("inside the gate (%d):" % len(ins))
        for rel in ins:
            print("  IN      %s" % rel)
        print("registered outside, accepted (%d):" % len(oks))
        for rel in oks:
            print("  OUT-OK  %-52s %s" % (rel, reg[rel][1]))
        if followers:
            print("compiled by an outside runner (follow their owner, %d):"
                  % sum(len(v) for v in followers.values()))
            for owner in sorted(followers):
                print("  -> %-48s %d file(s)" % (owner, len(followers[owner])))
        print("registered outside, holes (%d):" % len(gaps))
        for rel in gaps:
            print("  OUT-GAP %-52s %s" % (rel, reg[rel][1]))

    if failures:
        for f in failures:
            print("FAIL: " + f)
        print("\ngate inventory: %d failure(s)" % len(failures))
        return 1
    print("gate inventory: %d asset(s) - %d inside the entry closure, %d registered "
          "outside (%d accepted, %d open holes), %d following their runner, 0 orphans "
          "- PASS"
          % (len(assets), len(ins), len(oks) + len(gaps), len(oks), len(gaps),
             sum(len(v) for v in followers.values())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
