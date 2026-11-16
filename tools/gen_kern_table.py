#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
"""从 `builtin.ttf` 的 GPOS 展平出 kerning 表（[124]）。

中文：
  内置字体**没有 `kern` 表，但有 `GPOS`**：`kern` feature → lookup 1（Pair Adjustment），
  Format 2 是类表（50 左类 × 40 右类、覆盖 73 个字形），运行期解析它要 ~500 行 + 内存。
  这里离线展平成一张「字形对 → 调整值」的静态表：**算法能跑、表不能猜**——表跟字体走，
  字体换了表就作废，所以把字体 sha256 钉进生成头，`check_kern_table.py` 逐字节比对。

  单位是字体设计单位（upem=1000），运行期按 `stbtt_ScaleForPixelHeight` 的同一把尺换算，
  与 `advance` 的取整路径完全相同——度量与绘制必须同源，否则门 1 立刻红。

  键 = (left_gid << 11) | right_gid（内置字体 1683 字形 < 2048，11 bit 够），升序排列，
  运行期二分。值 int16（最小 -167，int8 装不下）。

English:
  Flatten the builtin face's GPOS `kern` lookup (Pair Adjustment, Format 2) into a static
  pair table. Keys pack both glyph ids into 32 bits and are sorted for binary search; values
  are font units (upem 1000). The face's sha256 is pinned in the generated header so a face
  swap invalidates the table loudly instead of silently kerning by the wrong glyph ids.

用法 / usage:
    python3 tools/gen_kern_table.py            # 重新生成 main/font/kern_pairs.[ch]
    python3 tools/gen_kern_table.py --stdout   # 打到标准输出（校验脚本用）
"""
from __future__ import annotations

import argparse
import hashlib
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
FONT = ROOT / "main/assets/builtin.ttf"
OUT_C = ROOT / "main/font/kern_pairs.c"
OUT_H = ROOT / "main/font/kern_pairs.h"

GID_BITS = 11
GID_MASK = (1 << GID_BITS) - 1


def flatten(font_path: pathlib.Path) -> tuple[dict[tuple[str, str], int], int]:
    from fontTools.ttLib import TTFont

    f = TTFont(str(font_path))
    upem = int(f["head"].unitsPerEm)
    gpos = f["GPOS"].table
    # kern feature → lookup indices
    lookups: list[int] = []
    for rec in gpos.FeatureList.FeatureRecord:
        if rec.FeatureTag == "kern":
            lookups.extend(rec.Feature.LookupListIndex)
    if not lookups:
        raise SystemExit("gen_kern_table: 字体没有 kern feature / no kern feature in GPOS")

    pairs: dict[tuple[str, str], int] = {}
    for li in sorted(set(lookups)):
        lookup = gpos.LookupList.Lookup[li]
        if lookup.LookupType != 2:
            raise SystemExit("gen_kern_table: kern lookup 不是 Pair Adjustment（type 2）")
        for st in lookup.SubTable:
            if st.Format == 1:
                g1 = st.Coverage.glyphs
                for i, left in enumerate(g1):
                    for rec in st.PairSet[i].PairValueRecord:
                        v = rec.Value1.XAdvance if rec.Value1 else 0
                        if v:
                            pairs[(left, rec.SecondGlyph)] = v
            elif st.Format == 2:
                from collections import defaultdict

                by_class: dict[int, list[str]] = defaultdict(list)
                for name, cls in st.ClassDef2.classDefs.items():
                    by_class[cls].append(name)
                for left in st.Coverage.glyphs:
                    lc = st.ClassDef1.classDefs.get(left, 0)
                    record = st.Class1Record[lc]
                    for cls, rights in by_class.items():
                        if cls == 0:
                            continue
                        v = record.Class2Record[cls].Value1.XAdvance
                        if v:
                            for right in rights:
                                pairs[(left, right)] = v
            else:
                raise SystemExit("gen_kern_table: 未知子表格式 %d" % st.Format)
    return pairs, upem


def render(font_path: pathlib.Path) -> tuple[str, str]:
    from fontTools.ttLib import TTFont

    pairs, upem = flatten(font_path)
    f = TTFont(str(font_path))
    order = f.getGlyphOrder()
    gid = {name: i for i, name in enumerate(order)}
    sha = hashlib.sha256(font_path.read_bytes()).hexdigest()

    rows = sorted(((gid[l], gid[r], v) for (l, r), v in pairs.items()))
    for lg, rg, _ in rows:
        if lg > GID_MASK or rg > GID_MASK:
            raise SystemExit("gen_kern_table: 字形 id 超过 %d bit，键会撞" % GID_BITS)
    keys = [(lg << GID_BITS) | rg for lg, rg, _ in rows]
    vals = [v for _, _, v in rows]

    h = f"""/* 生成文件，勿手改 / GENERATED FILE - DO NOT EDIT.
 * 生成器 / generator: tools/gen_kern_table.py
 * 源字体 / source: main/assets/builtin.ttf
 * 字体 sha256 / font sha256: {sha}
 * 对数 / pairs: {len(rows)}    upem: {upem}    key: (left << {GID_BITS}) | right
 *
 * 键升序、运行期二分；值是**字体设计单位**，换算成像素必须与 advance 用同一把尺
 * （门 1 咬的就是这条）。字体一换，本文件的键全部失效——sha256 钉在这里就是为了让
 * `check_kern_table.py` 当场红，而不是悄悄按错的字形对去调字距。
 */
#pragma once

#include <stdint.h>

#define KERN_PAIR_COUNT {len(rows)}
#define KERN_GID_BITS {GID_BITS}
#define KERN_UNITS_PER_EM {upem}
#define KERN_FONT_SHA256 "{sha}"

extern const uint32_t kern_pair_keys[KERN_PAIR_COUNT];
extern const int16_t kern_pair_values[KERN_PAIR_COUNT];
"""

    def chunks(seq, n):
        for i in range(0, len(seq), n):
            yield seq[i:i + n]

    body = [f'/* 生成文件，勿手改。见 kern_pairs.h 头部的来源与 sha256。 */',
            '#include "kern_pairs.h"', '',
            f'const uint32_t kern_pair_keys[KERN_PAIR_COUNT] = {{']
    for ch in chunks(keys, 8):
        body.append("    " + ", ".join("0x%08xu" % k for k in ch) + ",")
    body.append("};")
    body.append('')
    body.append(f'const int16_t kern_pair_values[KERN_PAIR_COUNT] = {{')
    for ch in chunks(vals, 12):
        body.append("    " + ", ".join("%d" % v for v in ch) + ",")
    body.append("};")
    body.append('')
    return h, "\n".join(body)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--stdout", action="store_true")
    args = ap.parse_args()
    if not FONT.exists():
        print("gen_kern_table: 找不到 %s" % FONT, file=sys.stderr)
        return 2
    header, source = render(FONT)
    if args.stdout:
        sys.stdout.write(header + "\n=====\n" + source)
        return 0
    OUT_H.write_text(header, encoding="utf-8")
    OUT_C.write_text(source, encoding="utf-8")
    pairs = header.split("pairs: ")[1].split()[0]
    print("kern-table: %s pairs -> %s, %s (%d B / %d B)"
          % (pairs, OUT_H.name, OUT_C.name, len(header.encode()), len(source.encode())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
