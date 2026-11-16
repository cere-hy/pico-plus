#!/usr/bin/env python3
"""26.11.19 解耦批：结构守卫的刀（确认守卫能红）。

这一组不是产品行为的变异测试，是**守卫自己的**变异测试：
每个变体把源码改回一种「下一次改动很可能真的会写成这样」的形态，
然后断言守卫**必须红**。守卫绿着跑完这一组 = 守卫是假的。

方案 §4.3 已分层：V-A–V-E 是宿主刀（红在宿主真值表），V-F 是**守卫刀**（红在守卫脚本）。
混排会让 V-F 在宿主里找不到红、被误记 SURVIVED——所以本文件只跑守卫刀，
别把宿主用例挂进来。

每个变体都 try/finally 还原源码；跑完比对文件哈希，任何残留都算失败。
"""

from pathlib import Path
import hashlib
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
loop = root / "main/app/app_loop.c"
pmu = root / "components/read_pico_pmu/read_pico_pmu.c"
guard = root / "tools/check_pmu_bus_guard.py"


def digest(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def run_guard():
    r = subprocess.run([sys.executable, str(guard)], capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


VARIANTS = [
    (
        "V-F 在 refresh_core 里加 ACK（咬 K2b / §5.3）",
        pmu,
        lambda s: s.replace(
            "    if (count != 0 && pmu_read_reg(PMU_REG_EVENT_PEEK, ev, sizeof(ev)) == ESP_OK) {\n        parse_event(ev);",
            "    if (count != 0 && pmu_read_reg(PMU_REG_EVENT_PEEK, ev, sizeof(ev)) == ESP_OK) {\n"
            "        parse_event(ev);\n        read_pico_pmu_event_ack(s_snap.event.event_id);",
            1),
        ["动点总数", "PEEK 升级为消费者的标志"],
    ),
    (
        "V-G 采样戳串写到按键戳（咬 §5.2）",
        loop,
        lambda s: s.replace(
            "if (pmu_bus_ok && pmu_bus_due(ctx.now_ms, last_sample_ms)) {",
            "if (pmu_bus_ok && pmu_bus_due(ctx.now_ms, last_key_ms)) {",
            1),
        ["采样侧 due 配对", "按键侧 due 配对"],
    ),
    (
        "V-H 门谓词就地展开（咬 §5.2 单源）",
        loop,
        lambda s: s.replace(
            "const bool pmu_bus_ok = pmu_bus_static_ok(pmu_ready_now, current->holds_pmu, usb_storage_active());",
            "const bool pmu_bus_ok = read_pico_pmu_ready() && !current->holds_pmu && !usb_storage_active();",
            1),
        ["手写展开的三条件门", "调用单源谓词"],
    ),
]


def main():
    before = {p: digest(p) for p in (loop, pmu, guard)}
    failures = []

    for name, path, mutate, expect_keys in VARIANTS:
        original = path.read_text()
        original_digest = digest(path)
        try:
            mutated = mutate(original)
            if mutated == original:
                failures.append(f"{name}: 变体没改到源码（锚点漂了，先修脚本）")
                continue
            path.write_text(mutated)
            rc, out = run_guard()
            if rc == 0:
                failures.append(f"{name}: 守卫仍然绿——这条守卫没有牙")
                continue
            missing = [k for k in expect_keys if k not in out]
            if missing:
                failures.append(f"{name}: 守卫红了但没红在该红的地方，缺 {missing}")
                continue
            print(f"KILLED（守卫红）: {name}")
        finally:
            path.write_text(original)
            assert digest(path) == original_digest, f"{path} 未还原"

    after = {p: digest(p) for p in (loop, pmu, guard)}
    for p in before:
        if before[p] != after[p]:
            failures.append(f"{p.name} 跑完后内容变了（还原失败）")

    if failures:
        print("FAIL: PMU bus guard mutations")
        for line in failures:
            print("  -", line)
        return 1

    print(f"PASS: PMU bus guard mutations ({len(VARIANTS)}/{len(VARIANTS)} killed, sources restored)")
    return 0


sys.exit(main())
