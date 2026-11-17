#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
上站前联网 OTA 清单校验 / Pre-publish OTA manifest validation.

复刻设备端 main/ota_release.c 的 pico_release_parse() 判据，防止「清单无效」这类
在线事故：字段缓冲上限、版本语法、URL 前缀白名单、size 范围、清单总长全部按
设备固件源码真相源校验，任何一条不过即 FAIL（exit 1）。

字段 cap 从 main/ota_online.h 的 pico_release_t 结构解析，board/layout/feed 前缀
从同文件宏解析——cap 或宏改了，本脚本自动跟随，不硬编码漂移。

用法 / Usage:
    python3 tools/check_ota_manifest.py [path/to/update.json] [repo_root]
无参数时校验仓库根的 update.json（待发布清单真相源）；repo_root 默认为本脚本上级目录。
"""
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = Path(sys.argv[2]) if len(sys.argv) > 2 else HERE.parent


def die(msg):
    print("FAIL " + msg)
    sys.exit(1)


def load_source_truth(repo_root):
    """从设备源码解析字段 cap、board/layout 宏、URL 白名单前缀。"""
    hdr = (repo_root / "main" / "ota_online.h").read_text(encoding="utf-8")

    # cap 只从 pico_release_t 结构体块内取，避免误吃 pico_update_status_t.message[96]
    m = re.search(r"typedef\s+struct\s*\{(.*?)\}\s*pico_release_t\s*;", hdr, re.S)
    if not m:
        die("未找到 pico_release_t 结构定义")
    caps = {name: int(num) for name, num in re.findall(r"(\w+)\[(\d+)\]", m.group(1))}
    need = {"version", "project", "board", "layout", "minimum_base_version",
            "url", "sha256", "notes"}
    if not need <= caps.keys():
        die("pico_release_t 字段 cap 解析不全: %s" % sorted(need - caps.keys()))

    def macro(name):
        mm = re.search(r'#define\s+%s\s+"([^"]+)"' % name, hdr)
        return mm.group(1) if mm else None

    board = macro("PICO_OTA_BOARD")
    layout = macro("PICO_OTA_LAYOUT")
    if not board or not layout:
        die("PICO_OTA_BOARD/LAYOUT 宏解析失败")

    # URL 白名单前缀真相源是 ota_release.c:pico_release_url_valid 的 prefixes[]，不是 feed 宏
    rel = (repo_root / "main" / "ota_release.c").read_text(encoding="utf-8")
    pm = re.search(r'prefixes\s*\[\]\s*=\s*\{(.*?)\}', rel, re.S)
    prefixes = re.findall(r'"([^"]+)"', pm.group(1)) if pm else []
    if not prefixes:
        die("pico_release_url_valid 的 prefixes[] 解析失败")
    return caps, board, layout, prefixes


def field_ok(j, key, cap):
    v = j.get(key)
    return isinstance(v, str) and v != "" and len(v.encode("utf-8")) < cap


def version_parts(s):
    """等价 ota_release.c:17 version_parts：X.Y.Z-picoYY.MM.DD。"""
    if not s or len(s) > 32:
        return False
    return bool(re.match(r"^\d{1,5}\.\d{1,5}\.\d{1,5}-pico\d{1,5}\.\d{1,5}\.\d{1,5}$", s)) \
        or bool(re.match(r"^\d{1,5}\.\d{1,5}\.\d{1,5}(-rc\d{1,5})?$", s))


def url_ok(url, prefixes):
    """等价 ota_release.c:58 pico_release_url_valid（多前缀白名单）。"""
    if not url or len(url) >= 256:
        return False
    name = None
    for pfx in prefixes:
        if url.startswith(pfx):
            name = url[len(pfx):]
            break
    if not name or ".." in name or "/" in name or "\\" in name or not name.endswith(".bin"):
        return False
    return all(c.isalnum() or c in "-_." for c in name) and len(name) > 4


def main(argv):
    repo_root = Path(argv[1]) if len(argv) > 1 else ROOT
    path = Path(argv[0]) if argv else repo_root / "update.json"
    if not path.exists():
        die("清单不存在: %s" % path)

    caps, board, layout, prefixes = load_source_truth(repo_root)
    raw = path.read_bytes()
    problems = []

    if len(raw) > 8192:
        problems.append("清单总长 %d > 8192" % len(raw))
    try:
        j = json.loads(raw.decode("utf-8"))
    except Exception as e:
        die("JSON 解析失败: %s" % e)

    # 重复键检测（等价 pico_release.c:94 的 JOBJET 去重）
    def hook(pairs):
        seen = set()
        for k, _ in pairs:
            if k in seen:
                problems.append("重复键: %s" % k)
            seen.add(k)
        return dict(pairs)
    try:
        json.loads(raw.decode("utf-8"), object_pairs_hook=hook)
    except Exception:
        pass

    schema = j.get("schema")
    if not isinstance(schema, (int, float)) or schema != 1:
        problems.append("schema 必须为数字 1")

    for key, cap in caps.items():
        if not field_ok(j, key, cap):
            n = len(j[key].encode("utf-8")) if isinstance(j.get(key), str) else "N/A"
            problems.append("字段 %s 缺失/空/超长: %s >= cap %d" % (key, n, cap))

    size = j.get("size")
    if not (isinstance(size, (int, float)) and 288 <= size <= 0x580000 and size == int(size)):
        problems.append("size 越界或非整数: %s" % size)

    if j.get("board") != board:
        problems.append("board 不符: %s != %s" % (j.get("board"), board))
    if j.get("layout") != layout:
        problems.append("layout 不符: %s != %s" % (j.get("layout"), layout))
    if not version_parts(j.get("version", "")):
        problems.append("version 语法非法: %s" % j.get("version"))
    if not version_parts(j.get("minimum_base_version", "")):
        problems.append("minimum_base_version 语法非法: %s" % j.get("minimum_base_version"))
    if not url_ok(j.get("url", ""), prefixes):
        problems.append("url 非法或不在白名单前缀: %s" % j.get("url"))
    sha = j.get("sha256", "")
    if not (isinstance(sha, str) and len(sha) == 64 and all(c in "0123456789abcdefABCDEF" for c in sha)):
        problems.append("sha256 非 64 位 hex")

    if problems:
        for p in problems:
            print("  - " + p)
        die("清单未通过设备解析器等价判据（%d 项）" % len(problems))

    print("OTA_MANIFEST_PASS %s" % path)
    print("  version=%s size=%d notes=%dB/%d bytes=%d prefix=%s" % (
        j["version"], j["size"], len(j["notes"].encode("utf-8")), caps["notes"],
        len(raw), prefixes[0]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
