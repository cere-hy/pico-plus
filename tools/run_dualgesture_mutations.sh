#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# 中文：双指识别器（v2：抬手定轴）的变异验证：逐个植入变异，确认宿主测试/静态门禁
# 擒得住（exit 1 = 擒住）。任何存活变异都说明断言有洞。
# M12 的靶子在 app_loop.c，用 wiring 的 P10 静态门禁来擒。
# English: Mutation check for the two-finger recognizer (v2: verdict on lift).
# M12 targets app_loop.c and is caught by the wiring P10 static gate.
#
# 靶点漂移保护：每次替换都 assert 目标串存在；找不到直接判失败（TARGET MISSING），
# 绝不静默跳过——否则重构后会出现「变异根本没植入、测试照过 → 假 SURVIVED」。
# 这一条是 v2 重构时踩出来的（M1/M2/M8/M9/M10 的旧靶点在重构后全部消失）。
# Target-drift guard: every replacement asserts its anchor exists; a miss fails the
# sweep instead of silently skipping (which would fake a survivor after a refactor).
# Learned the hard way in v2: five old anchors vanished in the rewrite.
set -uo pipefail
cd "$(dirname "$0")/.."

TARGET=main/ui/ui_dualgesture.c
# 26.9.6：有效性判据住在头文件（唯一真相源），M19/M20 的靶子在里面。
# 26.9.6: the validity predicate lives in the header (single source of truth), which is
# where M19/M20 aim.
HEADER_TARGET=main/ui/ui_dualgesture.h
LOOP_TARGET=main/app/app_loop.c
BACKUP=$(mktemp)
HEADER_BACKUP=$(mktemp)
LOOP_BACKUP=$(mktemp)
cp "$TARGET" "$BACKUP"
cp "$HEADER_TARGET" "$HEADER_BACKUP"
cp "$LOOP_TARGET" "$LOOP_BACKUP"
trap 'cp "$BACKUP" "$TARGET"; cp "$HEADER_BACKUP" "$HEADER_TARGET"; cp "$LOOP_BACKUP" "$LOOP_TARGET"; rm -f "$BACKUP" "$HEADER_BACKUP" "$LOOP_BACKUP"' EXIT

FAILED=0

build_run() {
    # 26.9.15：`build/` 被 idf.py 清过之后这里不存在，`cc -o` 会失败而 `2>/dev/null` 把
    # 原因吞掉，基线表现为「未变异也红」——查了半天才知道是目录没建。自己建。
    # 26.9.15: after an `idf.py` clean, `build/` may not exist; `cc -o` then fails and
    # `2>/dev/null` swallows why, so the baseline reads RED on unmutated code. Create it.
    mkdir -p build/book-tests
    cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
       -Itools/ui_dualgesture_stubs -Imain/ui \
       -o build/book-tests/ui_dualgesture_mut \
       tools/ui_dualgesture_host_test.c "$TARGET" 2>/dev/null || return 1
    ./build/book-tests/ui_dualgesture_mut >/dev/null 2>&1
}

# 植入：找不到靶点 = 失败（不静默跳过）。
# Plant: a missing anchor is a failure (never skip silently).
mutate() {
    local name="$1" file="$2" old="$3" new="$4" rc=0
    OLD="$old" NEW="$new" FILE="$file" python3 - <<'EOF' || rc=$?
import os, pathlib
p = pathlib.Path(os.environ['FILE'])
s = p.read_text()
old, new = os.environ['OLD'], os.environ['NEW']
n = s.count(old)
if n == 0:
    raise SystemExit(3)   # 锚点不存在 / anchor absent
if n > 1:
    raise SystemExit(4)   # 锚点不唯一 / anchor ambiguous
p.write_text(s.replace(old, new, 1))
EOF
    # 26.9.7：锚点必须**唯一**。以前只判「在不在」，`replace(...,1)` 会静默改第一处——
    # M23 就是这样活下来的：`out->end = end_kind(t);` 全文三处，裸锚点打中融合窗中止，
    # 判定那处原样活着，变异「被擒」却是假的。现在 count!=1 直接红，全量靶点一次性受检。
    # 26.9.7: the anchor must be UNIQUE. Previously we only asked "present", and
    # `replace(...,1)` silently hit the first site - which is exactly how M23 survived:
    # `out->end = end_kind(t);` appears three times, the bare anchor hit the fusion-slip
    # site, and the verdict site stayed intact, so a fake "caught". Now count!=1 is red,
    # and every anchor in the sweep is checked at once.
    if [ "$rc" -eq 3 ]; then
        echo "TARGET MISSING: $name"
        FAILED=1
        return
    elif [ "$rc" -eq 4 ]; then
        echo "AMBIGUOUS ANCHOR: $name (matches >1 site; a bare anchor would mutate the wrong one)"
        FAILED=1
        return
    elif [ "$rc" -ne 0 ]; then
        echo "MUTATE ERROR($rc): $name"
        FAILED=1
        return
    fi
}

run_mutation() {
    local name="$1"
    if build_run; then
        echo "MUTATION SURVIVED: $name"
        FAILED=1
    else
        echo "caught: $name"
    fi
    cp "$BACKUP" "$TARGET"
}

restore_loop() { cp "$LOOP_BACKUP" "$LOOP_TARGET"; }

# ---- 基线必绿（26.9.7 立，本轮踩出来的假绿）----
# 变异全「被擒」也可能是因为**基线本来就红**——那是最坏的假绿：门禁全线报绿，实际
# 一条断言都没在测。本轮实例：⑦-6 用稀疏喂入测速度门，先撞上「刷新期丢弃」（gap），
# 于是 24/24 全灭，而基线自己就是红的。任何变异套件都必须先证明基线。
# All-killed can also mean the baseline itself is red - the worst kind of fake green:
# every gate reports success while nothing is actually being tested. This round's
# instance: case 7-6 fed sparsely and hit the refresh-gap abort instead of the speed
# gate, so 24/24 "died" over a red baseline. Every mutation sweep must prove its baseline.
if ! build_run; then
    echo "BASELINE RED: the unmutated ui_dualgesture host test fails; every 'caught' below would be fake"
    exit 1
fi
if ! sh tools/run_app_loop_host_tests.sh >/dev/null 2>&1; then
    echo "BASELINE RED: the unmutated app_loop suite fails; M22 would be fake"
    exit 1
fi
echo "ok: baseline green (ui_dualgesture host + app_loop suite)"

# ---- M1：横滑被静默改写（横滑必须仍是切屏）----
# 26.9.7 锚点唯一化：`out->type = UI_DUAL_EVT_ORIENT;` 全文两处（强轴横支 380 /
# fallback 横支 407）。裸锚点会静默改第一处（强轴）——碰巧是测试用的那支，但依赖代码
# 顺序。带其后独有的 `} else if ay * AXIS_RATIO`（只有强轴横支后面是它）唯一化。
# 26.9.7 unique anchor: the ORIENT line appears twice (strong-h 380 / fallback-h 407); the
# bare anchor silently hit the first (strong), which happens to be the tested branch but
# depends on order. The trailing `} else if ay * AXIS_RATIO` (only after strong-h) pins it.
mutate "M1" "$TARGET" \
  '                    out->type = UI_DUAL_EVT_ORIENT;
                } else if ((long)ay * UI_G_DUAL_AXIS_RATIO_DEN >=' \
  '                    out->type = UI_DUAL_EVT_FONT_STEP; out->steps = 1; /* MUTATION M1 */
                } else if ((long)ay * UI_G_DUAL_AXIS_RATIO_DEN >='
run_mutation "M1 horizontal swipe silently re-mapped (must stay ORIENT)"

# ---- M2：竖滑对偶反转（下滑减档）----
# 26.9.7 锚点唯一化：`dual->peak_steps = clamp_steps(dy / STEP);` 全文两处（强轴纵支
# 388 / fallback 纵支 413），从 axis=2 到 FONT 那几行两支逐字相同。用强轴纵支前独有的
# 注释行（"quantifies travel, it is not a font value."）作前缀区分。
# 26.9.7 unique anchor: the peak_steps line appears twice (strong-v 388 / fallback-v 413),
# and both vertical branches are byte-identical from axis=2 through FONT. The comment line
# unique to strong-v ("quantifies travel, it is not a font value.") pins it.
mutate "M2" "$TARGET" \
  '                    // quantifies travel, it is not a font value.
                    dual->axis = 2;
                    dual->peak_steps = clamp_steps(dy / UI_G_SWIPE_STEP_PX);' \
  '                    // quantifies travel, it is not a font value.
                    dual->axis = 2;
                    dual->peak_steps = clamp_steps(-dy / UI_G_SWIPE_STEP_PX); /* MUTATION M2 */'
run_mutation "M2 vertical pair inverted (down must grow, not shrink)"

# ---- M3：钳位失效 ----
mutate "M3" "$TARGET" \
  'if (n > UI_G_DUAL_SWIPE_MAX_DZ) return UI_G_DUAL_SWIPE_MAX_DZ;' \
  'if (n > UI_G_DUAL_SWIPE_MAX_DZ * 100) return UI_G_DUAL_SWIPE_MAX_DZ; /* MUTATION M3 */'
run_mutation "M3 step clamp disabled"

# ---- M4：静默撤销缺失 ----
mutate "M4" "$TARGET" \
  'out->type = UI_DUAL_EVT_RETRACT;' \
  'out->type = UI_DUAL_EVT_NONE; /* MUTATION M4 */'
run_mutation "M4 silent retract missing"

# ---- M5：单指恢复抑制缺失 ----
# 26.9.12：落定那行改成 `suppress = allow_suppress`（框选态可关），锚点随之更新。
# 26.9.12: the landing line became `suppress = allow_suppress` (the box mode may turn it
# off), so the anchor moved with it.
mutate "M5" "$TARGET" \
  'dual->suppress = true;' \
  'dual->suppress = false; /* MUTATION M5 */'
run_mutation "M5 single-finger swallow missing"

# ---- M25：解除条件退回「只认 count==0」（26.9.11 的关键缺陷，P1-1 的根因）----
# 真机抬手帧是 `end=zero`：count 停在 2、坐标全零。只认 count==0 就永远解除不了，
# 抑制一直挂着，单指识别器整段收不到拍子，框选「点了没反应」直到 30 s 超时。
# The hardware lift frame is `end=zero`: count stays 2 with zeroed coordinates. Releasing
# only on count==0 never fires, the swallow stays set, the single finger is never fed, and
# the selection reads as "taps do nothing" until the 30 s timeout.
mutate "M25" "$TARGET" \
  'if (t->count == 0 || (t->count >= 2 && !two)) {' \
  'if (t->count == 0) { /* MUTATION M25: the sick terminal frame keeps the swallow */'
run_mutation "M25 release reverted to count==0 only (sick frame starves the single finger)"

# ---- M26：单指优先策略位被忽略（框选态重新被饿死）----
# 26.9.13：靶子从「合取里的 allow_suppress」搬到「策略位那一行」——极性翻转之后，
# 这句 `if (single_finger_priority) return false;` 就是整条修复的唯一闸口。
# 26.9.13: the target moved from the `allow_suppress` conjunct to the policy line. After the
# polarity flip this single statement is the whole fix's only gate.
mutate "M26" "$TARGET" \
  '    if (dual->single_finger_priority) return false;' \
  '    /* MUTATION M26: policy bit ignored */'
run_mutation "M26 single-finger priority ignored (box mode starves the single finger again)"

# ---- M28：26.9.12 的短路顺序被还原（策略位只在 IDLE 生效）----
# 这是**真机上真的发生过的**那个写法：先判 `state != IDLE` 就 return true，策略位根本
# 没被读到。它必须被「TRACK 期间 engaged==false」那条断言擒住——那条断言就是本轮的牙。
# This is literally the shape that shipped in 26.9.12: `state != IDLE` returns true before the
# policy bit is ever read. The "engaged==false while TRACK" assertion is the tooth that has
# to catch it.
mutate "M28" "$TARGET" \
  '    if (dual->single_finger_priority) return false;
    if (dual->state != UI_DUAL_IDLE) return true;' \
  '    if (dual->state != UI_DUAL_IDLE) return true; /* MUTATION M28: 26.9.12 short-circuit */
    if (dual->single_finger_priority) return false;'
run_mutation "M28 engaged short-circuits before the policy bit (the 26.9.12 defect verbatim)"

# ---- M29：策略位默认值翻错（等于把「单指优先」变成全局默认）----
# 这条一旦存活，框选态「修好了」而全仓库其他页面全部退化：硬语义 2 被取消，双指判定后
# 剩下一指会被当成点击/翻页。它不是框选态的问题，是「一个位管两件事」的代价——所以
# 默认值也必须被断言钉住。
# If this survives, the box mode looks fixed while every other page regresses: hard rule 2 is
# gone and the leftover finger reads as a tap or a page turn. This is the price of one bit
# serving two behaviours, so the default is pinned by an assertion too.
mutate "M29" "$TARGET" \
  '    dual->single_finger_priority = false;' \
  '    dual->single_finger_priority = true; /* MUTATION M29: priority on for every page */'
run_mutation "M29 priority defaults to true (every page loses hard rule 2)"

# ---- M30：只修一半（策略位只在 IDLE 生效，手势进行中失效）----
# 与 M28 形似而不同：M28 是顺序错，M30 是**条件错**——`&& state == IDLE` 让修复恰好
# 在最需要它的那一段（DOWN2/TRACK）失效。两者都必须被同一条断言擒住，否则那条断言
# 只是在测「这行代码存在」，不是在测「这行代码有用」。
# Similar to M28 but not the same: M28 is ordering, M30 is the CONDITION - `&& state == IDLE`
# disables the fix exactly where it is needed (DOWN2/TRACK). Both must die on the same
# assertion, otherwise that assertion only tests "the line exists", not "the line works".
mutate "M30" "$TARGET" \
  '    if (dual->single_finger_priority) return false;' \
  '    if (dual->single_finger_priority && dual->state == UI_DUAL_IDLE) return false; /* MUTATION M30 */'
run_mutation "M30 priority honoured only in IDLE (fix disabled during the gesture)"

# ---- M27：重入守卫被拆（手没抬就重开手势 → 同一段按住反复发 RETRACT）----
# `need_lift` 与 `suppress` 拆开正是为了防止这一类：抑制在病灶帧解除后，若没有独立
# 的重入守卫，坐标一恢复有效就会再识别一次手势、再发一次 RETRACT。
# Splitting need_lift from suppress exists to prevent exactly this: once the swallow is
# released on a sick frame, without an independent re-entry guard the returning valid
# coordinates restart the gesture and fire a second RETRACT.
mutate "M27" "$TARGET" \
  'if (!two || dual->need_lift) {' \
  'if (!two) { /* MUTATION M27: re-entry guard removed */'
run_mutation "M27 re-entry guard removed (a held pair restarts the gesture)"

# ---- M6：刷新期丢弃缺失 ----
mutate "M6" "$TARGET" \
  'now - dual->last_ms > (int64_t)UI_G_COOLDOWN_MS) {' \
  'now - dual->last_ms > (int64_t)UI_G_COOLDOWN_MS * 1000) { /* MUTATION M6 */'
run_mutation "M6 refresh-gap discard missing"

# ---- M7：融合窗消歧失效（窗内抬手也进 TRACK）----
# 靶点用「融合窗那一行」本身：26.9.7 在 ABORT 分支里加了中止命名，跨分支的长锚点
# 一碰就断；锚点越贴靶子越不容易被无关改动带塌。
# Anchor on the fusion-window line itself: 26.9.7 added abort naming inside the ABORT
# branch, and a long cross-branch anchor breaks on any change next door.
mutate "M7" "$TARGET" \
  '            if (now - dual->t0_ms >= (int64_t)UI_G_DUAL_FUSE_MS) dual->state = UI_DUAL_TRACK;' \
  '            dual->state = UI_DUAL_TRACK; /* MUTATION M7: no fusion window */'
run_mutation "M7 fusion-window disambiguation gone"

# ---- M8：迟滞退回 ≈1.0（本次真机串扰的事故形态）----
mutate "M8" "$TARGET" \
  '} else if ((long)ay * UI_G_DUAL_AXIS_RATIO_DEN >=
                           (long)ax * UI_G_DUAL_AXIS_RATIO_NUM) {' \
  '} else if ((long)ay * 100 >= (long)ax * 101) { /* MUTATION M8 */'
run_mutation "M8 axis hysteresis back to ~1.0 (the cross-talk accident)"

# ---- M9：总位移死区被放大到 48（应 40，方向与本次修正相反）----
mutate "M9" "$TARGET" \
  'if (dist2 < (long)UI_G_DUAL_SWIPE_TRIGGER_PX * UI_G_DUAL_SWIPE_TRIGGER_PX) {' \
  'if (dist2 < (long)48 * 48) { /* MUTATION M9 */'
run_mutation "M9 total-travel dead zone widened to 48 (should be 40)"

# ---- M10：互斥失效（横轴也累积字号档位）----
# 26.9.7 锚点唯一化：`dual->axis = 1; dual->state = UI_DUAL_FIRE;` 全文两处（强轴横支
# 378 / fallback 横支 405）。带其后独有的 `out->type = ORIENT + } else if ay AXIS_RATIO`
# （只有强轴横支后面是它）唯一化。
# 26.9.7 unique anchor: `axis = 1; state = FIRE;` appears twice (strong-h 378 / fallback-h
# 405); the trailing ORIENT + `} else if ay AXIS_RATIO` (only after strong-h) pins it.
mutate "M10" "$TARGET" \
  '                    dual->axis = 1;
                    dual->state = UI_DUAL_FIRE;
                    out->type = UI_DUAL_EVT_ORIENT;
                } else if ((long)ay * UI_G_DUAL_AXIS_RATIO_DEN >=' \
  '                    dual->axis = 1;
                    dual->peak_steps = clamp_steps(dy / UI_G_SWIPE_STEP_PX); /* MUTATION M10 */
                    dual->state = UI_DUAL_FIRE;
                    out->type = UI_DUAL_EVT_ORIENT;
                } else if ((long)ay * UI_G_DUAL_AXIS_RATIO_DEN >='
run_mutation "M10 axis exclusion lost (horizontal also steps the font)"

# ---- M11：中途定轴回归（v2 之前的形态 = 本次串扰根因）----
mutate "M11" "$TARGET" \
  '            // While sliding: heartbeat only - no verdict, no event.
            dual->last_ms = now;
            return false;' \
  '            { /* MUTATION M11: verdict while sliding */
                const int mdx = dual->x_last - dual->x0;
                const int mdy = dual->y_last - dual->y0;
                const long m2 = (long)mdx * mdx + (long)mdy * mdy;
                if (m2 >= (long)UI_G_DUAL_SWIPE_TRIGGER_PX * UI_G_DUAL_SWIPE_TRIGGER_PX) {
                    const int a1 = abs_i(mdx), a2 = abs_i(mdy);
                    if ((long)a1 * UI_G_DUAL_AXIS_RATIO_DEN >= (long)a2 * UI_G_DUAL_AXIS_RATIO_NUM) {
                        dual->axis = 1; dual->state = UI_DUAL_FIRE;
                        out->type = UI_DUAL_EVT_ORIENT; out->dx = mdx; out->dy = mdy;
                        out->judged = true; dual->fire_ms = now; dual->last_ms = now;
                        return true;
                    }
                }
            }
            dual->last_ms = now;
            return false;'
run_mutation "M11 mid-gesture axis lock (the v2 root cause)"

# ---- M12：吞事件守卫缺失（app_loop 静态门禁 P10 的靶子）----
mutate "M12" "$LOOP_TARGET" \
  ' && touch.count == 1 && !ui_dual_engaged(&dual)) {' \
  ' ) { /* MUTATION M12 */'
if python3 tools/ai_detail_wiring_host_test.py >/dev/null 2>&1; then
    echo "MUTATION SURVIVED: M12 single-finger swallow guard missing"
    FAILED=1
else
    echo "caught: M12 single-finger swallow guard missing"
fi
restore_loop

# ---- M13：C-guard 横侧退化成 C-full（1.0–1.15 也开火 → 45° 误开横屏）----
mutate "M13" "$TARGET" \
  '} else if ((long)ax * UI_G_DUAL_FALLBACK_RATIO_DEN >=
                           (long)ay * UI_G_DUAL_FALLBACK_RATIO_NUM) {' \
  '} else if ((long)ax * 1 >= (long)ay * 1) { /* MUTATION M13 */'
run_mutation "M13 C-guard horizontal degraded to C-full (45-degree misfires)"

# ---- M14：C-guard 纵侧退化成 C-full（对称另一侧，必须独立钉，不能只钉一边）----
mutate "M14" "$TARGET" \
  '} else if ((long)ay * UI_G_DUAL_FALLBACK_RATIO_DEN >=
                           (long)ax * UI_G_DUAL_FALLBACK_RATIO_NUM) {' \
  '} else if ((long)ay * 1 >= (long)ax * 1) { /* MUTATION M14 */'
run_mutation "M14 C-guard vertical degraded to C-full (mirror side)"

# ---- M15：落定间距恒 0（M5-0 打点失效，框选/手势分界无数据）----
# 靶点是 two_point_gap 的返回值：乘 0 让函数仍被调用、dx/dy 仍被使用（避免
# -Werror 的 unused 把变异变成「编译失败」而不是「断言红」），结果恒 0。
# The anchor is two_point_gap's return: multiply by 0 so the function is still
# called and dx/dy still used (keeps -Werror from turning this into a build failure
# rather than a real assertion red), yielding 0.
mutate "M15" "$TARGET" \
  'return isqrt_i(dx * dx + dy * dy);' \
  'return isqrt_i(dx * dx + dy * dy) * 0; /* MUTATION M15 */'
run_mutation "M15 gap0 never recorded"

# ---- M16：速度门失效（按住抖动重新被判成滑动 → 误翻屏复现）----
# 靶点：速度判据整体 & 0（恒假），变量仍被引用（避免 -Werror 的 unused 把变异变成
# 「编译失败」而非「断言红」——M15 同法）。按住样本 26200 必须因此变红。
# The anchor ANDs the whole predicate with 0 (always false) while keeping the variables
# referenced, so this stays a real assertion red instead of a build failure (M15's trick).
mutate "M16" "$TARGET" \
  '                           (long)dist_px * UI_G_DUAL_MIN_SPEED_DEN < (long)UI_G_DUAL_MIN_SPEED_NUM * hold_ms) {' \
  '                           ((long)dist_px * UI_G_DUAL_MIN_SPEED_DEN < (long)UI_G_DUAL_MIN_SPEED_NUM * hold_ms) & 0) { /* MUTATION M16 */'
run_mutation "M16 speed gate disabled (hold jitter judged as a swipe)"

# ---- M17：two 退回只看 count（26.9.4 归零事故的形态）----
# 「判据双定义」复活：count 是芯片上报字段，抬手那一拍坐标已清零、两点已 inactive，
# 只有 count 仍是 2。退回只看 count 后这一拍不再算抬手 → 判定被推迟到（也许永远
# 不会到来的）count==0 帧。归零那两条用例会当场变红。
# Rolling `two` back to count alone revives the duplicate predicate: count is a
# chip-reported field, so on the lift tick the coordinates are already zeroed and both
# contacts inactive while count stays 2. This tick stops counting as a lift and the
# verdict is deferred to a count==0 frame that may never arrive.
mutate "M17" "$TARGET" \
  '    const bool two = ui_dual_two_valid(t);' \
  '    const bool two = t->count >= 2; /* MUTATION M17 */'
run_mutation "M17 two drops its active check (zero frame read as two fingers)"

# ---- M18：x_last 无条件写回（无视 midpoint 的返回值）----
# 第二层保护被拆：midpoint() 已经明说「算不出中点」，却照写 (0,0)，
# 于是 26.9.4 的伪位移（净位移 = −落点坐标、轴比 = |x0|/|y0|）原样回来。
# 判据层（M17/M19/M20）收紧之后，这一层是纵深——两层必须各有自己的牙。
# The second guard is torn out: midpoint() says it could not compute one and (0,0) is
# written anyway, bringing back the 26.9.4 fake travel (net travel = minus the landing
# point, ratio = |x0|/|y0|). With the predicate layer tightened (M17/M19/M20) this is
# depth of defence - each layer needs its own tooth.
mutate "M18" "$TARGET" \
'    int mx = 0, my = 0;
    if (midpoint(t, &mx, &my)) {
        dual->x_last = mx;
        dual->y_last = my;
    }' \
'    int mx = 0, my = 0; /* MUTATION M18: write x_last whatever midpoint() says */
    midpoint(t, &mx, &my);
    dual->x_last = mx;
    dual->y_last = my;'
run_mutation "M18 x_last written even with no valid midpoint"

# ---- M19：有效性判据去掉坐标子句（退回只信 active = 26.9.5 的形态）----
# round-3 实测：`pt->active = event != 1`，抬手过渡帧报的仍是 contact，坐标已清零而
# active 仍为真 → 26.9.5 的判据照样放行 → 归零伪位移 32.7% 原样保留。去掉
# `(x|y)!=0` 就等于把这一版退回去；宿主用例①②⑤（真机形态 + 单点归零）必红。
# Round 3 measured `pt->active = event != 1`: the lift transition frame still reports a
# contact, coordinates cleared but active true, so the 26.9.5 predicate lets it through
# and the 32.7% zero drift survives. Dropping `(x|y)!=0` reverts this version; host cases
# 1/2/5 (hardware shape + single-point zeroing) must go red.
mutate "M19" "$HEADER_TARGET" \
  'return p != NULL && p->active && (p->x != 0 || p->y != 0);' \
  'return p != NULL && p->active; /* MUTATION M19 */'
run_mutation "M19 validity predicate drops the coordinate clause (back to active-only)"
cp "$HEADER_BACKUP" "$HEADER_TARGET"

# ---- M20：逐点判据退化成成对 OR（26.9.5 的公式形态，审计裁定①点名的漏网）----
# `(p0.x|p0.y|p1.x|p1.y)!=0` 只要有一点带坐标就整体判有效：p0=(0,0)、p1 有效照样
# 算「两指有效」，中点被写成 p1/2、凭空生出半个间距的假位移。宿主用例⑤专钉这一条
# （手没动却算出 dx=150 dy=120、轴比 1.25 → 误翻屏）。
# The pairwise-OR form passes as soon as ANY point carries a coordinate: p0=(0,0) with a
# valid p1 still counts as "two contacts", the midpoint becomes p1/2 and half a gap of
# fake travel appears. Host case 5 pins exactly that (a still hand computing dx=150
# dy=120, ratio 1.25 -> a spurious flip).
mutate "M20" "$HEADER_TARGET" \
  '           ui_dual_point_valid(&t->points[0]) && ui_dual_point_valid(&t->points[1]);' \
  '           (t->points[0].x | t->points[0].y | t->points[1].x | t->points[1].y) != 0; /* MUTATION M20 */'
run_mutation "M20 per-point validity degraded to the pairwise OR (half-gap fake midpoint)"
cp "$HEADER_BACKUP" "$HEADER_TARGET"

# ---- M21：lift0 打回判定位（26.9.5 的形态，round-3 lift0 恒零的直接成因）----
# 判定位版本问的是「驱动这一拍还报 count>=2 吗」，而现象帧与判定帧常常不同拍——
# 现象帧 count=2 → 判定帧 count 已降为 0/1，于是帧级快照永远采不到，round-3 的
# 11 分钟日志里 lift0 一次都没亮。宿主用例③（active 滞后帧，count 仍是 2 但不是
# 病灶帧）当场变红。
# The verdict-site version asks "does the driver still report count>=2 this tick", but the
# symptom frame and the verdict tick are often different ticks - symptom has count==2,
# the verdict tick has already dropped to 0/1 - so a frame-level snapshot can never catch
# it and round 3's 11-minute log never lit lift0 once. Host case 3 (a lagging-active frame
# with count still 2 that is NOT a sick frame) goes red immediately.
mutate "M21" "$TARGET" \
  'out->lift0 = dual->saw_zero;' \
  'out->lift0 = t->count >= 2; /* MUTATION M21 */'
run_mutation "M21 lift0 marked at the verdict site again (round-3 permanently zero)"

# ---- M23：end 退回「看 active」（26.9.7 佐证件的靶子）----
# end 存在的全部理由就是**不查 active**：诚实型病灶帧（active 已假、坐标全零、count 仍 2）
# 是 lift0 的盲区，而它正是修复成立时的正常形态。若把 end 写成靠 ui_dual_saw_zero_frame()
# 判定，诚实型就被记成 `one`（像是「先抬了一指」），那条缝又隐形了——宿主 ⑦-1 立刻变红。
# The whole point of `end` is that it never consults `active`: an honest sick frame (active
# false, coordinates zero, count still 2) is lift0's blind spot and also the normal shape of
# a working repair. Deciding `end` from ui_dual_saw_zero_frame() reports such a frame as
# `one` ("a finger came up first") and hides the seam again - host case 7-1 goes red.
# 靶点必须带上后面的空行 + dist2 那一行：`out->end = end_kind(t);` 在文件里有三处
# （刷新期中止 / 融合窗中止 / 判定），只写这一行会打中第一处（融合窗中止），判定那处
# 原样活着——本轮实测就是这么活下来的（M23 SURVIVED）。锚点必须唯一。
# Anchor must include the blank line + dist2: `out->end = end_kind(t);` appears three
# times (refresh gap / fusion slip / verdict). Anchoring on the bare line hits the first
# one and leaves the verdict untouched - which is exactly how M23 survived this round.
mutate "M23" "$TARGET" \
'                out->end = end_kind(t);

                const long dist2' \
'                out->end = (t->count == 0) ? UI_DUAL_END_LIFT      /* MUTATION M23 */
                           : ui_dual_saw_zero_frame(t) ? UI_DUAL_END_ZERO
                                                       : UI_DUAL_END_ONE;

                const long dist2'
run_mutation "M23 end decided from the active-based symptom mark (honest frame reads as one)"

# ---- M24：freeze_n 不计数（佐证计数变成装饰）----
# 计数若不再累加，日志里恒为 0——「冻结保护跑过没有」就永远答不上来，与 26.9.5 的
# lift0 恒零是同一类失效：观测件坏了，把「采不到」伪装成「没发生」。宿主 ⑦-1 变红。
# If the counter never increments the log reads 0 forever, so "did the freeze guard ever
# run" becomes unanswerable - the same class of failure as 26.9.5's permanently zero lift0:
# a broken instrument dressing "cannot observe" up as "did not happen". Host 7-1 goes red.
mutate "M24" "$TARGET" \
'        dual->freeze_n++;' \
'        dual->freeze_n += 0; /* MUTATION M24: the counter never actually counts */'
run_mutation "M24 freeze_n never incremented (the corroborating counter goes decorative)"

# ---- M22：幽灵 TAP 回来（RETRACT 不再撤销单指识别器）----
# 26.9.7 修的就是这一行：双指落定发 RETRACT 的那一刻同步 cancel_gesture，陈旧 PRESS
# 才不会在两指抬起后被按 TAP 收尾。靶子在 app_loop.c，用 app_loop 宿主套件擒——
# 那条用例按真机形态摆了「过渡帧 count 仍 2 → 下一拍才是释放边沿」，拿掉撤销后
# TAP 立刻复现（实测 tap=1）。
# The line 26.9.7 added: RETRACT revokes the single-finger recognizer at the landing
# tick, so the stale PRESS cannot be closed as a TAP after both fingers leave. The
# target lives in app_loop.c and is caught by the app_loop host suite, whose case
# reproduces the hardware shape (transition tick with count==2, then the release edge);
# removing the revocation brings tap=1 straight back.
mutate "M22" "$LOOP_TARGET" \
'                if (dev.type == UI_DUAL_EVT_RETRACT) cancel_gesture(&ctx, current, &gesture);' \
'                /* MUTATION M22: no revocation on RETRACT (ghost TAP returns) */'
if sh tools/run_app_loop_host_tests.sh >/dev/null 2>&1; then
    echo "MUTATION SURVIVED: M22 RETRACT no longer revokes the stale single-finger PRESS"
    FAILED=1
else
    echo "caught: M22 RETRACT no longer revokes the stale single-finger PRESS (ghost TAP)"
fi
cp "$LOOP_BACKUP" "$LOOP_TARGET"

# ---- S1：判据唯一性（结构牙，不是变异）----
# 「两指有效」的全部成分必须封装在 ui_dual_two_valid() 里：本文件一旦重新出现裸的
# .active 测试，说明有人在喊函数之外又手写了一份判据，双定义就回来了。道理很硬：
# 常量有两份只是算错数，判据有两份是行为分叉。（先剥注释再查，免得被解说文字误伤。）
# Every ingredient of "two valid contacts" must live inside ui_dual_two_valid(): a raw
# .active test reappearing here means somebody hand-wrote the predicate again, and the
# fork is back. Duplicate constants merely miscompute; duplicate predicates fork
# behaviour. Comments are stripped first so the prose cannot trip it.
if python3 - <<'EOF'
import re, pathlib, sys
s = pathlib.Path('main/ui/ui_dualgesture.c').read_text()
code = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
code = '\n'.join(re.sub(r'//.*$', '', l) for l in code.split('\n'))
sys.exit(1 if re.search(r'\.active', code) else 0)
EOF
then
    echo "ok: the two-contact predicate lives only in ui_dual_two_valid()"
else
    echo "SURVIVOR: a raw .active test sits in ui_dualgesture.c (the predicate is split)"
    FAILED=1
fi

# ---- S2：判据必须**逐点**（审计裁定①的补强做成结构）----
# ui_dual_two_valid() 的体内必须对每个点各喊一次 ui_dual_point_valid()：只有两次调用
# 才是「两点各自有效」，一次（或零次）说明有人把判据写回了成对 OR / 只看 count。
# 原理做成结构，第二处定义才不可能存在——与 S1 同构，只是从「不许手写」升级到
# 「必须逐点」。
# The body of ui_dual_two_valid() must call ui_dual_point_valid() once per point: two
# calls mean "each point valid on its own"; one call (or none) means somebody rewrote it
# as a pairwise OR or a bare count test. Making the principle structural is what keeps a
# second definition impossible - same shape as S1, upgraded from "no hand-written copy"
# to "must be per-point".
if python3 - <<'EOF'
import re, pathlib, sys
s = pathlib.Path('main/ui/ui_dualgesture.h').read_text()
m = re.search(r'static inline bool ui_dual_two_valid\b.*?\n\}', s, re.S)
sys.exit(0 if m and m.group(0).count('ui_dual_point_valid(') == 2 else 1)
EOF
then
    echo "ok: ui_dual_two_valid() validates each point on its own (per-point, not pairwise)"
else
    echo "SURVIVOR: ui_dual_two_valid() is no longer per-point (pairwise OR / bare count)"
    FAILED=1
fi

# ---- S3：守卫点**分类型**逐点配对（审计裁定①第 3 项：枚举=2 与「成对」必须名实相符）----
# 幽灵 TAP 的本质不是「少了一行」，而是「喂食被守卫挡住 → 撤销没跟上」。但 `!ui_dual_engaged`
# 守卫有两类，**只有一类需要配对撤销**：
#   · 喂食守卫（包住 ui_gesture_feed）：挡的是"喂"，会饿死识别器 → **必须**有配对撤销，
#     且撤销必须在守卫**之外、之前**（写在守卫里面等于永远执行不到，因为饥饿时守卫为假）。
#   · 按下守卫（包住 ui_key_hit_test 的按键/菜单把手命中）：挡的是"把这一拍当成新的按下"，
#     不喂识别器、不产生悬挂状态 → **豁免**，但豁免的前提必须可检查：块体内不得出现
#     ui_gesture_feed。哪天有人把它改成也喂手势，豁免失效、立刻变红。
# 所以牙不是"守卫数=2 就算成对"，而是：**分类正确 + 每类各自的不变式成立 + 总数等于枚举值**。
# 新增第三个守卫会在分类时落进 exotic 分支 → 红，逼人回来看它属于哪一类。
# 行号只用于打印定位；判定全部锚在符号与上下文上（行号随 rebase 漂移，不能当锚）。
# The ghost TAP's essence is not a missing line but "feeding blocked by a guard while the
# revocation never followed". But `!ui_dual_engaged` guards come in two kinds and only one
# needs a paired revocation:
#   - feeding guard (wraps ui_gesture_feed): blocks the feed, starves the recognizer ->
#     MUST have a paired revocation, and that revocation must sit OUTSIDE and BEFORE the
#     guard (inside it would never run: the guard is false exactly when starving).
#   - press guard (wraps the ui_key_hit_test key/menu-handle hit): blocks "this tick is a
#     new press", never feeds the recognizer, leaves no dangling state -> EXEMPT, but the
#     exemption's premise is checkable: the body must not contain ui_gesture_feed. If
#     someone makes it feed gestures, the exemption lapses and this goes red.
# So the tooth is not "guard count == 2 means paired" but "classes correct + each class'
# own invariant holds + total equals the enumerated value". A third guard lands in the
# exotic bucket -> red, forcing a look at which class it belongs to. Line numbers are only
# printed for navigation; every check is anchored on symbols and context (line numbers
# drift across rebases and cannot serve as anchors).
if python3 - <<'EOF'
import re, pathlib, sys

src = pathlib.Path('main/app/app_loop.c').read_text()

def strip_comments(s):
    # 块注释替换为等量换行，保住行号；行注释直接剥掉。
    s = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'), s, flags=re.S)
    return '\n'.join(re.sub(r'//.*$', '', l) for l in s.split('\n'))

code = strip_comments(src)
GUARD = '!ui_dual_engaged(&dual)'
CANCEL = 'cancel_gesture(&ctx, current, &gesture)'
pos = [m.start() for m in re.finditer(re.escape(GUARD), code)]
line_of = lambda i: code.count('\n', 0, i) + 1

def block_after(i):
    j = code.find('{', i)
    if j < 0:
        return ''
    depth = 0
    for k in range(j, len(code)):
        if code[k] == '{':
            depth += 1
        elif code[k] == '}':
            depth -= 1
            if depth == 0:
                return code[j:k + 1]
    return ''

feeding, pressing, exotic = [], [], []
for i in pos:
    body = block_after(i)
    if 'ui_gesture_feed(' in body:
        feeding.append((i, body))
    elif 'ui_key_hit_test(' in body:
        pressing.append((i, body))
    else:
        exotic.append(i)

problems = []
if len(pos) != 2:
    problems.append('guard sites = %d, enumerated value is 2' % len(pos))
if len(feeding) != 1:
    problems.append('feeding guards = %d, expected exactly 1' % len(feeding))
if len(pressing) != 1:
    problems.append('press (exempt) guards = %d, expected exactly 1' % len(pressing))
for i in exotic:
    problems.append('unclassified guard at line %d: neither feeds ui_gesture_feed nor '
                    'ui_key_hit_test - classify it and state its rule' % line_of(i))

# 豁免前提：按下守卫不得喂手势识别器（喂了就不再是豁免点，必须配对撤销）。
for i, body in pressing:
    if 'ui_gesture_feed(' in body:
        problems.append('exempt press guard at line %d feeds ui_gesture_feed - the exemption '
                        'no longer holds, it needs a paired revocation' % line_of(i))

# 喂食守卫：撤销必须在守卫**之外**（块体内出现 cancel 等于永不执行）。
for i, body in feeding:
    if CANCEL in body:
        problems.append('feeding guard at line %d contains cancel_gesture inside its body - '
                        'that revocation can never run (guard is false exactly when starving)'
                        % line_of(i))

# 配对撤销：ui_dual_feed 之后、首个 ui_gesture_feed 之前，必须有 RETRACT 判定 + cancel，
# 且位置必须早于喂食守卫（时序：撤销先于饥饿，不能等饿了再撤）。
dual_feed = code.find('ui_dual_feed(&dual')
first_feed = code.find('ui_gesture_feed(')
window = code[dual_feed:first_feed] if (dual_feed >= 0 and first_feed > dual_feed) else ''
if 'UI_DUAL_EVT_RETRACT' not in window:
    problems.append('no UI_DUAL_EVT_RETRACT test between ui_dual_feed and the feeding guard')
rel = window.find(CANCEL)
if rel < 0:
    problems.append('no paired cancel_gesture between ui_dual_feed and the feeding guard')
elif feeding and (dual_feed + rel) > feeding[0][0]:
    problems.append('the revocation sits AFTER the feeding guard - starving ticks would run first')

if problems:
    for p in problems:
        print('  S3: ' + p)
    sys.exit(1)

print('  S3 pairing table:')
for i, _ in pressing:
    print('    line %-5d press-guard  (exempt: body hits ui_key_hit_test, never feeds the recognizer)'
          % line_of(i))
for i, _ in feeding:
    print('    line %-5d feeding-guard -> paired revocation at line %d (before the guard, outside it)'
          % (line_of(i), line_of(code.find(CANCEL, dual_feed))))
sys.exit(0)
EOF
then
    echo "ok: guard sites classified 1 feeding + 1 exempt, revocation precedes the feed and sits outside it"
else
    echo "SURVIVOR: guard classification broken, exemption premise lost, or the paired revocation is gone/misplaced"
    FAILED=1
fi

# ---- S4：DOWN2 的唯一进入点必须带 two 判据 + 有效中点（审计裁定的「注释没有牙」）----
# 「走到 TRACK 时 x_last 至少被写过一次真中点」这条不变式过去只写在注释里——注释是
# 真相源但没有牙。这里把它变成结构检查：`state = UI_DUAL_DOWN2` 全文件只能有一处，且
# 前后文必须同时出现 two 判据与 midpoint 调用。将来谁新增第二个入口（或把 midpoint
# 挪走），这颗牙立刻红。
# The invariant "by the time we reach TRACK, x_last was written with a real midpoint" used
# to live in a comment - a source of truth with no teeth. This makes it structural: exactly
# one `state = UI_DUAL_DOWN2` in the file, and its neighbourhood must contain both the two
# predicate and the midpoint call. A second entrance (or a moved midpoint) turns it red.
if python3 - <<'EOF'
import re, pathlib, sys
s = pathlib.Path('main/ui/ui_dualgesture.c').read_text()
code = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
code = '\n'.join(re.sub(r'//.*$', '', l) for l in code.split('\n'))
hits = re.findall(r'state\s*=\s*UI_DUAL_DOWN2', code)
m = re.search(r'case UI_DUAL_IDLE:(.*?)case UI_DUAL_DOWN2:', code, re.S)
seg = m.group(1) if m else ''
sys.exit(0 if (len(hits) == 1 and '!two' in seg and 'midpoint(' in seg) else 1)
EOF
then
    echo "ok: DOWN2 has a single entrance guarded by the two-predicate and a real midpoint"
else
    echo "SURVIVOR: DOWN2 gained an entrance (or lost its guard) - the invariant is unbounded"
    FAILED=1
fi

# ---- S5：策略位的接线必须存在，且必须在两个识别器**之前**各下发一次 ----
# 光有字段没有接线等于没修。26.9.13 起这一个位要同时管两个识别器，所以检查三件事：
#   ① `ctx.single_finger_priority` 被赋值（喂给单指识别器的那一份）；
#   ② `dual.single_finger_priority` 被同一拍下发（喂给双指状态机的那一份）；
#   ③ 两处都在 `ui_dual_feed` / `ui_gesture_feed` **之前**——写在后面用的是上一拍的
#      决定，正好错一拍，框选进入的第一拍仍然被饿死（26.9.12 就差点这么写）。
#   ④ 页面侧确实挂了 `.single_finger_priority =`（否则整个机制永远不会被打开）。
# A field without wiring fixes nothing. Since 26.9.13 the bit drives two recognizers, so four
# things are checked: (1) ctx gets the value (the single recognizer's copy); (2) the dual state
# machine gets the same tick's value; (3) both assignments precede ui_dual_feed /
# ui_gesture_feed - after them the recognizer acts on the previous tick, off by one, and the
# first box-mode tick is still starved; (4) the page really installs the callback.
if python3 - <<'PY_S5'
import pathlib, sys
loop = pathlib.Path('main/app/app_loop.c').read_text()
book = pathlib.Path('main/apps/app_book.c').read_text()
ctx_at = loop.find('ctx.single_finger_priority =')
dual_at = loop.find('dual.single_finger_priority = ctx.single_finger_priority')
feed_at = loop.find('ui_dual_feed(&dual')
gest_at = loop.find('ui_gesture_feed(&gesture')
wired = '.single_finger_priority =' in book
ok = (0 <= ctx_at < feed_at and 0 <= dual_at < feed_at and 0 <= feed_at < gest_at and wired)
sys.exit(0 if ok else 1)
PY_S5
then
    echo "ok: the policy bit is applied once per tick to ctx and the dual machine, before both feeds, and the page installs it"
else
    echo "SURVIVOR: policy bit is not wired (or wired after a feed, or no page installs it) - the box mode starves again"
    FAILED=1
fi

# ---- S6：单指识别器里的「裸 CANCEL」必须被策略位挡住（结构牙）----
# 26.9.13 之前 `if (touch && touch->count > 1) invalid = true;` 是一句裸判断：不管页面
# 要不要单指优先，一帧两点就把进行中的点按作废。这条牙要求：全文件里所有给 invalid
# 赋真的多点判断都必须带 `single_finger_priority` 条件，且这样的判断**至少存在一条**
# （删掉它同样是缺陷：两指手势会顺手触发一次点击）。
# Before 26.9.13 `if (touch && touch->count > 1) invalid = true;` was a bare test: a
# two-contact frame voided the tap no matter what the page wanted. This tooth requires every
# multitouch test that sets invalid to carry the policy condition, and at least one such test
# to exist at all (deleting it is the opposite defect: a two-finger gesture would click too).
if python3 - <<'PY_S6'
import pathlib, re, sys
s = pathlib.Path('main/ui/ui_gesture.c').read_text()
code = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
code = '\n'.join(re.sub(r'//.*$', '', l) for l in code.split('\n'))
hits = [l.strip() for l in code.split('\n') if 'count > 1' in l]
sys.exit(0 if (len(hits) >= 1 and all('single_finger_priority' in l for l in hits)) else 1)
PY_S6
then
    echo "ok: every multitouch cancel in the single-finger recognizer is gated by the policy bit"
else
    echo "SURVIVOR: a bare count>1 cancel (or no such check at all) - the tap dies with no policy"
    FAILED=1
fi

echo "dualgesture mutations: $([[ $FAILED -eq 0 ]] && echo 'all killed' || echo 'SURVIVORS PRESENT')"
exit $FAILED
