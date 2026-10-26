#!/usr/bin/env python3
"""APK 释义帽 / 固件槽 / 字数约束 三者一致性守门。

gen26.6.4 起把「APK 帽 ≤ 固件槽 − 48」「字数×3 ≤ APK 帽」变成静态检查：
任何一方单独改动都会在此失败，防「帽>槽」这类跨端漂移。

Consistency gate for the APK def cap, the firmware slot and the char limit:
APK cap <= slot - 48, and char_limit * 3 <= cap. A one-sided edit fails here.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
AI = ROOT / "tools/android/apk/src/dev/readpico/push/AiClient.java"
PROTO = ROOT / "components/push_sink/include/push_proto.h"


def grab(pat, text, name):
    m = re.search(pat, text)
    if not m:
        print(f"FAIL: cannot parse {name}")
        sys.exit(1)
    return int(m.group(1))


def main():
    ai = AI.read_text(encoding="utf-8")
    proto = PROTO.read_text(encoding="utf-8")
    cap = grab(r"DEF_MAX_BYTES\s*=\s*(\d+)", ai, "DEF_MAX_BYTES")
    chars = grab(r"SYSTEM_CHAR_LIMIT\s*=\s*(\d+)", ai, "SYSTEM_CHAR_LIMIT")
    slot = grab(r"#define\s+PUSH_AI_DEF_MAX\s+(\d+)u", proto, "PUSH_AI_DEF_MAX")

    errs = []
    if cap > slot - 48:
        errs.append(f"APK cap {cap} > slot {slot} - 48")
    if chars * 3 > cap:
        errs.append(f"char limit {chars}*3={chars*3} > cap {cap}")
    if cap <= 0 or chars <= 0 or slot <= 0:
        errs.append("non-positive constant")

    print(f"cap={cap} slot={slot} chars={chars} "
          f"(cap<=slot-48: {cap <= slot - 48}; chars*3={chars*3}<=cap: {chars * 3 <= cap})")
    if errs:
        print("FAIL: " + "; ".join(errs))
        sys.exit(1)
    print("PASS: APK cap consistent with firmware slot and char limit")


if __name__ == "__main__":
    main()
