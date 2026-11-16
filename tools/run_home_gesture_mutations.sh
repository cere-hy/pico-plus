#!/bin/sh
# 首页手势批（26.11.17）：变异套件，6 刀（G6 是 26.11.17 收窄决定落码时补的第六刀）。
# 每把刀都打在判定行、视图门控、热区表或长按阈值上；任何一刀跑绿，说明真值表有洞。
# Mutations for the home-gesture batch. Each knife hits a decision line, the view gate,
# the hot-zone table or the long-press threshold; a survivor means the table has a hole.
set -eu

GSRC=main/apps/home_gesture.c
DASH=main/apps/app_dashboard.c
KIT=main/ui/ui_kit.h
BAK_G=$(mktemp)
BAK_D=$(mktemp)
BAK_K=$(mktemp)
cp "$GSRC" "$BAK_G"
cp "$DASH" "$BAK_D"
cp "$KIT" "$BAK_K"
restore() { cp "$BAK_G" "$GSRC"; cp "$BAK_D" "$DASH"; cp "$BAK_K" "$KIT"; }
cleanup() { restore; rm -f "$BAK_G" "$BAK_D" "$BAK_K"; }
trap cleanup EXIT

pass=0
fail=0

run_mutation() {
    name=$1
    file=$2
    old=$3
    new=$4
    python3 - "$file" "$old" "$new" <<'PY'
import sys
p, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(p, encoding='utf-8').read()
if old not in s:
    print("TARGET MISSING: %r" % old[:60])
    sys.exit(2)
open(p, 'w', encoding='utf-8').write(s.replace(old, new, 1))
PY
    if bash tools/run_app_loop_host_tests.sh >/tmp/hg_mut.log 2>&1; then
        echo "SURVIVED      $name"
        fail=$((fail + 1))
    elif grep -q "error:" /tmp/hg_mut.log; then
        # 刀没进考场：记 COMPILE FAIL，不当 caught（变异第四态）。
        echo "COMPILE FAIL  $name"
        grep -m2 "error:" /tmp/hg_mut.log | sed 's/^/    /'
        fail=$((fail + 1))
    elif grep -q "home_gesture_cases" /tmp/hg_mut.log; then
        echo "caught        $name"
        grep -m1 "home_gesture_cases" /tmp/hg_mut.log | sed 's/^/    /'
        pass=$((pass + 1))
    else
        # 红在别的断言上：不是这把刀咬的（纪律 12）。
        echo "SURVIVED-打错门  $name"
        grep -m1 "Assertion" /tmp/hg_mut.log | sed 's/^/    /'
        fail=$((fail + 1))
    fi
    restore
}

# G1 方向反转：上滑不再开页面（下滑反而开）。
run_mutation "G1 方向反转 steps<0 -> steps>0" "$GSRC" \
    "if (ev->steps >= 0) return false;" \
    "if (ev->steps <= 0) return false;"

# G2 摘视图门控：任何子视图的手势都会开页面。
run_mutation "G2 摘掉视图门控" "$GSRC" \
    "    if (view != HOME_VIEW_ROOT) return false;
    // 只认竖滑" \
    "    if (view < 0) return false;   // G2
    // 只认竖滑"

# G3 空白判定放行右卡：长按右卡也会开传书页。
run_mutation "G3 blank_hit 放行右卡" "$GSRC" \
    "    if (rect_hit(HOME_HIT_RIGHT, x, y)) return false;" \
    "    /* G3: right card no longer excluded */"

# G4 RETRACT 改为触发动作：双指一落定就开页面。
run_mutation "G4 RETRACT 改为触发" "$GSRC" \
    "        seq->fired = false;
        return false;" \
    "        seq->fired = true;
        return true;"

# G5 长按阈值改 1 ms：300 ms 的按住也会触发。
run_mutation "G5 长按阈值 500 -> 1 ms" "$KIT" \
    "#define UI_LONG_PRESS_MS 500" \
    "#define UI_LONG_PRESS_MS 1"

# G6 摘掉状态栏信息带排除：长按电池区会开传书页（收窄决定自己的刀）。
# 刀值选「整行删掉」而不是改数字——改 88 为 0 与删行语义相同，但删行更像
# 「实现回到越界的那一天」，正是这条边界要防的缺陷形态。
run_mutation "G6 摘掉状态栏信息带排除" "$GSRC" \
    "    if (rect_hit(HOME_RESERVED_INFO, x, y)) return false;" \
    "    /* G6: info band excluded, blank overruns to y=0 */"

echo
if [ "$fail" -eq 0 ]; then
    echo "JUDGEMENT: HOME_GESTURE MUTATIONS 6/6 caught"
else
    echo "JUDGEMENT: HOME_GESTURE MUTATIONS ${pass}/6 caught, $fail survived/misfired"
    exit 1
fi
