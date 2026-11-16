#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
"""
段账双申报牙（26.11.3-C2，门禁 16/16）。

背景：内部 RAM 的 bss 一直有一条硬顶（0x21600 = 136,704 B）和一条人肉记账的
「余量 112 B」。26.11.3-C2 把四块长缓冲搬进 PSRAM 之后，内部压力松了、PSRAM 开始
增长——而 PSRAM 在此之前**从没进过任何判据**。没人看的量迟早长成缺陷，所以本牙
只做一件事：把两个量都打出来，并对「内部余量」给一条下限。

为什么咬余量而不是咬具体值：具体值每批都在动，咬死就变成「每批都要改牙」；而
「还剩不到 4 KB」是一个与批次无关的危险信号——下一个静态缓冲会直接撞顶。

用法 / usage:
  python3 tools/check_segment_ledger.py build_261106/Read_Pico.elf
"""
import glob
import os
import subprocess
import sys

INTERNAL_BSS_CAP = 0x21600          # 硬顶：内部 DRAM bss 段上限 / the internal bss cap
MIN_SPARE = 4096                    # 下限余量：下一块静态缓冲至少能放 4 KB / floor for the next buffer


def _size_tool():
    """找 `xtensa-esp32s3-elf-size`：先看 PATH，再落到 IDF 的安装目录。

    为什么要有这一步：门禁脚本不 source `idf_env.sh`（它只跑宿主测试与静态牙），
    所以在门禁里 PATH 上没有交叉工具链——本牙第一次跑就是这么红的。写死绝对路径
    会在换工具链版本时静默失效，所以按 glue 一一探测，找不到就报错而不是猜。
    Resolve the cross size tool: PATH first, then the IDF install tree. The gate does
    not source idf_env.sh, so PATH has no cross toolchain - that is exactly how this
    tooth went red the first time. A hard-coded path rots on a toolchain bump, so probe
    and fail loudly instead of guessing.
    """
    from shutil import which
    found = which("xtensa-esp32s3-elf-size")
    if found:
        return found
    pattern = os.path.expanduser(
        "~/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp32s3-elf-size")
    hits = sorted(glob.glob(pattern))
    return hits[-1] if hits else "xtensa-esp32s3-elf-size"


def main(argv):
    if len(argv) != 2:
        print("usage: check_segment_ledger.py <Read_Pico.elf>")
        return 2
    elf = argv[1]
    try:
        out = subprocess.run([_size_tool(), "-A", elf],
                             capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        print("FAIL: cannot read segment table from %s (%s)" % (elf, exc))
        return 1

    segs = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0].startswith("."):
            try:
                segs[parts[0]] = int(parts[1])
            except ValueError:
                continue

    bss = segs.get(".dram0.bss")
    psram = segs.get(".ext_ram.bss")
    if bss is None or psram is None:
        print("FAIL: .dram0.bss or .ext_ram.bss is missing from the segment table - "
              "either the linker script changed or this is not a Read_Pico.elf")
        return 1

    spare = INTERNAL_BSS_CAP - bss
    print("internal bss: %d / cap %d / spare %d" % (bss, INTERNAL_BSS_CAP, spare))
    print("ext_ram.bss : %d (PSRAM static, ledgered from 26.11.3-C2 on)" % psram)
    if spare < MIN_SPARE:
        print("FAIL: internal bss spare is %d B (< %d) - the next static buffer will hit the "
              "cap on hardware, not on the host" % (spare, MIN_SPARE))
        return 1
    print("segment ledger: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
