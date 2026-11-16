/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：双指识别器的状态机、三条硬语义与冻结映射的主机测试（M4-A v2：抬手定轴）。
 * English: Host tests for the two-finger machine, its three hard rules and the
 * frozen mapping (M4-A v2: verdict on lift).
 * 冻结：仅用于主机测试。/ Frozen: Host tests only.
 *
 * v2 的核心不变量：**滑动过程中零判定、零事件**；只有抬手那一拍用最终净位移
 * 做一次比对（置信 1.6 / 兜底 1.15）。这直接消灭了「起手瞬间哪轴先占到便宜就定终身」的串扰。
 * The v2 invariant: **no verdict and no event while sliding**; only the lift tick
 * judges the final net travel (1.6 confidence / 1.15 fallback). That removes the old
 * failure where whichever axis won the opening instant decided the whole gesture.
 */
#include "ui_dualgesture.h"
#include <assert.h>
#include <stdio.h>

// SEL_DIST 数据牙（M5-0 二轮重采）：阈值必须落在 round2 双峰之间的空档 289→312 内。
// 抬到 320 会漏掉 gap0=312 那笔慢而远的手势（spd=0.468 / dt=919ms），压到 280 会把
// gap0=289 的框选姿势当成手势放进来。定值可以改，空档不能忘——忘了这里就红。
// SEL_DIST data tooth (M5-0 round 2): the threshold must sit inside the 289->312
// valley between the two clusters. 320 would lose the slow, far gesture at gap0=312
// (spd=0.468 / dt=919ms); 280 would let the gap0=289 framing pose through as a
// gesture. The value may change; the valley may not be forgotten.
_Static_assert(UI_G_SEL_DIST_PX > 289 && UI_G_SEL_DIST_PX < 312,
               "SEL_DIST must sit inside the round-2 valley (289 -> 312)");

static ui_dual_t d;
static ui_dual_event_t ev;
static cst836u_touch_t touch;
static app_ctx_t ctx;

/// n=0/1/2 触点数；(x0,y0)/(x1,y1) 为两点坐标。/ n contacts at the given points.
static void put(int n, int x0, int y0, int x1, int y1) {
    touch = (cst836u_touch_t){0};
    touch.touched = n > 0;
    touch.count = (uint8_t)n;
    if (n >= 1) {
        touch.points[0] = (cst836u_point_t){ .active = true, .x = (uint16_t)x0, .y = (uint16_t)y0 };
        touch.x = (uint16_t)x0;
        touch.y = (uint16_t)y0;
    }
    if (n >= 2) {
        touch.points[1] = (cst836u_point_t){ .active = true, .x = (uint16_t)x1, .y = (uint16_t)y1 };
    }
}

/// 任意形态的一拍（26.9.5 归零帧 / active 滞后的畸形帧都靠它造）：active 与 count
/// 解耦，用来喂驱动真实会见到的畸形。
/// Any-shaped tick (how the zero-drift and lagging-active frames are built): `active`
/// is decoupled from `count` so we can feed the malformed ticks real hardware sends.
static void put_raw(int n, bool a0, int x0, int y0, bool a1, int x1, int y1) {
    touch = (cst836u_touch_t){0};
    touch.touched = n > 0;
    touch.count = (uint8_t)n;
    if (n >= 1) {
        touch.points[0] = (cst836u_point_t){ .active = a0, .x = (uint16_t)x0, .y = (uint16_t)y0 };
        touch.x = (uint16_t)x0;
        touch.y = (uint16_t)y0;
    }
    if (n >= 2) {
        touch.points[1] = (cst836u_point_t){ .active = a1, .x = (uint16_t)x1, .y = (uint16_t)y1 };
    }
}

static bool feed(int64_t t) {
    ev = (ui_dual_event_t){0};
    ctx = (app_ctx_t){ .touch = &touch, .now_ms = t };
    return ui_dual_feed(&d, &ctx, &ev);
}

/// 落下双指并越过融合窗，中点位于 (cx, cy)。/ Land two fingers and pass the fusion window.
static void land_and_track(int cx, int cy, int64_t t) {
    put(2, cx - 100, cy, cx + 100, cy);
    assert(feed(t) && ev.type == UI_DUAL_EVT_RETRACT);
    assert(d.gap0 == 200); // M5-0：落定间距 = 200px（手势两指靠拢）
    assert(!feed(t + UI_G_DUAL_FUSE_MS));
    assert(d.state == UI_DUAL_TRACK);
}

/// 把中点移到 (cx,cy) 并喂一拍（滑动途中）。/ Move the midpoint and feed one sliding tick.
static bool slide(int cx, int cy, int64_t t) {
    put(2, cx - 100, cy, cx + 100, cy);
    return feed(t);
}

/// 抬手并返回判定后的"是否有事件"。/ Lift and report whether an event was emitted.
static bool lift(int64_t t) {
    put(0, 0, 0, 0, 0);
    return feed(t);
}

/// 在 (cx,cy) 按住不动：每 100ms 喂一拍，跨 [t_from,t_to]，两指始终不抬。
/// 保持 100ms 节拍是为了不触发「刷新期丢弃」（相邻喂入 >150ms 会直接 ABORT），
/// 让速度门拿到真实 dt。真机上这一路径由主循环 10–50ms 的采样自然满足。
/// Hold still at (cx,cy): one tick every 100ms across [t_from,t_to], both contacts
/// stay down. The 100ms cadence avoids the refresh-gap abort (>150ms aborts) so the
/// speed gate sees a truthful dt; on hardware the main loop samples every 10-50ms.
static void feed_hold(int cx, int cy, int64_t t_from, int64_t t_to) {
    for (int64_t t = t_from; t <= t_to; t += 100) {
        put(2, cx - 100, cy, cx + 100, cy);
        feed(t);
    }
}

int main(void) {
    /* ---- 四态骨架 / State skeleton ---- */
    ui_dual_reset(&d);
    assert(d.state == UI_DUAL_IDLE && !ui_dual_engaged(&d));

    // DOWN2：双指落下即发静默撤销（硬语义 1——单指不挂起，只事后撤销）。
    // DOWN2: landing emits the retract (hard rule 1 - the single finger is never
    // suspended; the retract only happens afterwards).
    put(2, 100, 200, 300, 200);
    assert(feed(1000));
    assert(ev.type == UI_DUAL_EVT_RETRACT && d.state == UI_DUAL_DOWN2);
    assert(!feed(1000)); // 同一拍不重复发 / one event per tick
    assert(d.state == UI_DUAL_DOWN2);

    // 融合窗内抬手 = 误触，ABORT 零动作，且**没有做过判定**。
    // Lifting inside the window is a slip: abort, and no verdict was made.
    put(0, 0, 0, 0, 0);
    assert(!feed(1050));
    assert(d.state == UI_DUAL_ABORT && !ev.judged);

    /* ---- v2 核心不变量：滑动中零判定、零事件 ---- */
    // 即使位移早已满足 1.6，滑动途中也不得定轴、不得发事件。
    // Even with 1.6 long since satisfied, sliding must not decide or emit.
    ui_dual_reset(&d);
    land_and_track(200, 200, 2000);
    assert(!slide(400, 200, 2100)); // dx=200, dy=0：2:1 早就满足
    assert(d.axis == 0 && !ev.judged);
    assert(!slide(600, 200, 2200)); // 继续滑
    assert(d.axis == 0 && !ev.judged);
    assert(lift(2250));
    assert(ev.type == UI_DUAL_EVT_ORIENT && ev.dx == 400 && ev.dy == 0 && ev.judged);
    assert(d.axis == 1);

    /* ---- 横滑 = 切换横竖屏（不是翻页）/ Horizontal = orientation, not paging ---- */
    ui_dual_reset(&d);
    land_and_track(200, 200, 3000);
    assert(!slide(400, 200, 3100));
    assert(lift(3150) && ev.type == UI_DUAL_EVT_ORIENT);
    assert(ev.steps == 0 && d.axis == 1);

    /* ---- 竖滑 = 字号对偶（下滑加、上滑减）/ Vertical = font pair ---- */
    ui_dual_reset(&d);
    land_and_track(200, 200, 4000);
    assert(!slide(200, 320, 4100)); // 下移 120px = 一档
    assert(lift(4150) && ev.type == UI_DUAL_EVT_FONT_STEP && ev.steps == 1);
    assert(ev.dy == 120 && d.axis == 2);

    ui_dual_reset(&d);
    land_and_track(200, 200, 5000);
    assert(!slide(200, 80, 5100)); // 上移 120px = 减一档
    assert(lift(5150) && ev.type == UI_DUAL_EVT_FONT_STEP && ev.steps == -1);

    // 跨 3 档 / three steps
    ui_dual_reset(&d);
    land_and_track(200, 200, 6000);
    assert(!slide(200, 560, 6100)); // 下移 360px
    assert(lift(6150) && ev.steps == UI_G_DUAL_SWIPE_MAX_DZ);

    // 钳位：滑 720px 也不超过 3 档 / clamping: 720px still caps at 3
    ui_dual_reset(&d);
    land_and_track(200, 200, 7000);
    assert(!slide(200, 920, 7100));
    assert(lift(7150) && ev.steps == UI_G_DUAL_SWIPE_MAX_DZ);

    // 不足一档（40..119px）：定了纵轴但静默不做。
    // 用 dy=90 而非 60：60px/150ms=0.40<0.42 会先被速度门拦下、根本轮不到定轴；
    // 本用例要验的是「定了纵轴却不足一档」，必须让它先过速度门。
    // Vertical but under one step (40..119px): judged, silently dropped.
    // dy=90 (not 60): 60px/150ms=0.40<0.42 would be dropped by the speed gate before
    // the axis is decided; this case must test "axis locked vertical, short of a step".
    ui_dual_reset(&d);
    land_and_track(200, 200, 8000);
    assert(!slide(200, 290, 8100)); // dy=90（<120，不足一档；0.60 px/ms 过门）
    assert(!lift(8150));
    assert(ev.judged && ev.type == UI_DUAL_EVT_NONE && d.axis == 2 && d.peak_steps == 0);
    assert(d.state == UI_DUAL_ABORT);

    /* ---- 有效门槛 = 1.15：1.0–1.15 真模糊静默，≥1.15 按较大轴动作 ---- */
    // 置信 1.6（A）与兜底 1.15（C-guard）都取较大轴，行为并集落在 1.15；1.6 只区分
    // 「置信/兜底」，不改变结果——这正是不选 C-full 的代价被量化的地方。
    // The behavioural union of A (1.6) and C-guard (1.15) sits at 1.15; 1.6 only
    // labels confidence and does not change the outcome.

    // 45°（1:1，完全相等）→ 静默丢弃，但留下判定日志。
    ui_dual_reset(&d);
    land_and_track(200, 200, 9000);
    assert(!slide(400, 400, 9100)); // dx=dy=200（比值 1.00）
    assert(!lift(9150));
    assert(ev.judged && ev.type == UI_DUAL_EVT_NONE && d.axis == 0);
    assert(d.state == UI_DUAL_ABORT);

    // 1.01（69520 事故形态：73/72，45° 略偏横）→ 仍在 1.0–1.15，静默。
    // 这条专防 C 退化成 C-full：C-full 下会误开横屏（方向由 1px 决定）。
    ui_dual_reset(&d);
    land_and_track(200, 200, 9600);
    assert(!slide(301, 300, 9700)); // dx=101 dy=100 → 1.01
    assert(!lift(9750) && ev.type == UI_DUAL_EVT_NONE && d.axis == 0);

    // 1.14 横（<1.15）→ 静默；1.16 横（≥1.15，<1.6）→ C-guard 横。
    ui_dual_reset(&d);
    land_and_track(200, 200, 10000);
    assert(!slide(314, 300, 10100)); // dx=114 dy=100 → 1.14
    assert(!lift(10150) && ev.type == UI_DUAL_EVT_NONE && d.axis == 0);

    ui_dual_reset(&d);
    land_and_track(200, 200, 10200);
    assert(!slide(316, 300, 10300)); // dx=116 dy=100 → 1.16
    assert(lift(10350) && ev.type == UI_DUAL_EVT_ORIENT && d.axis == 1);

    // 1.59（<1.6 走 C-guard）与 1.61（≥1.6 走主锥）都必须是横——两条路都要绿。
    ui_dual_reset(&d);
    land_and_track(200, 200, 10500);
    assert(!slide(359, 300, 10600)); // dx=159 dy=100 → 1.59
    assert(lift(10650) && ev.type == UI_DUAL_EVT_ORIENT && d.axis == 1);

    ui_dual_reset(&d);
    land_and_track(200, 200, 10700);
    assert(!slide(361, 300, 10800)); // dx=161 dy=100 → 1.61
    assert(lift(10850) && ev.type == UI_DUAL_EVT_ORIENT && d.axis == 1);

    // 纵向 1.125（dx=160 dy=180，含 ≥1 档潜力）→ 静默；专钉纵向迟滞与兜底上限。
    // 若把纵向主锥或兜底任一侧放松到 ~1.0，此例会开火（M8 / M14 的靶子）。
    ui_dual_reset(&d);
    land_and_track(200, 200, 11000);
    assert(!slide(360, 380, 11100)); // dx=160 dy=180 → 1.125
    assert(!lift(11150) && ev.type == UI_DUAL_EVT_NONE && d.axis == 0);

    // 1.20 纵（≥1.15）→ C-guard 字；1.49 纵（80826 日志形态）→ C-guard 字。
    ui_dual_reset(&d);
    land_and_track(200, 200, 11300);
    assert(!slide(300, 320, 11400)); // dx=100 dy=120 → 1.20
    assert(lift(11450) && ev.type == UI_DUAL_EVT_FONT_STEP && ev.steps == 1);

    ui_dual_reset(&d);
    land_and_track(200, 200, 11600);
    assert(!slide(300, 349, 11700)); // dx=100 dy=149 → 1.49
    assert(lift(11750) && ev.type == UI_DUAL_EVT_FONT_STEP && ev.steps == 1);

    /* ---- 死区 40px + 速度门：两道「够格才算滑动」的闸 ---- */
    ui_dual_reset(&d);
    land_and_track(200, 200, 12000);
    assert(!slide(239, 200, 12100)); // dx=39（dist=39 <40）→ 死区
    assert(!lift(12150) && ev.judged && ev.type == UI_DUAL_EVT_NONE && d.axis == 0);

    // 41px 在 150ms 内 = 0.27px/ms < 0.42 → 越过 40px 死区却被速度门拦下（axis 保持 0）。
    // 26.9.4 的有意变更：40px 之上还要「够快」才算滑动，慢微动不再误翻屏。
    ui_dual_reset(&d);
    land_and_track(200, 200, 12200);
    assert(!slide(241, 200, 12300)); // dx=41
    assert(!lift(12350) && ev.judged && ev.type == UI_DUAL_EVT_NONE && d.axis == 0);

    // 45px 在 100ms 内 = 0.45px/ms：过死区(≥40) 且过速度门(>0.42) → 横滑。
    // 这一例同时是 M9 的靶子：把死区放大到 48，45<48 会被拦下 → 必红。
    // 两拍同时间戳 12500 是刻意的：融合窗最小 dt=100ms，只有 100ms 内走 45px 才能
    // 同时高于速度门、又落在 40..48 之间，把「死区 40 vs 48」区分开。
    ui_dual_reset(&d);
    put(2, 100, 200, 300, 200); // 落定，中点 200
    assert(feed(12400) && ev.type == UI_DUAL_EVT_RETRACT);
    assert(!feed(12500));       // 融合窗到点 → TRACK（dt 起点 12400）
    put(2, 145, 200, 345, 200); // 中点 245 → dx=45
    assert(!feed(12500));       // 同一拍内的滑动采样
    assert(lift(12500) && ev.type == UI_DUAL_EVT_ORIENT && d.axis == 1); // dt=100ms

    // 对照：同为 150ms，300px=2.0px/ms → 过门 → 仍按横滑切屏。
    ui_dual_reset(&d);
    land_and_track(200, 200, 12600);
    assert(!slide(500, 200, 12700)); // dx=300
    assert(lift(12750) && ev.type == UI_DUAL_EVT_ORIENT && d.axis == 1);

    /* ---- v2 关键收益：中途转向按最终净位移判定 ---- */
    // 先往下滑 300（若按旧实现"中途定轴"此刻已锁成纵向），再横向拖到净位移
    // dx=600 / dy=300（2:1）→ 必须判成横滑。旧实现在第一次满足时就锁死，会错判。
    // Slide down 300 first (the old per-tick lock would freeze "vertical" here),
    // then drag sideways to a net 600x300 (2:1) - the verdict must be horizontal.
    ui_dual_reset(&d);
    land_and_track(200, 200, 14000);
    assert(!slide(200, 500, 14100)); // dy=300 先占上风 / vertical leads first
    assert(d.axis == 0);
    assert(!slide(800, 500, 14200)); // 净 dx=600 dy=300 → 2:1 横
    assert(d.axis == 0);             // 滑动途中仍不得判定
    assert(lift(14250) && ev.type == UI_DUAL_EVT_ORIENT);
    assert(ev.dx == 600 && ev.dy == 300);

    // 反向同理：先横向再拉直为纵向 → 判为字号（并钳位到 ±3）。
    // The mirror case: horizontal first, then straighten vertically -> font step.
    ui_dual_reset(&d);
    land_and_track(200, 200, 15000);
    assert(!slide(500, 200, 15100)); // dx=300 先占上风
    assert(!slide(500, 500, 15200)); // 净 dx=300 dy=300 → 1:1 斜滑
    assert(!lift(15250) && ev.type == UI_DUAL_EVT_NONE && d.axis == 0);

    ui_dual_reset(&d);
    land_and_track(200, 200, 16000);
    assert(!slide(500, 200, 16100));
    assert(!slide(300, 800, 16200)); // 净 dx=100 dy=600 → 6:1 纵
    assert(lift(16250) && ev.type == UI_DUAL_EVT_FONT_STEP);
    assert(ev.steps == UI_G_DUAL_SWIPE_MAX_DZ); // 600/120=5，钳到 3
    assert(d.axis == 2);

    /* ---- 互斥：判定为横就绝不附带字号 / Exclusion: horizontal never carries steps ---- */
    ui_dual_reset(&d);
    land_and_track(200, 200, 17000);
    // dy 必须足够大（≥120，即至少一档），否则「横轴也累积档位」的变异观察不到。
    // dy must reach one step (>=120), or a mutation that also accumulates steps
    // on the horizontal axis stays invisible (this actually happened).
    assert(!slide(800, 400, 17100)); // dx=600 dy=200 → 3:1，仍判横
    assert(lift(17150) && ev.type == UI_DUAL_EVT_ORIENT);
    assert(ev.steps == 0 && d.peak_steps == 0);

    /* ---- 硬语义 2 · 单指恢复：抬起一指不复活成单指 ---- */
    ui_dual_reset(&d);
    land_and_track(200, 200, 18000);
    assert(!slide(200, 320, 18100));
    put(1, 200, 320, 0, 0); // 抬起一指 → 提交
    assert(feed(18150) && ev.type == UI_DUAL_EVT_FONT_STEP);
    // 提交后仍占用触摸：这一拍的单指不得被解释成点击/翻页。
    assert(ui_dual_engaged(&d));
    assert(!feed(18160)); // 收敛 FIRE → IDLE，但抑制保持
    assert(d.state == UI_DUAL_IDLE && ui_dual_engaged(&d));
    put(0, 0, 0, 0, 0);
    assert(!feed(18170));
    assert(!ui_dual_engaged(&d)); // 两指全抬才解除

    /* ---- 硬语义 3 · 刷新期丢弃 ---- */
    ui_dual_reset(&d);
    land_and_track(200, 200, 19000);
    assert(!slide(200, 320, 19100));
    assert(!slide(200, 320, 19300)); // 跳变 200ms > 冷却窗
    assert(d.state == UI_DUAL_ABORT);
    assert(!feed(19310));            // 丢弃后不因"看起来还在按"而复活
    assert(d.state == UI_DUAL_IDLE);

    /* ---- 冷却：一次手势只发一次 ---- */
    ui_dual_reset(&d);
    land_and_track(200, 200, 20000);
    assert(!slide(200, 320, 20100));
    assert(lift(20150) && ev.type == UI_DUAL_EVT_FONT_STEP);
    put(2, 100, 200, 300, 200);
    assert(!feed(20200)); // 冷却内不接受新手势
    assert(d.state == UI_DUAL_IDLE);
    put(2, 100, 200, 300, 200);
    assert(feed(20400));  // 冷却结束后照常
    assert(ev.type == UI_DUAL_EVT_RETRACT);

    /* ---- M5-0：落定间距 gap0（框选 vs 手势的分界靠数据说话，不预设） ---- */
    // 手势一侧已由 land_and_track 钉住（两指靠拢，200px）。这里补**框选**一侧：
    // 两指分开按句首句尾（600px），静止抬手 → 死区 ABORT，但 gap0 必须原样保留，
    // 调用方的 dual 日志在抬手那一拍读它——这正是 SEL_DIST 的数据来源。
    // The gesture side is pinned by land_and_track (fingers close, 200px). Here is
    // the **framing** side: a pair spread wide (600px), lifted without travel -> dead
    // zone ABORT, yet gap0 must survive intact because the caller's dual log reads it
    // on the lift tick - that is exactly the data SEL_DIST comes from.
    ui_dual_reset(&d);
    put(2, 100, 600, 700, 600); // 600px apart
    assert(feed(23000) && ev.type == UI_DUAL_EVT_RETRACT);
    assert(d.gap0 == 600);
    assert(!feed(23000 + UI_G_DUAL_FUSE_MS));
    assert(d.state == UI_DUAL_TRACK);
    assert(d.gap0 == 600);            // 跟踪期不变 / unchanged while tracking
    assert(!lift(23200));             // 无位移 → 死区，无事件 / no travel -> dead zone
    assert(ev.judged && ev.dx == 0 && ev.dy == 0);
    assert(d.gap0 == 600);            // 抬手后仍保留（日志要读）/ survives the lift

    /* ---- 速度门（26.9.4）：按住抖动不得判成滑动 ---- */
    // 真机日志 gen26.9.3：双指按住做框选姿势，dt 0.7–2.4s、净位移 187–425px、
    // 速度 0.10–0.39 px/ms，旧实现全判成滑动 → 10 分钟误翻屏 15 次、误调字号 6 次。
    // 以下三例钉死新契约（快滑 0.478–3.3 px/ms 必须不受影响）。
    // 1) 按住：400px / 2200ms = 0.18 px/ms → ABORT（不翻屏、不改字号）
    ui_dual_reset(&d);
    land_and_track(200, 200, 24000);
    feed_hold(600, 200, 24200, 26100); // 移到 600 后按住 ~1.9s
    assert(!lift(26200));              // dt=2200ms
    assert(ev.judged && ev.type == UI_DUAL_EVT_NONE && d.axis == 0);
    assert(d.state == UI_DUAL_ABORT);

    // 2) 快滑：400px / 200ms = 2.0 px/ms → ORIENT（真横滑照常翻屏）
    ui_dual_reset(&d);
    land_and_track(200, 200, 27000);
    assert(!slide(600, 200, 27150));
    assert(lift(27200) && ev.type == UI_DUAL_EVT_ORIENT && d.axis == 1);

    // 3) 慢而远竖滑：830px / 1700ms ≈ 0.49 px/ms（>0.42）→ FONT_STEP，不被误杀
    //    这正是「速度门优于时长门」的证据：慢（1.7s）但远（830px）仍是有意滑动。
    ui_dual_reset(&d);
    land_and_track(200, 200, 28000);
    feed_hold(260, 1030, 28200, 29600); // 净 dy=830，慢慢走
    assert(lift(29700) && ev.type == UI_DUAL_EVT_FONT_STEP);
    assert(d.axis == 2 && ev.steps == UI_G_DUAL_SWIPE_MAX_DZ); // 830/120=6 → 钳 3

    /* ---- 速度门整数式 ≡ 浮点 0.42（防 off-by-one 脆性）---- */
    // dist*50 < 21*dt ⇔ dist/dt < 0.42。逐点比对，跳过精确边界（浮点在边界无意义）。
    for (int dt = 100; dt <= 3000; dt += 7) {
        for (int dist = 40; dist <= 1500; dist += 3) {
            const long lhs = (long)dist * UI_G_DUAL_MIN_SPEED_DEN;
            const long rhs = (long)UI_G_DUAL_MIN_SPEED_NUM * dt;
            const long gap = lhs > rhs ? lhs - rhs : rhs - lhs;
            if (gap <= 40) continue; // 边界附近跳过浮点抖动
            const bool int_says_slow = lhs < rhs;
            const bool flt_says_slow = ((double)dist / (double)dt) < 0.42;
            if (int_says_slow != flt_says_slow) {
                printf("speed gate mismatch: dist=%d dt=%d int=%d flt=%d\n",
                       dist, dt, (int)int_says_slow, (int)flt_says_slow);
                assert(0 && "integer speed gate diverges from the 0.42 threshold");
            }
        }
    }

    /* ---- 26.9.5/26.9.6 归零修复：归零帧不许污染任何派生量 ---- */
    // 事故形态（round2 占判定 48%、round3 占 32.7%）：驱动抬手（过渡）帧 count 仍是 2、
    // 坐标报 0，而 **active 仍为真**（`pt->active = event != 1`，只有芯片明确报 up 才置
    // 假）——这正是 26.9.5 收紧到 active 后**仍然失效**的原因（round3 复验未通过）。
    // 于是 26.9.6 的判据信**位置**不信标志位：两点各自都得带坐标才算有效触点。
    // 旧代码把它当「两指还在」→ 最后有效中点记成 (0,0) → 抬手净位移 = −落点坐标，
    // 轴比退化成 |x0|/|y0|（**只由落点决定**，所以用户感觉是「随机」翻屏/调字号）。
    // The accident (48% of round-2 verdicts, 32.7% in round 3): the driver's lift
    // (transition) frame still reports count==2 with zeroed coordinates while **active
    // stays true** (`pt->active = event != 1` - only an explicit chip "up" clears it).
    // That is why 26.9.5's tightening onto active still failed (round-3 re-verification
    // did not pass). Hence 26.9.6 trusts POSITION, not flags: each point must carry
    // coordinates to count as valid. The old code read such a frame as "still two
    // fingers", stored (0,0) as the last valid midpoint, and got a net travel of minus
    // the landing point with a ratio of |x0|/|y0| - decided only by where the fingers
    // LANDED, hence the user's "random" flips and font steps.

    // ① 框选姿势原样抬手（**round-3 真机形态：active 真 + 坐标全零**）：手几乎没动 →
    // 伪位移本来会是 √(300²+240²)≈384px 的横滑。
    // A still framing pose lifting off (**the round-3 hardware shape: active true with
    // all-zero coordinates**): the fake travel used to be sqrt(300^2+240^2) ≈ 384px.
    ui_dual_reset(&d);
    land_and_track(300, 240, 30000);            // 落定中点 (300,240)
    assert(!slide(300, 240, 30100));            // 手指没动（沉降）
    put_raw(2, true, 0, 0, true, 0, 0);         // 病灶帧：count=2、active 真、坐标全零
    assert(!feed(30200));
    assert(ev.judged && ev.lift0);              // 这一拍就是判定，且现象位打标
    assert(ev.dx == 0 && ev.dy == 0);           // **零伪位移**：x_last 没被写成 (0,0)
    assert(ev.type == UI_DUAL_EVT_NONE && d.state == UI_DUAL_ABORT); // 死区 → 零动作
    // 判定只做一次：再来一拍真抬手不得重复判定（一次手势只准一份结论）。
    assert(!feed(30300) && !ev.judged);

    // ② 真滑动之后遇病灶帧：位移必须是**用户真的划出来的那一段**，不是落点的相反数。
    ui_dual_reset(&d);
    land_and_track(200, 200, 31000);
    assert(!slide(300, 200, 31100));            // 中点右移 100px
    put_raw(2, true, 0, 0, true, 0, 0);
    assert(feed(31200) && ev.type == UI_DUAL_EVT_ORIENT); // 100px/200ms=0.5 过门 → 横屏
    assert(ev.dx == 100 && ev.dy == 0 && ev.lift0);
    // 伪位移版本这里会是 dx=-200 dy=-200 → dist=282、轴比 1.00 → 静默丢弃，
    // 也就是真机上「我明明划了却没反应 / 我明明没划却翻屏」的分岔点。
    // With the fake travel this tick would read dx=-200 dy=-200 -> dist=282, ratio
    // 1.00 -> silently dropped: exactly the fork between "I swiped and nothing
    // happened" and "I did not swipe and it flipped".

    // ③ active 滞后一帧（count=2 但第二点已 inactive、坐标也已清零）：按抬手判定、
    // 不得污染；但**它不是病灶帧**——芯片已经不声称 active 了，lift0 不打标。
    // A lagging-active tick (count==2 but contact 2 is already inactive with cleared
    // coordinates): judge it as a lift and pollute nothing; it is NOT a sick frame
    // though - the chip no longer claims active, so lift0 stays clear.
    ui_dual_reset(&d);
    land_and_track(300, 240, 32000);
    assert(!slide(340, 240, 32100));            // 累计真实位移 dx=40
    put_raw(2, true, 240, 240, false, 0, 0);    // 一指已失效
    assert(!feed(32200));
    assert(ev.judged && !ev.lift0);             // 现象位：没见到「active 却全零」的点
    assert(ev.dx == 40 && ev.dy == 0);          // 仍是真实位移（40px），不是 -300/0 垃圾值
    assert(ev.type == UI_DUAL_EVT_NONE);        // 40px/200ms=0.2 < 0.42 → 速度门 ABORT

    // ④ 真抬手（count 降到 0）不打 lift0 标：标只对「芯片在骗人」的帧负责。
    // A real lift (count drops to 0) never sets lift0: the flag only marks frames where
    // the chip is lying.
    ui_dual_reset(&d);
    land_and_track(200, 200, 33000);
    assert(!slide(400, 200, 33100));
    assert(lift(33150) && ev.type == UI_DUAL_EVT_ORIENT && !ev.lift0);

    // ⑤ **单点归零**（26.9.6 收紧的那一条，M19 的靶子）：p0 无位置信息、p1 好好的。
    // 成对 OR 判据 `(p0.x|p0.y|p1.x|p1.y)!=0` 会因为 p1 带坐标而整体判有效 → 中点被
    // 写成 p1/2 = (450,360) → 凭空生出 dx=150 dy=120（dist=192 / 200ms = 0.96 过速度门、
    // 轴比 1.25 过 C-guard）→ **手没动却翻了屏**。逐点判据冻结 x_last → 零动作。
    // Per-point OR would pass on p1's coordinates, write the midpoint as p1/2 =
    // (450,360) and invent dx=150 dy=120 (dist=192 / 200ms = 0.96 clears the speed gate,
    // ratio 1.25 clears the C-guard) -> **a screen flip with a still hand**. The per-point
    // predicate freezes x_last, so nothing happens.
    ui_dual_reset(&d);
    land_and_track(300, 240, 40000);            // 落定中点 (300,240)
    assert(!slide(300, 240, 40100));            // 手指没动
    put_raw(2, true, 0, 0, true, 900, 720);     // p0 归零、p1 有效
    assert(!feed(40200));
    assert(ev.judged && ev.lift0);              // 见到了病灶点（p0），打标
    assert(ev.dx == 0 && ev.dy == 0);           // 冻结：x_last 停在 (300,240)
    assert(ev.type == UI_DUAL_EVT_NONE && d.state == UI_DUAL_ABORT);

    // ⑥ 锁存独立于判定：病灶帧若落在融合窗内（→ 误触 ABORT、这一拍没有判定），
    // 现象也必须被记下来，不能因为「没判」就神隐。这是 26.9.5 打在判定位的直接后果
    // （round-3 lift0 恒零），锁存到手势级才堵得住。
    // The latch is independent of the verdict: a sick frame inside the fusion window
    // (-> slip ABORT, no verdict that tick) must still be recorded rather than vanishing
    // because "nothing was judged". That is exactly what marking at the verdict site cost
    // in 26.9.5 (round-3 lift0 permanently zero); latching at gesture level closes it.
    ui_dual_reset(&d);
    put(2, 200, 200, 400, 200);
    assert(feed(41000) && ev.type == UI_DUAL_EVT_RETRACT);
    put_raw(2, true, 0, 0, true, 0, 0);         // 融合窗内的病灶帧
    assert(!feed(41050));                       // 窗内抬手 = 误触 → ABORT，无判定
    assert(d.state == UI_DUAL_ABORT && !ev.judged);
    assert(d.saw_zero);                         // 但现象已锁存 / the symptom is latched

    /* ---- 26.9.7：lift0 的盲区由 end / freeze_n 补上（审计裁定）---- */
    // lift0 只在「芯片仍报 active 却给全零坐标」时亮——**撒谎型**病灶帧。芯片若发的是
    // **诚实型**（active 已置假、坐标全零、count 仍 2），lift0 天然全无，而那正是修复
    // 成立该有的样子：把「lift0 全无」当可疑，会把成功判成失败。`end=zero` 只读 count
    // 与坐标、**不查 active**，两种病灶都抓得住——这才是「现象还在」的 active 无关证据。
    // lift0 only lights for the LYING kind of sick frame (active still true, coordinates
    // zero). An HONEST chip (active already false, coordinates zero, count still 2) leaves
    // it dark forever - which is exactly what a working repair looks like, so reading
    // "lift0 absent" as suspicious would fail a success. `end=zero` reads count and
    // coordinates only, never `active`, so it catches both kinds: the active-independent
    // evidence that the symptom is still there.

    // ⑦-1 诚实型病灶帧：lift0 **不亮**（盲区），但 end=zero 抓住了它，零动作。
    ui_dual_reset(&d);
    land_and_track(300, 240, 42000);
    assert(!slide(300, 240, 42100));
    put_raw(2, false, 0, 0, false, 0, 0);       // active 假 + 坐标全零 + count 仍 2
    assert(!feed(42200));
    assert(ev.judged && !ev.lift0);             // 盲区：这条打标看不见诚实型病灶
    assert(ev.end == UI_DUAL_END_ZERO);         // 但收尾方式看得见（count>=2 无可用两点）
    assert(ev.dx == 0 && ev.dy == 0);           // 冻结住 → 零伪位移
    assert(ev.type == UI_DUAL_EVT_NONE);
    // freeze_n 的结构上界是 1：`!two` 那一拍立刻结束手势，只有它能冻结。这条断言把
    // 事实钉住——将来若允许连续冻结帧，它会红，逼人回来把口径（和 README 的矩阵）改对。
    // freeze_n's structural bound is 1: the `!two` tick ends the gesture at once, so only
    // it freezes. Pinning that here means a future change allowing repeated freeze frames
    // turns this red and forces the wording (and the README matrix) to be fixed.
    assert(ev.freeze_n == 1);

    // ⑦-2 真抬手（count==0）：end=lift，不是病灶；lift0 也不亮。
    ui_dual_reset(&d);
    land_and_track(200, 200, 43000);
    assert(!slide(400, 200, 43100));
    assert(lift(43150));
    assert(ev.end == UI_DUAL_END_LIFT && !ev.lift0 && ev.freeze_n == 1);

    // ⑦-3 先抬起一指（count==1）：end=one——不对称抬手的正常收尾。
    ui_dual_reset(&d);
    land_and_track(200, 200, 44000);
    assert(!slide(300, 200, 44100));
    put_raw(1, true, 300, 200, false, 0, 0);
    assert(feed(44200) && ev.type == UI_DUAL_EVT_ORIENT); // 100px/200ms=0.5 过门
    assert(ev.end == UI_DUAL_END_ONE && !ev.lift0);

    /* ---- 26.9.7 中止原因：每道门都要报名（审计裁定⑤「静默是日志的敌人」）---- */
    // ⑦-4 融合窗内抬手 = 「双指快速点一下」：畸形输入的高发形态（本轮新增验收项），
    // 也是 M5-A「模式内双指挂起」要挡的那一种。以前它完全静默，日志里只剩「什么都
    // 没发生」——round-4 的「66 落定 / 65 判定」缺的那 1 笔就是这一类。
    // 7-4 lift inside the fusion window = a quick two-finger tap: the malformed input
    // this round adds to the protocol, and the one M5-A's "suspend two fingers inside
    // the mode" must block. It used to be completely silent; round 4's missing 1-in-66
    // was one of these.
    ui_dual_reset(&d);
    put(2, 400, 400, 600, 400);
    assert(feed(50000) && ev.type == UI_DUAL_EVT_RETRACT);
    assert(lift(50050) == false);
    assert(ev.abort == UI_DUAL_ABORT_SLIP); // 有名字了 / it has a name now
    assert(ev.end == UI_DUAL_END_LIFT);
    assert(!ev.judged && d.state == UI_DUAL_ABORT);

    // ⑦-5 死区：过了融合窗但净位移不足 → deadzone（不是「静默」，也不是 slip）。
    ui_dual_reset(&d);
    land_and_track(300, 300, 51000);
    assert(!lift(51200));
    assert(ev.abort == UI_DUAL_ABORT_DEADZONE && ev.judged);
    assert(ev.end == UI_DUAL_END_LIFT);

    // ⑦-6 速度门：走够了距离但太慢 → slow。三道门各报各的名字，才可能事后分辨
    // 「用户没做手势」与「手势被某道门拦下」。
    ui_dual_reset(&d);
    land_and_track(300, 400, 53000);
    assert(!slide(420, 400, 53200));
    // 按 100ms 节拍喂住：相邻喂入 >150ms 会先撞「刷新期丢弃」（gap），把速度门挡在后面。
    // 这一条本轮踩过——用稀疏喂入测速度门，测到的是 gap，不是 slow。
    // Keep feeding on a 100ms cadence: >150ms between ticks trips the refresh-gap abort
    // first and hides the speed gate. Hit this while writing the case - a sparse feed
    // measures gap, not slow.
    feed_hold(420, 400, 53300, 54000); // 120px / 1100ms = 0.11 px/ms < 0.42
    assert(!lift(54100));
    assert(ev.abort == UI_DUAL_ABORT_SLOW && ev.judged);

    /* ---- 单指完全不进本层 / A single finger never enters this layer ---- */
    ui_dual_reset(&d);
    put(1, 200, 200, 0, 0);
    assert(!feed(21000) && d.state == UI_DUAL_IDLE && !ui_dual_engaged(&d));
    assert(!feed(21100) && d.state == UI_DUAL_IDLE);

    /* ---- 健壮性 / Robustness ---- */
    ui_dual_reset(NULL);
    assert(!ui_dual_engaged(NULL));
    ui_dual_reset(&d);
    land_and_track(200, 200, 22000);
    ctx = (app_ctx_t){ .touch = NULL, .now_ms = 22100 };
    assert(!ui_dual_feed(&d, &ctx, &ev));
    assert(!ui_dual_feed(&d, NULL, &ev));
    assert(!ui_dual_feed(NULL, &ctx, &ev));
    assert(!ui_dual_feed(&d, &ctx, NULL));
    ui_dual_reset(&d);
    assert(d.state == UI_DUAL_IDLE && !ui_dual_engaged(&d));

    /* ================= 26.9.12 P1-1：抑制不得饿死单指 ================= */
    // 真机形态（gen26.9.11 日志）：抬手帧是 `end=zero` —— count 仍为 2、两个坐标都是 0、
    // 驱动也没把 active 置假。26.9.11 的解除条件只认 count==0，于是抑制一直挂着，
    // 单指识别器整段收不到拍子，框选表现成「点了没反应」，要等 30 s 超时才退出。
    // Hardware shape (gen26.9.11 log): the lift frame is `end=zero` - count is still 2,
    // both coordinates are 0, and the driver has not cleared active either. 26.9.11 only
    // released the swallow on count==0, so it stayed set, the single-finger recognizer was
    // never fed, and the selection read as "taps do nothing" until the 30 s timeout.
    ui_dual_reset(&d);
    land_and_track(200, 200, 60000);
    assert(d.suppress);                                   // 落定即抑制（硬语义 2）
    put_raw(2, false, 0, 0, false, 0, 0);                 // 病灶收尾：count 2 / 坐标全零
    assert(!feed(60100));
    assert(!d.suppress);                                  // 26.9.12：这一拍必须解除抑制
    // ABORT 是只存在一拍的终止态，这一拍仍算「占用触摸」；下一拍收敛回 IDLE 后再判，
    // 才是「单指通道真的回来了」。若抑制没解除，这一拍之后 engaged 依旧为真——正是
    // 26.9.11 那条「一直点不出来」的形态。
    // ABORT is a one-tick terminal state and still counts as owning the touch; judge after
    // the next tick settles to IDLE, which is when "the single path is really back" holds.
    // With the swallow still set, engaged stays true here - exactly 26.9.11's "taps do
    // nothing" shape.
    put(0, 0, 0, 0, 0);
    assert(!feed(60150));
    assert(d.state == UI_DUAL_IDLE && !ui_dual_engaged(&d));
    // 对照：`count == 1`（不对称抬手）**仍然**保持抑制——剩下一指不得被当成点击。
    // Control: count==1 (asymmetric lift) still HOLDS the swallow - the leftover finger
    // must not read as a tap.
    ui_dual_reset(&d);
    land_and_track(200, 200, 60200);
    put(1, 200, 320, 0, 0);
    assert(!feed(60300));
    assert(d.suppress);

    // 框选态：single_finger_priority = true → 双指照常判定与撤销，但不再占住单指。
    // 两个观察点缺一不可：
    //   ① **TRACK 期间**（26.9.13 路径 A 的牙）：26.9.12 在这一拍先判 `state != IDLE`
    //      就 return true，策略位根本没被读——真机两段 525/537 ms 输入真空正是这里；
    //   ② **state 回 IDLE、suppress 仍为真**那一拍（26.9.11 的饿死位置）。
    // Box mode: single_finger_priority = true - the dual layer judges and retracts as usual
    // but no longer owns the single finger. Two observation points, both required:
    // (1) DURING TRACK (the tooth for 26.9.13 path A) - 26.9.12 returned true on
    // `state != IDLE` before ever reading the policy bit, which is exactly the two
    // 525/537 ms input blackouts on hardware; (2) the tick where state settled back to
    // IDLE while suppress is still set (26.9.11's starvation spot).
    ui_dual_reset(&d);
    land_and_track(200, 200, 61000);
    assert(!slide(200, 320, 61100));
    put(1, 200, 320, 0, 0);                        // 抬一指 → 判定提交
    assert(feed(61150) && ev.type == UI_DUAL_EVT_FONT_STEP);
    assert(!feed(61160));                          // 收敛 FIRE → IDLE，抑制保持
    assert(d.state == UI_DUAL_IDLE && d.suppress && ui_dual_engaged(&d));

    ui_dual_reset(&d);
    d.single_finger_priority = true;
    land_and_track(200, 200, 62000);
    // ① 双指态**活着**的每一拍，单指通道都必须是通的（26.9.12 在这里断线）。
    // (1) Every tick the dual session is alive, the single path must be open (26.9.12
    // cut it here).
    assert(d.state == UI_DUAL_TRACK && !ui_dual_engaged(&d));
    assert(!slide(200, 320, 62100));
    assert(d.state == UI_DUAL_TRACK && !ui_dual_engaged(&d));
    put(1, 200, 320, 0, 0);
    assert(feed(62150) && ev.type == UI_DUAL_EVT_FONT_STEP); // 判定与撤销照旧
    assert(!feed(62160));
    assert(d.state == UI_DUAL_IDLE && d.suppress);            // 重入守卫仍在
    assert(!ui_dual_engaged(&d));                             // 但单指通道已经放开
    // 重入守卫：坐标又变回有效也不得重开手势（否则同一段按住会再发一次 RETRACT）。
    // Re-entry guard: valid coordinates returning must NOT restart the gesture (otherwise
    // the same hold fires a second RETRACT).
    put(2, 100, 320, 300, 320);
    assert(!feed(62200) && ev.type == UI_DUAL_EVT_NONE);
    assert(d.state == UI_DUAL_IDLE);
    // 两指真正抬离后才允许下一次手势（硬语义的门槛没被这条修复拆掉）。
    // Only after both contacts really leave may the next gesture start - the fix does not
    // tear down that threshold.
    put(0, 0, 0, 0, 0);
    assert(!feed(62250));
    put(2, 100, 200, 300, 200);
    assert(feed(62300) && ev.type == UI_DUAL_EVT_RETRACT);
    ui_dual_reset(&d);
    assert(!d.single_finger_priority && !d.need_lift);        // reset 恢复默认

    puts("dual gesture host tests passed");
    return 0;
}
