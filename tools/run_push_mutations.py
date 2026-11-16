#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
"""
26.9.30 commit 5 变异轮（复刻型）：`.part` 账本与欠账顺序。

每个变异逐字复刻一个真实存在过的缺陷形态，判据只有一个——**指名用例必须红**，
而且红必须落在指名的那条上。审计 2026-10-31 的原话：「16e 不在场时，红落在哪个
用例上无从指认，红在别处不算 killed」——这一条就是 M101 靶点漂移的原形态。

Mutants for commit 5, each reproducing a defect that really happened. The only
criterion is that the NAMED case turns red, on that case. A red elsewhere does
not count: that is target drift, the shape of M101.

制度 / house rules:
  1. 替换必须 assert 生效（M129 的教训）。/ every replacement must be asserted.
  2. 红必须带用例号（`RED [16e]`），否则「红在别处」无法分辨。
  3. 串行跑：与摘抄的变异脚本共用工作区。/ serial: shares the worktree.

用法 / usage: python3 tools/run_push_mutations.py   （退出码 0 = 全部 killed）
"""
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
PART = ROOT / "components/push_sink/src/push_part.c"
SINK = ROOT / "components/push_sink/src/push_sink.c"
RUNNER = ROOT / "tools/run_push_sink_host_tests.sh"
TEETH = ROOT / "tools/check_excerpt.py"

# (id, 文件, 原文, 替换, 判据类型, 期望红的判据, 复刻的是什么)
# 判据类型：cases = 宿主用例（红须落在指名用例上）；teeth = 静态牙（红须指名 B 号）。
MUTANTS = [
    (
        "M120",
        PART,
        "    if (!push_part_drain()) return false;",
        "    /* M120: 欠账不先补，新包直接写 */",
        "cases",
        "16g",
        "新包先于欠账落盘：顺序追加一旦乱序就是永久损坏（v36 §五.3 的顺序不变式）",
    ),
    (
        "M121b",
        PART,
        "    s_part_len = received;",
        "    s_part_len = 0;",
        "cases",
        "16e",
        "续传起点没就位（adopt 后账本仍为 0）：首包一失败就回退到 0，"
        "把 NVS 锚定的已收字节整段抹掉",
    ),
    (
        "M121c",
        PART,
        "    s_part_len = 0;\n    s_owed_valid = false;\n    s_owed_len = 0;\n    return true;\n}",
        "    s_owed_valid = false;\n    s_owed_len = 0;\n    return true;\n}",
        "cases",
        "16f",
        "全新会话没清账本：文件被 \"wb\" 截到 0，账本还是上一个会话的长度，"
        "第一次回退把文件延长成 0 填充",
    ),
    (
        "M121",
        PART,
        "    if (truncate(PART_PATH, (off_t)s_part_len) != 0) return false;",
        "    /* M121: 回退只放句柄，不截断 */",
        "cases",
        "16e",
        "回退不截断：短写之后从脏位置继续写，.part 多出半包残字节（形状对、位置错）",
    ),
    (
        "M118",
        SINK,
        "    if (rename(PUSH_TEXT_PATH, PUSH_BAK_PATH) != 0 && errno != ENOENT) {",
        "    if (remove(PUSH_TEXT_PATH) != 0 && errno != ENOENT) {",
        "teeth",
        "B49",
        "gen16 的原写法：先删成品再改名。改名一失败，旧书就没了——26.9.30 调查里"
        "「文件消失」的唯一机制",
    ),
    (
        "M118b",
        SINK,
        "        if (rename(PUSH_BAK_PATH, PUSH_TEXT_PATH) != 0) {",
        "        if (0) {",
        "teeth",
        "B49",
        "拍 2 失败不回滚：读者手里既没有新书也没有旧书，.bak 也不会回位",
    ),
    (
        "M119",
        SINK,
        "    if (!recover_bak_if_needed()) return false;\n    if (remove(PUSH_BAK_PATH) != 0 && errno != ENOENT) {",
        "    if (remove(PUSH_BAK_PATH) != 0 && errno != ENOENT) {",
        "teeth",
        "B50",
        "先清后捡：上一次被打断的提交留下的唯一那份书，被当成垃圾删掉",
    ),
    # ---- 26.11.1 注入三案补课（出包门第 4 条） ----
    # 三把刀各咬「接住 / 记录 / 放弃」里的一环，靶点是新增的 16h 与 16i。
    # Three teeth, one per link of "catch / record / give up"; targets are 16h and 16i.
    (
        "M122",
        PART,
        "    if (!part_rewind()) return false; // 回不去 = 保证不了不重复 / cannot prove no duplication",
        "    /* M122: 重试前不回退，从脏位置接着写 */",
        "cases",
        "16h",
        "重试前不回退：短写之后从脏位置继续写，包内容错位且长度不对"
        "（16g 同缺陷也会红——同一处缺陷的两个观察点，指名靶是 16h）",
    ),
    (
        "M123",
        PART,
        "    push_part_close();\n    return false;\n}",
        "    /* M123: 给不出去了也不关句柄 */\n    return false;\n}",
        "cases",
        "16i",
        "欠账补不上却不关句柄：CANCEL 走 goto_idle 不 discard，句柄跨会话残留，"
        "下一个 ARM 的 adopt truncate 被单开约定挡死（v36 :986 的软硬不对称）",
    ),
    (
        "M124",
        PART,
        "    s_owed_valid = false;\n    // 这里**不**回退",
        "    /* M124: 槽不作废，留着下次再试 */\n    // 这里**不**回退",
        "cases",
        "16i",
        "补不上的槽不作废：下一包进门先 drain 又失败一次，整篇在同一个死包上反复空转，"
        "而回调栈上等着的是下一条数据（只有一格，重试就是丢包）",
    ),
]


def run_cases():
    p = subprocess.run(["bash", str(RUNNER)], capture_output=True, text=True, cwd=str(ROOT))
    return p.returncode, p.stdout + p.stderr


def run_teeth():
    p = subprocess.run([sys.executable, str(TEETH)], capture_output=True, text=True, cwd=str(ROOT))
    return p.returncode, p.stdout + p.stderr


def main():
    survivors = []
    for mid, path, old, new, kind, target, what in MUTANTS:
        original = path.read_text(encoding="utf-8")
        if old not in original:
            print("MISS  %s: anchor absent - the mutation never landed (%s)" % (mid, what))
            survivors.append(mid + "(miss)")
            continue
        if original.count(old) != 1:
            print("MISS  %s: anchor is ambiguous (%d hits)" % (mid, original.count(old)))
            survivors.append(mid + "(ambiguous)")
            continue
        mutated = original.replace(old, new, 1)
        assert mutated != original, "%s: replacement produced identical text" % mid
        path.write_text(mutated, encoding="utf-8")
        try:
            code, out = run_cases() if kind == "cases" else run_teeth()
        finally:
            path.write_text(original, encoding="utf-8")
        needle = ("RED [%s]" % target) if kind == "cases" else ("FAIL: %s " % target)
        if code == 0:
            print("SURVIVED %s: all cases stayed green - %s" % (mid, what))
            survivors.append(mid)
        elif needle not in out:
            print("SURVIVED %s: red, but not on %s - target drift (%s)" % (mid, target, what))
            for line in out.splitlines():
                if line.startswith("RED "):
                    print("    " + line)
            survivors.append(mid + "(wrong-case)")
        else:
            reds = [l for l in out.splitlines() if l.startswith(needle)]
            print("killed %s: %s (%d red on %s)" % (mid, what.split("：")[0], len(reds), target))
    if survivors:
        print("\n%d survivor(s): %s" % (len(survivors), ", ".join(survivors)))
        return 1
    print("\ncommit-5 mutation round: %d/%d killed on the named target" % (len(MUTANTS), len(MUTANTS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
