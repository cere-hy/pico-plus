#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
#
# 26.11.4「西文按词断行」黄金向量的先红验证（在仓库根目录运行）。
# First-red sweep for the word-wrap golden vector.
#
# 一条刀：把词边界回退整条关掉（`if (0 && …)`，等价于改动之前的硬切），黄金向量
# 必须当场判红——英文行尾出现「字母 + 下行字母」。若关掉它还是绿的，那绿的就不是
# 这条规则在起作用，而是判据根本没看见断词。
# One knife: disable the word-boundary rollback entirely (equivalent to the old hard
# cut). If the vector stays green, the green was never about this rule.
set -uo pipefail
cd "$(dirname "$0")/.."

FILE=main/book/book_layout.c
BAK=$(mktemp)
cp "$FILE" "$BAK"
trap 'cp "$BAK" "$FILE"; rm -f "$BAK"' EXIT

if ! python3 tools/book_wrap_golden.py >/dev/null 2>&1; then
    echo "BASELINE RED: the golden vector fails before any mutation"
    exit 1
fi

OLD='            if (word_char((unsigned char)s_text[end]) && end > visible_off &&'
NEW='            if (0 && word_char((unsigned char)s_text[end]) && end > visible_off &&'
OLD="$OLD" NEW="$NEW" FILE="$FILE" python3 - <<'PY' || { echo "TARGET MISSING: M1"; exit 1; }
import os, pathlib
p = pathlib.Path(os.environ['FILE'])
s = p.read_text(encoding='utf-8')
old, new = os.environ['OLD'], os.environ['NEW']
if s.count(old) != 1:
    raise SystemExit(3)
p.write_text(s.replace(old, new, 1), encoding='utf-8')
PY

if python3 tools/book_wrap_golden.py >/tmp/wrap_mut.txt 2>&1; then
    echo "SURVIVED: M1 word-boundary rollback disabled, the vector still passed"
    echo "  -> the criterion is not looking at what it claims to look at"
    exit 1
fi
grep -E "word split|content not conserved|changed" /tmp/wrap_mut.txt | head -4
echo "caught: M1 rollback disabled -> word splits reappear"
echo "wrap mutations: 1/1 caught"
