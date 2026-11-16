/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：单指手势状态机，页面负责解释事件与绘制。
 * English: Single-finger gesture state machine; pages interpret and draw events.
 * 冻结：翻页不画按下态；引用 ui_kit.h 阈值；短拖取消以免误翻。
 * Frozen: No pressed decoration for page turns; use ui_kit.h thresholds; short drags cancel to avoid accidental turns.
 */
#include "ui_gesture.h"
#include "ui_kit.h"
#include <string.h>

void ui_gesture_reset(ui_gesture_t *g) {
    if (g) memset(g, 0, sizeof(*g));
}

static bool emit(const ui_gesture_t *g, ui_gesture_type_t type, int64_t now, ui_gesture_event_t *out) {
    *out = (ui_gesture_event_t){
        .type = type, .x0 = g->x0, .y0 = g->y0, .x = g->x, .y = g->y,
        .hold_ms = now >= g->t0_ms ? now - g->t0_ms : 0,
    };
    return true;
}

bool ui_gesture_feed(ui_gesture_t *g, const app_ctx_t *ctx, ui_gesture_event_t *out) {
    if (!g || !ctx || !out) return false;
    const cst836u_touch_t *touch = ctx->touch;
    bool invalid = ctx->consumed || !touch || ctx->now_ms < 0;
    // 26.9.13（P1 路径 B 的修法，审计裁定）：多点**默认**仍取消进行中的序列——这是
    // 历史设计，为防两指手势顺手触发一次点击。但「这一页靠精确点按吃饭」时（框选态，
    // 策略位为真）它就是有害的：按下与抬起之间只要插进一帧两点，这次点按就作废，
    // 而且**没有任何提示**。真机表现正是「十次里有一两次没反应，再点一次就有了」。
    // 审计裁定的取舍：点不出来（卡死 30 s）的伤害远大于点错一个字，所以框选态改为
    // 让这一帧两点只是噪音——序列继续走，抬手照常按 TAP 收尾。
    // 26.9.13 (the fix for P1 path B, by adjudication): multitouch **by default** still
    // cancels an in-flight sequence - the historical intent being to stop a two-finger
    // gesture from incidentally producing a click. But on a page that lives on precise
    // taps (the box mode, policy bit set) it is harmful: one frame carrying two contacts
    // between press and release voids the tap **with no feedback at all**, which is
    // exactly the field report "one in ten does nothing, tap again and it works".
    // Adjudicated trade-off: failing to respond (30 s dead) hurts far more than picking
    // the wrong glyph, so in that mode the two-contact frame is just noise - the sequence
    // keeps running and the release still closes as a TAP.
    if (touch && touch->count > 1 && !ctx->single_finger_priority) invalid = true;
    if (g->active && ctx->now_ms < g->last_ms) invalid = true;
    if (invalid) {
        if (!g->active) return false;
        bool result = emit(g, UI_GESTURE_CANCEL, ctx->now_ms, out);
        ui_gesture_reset(g);
        return result;
    }
    if (!g->active) {
        if (!ctx->pressed || ctx->released || !touch->touched) return false;
        *g = (ui_gesture_t){ .active = true, .x0 = touch->x, .y0 = touch->y,
            .x = touch->x, .y = touch->y, .t0_ms = ctx->now_ms, .last_ms = ctx->now_ms };
        return emit(g, UI_GESTURE_PRESS, ctx->now_ms, out);
    }
    g->last_ms = ctx->now_ms;
    g->x = touch->x; g->y = touch->y;
    int32_t dx = (int32_t)g->x - g->x0, dy = (int32_t)g->y - g->y0;
    int32_t ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    // 容差使用二维距离，移回起点也不恢复轻点资格。/ Slop uses two-dimensional distance; returning to the origin never restores tap eligibility.
    if ((int64_t)dx * dx + (int64_t)dy * dy > (int64_t)UI_TOUCH_SLOP_PX * UI_TOUCH_SLOP_PX) g->moved = true;
    if (ctx->released || !touch->touched) {
        ui_gesture_type_t type = UI_GESTURE_CANCEL;
        if (ctx->released && !g->long_fired) {
            if (ax >= UI_SWIPE_MIN_PX && ax > ay) type = dx < 0 ? UI_GESTURE_SWIPE_L : UI_GESTURE_SWIPE_R;
            else if (ay >= UI_SWIPE_MIN_PX && ay > ax) type = dy < 0 ? UI_GESTURE_SWIPE_U : UI_GESTURE_SWIPE_D;
            else if (!g->moved) type = UI_GESTURE_TAP;
        }
        bool result = emit(g, type, ctx->now_ms, out);
        ui_gesture_reset(g);
        return result;
    }
    if (!g->moved && !g->long_fired && ctx->now_ms - g->t0_ms >= UI_LONG_PRESS_MS) {
        g->long_fired = true;
        return emit(g, UI_GESTURE_LONG_PRESS, ctx->now_ms, out);
    }
    if (g->moved && !g->long_fired) return emit(g, UI_GESTURE_MOVE, ctx->now_ms, out);
    return false;
}
