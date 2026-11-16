#!/usr/bin/env python3
"""排版开关的双向等价断言（`[124]` 开关批）。

**断言什么**：同一份 `book_layout.c` 二进制，只改 `PICO_TYPO` 环境变量，分页指纹必须满足

  · off（`PICO_TYPO=0`）== **`[122]` 基线**（从 `3a2a405` 取的源码现编现跑）
  · on（`PICO_TYPO=1`）  == **当前树基线**（钉死字面量）
  · 默认（不设变量）     == on（默认就是增强，别让「没设」变成第三种态）

**为什么是双向**：只跑「off == 基线」证的是**退化正确**；还要证「on == 当前树」，
那才是**接线零污染**——防的是分支接线本身误伤 on 路径。两向都过，才说明开关是开关，
不是把一条路径拆成两条各带各的病。

**指纹怎么算**：`measure_line_whitespace.py --fingerprint` 把五档版心的每一行
（段末标记 + 文本 + 实宽）串起来取 sha256。它比「行数相等」硬——行数相同而断点不同，
指纹照样不同。
"""
from __future__ import annotations

import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
MEASURE = ROOT / "tools/measure_line_whitespace.py"

# 基线 a：`[122]` 无 justify / 无断词 / 无字距态（`[123]` 落码前的 book_layout.c）
BASELINE_REV = "3a2a405"

# 基线 b：当前树增强态的分页指纹。改 fixture、改字体、改任何一条排版规则都会让它变，
# 变了就重新钉一次并在提交信息里说明为什么变——否则它就成了「永远绿」的摆设。
EXPECT_ON = "a09a20f041b7db8898448eed3d90b021f08cf4165ddd8cf3bb4fad00d97be912"
EXPECT_OFF = "ce815c350b4d30df54c10452f6c3e944eb019a87f149d7262d3b19699fb85592"


def fingerprint(env_value: str | None, rev: str = "") -> str:
    env = {"PICO_TYPO": env_value} if env_value is not None else {}
    cmd = [sys.executable, str(MEASURE), "--fingerprint"]
    if rev:
        cmd += ["--rev", rev]
    import os
    full_env = dict(os.environ)
    full_env.pop("PICO_TYPO", None)
    full_env.update(env)
    out = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, env=full_env)
    if out.returncode != 0:
        print(out.stdout[-2000:])
        print(out.stderr[-2000:])
        raise SystemExit("switch gate: measure failed")
    m = re.search(r"^FINGERPRINT ([0-9a-f]{64})$", out.stdout, re.M)
    if not m:
        raise SystemExit("switch gate: no fingerprint in measure output")
    return m.group(1)


def main() -> int:
    fails = 0
    checks = 0

    off = fingerprint("0")
    on = fingerprint("1")
    default = fingerprint(None)
    base_a = fingerprint(None, rev=BASELINE_REV)

    # 基线 a 自己先要稳定：`[122]` 源码 + 当前桩，跑两次必须同指纹。
    base_a2 = fingerprint(None, rev=BASELINE_REV)

    def check(name: str, got: str, want: str) -> None:
        nonlocal fails, checks
        checks += 1
        if got != want:
            fails += 1
            print(f"FAIL  {name}\n      want {want}\n      got  {got}")
        else:
            print(f"ok    {name}  {got[:16]}")

    check(f"基线 [{BASELINE_REV}] 可复现（跑两次同指纹）", base_a2, base_a)
    check("off == [122] 基线（退化正确）", off, base_a)
    check("on == 当前树基线（接线零污染）", on, EXPECT_ON)
    check("默认 == on（默认增强，不是第三种态）", default, EXPECT_ON)
    # 往返：off 再回 on，指纹必须回到同一个值（防开关残留态）
    check("off → on 往返一致（无残留态）", fingerprint("1"), on)

    # 钉死值与现算值必须同源：EXPECT_OFF 只是 EXPECT_ON 之外的第二份记录，
    # 若 [122] 基线漂移（改了 fixture/字体），这里会红——那时该重钉，不是放宽。
    check("钉死的 off 基线 == 现算的 [122] 基线", base_a, EXPECT_OFF)

    print(f"\nswitch gate: {checks} checks, {fails} failures")
    if fails:
        print("SWITCH_GATE: FAIL")
        return 1
    print("SWITCH_GATE: PASS - off == [122] baseline, on == current tree, round-trip clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
