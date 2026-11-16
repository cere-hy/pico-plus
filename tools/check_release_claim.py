#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
"""
B39 出包口径牙（26.9.29，审计裁决）：验收状态与功能边界不许共享同一个形容词。

规则只有一条：**台账最新一行不是 `PASS` / `CARRIED` 时，出包说明的"状态"列不许写"生效"**。
新包的台账在出包那一刻必然是 `PENDING`（六步还没跑），所以新包的出包说明只能写
"待验证"。26.9.28 那句 `全量生效` 就是在这个组合下被放大成"文档替六步签了字"。

用法 / usage:
    python3 tools/check_release_claim.py ../26.9.29-出包说明-20261031.md
    （不给文件 = SKIP；SKIP 必须出现在门禁输出里，静默跳过等于假绿）
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
LEDGER = ROOT / "docs/ACCEPTANCE_LEDGER.md"
STATUSES = ("PENDING", "PASS", "CARRIED", "VOID")
PROMISING = ("全量生效", "已生效", "生效")


def ledger_status():
    """台账最后一条状态。/ The status of the newest ledger row."""
    if not LEDGER.exists():
        return None
    latest = None
    for line in LEDGER.read_text(errors="replace").splitlines():
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        for cell in cells:
            if cell in STATUSES:
                latest = cell
    return latest


def check(path, status, failures):
    head = "\n".join(path.read_text(errors="replace").splitlines()[:6])
    if "作废" in head:
        # 作废件不再做任何承诺，它的历史措辞是证据，不是 claim。
        # An obsolete note promises nothing; its wording is evidence, not a claim.
        print("note: %s is marked 作废 - claims ignored (kept as evidence)" % path.name)
        return
    rows = 0
    for lineno, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) < 2:
            continue
        # "状态"列按本期出包说明的表格约定是第 2 列（功能 | 状态 | 说明）。
        # Column two is the status column in this repo's release-note tables.
        cell = cells[1]
        if not any(word in cell for word in PROMISING):
            continue
        rows += 1
        if status not in ("PASS", "CARRIED"):
            failures.append(
                "%s:%d claims %r while the ledger says %s - a release note may not sign for "
                "the six steps; write 待验证 instead" % (path.name, lineno, cell, status))
    if rows == 0:
        print("note: %s carries no 生效 row (nothing to bite)" % path.name)


def main(argv):
    if len(argv) < 2:
        print("SKIP: no release note passed to B39 - the claim tooth did NOT run")
        return 0
    status = ledger_status()
    if status is None:
        print("FAIL: B39 cannot read the ledger status from %s" % LEDGER)
        return 1
    failures = []
    for arg in argv[1:]:
        path = pathlib.Path(arg)
        if not path.is_absolute():
            path = (pathlib.Path.cwd() / path).resolve()
        if not path.exists():
            failures.append("B39 release note %s does not exist" % arg)
            continue
        check(path, status, failures)
    if failures:
        for f in failures:
            print("FAIL: " + f)
        return 1
    print("B39 release claim: ledger=%s, %d note(s) checked - PASS" % (status, len(argv) - 1))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
