/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：真实识别器的时序与边界测试。
 * English: Timing and boundary tests for the actual recognizer.
 * 冻结：仅用于主机测试。/ Frozen: Host tests only.
 */
#include "ui_gesture.h"
#include "ui_kit.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
static ui_gesture_t g;
static ui_gesture_event_t event;
static cst836u_touch_t touch;
static app_ctx_t ctx;
static bool feed(int x, int y, int64_t time, bool down, bool press, bool release, bool consumed) {
    touch = (cst836u_touch_t){ .x = x, .y = y, .touched = down, .count = down ? 1 : 0 };
    ctx = (app_ctx_t){ .touch = &touch, .now_ms = time, .pressed = press, .released = release, .consumed = consumed };
    return ui_gesture_feed(&g, &ctx, &event);
}
// 26.9.13：显式给 count 与策略位的喂入口。多点帧与「框选态单指优先」这两件事
// 都必须能被测到——原来的 feed() 永远 count=1、策略位恒假，正好把本轮两条缺陷
// 都挡在测试面之外。
// 26.9.13: a feed that takes count and the policy bit explicitly. Both the
// two-contact frame and box-mode priority have to be testable - the old feed()
// always used count=1 and a false policy bit, which put both of this round's
// defects outside the tested surface.
static bool feed_n(int count, int x, int y, int64_t time, bool down, bool press,
                   bool release, bool consumed, bool priority) {
    touch = (cst836u_touch_t){ .x = x, .y = y, .touched = down, .count = count };
    ctx = (app_ctx_t){ .touch = &touch, .now_ms = time, .pressed = press, .released = release,
                       .consumed = consumed, .single_finger_priority = priority };
    return ui_gesture_feed(&g, &ctx, &event);
}
static void press(int x, int y, int64_t time) {
    ui_gesture_reset(&g);
    assert(feed(x, y, time, true, true, false, false));
    assert(event.type == UI_GESTURE_PRESS && event.hold_ms == 0);
}
int main(void) {
    press(200, 300, 100);
    assert(!feed(200, 300, 100, true, true, false, false));
    assert(feed(210, 310, 200, false, false, true, false));
    assert(event.type == UI_GESTURE_TAP && event.x0 == 200 && event.y0 == 300);
    assert(event.x == 210 && event.y == 310 && event.hold_ms == 100);
    assert(!feed(210, 310, 200, false, false, true, false));
    press(100, 100, 0);
    assert(!feed(124, 100, 499, true, false, false, false));
    assert(feed(124, 100, 500, true, false, false, false));
    assert(event.type == UI_GESTURE_LONG_PRESS);
    assert(!feed(124, 100, 900, true, false, false, false));
    assert(feed(124, 100, 901, false, false, true, false));
    assert(event.type == UI_GESTURE_CANCEL);
    const int dx[] = {-120, 120, 0, 0}, dy[] = {0, 0, -120, 120};
    const ui_gesture_type_t kinds[] = {UI_GESTURE_SWIPE_L, UI_GESTURE_SWIPE_R, UI_GESTURE_SWIPE_U, UI_GESTURE_SWIPE_D};
    for (int i = 0; i < 4; ++i) {
        press(200, 300, 100);
        assert(feed(200 + dx[i], 300 + dy[i], 150, true, false, false, false));
        assert(event.type == UI_GESTURE_MOVE);
        assert(feed(200 + dx[i], 300 + dy[i], 200, false, false, true, false));
        assert(event.type == kinds[i]);
    }
    press(100, 100, 0);
    assert(feed(125, 100, 500, true, false, false, false)); assert(event.type == UI_GESTURE_MOVE);
    assert(feed(100, 100, 600, false, false, true, false));
    assert(event.type == UI_GESTURE_CANCEL);
    press(100, 100, 0);
    assert(feed(220, 220, 100, false, false, true, false)); assert(event.type == UI_GESTURE_CANCEL);
    press(100, 100, 0);
    assert(feed(219, 100, 100, false, false, true, false)); assert(event.type == UI_GESTURE_CANCEL);
    press(100, 100, 0);
    assert(feed(117, 117, 500, true, false, false, false)); assert(event.type == UI_GESTURE_MOVE);
    assert(feed(117, 117, 600, false, false, true, false)); assert(event.type == UI_GESTURE_CANCEL);
    press(100, 100, 0);
    assert(feed(100, 100, 10, true, false, false, true)); assert(event.type == UI_GESTURE_CANCEL);
    assert(!feed(100, 100, 20, false, false, true, false));
    press(0, 0, INT64_MAX - 500);
    assert(feed(0, 0, INT64_MAX, true, false, false, false)); assert(event.type == UI_GESTURE_LONG_PRESS);
    press(65535, 65535, 100);
    assert(feed(0, 65535, 200, false, false, true, false)); assert(event.type == UI_GESTURE_SWIPE_L);
    press(10, 10, 100);
    assert(feed(10, 10, 99, true, false, false, false)); assert(event.type == UI_GESTURE_CANCEL);
    press(10, 10, 100); ctx.touch = NULL;
    assert(ui_gesture_feed(&g, &ctx, &event)); assert(event.type == UI_GESTURE_CANCEL);
    press(10, 10, 100); touch.count = 2; ctx.pressed = false;
    assert(ui_gesture_feed(&g, &ctx, &event)); assert(event.type == UI_GESTURE_CANCEL);
    ui_gesture_reset(&g);
    assert(!feed(10, 10, 100, true, true, false, true));
    assert(feed(10, 10, 100, true, true, false, false));
    assert(feed(10, 10, INT64_MIN, true, false, false, false));
    assert(event.type == UI_GESTURE_CANCEL && event.hold_ms == 0);
    assert(!ui_gesture_feed(NULL, &ctx, &event));

    // ---- 26.9.13 P1 路径 B（审计裁定）：单指优先下，一帧两点不得作废这次点按 ----
    // 真机形态：按下与抬起之间插进一帧 count=2（幻点/驱动多报一个触点），位移为 0。
    // 26.9.12 及以前：这一帧直接 CANCEL，点按无声作废——用户说的「十次里有一两次没反应」。
    // 26.9.13：框选态（策略位为真）只是把它当噪音，序列继续，抬手照常 TAP。
    // 26.9.13 P1 path B: with single-finger priority, one two-contact frame must not void
    // the tap. Hardware shape: a count=2 frame (phantom / duplicate report) between press
    // and release with zero travel. Through 26.9.12 it cancelled silently - the user's "one
    // in ten does nothing". With the policy bit set it is just noise and the release still
    // closes as a TAP.
    ui_gesture_reset(&g);
    assert(feed_n(1, 200, 300, 100, true, true, false, false, true));
    assert(event.type == UI_GESTURE_PRESS);
    assert(!feed_n(2, 200, 300, 130, true, false, false, false, true)); // 不产事件
    assert(g.active);                                                   // 序列还活着
    assert(feed_n(1, 200, 300, 160, false, false, true, false, true));
    assert(event.type == UI_GESTURE_TAP && event.x0 == 200 && event.y0 == 300);
    // 对照：策略位为假时，同一帧两点**必须**照旧取消——历史语义不许被这条修复拆掉
    // （两指手势不得顺手触发一次点击，那条理由对普通页面依然成立）。
    // Control: with the bit false the same frame must still cancel - the historical
    // guarantee (a two-finger gesture must not also click) still holds for normal pages.
    ui_gesture_reset(&g);
    assert(feed_n(1, 200, 300, 100, true, true, false, false, false));
    assert(feed_n(2, 200, 300, 130, true, false, false, false, false));
    assert(event.type == UI_GESTURE_CANCEL && !g.active);

    puts("gesture host tests passed");
}
