/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 产品页面的统一接口。每个页面是一个 app_desc_t，放在 main/apps/ 下自己的文件里，
 * 绘制、命中、轮询和页内状态都封在那一个文件中；app_loop.c 只按这套回调调度，
 * 加一个页面不必再改主循环。
 *
 * Shared page contract. Each page is an app_desc_t in its own main/apps/ file.
 * Draw, hit-test, poll and page state stay there. app_loop.c only dispatches
 * these callbacks; adding a page does not touch the loop.
 *
 * 冻结：手势回调替代旧触摸回调；三键接管页须保留强刷与菜单出口。
 * Frozen: Gesture callbacks replace legacy touch callbacks. Pages owning keys
 * must retain full refresh and menu exits.
 * 冻结：返回恢复最近切页来源，菜单来源保留菜单位置。
 * Frozen: Return restores the latest page origin, preserving its menu position.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cst836u.h"
#include "epd_highlevel.h"
#include "epdiy.h"
#include "app_config.h"
#include "sc7a20h_lab.h"

#ifdef __cplusplus
extern "C" {
#endif

/// 回调返回值告诉主循环该怎么把 framebuffer 推上屏。
/// / What the loop should do with the framebuffer.
typedef enum {
    /// 什么都没变，不刷屏。/ Nothing changed.
    APP_REDRAW_NONE = 0,
    /// 只有 area_hint() 那块变了，使用 APP_DYNAMIC_REFRESH_MODE。回调必须自己先画好 fb。
    /// / Push area_hint() with APP_DYNAMIC_REFRESH_MODE. Caller paints fb first.
    APP_REDRAW_AREA,
    /// 由主循环调 render() 重画整页，使用 APP_PAGE_REFRESH_MODE。
    /// / Loop calls render() and presents with APP_PAGE_REFRESH_MODE.
    APP_REDRAW_PAGE,
    /// 同上；APP_PAGE_FORCE_FULL 置 1 时升为整屏 GC16，否则仍走 APP_PAGE_REFRESH_MODE。
    /// / Same; full-screen GC16 when APP_PAGE_FORCE_FULL is 1.
    APP_REDRAW_FULL,
    /// 回调自己已经刷完屏了（自检、刷新测试这类要分步显示进度的页）。
    /// / Page already presented (self-test, refresh bench, …).
    APP_REDRAW_DONE,
} app_redraw_t;

typedef struct app_desc_s app_desc_t;
typedef struct ui_gesture_event_s ui_gesture_event_t;
typedef struct ui_dual_event_s ui_dual_event_t;

typedef struct {
    EpdiyHighlevelState* hl;
    uint8_t* fb;
    sc7a20h_handle_t acc;
    cst836u_handle_t tp;
    bool sensor_ready;
    bool continuous_ready;
    int64_t now_ms;
    /// 去抖后的触摸快照：空槽已清零，抬手后保留最后一次坐标。
    /// / Debounced touch: empty slots cleared; last coords kept after lift.
    const cst836u_touch_t* touch;
    const cst836u_info_t* touch_info;
    /// 本轮是不是按下沿 / 抬起沿，触摸页的跟随节流要用。
    /// / Press/release edge this tick. Touch tracking uses this to throttle.
    bool pressed;
    bool released;
    /// 分页页（字体列表）当前停在第几页，由 KEY1 和页内按钮共同改。
    /// / Leaf page index (font list). KEY1 and in-page buttons both change it.
    int leaf;
    /// 本轮已经被别的分支处理掉了，页内的跟随逻辑要跳过这一轮。
    /// / Another branch already handled this tick; skip in-page tracking.
    bool consumed;
    /// **26.9.13 · 单指优先策略位**（由主循环每拍从 `app_desc_t::single_finger_priority`
    /// 下发，两个输入层都只读这一份）。为真时同时约束两件事：
    ///   ① `ui_dual_engaged()` 恒假——双指态不得再饿死单指通道；
    ///   ② `ui_gesture_feed()` 遇 `count > 1` **不再**取消进行中的序列（TAP 照常成立）。
    /// 一个意图（"这一页靠精确点按吃饭"）一个位；26.9.12 只接了①的一半，②仍是历史
    /// 行为，于是两次点按照样会被一帧幻点吃掉。
    /// **26.9.13 - single-finger priority**, applied every tick by the loop from
    /// `app_desc_t::single_finger_priority`; both input layers read this one copy. True
    /// constrains two things at once: (1) `ui_dual_engaged()` is always false, so a dual
    /// session cannot starve the single path; (2) `ui_gesture_feed()` stops cancelling an
    /// in-flight sequence on `count > 1`, so the TAP still lands. One intent ("this page
    /// lives on precise taps"), one bit - 26.9.12 wired only half of (1) and left (2) at
    /// its historical behaviour, so two taps were still eaten by a single phantom frame.
    bool single_finger_priority;
    /// 页面识别到输入时置位，主循环消费后清除，避免晃动翻页被当成空闲。
    /// Set for recognized input; the loop consumes and clears it so shake turns reset idle time.
    bool user_activity;
    /// 页面想跳到别的页时填这里，主循环会在本轮末尾切过去。
    /// / Request a page switch; the loop applies it at the end of the tick.
    const struct app_desc_s* request_app;
    /// 本轮末打开菜单；切页请求优先。/ Open the menu at tick end; page requests take priority.
    bool request_menu;
    /// 返回最近切页来源；菜单来源直接恢复菜单，无历史打开当前所属菜单；优先于其它请求。
    /// Return to the latest page origin and its menu; no history opens the current menu leaf. Overrides other requests.
    bool request_return;
} app_ctx_t;

struct app_desc_s {
    /// 菜单里显示的标题与副标题。/ Menu title and subtitle.
    const char* title;
    const char* detail;
    /// true = 这一页会长时间独占 PMU，主循环的锁屏按键轮询要让路（设备功能自检）。
    /// / true = page owns the PMU; lock-key poll yields (device self-test).
    bool holds_pmu;
    /// true = 进页走 APP_REDRAW_FULL（均衡配置下整屏 GC16）。内容和上一页差别大的页面用，避免差分刷留边。
    /// / true = enter with APP_REDRAW_FULL so a differential update does not leave a seam.
    bool enter_full;
    /// 菜单关闭时接管三键，页面必须提供强刷和菜单入口。
    /// Own all three keys outside the menu; provide full refresh and menu exits.
    bool owns_keys;
    /// 中键短按在抬起时执行，便于长按触发不同动作。/ Fire middle-key short action on release so a hold can differ.
    bool defer_middle_short;
    /// 可选：按当前页面决定右下角菜单把手是否响应。/ Decide whether the bottom-right menu handle is active.
    bool (*menu_handle_enabled)(app_ctx_t* ctx);
    /// 可选：当前确实显示主页面，允许本地测试优化底栏切页；阅读/子页面返回 false。
    /// Optional: true only for a main-tab view eligible for local transition testing; reader/subviews return false.
    bool (*main_page_visible)(app_ctx_t* ctx);
    /// 可选：顶部状态栏当前是否在屏上，返回该区域的底色（0xFF/0xE0/0xF0）；
    /// 返回负数表示不可见或本页不参与电量差刷。判定在主循环一处，本回调只回答
    /// 「这一页有没有那条栏、底色是什么」——两件事都不该由主循环猜。
    /// Optional: is the status bar on screen, and what is its background colour
    /// (0xFF/0xE0/0xF0)? Negative = not visible / this page opts out. The gate lives
    /// in one place in the loop; this callback only answers "is the bar there, on
    /// what colour", neither of which the loop should guess.
    int (*status_bar_bg)(app_ctx_t* ctx);
    /// 可选：这一页**自己有**分钟节拍（首页的时钟 tick、阅读器的整条 tick）。
    /// true = 主循环的 nav_watch 不管分钟，只做电量差刷（否则同一带两条节拍重叠）。
    /// 缺省 false = 没有自己的节拍，nav_watch 负责分钟；新页面不写这一行就自动获得分钟更新。
    /// Optional: this page runs its own minute cadence (home clock tick, reader strip tick).
    /// true = nav_watch leaves the clock alone and only does the battery diff, otherwise two
    /// cadences overlap on one strip. Default false = no cadence of its own, nav_watch owns
    /// the minute, so a new page gets minute updates without opting in.
    bool self_clock_tick;
    /// 进页/离页。on_enter 里做上电、唤醒传感器、拉一次数据这类副作用。
    /// / Enter/exit. on_enter powers up, wakes sensors, takes a first sample.
    void (*on_enter)(app_ctx_t* ctx);
    void (*on_exit)(app_ctx_t* ctx);
    /// TF 挂载失效：停止并等待后台任务、关闭文件；主循环随后回退字体并重绘，菜单打开时也通知。
    /// On TF mount loss, join background work and close files before font fallback/redraw; also called behind menus.
    void (*on_media_lost)(app_ctx_t* ctx);
    /// TF 首次挂载或显式重新挂载后，页面按需更新目录与封面缓存。
    /// Refresh page-owned listings or cover caches after a first or explicit TF mount.
    void (*on_media_ready)(app_ctx_t* ctx);
    /// 锁屏前保存页面的阅读状态。/ Persist page state before lock rendering.
    void (*on_before_lock)(app_ctx_t* ctx);
    /// 可选：消费电源键短按；返回 NONE 时沿用锁屏，DONE 表示已消费但无需刷新。
    /// Optional: consume a short power press; NONE keeps locking, DONE consumes without redraw.
    app_redraw_t (*on_power_short)(app_ctx_t* ctx);
    /// 可选：控制中心点了【框选】。返回非 NONE 时主循环立刻按该模式重绘；
    /// 页面自行决定能不能进（非阅读态、竖排时返回 NONE，气泡菜单照常收起）。
    /// Optional: the quick layer's box-select button was tapped. A non-NONE result repaints
    /// right away; the page decides whether the mode applies (it returns NONE outside the
    /// reader or in the vertical setting) while the menu still closes.
    app_redraw_t (*on_quick_box)(app_ctx_t* ctx);
    /// 纯绘制，不做 I2C 写、蜂鸣这类副作用，整屏强刷才能安全复用它。
    /// / Paint only. No I2C writes or buzzer. KEY2 full redraw reuses this.
    void (*render)(app_ctx_t* ctx, uint8_t* fb);
    /// 自定义推屏。返回 true 表示页面已经刷过。
    /// / Custom present. Return true if the page updated the display.
    bool (*present)(app_ctx_t* ctx, app_redraw_t redraw);
    /// 设置后独占触摸派发，替代 on_touch。/ Exclusive gesture dispatch replaces on_touch.
    app_redraw_t (*on_gesture)(app_ctx_t* ctx, const ui_gesture_event_t* event);
    /// M4-A：可选的双指意图回调（横滑切横竖屏 / 竖滑字号 ±档 / 静默撤销）。
    /// 与 on_gesture 同构但互不干扰——双指层只接管 count==2，单指路径一行不改。
    /// M4-A: optional two-finger intent callback (horizontal swipe toggles
    /// orientation, vertical swipe steps the font, retract closes overlays).
    /// Shaped like on_gesture yet independent: the dual layer owns count==2 only
    /// and never touches the single-finger path.
    app_redraw_t (*on_dual_gesture)(app_ctx_t* ctx, const ui_dual_event_t* event);
    /// M5-A / 26.9.13：可选。本页此刻是否要求**单指优先**（框选态返回 true）。
    /// 一个位统一约束「双指不饿死单指」与「多点不取消 TAP」两件事——它们本是同一个
    /// 意图。策略只在主循环里下发一次（写进 `app_ctx_t::single_finger_priority`），
    /// 双指层与单指识别器都读那一份，谁都不许再自己判一遍。
    /// M5-A / 26.9.13: optional. Whether this page wants **single-finger priority** right
    /// now (the box-selection mode says true). One bit governs both "the dual layer must
    /// not starve the single finger" and "multitouch must not cancel a TAP" - they were
    /// always one intent. The loop applies it once per tick into
    /// `app_ctx_t::single_finger_priority`; both layers read that copy and neither is
    /// allowed to judge it again.
    bool (*single_finger_priority)(app_ctx_t* ctx);
    app_redraw_t (*on_touch)(app_ctx_t* ctx, const cst836u_touch_t* touch);
    /// 默认只收 UI_KEY_1；owns_keys 页面在菜单外接收三键。
    /// / Only KEY1 normally; owns_keys pages receive all three keys outside the menu.
    app_redraw_t (*on_key)(app_ctx_t* ctx, int key);
    /// 可选：owns_keys 页面同键持续按住500ms触发一次；defer_middle_short 时中键短按只在抬起执行。
    /// Optional: fire after a 500ms hold; with defer_middle_short, the middle short action runs only on release.
    app_redraw_t (*on_key_long)(app_ctx_t* ctx, int key);
    /// 每轮都调，页面自己判断到不到刷新间隔。
    /// / Called every tick; the page decides if its interval has elapsed.
    app_redraw_t (*on_tick)(app_ctx_t* ctx);
    /// APP_REDRAW_AREA 要刷的区域。/ Area for APP_REDRAW_AREA.
    EpdRect (*area_hint)(app_ctx_t* ctx);
    /// 同一份实现挂多个 app 时用来区分（加速度计的读数页与诊断页共享绘制代码）。
    /// / Distinguishes views that share one implementation (IMU live vs lab).
    void* user;
};

// 锁屏：浅睡按键或拿起回原页；软睡 EN=0，再短按开机；关机 EN=0，长按开机。
// Lock: light-sleep key or pickup returns to the page; soft sleep drops EN
// and a short press powers on; power-off drops EN and needs a long press.
#define APP_LOCK_POLL_MS 200
#define APP_LOCK_IGNORE_BOOT_MS 2000
#define APP_LOCK_IGNORE_SELFTEST_MS 2000

/// 按 redraw 把 framebuffer 推上屏，顺带做 pclk 欠载兜底。APP_REDRAW_DONE 直接返回。
/// / Present fb for redraw, with a pclk-underrun fallback. APP_REDRAW_DONE returns.
void app_present(app_ctx_t* ctx, const app_desc_t* app, app_redraw_t redraw);
/// 之后 ms 毫秒内忽略电源键短按，避免开机或自检的残留事件立刻把屏锁掉。
/// / Ignore short power-key presses for ms so boot/self-test leftovers do not lock.
void app_lock_ignore_for(int64_t ms);

#ifdef __cplusplus
}
#endif

/// Yield a long main-page load when another bottom tab is pressed. No display or lifecycle calls.
bool app_poll_main_navigation(app_ctx_t *ctx);

/// M 批 D/C 层共用的导航采样插点来源：一个原语三处调用，计数按来源分开记。
/// / M-batch D/C shared hook sources: one primitive, three call sites, counted per source.
typedef enum {
    APP_NAV_SRC_BATCH = 0,   ///< D 层：书架封面批次 / 汇合 / 预热（app_book）
    APP_NAV_SRC_PRESENT,     ///< C② 层：app_present 段间（Home/Books/Settings 出口）
    APP_NAV_SRC_FILES,       ///< C① 层：Files 同步目录扫描（count_root / scan_folder）
    APP_NAV_SRC_COUNT
} app_nav_src_t;

/// M 批 D+C 共用插点原语：INT 电平门控 → I2C 采样 → 自建边沿 → 底栏命中 → 仲裁 on_hit。
/// 返回 true = 本次采到一条新的底栏命中（已折叠进仲裁单槽，尚未写 request_app；
/// 执行由主循环尾部收敛一次性完成）。边沿、电平门控、index==1 屏蔽三件事只在本函数
/// 里判一次，D/C 各落点只负责「什么时候问一句」，不再各写一份。
/// / One hook for D+C: level gate, sample, self-built edge, hit test, arbiter fold.
/// True = a fresh bottom-bar hit was folded into the arbiter slot (commit happens in the
/// loop tail); callers only decide when to ask, never how to judge.
bool app_nav_probe(app_ctx_t *ctx, app_nav_src_t src);
