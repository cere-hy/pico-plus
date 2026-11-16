#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
"""
门禁体检批 ③ —「接口新增三处清单」脚本化（§13.12 修法落地）。

四次失效的清单是：改一个组件 API，要记得同步
  ① 宿主桩（`tools/*_stubs/`）
  ② 头文件副本（桩目录里的同名 `.h`，通常是真实头的精简版）
  ③ runner 的编译单元清单（新 `.c` 要加进 gcc 行）

① 与 ③ 漏了会**编译不过**——疼，但会说。真正静默的是 ②：
桩头是「精简副本」，它只抄用到的部分，所以少了什么没人比较得出来。抄错一个值更糟：
宿主测试跑在一个**和固件不一样的常量**上，全绿也证明不了固件对。

本轮实例：31b 给 `pico_bootbb.h` 加 `PICO_BOOTBB_NOTE_PUSH_HOLD = 10`，同时要改
`tools/sd_probe_stubs/pico_bootbb.h` 的副本。改对了，靠的是记忆——本脚本让它不再靠记忆。

判据（只比对**两边都出现**的常量，精简是合法的，缺项不报）：
  桩头与真实头同名的 `NAME = 数字` / `#define NAME 数字`，值必须相等。
交集为空的副本不参与（那是纯声明壳，没有可漂移的东西）。

用法 / usage:
  python3 tools/check_api_surface.py           # 判定（0 = 通过）
  python3 tools/check_api_surface.py --list    # 打印每份副本的比对规模
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

failures = []


def bad(msg):
    failures.append(msg)


def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return "\n".join(line.split("//")[0] for line in src.splitlines())


# 枚举成员与整型宏。只认「名字 = 数字」这一种，字符串/地址不参与（桩与实头必然不同）。
CONST_PATTERNS = (
    re.compile(r"^\s*([A-Z][A-Z0-9_]{2,})\s*=\s*(\d+)\s*[,;]?\s*$", re.M),
    re.compile(r"^#define\s+([A-Z][A-Z0-9_]{2,})\s+(\d+)\b", re.M),
)


def constants(path):
    try:
        src = strip_comments(path.read_text(errors="replace"))
    except OSError:
        return {}
    out = {}
    for pat in CONST_PATTERNS:
        for name, val in pat.findall(src):
            out[name] = int(val)
    return out


def real_headers_by_name():
    """头文件名 → 真实路径列表（components/ 与 main/ 下）。"""
    index = {}
    for base in ("components", "main"):
        for p in (ROOT / base).rglob("*.h"):
            index.setdefault(p.name, []).append(p)
    return index


def main():
    real = real_headers_by_name()
    stubs = sorted(ROOT.glob("tools/*_stubs/*.h"))
    if not stubs:
        bad("③ no stub header found under tools/*_stubs/: either the host layer moved "
            "or this tooth is now blind - both need a human look")
        for f in failures:
            print("FAIL: " + f)
        return 1

    compared = 0
    pairs = 0
    for stub in stubs:
        candidates = real.get(stub.name)
        if not candidates:
            continue  # 纯宿主自造头（如 esp_timer.h 替身），没有真实对应，跳过
        sc = constants(stub)
        if not sc:
            continue
        # 同名多处真实头时全部比对：任一处不一致就算漂移（副本对着哪个都不能错）。
        for realp in candidates:
            rc = constants(realp)
            common = set(sc) & set(rc)
            if not common:
                continue
            pairs += 1
            compared += len(common)
            for name in sorted(common):
                if sc[name] != rc[name]:
                    bad("③ %s says %s = %d but %s says %d: the host test would run on a "
                        "different constant than the firmware - a green host result then "
                        "proves nothing about the device"
                        % (stub.relative_to(ROOT).as_posix(), name, sc[name],
                           realp.relative_to(ROOT).as_posix(), rc[name]))

    if pairs == 0:
        bad("③ not a single stub header shares a numeric constant with its real "
            "counterpart: the comparison had nothing to bite on, which must not read "
            "as 'no drift'")
    if "--list" in sys.argv:
        print("api surface: %d pair(s), %d constant(s) compared" % (pairs, compared))

    if failures:
        for f in failures:
            print("FAIL: " + f)
        print("\napi surface: %d failure(s)" % len(failures))
        return 1
    print("api surface: %d stub/real header pair(s), %d shared constant(s) agree - PASS"
          % (pairs, compared))
    return 0


if __name__ == "__main__":
    sys.exit(main())
