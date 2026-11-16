#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# 26.9.10 生词本 G1-G4 变异扫描：逐条把四处改动「回退」，确认静态牙 + 宿主测试擒得住。
# Mutation sweep for the 26.9.10 vocabulary rules: revert each of the four changes and
# confirm the static teeth (and host tests) catch it. A survivor means a hole.
set -uo pipefail
cd "$(dirname "$0")/.."

VL=main/dict/vocab_log.c
AB=main/apps/app_book.c
DP=main/apps/dict_popup.c

if [ "${MUTATION_ALLOW_DIRTY:-0}" != "1" ] && { ! git diff --quiet || ! git diff --cached --quiet; }; then
    echo "REFUSE: working tree is dirty before the sweep (set MUTATION_ALLOW_DIRTY=1 for a dev loop)." >&2
    exit 2
fi

cp "$VL" /tmp/vl.26910.orig
cp "$AB" /tmp/ab.26910.orig
cp "$DP" /tmp/dp.26910.orig

restore() {
    cp /tmp/vl.26910.orig "$VL"
    cp /tmp/ab.26910.orig "$AB"
    cp /tmp/dp.26910.orig "$DP"
    cmp -s /tmp/vl.26910.orig "$VL" || { echo "FATAL: $VL not restored" >&2; exit 3; }
    cmp -s /tmp/ab.26910.orig "$AB" || { echo "FATAL: $AB not restored" >&2; exit 3; }
    cmp -s /tmp/dp.26910.orig "$DP" || { echo "FATAL: $DP not restored" >&2; exit 3; }
}
trap restore EXIT

run_mutation() {
    local label="$1"
    # 静态牙 + 宿主测试任一失败即「caught」。
    local caught=0
    python3 tools/check_26910_vocab.py >/tmp/g26910.txt 2>&1 || caught=1
    if [ "$caught" = "0" ]; then
        echo "MUTATION SURVIVED (BAD): $label"
    else
        echo "caught: $label"
    fi
    restore
}

echo "== baseline (must PASS) =="
python3 tools/check_26910_vocab.py || { echo "BASELINE FAIL"; exit 1; }

# V1 (G1)：把 queue 改回入批。
python3 - <<'EOF'
import pathlib
p = pathlib.Path('main/dict/vocab_log.c'); s = p.read_text()
old = "    (void)word;\n    (void)batch_ensure; // 保留机制符号；不调用、不分配 / keep the symbol, no call, no alloc\n}"
new = "    if (!s_autosave) return;\n    if (!word) return;\n    if (!batch_ensure()) return;\n    ++s_batch_count; /* MUTATION V1 */\n}"
assert old in s, 'V1 target not found'
p.write_text(s.replace(old, new, 1))
EOF
run_mutation "V1 G1: AI queue enqueues again"

# V2 (G2)：入口把 english 总闸加回去。
python3 - <<'EOF'
import pathlib
p = pathlib.Path('main/apps/app_book.c'); s = p.read_text()
old = "bool found = hit && dict_popup_open(word);"
new = "bool found = hit && english && dict_popup_open(word); /* MUTATION V2 */"
assert old in s, 'V2 target not found'
p.write_text(s.replace(old, new, 1))
EOF
run_mutation "V2 G2: entry re-gated by english"

# V3 (G3)：AI 落地改回 vocab_log_contains。
python3 - <<'EOF'
import pathlib
p = pathlib.Path('main/apps/dict_popup.c'); s = p.read_text()
old = "    if (ok) s_saved = false;"
new = "    if (ok) s_saved = vocab_log_contains(s_word); /* MUTATION V3 */"
assert old in s, 'V3 target not found'
p.write_text(s.replace(old, new, 1))
EOF
run_mutation "V3 G3: AI landing re-derives s_saved from contains"

# V4 (G4)：index_add 移回 append_row 之后（fclose 校验之前）。
python3 - <<'EOF'
import pathlib
p = pathlib.Path('main/dict/vocab_log.c'); s = p.read_text()
old = "        append_row(f, lower, s_batch[i].def, s_batch[i].sentence);"
new = "        append_row(f, lower, s_batch[i].def, s_batch[i].sentence);\n        index_add(lower); /* MUTATION V4 */"
assert old in s, 'V4 target not found'
p.write_text(s.replace(old, new, 1))
EOF
run_mutation "V4 G4: index_add moved before the fclose check"

echo "== sweep done =="
