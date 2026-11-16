#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
"""送验台账的牙 / The tooth that keeps the acceptance ledger honest.

中文：
  验收对象是**跑六步当时的那个 bin**，不是版本号。所以 `docs/ACCEPTANCE_LEDGER.md` 把
  每次送验的 sha256 钉住，这个脚本逐条对账：

    1. 最新一行的 sha256 必须等于当前构建产物——否则「送验的 bin」与「现在的 bin」是
       两个东西，六步的结论没有落点；
    2. `CARRIED`（沿续）必须是真的沿续：上一行 sha256 相同 + 理由 ≥10 字符。
       **bin 变了还写 CARRIED 就红**——这是「bin 变一字节，六步作废重跑」的机器形态；
    3. 日期 `YYYY-MM-DD` 且不晚于今天（未来日期是手滑，不是计划）；
    4. 状态只认 PENDING / PASS / CARRIED / VOID 四个词，理由一律必填；
    5. 理由列**不许出现包哈希**（`8位hex…` 形态）。包是自指量：包内容一变哈希就变，
       记进包内文件立刻过期——包括这张表自己。台账只记 bin 的 sha256。

  没有产物时打印 SKIP 放行，但**必须打印**：静默跳过是假绿家族的老朋友。

English:
  Acceptance is bound to the exact image the six field steps ran against, not to a version
  string. The ledger pins each submission's sha256 and this tooth reconciles it: the newest
  row must equal the current build artifact; `CARRIED` (reuse the previous field run) is
  only legal when the sha256 is unchanged and a reason is given; otherwise the run is void.

用法 / usage:
  python3 tools/check_pkg_validity.py [--bin build_x/Read_Pico.bin]
  退出码 / exit: 0 = 通过或 SKIP，1 = 红
"""
import datetime
import hashlib
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
LEDGER = ROOT / "docs/ACCEPTANCE_LEDGER.md"
STATES = {"PENDING", "PASS", "CARRIED", "VOID"}
SHA_RE = re.compile(r"[0-9a-f]{64}")
# 台账数据行：第一格必须是 YYYY-MM-DD。判读表里 `| 20:51 |` 这类行不算数。
DATE_ROW_RE = re.compile(r"^\|\s*\d{4}-\d{2}-\d{2}\s*\|")


def ledger_rows():
    """取台账表的数据行（跳过表头与分隔行）。/ Data rows of the ledger table.

    只认「单元格 1 是 YYYY-MM-DD」的行。早先按 `| 20` 前缀判断，任何以 `| 20:51 |`
    开头的判读表都会被误收成台账行（2026-11-03 第十一批真的踩到了）。
    """
    text = LEDGER.read_text(encoding="utf-8", errors="replace")
    rows = []
    for line in text.splitlines():
        if not DATE_ROW_RE.match(line):
            continue
        rows.append([c.strip() for c in line.strip().strip("|").split("|")])
    return rows


def build_artifact(explicit=None):
    """当前构建产物。显式给路径优先；否则取 build_*/Read_Pico.bin 里最新的一份。

    The current artifact: an explicit path wins, else the newest build_*/Read_Pico.bin.
    取最新的理由：多树构建时 a/b/c 逐字节相同（冻结要求），mtime 最新那份就是「刚建出来的」。
    """
    if explicit:
        p = pathlib.Path(explicit)
        return p if p.exists() else None
    cands = [p for p in ROOT.glob("build_*/Read_Pico.bin") if p.exists()]
    # 主构建目录也叫 build（不是 build_*），它才是「这一批刚建出来的」那一份；
    # 归档树 build_2611xx 只用于对照。漏掉它时，门禁会拿上一批的归档 bin 去比台账。
    # The main tree is `build/` (not `build_*`); the archives are build_2611xx. Without
    # this, the gate compares the ledger against last batch's archived image.
    main_bin = ROOT / "build/Read_Pico.bin"
    if main_bin.exists():
        cands.append(main_bin)
    if not cands:
        return None
    return max(cands, key=lambda p: p.stat().st_mtime)


def sha256_of(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main(argv):
    explicit = None
    if "--bin" in argv:
        explicit = argv[argv.index("--bin") + 1]

    if not LEDGER.exists():
        print("FAIL %s is gone: without it, acceptance is bound to a version string again"
              % LEDGER.name)
        return 1
    rows = ledger_rows()
    if not rows:
        print("FAIL the acceptance ledger lists no submission: a candidate package that "
              "nobody pinned cannot be accepted or invalidated")
        return 1

    art = build_artifact(explicit)
    if art is None:
        print("pkg-validity: SKIP (no build_*/Read_Pico.bin; ledger has %d row(s), latest %s)"
              % (len(rows), rows[-1][4] if len(rows[-1]) > 4 else "?"))
        return 0

    bad = []
    today = datetime.date.today()
    prev_sha = None
    for cells in rows:
        if len(cells) != 6:
            bad.append("expected 6 columns (date / version / sha256 / size / state / reason), "
                       "got %d: %r" % (len(cells), cells[:2]))
            continue
        date_s, version, sha, size_s, state, reason = cells
        m = re.fullmatch(r"(\d{4})-(\d{2})-(\d{2})", date_s)
        if not m:
            bad.append("%s: 送验日期 %r is not YYYY-MM-DD" % (version, date_s))
        elif datetime.date(*map(int, m.groups())) > today:
            bad.append("%s: 送验日期 %s is in the future (today %s) - a future date is a typo, "
                       "not a plan" % (version, date_s, today))
        if not SHA_RE.fullmatch(sha):
            bad.append("%s: bin sha256 %r is not 64 lowercase hex" % (version, sha))
        if not size_s.isdigit():
            bad.append("%s: size %r is not a byte count" % (version, size_s))
        if state not in STATES:
            bad.append("%s: state %r is not one of %s" % (version, state, sorted(STATES)))
        if len(reason) < 10:
            bad.append("%s: reason %r is too short - a row has to say which package it is and "
                       "what was run" % (version, reason))
        # 包哈希是自指量：写进包内文件（本表就在包里）必然立刻过期。只许记 bin 的 sha256。
        # A package hash is self-referential inside the package: it goes stale the moment the
        # package is rebuilt. Only the binary's sha256 belongs here.
        if re.search(r"\b[0-9a-f]{8}\s*…", reason):
            bad.append("%s: the reason carries a package hash (%r) - package hashes are "
                       "self-referential and belong OUTSIDE the package; the ledger records "
                       "only the bin's sha256" % (version, reason[:48]))
        if state == "CARRIED":
            if prev_sha is None:
                bad.append("%s: CARRIED on the first row - there is no previous field run to "
                           "carry over" % version)
            elif prev_sha != sha:
                bad.append("%s: CARRIED but the bin changed (%s -> %s) - 沿续只对没变的 bin "
                           "成立，变了就得重跑 / reuse is illegal once a single byte moves"
                           % (version, prev_sha[:12], sha[:12]))
        prev_sha = sha

    art_sha = sha256_of(art)
    art_size = art.stat().st_size
    latest_sha, latest_state = rows[-1][2], rows[-1][4]
    if SHA_RE.fullmatch(latest_sha) and latest_sha != art_sha:
        bad.append("the newest ledger row pins %s but the current artifact %s is %s - 送验的 bin "
                   "不是现在这个 bin，六步结论无处安放 / the submitted image is not this image"
                   % (latest_sha[:12], art.name, art_sha[:12]))

    if bad:
        for b in bad:
            print("FAIL " + b)
        return 1
    print("pkg-validity: bin %s (%s B, %s) — ledger %d row(s), latest %s"
          % (art_sha[:12], format(art_size, ","), art, len(rows), latest_state))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
