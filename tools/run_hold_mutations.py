#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
"""
26.9.30 commit 3 变异轮（复刻型）：每个变异逐字复刻一个**真实存在过的**缺陷形态，
不是随手改坏语法。判据只有一个——B46 必须红；红不出来说明牙没咬住。

Mutants for 26.9.30 commit 3: each one reproduces a defect shape that really
happened. The only criterion is that B46 turns red; a survivor means the tooth
does not bite.

制度（26.9.31a 立）：**替换必须 assert 生效**。否则「存活」与「没打中」在输出上
完全一样——M129 就这样骗过一次。
House rule (set in 26.9.31a): every replacement must be asserted. Otherwise a
survivor and a miss look identical, which is exactly how M129 fooled us.

用法 / usage: python3 tools/run_hold_mutations.py  （退出码 0 = 全部 killed）
"""
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TRANSFER = ROOT / "main/apps/app_transfer.c"
BOOK = ROOT / "main/apps/app_book.c"
TEETH = ROOT / "tools/check_excerpt.py"

# (id, 文件, 原文, 替换, 期望出现的判据关键字, 复刻的是什么)
MUTANTS = [
    (
        "M133",
        TRANSFER,
        "    if (!app_book_release_hold(PUSH_TEXT_PATH)) {",
        "    if (0) {",
        "never calls app_book_release_hold",
        "commit 2 单独出包：四拍搬离了 BLE 栈，读者句柄照旧被 rename 抽走（机制二原形）",
    ),
    (
        "M134",
        TRANSFER,
        "    if (!s_commit_hold_wait) return;",
        "    if (!s_commit_hold_wait) return;\n    (void)push_sink_run_commit(); // M134: 先换文件，后关书",
        "releases the reader AFTER run_commit",
        "顺序写反：文件已经换掉了才去关书，句柄指向的簇早已回到 FAT",
    ),
    (
        "M135",
        BOOK,
        "    free_book();   // 完整关书：text/blocks/runs/images/page_images + book_close() 关句柄",
        "    book_close();",
        "calls book_close() itself",
        "「只关句柄、保留阅读态」假修复（v36 §三.2 那行）：下一页 fread(NULL) 半死阅读器",
    ),
    (
        "M136",
        BOOK,
        "    if (s_prep_task) busy |= BOOK_HOLD_PREP;",
        "    /* M136: prep 位被删 */",
        "dropped BOOK_HOLD_PREP",
        "不等 prep 停机就释放：行循环每行读 s_text，free_book 之后就是 UAF",
    ),
    (
        "M137",
        TRANSFER,
        "        push_sink_commit_abort(PUSH_ERR_BUSY);",
        "        ;",
        "drop the request silently",
        "死线外静默丢请求：手机端只能等超时，且分不清「忙」与「无卡」",
    ),
]


def run_teeth():
    p = subprocess.run([sys.executable, str(TEETH)], capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def main():
    survivors = []
    for mid, path, old, new, needle, what in MUTANTS:
        original = path.read_text(errors="replace")
        if old not in original:
            print("MISS  %s: the anchor is absent - the mutation never landed, so a "
                  "'survived' verdict would be meaningless (%s)" % (mid, what))
            survivors.append(mid + "(miss)")
            continue
        mutated = original.replace(old, new, 1)
        if mutated == original:
            print("MISS  %s: replacement produced identical text" % mid)
            survivors.append(mid + "(miss)")
            continue
        path.write_text(mutated)
        try:
            code, out = run_teeth()
        finally:
            path.write_text(original)
        if code == 0:
            print("SURVIVED %s: B46 stayed green - %s" % (mid, what))
            survivors.append(mid)
        elif needle not in out:
            print("SURVIVED %s: B46 went red but not on the expected tooth (%s)" % (mid, needle))
            print(out)
            survivors.append(mid + "(wrong-tooth)")
        else:
            print("killed %s: %s" % (mid, what.split("：")[0]))
    if survivors:
        print("\n%d survivor(s): %s" % (len(survivors), ", ".join(survivors)))
        return 1
    print("\ncommit-3 mutation round: %d/%d killed" % (len(MUTANTS), len(MUTANTS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
