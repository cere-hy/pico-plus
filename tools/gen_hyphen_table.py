#!/usr/bin/env python3
"""中文：从 TeX `hyph-en-us` 的母本生成连字符模式表（Liang 算法，Apache-2.0 实现）。
English: Generate the hyphenation pattern table from the TeX `hyph-en-us` source.

来源链（可复现，不只写在注释里）：
  tools/vendor/hyph-en-us.tex 是 Knuth 的 `hyphen.tex`（Plain TeX hyphenation
  tables，lg_english 一组），hyph-utf8 的 `hyph-en-us.tex` 即由它派生（同一批
  4,447 条模式与 14 条例外，TeX 官方声明「NOT TO BE CHANGED IN ANY WAY」）。
  本机无外网（CTAN / GitHub 均不可达），取母本并把 sha256 记进生成物表头；
  `tools/check_hyphen_table.py` 每次重跑本脚本并**逐字节**比对，表被手改即红。

  **算法能抄，表不能抄**：Liang 的断点算法是公开的（这里自己写，不引第三方库），
  模式表必须是 TeX 那一份。所以生成物里同时钉住「源文件 sha256 + 模式条数 +
  例外条数」，任何一项对不上都说明表不是从这份源生成的。

输出：main/book/hyph_en_us_data.h（纯数据，供 main/book/hyphen.c 消费）。
编码：模式按字母序排序后，字母拼进一块 blob（不带分隔符），偏移表用 count+1 项
（末项 = blob 长度，于是长度可差分得到，省 4 KB）；数字按 4 bit/位打包进 uint64
（最长模式 8 字母 → 9 个位点 ≤ 36 bit）。
"""
from __future__ import annotations

import hashlib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "tools/vendor/hyph-en-us.tex"
OUT = ROOT / "main/book/hyph_en_us_data.h"
OUT_C = ROOT / "main/book/hyph_en_us_data.c"

# TeX 默认的左右最小切长（英语）。
LEFT_MIN = 2
RIGHT_MIN = 3


def strip_comments(text: str) -> str:
    """去掉 TeX 注释：未转义的 % 到行尾。"""
    out = []
    for line in text.splitlines():
        i = 0
        cut = len(line)
        while i < len(line):
            if line[i] == "\\":
                i += 2
                continue
            if line[i] == "%":
                cut = i
                break
            i += 1
        out.append(line[:cut])
    return "\n".join(out)


def parse_patterns(text: str) -> list[str]:
    m = re.search(r"\\patterns\{(.*?)\}", text, re.S)
    if not m:
        raise SystemExit("no \\patterns{} block")
    return m.group(1).split()


def parse_exceptions(text: str) -> list[str]:
    m = re.search(r"\\hyphenation\{(.*?)\}", text, re.S)
    if not m:
        return []
    return m.group(1).split()


def letters_and_digits(pattern: str) -> tuple[str, list[int]]:
    """把 'hy3phen' 拆成 ('hyphen', [0,0,3,0,0,0,0])。"""
    letters: list[str] = []
    digits = [0]
    for ch in pattern:
        if ch.isdigit():
            digits[-1] = int(ch)
        else:
            letters.append(ch)
            digits.append(0)
    return "".join(letters), digits


def pack_digits(digits: list[int]) -> int:
    v = 0
    for i, d in enumerate(digits):
        v |= (d & 0xF) << (4 * i)
    return v


def c_string(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main() -> int:
    raw = SRC.read_text(encoding="utf-8")
    digest = hashlib.sha256(raw.encode("utf-8")).hexdigest()
    text = strip_comments(raw)
    patterns = parse_patterns(text)
    exceptions = parse_exceptions(text)

    entries = []
    for p in patterns:
        letters, digits = letters_and_digits(p)
        if not letters or not letters.isascii() or not all(c.isalpha() or c == "." for c in letters):
            raise SystemExit(f"bad pattern {p!r}")
        entries.append((letters.lower(), digits))
    entries.sort(key=lambda e: e[0])

    # 重复模式（同字母不同数字）在 TeX 表里不存在，真有就并成最大数字——静默去重会
    # 让「表被换掉」查不出来，所以这里直接判失败。
    seen: dict[str, list[int]] = {}
    for letters, digits in entries:
        if letters in seen:
            raise SystemExit(f"duplicate pattern {letters!r}")
        seen[letters] = digits

    max_len = max(len(e[0]) for e in entries)
    if max_len > 15:
        raise SystemExit("pattern too long for the nibble packing")

    blob = "".join(e[0] for e in entries)
    offs = [0]
    for letters, _ in entries:
        offs.append(offs[-1] + len(letters))

    # 例外词：'ta-ble' → 词 + 允许断点集合（按字母下标，1 起算的词内位置）。
    exc_words = []
    exc_masks = []
    for w in exceptions:
        parts = w.split("-")
        word = "".join(parts)
        if not word.isascii() or not word.isalpha():
            raise SystemExit(f"bad exception {w!r}")
        pos, mask = 0, 0
        for k, part in enumerate(parts[:-1]):
            pos += len(part)
            if pos >= LEFT_MIN and len(word) - pos >= RIGHT_MIN:
                mask |= 1 << (pos - 1)
        exc_words.append(word.lower())
        exc_masks.append(mask)

    lines: list[str] = []
    a = lines.append
    a("/* 生成文件，勿手改 / GENERATED FILE - DO NOT EDIT.")
    a(" * 生成器 / generator: tools/gen_hyphen_table.py")
    a(" * 源 / source: tools/vendor/hyph-en-us.tex (Knuth hyphen.tex, lg_english)")
    a(f" * 源 sha256 / source sha256: {digest}")
    a(f" * 模式 / patterns: {len(entries)}   例外 / exceptions: {len(exc_words)}")
    a(f" * 最小切长 / left/right min: {LEFT_MIN}/{RIGHT_MIN}")
    a(" * 重新生成 / regenerate: python3 tools/gen_hyphen_table.py")
    a(" *")
    a(" * 为什么把 sha256 与条数钉在文件里：表是抄来的，算法是写的。表被替换、模式被")
    a(" * 增删、源文件被改，三个数字里必有一个对不上，check_hyphen_table.py 当场红。")
    a(" */")
    a("#pragma once")
    a("")
    a("#include <stdint.h>")
    a("")
    a(f'#define HYPH_EN_US_SOURCE_SHA256 "{digest}"')
    a(f"#define HYPH_EN_US_PATTERN_COUNT {len(entries)}")
    a(f"#define HYPH_EN_US_EXCEPTION_COUNT {len(exc_words)}")
    a(f"#define HYPH_EN_US_MAX_PATTERN {max_len}")
    a(f"#define HYPH_EN_US_LEFT_MIN {LEFT_MIN}")
    a(f"#define HYPH_EN_US_RIGHT_MIN {RIGHT_MIN}")
    a("")
    a(f"extern const char hyph_en_us_blob[{len(blob) + 1}];")
    a(f"extern const uint32_t hyph_en_us_off[{len(offs)}];")
    a(f"extern const uint32_t hyph_en_us_dig[{len(entries)}];")
    a(f"extern const char hyph_en_us_exc_blob[{sum(len(w) for w in exc_words) + 1}];")
    a(f"extern const uint32_t hyph_en_us_exc_off[{len(exc_words) + 1}];")
    a(f"extern const uint32_t hyph_en_us_exc_mask[{len(exc_words)}];")
    a("")

    # blob 按 32 字符折行，读起来不至于一条 30 KB 的长行。
    header = "\n".join(lines) + "\n"
    OUT.write_text(header, encoding="utf-8")

    c_lines: list[str] = []
    c = c_lines.append
    c("/* 生成文件，勿手改 / GENERATED FILE - DO NOT EDIT. 生成器 / generator: tools/gen_hyphen_table.py */")
    c('#include "hyph_en_us_data.h"')
    c("")
    c(f"const char hyph_en_us_blob[{len(blob) + 1}] =")
    for i in range(0, len(blob), 96):
        c('    "' + blob[i:i + 96] + '"')
    c("    ;")
    c("")
    c(f"const uint32_t hyph_en_us_off[{len(offs)}] = {{")
    for i in range(0, len(offs), 12):
        c("    " + ", ".join(str(v) for v in offs[i:i + 12]) + ",")
    c("};")
    c("")
    c(f"const uint32_t hyph_en_us_dig[{len(entries)}] = {{")
    for i in range(0, len(entries), 6):
        c("    " + ", ".join(f"0x{pack_digits(d):08x}u" for _, d in entries[i:i + 6]) + ",")
    c("};")
    c("")

    exc_blob = "".join(exc_words)
    exc_offs = [0]
    for w in exc_words:
        exc_offs.append(exc_offs[-1] + len(w))
    c(f"const char hyph_en_us_exc_blob[{len(exc_blob) + 1}] =")
    for i in range(0, len(exc_blob), 96):
        c('    "' + exc_blob[i:i + 96] + '"')
    c("    ;")
    c("")
    c(f"const uint32_t hyph_en_us_exc_off[{len(exc_offs)}] = {{")
    for i in range(0, len(exc_offs), 12):
        c("    " + ", ".join(str(v) for v in exc_offs[i:i + 12]) + ",")
    c("};")
    c("")
    c(f"const uint32_t hyph_en_us_exc_mask[{len(exc_masks)}] = {{")
    for i in range(0, len(exc_masks), 8):
        c("    " + ", ".join(f"0x{v:08x}u" for v in exc_masks[i:i + 8]) + ",")
    c("};")
    c("")

    OUT_C.write_text("\n".join(c_lines) + "\n", encoding="utf-8")
    print(f"wrote {OUT.relative_to(ROOT)} + {OUT_C.relative_to(ROOT)}: {len(entries)} patterns, {len(exc_words)} exceptions, "
          f"blob {len(blob)} B, table ~{(len(offs) * 4 + len(entries) * 4) / 1024:.1f} KiB, "
          f"max pattern {max_len}, source sha256 {digest[:16]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
