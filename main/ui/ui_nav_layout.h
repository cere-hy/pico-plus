/* SPDX-License-Identifier: Apache-2.0
 * 中文：轻量底栏边界，绘制和刷新共用，防止局部扫描碰到固定图标。
 * English: Lightweight navigation bounds shared by painting and presentation to protect fixed icons.
 */
#pragma once
#define UI_NAV_TOP 1096
// 状态栏条带：绘制与「整条重绘」共用的高度。内容只占 y≈18..60，80 是条带（含内边距）。
// 属主是 ui_nav；放在这个轻量头里，是因为手势判定（home_gesture.h）也要用，
// 而 ui_nav.h 依赖 app.h，拖进来会把判定模块压上整个 app 依赖。
// Status strip height, shared by painting and whole-strip repaint. Content occupies
// y≈18..60; 80 is the strip including padding. Owned by ui_nav, placed in this light
// header because the gesture decider needs it too and ui_nav.h pulls in app.h.
#define UI_STATUS_STRIP_H 80
// 手势领地 = 条带 + 8 px。多出的 8 px 是条带下沿到右上角热区上沿之间的空白，
// 属于信息区（可以什么都没有，但不许长按呼出传书页）。派生定义让「领地 ⊇ 条带」
// 由构造保证，不靠两个人各自记得。
// Gesture territory = strip + 8. The extra 8 px is the gap between the strip's bottom
// edge and the top-right hot zone: still info area, never a long-press target. Deriving
// it makes "territory ⊇ strip" true by construction instead of by memory.
#define UI_STATUS_GESTURE_H (UI_STATUS_STRIP_H + 8)

// 只扩大触控热区，不移动图标或改变内容/底栏的刷新边界。
// Expand touch admission only; icons and content/navigation refresh boundaries stay fixed.
#define UI_NAV_TOUCH_TOP (UI_NAV_TOP - 16)
#define UI_NAV_MARKER_TOP (UI_NAV_TOP + 1)
#define UI_NAV_MARKER_BOTTOM (UI_NAV_TOP + 6)
// 内容向内收至32列边界；横条单独短刷或并入快刷，外扩范围均止于图标首笔之前。
// Round content inward to 32 columns; marker-only or fused fast scans end before any icon ink.
#define UI_NAV_REFRESH_END (UI_NAV_TOP & ~31)
#define UI_NAV_MARKER_SCAN_END ((UI_NAV_MARKER_BOTTOM + 31) & ~31)
