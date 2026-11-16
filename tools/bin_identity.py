#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
"""bin 身份：一个 bin 的两套身份一起读出来。

为什么要有这个工具（2026-11-03 `[122]` 发现）：
    四个 bin（[121]、x0、[122]、重编 x0）的 `app_desc.version` **全是** `0.3.9-pico26.9.25`
    ——`PICO_PROJECT_VER` 从没设过，台账里的 `pico26.11.11` 只是文档串。
    也就是说，此前每一次「在场身份核查」只有哈希和包名可用，版本串一直是缺的。
    从 `[122]` 起正式化：

        **哈希 = 台账身份**（`sha256`，对文件）
        **`app_desc.version` = 在场身份**（esptool / 升级器读的那一个）

    两条都要记；报告里引用版本串必须引用**内嵌值**，文档串不算数字来源。

口径：ESP 应用镜像 = 24 B 镜像头 + 8 B 段头，`esp_app_desc_t` 紧接其后；
      以魔数 `0xABCD5432` 定位（不硬编偏移），字段序：
      magic(4) secure_version(4) reserv1(8) version(32) project_name(32)
      time(16) date(16) idf_ver(32) app_elf_sha256(32) reserv2(20)。

用法：
    python3 tools/bin_identity.py build/Read_Pico.bin
    python3 tools/bin_identity.py build/Read_Pico.bin --expect-version 0.3.9-pico26.11.12
    python3 tools/bin_identity.py build/Read_Pico.bin --json
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import struct
import sys

MAGIC = 0xABCD5432


def _cstr(b: bytes) -> str:
    return b.split(b"\0", 1)[0].decode("utf-8", "replace")


def parse(path: pathlib.Path) -> dict:
    raw = path.read_bytes()
    info = {
        "file": str(path),
        "sha256": hashlib.sha256(raw).hexdigest(),
        "size": len(raw),
        "app_desc_offset": None,
        "version": None,
        "project_name": None,
        "build_time": None,
        "build_date": None,
        "idf_ver": None,
    }
    off = raw.find(struct.pack("<I", MAGIC))
    if off < 0:
        return info
    info["app_desc_offset"] = off
    info["version"] = _cstr(raw[off + 16:off + 48])
    info["project_name"] = _cstr(raw[off + 48:off + 80])
    info["build_time"] = _cstr(raw[off + 80:off + 96]) or None
    info["build_date"] = _cstr(raw[off + 96:off + 112]) or None
    info["idf_ver"] = _cstr(raw[off + 112:off + 144]) or None
    return info


def main() -> int:
    ap = argparse.ArgumentParser(description="bin 的哈希身份 + 在场（内嵌）身份")
    ap.add_argument("bin", nargs="?", default="build/Read_Pico.bin")
    ap.add_argument("--expect-version", help="内嵌版本串必须等于它，否则 exit 1")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    p = pathlib.Path(args.bin)
    if not p.is_file():
        print("bin_identity: %s 不存在" % p, file=sys.stderr)
        return 2

    i = parse(p)
    if args.json:
        print(json.dumps(i, ensure_ascii=False))
    else:
        print("file      %s" % i["file"])
        print("sha256    %s" % i["sha256"])
        print("size      %d B" % i["size"])
        if i["app_desc_offset"] is None:
            print("app_desc  NOT FOUND (不是 ESP 应用镜像？)")
        else:
            print("version   %s   ← 在场身份（esptool / 升级器读这个）"
                  % (i["version"] or "(空)"))
            print("project   %s" % (i["project_name"] or "(空)"))
            print("built     %s %s" % (i["build_date"] or "-", i["build_time"] or "-"))
            print("idf       %s" % (i["idf_ver"] or "-"))
            print("desc@     0x%x" % i["app_desc_offset"])

    if args.expect_version is not None:
        if i["version"] != args.expect_version:
            print("VERSION MISMATCH: 内嵌 %r ≠ 期望 %r（文档串不算数，以 bin 内嵌为准）"
                  % (i["version"], args.expect_version), file=sys.stderr)
            return 1
        print("version ok: %s" % i["version"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
