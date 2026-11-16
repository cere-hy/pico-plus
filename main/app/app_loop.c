/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 事件循环。只做和页面无关的事：读触摸并去抖、三个实体键与菜单把手的路由、切页
 * 时调 on_exit/on_enter、按回调返回值决定怎么刷屏、电源键锁屏、SD 卡上的字体延迟
 * 加载。页面自己的状态和刷新节奏都在 main/apps/ 各自的文件里。
 *
 * Event loop for page-agnostic work: touch debounce, the three keys and
 * the menu handle, on_exit/on_enter, present, lock, delayed SD font load.
 * Page state and refresh cadence stay in main/apps/; gesture recognition is shared.
 *
 * 冻结：主循环只产出手势、不解释含义；产品系统页接管三键，按用户要求
 * 左右返回、中键首页；阅读页保留翻页、页面设置和长按回书架。把手仍按下即触发。
 * 接管页可选长按键回调，保留按下行为；同键单指500ms仅触发一次，中断取消。
 * Frozen: Produce gestures without interpreting page actions. Product system pages
 * own Back/Home/Back keys; reading retains page turns, settings and hold-to-shelf.
 * Owners may opt into a single 500ms same-key hold callback after the press action; interruptions cancel it.
 * The menu handle still fires on press.
 * 用户修订：四个主页面底栏作为统一导航路由在按下时报点即响应，模态和阅读手势保持原归属。
 * User revision: root-tab routing commits on initial contact; modal controls and reader gestures retain their owners.
 * 用户修订：系统页面切换使用 GL16，不再每次 GC16 全刷；锁屏、手动强刷和周期清残影仍可全刷。
 * User revision: system page changes use GL16 instead of GC16; lock, manual refresh and periodic ghost cleanup may still use full refresh.
 * 用户修订：设置可选普通/快刷/水波纹，默认普通；普通保留原灰阶路线，快刷采用确认的黑白主页面和亚克力灰阶。用户反馈残影和黑带后，书架退出只同向擦白，主页水波纹改白底/24窄带且端点单向推动，中灰保留校准序列；五行横条局部移动、图标固定，没有异步补刷。
 * User revision: settings offer ordinary/fast/water, default ordinary. Ordinary retains the original gray path; fast uses approved monochrome main screens and gray acrylic. Following reported ghosts and dark bands, shelf exit whitens directionally; main water uses white backgrounds, 24 narrow bands and directional endpoints, retaining calibrated midgrays. Move the five-row marker locally, retain icons, and schedule no asynchronous completion.
 * 用户修订：普通模式也固定主页面底栏，仅局部移动横条；快刷亚克力采用规则一像素棋盘格，与已缓存封面一起输出。普通及水波纹保留灰阶，首次进入及解锁仍完整绘制底栏。
 * User revision: ordinary main screens also retain navigation and move only its marker locally; fast acrylic uses a regular one-pixel checkerboard in the same output as cached covers. Ordinary/water retain grays; initial entry and unlock still draw complete navigation.
 * 冻结：返回消费最近切页来源，恢复原页及菜单位置，不跳固定首页。
 * Frozen: Return consumes the latest page origin, restoring its page and menu position.
 * 用户采用顶部快捷层：仅顶部32px起手下滑72px才接管，覆盖时冻结页内输入/轮询，WiFi直接连接最近保存网络/再次断开，蓝牙切换开关，全刷和锁屏复用原路径。用户要求三个模式均真实灰阶并加深轮廓；关闭重绘当前页并局部整理，不修改底栏。
 * User adopts the top quick layer: capture only a 72px downward swipe starting within the top 32px; suspend page input/ticks while covered. WiFi connects saved credentials or disconnects on the next tap; Bluetooth toggles its setting. Reuse full refresh and lock. Per the user, all modes retain grays with clearer outlines. Closing redraws and locally settles the underlay, retaining navigation.
 * 用户点击反馈：底栏及圆形返回键按下局部放大，释放复位；只暂存一个小控件，切页、全刷及锁屏丢弃缓存，不回放旧画面。
 * User click feedback: locally enlarge navigation/round backs on press and restore on release. Retain one small control only; navigation, full refresh and lock discard it without replaying old frames.
 * 用户修订：确认主动锁屏后，拼贴样式先给局部圆锁反馈，再保存/等待阅读预渲染；自动休眠和直接关机/重启不加此反馈。
 * User revision: acknowledge accepted manual collage locks before saves/reader joins; omit the badge for automatic sleep and direct shutdown/restart.
 */

#include "app_loop.h"
#include "auto_lock.h"
#include "crash_note.h"
// 26.11.19 解耦批：采样支与按键支共用一份总线门谓词。
// 26.11.19 decoupling: one bus-gate predicate shared by sampling and key handling.
#include "pmu_bus.h"

#include <string.h>
#include <stdio.h>

#include "app_registry.h"
#include "app_content_open.h"
#include "app_transfer_mode.h"
#include "boot_state.h"
#include "ble_page_turner.h"
#include "push_sink.h"
#include "ota_online.h"
#include "continuous_du.h"
#include "dict/vocab_log.h"
#include "display.h"
#include "e0470_epaper_waveform.h"
#include "esp_app_desc.h" // 26.9.7：每行日志的版本短码（esp_app_get_description）
#include "esp_attr.h"      // 26.11.3-C2：EXT_RAM_BSS_ATTR（段落接收槽落 PSRAM）
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "read_pico_board.h"
#include "read_pico_crashlog.h"
#include "read_pico_pmu.h"
#include "read_pico_transfer.h"
#include "read_pico_sd.h"
#include "settings.h"
#include "sleep.h"
#include "time_service.h"
#include "ui_avatar.h"
#include "book/book_cover_jobs.h"
#include "book/book_lock_collage.h"
#include "book/excerpt_store.h"
#include "ttf_font.h"
#include "app_font_context.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include "ui_nav.h"
#include "nav_status_watch.h"
#include "ui_gesture.h"
#include "ui_dualgesture.h"
#include "ui_power_dialog.h"
#include "ui_quick_menu.h"
#include "ui_click_feedback.h"
#include "usb_storage.h"
#include "local_latency_probe.h"
#include "nav_arbiter.h"  // M 批：导航仲裁「只认最后一指」纯逻辑，产品码与宿主守卫编同一份
#include "esp_err.h"      // 26.9.16 R3：sd: 状态行要打错误码名字
#include "pico_bootbb.h"  // 26.9.16 R2：把 SD 的每次尝试搬出 0~4 s 的失明窗口

#define TAG "app_loop"

// 触摸芯片报点率远高于屏幕，主循环空转一轮只等这么久。
// Touch reports far faster than the panel; idle this long per loop.
#define LOOP_TICK_MS 5
// SD 卡插上或者刚挂好时，隔一会儿再试一次存在设置里的字体。
// Retry the saved SD font shortly after a card appears or mounts.
#define FONT_RETRY_INTERVAL_MS 750
#define MEDIA_POLL_INTERVAL_MS 500

static int64_t s_lock_ignore_until_ms;
extern const app_desc_t app_transfer;

static const app_desc_t* s_presented_main;
static ui_quick_menu_t s_quick;
// M 批导航仲裁状态：折叠窗口标志 + 单槽最后一指 + 合并/丢弃/执行计数（几十字节级）。
// 不建队列：唯一的导航意图家仍是 ctx.request_app；本状态只在窗口内折叠边沿。
static nav_arbiter_t s_nav_arb;
static unsigned s_quick_status;
// 锁屏占用整屏后旧主页面底栏不再是物理基准；解锁第一帧必须完整绘制。
// A lock screen replaces the old navigation baseline; the first unlocked frame must render complete navigation.
static void main_lock_boundary(void) {
    ui_click_feedback_reset();
    ui_quick_menu_reset(&s_quick);
    s_presented_main = NULL;
    display_main_transition_cancel();
}

static unsigned quick_status(void) {
    read_pico_transfer_status_t status;
    read_pico_transfer_get_status(&status);
    bool wifi = status.mode == READ_PICO_TRANSFER_MODE_STA && status.state != READ_PICO_TRANSFER_STOPPED;
    return (wifi ? 1u : 0u) | (app_settings_ble_turner() ? 2u : 0u) | (status.network_ready ? 4u : 0u);
}

// 同步输出完再接收下一条触摸，不安排旧层回放；错误时关闭层并可靠全刷当前页。
// Complete presentation before accepting another contact, with no stale replay; close and full-refresh the current page on error.
static void quick_present(app_ctx_t *ctx, const app_desc_t *current, bool full) {
    ui_click_feedback_begin(ctx->fb);
    display_main_transition_cancel();
    if (current->render) current->render(ctx, ctx->fb);
    s_quick_status = quick_status();
    if (s_quick.open) ui_quick_menu_draw(ctx->fb, s_quick_status & 1u, s_quick_status & 2u);
    enum EpdDrawError error = full ? update_display_full(ctx->hl)
        : update_display_area_full_with(ctx->hl, &E0470_WAVEFORM, MODE_GL16, ui_quick_menu_rect());
    guard_draw_result(ctx->hl, error);
    if (error != EPD_DRAW_SUCCESS) {
        ui_quick_menu_reset(&s_quick);
        app_present(ctx, current, APP_REDRAW_FULL);
    }
}

static void quick_toggle_wifi(void) {
    read_pico_transfer_status_t status;
    read_pico_transfer_get_status(&status);
    if (status.state != READ_PICO_TRANSFER_STOPPED) {
        // 上传期间拒绝断开，复用存储任务已有的原子准入保护。
        // Refuse disconnecting during upload using the storage owner's atomic admission guard.
        (void)read_pico_transfer_try_stop_if_idle();
    } else {
        char ssid[33] = {0}; bool configured = false;
        if (read_pico_transfer_get_saved_wifi(ssid, &configured) == ESP_OK && configured) {
            // gen18：武装期不释放蓝牙占用——与息屏同款豁免（覆盖表第 1 行）。
            // ESP32 上 WiFi 与 BLE 可共存，跳过 ble_pt_sync(false) 让推送广播持续；
            // 栈由主循环 ble_pt_sync((armed||turner)&&!wifi_busy&&!flight) 继续保活。
            // 此前无条件 ble_pt_sync(false) 是漏网的武装期拆栈入口（覆盖表修正项）。
            // gen18: do not release Bluetooth while armed - the same exemption as
            // screen-off (coverage-table row 1). WiFi and BLE coexist on the ESP32,
            // so skip ble_pt_sync(false) and keep the push advertisement up; the
            // loop's ble_pt_sync((armed||turner)&&!wifi_busy&&!flight) keeps the
            // stack. An unconditional ble_pt_sync(false) here was a missed armed
            // teardown entry (coverage-table correction).
            if (!push_sink_armed()) ble_pt_sync(false);
            else ESP_LOGI(TAG, "wifi toggle skipped ble stop: push sink armed");
            const read_pico_transfer_cfg_t cfg = {.mode = READ_PICO_TRANSFER_MODE_STA, .network_only = true};
            (void)read_pico_transfer_start(&cfg);
        }
    }
}
static bool main_page_ready(app_ctx_t* ctx, const app_desc_t* app);

static void main_present_finished(app_ctx_t* ctx, const app_desc_t* app, app_redraw_t redraw) {
    if (redraw == APP_REDRAW_DONE || !main_page_ready(ctx, app)) s_presented_main = NULL;
    else if (redraw != APP_REDRAW_NONE) s_presented_main = app;
    display_main_transition_disarm();
}

void app_lock_ignore_for(int64_t ms) {
    s_lock_ignore_until_ms = esp_timer_get_time() / 1000 + ms;
}

// ---- M 批 C② 层：app_present 段间采样闸门 ----
// 刷屏是同步阻塞的（render + 推屏几百毫秒），主循环在这段里停转、不读触摸；落在段间的
// 点击只有在「段与段之间」还有机会被采到，这就是本插点的全部意义。闸门只问三件既有状态：
// 已经排着一个意图就不再叠；USB 占用 TF 时禁止任何切页；只有四个主 tab 的页面才是底栏
// 命中的合法宿主。
// 菜单打开时 app_present 一处都不进——所有调用点都是 `if (menu_open) present_menu(...)
// else app_present(...)`，而 present_menu 自己 ui_draw_menu_page + 推屏、不经过本函数。
// 所以这里不需要、也不该再问一遍 menu_open：那会把主循环的局部量复制出第二个家。
// C② gate between present segments. Presentation blocks the loop for hundreds of ms, so a
// tap that lands mid-segment can only be caught at a segment boundary. The gate reads only
// existing state; menu-open needs no extra question because app_present is never reached
// while the menu owns the screen (every caller takes the present_menu branch).
static void nav_probe_present(app_ctx_t *ctx, const app_desc_t *app) {
    if (ctx == NULL || app == NULL) return;
    if (ctx->request_app || usb_storage_active()) return;
    if (!main_page_ready(ctx, app)) return;
    (void)app_nav_probe(ctx, APP_NAV_SRC_PRESENT);
}

void app_present(app_ctx_t* ctx, const app_desc_t* app, app_redraw_t redraw) {
#ifdef KIIKO_LOCAL_LATENCY
    const int64_t probe_start=esp_timer_get_time();
    extern int64_t local_display_elapsed_us;
    const int64_t probe_display=local_display_elapsed_us;
#define LATENCY_PRESENT_DONE() do { if(redraw==APP_REDRAW_PAGE||redraw==APP_REDRAW_FULL) \
    ESP_LOGI("latency","PRESENT page=%d redraw=%d total_us=%lld display_us=%lld paint_us=%lld", \
    app_index_of(app),redraw,(long long)(esp_timer_get_time()-probe_start), \
    (long long)(local_display_elapsed_us-probe_display), \
    (long long)(esp_timer_get_time()-probe_start-(local_display_elapsed_us-probe_display))); } while(0)
#else
#define LATENCY_PRESENT_DONE() do {} while(0)
#endif
    enum EpdDrawError result = EPD_DRAW_SUCCESS;
    // C② 段间插点 ①：进段前采一次（上一段刚结束，主循环还没转回来）。
    // C② probe 1: on entry - the previous segment just finished and the loop has not spun.
    nav_probe_present(ctx, app);
    if (redraw == APP_REDRAW_PAGE && ui_click_feedback_active()) {
        // 切页固定底栏之前先复位真实按压像素，避免把放大的图标当作长期基准。
        // Restore physical pressed pixels before freezing navigation, so an enlarged icon never becomes its baseline.
        EpdRect area;
        if (ui_click_feedback_release(ctx->fb, &area)) {
            display_main_transition_cancel();
            result = update_display_area_diff_with(ctx->hl, &E0470_FOLLOW_WAVEFORM, MODE_DU, area);
            guard_draw_result(ctx->hl, result);
            if (result != EPD_DRAW_SUCCESS) redraw = APP_REDRAW_FULL;
        }
    }
    if (redraw == APP_REDRAW_PAGE || redraw == APP_REDRAW_FULL) ui_click_feedback_begin(ctx->fb);
    ui_text_set_system_scale(true);
    // 后台目录/封面完成及滚动仍属于当前主页面，不能借普通整页出口闪到底栏。
    // Background directory/cover completion and scrolling remain on this main page and must not flash navigation via ordinary page output.
    if ((redraw == APP_REDRAW_PAGE || redraw == APP_REDRAW_AREA) &&
        s_presented_main == app && main_page_ready(ctx, app)) display_main_transition_arm(app, false, false);
    if (app->present != NULL && app->present(ctx, redraw)) {
        // C② 段间插点 ②：页面自带推屏（app_book 走这条），出段再采一次。
        // C② probe 2: the page pushed its own frame; sample again on the way out.
        nav_probe_present(ctx, app);
        main_present_finished(ctx, app, redraw);
        LATENCY_PRESENT_DONE();
        return;
    }
    // C② 段间插点 ③：present 回调已画出 fb、还没推屏，这是最长的一段空窗。
    // C② probe 3: fb is painted but not yet pushed - the widest blind window.
    nav_probe_present(ctx, app);
    switch (redraw) {
        case APP_REDRAW_NONE:
        case APP_REDRAW_DONE:
            main_present_finished(ctx, app, redraw);
            return;
        case APP_REDRAW_AREA: {
            // 回调已经画好了 fb，这里只负责把它那一块推上屏。
            // The callback already painted fb; push that rectangle only.
            EpdRect area = app->area_hint != NULL
                ? app->area_hint(ctx)
                : ui_content_refresh_area();
            result = update_display_area_with(
                ctx->hl, &E0470_WAVEFORM, APP_DYNAMIC_REFRESH_MODE, area
            );
            break;
        }
        case APP_REDRAW_FULL:
            if (app->render != NULL) app->render(ctx, ctx->fb);
            result = APP_PAGE_FORCE_FULL
                ? update_display_full(ctx->hl)
                : update_display_mode(ctx->hl, APP_PAGE_REFRESH_MODE);
            break;
        case APP_REDRAW_PAGE:
        default:
            if (app->render != NULL) app->render(ctx, ctx->fb);
            result = update_display_fast_page(ctx->hl);
            break;
    }
    guard_draw_result(ctx->hl, result);
    // C② 段间插点 ④：推屏返回后、交还主循环前采一次（推屏这一段整体没有触摸采样）。
    // C② probe 4: after the panel push, before handing back to the loop.
    nav_probe_present(ctx, app);
    main_present_finished(ctx, app, redraw);
    LATENCY_PRESENT_DONE();
#undef LATENCY_PRESENT_DONE
}

// ---- 26.11.16 电量修复批：状态栏电量区的画闸门 ----
// 判定只有这一处，页面用 status_bar_bg 回答「有没有那条栏、底色多少」；逐页实现的是
// 底色，不是判据。值链不动——快照本来就每 200 ms 新一次（按键通道顺手 poll），
// 缺的只是「值变了要不要重画」。
// One gate, here. Pages answer only "is the bar there / what colour"; the decision is
// not per-page. The value chain is untouched: the snapshot is already fresh every
// 200 ms, what was missing is the repaint decision.
#define NAV_WATCH_TICK_MS 200
// 3 位百分比（100% 左缘约 x=527）到电池笔帽（x=653）;纵向含 28 px 数字与 22 px 电池。
// Three-digit percentage (100% starts near x=527) through the battery cap (x=653).
#define NAV_BATTERY_X 520
#define NAV_BATTERY_Y 24
#define NAV_BATTERY_W 136
#define NAV_BATTERY_H 32

static nav_watch_t s_nav_watch;
static uint32_t s_nav_watch_next_ms;

static void nav_battery_watch(app_ctx_t *ctx, const app_desc_t *app, bool overlay) {
    const uint32_t now = (uint32_t)ctx->now_ms;
    if ((uint32_t)(now - s_nav_watch_next_ms) < NAV_WATCH_TICK_MS) return;
    s_nav_watch_next_ms = now;

    // 覆盖层 / 触摸 / 已被消费的一拍：画面所有权都不在状态栏，别插一脚。
    // Overlay, active touch or an already-consumed tick: the frame is not the bar's.
    if (overlay || ctx->consumed || ctx->pressed || ctx->released ||
        (ctx->touch != NULL && ctx->touch->touched)) return;

    const pmu_snapshot_t *pmu = read_pico_pmu_get();
    const int percent = pmu_battery_percent(pmu);
    const bool charging = pmu_battery_charging(pmu);
    const int bg = app->status_bar_bg != NULL ? app->status_bar_bg(ctx) : -1;
    // 分钟源：页面自己有节拍的（首页时钟 tick、阅读器整条 tick）一律缺席——否则同一带
    // 两条节拍重叠。不可见时也不取时间，省掉一次无用的校时查询。
    // Minute source: absent on pages that run their own cadence (home clock tick, reader
    // strip tick), otherwise two cadences overlap on one strip.
    const int64_t minute = (app->self_clock_tick || bg < 0) ? NAV_WATCH_NO_MINUTE
                                                            : ui_nav_clock_minute();

    const nav_watch_action_t action =
        nav_watch_step(&s_nav_watch, percent, charging, minute, bg >= 0, now);
    if (action == NAV_WATCH_IDLE) return;

    // 矩形跟触发源走：分钟变只刷整条，电量变只刷那一小块；同拍双变由整条一次吞掉。
    // Rect follows the trigger: minute -> whole strip, battery -> the small rect. A
    // same-tick double change collapses into the strip pass (it contains the battery rect).
    const EpdRect area = action == NAV_WATCH_REPAINT_STRIP
        ? (EpdRect){0, 0, UI_LOCK_WIDTH, UI_STATUS_STRIP_H}
        : (EpdRect){NAV_BATTERY_X, NAV_BATTERY_Y, NAV_BATTERY_W, NAV_BATTERY_H};
    ui_fill_rect_fast(area, (uint8_t)bg, ctx->fb);
    ui_nav_status(ctx->fb);
    const enum EpdDrawError err =
        update_display_area_diff_with(ctx->hl, &E0470_WAVEFORM, MODE_GL16, area);
    guard_draw_result(ctx->hl, err);
    // 只有真正上屏成功才落定「上次上屏值」；失败保持旧值，下一拍重试。
    // 三个源一起提交：触发源之外的也得记，否则下一拍会为它再刷一次。
    // Commit only after a successful push; on failure keep the old values and retry.
    // All three sources are committed, not just the trigger, or the next tick repaints again.
    if (err == EPD_DRAW_SUCCESS) nav_watch_commit(&s_nav_watch, percent, charging, minute);
}

// 反馈只推控件附近，失败后重新绘制当前页面；不占用底栏切页许可。
// Push feedback only near its control, repainting the current page on failure without consuming navigation permission.
static void click_present(app_ctx_t *ctx, const app_desc_t *current, EpdRect area) {
    display_main_transition_cancel();
    enum EpdDrawError error = update_display_area_diff_with(ctx->hl, &E0470_FOLLOW_WAVEFORM, MODE_DU, area);
    guard_draw_result(ctx->hl, error);
    if (error != EPD_DRAW_SUCCESS) app_present(ctx, current, APP_REDRAW_FULL);
}

// 固件身份短码前缀（26.9.7，审计裁定⑤）。26.9.6 的做法是启动时播一次完整版本串，
// 但本轮那句正好落在最脏的启动突发里，丢了——「一次性广播」天生有窗口期，丢了就是
// 丢了，事后无从补救。改成**每一行关键日志自带身份**：`[97] dual: ...`，任意一行都
// 能独立回答「这是哪一版在跑」，永不丢失。
// 短码由 `esp_app_get_description()->version`（即 CMakeLists 的 PROJECT_VER）在运行期
// 解析出来，不另设宏——两处各写一份必然漂移，而漂移的版本号比没有版本号更坏。
// 解析失败打 `[??]`：身份不明就要**看起来不明**，不许静默降级成某个默认值。
// Firmware identity as a per-line prefix (26.9.7, adjudication 5). 26.9.6 broadcast the
// full version once at boot, and that one line landed inside the dirtiest burst of this
// round and was lost - a one-shot broadcast has a window, and a missed window cannot be
// reconstructed afterwards. Now every key line carries its own identity (`[97] dual: ...`),
// so any single line answers "which build is running", and it can never be lost.
// The code is parsed at runtime from esp_app_get_description()->version (the same
// PROJECT_VER as CMakeLists) rather than duplicated in a macro: two copies drift, and a
// drifted version string is worse than none. A parse failure prints `[??]` - an unknown
// identity must LOOK unknown instead of silently degrading to some default.
static const char *fw_tag(void) {
    static char tag[12];
    static bool ready;
    if (!ready) {
        ready = true;
        const char *v = esp_app_get_description()->version;
        int major = 0, minor = 0, patch = 0;
        if (v != NULL && sscanf(v, "%*[^-]-pico%d.%d.%d", &major, &minor, &patch) == 3)
            // minor.patch 的定点编码：minor*10+patch（26.9.9→99，26.9.10→100）。
            // 不能再用 "%d%d" 字符串拼接——patch>=10 时会拼成 910。
            // Fixed-point minor.patch: minor*10+patch (26.9.9->99, 26.9.10->100).
            // The old "%d%d" concat produced 910 once patch reached two digits.
            snprintf(tag, sizeof(tag), "[%d]", minor * 10 + patch);
        else
            snprintf(tag, sizeof(tag), "[??]");
    }
    return tag;
}

// 整数平方根（牛顿法）：只用于双指日志里 dist 的可读性，不引浮点。
// Integer square root (Newton): makes the `dist` field in the dual log readable
// without pulling in floating point.
static int dual_dist_px(int dx, int dy) {
    const long v = (long)dx * dx + (long)dy * dy;
    if (v <= 0) return 0;
    long x = v;
    for (;;) {
        const long y = (x + v / x) / 2;
        if (y >= x) break;
        x = y;
    }
    return (int)x;
}

// 收尾方式的短名（26.9.7）：日志里一个词就能看出「两指是怎么没的」。
// `zero` = count 仍 >=2 却没有两个可用触点（病灶收尾，与 active 无关，补 lift0 的盲区）；
// `lift` = 两指真的都抬起；`one` = 先抬起一指（不对称抬手，正常形态）。
// Short name for how the gesture ended (26.9.7): one word tells how the two contacts
// went away. `zero` = count>=2 with no usable pair (the seam's terminal shape,
// independent of `active`, covering lift0's blind spot); `lift` = both really gone;
// `one` = one finger lifted first (the ordinary asymmetric case).
static const char *dual_end_name(ui_dual_end_t e) {
    switch (e) {
        case UI_DUAL_END_LIFT: return "lift";
        case UI_DUAL_END_ONE: return "one";
        case UI_DUAL_END_ZERO: return "zero";
        default: return "-";
    }
}

// 中止原因的短名（26.9.7）：ABORT 过去一句话都不说，round-4 那 1 笔「66 落定 / 65
// 判定」的缺口因此只能猜。这里让每一道门自己报名。
// Short name for the abort reason (26.9.7): ABORT used to say nothing at all, which is
// why round 4's 1-in-66 gap could only be guessed at. Now every gate names itself.
static const char *dual_abort_name(ui_dual_abort_t a) {
    switch (a) {
        case UI_DUAL_ABORT_SLIP: return "slip";
        case UI_DUAL_ABORT_GAP: return "gap";
        case UI_DUAL_ABORT_DEADZONE: return "deadzone";
        case UI_DUAL_ABORT_SLOW: return "slow";
        case UI_DUAL_ABORT_SUBSTEP: return "substep";
        case UI_DUAL_ABORT_AMBIG: return "ambig";
        default: return "-";
    }
}

// 中断边界先清页面装饰再清识别器。/ Clear decoration before resetting recognition.
static void cancel_gesture(app_ctx_t* ctx, const app_desc_t* app, ui_gesture_t* gesture) {
    if (gesture->active && app->on_gesture) {
        ui_gesture_event_t event = { .type = UI_GESTURE_CANCEL,
            .x0 = gesture->x0, .y0 = gesture->y0, .x = gesture->x, .y = gesture->y };
        const app_desc_t* requested = ctx->request_app;
        bool menu_requested = ctx->request_menu;
        bool return_requested = ctx->request_return;
        app_redraw_t redraw = app->on_gesture(ctx, &event);
        ctx->request_app = requested;
        ctx->request_menu = menu_requested;
        ctx->request_return = return_requested;
        ui_gesture_reset(gesture);
        app_present(ctx, app, redraw);
    }
    ui_gesture_reset(gesture);
}

static void present_page(app_ctx_t* ctx, const app_desc_t* app,
                         ui_gesture_t* gesture, app_redraw_t redraw) {
    if (redraw == APP_REDRAW_PAGE || redraw == APP_REDRAW_FULL)
        cancel_gesture(ctx, app, gesture);
    app_present(ctx, app, redraw);
}

typedef struct {
    unsigned updates;
    int64_t last_ms;
    EpdRect area;
} menu_feedback_t;

// 菜单反馈只推叶内单行。/ Menu feedback pushes one local row only.
static void menu_feedback(app_ctx_t* ctx, const app_desc_t* current,
                          int leaf, int hit, bool pressed, menu_feedback_t* feedback) {
    EpdRect area;
    int row = hit - leaf * UI_MENU_ITEMS_PER_PAGE;
    if (!ui_menu_row_rect(leaf, row, &area)) return;
    ui_draw_menu_row_pressed(ctx->fb, current, leaf, row, pressed);
    if (feedback->updates == 0) feedback->area = area;
    else {
        int bottom = feedback->area.y + feedback->area.height;
        int next_bottom = area.y + area.height;
        if (area.y < feedback->area.y) feedback->area.y = area.y;
        feedback->area.height = (bottom > next_bottom ? bottom : next_bottom) - feedback->area.y;
    }
    feedback->updates++;
    feedback->last_ms = ctx->now_ms;
    guard_draw_result(ctx->hl, update_display_area_with(
        ctx->hl, &E0470_WAVEFORM, APP_DYNAMIC_REFRESH_MODE, area));
    if (feedback->updates >= UI_SETTLE_DU_MAX) {
        guard_draw_result(ctx->hl, update_display_area_with(
            ctx->hl, &E0470_WAVEFORM, MODE_GL16, feedback->area));
        feedback->updates = 0;
    }
}

// 切页：先让上一页收尾，再给新页一次 on_enter，最后整页画出来。
// Leave the old page, enter the new one, then present a full page.
static bool main_page_ready(app_ctx_t* ctx, const app_desc_t* app) {
    int index = app_index_of(app);
    return index >= 0 && index < 4 &&
           app->main_page_visible && app->main_page_visible(ctx);
}

static void switch_to(
    app_ctx_t* ctx, const app_desc_t** current, const app_desc_t* next
) {
    if (next == NULL || next == *current) return;
#ifdef KIIKO_LOCAL_LATENCY
    const int64_t probe_start=esp_timer_get_time();
    const int probe_from=app_index_of(*current);
#endif
    bool main_source = main_page_ready(ctx, *current);
    bool shelf_source = main_source && app_index_of(*current) == 1;
    display_main_transition_cancel();
    if ((*current)->on_exit != NULL) (*current)->on_exit(ctx);
#ifdef KIIKO_LOCAL_LATENCY
    const int64_t probe_exit=esp_timer_get_time();
#endif
    *current = next;
    app_font_activate_system();
#ifdef KIIKO_LOCAL_LATENCY
    const int64_t probe_font=esp_timer_get_time();
#endif
    if (next->on_enter != NULL) next->on_enter(ctx);
#ifdef KIIKO_LOCAL_LATENCY
    const int64_t probe_enter=esp_timer_get_time();
#endif
    if (main_source && main_page_ready(ctx, next)) display_main_transition_arm(next, shelf_source, true);
    app_present(ctx, next, next->enter_full ? APP_REDRAW_FULL : APP_REDRAW_PAGE);
    display_main_transition_disarm();
#ifdef KIIKO_LOCAL_LATENCY
    ESP_LOGI("latency","SWITCH from=%d to=%d mode=%d exit_us=%lld font_us=%lld enter_us=%lld present_us=%lld total_us=%lld",probe_from,app_index_of(next),app_settings_main_refresh_mode(),
        (long long)(probe_exit-probe_start),(long long)(probe_font-probe_exit),(long long)(probe_enter-probe_font),
        (long long)(esp_timer_get_time()-probe_enter),(long long)(esp_timer_get_time()-probe_start));
#endif
    ESP_LOGI(TAG, "page -> %s", next->title);
}

// 菜单页不是 app，单独画。/ The menu is not an app; present it here.
static void present_menu(app_ctx_t* ctx, const app_desc_t* current, int leaf, menu_feedback_t* feedback) {
    s_presented_main = NULL;
    feedback->updates = 0;
    if (display_take_white_exit()) {
        guard_draw_result(ctx->hl, update_display_white(ctx->hl));
    }
    ui_draw_menu_page(ctx->fb, current, leaf);
    guard_draw_result(ctx->hl, update_display_fast_page(ctx->hl));
}

// 横屏阅读：把 CST836U 原生坐标转成横屏逻辑坐标。触摸面板物理帧 684×1216，与
// 竖屏（INVERTED_PORTRAIT）逻辑帧逐像素重合，所以竖屏下原生坐标可以直接当逻辑
// 坐标用；横屏（LANDSCAPE）逻辑帧是 1216×684，必须补 90° 映射——缺失时原始 x
// 只覆盖 0..683，右半屏（逻辑 x≥684）永远触不到，且右半屏落点的 y 越界，被所有
// 命中测试（ui_rect_hit / book_layout_hit_test / ui_nav_hit）静默丢弃。
// 推导：同一个物理落点在两种方向下对应同一个帧缓冲像素——竖屏逻辑 (tx,ty) 经
// epdiy _rotate(INVERTED_PORTRAIT) 得 fb (ty, 683-tx)，横屏逻辑 (lx,ly) 经
// _rotate(LANDSCAPE) 恒等得 fb (lx,ly)，联立得 lx=ty、ly=683-tx。
// Landscape reading: remap CST836U native coords into landscape logical coords.
// The touch panel's physical frame (684x1216) coincides with the portrait
// (INVERTED_PORTRAIT) logical frame pixel-for-pixel, so raw coords double as
// logical coords in portrait; the landscape (LANDSCAPE) logical frame is
// 1216x684 and needs the 90-degree mapping, or raw x spans only 0..683, the
// right half (logical x>=684) is unreachable, and right-half hits carry
// out-of-range y that every hit test (ui_rect_hit / book_layout_hit_test /
// ui_nav_hit) silently drops. Derivation: one physical hit maps to one
// framebuffer pixel in both orientations - portrait logical (tx,ty) gives fb
// (ty, 683-tx) via epdiy _rotate(INVERTED_PORTRAIT), landscape logical (lx,ly)
// gives fb (lx,ly) via the identity _rotate(LANDSCAPE) - hence lx=ty, ly=683-tx.
// 例外：实体按键不在显示屏上，而在面板向下延伸的条纹区（实测原生 y≈1500，分界
// UI_KEY_AREA_TOP=1300，见 ui_menu.h 注释）。ui_key_hit_test() 按原生条纹坐标
// 识别按键（y>=1300 且 x<480 → x/160），两种方向语义相同——条纹点绝不能旋转：
// 旋转后 (80,1500) 会变成屏内 (1215,603)，三个键和菜单全部失效。条纹点原样放行，
// 横屏按键行为与竖屏 stock 逐字节一致（debounce 只拷贝不筛坐标，点击反馈/手势对
// 屏外点本就是竖屏存量行为，命中测试均有上界，屏外点不会误命中任何矩形）。
// Exception: the three physical keys are not on the display but in the panel
// strip below it (native y~1500; split at UI_KEY_AREA_TOP=1300, see ui_menu.h).
// ui_key_hit_test() reads them by native strip coords (y>=1300, x<480 -> x/160)
// in BOTH orientations, so strip points must pass through unrotated - rotating
// (80,1500) yields on-screen (1215,603) and kills all three keys plus the menu.
// Passing them through makes landscape keys byte-identical to portrait stock
// behavior (debounce only copies coords, click feedback/gestures already handle
// off-screen points in portrait, and every hit test is upper-bounded).
static void touch_native_to_logical(cst836u_touch_t* t) {
    if (epd_get_rotation() != EPD_ROT_LANDSCAPE) return;
    const int w = epd_width();   // 1216，横屏逻辑宽 / landscape logical width
    const int h = epd_height();  // 684，横屏逻辑高 / landscape logical height
    uint16_t nx = t->x, ny = t->y;
    if (ny < UI_KEY_AREA_TOP) {
        int lx = (int)ny, ly = h - 1 - (int)nx;
        t->x = (uint16_t)(lx < 0 ? 0 : lx > w - 1 ? w - 1 : lx);
        t->y = (uint16_t)(ly < 0 ? 0 : ly > h - 1 ? h - 1 : ly);
    }
    for (int i = 0; i < CST836U_MAX_POINTS; i++) {
        nx = t->points[i].x; ny = t->points[i].y;
        if (ny < UI_KEY_AREA_TOP) {
            int lx = (int)ny, ly = h - 1 - (int)nx;
            t->points[i].x = (uint16_t)(lx < 0 ? 0 : lx > w - 1 ? w - 1 : lx);
            t->points[i].y = (uint16_t)(ly < 0 ? 0 : ly > h - 1 ? h - 1 : ly);
        }
    }
}

// 空槽常带着抬起事件或残留坐标，不能进页面看到的快照；抬手后保留最后一次位置。
// Empty slots often carry lift events or stale coords; keep last position on lift.
static void debounce_touch(
    cst836u_touch_t* latest, const cst836u_touch_t* raw, bool released
) {
    if (raw->touched) {
        *latest = *raw;
        for (int i = 0; i < CST836U_MAX_POINTS; i++) {
            if (!latest->points[i].active) {
                memset(&latest->points[i], 0, sizeof(latest->points[i]));
            }
        }
    } else if (released) {
        latest->touched = false;
        latest->count = 0;
        for (int i = 0; i < CST836U_MAX_POINTS; i++) {
            latest->points[i].active = false;
        }
    }
    memcpy(latest->raw, raw->raw, sizeof(raw->raw));
}

static bool try_load_saved_sd_font(bool allow_probe) {
    read_pico_sd_info_t info;
    esp_err_t err = read_pico_sd_get_info(&info);
    if (err == ESP_ERR_NOT_FINISHED) return false;
    if (!info.mounted) {
        if (allow_probe && err == ESP_ERR_INVALID_STATE) read_pico_sd_start_probe();
        return false;
    }
    return app_font_retry_active();
}

// 26.9.16 R3：SD 的兜底探测与自愈。
// 26.9.15 的事故形状是「一次不成立的探测条件 + 一条都不打的日志」：卡从头到尾没进
// 初始化流程，而三处告警（`SD_CD absent` / `Mount failed` / `Mounted`）全在 SD 组件
// 内部、全落在 0～4 s 的失明窗口里，于是现场看到的是「书库空了，一句解释都没有」。
// 26.9.16 R3: fallback probing and self-healing for SD. The 26.9.15 failure shape was
// "a probe condition that never held plus not one log line": the card never entered the
// init path, and all three warnings lived inside the SD component, inside the 0-4 s deaf
// window - so the field saw an empty shelf and no explanation at all.
#define SD_PROBE_RETRY_MAX 3
// 31b 第 1 条：三拍封送的等待窗。2 s 是 31b 检查单写死的值，与既有的
// book_cover_jobs_quiesce/ui_avatar_quiesce 同口径——三个用户用同一个数，
// 慢的那一个决定整拍时长，不会出现「谁先超时谁先动卷」。
// The three-beat wait. 2 s is the value the 31b checklist fixed, identical to the
// existing cover/avatar quiesce so that the slowest user sets the beat.
#define PUSH_QUIESCE_MS 2000u
#define SD_PROBE_RETRY_GAP_US (8LL * 1000000)
// 26.9.31a：三次快速重试之后的看门狗节拍。**永不放弃**——「卡在位、没挂上」这个状态
// 只有重启才能出，从这一版起不存在。10 s 三次，之后 60 s 一次，稳态每分钟留一行痕。
// 26.9.31a: watchdog cadence after the three fast retries. Never give up: "card present,
// nothing mounted" must stop being a reboot-only state. 10 s three times, then once a minute.
#define SD_WD_FAST_US (10LL * 1000000)
#define SD_WD_SLOW_US (60LL * 1000000)

// 先停文件使用者，再换字体；此处只观察挂载，不卸载也不重新挂载。
// Stop file consumers before font fallback; observe mounting without unmounting or remounting.
static bool poll_media(app_ctx_t* ctx, const app_desc_t* current,
                       bool* mounted, bool* invalidated) {
    if (usb_storage_active()) { *mounted = false; *invalidated = false; return false; }

    // 31b：冻结看门狗的事件在这里落地。组件侧只负责「到点强制解冻 + 置一次性事件」，
    // 黑匣子归 app 写——组件不反向依赖 pico_bootbb（与 §13.8 任务拓扑同一条纪律）。
    // The watchdog event lands here: the component forces the thaw and raises a
    // one-shot; the blackbox belongs to the app (no component -> app dependency).
    unsigned frozen_ms = 0;
    if (push_sink_take_freeze_timeout(&frozen_ms)) {
        pico_bootbb_note(PICO_BOOTBB_NOTE_PUSH_FREEZE,
                         (uint8_t)(frozen_ms & 0xFFu),
                         (uint16_t)((frozen_ms >> 8) & 0xFFFFu));
        ESP_LOGE(TAG, "push freeze watchdog fired: forced thaw after %u ms", frozen_ms);
    }
    read_pico_sd_info_t info = {0};
    // ---- 31b P1-1：CD 之外的第二个信源 --------------------------------------
    // 必须在 `get_info()` **之前**：本拍判死后，紧接着的 get_info 就会把 `mounted`
    // 读成 0，于是同一拍的 lost 判定照常成立——不会多花一拍。
    // 为什么不放在组件里自己记黑匣子：组件不反向依赖 pico_bootbb（§13.8 同一条纪律），
    // 事件归 app 写。
    // P1-1: before get_info() so the same tick's lost judgement sees it. The note is
    // written here because components never depend on pico_bootbb.
    {
        uint8_t io_fails = 0;
        uint16_t io_ms = 0;
        if (read_pico_sd_watch_io(&io_fails, &io_ms) == ESP_ERR_NOT_FOUND) {
            pico_bootbb_note(PICO_BOOTBB_NOTE_SD_IO_DEAD, io_fails, io_ms);
        }
    }
    esp_err_t err = read_pico_sd_get_info(&info);

    // ---- R3-a：状态一变就说话 ------------------------------------------------
    // 只在 (err, present, mounted, invalidated) 这个四元组变化时打印，稳定后完全静默，
    // 所以它既能在现场留下证据，又不会把日志刷没。
    // Only when the (err, present, mounted, invalidated) tuple changes, so it leaves
    // evidence in the field without flooding the log once things settle.
    static esp_err_t s_last_err = ESP_OK;
    static uint8_t s_last_flags = 0xFF;
    static uint32_t s_retries;
    static int64_t s_next_retry_us;
    // 26.9.17 T1：拔卡释放的结果，以及卸载失败时的补卸载闩锁。
    // 卸载一旦失败，绝不能把 `*invalidated` 清掉——那会让插回重新落进自锁，
    // 而 `card` 仍指着那张已经离开的卡。
    // 26.9.17 T1: outcome of the removal release plus the retry latch for a failed
    // unmount. `*invalidated` must never be cleared on failure: that re-arms the
    // deadlock on reinsertion while `card` still points at the card that left.
    static esp_err_t s_last_release = ESP_OK;
    // 26.9.18 N2：`release=` 是否还该说话。只有真的卸载过才有意义，ready 那拍归 false。
    // 26.9.18 N2: whether `release=` still means anything. Only a real removal sets it;
    // the `ready` tick clears it.
    static bool s_release_known;
    static bool s_release_pending;
    static uint8_t s_release_tries;
    static int64_t s_next_release_us;

    // ---- 26.9.18 N3：判决先落，再说话 -----------------------------------------
    // 26.9.17 是「先打印、后判定」，于是每次拔卡印两行：先 `invalidated=0`，1 ms 后
    // `invalidated=1`。两行都对，并排放着却像「第一次没标记、第二次才标记」。
    // 现在先算 ready/lost/gained、把 `*invalidated` 落下去，再打这一行。
    // 26.9.17 printed BEFORE judging, so every removal produced two lines (`invalidated=0`
    // then `=1`). Both were true, but read side by side they look like the flag needed two
    // attempts. Judge first, then speak once.
    const bool ready = err == ESP_OK && info.mounted;
    const bool lost = *mounted && !ready;
    const bool gained = !*mounted && ready;

    // ---- 26.9.18 N2：release= 说人话 ------------------------------------------
    // `s_last_release` 只在 lost 和补卸载里写，插回后一直挂着上一轮的
    // `ESP_ERR_NOT_FOUND`，于是「恢复成功」那行也印着错误码。而 NOT_FOUND 在这里是
    // **成功**（卡已不在，卸载本来就是幂等的）。现在映射成 ok/fail，并在 ready 那一拍
    // 归 `-`（当前没有卸载记录）。
    // `release=` used to keep the previous removal's error code forever, so the successful
    // re-mount line also read like a failure. NOT_FOUND is a success here (the card is gone,
    // unmount is idempotent). Map to ok/fail and reset to `-` once the card is back.
    if (ready) {
        *invalidated = false;
        s_release_known = false;
    } else if (lost) {
        *invalidated = true;
    }

    // ---- 26.9.27 S26-4：挂载让行，只在「刚拿到卡」那一拍 ----
    // 插卡/退 USB 后挂载成功，先让行 5 s 再冲账摘抄（细化账一.4）：书架扫描、封面
    // 预取都在这一段里跑，抢同一张卡只会让两边都慢。**边沿触发**——写成 `if (ready)`
    // 就是每一拍都把窗口往后推，队列永远等不到可以落盘的那一拍（假接线，最坏那种）。
    // 5 s 由家务片状态机计，不起软件定时器（S26-4 钉死项）。
    // S26-4 mount yield, on the EDGE only: a freshly mounted card gets five seconds before
    // the excerpt queue touches it, so the shelf scan and cover prefetch go first. Writing
    // `if (ready)` would push the window forward every tick and the queue would never flush -
    // the worst kind of fake wiring. The housekeeping state machine counts the five seconds.
    // E-2（§19.4）：锚点换成**挂载代号**，不是 `gained` 状态边沿。
    // `gained = !*mounted && ready` 是状态差分：拔插快于一次轮询时 `*mounted` 全程为 true，
    // 这条边沿一次都不成立（26.9.31 现场 100.977 挂载、100.994 写卡、101.863 才见 mounted=1，
    // 中间 886 ms 全程无让行）。代号是事件，挂载一次 +1 一次，漏不掉。
    // 这里每拍都调，靠 `excerpt_store_mount_yield()` 的幂等收口——同一代号只开一次窗口。
    // E-2: the anchor is the mount EPOCH, not the `gained` state edge. A state difference
    // vanishes when a replug beats one poll interval; an event does not. Called every tick,
    // made idempotent by the callee.
    excerpt_store_mount_yield(read_pico_sd_mount_epoch(), (uint32_t)ctx->now_ms);

    const uint8_t flags = (uint8_t)((info.present ? 1u : 0u) | (info.mounted ? 2u : 0u) |
                                    (*invalidated ? 4u : 0u));
    // `changed` 同时门控下面那条 `probe start`——探测每拍都会被重新调用（`probe_state`
    // 停在 1 时返回 NOT_FINISHED），若不门控就是 500 ms 一条，四秒八条。
    // `changed` also gates the `probe start` line below: the probe is re-asked every tick
    // and returns NOT_FINISHED while `probe_state` sits at 1, so without the gate it would
    // log eight lines over four seconds.
    const bool changed = (err != s_last_err || flags != s_last_flags);
    if (changed) {
        // `release=` 的三态就地算：`-` 没有卸载记录、`ok` 卸载成功（NOT_FOUND 也算——
        // 卡已不在，卸载本来幂等）、`fail` 才是真失败。写成内联三元而不是先取一个
        // 变量，是因为宿主套件把日志宏定义成 `((void)0)`，那种变量会被 `-Werror
        // =unused-variable` 判成死代码。
        // The three-state `release=` is computed inline: `-` no removal on record, `ok` the
        // removal succeeded (NOT_FOUND counts - the card is gone and unmount is idempotent),
        // `fail` is a real failure. Inline rather than a local because the host suites
        // define the log macros as `((void)0)`, where such a local trips -Wunused-variable.
        ESP_LOGW(TAG, "sd: err=%s present=%d mounted=%d invalidated=%d release=%s retries=%u",
                 esp_err_to_name(err), (int)info.present, (int)info.mounted,
                 (int)*invalidated,
                 !s_release_known
                     ? "-"
                     : ((s_last_release == ESP_OK || s_last_release == ESP_ERR_NOT_FOUND)
                            ? "ok" : "fail"),
                 (unsigned)s_retries);
        // 错误码低 8 位进 a，高 4 位与三个标志进 b——黑匣子只有 8 B 的预算。
        // Low byte of err into a; upper nibble plus the three flags into b - the blackbox
        // has 8 bytes per note and no more.
        pico_bootbb_note(PICO_BOOTBB_NOTE_SD_STATE, (uint8_t)((unsigned)err & 0xFFu),
                         (uint16_t)(flags | ((((unsigned)err >> 8) & 0x0Fu) << 8)));
        s_last_err = err;
        s_last_flags = flags;
    }

    // ---- R3-b：兜底探测（去掉 info.present 这个致命条件）--------------------
    // `ESP_ERR_INVALID_STATE` 本身就是「还没探测过」的信号，不需要再问 `info.present`。
    //
    // 但**必须**问 `!*mounted`：这条分支的语义是「从没挂上过、而系统说还没探测过，
    // 那就探一次」。已经挂上过再收到 INVALID_STATE，交给下面的 lost 判定——
    // 否则拔卡时会被这条兜底拦下来，`on_media_lost` 一次都不触发（宿主测试当场抓到）。
    // There is one more condition, and it is `!*mounted`, not `info.present`: this branch
    // means "never mounted, and the stack says it has never probed - so probe once". Once
    // mounted, an INVALID_STATE belongs to the lost check below; letting the fallback catch
    // it would swallow every card removal (the host suite caught exactly that).
    //
    // 原条件在 `s_ioe == NULL` 时会**同时**不成立：`read_pico_sd_detect()` 返回
    // `ESP_ERR_INVALID_STATE` 且不动 `*present`，于是 `cached_info.present` 停在 false，
    // `get_info()` 把同一个错误原样返回，而这里的 `&& info.present` 把它挡死——
    // **一次都不探测，一句日志不打**。用户插一次卡就恢复，因为
    // `observe_media_locked()` 的 `inserted` 分支会把 `probe_state` 从 2 重置为 0。
    // 26.9.15 的 TF 卡就是这样"消失"的。
    //
    // `ESP_ERR_INVALID_STATE` already means "never probed"; asking `info.present` as well
    // is what killed it. When `s_ioe == NULL` the detect call returns INVALID_STATE and
    // leaves `*present` untouched, so `cached_info.present` stays false, `get_info()` hands
    // the same error back, and the `&& info.present` guard blocks the probe forever - with
    // zero log lines. Re-seating the card recovers it because `observe_media_locked()`'s
    // `inserted` branch resets probe_state from 2 to 0.
    //
    // ---- 26.9.17 T2：把 `!*invalidated` 拆掉 ---------------------------------
    // 26.9.16 为挡住「拔卡瞬间被兜底拦下」加了 `!*invalidated`，结果焊死了热插拔：
    // `*invalidated` 只有 `ready` 能清，`ready` 要 `mounted`，`mounted` 要探测，
    // 探测要 `!*invalidated`——闭合成环，插回再多次也不探测。26.9.16 现场 0 条
    // `sd: probe start`、0 条 `sd: remount` 就是这个环的签名。
    //
    // 拆得掉的理由是判据本来就够严：拔卡时 `get_info()` 走 `!info->present` 分支
    // 返回 `ESP_ERR_NOT_FOUND`（不是 INVALID_STATE），I/O 抖动返回 detect 的错误码，
    // 已挂载返回 ESP_OK，探测中返回 NOT_FINISHED。**只有「卡在位且从未探测过」
    // 会返回 INVALID_STATE**，不存在把拔卡误当插卡的路径。重复起任务也不必另设节流：
    // 探测一失败 `probe_state` 落到 2 且 `error != INVALID_STATE`，兜底自然不再成立，
    // 只有 `observe_media_locked()` 的 `inserted` 分支把它归 0 才会重来。
    // ---- 26.9.17 T2: drop `!*invalidated`. --------------------------------
    // 26.9.16 added it to stop the fallback from swallowing a removal, and welded
    // hot-plug shut: `*invalidated` is cleared only by `ready`, `ready` needs
    // `mounted`, `mounted` needs a probe, and the probe needed `!*invalidated`.
    // The 26.9.16 log's two zeros - no `sd: probe start`, no `sd: remount` - are
    // that ring's signature. It is safe to drop because the remaining predicate is
    // already exact: a removed card returns ESP_ERR_NOT_FOUND, a flaky I2C returns
    // the detect error code, a mounted card returns ESP_OK, a probe in flight
    // returns NOT_FINISHED. Only "card present, never probed" yields
    // INVALID_STATE. No extra throttle is needed either: a failed probe leaves
    // `probe_state` at 2 with `error != INVALID_STATE`, so the fallback stops
    // holding until `observe_media_locked()`'s `inserted` branch resets it to 0.
    if (err == ESP_ERR_INVALID_STATE && !*mounted) {
        esp_err_t perr = read_pico_sd_start_probe();
        // ---- 26.9.18 N1：这条日志以前是死的 ---------------------------------
        // `start_probe()` 成功启动任务时返回 `ESP_ERR_NOT_FINISHED`，而 26.9.17 写的是
        // `if (perr != ESP_ERR_NOT_FINISHED)` 才打——**启动成功恰恰不打，只在失败时报丧**。
        // 结果 26.9.17 的验收清单让测试端去看一条永远不会出现的日志，而它其实一直在正常
        // 工作。现在反过来：NOT_FINISHED 是「探测在跑」，其余才是失败。
        // ---- 26.9.18 N1: this line used to be dead -------------------------
        // `start_probe()` returns NOT_FINISHED on a successful start, and 26.9.17 only
        // printed when `perr != NOT_FINISHED` - i.e. it stayed silent exactly when the
        // probe started, and only spoke on failure. The 26.9.17 acceptance list then sent
        // the tester to look for a line that could never appear, while probing had been
        // working all along. Inverted now: NOT_FINISHED means "probe is running".
        if (changed) {
            if (perr == ESP_ERR_NOT_FINISHED) {
                ESP_LOGW(TAG, "sd: probe start ok (present=%d)", (int)info.present);
            } else {
                ESP_LOGW(TAG, "sd: probe start failed: %s (present=%d)",
                         esp_err_to_name(perr), (int)info.present);
            }
            pico_bootbb_note(PICO_BOOTBB_NOTE_SD_PROBE, (uint8_t)((unsigned)perr & 0xFFu),
                             (uint16_t)(info.present ? 1u : 0u));
        }
        return false;
    }
    if (err == ESP_ERR_NOT_FINISHED) return false;

    *mounted = ready;
    if (ready) s_retries = 0;
    if (gained || lost) {
        ui_avatar_invalidate();
        // B42：插拔边沿只作废旧一代封面，**不在这里收口**。收口是有代价的等待（2 s），
        // 属于 lost 分支的三拍；此处传 0 曾写成 `quiesce(0)`——那是「宣布收口却不等」，
        // 返回值还被 `(void)` 扔掉。两个语义分开：要旧结果消失用 discard，要句柄收齐用
        // quiesce(2000)。
        // B42: the hotplug edge only discards the stale generation; the real join belongs
        // to the three beats in the lost branch. `quiesce(0)` announced a join it never
        // performed, and its result was discarded anyway.
        book_cover_jobs_discard();
        book_lock_collage_invalidate();
    }
    // 31b：插回后重新挂载 = 解冻的**主通道**，不是兜底。
    // 「卸卷完成」只在掉卡场景出现；格式化 / USB 挂载 / 深睡之后卡还在卷还在，
    // 靠的就是这一条边沿。先解冻再叫页面，免得 on_media_ready 里的扫描被自己的闸挡住。
    // The remount edge IS the primary thaw path, not a fallback: after format/USB/
    // sleep the card never left, so "unmount done" never fires.
    if (gained) {
        // 条件②（挂载边沿）：闩锁与 hold 一并结清。thaw 清 FRZ_HOLD，pending 在
        // 这里清——漏掉任一边，就是「解冻了但下一拍又去卸这张卡」。
        // Ruling 2 (remount edge): clear both. `thaw` drops FRZ_HOLD; the latch is
        // cleared here. Leaving either behind means "thawed, but the next tick unmounts
        // the card again".
        if (s_release_pending) {
            s_release_pending = false;
            s_release_tries = 0;
            ESP_LOGW(TAG, "sd: remount edge clears the release latch");
        }
        push_sink_thaw("remount edge");
        // 26.11.5：拔卡那一刻我们断过链，这一拍把**可连性**还回去。漏掉它，
        // 「插回不认卡」就变成「插回认卡、手机再也连不上」——31a 家族的第三个入口。
        // 只在真的断过链时动作（push_sink 内部欠账标志），正常插卡不会平白开窗口。
        // 26.11.5: we cut the link when the card left, so this edge repays the
        // connectivity. Skipping it turns "card not recognised" into "card works,
        // phone can never connect again" - the third 31a entrance. Only acts when a
        // detach really happened.
        (void)push_sink_resume_after_media();
    }
    if (gained && current->on_media_ready) current->on_media_ready(ctx);

    // ---- 补卸载闩锁：必须放在 `gained` 判定**之后** ----------------------------
    // 放在最前面（31b 原样）会与条件②打架：pending 挂着时这里先 `return false`，
    // `gained` 永远走不到——用户插回的卷明明挂上了，闩锁却每拍去卸它（旧状态
    // 污染新会话的活体）。挪到边沿之后：真恢复由 `gained` 清账，真失败才重试。
    // The latch must run *after* the `gained` edge. Up front it returns false before
    // `gained` can ever fire, so a volume the user reinserted and mounted got unmounted
    // once per tick forever. Edge first, retry second.
    // 补卸载：上一拍 `close_card()` 没成，1 s ×3 快速重试，之后 10 s 一拍**永不放弃**。
    // 「最多 3 次」条款已于 2026-11-01 由 31a 环④证据废止（放弃卸载 = `card` 永非空
    // → 下次 mount 以 `stale card` 拒绝 → 插回不认卡）。计划原文保留，此处按勘误加注。
    // Retry latch: 3 fast retries at 1 s, then one attempt every 10 s, forever. The "at
    // most 3" clause was retired on 2026-11-01 by 31a's ring-4 evidence: giving up leaves
    // `card` non-NULL and every later mount is refused with `stale card`.
    if (s_release_pending) {
        const int64_t now = esp_timer_get_time();
        if (now >= s_next_release_us) {
            // 31b：闩锁重试同样要先过三拍。上一次失败可能就是会话没停，不带闸
            // 直接重试等于绕过它——而这次重试碰的正是同一个卷。
            // The latch retry must pass the three-beat too: the previous attempt may
            // have failed because a session would not stop, and this one touches the
            // very same volume.
            if (!push_sink_quiesce(PUSH_QUIESCE_MS)) {
                // ---- 批复条件③（2026-11-01）：第三方 FILE* 永不关 -------------------
                // 本态 = 冻结 + 重试直到句柄关闭或重启，**不是放弃**。26.9.29 在这里
                // `s_release_pending=false`，等于把卸载交给运气：`card` 永远非空，
                // 之后每次 mount 都以 `stale card` 被拒——31a 环④的第二个入口。
                // 计数饱和在 255：慢档的「每拍一行」依赖它不回落，uint8_t 溢出会让
                // 10 s 档悄悄退回 fast 档，日志与节拍一起失真。
                // Ruling 3: a FILE* that never closes means frozen + retry until it does or
                // the device reboots - never "give up" (that is what stranded `card`). The
                // counter saturates so the slow tier cannot silently fall back to fast.
                const bool fast = s_release_tries < SD_PROBE_RETRY_MAX;
                const bool first_slow = (s_release_tries == SD_PROBE_RETRY_MAX);
                if (s_release_tries < 0xFFu) ++s_release_tries;
                if (fast) {
                    s_next_release_us = now + 1000000;
                    ESP_LOGW(TAG, "sd: release waiting for push to stop (%u/%u)",
                             (unsigned)s_release_tries, (unsigned)SD_PROBE_RETRY_MAX);
                } else {
                    if (first_slow) {
                        // 跨档那一刻留痕一次：bootbb 条目是 critical，不逐拍刷环。
                        push_sink_quiesce_hold("release latch: push never stopped",
                                               PUSH_HOLD_WHY_QUIESCE);
                        ESP_LOGE(TAG,
                                 "sd: push never stopped (%s), frozen; unmount retries every 10 s",
                                 esp_err_to_name(s_last_release));
                    }
                    // 条件①：每拍一行，带计数。HOLD 之后只剩一次性留痕就是「无限
                    // 静默冻结」换了形态——10 s 一拍必须说得出「还在等谁」。
                    ESP_LOGW(TAG, "sd: release still waiting for push (try %u, frozen)",
                             (unsigned)s_release_tries);
                    s_next_release_us = now + SD_WD_FAST_US;
                }
                return false;
            }
            s_last_release = read_pico_sd_remount();
            if (s_last_release == ESP_OK || s_last_release == ESP_ERR_NOT_FOUND) {
                s_release_pending = false;
                // 三拍终态①：卸卷完成 → 解冻。/ Terminal state 1: released, thaw.
                push_sink_thaw("unmount done (latch)");
            } else if (s_release_tries < SD_PROBE_RETRY_MAX) {
                ++s_release_tries;
                s_next_release_us = now + 1000000;
            } else {
                // ---- 合流裁决点 1（31a 永不放弃 × 31b 三拍终态）--------------------
                // 两边说的是两件事，分开办：
                //   · **卸载动作**永不放弃（31a）。26.9.29 在这里 `s_release_pending=false`
                //     之后 `card` 永远非空、`mount_card()` 每次以 `stale card` 拒绝——
                //     「插回不认卡、只能重启」的第二个入口。卸载失败 = 卷还挂着，
                //     唯一出路是继续卸，10 s 一拍，永不停。
                //   · **push 冻结**必须在跨过 fast 档那一刻留痕（31b），否则就是
                //     「无限静默冻结」——三拍要的是「说得出为什么还冻着」，不是「冻着就行」。
                //     hold 是**留痕**不是终态：卸载一成功，上面的 `push_sink_thaw()` 照常解冻。
                // 与 31b 的差别只有一处，且是**放宽**：`s_release_pending` 不再置 false。
                // 理由：31b 置 false 是「放弃卸载」，31a 已证明放弃卸载才是那个死锁；
                // 而「无限冻结」的代价由 hold 留痕 + 每拍一句承担。
                //
                // Merged ruling 1: two different things. The **unmount** never gives up
                // (31a: giving up strands `card` and every later mount is refused with
                // `stale card`). The **push freeze** gets its mark at the moment it crosses
                // the fast tier (31b: a freeze that cannot say why is the silent-freeze
                // failure the three-beat exists to prevent). `hold` is a mark, not a
                // terminal state; the thaw above still fires on success.
                if (s_release_tries == SD_PROBE_RETRY_MAX) {
                    push_sink_quiesce_hold("release latch exhausted: unmount failed",
                                           PUSH_HOLD_WHY_RELEASE);
                    ESP_LOGE(TAG, "sd: release gave up fast retries (%s), now retrying every 10 s",
                             esp_err_to_name(s_last_release));
                }
                if (s_release_tries < 0xFFu) ++s_release_tries;
                // 条件①：10 s 一拍必须逐拍可见，不能每 6 次才吭一声。
                // Ruling 1: one line per 10 s attempt, not one per six.
                ESP_LOGW(TAG, "sd: release still failing (%s, try %u), retry in 10 s",
                         esp_err_to_name(s_last_release), (unsigned)s_release_tries);
                s_next_release_us = now + SD_WD_FAST_US;
            }
        }
        return false;
    }

    // ---- 26.9.31a P0-1(e)：看门狗 --------------------------------------------
    // 三次快速重试（8 s 间隔）之后**不再撒手**，改 10 s ×3、之后 60 s 一次的周期重试，
    // 永不停止。「卡在位、没挂上」从这一版起不再是只能重启的状态。
    //
    // 同时删掉 26.9.29 的 `!*invalidated` 前置条件：锁存 + 卡在位正是最需要 remount 的那
    // 一拍（先卸载、再清闩、再探测），26.9.29 反而把它挡在门外——那是环④。
    // 判据仍保留 `!lost`：刚丢卡那一步必须落到 on_media_lost，不能在这里被静默重试吞掉。
    // 26.9.31a P0-1(e): after the three fast retries the ladder becomes a watchdog - 10 s
    // three times, then once a minute, forever. "Present, nothing mounted" stops being a
    // reboot-only state. The `!*invalidated` guard is gone: latched + present is exactly the
    // tick that needs remount (release, clear, probe); 26.9.29 blocked it. `!lost` stays.
    if (!ready && !lost && info.present && err != ESP_ERR_NOT_FOUND &&
        err != ESP_ERR_NOT_FINISHED) {
        const int64_t now = esp_timer_get_time();
        if (now >= s_next_retry_us) {
            const bool fast = s_retries < SD_PROBE_RETRY_MAX;
            const bool wd_speaks = fast || s_retries <= SD_PROBE_RETRY_MAX + 3 ||
                                   (s_retries % 30u) == 0u;
            ++s_retries;
            s_next_retry_us = now + (fast ? SD_PROBE_RETRY_GAP_US
                                          : (s_retries <= SD_PROBE_RETRY_MAX + 3
                                                 ? SD_WD_FAST_US : SD_WD_SLOW_US));
            if (fast) {
                ESP_LOGW(TAG, "sd: remount %u/%u after err=%s", (unsigned)s_retries,
                         (unsigned)SD_PROBE_RETRY_MAX, esp_err_to_name(err));
            } else if (wd_speaks) {
                ESP_LOGE(TAG,
                         "sd: watchdog remount #%u after err=%s (present=1 mounted=0 invalidated=%d)",
                         (unsigned)s_retries, esp_err_to_name(err), (int)*invalidated);
            }
            pico_bootbb_note(PICO_BOOTBB_NOTE_SD_PROBE, (uint8_t)s_retries,
                             (uint16_t)((unsigned)err & 0xFFFFu));
            (void)read_pico_sd_remount();
            return false;
        }
    }

    if (!lost) return false;
    // `*invalidated` 已在上面判决时置位（N3），这里不再重复。
    // `*invalidated` was already set by the judgement above (N3); no repeat here.
    if (current->on_media_lost) current->on_media_lost(ctx);
    if (ttf_font_ready() && !ttf_font_is_builtin()) ttf_font_open_builtin();
    ui_text_set_system_font(false);

    // 31b 第 1 条：三拍的「停用 → 收口」两拍。push 是 SD 之外的第五个用户，
    // 它必须在 REMOVE 之前停到「状态机空闲 + 零 FILE*」——否则卷被抽走时 .part
    // 还开着，26.9.16 的 83 条 `sdmmc_read_blocks failed (0x107)` 就是那个形状。
    // 放在字体切换之后：字体必须切，否则阅读器画不出来，与 push 停不停无关。
    // The stop/gather beats. push must reach idle+zero-FILE* before REMOVE, or the
    // volume is pulled out from under an open .part (the 26.9.16 shape).
    //
    // 26.11.5：先由 push_sink 自己把链路收掉，再走三拍。手机常驻时（APK 长连）
    // 用户拔卡，链路还活着——只放行卸载而不收链路，手机下一次 BEGIN 会写向一个
    // 不存在的卷。顺序是硬的：detach 在前、quiesce 在后（B78 咬这条序）。
    // 26.11.5: push_sink drops the link first, then the three beats run. With a
    // resident APK the link is still up when the card leaves; letting the unmount
    // through without cutting it would send the next BEGIN into a missing volume.
    // The order is hard: detach before quiesce (B78 bites it).
    push_sink_detach_media("card removed");
    if (!push_sink_quiesce(PUSH_QUIESCE_MS)) {
        // 没停：不卸卷，保留 invalidated，交给下面的闩锁。冻结保持（仍在三拍区间）。
        // Not stopped: leave the volume alone; the latch retries. The freeze stays.
        s_last_release = ESP_ERR_NOT_FINISHED;
        s_release_known = false;
        s_release_pending = true;
        s_release_tries = 0;
        s_next_release_us = esp_timer_get_time() + 1000000;
        return true;
    }

    // ---- 26.9.17 T1：句柄关完就卸载 ----------------------------------------
    // 顺序是硬的：先让各页关掉 FILE*/字体/词典句柄，再动 FAT 卷，否则 unregister
    // pdrv 之后还有人在读，就是悬空句柄。
    // 卡不在槽里时 `read_pico_sd_remount()` 只做三件不碰卡的事：`esp_vfs_fat_sdcard_unmount()`
    // （f_mount(0) + ff_diskio_unregister + host deinit + free(card)）、清
    // `media_invalidated`、把 `probe_state` 归 0；随后 `start_probe()` 先 detect，
    // 看见 `!present` 直接落 `NOT_FOUND`，**不建任务、不访问卡**。
    // 不卸载的后果 26.9.16 留了两行铁证：`dict opendir errno=5 (EIO)` 而不是 ENOENT，
    // 以及拔卡后 83 条 `sdmmc_read_blocks failed (0x107)`——卷还挂着在空转。
    // ---- 26.9.17 T1: unmount once the handles are closed -------------------
    // The order is hard: every page closes its FILE*/font/dictionary handles first,
    // then the FAT volume moves - otherwise someone is still reading after
    // `ff_diskio_unregister(pdrv)` and that is a dangling handle.
    // With no card in the slot `read_pico_sd_remount()` does three things that never
    // touch the card: `esp_vfs_fat_sdcard_unmount()` (f_mount(0) +
    // ff_diskio_unregister + host deinit + free(card)), clear `media_invalidated`,
    // and reset `probe_state` to 0. The probe that follows detects `!present` and
    // lands on NOT_FOUND without creating a task or touching the card.
    // Skipping it left two proofs in the 26.9.16 log: `dict opendir errno=5 (EIO)`
    // instead of ENOENT, and 83 `sdmmc_read_blocks failed (0x107)` - the volume was
    // still mounted, spinning on a card that had left.
    s_last_release = read_pico_sd_remount();
    s_release_known = true;   // 26.9.18 N2：从现在起 `release=` 有意义，插回那拍再归 "-"
    if (s_last_release != ESP_OK && s_last_release != ESP_ERR_NOT_FOUND) {
        // 卸载没成：保留 `*invalidated`，交给上面的补卸载闩锁，绝不在这里放假消息。
        // Release failed: keep `*invalidated` and leave it to the retry latch above.
        s_release_pending = true;
        s_release_tries = 0;
        s_next_release_us = esp_timer_get_time() + 1000000;
    } else {
        // 三拍终态①：卸卷完成 → 解冻。卡已不在，但入口必须开——否则用户插回卡
        // 之前这一整段时间 push 都收不了包，而那不是本节要保护的东西。
        // Terminal state 1: released, thaw. The card is gone, but the gate must
        // open - a permanent freeze here is not what this section protects.
        push_sink_thaw("unmount done");
    }
    return true;
}

// 电源动作先保存当前页面，再定稿一张不带时钟的静态画面；避免关机后留下会误读的冻结时间。
// Save the current page first, then settle a clock-free static frame so shutdown never leaves a misleading frozen time.
// 锁屏前先由页面保存进度，再持久化一次恢复上下文；不在翻页或点击时写入。
// Flush page progress before persisting one lock checkpoint; never write it on page turns or taps.
static void prepare_lock(app_ctx_t *ctx, const app_desc_t *current, bool feedback, bool manual) {
    // 物理横屏是阅读页私有状态（全工程运行时唯一的旋转切换点在 book render()）；
    // 进入任何系统画面（锁屏/壁纸/PIN/票根/关机/重启）前必须复位，否则竖屏资产
    // 会画在横屏物理屏上——真机实证：长按电源睡眠后壁纸横置、关机画面错位。
    // epd_set_rotation 只改状态不刷新，唯一一次刷新由各调用路径固有的 present /
    // 全刷完成，不会双重刷新。boot 解锁门不走本路径，由 init 竖屏 + 锁屏 art 的
    // rotation 校验自愈（ESP rail 被 PMU 掉电后完整重启）。
    // Landscape is the reader's private state (the only runtime rotation switch
    // lives in book render()); reset it before any system frame (lock/wallpaper/
    // PIN/ticket/shutdown/restart), or portrait assets land on a landscape panel
    // — observed on hardware: sideways sleep wallpaper after a long power press,
    // misaligned shutdown frame. epd_set_rotation only mutates state, so each
    // caller's inherent present/full refresh stays the single refresh. The boot
    // gate bypasses this path and self-heals via init portrait + the lock art's
    // rotation check (the PMU drops the ESP rail for a full restart).
    if (epd_get_rotation() == EPD_ROT_LANDSCAPE) epd_set_rotation(EPD_ROT_INVERTED_PORTRAIT);
    if (feedback) app_lock_feedback(ctx->hl);
    // gen21：手动电源键（息屏/休眠）拥有最高优先级——武装期绝不拦截，改为静默
    // 取消武装（安全关射频）后正常息屏；唤醒后状态卡回 IDLE/「未开启」，闭环成立。
    // 自动锁屏/深睡（manual=false）保持 gen18 豁免保活（M1.5 关屏保活），不在此取消，
    // 仍交 enter_lock_and_sleep 拒锁。cancel_why 在未 IDLE 时才打
    // 「armed canceled by manual sleep」，重复调用幂等静默。
    // gen21: a manual power-key lock outranks the push arm - never refuse, just
    // silently disarm (tear the radio down) then sleep; on wake the status card
    // returns to IDLE/"未开启", closing the loop. Auto lock (manual=false) keeps
    // the gen18 exemption (M1.5 screen-off keepalive) and is still refused by
    // enter_lock_and_sleep; cancel_why only logs "armed canceled by manual sleep"
    // while not IDLE, so re-calling is idempotent and silent.
    if (manual && push_sink_armed()) {
        push_sink_cancel_why("manual sleep");
    }
    if (current->on_before_lock) current->on_before_lock(ctx);
    int tab = app_index_of(current);
    pico_resume_t resume = {.tab = tab >= 0 && tab < 4 ? (uint8_t)tab : 0};
    bool fullscreen = false;
    if (tab == 1 && app_book_resume_context(resume.path, sizeof(resume.path), &fullscreen)) {
        resume.reader = 1;
        resume.fullscreen = fullscreen;
    }
    (void)pico_boot_save_resume(&resume);
}
static void run_power_action(app_ctx_t *ctx, const app_desc_t *current, bool restart) {
    prepare_lock(ctx, current, false, false);  // 关机/重启走 app_enter_host_sleep，深睡路径自带 cancel（gen18 ③），此处 manual=false
    ui_power_final_draw(ctx->fb, restart);
    guard_draw_result(ctx->hl, update_display_full(ctx->hl));
    app_lock_wait_key_idle(800);
    epd_poweroff();
    pico_boot_clear_resume();
    if (restart) app_restart_host();
    esp_err_t error = app_enter_host_sleep(APP_SLEEP_OFF);
    if (error != ESP_OK) ESP_LOGW(TAG, "power off failed: %s", esp_err_to_name(error));
    // 关机交接失败时回到可操作原页；不关闭书籍后再重新自动开书。/ A failed shutdown returns to the operable current page without closing/reopening the book.
    app_present(ctx, current, APP_REDRAW_PAGE);
}

// ---- M 批 D/C 层：导航采样共用插点原语 -------------------------------------
// 一处实现，三处调用（D 封面批次 / C① 目录扫描 / C② present 段间），不三份漂移。
// 采样拿到的是 cst836u_read 的当前触点快照（最多两点、无 FIFO、每读一次即覆盖），
// 所以「按下」这条边沿必须自己记：只有「上次采样未按、这次采样按」才算一条命中，
// 与主循环 `pressed = touch.touched && !was_touched`（:1282）同语义。先例那套
// 「读到有指就算一次」在这里会失效——一只按住不放的手指会在每个插点各灌一次 MERGE。
// 计数分工：merged/dropped/commits 由仲裁层在「边沿→单槽」这一步产生，本层不补记；
// 本层只记采样侧诊断量（问了几次 / 几次低电平 / 几次真读 / 几条边沿 / 几条命中）。
// M-batch D/C shared hook: one implementation, three call sites. The driver hands back a
// contact snapshot with no FIFO, so the press edge has to live here: only a
// lift -> press transition counts, matching the loop's `pressed = touched &&
// !was_touched`. "Anything readable is a press" would let one held finger inject a
// MERGE at every call site. Arbitration counters stay in the arbiter; this layer only
// keeps sampling-side diagnostics.
typedef struct {
    bool     was_touched;               // 上次采样是否已按（边沿源，跨插点共用一份）
    uint32_t probes;                    // 问过几次（不管电平）
    uint32_t low;                       // 其中 INT 低电平（有指待读）次数
    uint32_t reads;                     // 其中真的走了 I2C 读坐标次数
    uint32_t edges;                     // 其中新按下沿次数
    uint32_t hits;                      // 其中命中底栏次数
    uint32_t masked;                    // 其中命中 index==1 被屏蔽的次数
    uint32_t multi;                     // 其中 count!=1 被跳过的次数
    uint32_t held;                      // 其中「主循环本拍已认得这只手」而不再计边沿的次数
    uint32_t by_src[APP_NAV_SRC_COUNT]; // 各插点采到的命中数（定位是哪一窗救回的）
} app_nav_probe_t;
static app_nav_probe_t s_nav_probe;

// 窗口计数清零；`was_touched` 必须留着——它是跨窗的触点状态，不是本窗计数。
// Clear window counters; keep `was_touched`, the contact state spans windows.
static void nav_probe_clear_window(void) {
    s_nav_probe.probes = 0;
    s_nav_probe.low = 0;
    s_nav_probe.reads = 0;
    s_nav_probe.edges = 0;
    s_nav_probe.hits = 0;
    s_nav_probe.masked = 0;
    s_nav_probe.multi = 0;
    s_nav_probe.held = 0;
    memset(s_nav_probe.by_src, 0, sizeof(s_nav_probe.by_src));
}

bool app_nav_probe(app_ctx_t *ctx, app_nav_src_t src) {
    if (ctx == NULL || ctx->tp == NULL) return false;
    app_nav_probe_t *p = &s_nav_probe;
    p->probes++;
    // 主循环这一拍已经认得这只手（去抖快照 touched=true）：边沿在它那边早消费过了，
    // 这里不能再算一次「新按下」——否则每次切页都会从那只还按着的手指里再折出一条
    // 幽灵命中，把真机对账的 hit 计数污染成「有切页就必然有命中」。
    // 快照在刷屏段内是陈旧的（段内不更新），所以它吞不掉段内新落下的指——那正是本插点
    // 要救的对象，held 与 hit 两个计数把这两类分得开。
    // The loop already knows this hand (debounced snapshot says touched): the edge was
    // consumed there. Without this, every switch would re-fold a phantom hit out of the
    // finger that caused it and poison the hit counter. The snapshot is stale inside a
    // blocking segment, so a finger that lands *during* the segment is still caught -
    // `held` versus `hit` keeps the two apart in the log.
    if (ctx->touch != NULL && ctx->touch->touched) {
        p->held++;
        p->was_touched = true;
        return false;
    }
    // ③ 电平门控：先问 INT 脚，低电平才走 I2C 读坐标；高电平顺手把边沿复位成「未按」，
    // 与主循环抬手后 `was_touched = false` 等价。窗内低电平计数进 probes/low 对账日志。
    // ③ Level gate first: only a low INT pin buys an I2C read; a high pin also resets the
    // edge to "not pressed", the same thing the loop does on release.
    if (cst836u_int_level(ctx->tp) != 0) {
        p->was_touched = false;
        return false;
    }
    p->low++;
    // 同一触点的后续探针不再占 I2C：边沿在首拍已经消费，抬手由上面的高电平分支复位。
    // A still-held contact already spent its edge; nothing to read until the pin lifts.
    if (p->was_touched) return false;
    cst836u_touch_t touch = { 0 };
    if (cst836u_read(ctx->tp, &touch) != ESP_OK) {
        p->was_touched = false;
        return false;
    }
    p->reads++;
    p->was_touched = touch.touched;   // ② 自建边沿：本次采样结果即下次比较的基准
    if (!touch.touched) return false;
    p->edges++;
    if (touch.count != 1) { p->multi++; return false; }
    // 横屏阅读要的是逻辑坐标；主循环在 debounce 之前做过同一次映射，这里必须同样做，
    // 否则 ui_nav_hit 拿到的是物理坐标（横屏下底栏判定直接失效）。
    // Landscape needs logical coords, the same remap the loop does before debounce.
    touch_native_to_logical(&touch);
    const int tab = ui_nav_hit(touch.x, touch.y);
    if (tab < 0) return false;
    p->hits++;
    if (tab == 1) { p->masked++; return false; }   // ④ index==1 屏蔽保留（:1155 既有语义）
    // ① 旁接仲裁：命中折叠进单槽，不写 ctx->request_app。merged/dropped 由仲裁层产生，
    // 窗口收敛后由主循环尾部一次性写唯一的家，消费点照常只切一次。
    // ① Fold into the arbiter slot instead of writing request_app here; the loop tail
    // commits the last finger once, and merged/dropped are counted by the arbiter.
    (void)nav_arb_on_hit(&s_nav_arb, tab);
    if (src >= 0 && src < APP_NAV_SRC_COUNT) p->by_src[src]++;
    ctx->user_activity = true;
    return true;
}

bool app_poll_main_navigation(app_ctx_t *ctx) {
    if (!ctx || !ctx->tp) return false;
    if (ctx->request_app) return true;
    // 命中已折叠进仲裁单槽（不再直接 ui_nav_request 写槽）：返回 true 让封面批次/汇合
    // 立刻让出 CPU，切页在采样间隔级的 40ms 收敛窗后补执行，不是丢掉。
    // The hit is folded, not committed: true asks the cover batch to yield now; the switch
    // lands after the 40ms settle window instead of being dropped.
    return app_nav_probe(ctx, APP_NAV_SRC_BATCH);
}

void app_loop_run(const app_loop_config_t* config) {
    // B62 启动版本戳横幅（26.11.4）。短码 `[113]` 只回答「版本号」，不回答「这一次构建」：
    // 26.11.3 实测同一版本号、只动 3 行注释，bin 就从 68be9ff5 变成 193b51c2，短码一声不响，
    // 版本归因只能靠「按钮字号已归一」这种行为反推。app_elf_sha256 是构建产物自带的
    // 32 B 指纹，同源同选项必同、任一字节变必异；打前 6 字节（12 hex），与交付件的
    // 「bin sha256 ↔ elf sha 前 12」对照表同桌对账。局部缓冲，不加静态量（Δbss 0）。
    // B62 boot banner: the short code answers "which version", never "which build". On
    // 26.11.3 a 3-line comment edit moved the bin hash with the version unchanged, and the
    // only way to attribute it was to infer from behaviour. The ELF sha256 is carried by
    // the build itself; six bytes (12 hex) is enough to tell two builds apart on one line.
    // 注意：这里**不许**打 __DATE__/__TIME__。首版打了编译时间，三树当场三异哈希
    // （体积一模一样）——时间戳进 .rodata 就等于把「同一份源码」变成三个不同的二进制，
    // 可复现性当场作废。版本归因靠 version + elf 指纹，不靠「几点编的」。
    // No __DATE__/__TIME__ here: the first cut printed them and the three trees produced
    // three different hashes at identical sizes. A timestamp in .rodata turns one source
    // into three binaries; identity comes from version + ELF fingerprint, not from the clock.
    {
        char elf_hex[13];
        const uint8_t *elf = esp_app_get_description()->app_elf_sha256;
        for (int i = 0; i < 6; ++i) snprintf(elf_hex + i * 2, 3, "%02x", elf[i]);
        ESP_LOGI(TAG, "build: ver=%s elf=%s", esp_app_get_description()->version, elf_hex);
    }

    s_presented_main = NULL;
    const app_desc_t* current = config->first_app != NULL
        ? config->first_app
        : app_home_page();

    cst836u_info_t touch_info = { 0 };
    cst836u_get_info(config->tp, &touch_info);
    cst836u_touch_t latest = { 0 };

    app_ctx_t ctx = {
        .hl = config->hl,
        .fb = config->fb,
        .acc = config->acc,
        .tp = config->tp,
        .sensor_ready = config->sensor_ready,
        .continuous_ready = continuous_du_init(),
        .touch = &latest,
        .touch_info = &touch_info,
    };

    bool was_touched = false;
    ui_quick_menu_reset(&s_quick);
    bool menu_open = false;
    ui_gesture_t gesture = {0};
    // M4-A：双指旁路。它只接管 count==2；单指识别器一行未改，只是在本层占用
    // 触摸的那些拍里不被喂入（"单指零接触" + "抬起一指不恢复成单指"）。
    // M4-A: the two-finger bypass. It owns count==2 only; the single-finger
    // recognizer is untouched - it just is not fed while this layer holds the touch.
    ui_dual_t dual = {0};
    ui_power_dialog_t power_dialog = {0};
    bool power_dialog_open = false;
    int menu_pressed = UI_MENU_HIT_NONE;
    menu_feedback_t feedback = {0};
    int menu_leaf = 0;
    const app_desc_t* return_app = NULL;
    int return_leaf = 0, return_menu_leaf = 0;
    bool return_to_menu = false;
    int held_key = -1;
    int64_t held_since_ms = 0;
    // 两枚戳，各归各的支（26.11.19 解耦批）：采样侧只写 last_sample_ms，按键侧只写
    // last_key_ms。以前只有一枚 last_lock_poll_ms，兼两用——那正是「取键才有采样」
    // 的形状。改名而不是新增，是为了让「这枚戳原本就属于门 tick」这件事留在名字外沿。
    // Two stamps, one per branch: sampling writes only last_sample_ms, keys only
    // last_key_ms. There used to be a single last_lock_poll_ms doing both jobs - the
    // very shape of "sampling only when someone takes keys".
    int64_t last_sample_ms = 0;
    int64_t last_key_ms = 0;
    int64_t last_font_retry_ms = 0;
    // #121: 字体重试闩锁——卡稳定挂载后退出周期重试，卡离位时重开（见主循环）。
    // #121: font retry latch - retires once the card stays mounted, rearms on removal (see the loop).
    bool font_retry_settled = false;
    int64_t last_media_poll_ms = 0;
    int64_t last_network_poll_ms = 0;
    int64_t last_input_ms = esp_timer_get_time() / 1000;
    uint32_t last_ble_input = ble_pt_input_serial();
    unsigned idle_minutes = app_settings_auto_lock_minutes();
    unsigned cover_policy = (unsigned)app_settings_main_refresh_mode() | ((unsigned)app_settings_system_contrast() << 8);
    read_pico_sd_info_t initial_media = {0};
    (void)read_pico_sd_get_info(&initial_media);
    bool media_mounted = initial_media.mounted || (ttf_font_ready() && !ttf_font_is_builtin());
    bool media_invalidated = false;
    app_lock_ignore_for(APP_LOCK_IGNORE_BOOT_MS);

    // The SD probe starts during board setup and is usually ready by the first product frame.
    // Retry here so the very first bookshelf paint already has a full CJK system face.
    try_load_saved_sd_font(false);
    rp_boot_mark("sdfont");
    if (current->on_enter != NULL) current->on_enter(&ctx);
    rp_boot_mark("enter");
    poll_media(&ctx, current, &media_mounted, &media_invalidated);
    // 首帧必须整屏 GC16：fb 里还是 app_main 画的开机图，DU 盖不掉。
    // First frame must be full GC16: fb still holds the splash; DU cannot cover it.
    app_present(&ctx, current, APP_REDRAW_FULL);
    rp_boot_mark("frame");
    pico_boot_ready();
    ESP_LOGI(TAG, "UI ready on %s", current->title);
    // 上次崩溃的现场（异常类型与触发地址）：有的话打一遍，串口和屏幕各有一份。
    // The previous panic scene, when there was one: logged here, and shown on the home screen.
    crash_note_log();
#ifdef KIIKO_LOCAL_LATENCY
    local_latency_init();
#endif

    nav_arb_reset(&s_nav_arb);  // M 批：仲裁状态显式归零（单槽空 / 计数清零）
    while (true) {
        unsigned requested_cover_policy = (unsigned)app_settings_main_refresh_mode() | ((unsigned)app_settings_system_contrast() << 8);
        if (requested_cover_policy != cover_policy) {
            app_home_cover_mode_changed(); app_book_cover_mode_changed();
            cover_policy = requested_cover_policy;
        }
        cst836u_touch_t touch = { 0 };
        esp_err_t err = cst836u_read(config->tp, &touch);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "CST836U read failed: %s", esp_err_to_name(err));
            memset(&touch, 0, sizeof(touch));
        }
        // 横屏阅读补一次坐标映射；竖屏恒等跳过。必须在 debounce 之前，latest/ctx.touch
        // 全链路（手势、快捷菜单、ui_nav_hit、各 app 的 on_touch）才拿到逻辑坐标。
        // Remap once for landscape reading; portrait is a no-op. Must run before
        // debounce so latest/ctx.touch carry logical coords for the whole chain
        // (gestures, quick menu, ui_nav_hit, every app's on_touch).
        touch_native_to_logical(&touch);

        const bool pressed = touch.touched && !was_touched;
        const bool released = !touch.touched && was_touched;
        debounce_touch(&latest, &touch, released);
        ctx.now_ms = esp_timer_get_time() / 1000;
        if (err == ESP_OK && (touch.touched || released)) last_input_ms = ctx.now_ms;
        if (ctx.now_ms - last_network_poll_ms >= 500) {
            last_network_poll_ms = ctx.now_ms;
            // ---- 26.9.18 S2-a：黑匣子的出口挪到主循环 -------------------------
            // 26.9.15 实测「13.8 s 才吐」，原因不是 `is_connected()` 迟钝，而是这个出口
            // 原本挂在 `app_time_service_poll()` 里，而后者被 `!touch.touched &&
            // 距上次输入 >= 10 s` 门控——**只要用户一直在摸屏幕，它就一直不执行**。
            // 现在挪到 500 ms 的网络轮询块里无条件调用，`poll()` 自己幂等且自己判时机。
            // 4 s 的物理窗口一过、主机一连上，最迟 0.5 s 就吐。
            // 26.9.18 S2-a: move the blackbox outlet into the main loop. The 26.9.15
            // "spilled at 13.8 s" was not a slow `is_connected()`: the outlet lived inside
            // `app_time_service_poll()`, which is gated on "no touch and 10 s since last
            // input" - a user who keeps poking the screen starves it forever. It now runs
            // unconditionally in the 500 ms block; `poll()` is idempotent and self-gating,
            // so the buffer spills at most 0.5 s after the host is heard.
            pico_bootbb_poll();
            read_pico_transfer_service_poll();
            app_time_service_poll(!touch.touched && ctx.now_ms - last_input_ms >= 10000 &&
                !current->holds_pmu && !usb_storage_active() && !pico_online_busy() && !ble_pt_connected());
            // gen26 AI 查词：轮询释义 UI 槽，来了就交阅读器出卡。沿用 media 分支的
            // 既定模式——出卡即呈现、丢弃本轮陈旧触摸、delay 后 continue：释义到达
            // 与触摸分发同一 Tick 撞车的窗口由这次丢弃吸收（与拔卡事件同口径）。
            // gen26 AI lookup: poll the definition UI slot and hand arrivals to
            // the reader. Same established pattern as the media branch - present
            // on arrival, discard this tick's stale touch, delay and continue:
            // the def-arrival/touch-dispatch same-tick window is absorbed by the
            // discard (same treatment as card-removal events).
            char ai_word[PUSH_AI_WORD_MAX + 1];
            // gen26.6.4：静态化。PUSH_AI_DEF_MAX 768→1200 若留在栈上会把 12KB 主任务
            // 栈再压 +432B；栈溢出的排障成本远高于 1.2KB 静态内存。单线程主循环、无
            // 重入，static 安全。
            // 26.11.3-C2：8,193 B 静态也不行了——内部 bss 只剩 112 B，这具接收槽
            // 落 PSRAM。它装的是任意长度的答案（词义 ≤1152 B，段落 ≤8192 B），
            // 按传输槽尺寸化是这里唯一诚实的尺寸。
            // gen26.6.4: static, not stack - widening DEF_MAX to 1200 would add +432B
            // to the 12KB main-task stack; a stack overflow costs far more than 1.2KB
            // of .bss. The main loop is single-threaded and non-reentrant.
            // 26.11.3-C2: 8,193 B static no longer fits either - internal bss had 112 B
            // left, so this receive slot lives in PSRAM. It holds an answer of any
            // length (word <= 1152 B, passage <= 8192 B); the transport slot is the
            // only honest size for it.
            static EXT_RAM_BSS_ATTR char ai_def[PUSH_AI_DEF_MAX + 1];
            if (push_sink_take_ai_def(ai_word, sizeof(ai_word), ai_def, sizeof(ai_def))) {
                // M3-B：情景答案**先认领**。0x08 帧没有情景标记（协议零改动），
                // 待命态按「词 + 发起页 + 期限」三重匹配认领；认领成功就只开情景
                // 视图，**不进生词批**——情景结果是四节分析，不是「这个词的释义」，
                // 落了库就是一条 340 字 blob 冒充释义（审计裁定的内容边界）。
                // M3-B: a scene answer is claimed FIRST. The 0x08 frame carries no
                // scene mark (zero protocol change); the pending state claims it by
                // word + originating page + deadline. A claimed answer only opens
                // the scene view and never enters the vocabulary batch - the scene
                // text is a four-section analysis, not "this word's definition",
                // and logging it would plant a 340-char blob where a definition
                // belongs (the ruling's content boundary).
                if (app_book_scene_def_ready(ai_word, ai_def)) {
                    was_touched = touch.touched;
                    // 词卡→情景卡是版面跨度大的切换，一次 GC16 打底（双 GC16 家族）。
                    // The card -> scene-card jump is a wide layout change: one GC16 base.
                    app_present(&ctx, current, APP_REDRAW_FULL);
                    vTaskDelay(pdMS_TO_TICKS(LOOP_TICK_MS));
                    continue;
                }
                // gen26.5 M2 Step8：释义回流先进 AI 生词批，再走弹卡。两个动作解耦——
                // 弹卡被门 A 丢弃（其他面板开着）时，落库照旧：交互已经发生，
                // 「与 AI 交互过的词全落」不因 UI 状态而改变。批里没这个词
                // （早已落盘或从未入批）时 set_def 静默忽略，释义留在预取缓存。
                // gen26.5 M2 Step8: the returning definition feeds the AI vocabulary
                // batch BEFORE the card. The two are decoupled - when gate A drops
                // the card (another panel is open), logging still happens: the
                // interaction occurred, and "every AI-interacted word logs" does not
                // bend to UI state. A word not in the batch (already flushed or
                // never queued) is silently ignored; the definition stays in the
                // prefetch cache.
                vocab_log_ai_set_def(ai_word, ai_def);
                if (app_book_ai_def_ready(ai_word, ai_def)) {
                    was_touched = touch.touched;
                    // M2 Step9：本轮回流若是开详解视图（门 A 放行 + 折行触顶），走一次
                    // GC16 全刷打底；词面→详解页的版面跨度大，局部刷新会留残影。
                    // 其余回流（卡面刷新）维持局部刷新档位不变。app_book_ai_def_full
                    // 是一次性查询：开详解时置位、此处消费，下一轮回流自动回落 PAGE。
                    // M2 Step9: if this return flow opens the detail view (gate A
                    // passed + wrapped text hit the cap), take one GC16 FULL refresh
                    // to lay the base; the word-card -> detail-page layout jump is
                    // wide enough that a partial update would ghost. Every other
                    // return flow (card refresh) keeps its partial-update gear.
                    // app_book_ai_def_full is a one-shot query: set when the detail
                    // view opens, consumed here, so the next flow falls back to PAGE.
                    app_present(&ctx, current,
                                app_book_ai_def_full() ? APP_REDRAW_FULL : APP_REDRAW_PAGE);
                    vTaskDelay(pdMS_TO_TICKS(LOOP_TICK_MS));
                    continue;
                }
            }
            // gen26.5 M2 Step8：AI 生词批合批驱动。首次入队满 5s 才真正写卡，
            // 没到点或没批都是零开销的两次比较。/ gen26.5 M2 Step8: drive the AI
            // vocabulary batch. Writes only once 5s after the first queued word;
            // with no batch or before the deadline it is two comparisons, free.
            vocab_log_ai_flush();
        }
        ctx.pressed = pressed;
        ctx.released = released;
        ctx.consumed = false;
        // 26.9.13（审计裁定：升成策略位）：**单指优先每拍只下发一次**，写进 ctx 与
        // 双指状态机各自那一份，后面两个识别器都只读这份结果、谁都不再自己判一遍。
        // 26.9.12 的写法是「在双指块里给 dual.allow_suppress 赋值」，而单指识别器另走
        // 一条完全独立的 `count > 1 → CANCEL` 老路——同一个意图两个判据，正是本仓库
        // 反复记在台账上的那类缺陷。现在一个位管两件事：
        //   ① dual.single_finger_priority → ui_dual_engaged() 恒假，双指态不饿死单指；
        //   ② ctx.single_finger_priority  → ui_gesture_feed() 遇 count>1 不再取消 TAP。
        // 没实现该回调的页面维持默认（false = 允许抑制、多点照旧取消），与本改动前
        // 逐字等价——这一条对全仓库其余页面是**零行为变化**的保证。
        // **位置也是内容**：这一段必须放在「本拍刚拍下快照」的地方，而不是紧贴双指块。
        // 放在 13 KB 的 app_loop_run 后半段（紧挨 ui_dual_feed）时，多出来的这一次间接
        // 调用会把一大票活跃变量挤到栈上，实测让整个函数胖 **588 B**；挪到拍子开头
        // （此处活跃变量最少）后整个函数反而比 26.9.12 **小 40 B**。同一份逻辑，两个位置
        // 差 628 B——这不是优化技巧，是把代价放回它该在的地方（见 26.9.13 体积对账 §三）。
        // **Placement is content**: this has to sit where the tick's snapshot is taken,
        // not next to the dual block. Placed late in the 13 KB app_loop_run (right before
        // ui_dual_feed), the extra indirect call spilled a pile of live values and the
        // function grew **588 B**; moved to the head of the tick (fewest live values) the
        // whole function ends up **40 B smaller** than 26.9.12. Same logic, 628 B apart -
        // not a trick, just paying the price where it belongs (see the size report §3).
        ctx.single_finger_priority =
            current->single_finger_priority != NULL && current->single_finger_priority(&ctx);
        dual.single_finger_priority = ctx.single_finger_priority;
        // A load may have captured navigation after the prior switch. Consume it at the normal boundary.

        ctx.request_menu = false;
        ctx.request_return = false;
        bool selected_from_menu = false;
        const bool power_input_owned = power_dialog_open;
        bool quick_lock_requested = false;
        bool quick_input_owned = false;

        if (ctx.now_ms - last_media_poll_ms >= MEDIA_POLL_INTERVAL_MS) {
            last_media_poll_ms = ctx.now_ms;
            bool media_event = poll_media(&ctx, current, &media_mounted, &media_invalidated);
            // #121: 卡不在位（拔卡/ USB 存储/ 尚未挂载）即重开字体重试，重插后由下方
            // 重试块补载保存字体。必须放在 continue 之前：拔卡那一轮会整体跳过重试块。
            // #121: any card absence rearms the font retry so the block below reloads
            // the saved face after a reinsert. This must precede the continue - a loss
            // iteration skips the retry block entirely.
            if (!media_mounted) font_retry_settled = false;
            if (media_event) {
                power_dialog_open = false;
                ui_power_dialog_reset(&power_dialog, touch.touched);
                ui_quick_menu_reset(&s_quick);
                held_key = -1;
                cancel_gesture(&ctx, current, &gesture);
                menu_pressed = UI_MENU_HIT_NONE;
                ctx.consumed = true;
                ctx.pressed = ctx.released = false;
                was_touched = touch.touched;
                if (menu_open) present_menu(&ctx, current, menu_leaf, &feedback);
                else app_present(&ctx, current, APP_REDRAW_FULL);
                // 本轮旧输入不再触发动作，下一轮恢复正常事件处理。
                // Discard this tick's stale input, then resume normal dispatch next tick.
                vTaskDelay(pdMS_TO_TICKS(LOOP_TICK_MS));
                continue;
            }
        }

        if (!power_input_owned && !menu_open) {
            if (err != ESP_OK) ui_quick_menu_cancel_input(&s_quick);
            else {
                ui_quick_action_t action;
                quick_input_owned = ui_quick_menu_feed(&s_quick, &ctx,
                    current->render && !current->holds_pmu && !usb_storage_active() && !pico_online_busy(), &action);
                if (quick_input_owned) {
                    held_key = -1;
                    cancel_gesture(&ctx, current, &gesture);
                    ctx.consumed = true;
                }
                if (action == UI_QUICK_OPEN || action == UI_QUICK_CLOSE) quick_present(&ctx, current, false);
                else if (action == UI_QUICK_WIFI) {
                    quick_toggle_wifi();
                    quick_present(&ctx, current, false);
                } else if (action == UI_QUICK_BLUETOOTH) {
                    app_settings_set_ble_turner(!app_settings_ble_turner());
                    if (!app_settings_ble_turner()) {
                        // gen24.2 块 2：快捷层与设置页是同一设置项的两个写入口，
                        // B 模型下关开关的行为必须一致——不收摊就是「关了还在
                        // 推」的静默后门（gen24 真机 109.27s 同款现场）。
                        // Block 2: the quick layer and the settings page are two
                        // writers of the same setting; under model B the
                        // switch-off behaviour must match - skipping the
                        // close-up here is the silent back door of "off but
                        // still pushing" (the same gen24 hardware scene at
                        // 109.27s).
                        push_sink_cancel_why("蓝牙已关闭");
                        ble_pt_reset_failure();
                    }
                    quick_present(&ctx, current, false);
                } else if (action == UI_QUICK_FULL) quick_present(&ctx, current, true);
                else if (action == UI_QUICK_LOCK) quick_lock_requested = true;
                else if (action == UI_QUICK_BOX) {
                    // 按钮只表达意图：能不能进由页面裁决（非阅读态/竖排返回 NONE），
                    // 菜单无论如何先收起，避免「按钮亮着但页面没动」的悬空态。
                    // The button states intent only; the page decides whether the mode
                    // applies (NONE outside the reader or in the vertical setting), and the
                    // menu always closes so the button never looks stuck on.
                    ui_quick_menu_reset(&s_quick);
                    if (current->on_quick_box) {
                        app_redraw_t box_redraw = current->on_quick_box(&ctx);
                        if (box_redraw != APP_REDRAW_NONE)
                            present_page(&ctx, current, &gesture, box_redraw);
                    }
                }
            }
        }

        if (power_input_owned && err == ESP_OK) {
            ctx.consumed = false;
            EpdRect dirty;
            ui_power_action_t action = ui_power_dialog_feed(&power_dialog, &ctx, &dirty);
            if (action == UI_POWER_ACTION_NONE && dirty.height > 0) {
                ui_power_dialog_draw_controls(ctx.fb, &power_dialog);
                enum EpdDrawError result = update_display_area_diff_with(
                    ctx.hl, &E0470_FOLLOW_WAVEFORM, MODE_DU, dirty);
                guard_draw_result(ctx.hl, result);
                if (result != EPD_DRAW_SUCCESS) {
                    power_dialog_open = false;
                    ui_power_dialog_reset(&power_dialog, touch.touched);
                    app_present(&ctx, current, APP_REDRAW_FULL);
                }
            } else {
                if (action == UI_POWER_ACTION_CANCEL) {
                    power_dialog_open = false;
                    ui_power_dialog_reset(&power_dialog, false);
                    if (menu_open) present_menu(&ctx, current, menu_leaf, &feedback);
                    else app_present(&ctx, current, APP_REDRAW_PAGE);
                } else if (action == UI_POWER_ACTION_SHUTDOWN) {
                    if (!app_settings_staged_shutdown()) {
                        power_dialog_open = false;
                        ui_power_dialog_reset(&power_dialog, false);
                        run_power_action(&ctx, current, false);  // 走 app_enter_host_sleep，深睡路径自带 cancel（"关机/深睡，接收结束"），此处 manual=false
                    } else {
                        power_dialog_open = false;
                        ui_power_dialog_reset(&power_dialog, false);
                        prepare_lock(&ctx, current, true, true);
                        extern const app_desc_t app_book;
                        main_lock_boundary();
                        enter_lock_and_sleep(ctx.hl, &s_lock_ignore_until_ms, ctx.acc, ctx.tp,
                                             current == &app_book && app_book_reader_body_visible());
                        display_request_full_refresh();
                        ctx.now_ms = esp_timer_get_time() / 1000;
                        last_input_ms = ctx.now_ms;
                        poll_media(&ctx, current, &media_mounted, &media_invalidated);
                        last_media_poll_ms = ctx.now_ms;
                        ctx.pressed = false;
                        ctx.released = false;
                        app_present(&ctx, current, APP_REDRAW_PAGE);
                    }
                } else if (action == UI_POWER_ACTION_RESTART) {
                    power_dialog_open = false;
                    ui_power_dialog_reset(&power_dialog, false);
                    run_power_action(&ctx, current, true);
                }
            }
            ctx.consumed = true;
        }

        if (err != ESP_OK) {
            held_key = -1;
            // 读点失败：双指层与单指识别器一起回到静止，避免残留半截手势。
            // A failed read: the dual layer returns to rest with the single-finger
            // recognizer so no half-finished gesture survives.
            ui_dual_reset(&dual);
            cancel_gesture(&ctx, current, &gesture);
            EpdRect dirty;
            ui_power_dialog_cancel_input(&power_dialog, &dirty);
            if (power_dialog_open && dirty.height > 0) {
                ui_power_dialog_draw_controls(ctx.fb, &power_dialog);
                guard_draw_result(ctx.hl, update_display_area_diff_with(
                    ctx.hl, &E0470_FOLLOW_WAVEFORM, MODE_DU, dirty));
            }
            ctx.consumed = true;
            if (menu_pressed >= 0) menu_feedback(&ctx, current, menu_leaf, menu_pressed, false, &feedback);
            menu_pressed = UI_MENU_HIT_NONE;
        }
        EpdRect click_area;
        if (ui_click_feedback_active() && (err != ESP_OK || released || touch.count != 1 ||
            quick_input_owned || power_input_owned || menu_open ||
            ui_click_feedback_cancel_at(touch.x, touch.y))) {
            if (ui_click_feedback_release(ctx.fb, &click_area)) click_present(&ctx, current, click_area);
        }
        // 主页面底栏在首次单指报点就确认；抬手、轻滑和停留不再重复切页。
        // Commit root navigation on the first single contact; lift, small motion and holds never repeat it.
        bool nav_press_owned = false;
        bool nav_folded = false;  // M 批③：本 tick 是否为窗口内被折叠/丢弃的底栏命中
        if (err == ESP_OK && pressed && touch.count == 1 && !power_input_owned &&
            !quick_input_owned && !menu_open && !usb_storage_active() && !ctx.request_app &&
            main_page_ready(&ctx, current)) {
            const int tab = ui_nav_hit(touch.x, touch.y);
            if (tab >= 0) {
                held_key = -1;
                cancel_gesture(&ctx, current, &gesture);
                ctx.consumed = true;
                ctx.user_activity = true;
                nav_press_owned = true;
                if (tab != app_index_of(current)) {
                    // ① 入队仲裁 + ② 出队合并兜底：命中先折叠进单槽/计数，不各自写
                    // request_app。窗口收敛后由尾部 tick 块一次性写入唯一的家，只切一次。
                    // 合并/丢弃计数在仲裁层(边沿→单槽)产生，随收敛日志输出，不在消费点补记。
                    nav_arb_action_t act = nav_arb_on_hit(&s_nav_arb, tab);
                    nav_folded = (act != NAV_ARB_OPEN);  // 采纳指回显，折叠/丢弃指不回显
                }
            }
        }
        if (err == ESP_OK && pressed && touch.count == 1 && !power_input_owned &&
            !quick_input_owned && !menu_open && !usb_storage_active() &&
            !nav_folded &&  // M 批③：窗口内被覆盖/丢弃指不逐指回显；采纳指单次回显
            ui_click_feedback_press(ctx.fb, touch.x, touch.y, &click_area))
            click_present(&ctx, current, click_area);
        // 双指期间**不进**单指按下大分支（M4-A v2）：里面会拿 points[0] 去撞按键
        // 热区（ui_key_hit_test）、跑菜单与旧页 on_touch——这些路径没有 count 守卫，
        // 双指落下那一拍会透传成一次「按下」。`count==1` 与既有 8 处守卫同构；
        // `!ui_dual_engaged` 再覆盖「双指后剩一指」的拍（硬语义 2）。
        // 单指不因此被挂起：count==1 时照常派发，零延迟。
        // Two fingers never enter the single-finger press branch (M4-A v2): that
        // branch feeds points[0] into key hit-testing, the menu and legacy on_touch -
        // none of which guard on count, so the landing tick leaked through as a press.
        // `count==1` matches the eight existing guards; `!ui_dual_engaged` also covers
        // the tick after one finger lifts (hard rule 2). Single-finger input is never
        // suspended: with count==1 it dispatches as usual, with zero delay.
        if (!power_input_owned && !quick_input_owned && !nav_press_owned && pressed &&
            err == ESP_OK && touch.count == 1 && !ui_dual_engaged(&dual)) {
            held_key = -1;
            const int key = ui_key_hit_test(touch.x, touch.y);
            const bool handle_hit = ui_menu_handle_hit_test(touch.x, touch.y) &&
                (!current->menu_handle_enabled || current->menu_handle_enabled(&ctx));
            ctx.consumed = true;
            if (key >= 0 || handle_hit) {
                cancel_gesture(&ctx, current, &gesture);
                if (menu_pressed >= 0) menu_feedback(&ctx, current, menu_leaf, menu_pressed, false, &feedback);
                menu_pressed = UI_MENU_HIT_NONE;
            }
            if (key >= 0 && current->owns_keys && !menu_open) {
                // M2 Step9：详解会话活跃时，KEY_2 短按豁免 defer_middle_short——直达
                // app_book 的 on_key 关闭详解页。否则短按会被中键菜单吞掉，菜单盖住
                // 详解页且失去关闭入口。豁免只查 app_book 的 detail_active 谓词；
                // 非详解态（含其他 app）条件与原来逐字等价，延迟菜单行为不变。
                // 详解态的输入隔离（app_book 五处入口）保证会话期内切不出 app_book，
                // 故谓词为真时 current 必为 app_book，豁免不会误伤别的 app。
                // M2 Step9: while a detail session is live, a short KEY_2 press is
                // exempt from defer_middle_short - it goes straight to app_book's
                // on_key to close the detail page. Otherwise the middle-key menu
                // would swallow the press, covering the detail page and losing the
                // close affordance. The exemption consults only app_book's
                // detail_active predicate; outside a detail session (including for
                // other apps) the condition is byte-for-byte the old one, so
                // deferred-menu behavior is unchanged. The detail session's input
                // isolation (app_book's five entry points) keeps the user inside
                // app_book for its lifetime, so a true predicate implies
                // current == app_book and the exemption cannot misfire elsewhere.
                const bool ai_detail = app_book_ai_detail_active();
                app_redraw_t redraw = current->on_key &&
                    !(key == UI_KEY_2 && current->defer_middle_short && !ai_detail)
                    ? current->on_key(&ctx, key) : APP_REDRAW_NONE;
                present_page(&ctx, current, &gesture, redraw);
                // M2 Step9：豁免过的 KEY_2 短按不再挂 held_key。压下时 on_key 已直达
                // 关闭详解；若照旧挂键，抬手时下面的 defer 释放块会二次调用 on_key——
                // 那一刻详解已关，KEY_2 会落到原书路径（工具面板/按键设置），多弹一个
                // 面板。详解态 KEY_2 长按语义=关闭，由压路直达已经满足，无需挂键。
                // M2 Step9: an exempted KEY_2 short press no longer arms held_key.
                // on_key already ran straight to close the detail view on press;
                // arming anyway would make the deferred release block below call
                // on_key a second time - by then the detail view is closed and
                // KEY_2 would land on the source path (tools panel / key action),
                // popping an extra panel. Long-press KEY_2 in a detail session
                // means "close" too, which the press path already satisfies.
                if (current->on_key_long && touch.count == 1 && !(key == UI_KEY_2 && ai_detail)) {
                    held_key = key;
                    held_since_ms = ctx.now_ms;
                }
            } else if (key == UI_KEY_2) {
                if (menu_open) {
                    feedback.updates = 0;
                    ui_draw_menu_page(ctx.fb, current, menu_leaf);
                    guard_draw_result(ctx.hl, update_display_full(ctx.hl));
                } else if (current->present != NULL && current->present(&ctx, APP_REDRAW_FULL)) {
                } else if (current->render != NULL) {
                    current->render(&ctx, ctx.fb);
                    guard_draw_result(ctx.hl, update_display_full(ctx.hl));
                }
            } else if ((key == UI_KEY_3 || handle_hit) && !usb_storage_active()) {
                menu_open = !menu_open;
                if (menu_open) {
                    menu_leaf = ui_menu_leaf_for_app(current);
                    present_menu(&ctx, current, menu_leaf, &feedback);
                } else app_present(&ctx, current, APP_REDRAW_PAGE);
            } else if (key >= 0 && menu_open) {
                int next = menu_leaf + (key == UI_KEY_1 ? -1 : 1);
                if (next >= 0 && next < ui_menu_leaf_count()) {
                    menu_leaf = next;
                    present_menu(&ctx, current, menu_leaf, &feedback);
                }
            } else if (key >= 0) {
                app_redraw_t redraw = current->on_key ? current->on_key(&ctx, key) : APP_REDRAW_NONE;
                if (redraw == APP_REDRAW_NONE)
                    ESP_LOGI(TAG, "KEY%d pressed, no action bound", key + 1);
                present_page(&ctx, current, &gesture, redraw);
            } else if (menu_open) {
                int hit = ui_menu_hit_test(touch.x, touch.y, menu_leaf);
                if (hit == UI_MENU_HIT_PREV || hit == UI_MENU_HIT_NEXT) {
                    menu_leaf += hit == UI_MENU_HIT_NEXT ? 1 : -1;
                    present_menu(&ctx, current, menu_leaf, &feedback);
                } else if (hit >= 0 && touch.count == 1) {
                    menu_pressed = hit;
                    menu_feedback(&ctx, current, menu_leaf, hit, true, &feedback);
                }
            } else if (current->on_gesture) {
                ctx.consumed = false;
            } else {
                // 旧页按下先收到 consumed=true，仅 NONE 为同轮 tick 放行。
                // Legacy press sees consumed=true; only NONE clears it for this tick.
                app_redraw_t redraw = current->on_touch
                    ? current->on_touch(&ctx, &latest) : APP_REDRAW_NONE;
                app_present(&ctx, current, redraw);
                if (redraw == APP_REDRAW_NONE) ctx.consumed = false;
            }
        }
        if (err == ESP_OK) was_touched = touch.touched;
        if (!power_input_owned && !quick_input_owned && menu_open && menu_pressed >= 0) {
            int hit = ui_menu_hit_test(latest.x, latest.y, menu_leaf);
            if (touch.count > 1 || hit != menu_pressed || released) {
                int chosen = menu_pressed;
                menu_feedback(&ctx, current, menu_leaf, chosen, false, &feedback);
                menu_pressed = UI_MENU_HIT_NONE;
                if (released && err == ESP_OK && hit == chosen) {
                    menu_open = false;
                    ctx.request_app = app_at(chosen);
                    selected_from_menu = true;
                    if (ctx.request_app == current) app_present(&ctx, current, APP_REDRAW_PAGE);
                }
            }
        }
        // 单指优先策略位已在本拍开头（拍快照处）下发，见那里的长注释。
        // The single-finger priority bit was applied at the head of this tick (where the
        // snapshot is taken); see the long note there.

        // ---- M4-A 双指旁路：与 on_gesture 同构，但只吃 count==2 ----
        // 双指事件先于单指结算：单指早已照常即时派发（不挂起、零延迟），这里
        // 只是把「双指成立」这件事交给页面去撤销/执行。
        // M4-A two-finger bypass: shaped like on_gesture but fed only count==2.
        // It settles before the single-finger branch because the single finger has
        // already dispatched in real time (no suspension, no delay); this merely
        // hands the page the fact that two fingers are now established.
        if (!power_input_owned && !quick_input_owned && !menu_open && !usb_storage_active() &&
            current->on_dual_gesture && err == ESP_OK) {
            ui_dual_event_t dev = {0};
            // 策略位已在上面每拍下发过（见那段注释），这里不再重设——双指块可能因为
            // 菜单/自检/USB 被整段跳过，而策略位必须对「这一刻的页面」成立。
            // The policy bit was applied above for this tick; nothing to re-apply here.
            // This block can be skipped wholesale (menu, self-test, USB) while the bit
            // still has to describe the page as it is right now.
            const bool fired = ui_dual_feed(&dual, &ctx, &dev);
            // 抬手判定这一拍**必有日志**（含「判了但丢弃」）：dx/dy/dist/axis/steps
            // 是「偶尔串扰」的照妖镜。上一轮这里完全无打点，只能靠猜；有了它，
            // 下一轮任何「偶尔」都能直接用数字定位，A 项复议也有据可依。
            // Every lift verdict logs, including "judged and dropped": these numbers
            // are the only instrument for an *occasional* cross-talk. The previous
            // round had none and could only guess.
            if (fired || dev.judged) {
                // 距离先算成变量再进日志：宿主桩把 ESP_LOGI 定义成空宏、**不求值参数**，
                // 直接写在宏里会让 dual_dist_px 在宿主构建中变成「定义未使用」而
                // -Werror 报错。提出来 + (void) 兼顾两端。
                // Compute the distance before the log: the host stub defines ESP_LOGI as
                // an empty macro that does **not** evaluate its arguments, so inlining the
                // call would leave dual_dist_px unused under -Werror on the host build.
                const int dist = dual_dist_px(dev.dx, dev.dy);
                (void)dist;
                const char *end_name = dual_end_name(dev.end);
                (void)end_name;
                // 每行自带身份（26.9.7）：分析正则从 `dual:` 改成 `\] dual:`／`dual` 仍可。
                // Self-identifying line (26.9.7); the offline regexp keys on `dual:` as before.
                const char *fw = fw_tag();
                (void)fw;
                // gap0==0 打标：两指间距不该为 0（物理上不可能同像素），出现即驱动幻点/
                // 重复点。日志带 gap0zero=1 供离线统计把它排除出 gap0 分布，防污染 SEL_DIST。
                // Tag gap0==0: two contacts cannot share a pixel, so 0 means a driver
                // phantom/duplicate. The marker lets offline stats exclude it from the
                // gap0 distribution and keep SEL_DIST clean.
                // 26.9.6 **现象位**打标：lift0=1 = 这次手势期间**见到过病灶帧**（芯片
                // 仍报 active、坐标却全零）。语义是「这条命是从那条缝里钻进来的」，
                // 与判定结论无关。26.9.4 及以前这种帧会把中点记成 (0,0)，派生出凭空的
                // dist=√(x0²+y0²) 伪位移与只看落点的随机轴比——真机上表现为「有时候」
                // 翻屏/调字号（round2 占 48%、round3 占 32.7%）。26.9.6 起这类帧被判为
                // 无效触点、冻结 x_last，不再污染任何派生量。
                // 判读口径（26.9.7 按审计裁定修正）：lift0 **不是「修复成立」的必需项**。
                // 判据信位置不信 active，芯片若发的是诚实型归零帧（active 已假），标天然
                // 全无——那正是修复成立的样子。四行矩阵见验收 README：
                //   启动串 → 零误发射 → lift0 有/无；
                // 「零误发射 + lift0 全无」= 修复成立（未遇靶帧或芯片诚实），不是可疑。
                // 要证明「保护机制在跑」，看 freeze_n（不依赖 active 语义），不看 lift0。
                // 要看「病灶帧到底来没来」又不被 active 骗，看 `end`（26.9.7 新增）：
                // `end=zero` = count 仍 >=2 却没有两个可用触点，那条缝的收尾指纹，两种芯片都算；
                // `end=lift` = 两指真的都抬起（正常）；`end=one` = 先抬起一指（正常）。
                // To prove the guard runs, read freeze_n. To see whether a sick frame
                // actually arrived without being fooled by `active`, read `end` (26.9.7):
                // `zero` = count>=2 with no usable pair (the seam's fingerprint, either
                // chip kind), `lift` = both really gone, `one` = one finger lifted first.
                // How to read it (corrected in 26.9.7 by adjudication): lift0 is NOT a
                // prerequisite for "the repair holds". The predicate trusts position and
                // never consults `active`, so an honest chip (active already false) leaves
                // this flag dark - the shape of a working repair. The four-row matrix lives
                // in the acceptance README: boot string -> zero mis-fires -> lift0 on/off,
                // where "zero mis-fires + lift0 absent" means the repair holds, not that
                // something is suspicious. To prove the guard actually runs, read freeze_n.
                // freeze_n：本次手势因「有效点 <2」冻结 x_last 的帧数（26.9.7 佐证计数）。
                // 它不查 active，撒谎型/诚实型归零帧都计，故能补 lift0 的盲区；真抬手那
                // 一帧也计，因此任何手势至少为 1——它证明机制在跑，不证明遇到了病灶。
                // freeze_n: frames of this gesture that froze x_last for having fewer than two
                // valid points (26.9.7). It never consults `active`, so it counts honest and
                // lying zeroing frames alike and covers lift0's blind spot. The real lift
                // tick counts too, so every gesture scores at least 1: it proves the guard
                // runs, not that a sick frame was met.
                ESP_LOGI(TAG, "%s dual: gap0=%d mid=(%d,%d) dx=%d dy=%d dist=%d axis=%d steps=%d type=%d end=%s freeze_n=%d%s%s",
                         fw, dual.gap0, dual.x0, dual.y0, dev.dx, dev.dy, dist, dual.axis,
                         dev.steps, (int)dev.type, end_name, dev.freeze_n,
                         dual.gap0 == 0 ? " gap0zero=1" : "",
                         dev.lift0 ? " lift0=1" : "");
            }
            // 26.9.7 静默中止打日志（审计裁定⑤）：以前 ABORT 什么都不说，日志里只剩
            // 「什么都没发生」——round-4 的「66 落定 / 65 判定」缺的那 1 笔就是一次静默
            // 中止，没有日志就只能猜。现在每道门报名，`reason=` 直接指出是哪一道关的。
            // 26.9.7 silent aborts get a line (adjudication 5): ABORT used to say nothing,
            // leaving only "nothing happened" in the log - round 4's missing 1-in-66 was
            // one of these, and with no line to read the only honest move was a guess.
            // Every gate now names itself, so `reason=` says which one closed.
            if (!fired && dev.abort != UI_DUAL_ABORT_NONE) {
                const char *fw = fw_tag();
                (void)fw;
                const char *end_name = dual_end_name(dev.end);
                (void)end_name;
                const char *why = dual_abort_name(dev.abort);
                (void)why;
                ESP_LOGI(TAG, "%s dual abort: reason=%s end=%s saw_zero=%d gap0=%d dt=%d",
                         fw, why, end_name, (int)dual.saw_zero, dual.gap0,
                         (int)(ctx.now_ms - dual.t0_ms));
            }
            if (fired) {
                // 26.9.7 幽灵 TAP 修复（审计裁定①）：RETRACT 那一刻**同步撤销单指识别器**。
                // 根因是「饥饿不是撤销」：第一指落下就起了 PRESS，第二指落下后 `!ui_dual_engaged`
                // 守卫把拍子全挡住——识别器不再被喂，却**没有收到任何撤销**，那个 PRESS 一直
                // 挂着（从未收到 MOVE，moved=false）。两指抬起后 engaged 归假，识别器重新被
                // 喂到 count==0 的拍子，就把这条陈旧 PRESS 按 TAP 收尾 → action_at()，落在
                // 书架上是开书、落在工具条上是点按钮。round-4 的 29 笔紧贴判定后 7–61ms
                // （中位 24ms = 1–2 个节拍）在人手上不可能出现。
                // 语义上这不是补丁而是本来的含义：RETRACT 是双指落定那一拍发出的，所以
                // 「双指成立即撤销」与「这次按压从未发生」是同一句话——cancel_gesture 正是
                // 中断边界用的那个函数，页面据此收回按下装饰，识别器归零。
                // Round-4 ghost TAP repair (adjudication 1): cancel the single-finger
                // recognizer the moment RETRACT goes out. The root cause is "starvation is
                // not revocation": the first finger started a PRESS, the second finger's
                // arrival made `!ui_dual_engaged` skip every tick - the recognizer stopped
                // being fed but was never told to forget, so the stale PRESS (never moved,
                // moved==false) sat there. Once both fingers left, engaged went false, the
                // recognizer was fed a count==0 tick and closed that PRESS as a TAP ->
                // action_at(): opening a book from the shelf, pressing a toolbar button.
                // Round 4 logged 29 of these 7-61ms (median 24ms, 1-2 ticks) after the
                // verdict - a human hand cannot do that. Semantically this is not a patch
                // but the original meaning: RETRACT is emitted on the landing tick, so
                // "two fingers established means revoke" and "this press never happened"
                // are the same sentence, and cancel_gesture is exactly the interrupt-
                // boundary routine the rest of the loop uses.
                if (dev.type == UI_DUAL_EVT_RETRACT) cancel_gesture(&ctx, current, &gesture);
                app_redraw_t redraw = current->on_dual_gesture(&ctx, &dev);
                present_page(&ctx, current, &gesture, redraw);
                if (redraw != APP_REDRAW_NONE) ctx.consumed = true;
            }
        }
        if (!power_input_owned && !quick_input_owned && !menu_open && current->on_gesture && err == ESP_OK
            && !ui_dual_engaged(&dual)) {
            ui_gesture_event_t event;
            if (ui_gesture_feed(&gesture, &ctx, &event)) {
                app_redraw_t redraw = current->on_gesture(&ctx, &event);
                present_page(&ctx, current, &gesture, redraw);
                if (redraw != APP_REDRAW_NONE) ctx.consumed = true;
            }
        }

        ctx.now_ms = esp_timer_get_time() / 1000;
        rails_idle_check(ctx.now_ms);

        // ---- 采样支（26.11.19 解耦批）：值链的 200 ms 节拍归主循环 ----
        // 以前电量与充电状态的新鲜度寄生在取键动作里（`take_key_action` 内部拉整份
        // 快照），没人取键就不刷新——插线充电态最明显。现在采样有自己的戳，
        // 取键只负责取键；两者共用同一条总线，所以共用同一份门谓词（pmu_bus.h）。
        // Sampling branch: the value chain's 200 ms cadence now belongs to the loop.
        // Freshness used to ride inside key-taking; with no key there was no refresh.
        // It has its own stamp now. Both sides share one bus, so they share one gate.
        //
        // 戳分两枚、各写各的：采样支只推自己那枚，按键支只推自己那枚（§5.2 守卫
        // 按出现次数与 due 配对咬串写，不靠行号）。
        // Two stamps, each advanced on its own side; the §5.2 guard counts
        // occurrences and due-call pairings rather than line numbers.
        const bool pmu_ready_now = read_pico_pmu_ready();
        const bool pmu_bus_ok = pmu_bus_static_ok(pmu_ready_now, current->holds_pmu, usb_storage_active());
        if (pmu_bus_ok && pmu_bus_due(ctx.now_ms, last_sample_ms)) {
            last_sample_ms = ctx.now_ms;
            (void)read_pico_pmu_poll();
        }

        // 电源键短按默认锁屏；阅读正文可选择将它用于下一页。
        // A short power press normally locks; the reader body may use it for the next page.
        //
        // 短路支只推按键那枚戳：`quick_lock_requested` 不取 FIFO，但它是**按键事件**，
        // 推采样戳会把采样推迟 200 ms（K1 直接红）。
        // The short-circuit branch advances the key stamp only. It takes no FIFO
        // entry, but it is a key event: advancing the sampling stamp would delay
        // sampling by 200 ms and turn K1 red.
        if (quick_lock_requested || (pmu_bus_ok && pmu_bus_due(ctx.now_ms, last_key_ms))) {
            last_key_ms = ctx.now_ms;
            read_pico_pmu_key_action_t key_action = quick_lock_requested ? READ_PICO_PMU_KEY_SHORT : read_pico_pmu_take_key_action(NULL);
            if (key_action != READ_PICO_PMU_KEY_NONE) {
                last_input_ms = ctx.now_ms;
                bool quick_was_open = s_quick.open;
                if (quick_was_open && !quick_lock_requested) {
                    ui_quick_menu_reset(&s_quick);
                    quick_present(&ctx, current, false);
                }
                if (!quick_lock_requested && ctx.now_ms < s_lock_ignore_until_ms) {
                    ESP_LOGI(TAG, "ignore boot power-key action %d", (int)key_action);
                } else if (key_action == READ_PICO_PMU_KEY_LONG) {
                    held_key = -1;
                    cancel_gesture(&ctx, current, &gesture);
                    if (menu_pressed >= 0) menu_feedback(&ctx, current, menu_leaf, menu_pressed, false, &feedback);
                    menu_pressed = UI_MENU_HIT_NONE;
                    extern const app_desc_t app_book;
                    bool reader_power_lock = !menu_open && current == &app_book &&
                        app_book_reader_body_visible() && app_settings_reader_power_turn();
                    if (reader_power_lock) {
                        prepare_lock(&ctx, current, true, true);
                        main_lock_boundary();
                        enter_lock_and_sleep(ctx.hl, &s_lock_ignore_until_ms, ctx.acc, ctx.tp, true);
                        display_request_full_refresh();
                        ctx.now_ms = esp_timer_get_time() / 1000;
                        last_input_ms = ctx.now_ms;
                        poll_media(&ctx, current, &media_mounted, &media_invalidated);
                        last_media_poll_ms = ctx.now_ms;
                        ctx.consumed = true;
                        ctx.pressed = false;
                        ctx.released = false;
                        app_present(&ctx, current, APP_REDRAW_PAGE);
                    } else {
                        // 关机对话框是第 7 个系统画面，并入「系统画面一律竖屏」不变式：
                        // 横屏下必须先复位物理旋转。肖像几何的对话框在横屏物理屏上三重
                        // 失效（真机 round-2 实证）：按钮 Y=960 越出横屏 UI_LOCK_HEIGHT
                        // =684 不绘制；命中区肖像坐标触摸错位不可达；可见元素经肖像→
                        // 横屏旋转映射「没有转过来」。
                        // 仅内联 prepare_lock 的旋转复位一行，是 2026-10-15 用户裁决
                        // 选 A 的降级形态（降级理由入档）：prepare_lock 其余副作用为
                        // 终态转移设计，在可取消的对话框语境有害——app_device_settings
                        // 的 on_before_lock 会中止升级/停联机/停 BLE（阻塞至 2s）/关
                        // PIN 入口，取消后均不恢复；app_lock_feedback 需另传 false 才
                        // 能避开；resume 保存在确认路径 run_power_action→prepare_lock
                        // 会再做，取消路径不重启无需保存。横屏是阅读页私有状态（运行
                        // 时唯一旋转切换点在 book render()），本行对其它 app 空操作。
                        // 取消返回闭环已实证：取消走 app_present(APP_REDRAW_PAGE)→当前
                        // 视图 render() 自设旋转回横屏并整页重绘，无帧缓冲区域恢复路径，
                        // 像素/旋转/触摸一致。
                        // The shutdown dialog is the 7th system screen and joins the
                        // "system frames are portrait" invariant: under landscape the
                        // panel rotation must be reset first. On a landscape panel the
                        // portrait-geometry dialog fails three ways (observed on
                        // hardware, round 2): the button at Y=960 exceeds the landscape
                        // UI_LOCK_HEIGHT=684 so it is never drawn; the hit test uses
                        // portrait coordinates so touches miss; visible elements map
                        // through portrait→landscape and "do not turn".
                        // Only prepare_lock's rotation reset line is inlined - the
                        // downgraded form of option A per the 2026-10-15 ruling
                        // (rationale archived): its other side effects are terminal-
                        // state transfers by design and harmful in a cancelable dialog
                        // context - the settings app's on_before_lock aborts upgrades,
                        // cancels online joins, stops BLE (blocking up to 2s) and
                        // closes the PIN entry, none of which restore after a cancel;
                        // app_lock_feedback would need an extra false to avoid; the
                        // resume save is redundant because the confirm path
                        // run_power_action→prepare_lock saves again and a cancel never
                        // restarts. Landscape is the reader's private state (the only
                        // runtime rotation switch lives in book render()), so this
                        // line is a no-op for other apps.
                        // The cancel return path is verified closed: cancel goes
                        // through app_present(APP_REDRAW_PAGE) → the current view's
                        // render() sets rotation back to landscape and fully repaints;
                        // no framebuffer region-restore path exists, so pixels,
                        // rotation and touch agree.
                        if (epd_get_rotation() == EPD_ROT_LANDSCAPE) epd_set_rotation(EPD_ROT_INVERTED_PORTRAIT);
                        power_dialog_open = true;
                        ui_power_dialog_reset(&power_dialog, touch.touched);
                        ctx.request_app = NULL;
                        ctx.request_return = ctx.request_menu = false;
                        main_lock_boundary();
                        ui_text_set_system_scale(true);
                        ui_power_dialog_draw(ctx.fb, &power_dialog);
                        enum EpdDrawError result = update_display_area_with(
                            ctx.hl, &E0470_WAVEFORM, MODE_GL16, ui_power_dialog_rect());
                        guard_draw_result(ctx.hl, result);
                        if (result != EPD_DRAW_SUCCESS) {
                            power_dialog_open = false;
                            ui_power_dialog_reset(&power_dialog, touch.touched);
                            app_present(&ctx, current, APP_REDRAW_FULL);
                        }
                    }
                } else if (!power_dialog_open) {
                    held_key = -1;
                    cancel_gesture(&ctx, current, &gesture);
                    if (menu_pressed >= 0) menu_feedback(&ctx, current, menu_leaf, menu_pressed, false, &feedback);
                    menu_pressed = UI_MENU_HIT_NONE;
                    app_redraw_t power_redraw = !quick_lock_requested && !quick_was_open && !menu_open && current->on_power_short
                        ? current->on_power_short(&ctx) : APP_REDRAW_NONE;
                    if (power_redraw != APP_REDRAW_NONE) {
                        present_page(&ctx, current, &gesture, power_redraw);
                        ctx.consumed = true;
                    } else {
                        prepare_lock(&ctx, current, true, true);
                        extern const app_desc_t app_book;
                        main_lock_boundary();
                        enter_lock_and_sleep(ctx.hl, &s_lock_ignore_until_ms, ctx.acc, ctx.tp,
                                             !menu_open && current == &app_book && app_book_reader_body_visible());
                        display_request_full_refresh();
                        ctx.now_ms = esp_timer_get_time() / 1000;
                        last_input_ms = ctx.now_ms;
                        poll_media(&ctx, current, &media_mounted, &media_invalidated);
                        last_media_poll_ms = ctx.now_ms;
                        ctx.consumed = true;
                        ctx.pressed = false;
                        ctx.released = false;
                        // 醒来还在同一页，重画一次免得留着锁屏图。
                        // Still the same page; redraw so the lock image does not stay.
                        if (menu_open) present_menu(&ctx, current, menu_leaf, &feedback);
                        else app_present(&ctx, current, APP_REDRAW_PAGE);
                    }
                }
            }
        }

        // 设置里存的字体在 SD 卡上，开机时卡可能还没挂好，这里定期重试；
        // #121: 闩锁语义——卡稳定挂载且跑过一遍补载后退出周期重试（旧代码无退出
        // 条件，把会话内每次字体激活都变成对用户显式选择的强制回退，gen8 真机：
        // 选字体一秒后弹回默认）；卡离位时由上面的 media poll 重开，重插补载。
        // The saved font lives on the card; retry until the mount is ready.
        // #121: latch semantics - retire once the card stays mounted and one
        // catch-up pass has run (with no exit, every in-session activation became
        // a forced rollback of the user's explicit choice - gen8 hardware: the
        // font snapped back one second after a pick); the media poll above rearms
        // the retry while the card is absent, so a reinsert still reloads.
        if (!font_retry_settled && !power_dialog_open && !s_quick.open && !usb_storage_active() && current != &app_transfer &&
            ctx.now_ms - last_font_retry_ms >= FONT_RETRY_INTERVAL_MS) {
            last_font_retry_ms = ctx.now_ms;
            if (try_load_saved_sd_font(!media_invalidated)) {
                held_key = -1;
                cancel_gesture(&ctx, current, &gesture);
                ctx.consumed = true;
                menu_pressed = UI_MENU_HIT_NONE;
                if (menu_open) {
                    present_menu(&ctx, current, menu_leaf, &feedback);
                } else {
                    app_present(&ctx, current, APP_REDRAW_FULL);
                }
                ESP_LOGI(TAG, "TTF font ready");
            }
            // 卡挂载且未作废：这一遍就是补载，退出周期重试；卡再离位时上面重开。
            // Mounted and not invalidated: that pass was the catch-up - retire; the line above rearms on removal.
            if (media_mounted && !media_invalidated) font_retry_settled = true;
        }

        if (!power_dialog_open && released && held_key == UI_KEY_2 && ui_key_hit_test(latest.x, latest.y) == UI_KEY_2 &&
            current->defer_middle_short && !menu_open &&
            !ctx.request_app && !ctx.request_menu && !ctx.request_return) {
            int key = held_key;
            held_key = -1;
            ctx.consumed = true;
            app_redraw_t redraw = ctx.now_ms - held_since_ms >= UI_LONG_PRESS_MS
                ? current->on_key_long(&ctx, key) : current->on_key(&ctx, key);
            present_page(&ctx, current, &gesture, redraw);
        }
        if (!power_dialog_open && held_key >= 0) {
            if (menu_open || ctx.request_app || ctx.request_menu || ctx.request_return || !touch.touched ||
                touch.count != 1 || ui_key_hit_test(touch.x, touch.y) != held_key ||
                ctx.now_ms < held_since_ms) {
                held_key = -1;
            } else if (!pressed && ctx.now_ms - held_since_ms >= UI_LONG_PRESS_MS) {
                int key = held_key;
                held_key = -1;
                ctx.consumed = true;
                present_page(&ctx, current, &gesture, current->on_key_long(&ctx, key));
            }
        }
        // 蓝牙翻页器：BLE 栈只在这里跟随设置启停。必须走 ble_pt_sync()——自己写
        // 「设置与状态不一致就 start()」会让启动失败的那次判断每轮都成立，变成每秒
        // 几十次重复初始化并把内存耗光。sync() 内部有内存闸门、失败退避和崩溃自锁。
        // Bluetooth page-turner: the BLE stack follows the setting only here, and only through
        // ble_pt_sync(). Rolling our own "state differs from setting, so start()" makes a failed
        // start true on every tick, re-initialising dozens of times a second until memory runs
        // out; sync() carries the memory gate, the failure back-off and the crash-loop lock.
        // WiFi 在 C6、BLE 在 S3；传输仍与 BLE 竞争 S3 内部内存，网络占用先拆 BLE，结束后恢复。
        // WiFi runs on C6 and BLE on S3; both consume S3 internal memory, so network leases pause BLE and release it afterwards.
        read_pico_transfer_status_t transfer;
        read_pico_transfer_get_status(&transfer);
        const bool wifi_busy = transfer.state != READ_PICO_TRANSFER_STOPPED ||
            pico_online_busy();
        // 飞行模式下蓝牙同样停摆；解除后按原设置自动恢复。
        // Flight mode halts Bluetooth too; clearing it restores whatever the setting asks for.
        // 推流武装期同理要栈：手机在等一条推送（on_sync 会注册 GATT 服务）。
        // An open push arm needs the stack for the same reason: the phone is
        // waiting for a push (on_sync registers the GATT services).
        // gen24.2（块 1，B 模型下 gen18 注释的修订）：gen18 原文说「武装入口
        // 可能就是翻页器设置关着时点的，栈必须为推流保持运行」——那个入口
        // （deep-link 自动武装）已按 gen23 裁定删除，武装只可能发生在四门
        // 全过之后，而门 2 就是蓝牙总开关：开关关着根本进不到武装。关开关
        // 即收摊（cancel_why「蓝牙已关闭」），故 app_settings_ble_turner()
        // 与 push_sink_armed() 在实践中同真；|| 保留是防御——pending 拉栈
        // 窗口与极端时序下的双保险。R1 的「pending 不算 armed」缺口在 B
        // 模型下自然消解：pending 只在开关开启时发生，sync 条件恒真。
        // gen24.2 (block 1, the gen18 note revised under model B): gen18's
        // original wording - "the arm entry may have been tapped with the
        // page-turner setting off, so the stack must stay up for the push" -
        // described an entry (the deep-link auto-arm) that gen23's ruling has
        // since removed; an arm is now possible only after all four gates,
        // and gate 2 IS the master switch: with it off nothing arms. Flipping
        // it off closes up (cancel_why "bluetooth off"), so
        // app_settings_ble_turner() and push_sink_armed() are true together
        // in practice; the || remains defensive - a double safety net for the
        // pending stack-pull window and extreme timings. R1's "pending does
        // not count as armed" gap closes naturally under model B: a pending
        // arm only exists with the switch on, so the sync condition holds.
        ble_pt_sync((app_settings_ble_turner() || push_sink_armed()) && !wifi_busy && !app_settings_flight_mode());
        ble_pt_poll();
        // 推流轮询：驱动窗口/宽限到期、拔电看守、待武装超时。放在 poll 之后，
        // 翻页器让位闸（ble_pt_poll 内）先执行，射频先归推流。
        // Push poll: window/grace expiry, the unplug watch and the pending-arm
        // timeout. It runs after ble_pt_poll so the page-turner's yield gate
        // (inside that poll) hands the radio over first.
        push_sink_poll();
        // 26.9.30 commit 2：提交段封送。请求由 BLE 回调投递（只置一个标志），
        // 在这里取走并在帧间执行四拍——rename/remove 不上 BLE 回调栈。
        // 26.9.30 commit 3：泵改成每帧都进——请求的取走与「读者释放的第二拍」
        // 都在泵内部（跨帧状态只在那里有一份），这里不再做条件判断。
        // Commit hand-off: posted by the BLE callback (one flag), taken here and
        // executed between frames - rename/remove never run on that stack.
        // Commit 3 makes the pump unconditional: both the take and the reader
        // release's second beat live inside it (the only owner of that cross-frame
        // state), so there is no condition left to test out here.
        transfer_pump_commit();
        if (s_quick.open || quick_input_owned) {
            // 覆盖层期间丢弃页内蓝牙输入，关闭后不重放翻页或学习事件。
            // Discard page BLE input while covered, without replaying turns or learning edges after closing.
            ble_pt_event_t key_event; ble_pt_raw_t raw_event;
            while (ble_pt_pop_key(&key_event)) {}
            while (ble_pt_pop_raw(&raw_event)) {}
        }
        if (s_quick.open && quick_status() != s_quick_status) quick_present(&ctx, current, false);
        if (!power_dialog_open && !s_quick.open && !quick_input_owned && !ui_click_feedback_active() && !menu_open && !ctx.request_app && !ctx.request_menu && !ctx.request_return && current->on_tick != NULL) {
            app_redraw_t redraw = current->on_tick(&ctx);
            if (redraw == APP_REDRAW_PAGE || redraw == APP_REDRAW_FULL) held_key = -1;
            present_page(&ctx, current, &gesture, redraw);
        }
        if (ctx.user_activity) { last_input_ms = ctx.now_ms; ctx.user_activity = false; }
        uint32_t ble_input = ble_pt_input_serial();
        if (ble_input != last_ble_input) { last_ble_input = ble_input; last_input_ms = ctx.now_ms; }
        unsigned requested_idle = app_settings_auto_lock_minutes();
        // 武装期不锁屏：深睡没有 BLE 唤醒源，睡下去这条推送就收不到了，而用户
        // 此刻正是在等它。/ No locking while a push arm is open: deep sleep has
        // no BLE wake source, so a sleep would lose the push the user is
        // waiting for.
        bool idle_blocked = usb_storage_active() || wifi_busy || current == &app_transfer ||
            current->holds_pmu || power_dialog_open || touch.touched ||
            ctx.request_app || ctx.request_menu || ctx.request_return || push_sink_armed();
        if (idle_blocked || requested_idle != idle_minutes) last_input_ms = ctx.now_ms;
        idle_minutes = requested_idle;
        if (!idle_blocked && ctx.now_ms >= s_lock_ignore_until_ms &&
            auto_lock_due(ctx.now_ms, last_input_ms, idle_minutes)) {
            held_key = -1;
            cancel_gesture(&ctx, current, &gesture);
            if (menu_pressed >= 0) menu_feedback(&ctx, current, menu_leaf, menu_pressed, false, &feedback);
            menu_pressed = UI_MENU_HIT_NONE;
            prepare_lock(&ctx, current, false, false);
            extern const app_desc_t app_book;
            main_lock_boundary();
            enter_lock_and_sleep(ctx.hl, &s_lock_ignore_until_ms, ctx.acc, ctx.tp,
                                 !menu_open && current == &app_book && app_book_reader_body_visible());
            display_request_full_refresh();
            ctx.now_ms = esp_timer_get_time() / 1000;
            last_input_ms = ctx.now_ms;
            poll_media(&ctx, current, &media_mounted, &media_invalidated);
            last_media_poll_ms = ctx.now_ms;
            ctx.pressed = ctx.released = false;
            if (menu_open) present_menu(&ctx, current, menu_leaf, &feedback);
            else app_present(&ctx, current, APP_REDRAW_PAGE);
        }
        if (menu_open && feedback.updates > 0
            && ctx.now_ms - feedback.last_ms >= UI_SETTLE_IDLE_MS) {
            guard_draw_result(ctx.hl, update_display_area_with(
                ctx.hl, &E0470_WAVEFORM, MODE_GL16, feedback.area));
            feedback.updates = 0;
        }
        if (!menu_open) feedback.updates = 0;
#ifdef KIIKO_LOCAL_LATENCY
        static int64_t probe_heartbeat;
        if(ctx.now_ms-probe_heartbeat>5000){probe_heartbeat=ctx.now_ms;ESP_LOGI("latency","STATE touch=%d menu=%d dialog=%d usb=%d",touch.touched,menu_open,power_dialog_open,usb_storage_active());}
        if(!menu_open&&!power_dialog_open&&!usb_storage_active()&&!touch.touched) {
            app_redraw_t probe=local_latency_poll(&ctx,current);
            if(probe!=APP_REDRAW_NONE)app_present(&ctx,current,probe);
        }
#endif
        // 返回来源优先；普通请求仍按切页优先处理。
        // Return requests win; ordinary requests still prioritize page switches.
        // USB MSC 持有 TF 卡时禁止任何页面切换；必须先安全停止并重新挂载。
        // Never navigate while USB MSC owns the card; stop and remount it first.
        if (usb_storage_active()) {
            ctx.request_return = false;
            ctx.request_app = NULL;
            ctx.request_menu = false;
        }
        // M 批仲裁：每 tick 推进稳定计时；窗口收敛时把单槽「最后一指」一次性写入
        // 唯一的家 ctx.request_app，消费点(下一 if)照常只切一次。计数随收敛日志输出。
        if (nav_arb_tick(&s_nav_arb)) {
            const int adopt = nav_arb_settle(&s_nav_arb);
            // ⑤ 计数打点随切换日志输出：merged/dropped/commits 由仲裁层产生，
            // probe/low/read/edge/hit/mask/multi 由采样层产生，src 三格分别对应
            // D 封面批次 / C② present 段间 / C① 目录扫描。三态对账读这一行：
            // low==0            → 窗内 INT 一次都没低，物理上没指；
            // low>0 && hit==0   → 有指但没采到（电平门控/坐标/命中几何问题）；
            // hit>0             → 采到了，切页在 40ms 收敛窗后补执行。
            // ⑤ Counters ride the settle line: arbitration counters come from the arbiter,
            // sampling counters from the hook, and src splits D / C② / C①. Reconciliation
            // reads one line: no low = nothing touched; low but no hit = missed; hit = caught.
            ESP_LOGI(TAG, "nav_arb settle tab=%d merged=%u dropped=%u commits=%u "
                     "probe=%u low=%u read=%u edge=%u hit=%u mask=%u multi=%u held=%u "
                     "src=%u/%u/%u",
                     adopt, nav_arb_merged(&s_nav_arb), nav_arb_dropped(&s_nav_arb),
                     nav_arb_commits(&s_nav_arb),
                     s_nav_probe.probes, s_nav_probe.low, s_nav_probe.reads,
                     s_nav_probe.edges, s_nav_probe.hits, s_nav_probe.masked,
                     s_nav_probe.multi, s_nav_probe.held,
                     s_nav_probe.by_src[APP_NAV_SRC_BATCH],
                     s_nav_probe.by_src[APP_NAV_SRC_PRESENT],
                     s_nav_probe.by_src[APP_NAV_SRC_FILES]);
            nav_probe_clear_window();  // 计数描述「本次切换周期」，收敛即清零
            if (adopt >= 0 && adopt != app_index_of(current))
                ui_nav_request(&ctx, adopt);
        }
        if (!power_dialog_open && (ctx.request_return || ctx.request_app || ctx.request_menu)) {
            if (ui_click_feedback_release(ctx.fb, &click_area)) click_present(&ctx, current, click_area);
            ui_quick_menu_reset(&s_quick);
            held_key = -1;
            const app_desc_t* next = ctx.request_app;
            const bool go_back = ctx.request_return;
            cancel_gesture(&ctx, current, &gesture);
            menu_pressed = UI_MENU_HIT_NONE;
            ctx.request_app = NULL;
            ctx.request_menu = false;
            ctx.request_return = false;
            ctx.consumed = true;
            if (go_back) {
                bool main_source = main_page_ready(&ctx, current);
                bool shelf_source = main_source && app_index_of(current) == 1;
                display_main_transition_cancel();
                menu_open = return_app ? return_to_menu : true;
                menu_leaf = return_app ? return_menu_leaf : ui_menu_leaf_for_app(current);
                if (return_app) {
                    if (current->on_exit) current->on_exit(&ctx);
                    current = return_app;
                    return_app = NULL;
                    app_font_activate_system();
                    if (current->on_enter) current->on_enter(&ctx);
                    // 初始化后恢复通用页码，避免 on_enter 的默认页码覆盖来源。
                    // Restore the shared leaf after initialization overrides its default.
                    ctx.leaf = return_leaf;
                }
                if (menu_open) present_menu(&ctx, current, menu_leaf, &feedback);
                else {
                    if (main_source && main_page_ready(&ctx, current)) display_main_transition_arm(current, shelf_source, true);
                    app_present(&ctx, current, APP_REDRAW_PAGE);
                    display_main_transition_disarm();
                }
            } else if (next) {
                if (next != current) {
                    return_app = current;
                    return_leaf = ctx.leaf;
                    return_to_menu = selected_from_menu;
                    return_menu_leaf = menu_leaf;
                }
                menu_open = false;
                switch_to(&ctx, &current, next);

            } else {
                menu_open = true;
                menu_leaf = ui_menu_leaf_for_app(current);
                present_menu(&ctx, current, menu_leaf, &feedback);
            }
        }
        nav_battery_watch(&ctx, current, menu_open || power_dialog_open || s_quick.open);
        // 连续列表相位期间缩短等待；每次仍读触摸，并给其他任务让出一个调度周期。
        // Reduce waits during continuous list phases; poll touch each time and yield one scheduling tick.
        vTaskDelay(pdMS_TO_TICKS(LOOP_TICK_MS));
    }
}
