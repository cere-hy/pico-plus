#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
#
# 排版牙的**先红验证**（[123]，与刷新牙分开成套件——套件跟域走）。
#
# 每一刀都必须被**指定的那一个**检查擒住：门 1（运行时零差值）、结构牙、断词宿主
# 测试、表重生成校验，四者分工不同。刀与检查错配（拿门 1 去咬结构改动）会让
# 「被咬住」变成巧合，所以这里逐刀写死判据。
#
# 总闸（沿用 [122] 事故的制度）：跑完必须证明工作树与跑前逐字节相同。
# 出包四步锁序：变异 → 树身份 → 门禁 → 构建打包（`tools/release_gate.sh`）。
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

FILES=(
    main/font/ttf_font.h
    main/font/ttf_justify.h
    main/font/ttf_kern.h
    main/font/kern_pairs.h
    main/font/kern_pairs.c
    main/font/ttf_font.c
    main/book/book_layout.c
    main/book/hyphen.c
    main/book/hyph_en_us_data.c
    main/apps/ai_reader.c
    tools/vendor/hyph-en-us.tex
)

BACKUPS=()
snapshot() {
    BACKUPS=()
    for f in "${FILES[@]}"; do
        b=$(mktemp); cp "$f" "$b"; BACKUPS+=("$b")
    done
}
restore() {
    [ "${#BACKUPS[@]}" -eq 0 ] && return 0
    local i=0
    for f in "${FILES[@]}"; do cp "${BACKUPS[$i]}" "$f"; rm -f "${BACKUPS[$i]}"; i=$((i + 1)); done
    BACKUPS=()
}

TREE_BEFORE=$(python3 tools/tree_identity.py --quiet) || {
    echo "TREE IDENTITY UNAVAILABLE: 无法取得树身份，拒绝开跑（失败关闭）"; exit 2; }
TREE_CHECKED=0
tree_gate() {
    TREE_CHECKED=1
    if python3 tools/tree_identity.py --expect "$TREE_BEFORE" --quiet; then
        echo "tree gate: worktree byte-identical to pre-run ($TREE_BEFORE)"
        return 0
    fi
    echo "TREE GATE FAILED: 变异套件污染了工作树——此时任何构建产物都不可送验"
    FAILED=1
    return 1
}
on_exit() {
    local rc=$?
    restore
    if [ "$TREE_CHECKED" -eq 0 ]; then
        python3 tools/tree_identity.py --expect "$TREE_BEFORE" --quiet || rc=1
    fi
    exit "$rc"
}
trap on_exit EXIT
snapshot

FAILED=0
CAUGHT=0
EXPECTED=13  # 加一刀就改这里；少一刀脚本自己会红

mutate() {
    local name="$1" file="$2" old="$3" new="$4" rc=0
    local listed=""
    for f in "${FILES[@]}"; do [ "$f" = "$file" ] && listed=1; done
    if [ -z "$listed" ]; then
        echo "TARGET NOT IN FILES: $name ($file) - restore would not cover it"; FAILED=1; return 1
    fi
    OLD="$old" NEW="$new" FILE="$file" python3 - <<'EOF' || rc=$?
import os, pathlib
p = pathlib.Path(os.environ['FILE'])
s = p.read_text(encoding='utf-8')
old, new = os.environ['OLD'], os.environ['NEW']
n = s.count(old)
if n == 0:
    raise SystemExit(3)
if n > 1:
    raise SystemExit(4)
p.write_text(s.replace(old, new, 1), encoding='utf-8')
EOF
    if [ "$rc" -eq 3 ]; then echo "TARGET MISSING: $name"; FAILED=1; return 1
    elif [ "$rc" -eq 4 ]; then echo "TARGET AMBIGUOUS: $name"; FAILED=1; return 1
    elif [ "$rc" -ne 0 ]; then echo "MUTATE ERROR: $name ($rc)"; FAILED=1; return 1
    fi
    return 0
}

run_mutation() {
    local name="$1" desc="$2"; shift 2
    if "$@" >/dev/null 2>&1; then
        echo "SURVIVED: $name $desc"; FAILED=1
    else
        echo "caught: $name $desc"; CAUGHT=$((CAUGHT + 1))
    fi
    restore; snapshot
}

# ---- 基线：四个检查器全绿，才谈得上「被咬住」-------------------------------
for g in "python3 tools/check_typography.py" "python3 tools/check_hyphen_table.py" \
         "python3 tools/check_kern_table.py" "python3 tools/hyphen_host_test.py" \
         "python3 tools/typography_gate1.py"; do
    if ! $g >/dev/null 2>&1; then
        echo "BASELINE RED: $g fails before any mutation"
        exit 1
    fi
done
echo "baseline: 5/5 typography checks green"

# ---- R1：空格不再参与分配（英文两端对齐的死因）------------------------------
# 靶点在 ttf_justify.h：规则单源化之后 ttf_font.h 只剩一行 include，改这里才改得动行为。
mutate "R1" main/font/ttf_justify.h \
    "    if (cp == ' ' || cp == '\\t') return true;" \
    "    if (cp == '\\t') return true;" || true
run_mutation "R1" "word spaces no longer adjustable: English justification dies" \
    python3 tools/typography_gate1.py

# ---- R2：余量不再摊到各间隙（step*gaps 对不上目标）--------------------------
mutate "R2" main/font/ttf_justify.h \
    '    out->remainder = out->partial ? 0 : delta - per * out->gaps;' \
    '    out->remainder = 0;' || true
run_mutation "R2" "remainder dropped: the line no longer lands on the right edge" \
    python3 tools/typography_gate1.py

# ---- R3：断词不接线（长词又断在词中）---------------------------------------
mutate "R3" main/book/book_layout.c \
    'hyph_points(s_text + word_start, word_end - word_start, &pts)' \
    '0' || true
run_mutation "R3" "hyphenation unplugged from take_line" \
    python3 tools/check_typography.py

# ---- R4：引用条又出现自写判宽循环 -----------------------------------------
mutate "R4" main/apps/ai_reader.c \
    '    const size_t ws = s_session.quote_word_off;' \
    '    if (ui_text_fixed_width_px(px, "x") > avail) return;
    const size_t ws = s_session.quote_word_off;' || true
run_mutation "R4" "quote strip regrows its own width loop" \
    python3 tools/check_typography.py

# ---- R5：比较方向反转（算法错，表对）---------------------------------------
mutate "R5" main/book/hyphen.c \
    '    if (c) return c;' \
    '    if (c) return -c;' || true
run_mutation "R5" "binary search comparator inverted" \
    python3 tools/hyphen_host_test.py

# ---- R6：源文件被改（表不再是这一份）---------------------------------------
mutate "R6" tools/vendor/hyph-en-us.tex \
    '.ach4' \
    '.ach5' || true
run_mutation "R6" "TeX source edited: the table is no longer that table" \
    python3 tools/check_hyphen_table.py

# ---- R7：绘制推进多 1 px（度量与绘制分家）-----------------------------------
mutate "R7" main/font/ttf_font.c \
    '        if (glyph) cursor_x += glyph->advance_x;' \
    '        if (glyph) cursor_x += glyph->advance_x + 1;' || true
run_mutation "R7" "draw advances by +1 px: measure != draw" \
    python3 tools/typography_gate1.py

# ---- R8：natural 丢掉 tracking 项（用户一开字距预算就错）--------------------
mutate "R8" main/font/ttf_font.c \
    '    const int natural = measure_width(pixel_height, text) + tracking_px * (glyphs - 1);' \
    '    const int natural = measure_width(pixel_height, text);' || true
run_mutation "R8" "natural without tracking: budget wrong once tracking is on" \
    python3 tools/check_typography.py

# ---- R9：kern 只加在绘制侧（度量漏掉 → 行宽比画出来的窄）-------------------
mutate "R9" main/font/ttf_font.c \
    '        if (prev_cp != 0) width += ttf_kern_px(pixel_height, prev_cp, cp);' \
    '        if (0) width += 0;' || true
run_mutation "R9" "kern only on the draw side: measure drifts from the pen" \
    python3 tools/typography_gate1.py

# ---- R10：kern 根本没接上（函数永远返回 0，表在但没人用）--------------------
mutate "R10" main/font/ttf_font.c \
    '    if (!font_ready || left_cp == 0 || right_cp == 0) return 0;' \
    '    return 0;' || true
run_mutation "R10" "kern returns zero: the table is dead weight" \
    python3 tools/typography_gate1.py

# ---- R11：表里某个被锚定的对值改了（符号仍对，kern 照常发生）--------------
# 这一刀**只有正向锚能咬**：check_kern 只看符号与「整串==逐字和+kern」，
# 值从 -167 变 -300 它照样绿；而正向锚拿真实表算出的像素做精确断言，立刻红。
mutate "R11" main/font/kern_pairs.c \
    "-167," \
    "-300," || true
run_mutation "R11" "table value drifted: sign still right, pixels wrong" \
    python3 tools/typography_gate1.py

# ---- R12：字距开关被摘掉（经典态仍然有字距 → off 就不再等于 [122]）----------
mutate "R12" main/font/ttf_font.c \
'    if (!app_typography_enhanced()) return 0;
    if (!font_ready || left_cp == 0 || right_cp == 0) return 0;' \
'    if (0) return 0;
    if (!font_ready || left_cp == 0 || right_cp == 0) return 0;' || true
# 靶子是门 1 的经典态判据（它编译真 ttf_font.c，能看见真 kern）；switch 门走的是
# 宿主桩里的 kern，摘产品开关它看不见——这一刀的归属要写清楚，不然就是假咬。
run_mutation "R12" "kerning switch removed: classic mode still kerns" \
    env PICO_TYPO=0 python3 tools/typography_gate1.py

# ---- R13：断词开关被摘掉（经典态仍然断词 → off 指纹不再等于 [122]）----------
# 注意：justify 的经典分支不在这一刀里——它只影响绘制笔位、不影响分页指纹，
# switch 门看不见它，拿它当变异等于自欺。它由门 1 的「fitted lands on target」覆盖。
mutate "R13" main/book/book_layout.c \
'            if (app_typography_enhanced() && !s_vertical && !*heading &&' \
'            if (!s_vertical && !*heading &&' || true
run_mutation "R13" "hyphenation switch removed: classic mode still hyphenates" \
    python3 tools/check_typography_switch.py

echo
tree_gate
echo "typography mutations: ${CAUGHT}/${EXPECTED} caught"
[ "$CAUGHT" -ne "$EXPECTED" ] && FAILED=1
[ "$FAILED" -ne 0 ] && { echo "TYP0GRAPHY_MUTATIONS: FAIL"; exit 1; }
echo "TYPOGRAPHY_MUTATIONS: PASS"
