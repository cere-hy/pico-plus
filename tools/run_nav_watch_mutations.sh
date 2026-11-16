#!/bin/sh
# 状态栏电量差刷：变异套件（26.11.16 电量修复批）。
# 每把刀都打在「值变才刷」的判定行或去抖常数上；任何一刀跑绿，说明真值表有洞。
# Mutations for the battery-repaint gate. Each knife hits a decision line or the
# debounce constant; a survivor means the truth table has a hole.
set -eu

SRC=main/ui/nav_status_watch.c
HDR=main/ui/nav_status_watch.h
APP=main/app/app_loop.c
BAK=$(mktemp)
BAK_H=$(mktemp)
BAK_APP=$(mktemp)
cp "$SRC" "$BAK"
cp "$HDR" "$BAK_H"
cp "$APP" "$BAK_APP"
restore() { cp "$BAK" "$SRC"; cp "$BAK_H" "$HDR"; cp "$BAK_APP" "$APP"; }
cleanup() { restore; rm -f "$BAK" "$BAK_H" "$BAK_APP"; }
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
    if ! cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
        -Imain/ui tools/nav_status_watch_host_test.c "$SRC" -o /tmp/navwatch_mut 2>/tmp/navwatch_mut_cc.log; then
        echo "COMPILE FAIL  $name"
        tail -3 /tmp/navwatch_mut_cc.log
        fail=$((fail + 1))
        restore
        return
    fi
    if /tmp/navwatch_mut >/tmp/navwatch_mut.log 2>&1; then
        echo "SURVIVED      $name"
        fail=$((fail + 1))
    else
        echo "caught        $name"
        pass=$((pass + 1))
    fi
    restore
}

# M1 百分比比较失效：值变了也不刷。
run_mutation "M1 百分比比较恒假" "$SRC" \
    "if (percent != (int)w->shown_percent) {" \
    "if (0) {"

# M2 可见性守卫失效：看不见也刷（保留 visible 的读取，只让守卫恒假）。
run_mutation "M2 可见性守卫恒假" "$SRC" \
    "if (!visible) {" \
    "if (!visible && 0) {"

# M3 去抖常数归零：窗内第二次变化不再被吞。
run_mutation "M3 去抖常数 2000→1" "$HDR" \
    "#define NAV_WATCH_DEBOUNCE_MS 2000" \
    "#define NAV_WATCH_DEBOUNCE_MS 1"

# M4 挂起失效：charging 一变就刷，没有去抖。
run_mutation "M4 去掉挂起直接刷" "$SRC" \
    "    if (!w->pending) {
        w->pending = true;
        w->pending_since = now_ms;
        return NAV_WATCH_IDLE;
    }" \
    "    return NAV_WATCH_REPAINT;"

# M5 失败也落定（charging 路径）：REPAINT 的同时自作主张 commit，于是上屏失败
#    也不再重试。H 组咬这一刀——「落定权只在 commit」。
run_mutation "M5 charging 路径 REPAINT 即落定" "$SRC" \
    "    if ((uint32_t)(now_ms - w->pending_since) < NAV_WATCH_DEBOUNCE_MS) return NAV_WATCH_IDLE;
    return NAV_WATCH_REPAINT;" \
    "    if ((uint32_t)(now_ms - w->pending_since) < NAV_WATCH_DEBOUNCE_MS) return NAV_WATCH_IDLE;
    nav_watch_commit(w, percent, charging, minute);
    return NAV_WATCH_REPAINT;"

# M6 单向去抖：只给置位去抖、清除立即刷。这是「为响应速度优化」时最容易犯的那个错，
#    I 组咬它——拔一次线会闪两下。
run_mutation "M6 清除方向立即刷（单向去抖）" "$SRC" \
    "    if (!w->pending) {
        w->pending = true;" \
    "    if (!charging) return NAV_WATCH_REPAINT;
    if (!w->pending) {
        w->pending = true;"

# M7 失败也落定（百分比路径）：H2 组咬它。
run_mutation "M7 percent 路径 REPAINT 即落定" "$SRC" \
    "    if (percent != (int)w->shown_percent) {
        w->pending = false;
        return NAV_WATCH_REPAINT;
    }" \
    "    if (percent != (int)w->shown_percent) {
        nav_watch_commit(w, percent, charging, minute);
        return NAV_WATCH_REPAINT;
    }"

# M8 区域收窄回典型值（520 → 560）：判据 5 的常量守卫必须红。这一刀不编固件，
#    只跑宿主脚本里的算术断言——它砍的是「按当时测到的 85% 定框」这种思维。
python3 - "$APP" "#define NAV_BATTERY_X 520" "#define NAV_BATTERY_X 560" <<'PY'
import sys
p, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(p, encoding='utf-8').read()
if old not in s:
    print("TARGET MISSING: %r" % old[:60]); sys.exit(2)
open(p, 'w', encoding='utf-8').write(s.replace(old, new, 1))
PY
if sh tools/run_nav_watch_host_tests.sh >/tmp/navwatch_m8.log 2>&1; then
    echo "SURVIVED      M8 区域收窄 520→560"
    fail=$((fail + 1))
else
    echo "caught        M8 区域收窄 520→560"
    pass=$((pass + 1))
fi
restore

# M9 去掉调用点的 err 判断：无条件落定。模块测试仍全绿（它测模块契约），
#    咬住它的是源码守卫——这一刀专门证明「判据 6 在调用点也有牙」。
python3 - "$APP" \
    "if (err == EPD_DRAW_SUCCESS) nav_watch_commit(&s_nav_watch, percent, charging, minute);" \
    "nav_watch_commit(&s_nav_watch, percent, charging, minute);" <<'PY'
import sys
p, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(p, encoding='utf-8').read()
if old not in s:
    print("TARGET MISSING: %r" % old[:60]); sys.exit(2)
open(p, 'w', encoding='utf-8').write(s.replace(old, new, 1))
PY
if sh tools/run_nav_watch_host_tests.sh >/tmp/navwatch_m9.log 2>&1; then
    echo "SURVIVED      M9 调用点去掉 err 判断"
    fail=$((fail + 1))
else
    echo "caught        M9 调用点去掉 err 判断"
    pass=$((pass + 1))
fi
restore

# ---- 26.11.18 节拍批：分钟支四刀 ------------------------------------------------
# V1 摘掉分钟支（整段恒假）：J1 红——分钟前进不再触发整条刷。
run_mutation "V1 摘掉分钟支" "$SRC" \
    "    if (minute != NAV_WATCH_NO_MINUTE && minute != (int64_t)w->shown_minute) {" \
    "    if (0 && minute != (int64_t)w->shown_minute) {"

# V2 把 -1 哨兵**折叠成 0**（宏 + commit 两处，同一语义）：等于把「未校时」记成 1970 那一分钟。
#    J3 的「-1 → -1 永不刷」当场红；J8 走宏、跟着折叠，仍绿——所以这一刀只切中哨兵语义。
python3 - "$HDR" "$SRC" <<'PY'
import sys
h, c = sys.argv[1], sys.argv[2]
s = open(h, encoding='utf-8').read()
old = "#define NAV_WATCH_NO_MINUTE (-1)"
if old not in s: print("TARGET MISSING: sentinel"); sys.exit(2)
open(h, 'w', encoding='utf-8').write(s.replace(old, "#define NAV_WATCH_NO_MINUTE 0", 1))
s = open(c, encoding='utf-8').read()
old = "    w->shown_minute = (int32_t)minute;"
if old not in s: print("TARGET MISSING: commit"); sys.exit(2)
open(c, 'w', encoding='utf-8').write(s.replace(
    old, "    w->shown_minute = minute < 0 ? 0 : (int32_t)minute;", 1))
PY
if cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -Imain/ui tools/nav_status_watch_host_test.c "$SRC" -o /tmp/navwatch_mut 2>/tmp/navwatch_mut_cc.log; then
    if /tmp/navwatch_mut >/tmp/navwatch_mut.log 2>&1; then
        echo "SURVIVED      V2 哨兵折叠"
        fail=$((fail + 1))
    else
        echo "caught        V2 哨兵折叠"
        pass=$((pass + 1))
    fi
else
    echo "COMPILE FAIL  V2 哨兵折叠"
    tail -3 /tmp/navwatch_mut_cc.log
    fail=$((fail + 1))
fi
restore

# V3 分钟变也刷电量小矩形：J1 红（期望整条，得到小矩形）——矩形跟触发源走，不许“反正都是整条”。
run_mutation "V3 分钟变只刷小矩形" "$SRC" \
    "        w->pending = false;
        return NAV_WATCH_REPAINT_STRIP;" \
    "        w->pending = false;
        return NAV_WATCH_REPAINT;"

# V4 比较退成 int16：J5 的回绕组红（低 16 位相同 → 判“没变”）。
run_mutation "V4 分钟比较退成 int16" "$SRC" \
    "minute != (int64_t)w->shown_minute) {" \
    "(int16_t)minute != (int16_t)w->shown_minute) {"

echo "---"
echo "caught=$pass survived_or_broken=$fail"
[ "$fail" -eq 0 ] || exit 1
echo "JUDGEMENT: NAV_WATCH MUTATIONS 13/13 caught"
