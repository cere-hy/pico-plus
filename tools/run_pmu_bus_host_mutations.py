#!/usr/bin/env python3
"""26.11.19 解耦批：宿主刀（V-A / V-B / V-D / V-E），确认宿主用例能红。

方案 §4.3 的分层：V-A–V-E 是**宿主刀**——红出现在宿主真值表；
V-F 是**守卫刀**——红出现在守卫脚本（见 run_pmu_bus_guard_mutations.py）。
两者混排会让 V-F 在宿主里找不到红、被误记 SURVIVED，所以分两个文件跑。

刀值口径照 M3/G6：选语义上最像真缺陷的变体，不选等价改数字。
  V-A 摘采样支          → K1 无按键采样归零
  V-B 塞回值链读        → K2a 零值链读被打破
  V-D 取一即返不 ACK    → K4② 一次取空失败
  V-E 深度折 1          → K4② 一次取空失败（同族，粒度不同）
V-C（放宽上界）没有宿主落点：K3 的真机粗检在真机，宿主只做合成上界，另行处理。

每个变体 try/finally 还原并比对哈希。
"""

from pathlib import Path
import hashlib
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
loop = root / "main/app/app_loop.c"
pmu = root / "components/read_pico_pmu/read_pico_pmu.c"


def digest(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def body_span(text, name):
    m = re.search(r"^(?:static )?[^\n]+\b" + name + r"\([^;{}]*?\)\s*\{", text, re.M)
    assert m, name
    n = m.end()
    level = 1
    while level:
        if text[n] == "{":
            level += 1
        elif text[n] == "}":
            level -= 1
        n += 1
    return m.start(), n


def mutate_in_function(text, name, old, new):
    a, b = body_span(text, name)
    seg = text[a:b]
    assert old in seg, name
    return text[:a] + seg.replace(old, new, 1) + text[b:]


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=str(root))
    return r.returncode, r.stdout + r.stderr


HOST = ["sh", "tools/run_app_loop_host_tests.sh"]
UNIT = ["python3", "tools/pmu_bus_host_test.py"]

VARIANTS = [
    (
        "V-A 摘采样支（咬 K1）",
        loop,
        lambda s: s.replace(
            "if (pmu_bus_ok && pmu_bus_due(ctx.now_ms, last_sample_ms)) {",
            "if (false && pmu_bus_ok && pmu_bus_due(ctx.now_ms, last_sample_ms)) {", 1),
        HOST,
        "pmu_poll_calls >= 5",
    ),
    (
        "V-B 取键里塞回值链读（咬 K2a）",
        pmu,
        lambda s: mutate_in_function(
            s, "read_pico_pmu_take_key_action",
            "    uint16_t first_short_arg0 = 0;",
            "    { uint8_t st[64]; (void)pmu_read_reg(PMU_REG_STATUS, st, sizeof(st)); (void)st; }\n"
            "    uint16_t first_short_arg0 = 0;"),
        UNIT,
        "value_reg_reads == 0",
    ),
    (
        "V-D 取一即返不 ACK（咬 K4②）",
        pmu,
        lambda s: mutate_in_function(
            s, "read_pico_pmu_take_key_action",
            "        if (read_pico_pmu_event_ack(id) != ESP_OK) break;",
            "        break;   /* 不 ACK，事件留在队列里 */"),
        UNIT,
        "qn == 0",
    ),
    (
        "V-E 深度折 1（咬 K4②）",
        pmu,
        lambda s: mutate_in_function(
            s, "read_pico_pmu_take_key_action",
            "    for (int i = 0; i < PMU_EVENT_FIFO_DEPTH; i++) {",
            "    for (int i = 0; i < 1; i++) {"),
        UNIT,
        "qn == 0",
    ),
]


def main():
    failures = []

    for name, path, mutate, cmd, key in VARIANTS:
        original = path.read_text()
        before = digest(path)
        try:
            mutated = mutate(original)
            if mutated == original:
                failures.append(f"{name}: 变体没改到源码（锚点漂了）")
                continue
            path.write_text(mutated)
            rc, out = run(cmd)
            if rc == 0:
                failures.append(f"{name}: 宿主用例仍然绿——刀没有咬点")
                continue
            if key not in out:
                failures.append(f"{name}: 红了但不在该红的地方（缺 {key!r}）")
                continue
            print(f"KILLED（宿主红）: {name}")
        finally:
            path.write_text(original)
            assert digest(path) == before, f"{path} 未还原"

    if failures:
        print("FAIL: PMU bus host mutations")
        for line in failures:
            print("  -", line)
        return 1

    # 还原后必须仍然全绿——否则「红」可能来自别处的破坏。
    for name, cmd in (("主循环", HOST), ("取键单元", UNIT)):
        rc, out = run(cmd)
        if rc != 0:
            print(f"FAIL: 还原后 {name} 不绿")
            print(out[-2000:])
            return 1

    print(f"PASS: PMU bus host mutations ({len(VARIANTS)}/{len(VARIANTS)} killed, sources restored, both suites green)")
    return 0


sys.exit(main())
