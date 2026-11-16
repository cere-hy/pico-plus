/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：首页手势纯判定（26.11.17）。见 home_gesture.h 的说明：这里不画、
 * 不读文件、不碰 app_ctx，只把「空白 / 双指上滑 / 长按归属」判成结论。
 * English: pure decisions for the home page's gestures (26.11.17). See the header:
 * no drawing, no file I/O, no app_ctx - just the three verdicts.
 */
#include "home_gesture.h"

/// 矩形命中：右/下边界开区间，与 app_dashboard.c 的 hit() 同语义。
/// Rect hit with half-open right/bottom edges, same semantics as the page's hit().
static bool rect_hit(EpdRect r, int x, int y) {
    return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

bool home_blank_hit(int view, int x, int y) {
    // 空白长按只在主视图启用：其余视图各有自己的版面与热区，不参与这套表。
    // Blank long press is root-only: other views carry their own layout and hot zones.
    if (view != HOME_VIEW_ROOT) return false;
    if (x < 0 || y < 0) return false;
    // 底部 tab 带：它的点按由 ui_nav_hit 直接接管，不属于「空白」。
    // The bottom tab band is owned by ui_nav_hit and is not blank.
    if (y >= UI_NAV_TOUCH_TOP) return false;
    // 状态栏信息带（26.11.17 拍板「收窄」）：时钟与电量不做手势领地。
    // 这条带子的下沿与右上角热区的上沿同高（88），带子与热区连成一片，不留缝。
    // Info band (ruled 26.11.17, "narrow"): clock and battery are not gesture
    // territory. Its bottom equals the corner zone's top, so they form one block.
    if (rect_hit(HOME_RESERVED_INFO, x, y)) return false;
    // 五块热区逐一排除。顺序无关，但必须与 control() 用的是同一批常量。
    // Exclude the five hot zones one by one; order is irrelevant, the constants must be
    // the same ones control() uses.
    if (rect_hit(HOME_HIT_CONTINUE, x, y)) return false;
    if (rect_hit(HOME_HIT_PUSH, x, y)) return false;
    if (rect_hit(HOME_HIT_CORNER, x, y)) return false;
    if (rect_hit(HOME_HIT_LEFT, x, y)) return false;
    if (rect_hit(HOME_HIT_RIGHT, x, y)) return false;
    return true;
}

bool home_dual_wants_push(int view, home_dual_seq_t *seq, const ui_dual_event_t *ev) {
    if (!seq || !ev) return false;
    // 序列边界先处理，且**先于视图门控**：换页不该把「本条接触序列已用过」这件事
    // 带过去，也不该让它留到下一次手势。
    // Sequence boundary first, and BEFORE the view gate: a page switch must neither
    // carry the latch over nor leave it set for the next gesture.
    if (ev->type == UI_DUAL_EVT_RETRACT) {
        seq->fired = false;
        return false;
    }
    if (view != HOME_VIEW_ROOT) return false;
    // 只认竖滑；横滑（ORIENT）与其它事件一律不动。
    // Vertical swipes only; ORIENT and everything else are ignored.
    if (ev->type != UI_DUAL_EVT_FONT_STEP) return false;
    // 上滑 = steps < 0：锚在 ui_dualgesture.c 的 steps 生成行（两处：主轴支与
    // 模糊带支），`steps = clamp_steps(dy / UI_G_SWIPE_STEP_PX)`，屏幕坐标向下为
    // 正，所以上滑得到负值。下滑是「加档」，在本页没有动作。
    // Up = steps < 0, anchored at the two assignment sites in ui_dualgesture.c where
    // steps = clamp_steps(dy / UI_G_SWIPE_STEP_PX); y grows downward, so an upward
    // swipe is negative. A downward swipe grows the font and has no action here.
    if (ev->steps >= 0) return false;
    // 同一接触序列只触发一次：长滑把 |steps| 抬到 2、3 是同一个事件的档位大小，
    // 而抖动重判可能再发一个事件——这一次要落空。
    // One trigger per contact sequence: |steps| = 2 or 3 is one event's magnitude, and a
    // jitter re-judge may emit a second event - which must find the latch set.
    if (seq->fired) return false;
    seq->fired = true;
    return true;
}

home_long_action_t home_long_decide(int view, int x, int y) {
    if (view != HOME_VIEW_ROOT) return HOME_LONG_NONE;
    // 热区优先，空白兜底：左卡是「组件浏览」的原动作，其余热区没有长按动作。
    // Hot zones first, blank as the fallback: the left card keeps its component-browse
    // action; the other zones have no long-press action at all.
    if (rect_hit(HOME_HIT_LEFT, x, y)) return HOME_LONG_COMPONENT;
    if (home_blank_hit(view, x, y)) return HOME_LONG_TRANSFER;
    return HOME_LONG_NONE;
}

home_push_route_t home_push_route(bool text_ready) {
    return text_ready ? HOME_PUSH_OPEN : HOME_PUSH_FALLBACK;
}
