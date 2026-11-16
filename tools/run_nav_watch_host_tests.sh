#!/bin/sh
# 状态栏电量差刷：真值表宿主用例 + 区域常量守卫（26.11.16 电量修复批）。
# 只编纯逻辑模块，不碰任何硬件桩；判据在这里，I/O 在 app_loop。
set -eu
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

# 判据 5 的牙：区域必须覆盖「最宽合法值」，不是当时测到的典型值。
# 520 = 三位数 `100%` 的左缘（22 px 字号实测量），653 = 电池笔帽右缘；
# 520+136 = 656，留 3 px 边距。审计稿的 {560,...} 就是把 100% 的百位切在框外的那种框：
# 99% 看着对，充到 100% 时左边多一个「1」。这条守卫拦的正是「按典型值收窄」。
# Constant guard: the rect must cover the widest legal value, not a typical one.
X=$(sed -n 's/^#define NAV_BATTERY_X \([0-9][0-9]*\)$/\1/p' main/app/app_loop.c)
W=$(sed -n 's/^#define NAV_BATTERY_W \([0-9][0-9]*\)$/\1/p' main/app/app_loop.c)
if [ -z "$X" ] || [ -z "$W" ]; then
    echo "FAIL 区域常量缺失（NAV_BATTERY_X / NAV_BATTERY_W 未在 app_loop.c 找到）"
    exit 1
fi
if [ "$X" -gt 527 ]; then
    echo "FAIL 区域左界 $X > 527：100% 的百位会掉在框外（清不掉也刷不到）"
    exit 1
fi
if [ $((X + W)) -le 653 ]; then
    echo "FAIL 区域右界 $((X + W)) <= 653：盖不住电池笔帽"
    exit 1
fi

# 判据 6 的调用点面：模块层证明了「不 commit 就重试」，但产品里真正的落定权在这行
# `if (err == EPD_DRAW_SUCCESS)`。删掉这个判断，模块测试仍会全绿（它测的是模块契约），
# 而产品会「上屏失败也当成功」——那种 bug 只能由源码守卫咬住。
# Call-site guard: the commit must stay behind the draw-result check.
if ! grep -Fq "if (err == EPD_DRAW_SUCCESS) nav_watch_commit(&s_nav_watch" main/app/app_loop.c; then
    echo "FAIL 判据 6 调用点失守：commit 未受 err 判断保护（上屏失败也会落定，不再重试）"
    exit 1
fi

# 26.11.18 节拍批三条结构守卫。
# (a) 手势领地必须是**派生**的：领地 ⊇ 条带由构造保证，不靠两处各自记得。
if ! grep -Fq '#define UI_STATUS_GESTURE_H (UI_STATUS_STRIP_H + 8)' main/ui/ui_nav_layout.h; then
    echo "FAIL 条带派生失守：UI_STATUS_GESTURE_H 不再是 UI_STATUS_STRIP_H + 8"
    exit 1
fi
# (b) 首页必须显式声明「自己有分钟节拍」：漏置 true，ROOT 上会出现双调度器
#     （整条 80 含时钟区 156×44）——两条节拍同拍各刷一次。
if ! grep -Eq '\.self_clock_tick=true' main/apps/app_dashboard.c; then
    echo "FAIL 首页 self_clock_tick 未置 true：ROOT 会双调度器重叠"
    exit 1
fi
# (c) 「整条」这个矩形只能有一个来源：阅读器与主循环都必须引用 UI_STATUS_STRIP_H，
#     不许再写字面 80（第二真相的第一形态就是一个字面量）。
if ! grep -Fq 'UI_LOCK_WIDTH,UI_STATUS_STRIP_H' main/apps/app_book.c; then
    echo "FAIL 阅读器整条矩形未引用 UI_STATUS_STRIP_H（可能又写回字面 80）"
    exit 1
fi
if ! grep -Fq '(EpdRect){0, 0, UI_LOCK_WIDTH, UI_STATUS_STRIP_H}' main/app/app_loop.c; then
    echo "FAIL 主循环整条矩形未引用 UI_STATUS_STRIP_H"
    exit 1
fi

cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -Imain/ui tools/nav_status_watch_host_test.c main/ui/nav_status_watch.c -o "$out/test"
"$out/test"
echo "JUDGEMENT: NAV_WATCH PASS (20 groups, rect ${X}+${W} covers 527..653, strip derived)"
