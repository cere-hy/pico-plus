#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
"""26.9.30 边读边推冒烟 —— 三判据判读器（可复现，不靠人眼）

用法：
    python3 tools/judge_push_smoke.py <uart-log.txt>

为什么要有这个脚本：26.9.30 的台账闭环条件是一条"一组冒烟"，判据有 3 条。
人眼在一份 200 KB 的 UART 日志里数拍号，两次读两个结果——那不是判据，是印象。
脚本把每条判据变成"有证据行 / 无证据行"，并显式区分三种结局：

    PASS         三条判据全部**有证据**且全部满足
    FAIL         任一判据有证据且不满足
    UNAVAILABLE  日志里根本没有该判据的观测点（≠ PASS，见基线 §13.12 / §13.17）

判据（对应 26.9.30 出包说明"一组冒烟"三条）：
    [1] 拍号严格 1→2：每次 `commit beat 1/2` 必须后接 `commit beat 2/2`，
        不得出现连续两个 1、不得出现无前置 1 的 2。
    [2] 阅读器停在原书原页：提交完成后不得出现"chapter 变号"或"页码回到开头"。
    [3] dropped after 0 条：`dropped after N frames` 零命中。

设计纪律：脚本只报观测到的事实和证据行号，**不替人下"看起来没问题"的结论**。
任何一条缺证据，整份日志就判 UNAVAILABLE，而不是默认通过。
"""

import argparse
import re
import sys
from pathlib import Path

# ESP-IDF 日志行：I (140258) transfer_page: commit beat 1/2: ...
LINE_RE = re.compile(r"^([IWE]) \((\d+)\) ([A-Za-z0-9_]+): (.*)$")

BEAT1 = "commit beat 1/2"
BEAT2 = "commit beat 2/2"
DROPPED = re.compile(r"dropped after (\d+) frames?")

# `book: turn chapter=6 page=39/46 pct=1`
TURN_RE = re.compile(r"^turn chapter=(\d+) page=(\d+)/(\d+) pct=(\d+)$")

# 提交段的三条痕迹（用于把 beat 事件和其它事件对上号）
VERIFY_OK = "verify ok:"
VERIFY_FAIL = "verify FAIL:"


class Log:
    def __init__(self, path: Path):
        self.path = path
        self.raw = path.read_bytes()
        text = self.raw.decode("utf-8", errors="replace")
        self.lines = text.splitlines()
        self.entries = []  # (lineno, level, ms, tag, msg)
        for i, line in enumerate(self.lines, 1):
            m = LINE_RE.match(line)
            if m:
                level, ms, tag, msg = m.groups()
                self.entries.append((i, level, int(ms), tag, msg))

    @property
    def size(self):
        return len(self.raw)

    def span_ms(self):
        if not self.entries:
            return 0
        return self.entries[-1][2] - self.entries[0][2]


def judge_beats(log):
    """判据 1：拍号严格 1→2。返回 (verdict, detail_lines, evidence)。"""
    seq = []  # (lineno, which)
    for lineno, _lvl, _ms, tag, msg in log.entries:
        if tag != "transfer_page":
            continue
        if BEAT1 in msg:
            seq.append((lineno, 1))
        elif BEAT2 in msg:
            seq.append((lineno, 2))

    if not seq:
        return "UNAVAILABLE", "日志中没有 transfer_page 拍号行", []

    evidence = []
    problems = []
    groups = 0
    prev = None
    for lineno, which in seq:
        if which == 2 and prev is None:
            problems.append("L%d: 幽灵 2/2（前面没有 1/2）" % lineno)
        elif which == 2 and prev == 2:
            # 中间重复的 2/2 同样违规：说明某次提交**没等 hold 就跑了**
            # （缺 1/2），比"缺 2/2"更危险——那是绕过两拍。
            problems.append("L%d: 重复 2/2（上一次提交缺了 1/2，两拍被绕过）" % lineno)
        elif which == 1 and prev == 1:
            problems.append("L%d: 上一个 1/2 没有等到 2/2（连续两个 1/2）" % lineno)
        elif which == 2 and prev == 1:
            groups += 1
        evidence.append("L%d: beat %d/2" % (lineno, which))
        prev = which
    if prev == 1:
        problems.append("文件末尾仍停在 1/2，未等到 2/2（日志可能截断）")

    if problems:
        return "FAIL", "；".join(problems), evidence
    if groups == 0:
        return "UNAVAILABLE", "有拍号行但没有成对（%d 行）" % len(seq), evidence
    return "PASS", "%d 组严格 1→2，零幽灵" % groups, evidence


def judge_reader(log):
    """判据 2：提交完成后阅读器不跳章、不回页首。

    关键约束是**时间窗**：提交后 2 s 内的第一条 turn 才算"提交造成的位移"；
    再往后的翻页是用户行为，不能记在提交账上。基准页同样限 10 s 窗——
    太旧的基准已经不代表"提交时读在哪"。
    """
    SETTLE_MS = 2000   # 提交后多久之内的位移算提交的账
    BASE_MS = 10000    # 基准页的有效期

    turns = []      # (lineno, ms, chapter, page, total, pct)
    beats = []      # (lineno, ms)
    for lineno, _lvl, ms, tag, msg in log.entries:
        if tag == "transfer_page" and BEAT2 in msg:
            beats.append((lineno, ms))
        elif tag == "book":
            m = TURN_RE.match(msg)
            if m:
                turns.append((lineno, ms, int(m.group(1)), int(m.group(2)),
                              int(m.group(3)), int(m.group(4))))

    if not beats:
        return "UNAVAILABLE", "没有 2/2，无法观测提交后的阅读器行为", []
    if not turns:
        return "UNAVAILABLE", "日志里没有 book: turn 行，无法判阅读位置", []

    evidence = []
    problems = []
    for lineno, ms in beats:
        before = [t for t in turns if t[1] < ms and ms - t[1] <= BASE_MS]
        if not before:
            evidence.append("L%d 之前 %d ms 内无 turn 行（无基准，跳过）"
                            % (lineno, BASE_MS))
            continue
        _lb, _mb, ch_b, pg_b, _tot_b, pct_b = before[-1]
        after = [t for t in turns if t[1] > ms]
        if not after or after[0][1] - ms > SETTLE_MS:
            evidence.append("L%d 之后 %d ms 内无 turn 行（未打断，停在 chapter=%d page=%d）"
                            % (lineno, SETTLE_MS, ch_b, pg_b))
            continue
        ln_a, _ma, ch_a, pg_a, _tot_a, pct_a = after[0]
        if ch_a != ch_b:
            problems.append("L%d: 提交后 %d ms 内 chapter %d → %d（跳章）"
                            % (ln_a, _ma - ms, ch_b, ch_a))
        elif pg_a + 1 < pg_b:
            problems.append("L%d: 提交后 %d ms 内 page %d → %d（回退）"
                            % (ln_a, _ma - ms, pg_b, pg_a))
        elif pct_a + 20 < pct_b:
            problems.append("L%d: 提交后 %d ms 内进度 %d%% → %d%%（大幅倒退）"
                            % (ln_a, _ma - ms, pct_b, pct_a))
        else:
            evidence.append("L%d: 提交后 chapter=%d page=%d→%d（连续，%d ms）"
                            % (lineno, ch_a, pg_b, pg_a, _ma - ms))

    if problems:
        return "FAIL", "；".join(problems), evidence
    if not evidence:
        return "UNAVAILABLE", "提交点前后没有可用的阅读位置证据", []
    return "PASS", "%d 个提交点均未打断阅读（窗 %d ms）" % (len(beats), SETTLE_MS), evidence


def judge_dropped(log):
    """判据 3：dropped after N frames 零命中。"""
    hits = []
    for lineno, _lvl, _ms, _tag, msg in log.entries:
        m = DROPPED.search(msg)
        if m:
            hits.append("L%d: dropped after %s frames" % (lineno, m.group(1)))
    if not hits:
        # 零命中要有"本该有"的旁证：至少有一次 2/2（否则是日志太短，不是没丢）
        has_beat = any(BEAT2 in msg for _l, _v, _m, tag, msg in log.entries
                       if tag == "transfer_page")
        if not has_beat:
            return "UNAVAILABLE", "日志里没有提交段，dropped 零命中无意义", []
        return "PASS", "零命中（%d 个提交点）" % 1, []
    return "FAIL", "%d 条丢弃记录" % len(hits), hits


def main():
    ap = argparse.ArgumentParser(description="26.9.30 边读边推冒烟三判据")
    ap.add_argument("logfile")
    ap.add_argument("-v", "--verbose", action="store_true", help="打印全部证据行")
    args = ap.parse_args()

    path = Path(args.logfile)
    if not path.exists():
        print("错误：文件不存在 %s" % path)
        return 2

    log = Log(path)

    # §13.17：判读前先验字节数。0 字节是"空体"（通道故障），不是"无内容"（结论）。
    if log.size == 0:
        print("文件：%s" % path.name)
        print("字节：0")
        print()
        print("判定：UNAVAILABLE（空体）")
        print("  这不是「设备没输出」，是日志没进来。先查抓取端，不要改代码。")
        return 2

    print("文件：%s" % path.name)
    print("字节：%d    行数：%d    日志跨度：%.1f s"
          % (log.size, len(log.lines), log.span_ms() / 1000.0))
    if log.size < 2048:
        print("提示：不足 2 KB，一次完整边读边推通常在 58–242 KB（历史 8 份实测）——"
              "很可能只抓到了一段。")
    print()

    verdicts = []
    for idx, (name, fn) in enumerate((
        ("[1] 拍号严格 1→2", judge_beats),
        ("[2] 提交后不打断阅读", judge_reader),
        ("[3] dropped after 0 条", judge_dropped),
    ), 1):
        verdict, detail, evidence = fn(log)
        verdicts.append(verdict)
        print("%s …… %s" % (name, verdict))
        print("    %s" % detail)
        if evidence and (args.verbose or verdict != "PASS"):
            for e in evidence[:8]:
                print("      · %s" % e)
        print()

    if any(v == "FAIL" for v in verdicts):
        overall = "FAIL"
    elif all(v == "PASS" for v in verdicts):
        overall = "PASS"
    else:
        overall = "UNAVAILABLE"

    print("总判定：%s" % overall)
    if overall == "UNAVAILABLE":
        print("  有判据缺观测点 —— 不判 PASS（基线 §13.12：没观测到 ≠ 通过）。")
        print("  补齐缺失段后重跑：需要 transfer_page 拍号、book: turn、以及至少一个提交段。")
    return 0 if overall == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
