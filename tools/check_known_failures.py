#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
"""登记簿的账本牙 / The ledger tooth for the known-failure registry.

中文：
  `tools/KNOWN_FAILURES.md §7` 是**账**，不是免罪符。这个脚本把它当账查：

    1. 每一条都得写清影响面、补偿方、还账触发、还账日期（`YYYY-MM-DD`）；
    2. 还账日期过了没还 → **红**（到期未还账不是提醒，是门禁失败）；
    3. 登记条数必须等于所有 runner 里 `# known-fail: KF-x.y` 标记的实际个数
       —— 登记了没人放行是空账，放行了没登记是暗账，两种都红；
    4. 不许出现「永久」二字作为还账触发；
    5. **第四方向（S26-4 收尾补）**：每个**放行点**都必须被一条标记认领。放行点有两种
       形态——`tolerant <tag> '<shape>' <cmd>` 调用、以及内联分支打印 `<suite>: KNOWN-FAIL`。
       既没有标记、也没有登记行的放行点是**静默容忍**：账本牙前三个方向全部看不见它
       （ui_kit 就这么活了好几轮，靠人眼抓的）。一条标记也只能认领一个放行点，
       否则「一个标记罩两个放行」还是暗账。

  只增不减的登记簿不是豁免机制，是坟场；真回归最爱躲的地方。牙的作用就是让它
  没法悄悄长青苔。

English:
  §7 of KNOWN_FAILURES.md is a LEDGER, not an indulgence. This tooth reads it as one:
  every row needs impact / compensating owner / payoff trigger / payoff date; an overdue
  date is a hard failure; the row count must equal the number of `# known-fail: KF-x.y`
  markers across every tolerating runner (a row nobody tolerates and a tolerance nobody
  registered are both red); and "permanent" is not an option. The fourth direction: every
  release point (a `tolerant` call, or an inline `<suite>: KNOWN-FAIL` branch) must be
  claimed by exactly one marker - an unmarked release point is a silent tolerance that the
  other three directions cannot see at all.

用法 / usage: python3 tools/check_known_failures.py   （0 = 通过 / 0 = pass）
"""
import datetime
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
REGISTRY = ROOT / "tools/KNOWN_FAILURES.md"
# 所有会「放行登记形态」的 runner 都要扫：账本不能只盯着自己眼前那个文件。
# Every runner that tolerates a registered shape is scanned - a ledger that only watches
# one file develops a blind spot exactly where the oldest entry lives.
RUNNERS = (
    ROOT / "tools/run_book_host_tests.sh",
    ROOT / "tools/run_book_management_tests.sh",
)

PLACEHOLDERS = {"", "-", "—", "待定", "TBD", "todo", "TODO"}

# 放行点的两种形态；标记的形态。识别的对象是「哪一行在放行」，不是排版。
# The two shapes a release point takes, plus the marker shape. We match what releases a
# failure, not how it is laid out.
MARK_RE = re.compile(r"#\s*known-fail:\s*(KF-[\d.]+)")
TOLERANT_RE = re.compile(r"^\s*tolerant\s+(\S+)")


def release_points(runner):
    """把一个 runner 拆成「标记」与「放行点」两份有序清单。

    Returns (marks, points) with 1-based line numbers: marks = [(line, "KF-x.y")],
    points = [(line, kind, label)].

    判据的边界写在这里，免得将来被顺手放宽：
      - 纯注释行只收标记，注释里提到 KNOWN-FAIL 不算放行（否则文档会把自己咬成暗账）；
      - `tolerant` 函数**定义**（`tolerant() {`）不是放行点，它后面没有标签参数；
      - 函数体内的通用模板 `echo "$tag: KNOWN-FAIL"` 不是放行点——它是通道实现，
        具体是哪一套由调用方的标签决定。
    """
    marks, points = [], []
    for lineno, line in enumerate(runner.read_text(encoding="utf-8",
                                                   errors="replace").splitlines(), 1):
        if line.lstrip().startswith("#"):
            m = MARK_RE.search(line)
            if m:
                marks.append((lineno, m.group(1)))
            continue
        m = TOLERANT_RE.match(line)
        if m:
            points.append((lineno, "tolerant", m.group(1)))
            continue
        if "KNOWN-FAIL" in line and "$tag" not in line:
            points.append((lineno, "inline", line.strip()[:48]))
    return marks, points


def registry_rows():
    """解析 §7 的 KF- 表格行。 / Parse the KF- rows of section 7."""
    text = REGISTRY.read_text(encoding="utf-8", errors="replace")
    start = text.find("## 7.")
    if start < 0:
        return None
    rows = []
    for line in text[start:].splitlines():
        if not line.startswith("| KF-"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        rows.append(cells)
    return rows


def main():
    bad = []
    rows = registry_rows()
    if rows is None:
        print("FAIL the registry has no §7: the ledger itself is gone")
        return 1
    if not rows:
        print("FAIL §7 lists no KF- row: an empty ledger means the four black suites "
              "are silently unprotected")
        return 1

    today = datetime.date.today()
    dues = []
    for cells in rows:
        if len(cells) != 6:
            bad.append("%s: expected 6 columns (id / suite / impact / owner / trigger / "
                       "due), got %d" % (cells[0], len(cells)))
            continue
        kid, suite, impact, owner, trigger, due = cells
        for label, value in (("影响面/impact", impact), ("补偿方/owner", owner),
                             ("还账触发/trigger", trigger)):
            if value in PLACEHOLDERS:
                bad.append("%s: empty %s - a row without it is a hole, not a ledger "
                           "entry" % (kid, label))
        if "永久" in trigger or "permanent" in trigger.lower():
            bad.append("%s: '永久/permanent' is not one of the three allowed triggers" % kid)
        m = re.fullmatch(r"(\d{4})-(\d{2})-(\d{2})", due)
        if not m:
            bad.append("%s: 还账日期 %r is not YYYY-MM-DD - a row without a date never "
                       "gets paid" % (kid, due))
            continue
        d = datetime.date(*map(int, m.groups()))
        dues.append(d)
        if d < today:
            bad.append("%s: OVERDUE since %s (today %s) - 到期未还账即门禁失败 / an "
                       "overdue row fails the gate, it does not just warn" % (kid, due, today))

    # 登记条数 == 所有 runner 里的实际放行标记数。空账与暗账都红。
    # Row count must equal the number of `# known-fail: KF-x.y` markers: a row nobody
    # tolerates and a tolerance nobody registered are both red.
    per_runner = {r.name: release_points(r) for r in RUNNERS}
    marked = [(name, kid) for name, (marks, _) in per_runner.items() for _, kid in marks]
    points_all = [(name, ln, kind, label)
                  for name, (_, points) in per_runner.items()
                  for ln, kind, label in points]

    # 第四方向：每个放行点都必须被一条标记认领，且一条标记只认领一个放行点。
    # 这一条抓的是前三个方向**结构上看不见**的东西——一个既无标记、也无登记行的
    # tolerant 调用 / 内联分支。它就是 ui_kit 那一类，靠人眼活了好几轮。
    # Fourth direction: every release point is claimed by exactly one marker (nearest one
    # above it). This is the blind spot of the other three directions: an unmarked release
    # point is a silent tolerance, and one marker covering two releases is still a dark entry.
    used = {}
    for name, ln, kind, label in points_all:
        cands = [(mln, kid) for mln, kid in per_runner[name][0]
                 if mln < ln and (name, mln) not in used]
        if not cands:
            bad.append("%s:%d releases a failure shape (%s: %s) with NO `# known-fail: KF-x.y` "
                       "marker above it - 静默容忍，账本看不见 / a silent tolerance the ledger "
                       "cannot see" % (name, ln, kind, label))
            continue
        mln, kid = cands[-1]
        used[(name, mln)] = (ln, kid)
    for name, (marks, _) in per_runner.items():
        for mln, kid in marks:
            if (name, mln) not in used:
                bad.append("%s:%d carries %s but no release point below it is claimed - "
                           "悬空标记，账目虚高 / a marker that claims nothing" % (name, mln, kid))

    ids = [c[0] for c in rows if len(c) == 6]
    if len(marked) != len(ids):
        bad.append("runners carry %d known-fail marker(s) %s but §7 registers %d - "
                   "空账或暗账都算红 / phantom or hidden entries are both failures"
                   % (len(marked), [m for _, m in marked], len(ids)))
    for name, mid in marked:
        if mid not in ids:
            bad.append("%s marks %s, which §7 does not register" % (name, mid))
    for kid in ids:
        if kid not in [m for _, m in marked]:
            bad.append("%s is registered but no runner marks it - a row nobody tolerates "
                       "is a phantom entry" % kid)
        if [m for _, m in marked].count(kid) > 1:
            bad.append("%s is marked more than once: one row, one tolerance point" % kid)

    if bad:
        for b in bad:
            print("FAIL " + b)
        return 1
    print("known-failures: %d registered (earliest due %s)"
          % (len(rows), min(dues).isoformat()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
