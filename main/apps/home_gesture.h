/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：首页手势的**纯判定**（26.11.17 手势批）。只回答三个问题——
 * 这一点算不算空白、这一次双指上滑要不要开最新推送、这一次长按该走哪条路。
 * 不画、不读文件、不碰 app_ctx；动作出口留在 app_dashboard.c。
 * 这样做的好处与电量批同构：判据可以在宿主上逐条钉死，页面只剩接线。
 *
 * English: Pure decisions for the home page's gestures (26.11.17). It answers
 * exactly three questions - is this point blank, does this two-finger swipe
 * want the newest push, and which route does this long press take - and does
 * nothing else: no drawing, no file I/O, no app_ctx. The action stays in
 * app_dashboard.c, so the judgement is pinned on the host and the page keeps
 * only the wiring.
 */
#pragma once

#include <stdbool.h>

#include "epdiy.h"          // EpdRect
#include "ui_dualgesture.h" // ui_dual_event_t / UI_DUAL_EVT_*
#include "ui_nav_layout.h"  // UI_NAV_TOUCH_TOP

/// 视图编号：与 app_dashboard.c 的 `enum { HOME_ROOT, HOME_COMPONENT, HOME_GOAL,
/// HOME_TIME }` **逐值一致**（0..3）。用 int 传，避免把页面的私有枚举拖进判定模块；
/// 数值一致性由 dashboard 侧的 `_Static_assert` 钉住——顺序若被谁改了，编译当场红。
/// View ids matching app_dashboard.c's enum one for one (0..3). Passing int keeps the
/// page's private enum out of this module; a _Static_assert on the dashboard side pins
/// the numbers, so reordering either side fails the build instead of the device.
typedef enum {
    HOME_VIEW_ROOT = 0,
    HOME_VIEW_COMPONENT = 1,
    HOME_VIEW_GOAL = 2,
    HOME_VIEW_TIME = 3,
} home_view_id_t;

/* ---- 主视图的五块热区（单源：control()/on_gesture 与本模块引用同一批常量） ----
 * 中文：原来这些矩形是 app_dashboard.c 里的散落字面量。搬到此处后，
 * 「空白判定」与「命中反馈」用的是同一批数字，不会出现「判定说空白、
 * 页面却认为点在卡上」这种两套表打架的情况。
 * English: these rects used to be literals scattered in app_dashboard.c. Kept here,
 * the blank test and the hit feedback share one table - no "blank says yes, the
 * page says card" disagreement.
 */
#define HOME_HIT_CONTINUE ((EpdRect){24, 172, 636, 394}) ///< 继续阅读卡 / continue card
#define HOME_HIT_PUSH     ((EpdRect){24, 566, 636, 74})  ///< 推送状态卡 / push card
/// 右上角：`control()` 用左半 `{510,88,60,60}`、tap 分支用右半 `{588,88,60,60}`，
/// 两处本就是相邻元素。空白判定取**包络框**（envelope）`{510,88,138,60}`——把两半之间
/// 那条 18 px 缝也吞进去。术语纠正（26.11.17）：它**不是并集**，并集会留下缝隙，
/// 而缝隙一旦被判成空白，就是热区边缘的误触带；宁可少一次触发，不可把热区旁当空白。
/// Top-right: control() uses the left half and the tap branch the right half. The blank
/// test uses the ENVELOPE {510,88,138,60}, which also swallows the 18 px gap between
/// them. Terminology fix (26.11.17): not a union - a union leaves the gap, and a gap
/// judged blank is a false-trigger strip beside a hot zone.
#define HOME_HIT_CORNER ((EpdRect){510, 88, 138, 60}) ///< 右上角包络框 / envelope

/// 状态栏信息带（y 0..88）：时钟、网络/蓝牙图标、电量笔。**不属于空白**。
/// 26.11.17 拍板「收窄」：信息区不做手势领地。原因有两条——持机误触集中在屏缘，
/// 而不是屏顶信息区；冲突矩阵草稿的空白本来就从 y=150 起笔，**隐含**排除了顶条。
/// 实现曾经扩到「除热区与 tab 外的全页」，那是越界，补的是这条显式边界。
/// **下沿**与右上角热区**上沿**同高，所以这条带子与热区连成一片，不留缝。
/// 高度不再写字面 88，用 `UI_STATUS_GESTURE_H`（= 条带 80 + 8）派生：条带由 ui_nav 拥有，
/// 这里只是「比它多 8 px」，两边谁改了都会一起动。
/// Status-bar band (y 0..88): clock, network/BT icons, battery. NOT blank. Ruled on
/// 26.11.17 ("narrow"): the info strip is not gesture territory. Grip false-touches
/// cluster at the screen edges, not at the top info strip, and the conflict-matrix draft
/// started its blank area at y=150 - the strip was excluded implicitly. The
/// implementation once covered "everything but hot zones and the tab", which overran.
/// Its bottom edge equals the corner zone's top, so band and hot zone form one block.
#define HOME_RESERVED_INFO ((EpdRect){0, 0, 684, UI_STATUS_GESTURE_H})
#define HOME_HIT_LEFT   ((EpdRect){24, 640, 320, 365}) ///< 左卡（组件浏览）/ left card
#define HOME_HIT_RIGHT  ((EpdRect){354, 640, 294, 365}) ///< 右卡（目标）/ right card

/// 双指序列去重：一条接触序列只允许开一次页面。
/// 长滑会吐出 |steps| = 2、3（同一个事件里的档位大小），而识别器在极少数抖动重判时
/// 可能对同一序列再发一次 FONT_STEP；`fired` 让第二次落空。序列边界是 RETRACT
/// （双指成立瞬间必发一次），所以不存在「永远开不了第二次」。
/// Per-sequence latch: one contact sequence opens the page at most once. A long swipe
/// reports |steps| = 2 or 3 in ONE event; a rare re-judge could emit a second event for
/// the same sequence, and `fired` absorbs it. The boundary is RETRACT (always emitted
/// when two contacts are established), so a later gesture is never locked out.
typedef struct {
    bool fired; ///< 本序列已经触发过 / already fired in this sequence
} home_dual_seq_t;

/// 长按归属 / Long-press route.
typedef enum {
    HOME_LONG_NONE = 0,     ///< 热区但无长按动作，或非主视图 / hot zone without a long action
    HOME_LONG_COMPONENT = 1,///< 左卡：组件浏览 / left card: component browse
    HOME_LONG_TRANSFER = 2, ///< 空白：传书页 / blank: transfer page
} home_long_action_t;

/// 双指上滑的去向 / Where a two-finger swipe ends up.
typedef enum {
    HOME_PUSH_OPEN = 0,     ///< 有内容：打开最新推送 / content ready: open the newest push
    HOME_PUSH_FALLBACK = 1, ///< 落空：送到能推书的地方 / nothing to open: go where one can push
} home_push_route_t;

/// 这一点是不是主视图的空白（不含五块热区、底部 tab 与状态栏信息带）。
/// 非主视图一律 false——空白长按只在主视图启用。
/// Is this point blank on the root view? Hot zones, the bottom tab and the status-bar
/// info band are excluded; any other view answers false (blank long press is root-only).
bool home_blank_hit(int view, int x, int y);

/// 一次双指事件是否要求打开最新推送内容；`seq` 由调用方持有。
/// 语义：RETRACT 只清序列（永不触发）；非主视图忽略；只有 `FONT_STEP && steps < 0`
/// 且本序列未触发过才返回 true。
/// Whether this event asks for the newest push. RETRACT only clears the sequence and
/// never fires; other views are ignored; only FONT_STEP with steps < 0 in an unfired
/// sequence returns true.
bool home_dual_wants_push(int view, home_dual_seq_t *seq, const ui_dual_event_t *ev);

/// 一次长按该走哪条路：热区优先，空白兜底。
/// Which route a long press takes: hot zones first, blank as the fallback.
home_long_action_t home_long_decide(int view, int x, int y);

/// 双指上滑落空时的诚实失败：没有可开的内容就去能推书的地方，绝不静默。
/// Honest failure for an empty inbox: with nothing to open, go where one can push -
/// never answer with silence.
home_push_route_t home_push_route(bool text_ready);
