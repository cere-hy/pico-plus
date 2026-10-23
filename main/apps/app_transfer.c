/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 传书页负责两种接入模式生命周期、书源注入与状态展示；HTTP和文件接收属于独立组件。
 * Transfer page owns both network modes, storage injection and status; the component receives files.
 *
 * 冻结：按用户双模式要求提供热点与已有WiFi；离开传书时停服，配网后已连接的 STA 保持在线；深睡/断电会中断传输。
 * render只画快照；内置存储单文件上限由book_store提供，不格式化TF卡。
 * 上传并发扫描曾欠载，页内增加预填余量；离页停服后恢复，不改波形或像素时钟。
 * 用户要求设备端配网：停服后扫描选网，ASCII密码只在连接保存时写入，离开输入页即清空。
 * 用户要求输入光标：密码框常亮光标可轻点定位，显示/隐藏保留位置，离页一并清空。
 * 用户授权新输入法：WiFi 密码共用英文、大小写、数字及符号键盘，不启用中文候选。
 * 用户验收要求停止后恢复进入传书前的页面或菜单位置；遗忘网络必须确认。
 * 用户要求联网后提供网址二维码；热点用单码切换连接/网页，网络变更清除旧码。
 * 卡失效时停止并汇合接收任务，清除旧容量与二维码；当前请求不得切换存储源。
 * 配网列表与确认页底栏可切换主页面；密码页底部保留输入操作键，已连接的 STA 在离页后保活。
 * 正在上传时禁止主动切页；USB 退出必须先断开电脑再启动本机重新挂载。
 * Frozen: AP/STA transfer stops on exit while a provisioned connected STA stays online; deep sleep or power loss interrupts transfer.
 * Render only paints snapshots; book_store defines the flash file limit. Never format the TF card.
 * Concurrent uploads underrun scan queues; increase prefill until service exit without changing waveforms or pixel clocks.
 * User-requested device provisioning scans while stopped; save ASCII passwords only on connect and clear input on leaving the editor.
 * User-requested caret: tap to position a steady password caret; show/hide preserves position, and exit clears it too.
 * Authorized keyboard revision: WiFi passwords share English, case, number and symbol keys without Chinese candidates.
 * 用户修订：输入密码只刷新输入框与字数，布局切换才重画键盘；不在打字时全刷。
 * User revision: password typing refreshes its field/count; repaint keys only on layout switches, with no typing-triggered wipes.
 * Acceptance requires returning to the page or menu position used to enter transfer; forgetting WiFi requires confirmation.
 * User-requested URL QR follows network readiness; AP switches one code between joining and browsing, discarding stale codes on changes.
 * Lost media stops and joins reception and clears capacity/QR; never switch storage beneath an active request.
 * Provisioning lists and confirmation keep the main tabs active; the password editor keeps its input actions, and connected STA persists after exit.
 * Navigation waits for an active upload; USB exit disconnects the host before local remount.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "app.h"
#include "app_content_open.h"
#include "book_store.h"
#include "book_progress.h"
#include "book_title.h"
#include "dict_lookup.h"
#include "dict_popup.h"
#include "display.h"
#include "e0470_epaper_waveform.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "read_pico_transfer.h"
#include "read_pico_sd.h"
#include "ble_page_turner.h"
#include "push_sink.h"
#include "read_pico_pmu.h"
#include "settings.h"
#include "ttf_font.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_text_input.h"
#include "ui_keyboard.h"
#include "ui_menu.h"
#include "ui_nav.h"
#include "ui_wifi_qr.h"
#include "usb_storage.h"

static const char* TAG = "transfer_page";
static book_store_root_t s_root;
static read_pico_transfer_status_t s_status;
static bool s_start_pending, s_settle, s_any_changed;
static bool s_media_lost;
static int s_pressed = -1;
static read_pico_transfer_mode_t s_mode = READ_PICO_TRANSFER_MODE_AP;
static bool s_session_started;
static int64_t s_poll_ms;
static uint64_t s_free;
static size_t s_heap_before, s_internal_before;
static EpdRect s_area;
static bool s_qr_url, s_qr_ready;
static char s_usb_message[96];
static bool s_usb_requested, s_wifi_setup_requested, s_wifi_settings_only;
static bool s_wifi_upload_requested, s_hotspot_requested, s_direct_entry, s_usb_entry;
// 手机 BLE 推流（二期 M1）：s_push_requested 由入口置位，on_enter 据此武装；
// s_push_poll_ms 节流武装页刷新，s_push_note 是页内动态消息（错误/已接收占位）。
// Phone BLE push (phase 2 M1): s_push_requested is set by the entry and on_enter
// arms on it; s_push_poll_ms throttles the arm-page refresh and s_push_note is
// the page's live message (errors / received placeholders).
static bool s_push_requested;
static int64_t s_push_poll_ms;
static char s_push_note[96];
// gen24.4（渲染发灰根治）：进入武装页的第一次呈现必须整屏 GC16。跨页切入
// （首页/阅读器弹回）时上一页的深墨——首页月相钟轮廓——会被 GL16 差分留着，
// 叠在按钮上就是「文字发糊、发灰、背景有旧图案残影」。清晰度第一：武装页
// 落地只用全刷擦旧底，绝不用低质量局部快刷打底。置位点：push_arm_now
// （页内点按武装）与 on_enter 的 armed 保活分支；消费点：present，一次即止。
// Gen24.4 (the gray-render root fix): the arm view's first presentation must
// be a screen-wide GC16. On a cross-page entry (home / a bounce back from the
// reader) the previous page's dark ink - the home moon-phase clock outline -
// survives a GL16 differential and stacks onto the buttons: "blurred, gray
// text with an old pattern's ghost in the background". Clarity first: the arm
// page lands on a full refresh that wipes the old base, never on a low-quality
// partial refresh. Set by push_arm_now (in-page arm tap) and on_enter's armed
// keep-alive branch; consumed once by present.
static bool s_push_entry_full_once;
// Step 4：单词卡开关与活区比对簿记。/ Step 4: the word-card switch and the
// live-region comparison bookkeeping.
static bool s_push_word_card;
static push_state_t s_push_last_state = PUSH_STATE_IDLE;
static uint32_t s_push_last_left_s = 1; // 初值取不到的值，首拍必重绘 / an impossible first value so the first tick repaints
static uint32_t s_push_last_recv = 1, s_push_last_total = 1;
static char s_push_last_adv[64];
static bool s_usb_start_pending;
static int64_t s_usb_retry_ms;
static bool s_usb_host_connected;
static uint64_t s_usb_capacity_bytes;

static uint32_t s_signature_revision, s_signature_seen;
static bool transfer_signature_get(char *out, size_t cap) {
    const char *text = app_settings_status_signature();
    if (!out || !cap || strlen(text) >= cap) return false;
    snprintf(out, cap, "%s", text); return true;
}
static esp_err_t transfer_signature_set(const char *text) {
    app_settings_set_status_signature(text);
    if (strcmp(app_settings_status_signature(), text)) return ESP_FAIL;
    __atomic_add_fetch(&s_signature_revision, 1, __ATOMIC_RELEASE);
    return ESP_OK;
}

static bool transfer_book_file(const char *path) {
    const char *ext = strrchr(path, '.');
    return ext && (!strcasecmp(ext, ".epub") || !strcasecmp(ext, ".txt"));
}

static bool transfer_path_below(const char *path, const char *directory) {
    size_t n = strlen(directory);
    return !strncmp(path, directory, n) && (!path[n] || path[n] == '/');
}

static void transfer_directory_deleted(const char *path) {
    if (transfer_path_below(app_settings_books_dir(), path))
        (void)app_settings_set_books_dir("/sdcard/books");
    if (transfer_path_below(app_settings_fonts_dir(), path))
        (void)app_settings_set_fonts_dir("/sdcard/fonts");
}

static void transfer_directory_moved(const char *old_path, const char *new_path) {
    const char *selected[2] = {app_settings_books_dir(), app_settings_fonts_dir()};
    for (int i = 0; i < 2; ++i) {
        if (!transfer_path_below(selected[i], old_path)) continue;
        char updated[288];
        if (snprintf(updated, sizeof(updated), "%s%s", new_path, selected[i] + strlen(old_path)) >= (int)sizeof(updated)) continue;
        bool saved = i == 0 ? app_settings_set_books_dir(updated) : app_settings_set_fonts_dir(updated);
        if (!saved) {
            if (i == 0) (void)app_settings_set_books_dir("/sdcard/books");
            else (void)app_settings_set_fonts_dir("/sdcard/fonts");
        }
    }
}

static void transfer_file_deleted(const char *path) {
    if (transfer_book_file(path)) {
        (void)book_progress_forget(path);
        (void)book_title_clear(path);
    }
    if (!strcmp(app_settings_system_font_path(), path)) app_settings_set_system_font_path("");
    if (!strcmp(app_settings_font_path(), path)) app_settings_set_font_path("");
    if (!strcmp(app_settings_wallpaper_path(), path)) app_settings_set_wallpaper_path("");
}

static void transfer_file_moved(const char *old_path, const char *new_path, uint32_t size) {
    if (transfer_book_file(old_path)) {
        book_progress_t progress = {0};
        bool saved = book_progress_load(old_path, size, &progress);
        char last[BOOK_STORE_PATH_MAX] = {0};
        bool was_last = book_progress_last_path(last, sizeof(last)) && !strcmp(last, old_path);
        if (saved) (void)book_progress_save(new_path, &progress);
        (void)book_progress_forget(old_path);
        if (was_last) (void)book_progress_set_last_path(new_path);
    }
    if (transfer_book_file(old_path)) (void)book_title_clear(old_path);
    if (!strcmp(app_settings_system_font_path(), old_path)) app_settings_set_system_font_path(new_path);
    if (!strcmp(app_settings_font_path(), old_path)) app_settings_set_font_path(new_path);
    if (!strcmp(app_settings_wallpaper_path(), old_path)) app_settings_set_wallpaper_path(new_path);
}

static bool transfer_set_wallpaper(const char *path) {
    if (strncmp(path, "/sdcard/pictures/", 17)) return false;
    app_settings_set_wallpaper_path(path);
    if (strcmp(app_settings_wallpaper_path(), path)) return false;
    app_settings_set_lock_style(1);
    return app_settings_lock_style() == 1;
}

void app_transfer_request_method_picker(void) {
    s_usb_requested = s_hotspot_requested = s_wifi_upload_requested = s_wifi_setup_requested = false;
    s_wifi_settings_only = false;
}

void app_transfer_request_usb_start(void) {
    s_usb_requested = true;
}

void app_transfer_request_wifi_setup(void) {
    s_wifi_setup_requested = true;
    s_wifi_settings_only = true;
}

void app_transfer_request_wifi_upload(void) {
    s_wifi_upload_requested = true;
    s_wifi_settings_only = false;
}

void app_transfer_request_hotspot_start(void) {
    s_hotspot_requested = true;
    s_wifi_settings_only = false;
}

// 手机推送入口（gen24.2 块 8）：只置跳转标志——on_enter 据此落方法页，
// 武装交给用户在传书页显式点「手机推送」行走四门（gen23 裁定：deep-link
// 只跳转不武装；gen18~gen24.1 的自动武装绕过四门，已修正）。
// gen24.3 问题 3：文件管理页的「手机推送」按钮已删（蓝牙不能传书，该入口
// 多余），本函数随之失去生产调用方；首页推流状态卡走的是
// app_transfer_request_method_picker() 独立路径，不受影响。保留定义：
// transfer_ui 宿主测试（push_arm_regression 等）直调本函数，删除会破坏
// gen24.2 回归；且为公共 API、非 static，无 -Wunused-function 风险。
// The phone-push entry (gen24.2 block 8): only sets the navigate flag -
// on_enter lands on the method page and the arm waits for the user's
// explicit "phone push" tap, which runs the four gates (gen23 ruling: the
// deep link navigates only; the gen18-gen24.1 auto-arm bypassed the gates
// and is corrected). gen24.3 problem 3: the file manager's "phone push"
// button is gone (Bluetooth cannot transfer books, so that entry was
// redundant) and this function has lost its production caller; the home
// push status card takes the separate app_transfer_request_method_picker()
// path and is unaffected. The definition stays: the transfer_ui host tests
// (push_arm_regression etc.) call it directly, and it is a public API (not
// static), so there is no -Wunused-function risk.
void app_transfer_request_push(void) {
    s_push_requested = true;
}

static void clear_qr(void) {
    ui_wifi_qr_clear();
    s_qr_ready = false;
}

// 仅在网络快照变化或用户切换时编码，render与上传进度不触发编码。
// Encode only on network snapshot changes or user switching, never from render or upload progress.
static void prepare_qr(void) {
    clear_qr();
    if (!s_status.network_ready) return;
    s_qr_ready = s_qr_url || s_mode == READ_PICO_TRANSFER_MODE_STA
        ? ui_wifi_qr_prepare_url(s_status.url)
        : ui_wifi_qr_prepare(s_status.ssid, READ_PICO_TRANSFER_PASSWORD);
}

typedef enum { TRANSFER_METHODS, TRANSFER_HOME, TRANSFER_NETWORKS, TRANSFER_PASSWORD, TRANSFER_PUSH } transfer_view_t;
#define NETWORK_ROWS 4
#define PASSWORD_MAX 64
static transfer_view_t s_view;
// gen18（18-a）：本次离页若是 OPEN_BOOK(push.txt) 的交接就不收摊——sink 转后台
// 属主，手机在宽限期/窗口内还能直接连推。用户手动离开照旧收摊。
// gen18 (18-a): if this exit is the OPEN_BOOK(push.txt) handoff, do not tear
// down - the sink moves to a background owner and the phone can still push
// within the grace/window. Manual exits close up as before.
static bool s_push_handed_off;
// gen25 让路链（接线裁定 #1 异步形态）：0 = 无让路；1 = 已发停机请求、等 prep
// 退出（12K 栈归还）；UI 在 phase 1 期间显示「正在为推送腾出内存…」，收尾由
// push_tick 的下一拍完成，绝不在一个按钮回调里同步等满 1000ms。
// Gen25 yield chain (wiring ruling #1, async shape): 0 = no yield; 1 = stop
// requested, waiting for prep to exit (12K stack back). During phase 1 the UI
// shows "freeing memory for the push..."; the next push_tick finishes the job -
// a button callback never blocks for the full 1000 ms.
static int s_push_yield_phase;
static int64_t s_push_yield_deadline;
// 分拍等待上限：prep 一轮是百毫秒量级，给 1500ms（3 个 500ms tick）覆盖最大书
// 的最长 prep；到点未停即按失败处理（裁定 #3：不原地重试，等待 = 卡死观感）。
// Tick-wait ceiling: a prep round runs in the hundreds of milliseconds, so
// 1500 ms (three 500 ms ticks) covers the longest prep of the biggest book; on
// expiry it is treated as a failure (ruling #3: never retry in place - waiting
// reads as a freeze).
#define PUSH_YIELD_WAIT_MS 1500
static void push_arm_now(void); // gen25：让路链收尾在 push_tick 里武装，早于其定义 / gen25: the yield chain arms from push_tick, above push_arm_now's definition
static read_pico_transfer_network_t s_networks[READ_PICO_TRANSFER_SCAN_MAX], s_selected;
static size_t s_network_count, s_network_page;
static bool s_scan_pending, s_network_connect_pending, s_saved_configured, s_password_visible, s_forget_confirm;
static char s_saved_ssid[33], s_password[PASSWORD_MAX + 1], s_network_message[96], s_method_message[160];
// gen24.2 块 6：最近一次收摊原因的内存态副本（不做 NVS——重启后旧原因
// 没有指路价值，push_sink 的 s_close_why 也会被下一次武装清掉，故 on_enter
// 时取一次快照在此）。/ Gen24.2 block 6: an in-memory copy of the most recent
// close-up reason (no NVS - a stale reason points nowhere after a reboot, and
// push_sink's own s_close_why is cleared by the next arm, so we snapshot it at
// on_enter).
static char s_last_cancel_why[64];
// gen24.2 块 3：门 2（蓝牙总开关）提示条的行内【开启】按钮可见性——提示条
// 是瞬态 UI，按钮只在开关门拦截时出现。/ Gen24.2 block 3: visibility of the
// inline [enable] button on the gate-2 (BLE master switch) hint bar - a
// transient UI that appears only when the switch gate refuses.
static bool s_method_inline_enable;
// 行内【开启】按钮的命中区：钉在提示条横幅（36,584,612,76）右侧，文字让位
// 左半区。/ The inline [enable] button's hit rect: pinned to the right of the
// hint banner, with the text yielding the left half.
static EpdRect inline_enable_rect(void) { return (EpdRect){496, 596, 128, 52}; }
static ui_text_edit_t s_password_input;

static esp_err_t s_scan_error;

static void render(app_ctx_t* ctx, uint8_t* fb);
static void stop_session(void);
static bool stop_upload_if_idle(void);
static void fit_label(char* text, int px, int width);

static void transfer_header(uint8_t *fb, const char *title, const char *subtitle) {
    ui_nav_status(fb);
    ui_nav_back(fb, 36, 79);
    ui_text_vc(fb, 342, 107, 34, title, EPD_DRAW_ALIGN_CENTER, false);
    if (subtitle && subtitle[0]) ui_text(fb, 36, 153, 20, subtitle, EPD_DRAW_ALIGN_LEFT, false);
    if (subtitle && subtitle[0]) ui_hairline(fb, 190, 36, 612, UI_GRAY_LIGHT);
}

static EpdRect method_control_rect(int id) {
    return (EpdRect){36, 224 + id * 112, 612, 112};
}

/* ---- gen25 内存体检面板 / gen25 memory health panel ---- */
// 入口：方法页长按。手势边界（预审点 2①）：ui_gesture.c 里 LONG_PRESS 置位
// long_fired 后松手只发 CANCEL，TAP 永不随之发出——长按与三行点按天然互斥，
// 零冲突；按下反馈只画了行底，面板整页盖上去不留半按态。
// Entry: a long press on the method page. Gesture boundary (pre-audit 2-(1)):
// once LONG_PRESS sets long_fired the release emits only CANCEL and a TAP is
// never sent alongside - the long press and the three row taps are naturally
// exclusive, zero conflict; the press feedback only painted the row base and
// the panel covers the page whole, leaving no half-pressed state.
// 读数同源（预审点 1 的另一半）：ble_pt_mem_snapshot + ble_pt_mem_gate——与
// 「手机推送」按钮预检、start_locked 同一份数字与判定，面板不说第二家话。
// Same-source readings (the other half of pre-audit 1): ble_pt_mem_snapshot +
// ble_pt_mem_gate - the very figures and verdict the phone-push button's
// precheck and start_locked use, so the panel never speaks a second dialect.
// 红绿灯（用户裁定定案）：以最大连续块为主判、碎片率为辅显——红 <15K 直接
// 判红（不看碎片率）；黄 15–20K 时碎片率只作附加说明；绿 ≥20K（即 BLE 闸门的
// 最大块门槛）。free 趋于 0 时 1−largest/free 会虚低（free=4K、largest=4K
// →0% 但实际濒危），主判根本不读它，误导显示从结构上出局。
// The traffic light (the user's ruling, now settled): the largest block
// decides and fragmentation only annotates - red below 15K outright
// (fragmentation unread), yellow 15-20K with fragmentation as a note only,
// green from 20K (the BLE gate's own largest threshold). As free approaches
// zero 1-largest/free reads artificially low (free=4K with largest=4K says 0%
// while the heap is dying); the verdict never reads it, so the misleading
// display is designed out structurally.
#define MEM_HEALTH_RED_BELOW (15 * 1024) // 红线：低于此直接判红 / the red line
typedef enum { MEM_LEVEL_RED, MEM_LEVEL_YELLOW, MEM_LEVEL_GREEN } mem_level_t;

static mem_level_t mem_health_level(size_t largest_internal) {
    if (largest_internal < MEM_HEALTH_RED_BELOW) return MEM_LEVEL_RED;
    if (largest_internal < BLE_PT_MIN_LARGEST_INTERNAL) return MEM_LEVEL_YELLOW;
    return MEM_LEVEL_GREEN;
}

// 碎片率 = 1 − largest/free（整数百分比）。free 为 0 按 100% 计——最坏解释，
// 顺带杜绝除零；此刻 largest 必为 0、主判早已判红，比值只剩展示意义。
// Fragmentation = 1 - largest/free as an integer percent. A zero free heap
// reads 100% (worst case, which also rules out division by zero); largest is
// necessarily 0 there and the largest-driven verdict has long since gone red,
// so the ratio is display-only.
static unsigned mem_fragmentation_pct(size_t free_internal, size_t largest_internal) {
    if (free_internal == 0) return 100;
    if (largest_internal >= free_internal) return 0;
    return (unsigned)(((free_internal - largest_internal) * 100u) / free_internal);
}

// 一句准话（纯函数，宿主对表）。绿区再核一遍总闸门：连续块够但总内存/PSRAM
// 未达门槛时不许说「充足」——那是红绿灯只看 largest 的镜像误导。
// One honest sentence (pure, host-tested). The green zone re-checks the full
// gate: when the largest block is fine but total/PSRAM misses the gate it must
// not say "ample" - the mirror image of trusting the largest-only light.
static void mem_panel_verdict(char *out, size_t cap, mem_level_t level,
                              unsigned frag_pct, bool gate_ok) {
    switch (level) {
    case MEM_LEVEL_RED:
        snprintf(out, cap, "内存不足，请重启设备");
        break;
    case MEM_LEVEL_YELLOW:
        snprintf(out, cap, frag_pct >= 50 ? "内存偏紧且碎片较多，建议退出阅读"
                                          : "内存偏紧，建议退出阅读后再试");
        break;
    default:
        snprintf(out, cap, gate_ok ? "内存充足，可以开启推送"
                                   : "连续块充足，总内存未达推送门槛");
        break;
    }
}

static void mem_size_text(char *out, size_t cap, size_t bytes) {
    if (bytes >= 1024u * 1024u) snprintf(out, cap, "%.1f M", bytes / 1048576.0);
    else snprintf(out, cap, "%.1f K", bytes / 1024.0);
}

// 位置边界（预审点 2②）：面板只盖方法卡（224..560），不碰提示条横幅
// （584..660，gen24.1 起可挂两行）、两行说明（694/734）、返回键（36,79 起）
// 与底栏（1096 起）——gen24.5 刚为按钮糊团动过布局，面板不再叠新问题。
// Position boundary (pre-audit 2-(2)): the panel covers the method card
// (224-560) only, touching neither the hint banner (584-660, two lines since
// gen24.1), the two info lines (694/734), the back key (36,79 up) nor the tab
// bar (1096 up) - gen24.5 just moved the layout for the button blob; the panel
// adds no new collisions.
static bool s_mem_panel;
static EpdRect mem_panel_rect(void) { return (EpdRect){36, 224, 612, 336}; }
static EpdRect mem_panel_close_rect(void) { return (EpdRect){246, 496, 192, 44}; }

static void draw_mem_panel(uint8_t *fb) {
    const EpdRect panel = mem_panel_rect();
    ui_fill_round_rect(fb, panel, 24, UI_GRAY_WHITE);
    ui_draw_round_rect(fb, panel, 24, UI_GRAY_BLACK);
    ui_draw_round_rect(fb, (EpdRect){panel.x + 1, panel.y + 1, panel.width - 2, panel.height - 2}, 23, UI_GRAY_BLACK);
    ui_text(fb, 64, 246, 27, "内存体检", EPD_DRAW_ALIGN_LEFT, false);
    ui_hairline(fb, 292, 64, 556, UI_GRAY_LIGHT);
    size_t free_internal, largest_internal, free_psram;
    ble_pt_mem_snapshot(&free_internal, &largest_internal, &free_psram);
    const mem_level_t level = mem_health_level(largest_internal);
    const unsigned frag = mem_fragmentation_pct(free_internal, largest_internal);
    const bool gate_ok = ble_pt_mem_gate(free_internal, largest_internal, free_psram);
    const struct { const char *label; size_t bytes; } rows[] = {
        {"内部可用内存", free_internal}, {"最大连续块", largest_internal}, {"PSRAM 可用", free_psram},
    };
    int y = 308;
    char value[32];
    for (size_t i = 0; i < 3; ++i, y += 38) {
        ui_text(fb, 64, y, 19, rows[i].label, EPD_DRAW_ALIGN_LEFT, false);
        mem_size_text(value, sizeof(value), rows[i].bytes);
        ui_text(fb, 620, y, 19, value, EPD_DRAW_ALIGN_RIGHT, false);
    }
    snprintf(value, sizeof(value), "%u%%", frag);
    ui_text(fb, 64, y, 19, "内部碎片率", EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, 620, y, 19, value, EPD_DRAW_ALIGN_RIGHT, false);
    // 红绿灯：三圆点左→右 = 红/黄/绿，实心为当前档。墨水屏无彩色，实心/空心
    // 即档位语言，判定与「手机推送」按钮预检同源。/ The traffic light: three
    // dots left to right = red/yellow/green, the solid one marks the level. An
    // e-ink panel has no colour, so solid vs hollow is the level language - and
    // the verdict shares the phone-push button's precheck source.
    const int cy = 470;
    for (int i = 0; i < 3; ++i) {
        EpdRect dot = {80 + i * 24 - 8, cy - 8, 16, 16};
        if ((mem_level_t)i == level) ui_fill_round_rect(fb, dot, 8, UI_GRAY_BLACK);
        else ui_draw_round_rect(fb, dot, 8, 0x88);
    }
    char verdict[96];
    mem_panel_verdict(verdict, sizeof(verdict), level, frag, gate_ok);
    fit_label(verdict, 20, 460);
    ui_text(fb, 152, cy - 10, 20, verdict, EPD_DRAW_ALIGN_LEFT, false);
    ui_draw_button(fb, mem_panel_close_rect(), "知道了", false);
}

// 面板手势：模态。只有【知道了】与再长按能关；其余一律吞——底栏、返回键、
// 方法行都不该被顺手误触，正在读的数字不因一次误触消失。
// The panel's gestures: modal. Only [got it] and another long press close it;
// everything else is swallowed - the tab bar, the back key and the method rows
// must not fall to a stray tap, and the readings must not vanish mid-read.
static app_redraw_t mem_panel_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    (void)ctx;
    if (ev->type == UI_GESTURE_LONG_PRESS ||
        (ev->type == UI_GESTURE_TAP && ui_rect_hit(mem_panel_close_rect(), ev->x, ev->y))) {
        s_mem_panel = false;
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

// S05：入口只负责选择传输方式，具体服务信息留给下一页。
// S05 keeps method selection separate from the service details shown on the next page.
static void draw_methods(uint8_t *fb) {    ui_clear_page(fb);
    ui_fill_rect_fast((EpdRect){0, 0, UI_LOCK_WIDTH, UI_NAV_TOP}, 0xe0, fb);
    transfer_header(fb, "Wifi/热点传书", NULL);
    ui_text(fb, 42, 180, 21, "选择方式", EPD_DRAW_ALIGN_LEFT, false);
    EpdRect card = {36, 224, 612, 336};
    ui_fill_round_rect(fb, card, 24, UI_GRAY_WHITE);
    ui_draw_round_rect(fb, card, 24, 0x60);
    ui_draw_round_rect(fb, (EpdRect){37, 225, 610, 334}, 23, 0x60);
    // 注意不能用 static：第 3 行的词要随状态每次重算，static 会把上一次的
    // 状态词永久留在数组里。/ Not static on purpose: row 3's wording is recomputed
    // per state each call; a static array would keep the last state's words forever.
    const char *titles[] = {"WiFi传书", "热点传书", "手机推送"};
    const char *details[] = {
        "与手机或电脑连接同一个无线网络",
        "由墨水屏创建临时网络，离线也能传书",
        "安卓复制文字，通过蓝牙发送到墨水屏"
    };
    // gen24 多态按钮：第 3 行标题随 push_sink 状态改词，按钮本身就是状态指示。
    // 判定与点按动作同源：干净 IDLE=手机推送；上次失败/自锁=重试接收（一键自愈
    // 入口）；武装/窗口/宽限=取消接收；传输中=正在接收（禁用）。
    // gen24 multi-state button: row 3 retitles itself off push_sink state - the
    // button is the status. The verdict shares the tap action's source: a clean
    // IDLE says "phone push"; a failed/self-locked IDLE says "retry" (the heal
    // entry); armed/window/grace says "cancel"; mid-transfer says "receiving"
    // (disabled).
    const push_state_t pst = push_sink_state();
    if (pst == PUSH_STATE_CONNECTED || pst == PUSH_STATE_RECEIVING || pst == PUSH_STATE_PROCESSING) {
        titles[2] = "正在接收";
        details[2] = "手机正在推送，请勿离开本页";
    } else if (pst != PUSH_STATE_IDLE) {
        titles[2] = "取消接收";
        details[2] = "正在等待手机连接，再点一次取消";
    } else if (ble_pt_locked()) {
        titles[2] = "重试接收";
        details[2] = "上次启动未成功，再点一次自动重试";
    }
    for (int i = 0; i < 3; ++i) {
        EpdRect row = method_control_rect(i);
        if (s_pressed == i) ui_draw_pressed_round_rect(fb, row, 0);
        if (i) ui_fill_rect_fast((EpdRect){68, row.y, 548, 2}, 0x80, fb);
        int cy = row.y + row.height / 2;
        if (i == 0) ui_nav_wifi_icon(fb, 72, cy, 30, 0x6a);
        else if (i == 1) ui_draw_icon(fb, 72, cy, 48, UI_ICON_RADIO_TOWER, 0x6a);
        else ui_draw_icon(fb, 72, cy, 40, UI_ICON_BLUETOOTH, 0x6a);
        ui_text(fb, 112, row.y + 21, 27, titles[i], EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 112, row.y + 64, 19, details[i], EPD_DRAW_ALIGN_LEFT, false);
        ui_text_vc(fb, 617, cy, 29, "›", EPD_DRAW_ALIGN_CENTER, false);
    }
    // 提示条：优先瞬态消息（s_method_message），无消息时挂 gen24.2 块 6 的
    // 收摊原因底账（上次取消/超时的一句话，武装后清除）。
    // The hint bar: the transient message first; with none, gen24.2 block 6's
    // close-up reason ledger (last cancel/timeout one-liner, cleared on arm).
    if (s_method_message[0]) {
        ui_fill_round_rect(fb, (EpdRect){36, 584, 612, 76}, 20, 0xd8);
        if (s_method_inline_enable) {
            // gen24.2 块 3：门 2 提示 + 行内【开启】按钮——文字让位左半区
            // （cx=258），按钮钉横幅右侧。/ Gate-2 hint plus the inline
            // [enable] button - the text yields the left half (cx=258) and
            // the button pins to the banner's right.
            ui_text_vc(fb, 258, 622, 20, s_method_message, EPD_DRAW_ALIGN_CENTER, false);
            const EpdRect btn = inline_enable_rect();
            ui_fill_round_rect(fb, btn, 16, UI_GRAY_WHITE);
            ui_draw_round_rect(fb, btn, 16, 0x60);
            ui_text_vc(fb, btn.x + btn.width / 2, btn.y + btn.height / 2, 24, "开启",
                       EPD_DRAW_ALIGN_CENTER, false);
        } else {
            // gen24.1：两段式文案以「——」分界拆两行画——单行约 780px（20px 字号），
            // 超出 612px 横幅。无分隔符的单行文案（如「蓝牙启动失败，请重试」）仍走
            // 居中单行，行为不变。
            // gen24.1: the two-part hint renders as two lines split at the em-dash
            // separator - one line runs ~780px at a 20px font, past the 612px banner.
            // Single-line hints without the separator (e.g. "bluetooth start failed,
            // retry") stay centered one-liners, unchanged.
            const char *dash = strstr(s_method_message, "——");
            if (dash) {
                char head[160];
                size_t n = (size_t)(dash - s_method_message);
                if (n >= sizeof(head)) n = sizeof(head) - 1;
                memcpy(head, s_method_message, n);
                head[n] = '\0';
                ui_text_vc(fb, 342, 608, 20, head, EPD_DRAW_ALIGN_CENTER, false);
                ui_text_vc(fb, 342, 638, 20, dash + strlen("——"), EPD_DRAW_ALIGN_CENTER, false);
            } else {
                ui_text_vc(fb, 342, 622, 20, s_method_message, EPD_DRAW_ALIGN_CENTER, false);
            }
        }
    } else if (s_last_cancel_why[0]) {
        // gen24.2 块 6：非主动收摊（窗口到期/拔电/关开关）后进传书页，先把
        // 最近一次原因挂在提示条位——用户不必猜上一次为什么结束。
        // Block 6: after a non-voluntary close-up (window expiry, unplug,
        // switch-off), entering the page shows the latest reason in the hint
        // slot - the user should not have to guess why it ended.
        ui_fill_round_rect(fb, (EpdRect){36, 584, 612, 76}, 20, 0xd8);
        char why_line[96];
        snprintf(why_line, sizeof(why_line), "上次接收已结束：%s", s_last_cancel_why);
        fit_label(why_line, 20, 560);
        ui_text_vc(fb, 342, 622, 20, why_line, EPD_DRAW_ALIGN_CENTER, false);
    }
    ui_text(fb, 42, 694, 19, "浏览器传书支持 EPUB、TXT、字体和图片文件。", EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, 42, 734, 18, "传输完成后，新内容会自动出现在书架或字体列表。", EPD_DRAW_ALIGN_LEFT, false);
    // gen25 内存体检的入口指引：长按无图无标不可发现，一行静态小字指路
    // （774..795 空白区，上离说明行 755、下离底栏 1096，无任何遮挡）。
    // Gen25: the memory-health entry needs a signpost - a long press has no
    // icon to find. One static small line in blank space (774-795: 20px below
    // the info line, 300px above the tab bar) covers nothing.
    ui_text(fb, 42, 774, 18, "长按本页任意位置，可查看内存体检。", EPD_DRAW_ALIGN_LEFT, false);
    ui_nav_draw(fb, 2);
    // 体检面板最上层：模态卡片压住方法卡区，提示条/说明行/返回键/底栏全留白。
    // The health panel on top: a modal card over the method-card area; the hint
    // banner, info lines, back key and tab bar all stay visible.
    if (s_mem_panel) draw_mem_panel(fb);
}

/* ---- 设备端配网 / Device provisioning ---- */
static void fit_label(char* text, int px, int width) {
    while (*text && ttf_text_width_px(px, text) > width) {
        size_t n = strlen(text) - 1;
        while (n && ((unsigned char)text[n] & 0xc0) == 0x80) --n;
        text[n] = 0;
    }
}

static void clear_password(void) {
    volatile char* p = s_password;
    for (size_t i = 0; i < sizeof(s_password); ++i) p[i] = 0;
    ui_text_edit_init(&s_password_input, s_password, sizeof(s_password));
    s_password_visible = false;
    ui_keyboard_end();
}

static EpdRect network_control_rect(int id) {
    if (id >= 0 && id < NETWORK_ROWS) return (EpdRect){36, 465 + id * 95, 612, 95};
    if (id == 6) return (EpdRect){501, 244, 123, 56};
    if (id == 11) return s_forget_confirm ? ui_row_rect(0, 2, 600, UI_BTN_H) : (EpdRect){501, 315, 123, 52};
    if (id == 12) return ui_row_rect(1, 2, 600, UI_BTN_H);
    bool paged = s_network_count > NETWORK_ROWS;
    if (id == 7) return paged ? (EpdRect){36, 873, 64, 66} : (EpdRect){0};
    if (id == 8) return paged ? (EpdRect){112, 873, 460, 66} : (EpdRect){36, 873, 612, 66};
    if (id == 9) return paged ? (EpdRect){584, 873, 64, 66} : (EpdRect){0};
    return (EpdRect){0};
}

static EpdRect password_control_rect(int id) {
    if (id == 44) return (EpdRect){36, 434, 278, 68};
    if (id == 45) return (EpdRect){500, 215, 148, 58};
    if (id == 46) return (EpdRect){330, 434, 318, 68};
    if (id == 47) return (EpdRect){UI_MARGIN, 288, ui_content_width(), 86};
    return (EpdRect){0};
}

static void provisioning_button(uint8_t* fb, EpdRect rect, const char* text, int id) {
    if (s_pressed == id) ui_draw_pressed_round_rect(fb, rect, UI_BTN_RADIUS);
    ui_draw_button(fb, rect, text, false);
}

static void network_border(uint8_t *fb, EpdRect rect, int radius) {
    ui_draw_round_rect(fb, rect, radius, 0x50);
    ui_draw_round_rect(fb, (EpdRect){rect.x + 1, rect.y + 1, rect.width - 2, rect.height - 2}, radius - 1, 0x50);
}

static void network_button(uint8_t *fb, int id, const char *label, bool dark) {
    EpdRect rect = network_control_rect(id);
    ui_fill_round_rect(fb, rect, 16, s_pressed == id ? 0xb0 : dark ? UI_GRAY_BLACK : UI_GRAY_WHITE);
    if (!dark) network_border(fb, rect, 16);
    ui_text_vc(fb, rect.x + rect.width / 2, rect.y + rect.height / 2, 24, label,
               EPD_DRAW_ALIGN_CENTER, dark && s_pressed != id);
}

static void draw_networks(uint8_t* fb) {
    ui_clear_page(fb);
    ui_fill_rect_fast((EpdRect){0, 0, UI_LOCK_WIDTH, UI_NAV_TOP}, 0xe0, fb);
    if (s_forget_confirm) {
        transfer_header(fb, "遗忘网络", "仅清除 WiFi 连接信息，不影响图书");
        char name[33];
        snprintf(name, sizeof(name), "%s", s_saved_ssid);
        fit_label(name, UI_PX_BODY, ui_content_width());
        ui_text(fb, UI_MARGIN, 350, UI_PX_BODY, name, EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, UI_MARGIN, 450, UI_PX_CAPTION, s_network_message, EPD_DRAW_ALIGN_LEFT, false);
        provisioning_button(fb, network_control_rect(11), "确认遗忘", 11);
        provisioning_button(fb, network_control_rect(12), "取消", 12);
        ui_nav_draw(fb, 3);
        return;
    }
    transfer_header(fb, "无线网络", NULL);
    ui_text(fb, 42, 182, 24, "当前网络", EPD_DRAW_ALIGN_LEFT, false);
    EpdRect current = {36, 224, 612, 154};
    ui_fill_round_rect(fb, current, 22, UI_GRAY_WHITE);
    network_border(fb, current, 22);
    if (s_saved_configured) {
        char saved[40]; snprintf(saved, sizeof(saved), "%s", s_saved_ssid); fit_label(saved, 29, 375);
        ui_nav_wifi_icon(fb, 74, 265, 28, UI_GRAY_BLACK);
        ui_text(fb, 104, 243, 29, saved, EPD_DRAW_ALIGN_LEFT, false);
        const char *state = s_status.network_ready ? "已连接" :
            s_network_connect_pending || s_status.state == READ_PICO_TRANSFER_STARTING ? "正在连接…" : "已保存 · 尚未连接";
        ui_text(fb, 64, 297, 23, state, EPD_DRAW_ALIGN_LEFT, false);
        char address[80];
        const char *url = !strncmp(s_status.url, "http://", 7) ? s_status.url + 7 : s_status.url;
        snprintf(address, sizeof(address), "%.79s", s_status.network_ready ? url[0] ? url : "已连接，可传书和对时" : "连接后可传书和对时");
        fit_label(address, 21, 419);
        ui_text(fb, 64, 336, 21, address, EPD_DRAW_ALIGN_LEFT, false);
        network_button(fb, 6, s_status.network_ready ? "断开" : "连接", !s_status.network_ready);
        if (s_pressed == 11) ui_draw_pressed_round_rect(fb, network_control_rect(11), 14);
        ui_text_vc(fb, 563, 341, 21, "遗忘网络", EPD_DRAW_ALIGN_CENTER, false);
    } else {
        ui_text(fb, 64, 253, 29, "尚未保存网络", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 64, 312, 23, "请从下方列表选择 2.4 GHz WiFi", EPD_DRAW_ALIGN_LEFT, false);
    }
    char count[64];
    snprintf(count, sizeof(count), "%u 个 · %u/%u 页", (unsigned)s_network_count,
             (unsigned)s_network_page + 1, (unsigned)(s_network_count ? (s_network_count + NETWORK_ROWS - 1) / NETWORK_ROWS : 1));
    ui_text(fb, 42, 418, 24, "附近网络", EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, 642, 420, 21, count, EPD_DRAW_ALIGN_RIGHT, false);
    EpdRect list = {36, 465, 612, 380};
    ui_fill_round_rect(fb, list, 22, UI_GRAY_WHITE);
    if (s_scan_pending) {
        ui_text_vc(fb, 342, 655, 25, "正在扫描 2.4 GHz WiFi", EPD_DRAW_ALIGN_CENTER, false);
    } else if (!s_network_count) {
        ui_text_vc(fb, 342, 620, 27, s_scan_error == ESP_OK ? "没有发现可用网络" : "扫描没有完成",
                   EPD_DRAW_ALIGN_CENTER, false);
        ui_text_vc(fb, 342, 678, 22, s_scan_error == ESP_OK ? "请靠近路由器后重新扫描" : "点下方按钮自动重试",
                   EPD_DRAW_ALIGN_CENTER, false);
    }
    for (int row = 0; !s_scan_pending && row < NETWORK_ROWS; ++row) {
        size_t index = s_network_page * NETWORK_ROWS + row;
        if (index >= s_network_count) break;
        const read_pico_transfer_network_t* network = &s_networks[index];
        EpdRect rect = network_control_rect(row);
        if (s_pressed == row) ui_draw_pressed_round_rect(fb, rect, 0);
        if (row) ui_fill_rect_fast((EpdRect){64, rect.y, 556, 2}, 0x70, fb);
        char name[33], detail[64];
        snprintf(name, sizeof(name), "%s", network->ssid);
        fit_label(name, 28, 520);
        ui_text(fb, 64, rect.y + 16, 28, name, EPD_DRAW_ALIGN_LEFT, false);
        snprintf(detail, sizeof(detail), "%s · %s", network->rssi > -60 ? "信号强" : network->rssi > -75 ? "信号良好" : "信号一般",
                 !network->supported ? "暂不支持" : network->requires_password ? "需要密码" : "开放网络");
        ui_text(fb, 64, rect.y + 59, 21, detail, EPD_DRAW_ALIGN_LEFT, false);
        ui_text_vc(fb, 615, rect.y + 47, 27, "›", EPD_DRAW_ALIGN_CENTER, false);
    }
    network_border(fb, list, 22);
    if (s_network_count > NETWORK_ROWS) {
        network_button(fb, 7, "‹", false);
        network_button(fb, 9, "›", false);
    }
    network_button(fb, 8, s_scan_error == ESP_OK ? "重新扫描" : "重试扫描", false);
    if (s_network_count <= NETWORK_ROWS) ui_draw_icon(fb, 597, 906, 32, UI_ICON_ROTATE_CW, UI_GRAY_BLACK);
    ui_text(fb, 42, 976, 21, "支持 2.4 GHz WiFi（信道 1–13）", EPD_DRAW_ALIGN_LEFT, false);
    char message[96];
    snprintf(message, sizeof(message), "%s", s_network_message[0] ? s_network_message :
             s_status.network_ready ? "退出此页后，WiFi 仍保持连接。" : "连接后，可退出此页继续使用。");
    fit_label(message, 21, 600);
    ui_text(fb, 42, 1014, 21, message, EPD_DRAW_ALIGN_LEFT, false);
    ui_nav_draw(fb, 3);
}

static void draw_password(uint8_t* fb) {
    ui_clear_page(fb);
    transfer_header(fb, "连接 WiFi", "输入完成后连接，密码只保存在本机");
    char name[33];
    snprintf(name, sizeof(name), "%s", s_selected.ssid);
    fit_label(name, UI_PX_BODY, ui_content_width());
    ui_text(fb, UI_MARGIN, 176, UI_PX_BODY, name, EPD_DRAW_ALIGN_LEFT, false);
    char caption[80];
    size_t len = strlen(s_password);
    snprintf(caption, sizeof(caption), "%s · %u/%u", s_selected.requires_password ? "密码" : "开放网络，无需密码", (unsigned)len, PASSWORD_MAX);
    ui_text_fixed_vc(fb, UI_MARGIN, 240, 22, caption, EPD_DRAW_ALIGN_LEFT, false);
    ui_text_input_draw(fb, &s_password_input, password_control_rect(47), UI_PX_BODY,
                       !s_password_visible, NULL);
    ui_text_fixed_vc(fb, 36, 399, 22, s_network_message[0] ? s_network_message : "支持英文、数字、符号和空格", EPD_DRAW_ALIGN_LEFT, false);
    provisioning_button(fb, password_control_rect(44), "取消", 44);
    provisioning_button(fb, password_control_rect(45), s_password_visible ? "隐藏" : "显示", 45);
    provisioning_button(fb, password_control_rect(46), "连接并保存", 46);
    ui_keyboard_draw(fb, 560);
}

static void queue_network_start(read_pico_transfer_mode_t mode) {
    s_mode = mode;
    s_qr_url = mode == READ_PICO_TRANSFER_MODE_STA;
    clear_qr();
    s_view = s_wifi_settings_only ? TRANSFER_NETWORKS : TRANSFER_HOME;
    s_forget_confirm = false;
    s_scan_pending = false;
    s_pressed = -1;
    clear_password();
    memset(&s_status, 0, sizeof(s_status));
    s_status.mode = s_mode;
    read_pico_transfer_get_saved_wifi(s_status.wifi_ssid, &s_status.wifi_configured);
    s_saved_configured = s_status.wifi_configured;
    snprintf(s_saved_ssid, sizeof(s_saved_ssid), "%s", s_status.wifi_ssid);
    if (s_wifi_settings_only) {
        s_network_connect_pending = true;
        snprintf(s_network_message, sizeof(s_network_message), "正在连接 %s…", s_status.wifi_ssid);
    } else s_start_pending = true;
}

static bool enter_networks(void) {
    if (!read_pico_transfer_try_stop_if_idle()) return false;
    stop_session();
    s_start_pending = false;
    s_view = TRANSFER_NETWORKS;
    s_forget_confirm = false;
    s_network_count = s_network_page = 0;
    s_scan_pending = true;
    s_scan_error = ESP_OK;
    s_network_message[0] = 0;
    s_saved_ssid[0] = 0;
    s_saved_configured = false;
    read_pico_transfer_get_saved_wifi(s_saved_ssid, &s_saved_configured);
    clear_password();
    return true;
}

static bool start_hotspot_service(void) {
    if (!read_pico_transfer_try_stop_if_idle()) return false;
    stop_session();
    s_mode = READ_PICO_TRANSFER_MODE_AP;
    s_qr_url = false;
    clear_qr();
    memset(&s_status, 0, sizeof(s_status));
    s_status.mode = s_mode;
    s_start_pending = true;
    s_view = TRANSFER_HOME;
    s_method_message[0] = 0;
    return true;
}

// 提示页先呈现，下一轮才同步扫描；输入页不轮询已停服的传书状态。
// Present the scan notice first, scan synchronously next tick, and do not poll stopped transfer state in the editor.
static app_redraw_t network_ui_tick(app_ctx_t* ctx) {
    if (s_view != TRANSFER_NETWORKS || s_forget_confirm || ctx->consumed) return APP_REDRAW_NONE;
    if (s_network_connect_pending) {
        s_network_connect_pending = false;
        read_pico_transfer_cfg_t cfg = {.mode = READ_PICO_TRANSFER_MODE_STA, .network_only = true};
        esp_err_t err = read_pico_transfer_start(&cfg);
        s_session_started = err == ESP_OK;
        read_pico_transfer_get_status(&s_status);
        if (err != ESP_OK) snprintf(s_network_message, sizeof(s_network_message),
                                    read_pico_transfer_rf_blocked()
                                        ? "飞行模式已开启，请先在设置 › 无线连接中关闭"
                                        : "连接启动失败，请重试");
        return APP_REDRAW_PAGE;
    }
    if (s_session_started) {
        read_pico_transfer_service_poll();
        read_pico_transfer_status_t next = {0};
        read_pico_transfer_get_status(&next);
        bool changed = next.state != s_status.state || next.network_ready != s_status.network_ready ||
                       next.last_error != s_status.last_error;
        s_status = next;
        if (s_status.network_ready) snprintf(s_network_message, sizeof(s_network_message), "连接成功");
        else if (s_status.state == READ_PICO_TRANSFER_ERROR) snprintf(s_network_message, sizeof(s_network_message), "连接失败，请检查密码或信号");
        return changed ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
    }
    if (!s_scan_pending) return APP_REDRAW_NONE;
    s_scan_pending = false;
    s_network_count = s_network_page = 0;
    // The scan notice is already in the framebuffer. Drop rebuildable glyph
    // bitmaps before starting WiFi, then recreate them while drawing the result.
    // This prevents the hosted radio from competing with the UI cache at init.
    ttf_font_cache_clear();
    esp_err_t err = read_pico_transfer_scan_wifi(s_networks, &s_network_count);
    s_scan_error = err;
    if (err != ESP_OK) {
        s_network_count = 0;
        // 飞行模式下扫描被闸门挡住，此时「扫描失败」是误导——列表根本扫不出来，
        // 所以要先说清楚原因，让用户知道该去关飞行模式而不是反复重扫。
        // In flight mode the scan is gated off, so "scan failed" would be misleading: there is no
        // list to retry into. Say why, so the user turns flight mode off instead of rescanning.
        snprintf(s_network_message, sizeof(s_network_message), "%s",
                 read_pico_transfer_rf_blocked() ? "飞行模式已开启，请先在设置 › 无线连接中关闭"
                 : err == ESP_ERR_NO_MEM ? "内存已清理，请点下方按钮重试" : "扫描失败，请点下方按钮重试");
    }
    else if (!s_network_count) snprintf(s_network_message, sizeof(s_network_message), "未发现网络，请靠近路由器后重扫");
    else s_network_message[0] = 0;
    s_pressed = -1;
    return APP_REDRAW_PAGE;
}

static int provisioning_hit(uint16_t x, uint16_t y) {
    if (s_forget_confirm) {
        for (int i = 11; i <= 12; ++i) if (ui_rect_hit(network_control_rect(i), x, y)) return i;
        return -1;
    }
    if (s_view == TRANSFER_PASSWORD) {
        for (int i = 44; i < 48; ++i) if (ui_rect_hit(password_control_rect(i), x, y)) return i;
    } else {
        for (int i = 0; i < 12; ++i) {
            if (i < NETWORK_ROWS && (s_scan_pending || s_network_page * NETWORK_ROWS + i >= s_network_count)) continue;
            if ((i == 6 || i == 11) && !s_saved_configured) continue;
            if (ui_rect_hit(network_control_rect(i), x, y)) return i;
        }
    }
    return -1;
}

static app_redraw_t provisioning_action(int id) {
    if (s_forget_confirm) {
        if (id == 12) { s_forget_confirm = false; s_network_message[0] = 0; }
        else if (id == 11) {
            // 断联后页面标记可能已丢失；以组件的真实会话为准，上传中不能清配置。
            // A failed reconnect may lose the page flag; stop the actual component session, never an active upload.
            if (!stop_upload_if_idle()) {
                snprintf(s_network_message, sizeof(s_network_message), "正在传输，请完成后再遗忘");
                return APP_REDRAW_PAGE;
            }
            s_network_connect_pending = s_scan_pending = false;
            esp_err_t err = read_pico_transfer_forget_wifi();
            if (err == ESP_OK) {
                read_pico_transfer_get_status(&s_status);
                s_forget_confirm = false;
                s_saved_configured = false;
                s_saved_ssid[0] = 0;
                snprintf(s_network_message, sizeof(s_network_message), "已遗忘网络");
            } else snprintf(s_network_message, sizeof(s_network_message), "遗忘失败，请重试或取消");
        }
        return APP_REDRAW_PAGE;
    }
    if (s_view == TRANSFER_NETWORKS) {
        if (id < NETWORK_ROWS) {
            s_selected = s_networks[s_network_page * NETWORK_ROWS + id];
            if (!s_selected.supported) {
                snprintf(s_network_message, sizeof(s_network_message), "此网络认证暂不支持，请选择其他网络");
                return APP_REDRAW_PAGE;
            }
            if (!s_selected.requires_password) {
                esp_err_t err = read_pico_transfer_save_wifi(s_selected.ssid, "");
                if (err == ESP_OK) {
                    queue_network_start(READ_PICO_TRANSFER_MODE_STA);
                    return APP_REDRAW_PAGE;
                }
                snprintf(s_network_message, sizeof(s_network_message), "开放网络保存失败，请重试");
                return APP_REDRAW_PAGE;
            }
            clear_password();
            s_network_message[0] = 0;
            ui_keyboard_begin(&s_password_input, true);
            s_view = TRANSFER_PASSWORD;
        } else if (id == 6) {
            if (s_status.network_ready) {
                if (!stop_upload_if_idle()) {
                    snprintf(s_network_message, sizeof(s_network_message), "正在传输，请完成后再断开");
                    return APP_REDRAW_PAGE;
                }
                s_network_connect_pending = false;
                snprintf(s_network_message, sizeof(s_network_message), "已断开，连接信息保留");
            } else {
                // 未连上不等于驱动已停止；重试前先释放仍在等待或超时的旧 STA。
                // Disconnected does not mean stopped; release the pending or timed-out STA before retrying.
                if (!stop_upload_if_idle()) {
                    snprintf(s_network_message, sizeof(s_network_message), "正在传输，请完成后再连接");
                    return APP_REDRAW_PAGE;
                }
                s_network_connect_pending = false;
                queue_network_start(READ_PICO_TRANSFER_MODE_STA);
            }
        }
        else if (id == 7 && s_network_page) --s_network_page;
        else if (id == 8) {
            if (s_session_started) stop_session();
            s_scan_pending = true; s_scan_error = ESP_OK; s_network_message[0] = 0;
        }
        else if (id == 9 && (s_network_page + 1) * NETWORK_ROWS < s_network_count) ++s_network_page;
        else if (id == 10) queue_network_start(s_mode);
        else if (id == 11) { s_forget_confirm = true; s_network_message[0] = 0; }
        return APP_REDRAW_PAGE;
    }
    size_t len = strlen(s_password);
    if (id == 44) { clear_password(); s_network_message[0] = 0; s_view = TRANSFER_NETWORKS; return APP_REDRAW_PAGE; }
    else if (id == 45) s_password_visible = !s_password_visible;
    else if (id == 46) {
        if (s_selected.requires_password && len < 8) {
            snprintf(s_network_message, sizeof(s_network_message), "加密网络密码至少 8 个字符");
        } else {
            esp_err_t err = read_pico_transfer_save_wifi(s_selected.ssid,
                s_selected.requires_password ? s_password : "");
            if (err == ESP_OK) { queue_network_start(READ_PICO_TRANSFER_MODE_STA); return APP_REDRAW_PAGE; }
            snprintf(s_network_message, sizeof(s_network_message), "%s", err == ESP_ERR_INVALID_ARG ?
                     "密码须 8–63 字符或 64 位十六进制" : "保存失败，输入已保留，请重试");
        }
    }
    return APP_REDRAW_AREA;
}

static bool s_input_layout, s_input_settle;
static app_redraw_t password_paint(app_ctx_t *ctx, bool field, bool controls) {
    ui_keyboard_update_t update = ui_keyboard_update(ctx->fb, 560, password_control_rect(47),
        UI_PX_BODY, !s_password_visible, NULL, field);
    EpdRect extra = {UI_MARGIN, 215, ui_content_width(), controls ? 287 : 159};
    if (update.field || controls) {
        ui_clear_rect_fast(ctx->fb, (EpdRect){UI_MARGIN, 215, ui_content_width() - 156, 58});
        char caption[80];
        snprintf(caption, sizeof(caption), "%s · %u/%u", s_selected.requires_password ? "密码" : "开放网络，无需密码", (unsigned)strlen(s_password), PASSWORD_MAX);
        ui_text_fixed_vc(ctx->fb, UI_MARGIN, 240, 22, caption, EPD_DRAW_ALIGN_LEFT, false);
    }
    if (controls) {
        ui_clear_rect_fast(ctx->fb, (EpdRect){36, 377, 612, 36});
        ui_text_fixed_vc(ctx->fb, 36, 399, 22, s_network_message[0] ? s_network_message : "支持英文、数字、符号和空格", EPD_DRAW_ALIGN_LEFT, false);
        for (int i = 44; i <= 46; ++i) {
            EpdRect button = password_control_rect(i); ui_clear_rect_fast(ctx->fb, button);
            provisioning_button(ctx->fb, button, i == 44 ? "取消" : i == 45 ? s_password_visible ? "隐藏" : "显示" : "连接并保存", i);
        }
    }
    if (update.field || controls) update.area = update.area.width ? ui_rect_union(update.area, extra) : extra;
    s_area = update.area; s_input_layout = update.layout; s_input_settle = update.settle; s_settle = false;
    return update.area.width ? APP_REDRAW_AREA : APP_REDRAW_NONE;
}
static app_redraw_t provisioning_gesture(app_ctx_t* ctx, const ui_gesture_event_t* ev) {
    if (s_view == TRANSFER_PASSWORD) {
        if (ev->type == UI_GESTURE_PRESS)
            return ui_keyboard_press(ev->x0, ev->y0, 560, ctx->now_ms) ? password_paint(ctx, false, false) : APP_REDRAW_NONE;
        bool feedback = ev->type != UI_GESTURE_LONG_PRESS && ui_keyboard_release();
        if (ev->type != UI_GESTURE_TAP) return feedback ? password_paint(ctx, false, false) : APP_REDRAW_NONE;
        int x = ev->x0, y = ev->y0;
        if (ui_text_input_tap(&s_password_input, password_control_rect(47), UI_PX_BODY, !s_password_visible, NULL, x, y)) {
            return password_paint(ctx, true, false);
        }
        for (int i = 44; i <= 46; ++i) if (ui_rect_hit(password_control_rect(i), x, y)) {
            app_redraw_t changed = provisioning_action(i);
            if (changed == APP_REDRAW_AREA) return password_paint(ctx, true, true);
            return changed;
        }
        ui_keyboard_result_t result = ui_keyboard_tap(x, y, 560, ctx->now_ms);
        if (result == UI_KEYBOARD_DONE) {
            app_redraw_t changed = provisioning_action(46);
            if (changed == APP_REDRAW_PAGE) return changed;
            result = UI_KEYBOARD_CHANGED;
        }
        if (result == UI_KEYBOARD_CHANGED) return password_paint(ctx, false, y >= 990 && x >= 534);
        return APP_REDRAW_NONE;
    }
    int old = s_pressed;
    int origin = provisioning_hit(ev->x0, ev->y0);
    s_pressed = ev->type == UI_GESTURE_PRESS ? origin : -1;
    bool action = ev->type == UI_GESTURE_TAP && old >= 0 && old == origin && provisioning_hit(ev->x, ev->y) == origin;
    EpdRect old_rect = s_view == TRANSFER_PASSWORD ? password_control_rect(old >= 0 ? old : origin >= 0 ? origin : 0)
                                                  : network_control_rect(old >= 0 ? old : origin >= 0 ? origin : 0);
    if (action) {
        if (s_view == TRANSFER_PASSWORD && origin == 47) {
            ui_text_input_tap(&s_password_input, password_control_rect(47), UI_PX_BODY,
                              !s_password_visible, NULL, ev->x0, ev->y0);
            render(ctx, ctx->fb);
            s_area = password_control_rect(47);
            s_settle = false;
            return APP_REDRAW_AREA;
        }
        app_redraw_t redraw = provisioning_action(origin);
        if (redraw == APP_REDRAW_PAGE) return redraw;
        render(ctx, ctx->fb);
        s_area = ui_rect_union(old_rect, (EpdRect){UI_MARGIN, 240, ui_content_width(), 744});
        s_settle = false;
        return APP_REDRAW_AREA;
    }
    if (old == s_pressed) return APP_REDRAW_NONE;
    render(ctx, ctx->fb);
    s_area = old_rect;
    s_settle = false;
    return APP_REDRAW_AREA;
}

static EpdRect status_rect(void) { return (EpdRect){36, 676, 612, 270}; }
static uint64_t free_bytes(void* arg) { return book_store_free_bytes(arg); }

static void draw_status(uint8_t* fb) {
    EpdRect area = status_rect();
    ui_clear_rect_fast(fb, area);
    ui_fill_round_rect(fb, area, 24, UI_GRAY_WHITE);
    ui_draw_round_rect(fb, area, 24, 0x98);
    const char* state = "网络已停止";
    if (s_start_pending || s_status.state == READ_PICO_TRANSFER_STARTING) state = "正在准备传书…";
    else if (s_status.state == READ_PICO_TRANSFER_READY) state = "等待上传";
    else if (s_status.state == READ_PICO_TRANSFER_UPLOADING) state = "正在接收";
    else if (s_status.state == READ_PICO_TRANSFER_ERROR) state = s_status.network_ready ? "上传失败，请重试" :
        s_mode == READ_PICO_TRANSFER_MODE_AP ? "热点传书未启动" : "WiFi 传书未启动";
    if (s_media_lost) state = "存储已移除，传书已停止";
    ui_text(fb, area.x + 28, area.y + 24, 26, state, EPD_DRAW_ALIGN_LEFT, false);
    char line[256];
    if (s_media_lost) snprintf(line, sizeof(line), "重新进入传书可使用内置存储");
    else if (s_status.state == READ_PICO_TRANSFER_ERROR && !s_status.network_ready)
        snprintf(line, sizeof(line), "原因：%s",
                 read_pico_transfer_rf_blocked() ? "飞行模式已开启，请先在设置中关闭"
                                                : esp_err_to_name(s_status.last_error));
    else if (s_mode == READ_PICO_TRANSFER_MODE_AP)
        snprintf(line, sizeof(line), "已连接 %u 台 · 已完成 %u 本", s_status.sta_count, s_status.done_count);
    else snprintf(line, sizeof(line), "同网浏览器上传 · 已完成 %u 本", s_status.done_count);
    ui_text(fb, area.x + 28, area.y + 68, 19, line, EPD_DRAW_ALIGN_LEFT, false);
    snprintf(line, sizeof(line), "%s", s_status.cur_name);
    while (*line && ttf_text_width_px(ui_text_effective_px(19), line) > area.width - 280) {
        size_t n = strlen(line) - 1;
        while (n && ((unsigned char)line[n] & 0xc0) == 0x80) --n;
        line[n] = 0;
    }
    ui_text(fb, area.x + 28, area.y + 108, 19, line[0] ? line : "尚无正在传输的文件", EPD_DRAW_ALIGN_LEFT, false);
    snprintf(line, sizeof(line), "%.1f / %.1f MB", s_status.cur_bytes / 1048576.0, s_status.cur_total / 1048576.0);
    ui_text(fb, area.x + area.width - 28, area.y + 108, 19, line, EPD_DRAW_ALIGN_RIGHT, false);
    EpdRect track = {area.x + 28, area.y + 150, area.width - 56, 10};
    ui_fill_round_rect(fb, track, 5, 0xc8);
    if (s_status.cur_total) {
        uint64_t width = (uint64_t)s_status.cur_bytes * track.width / s_status.cur_total;
        track.width = width > (uint64_t)track.width ? track.width : (int)width;
        ui_fill_round_rect(fb, track, 5, UI_GRAY_BLACK);
    }
    char capacity[112];
    if (s_media_lost) snprintf(capacity, sizeof(capacity), "TF 卡已移除，原存储已停用");
    else snprintf(capacity, sizeof(capacity), "%s · 剩余 %.1f MB%s", s_root.is_flash ? "内置" : "TF 卡",
                  s_free / 1048576.0, s_root.is_flash ? " · 单文件 ≤ 1 MB" : "");
    ui_text(fb, area.x + 28, area.y + 190, 18, capacity, EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, area.x + 28, area.y + 228, 18,
            s_status.last_error != ESP_OK ? "传输未完成，请检查网络后重试" : "同名文件会覆盖；完成后自动更新书架",
            EPD_DRAW_ALIGN_LEFT, false);
}

static EpdRect control_rect(int id) {
    if (id == 0) return (EpdRect){36, 608, 296, 56};
    if (id == 1) return (EpdRect){352, 608, 296, 56};
    if (id == 2) return (EpdRect){36, 976, 612, 76};
    return (EpdRect){0};
}

// gen24.5：常开按钮已整块删除（用户裁定），其独立几何随之取消。
// Gen24.5: the hold button is gone outright (the user's ruling), and so is its
// own geometry.

/* ---- 手机推送武装页 / The phone-push arm page ---- */

// 武装入口的默认窗口：30 分钟。充电时常开（window 0）由页内按钮切换。
// The arm entry's default window: 30 minutes. Charger-held (window 0) is the
// in-page button's job.
#define PUSH_ENTRY_WINDOW_MS (30u * 60u * 1000u)

static const char *push_state_text(push_state_t st) {    switch (st) {
    case PUSH_STATE_ARMED: return "正在开启接收…";
    case PUSH_STATE_WINDOW: return "等待手机连接";
    case PUSH_STATE_CONNECTED: return "手机已连接";
    case PUSH_STATE_RECEIVING: return "正在接收推送…";
    case PUSH_STATE_PROCESSING: return "正在校验内容…";
    case PUSH_STATE_COOLDOWN: return "推送已完成";
    default: return "接收未开启";
    }
}

// gen24.5：push_charging_now() 随常开按钮一并移除——它唯一的用处是判断按钮
// 该不该亮，按钮没了就成了死代码（还会多一条 -Wunused-function，破 8 警告
// 基线）。日后若恢复常开入口，把这段和按钮一起请回来。
// Gen24.5: push_charging_now() leaves with the hold button - its only job was
// deciding whether that button should light up, and with the button gone it is
// dead code (plus one stray -Wunused-function, breaking the 8-warning
// baseline). Bring this pair back if the hold entry ever returns.

/* ---- gen26 预留：可扩展卡片渲染骨架（只留骨架不出函数）----
 * Gen26 reservation: the extensible card-render skeleton (shape only, no function).
 * 现状清单：武装页两卡——广播名卡 {36,168,612,116}、状态卡 {36,308,612,224}；
 * 活区 push_dynamic_rect() = 两卡并集，每秒 DU 快刷只落在活区。
 * gen26 扩展位（代理层三件套落地时照此办理）：
 *   1. 新卡（AI 释义卡 / 代理结果卡）二选一：a) 模态——照单词卡先例
 *      （dict_popup_draw 盖页 + push_gesture 先问卡片）；b) 第三张静态卡——
 *      状态卡下移，卡区计入 push_dynamic_rect() 与否必须显式裁定并写进
 *      该代记录（静态卡混进每秒 DU 就是 gen24.5 按钮糊团的复刻）。
 *   2. 渲染派发沿 draw_push 自上而下：每卡一个 draw_* 静态函数，命中区一个
 *      rect() 静态函数，手势由 push_gesture 统一先问模态、再问卡内控件。
 *   3. 内容进入渠道走 push_proto.h：CTRL 预留区 0x06..0x0F（gen25 已整段留号），
 *      启用哪个号先在协议变更记录登记一行。
 * 冻结：本骨架只登记形状与纪律，不出任何函数/结构体；gen26 启用时删本注释、
 * 按上述三条落码。
 * Frozen: this skeleton registers shape and discipline only - no function, no
 * struct. Gen26 deletes this comment and lands the code per the three rules. */

static void draw_push(uint8_t *fb) {
    ui_clear_page(fb);
    ui_fill_rect_fast((EpdRect){0, 0, UI_LOCK_WIDTH, UI_NAV_TOP}, 0xe0, fb);
    transfer_header(fb, "接收推送", NULL);

    // 广播名卡：手机在蓝牙列表里认的就是这一行。/ The adv-name card: this is the
    // line the phone picks from its Bluetooth list.
    EpdRect name = {36, 168, 612, 116};
    ui_fill_round_rect(fb, name, 24, UI_GRAY_WHITE);
    ui_draw_round_rect(fb, name, 24, 0x98);
    const char *adv = push_sink_adv_name();
    char name_line[80];
    snprintf(name_line, sizeof(name_line), "%s", adv[0] ? adv : "正在生成名称…");
    fit_label(name_line, 30, 500);
    ui_text(fb, 64, 196, 30, name_line, EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, 64, 246, 19, "在手机蓝牙列表中选择这个名称", EPD_DRAW_ALIGN_LEFT, false);

    // 状态卡：状态、窗口/进度、进度条。/ The status card: state, window/progress
    // and the progress bar.
    EpdRect status = {36, 308, 612, 224};
    ui_fill_round_rect(fb, status, 24, UI_GRAY_WHITE);
    ui_draw_round_rect(fb, status, 24, 0x98);
    const push_state_t st = push_sink_state();
    // gen24.2 R2：pending 期间 push_sink 状态机仍是 IDLE（栈还没起来，会话
    // 未开），但页面留在推送页说「正在启动蓝牙…」——状态机没词的地方 UI
    // 必须自己说，闪回方法页就是死锁。/ Gen24.2 R2: while pending the state
    // machine is still IDLE (the stack is not up, the session has not opened),
    // but the page stays on the push view saying "starting bluetooth..." -
    // where the machine has no word the UI must speak its own; bouncing back
    // to the method page was the deadlock.
    const bool starting = !push_sink_armed() && push_sink_pending();
    ui_text(fb, 64, 336, 27, starting ? "正在启动蓝牙…" : push_state_text(st), EPD_DRAW_ALIGN_LEFT, false);
    char sub[128];
    uint32_t received = 0, total = 0;
    push_sink_progress(&received, &total);
    if (starting) {
        snprintf(sub, sizeof(sub), "蓝牙正在启动，请稍候…");
    } else if (st == PUSH_STATE_RECEIVING && total) {
        snprintf(sub, sizeof(sub), "已接收 %.1f / %.1f KB", received / 1024.0, total / 1024.0);
    } else if (st == PUSH_STATE_COOLDOWN) {
        snprintf(sub, sizeof(sub), "共 %u 字节，可继续等待下一条", (unsigned)total);
    } else {
        const uint32_t left = push_sink_window_left_ms();
        snprintf(sub, sizeof(sub), "%s", left ? "窗口剩余时间见下方提示" : "充电时常开中，拔电即退出");
    }
    ui_text(fb, 64, 386, 19, sub, EPD_DRAW_ALIGN_LEFT, false);
    if (s_push_note[0]) {
        fit_label(s_push_note, 19, 520);
        ui_text(fb, 64, 424, 19, s_push_note, EPD_DRAW_ALIGN_LEFT, false);
    } else if (st == PUSH_STATE_RECEIVING && total) {
        EpdRect track = {64, 430, 556, 12};
        ui_fill_round_rect(fb, track, 6, 0xc8);
        uint64_t width = (uint64_t)received * track.width / total;
        if (width > (uint64_t)track.width) width = track.width;
        track.width = (int)width;
        ui_fill_round_rect(fb, track, 6, UI_GRAY_BLACK);
    }
    char window[96];
    const uint32_t left_ms = push_sink_window_left_ms();
    if (starting) snprintf(window, sizeof(window), "启动完成后自动进入接收窗口");
    else if (left_ms) snprintf(window, sizeof(window), "接收窗口剩余 %u 分 %02u 秒，超时自动退出",
                          (unsigned)(left_ms / 60000u), (unsigned)((left_ms / 1000u) % 60u));
    else snprintf(window, sizeof(window), "充电时常开：没有超时，拔下充电器即退出");
    ui_text(fb, 64, 486, 18, window, EPD_DRAW_ALIGN_LEFT, false);

    // 裁定 c 的明示文案：用户必须知道这会暂停翻页器。/ Ruling c's plain notice:
    // the user has to know this pauses the page-turner.
    ui_text(fb, 42, 556, 18, "开启后将暂停翻页器连接，退出接收后自动恢复。", EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, 42, 586, 18, "接收期间屏幕不会自动锁屏，推送到会自动亮屏。", EPD_DRAW_ALIGN_LEFT, false);

    // gen24.5（用户裁定：按钮去除）：此处原是「切换充电时常开 / 常开已开启 /
    // 常开需充电」三态开关，整块删除。理由有二：一、真机 gen24.4 上它糊成一团
    // 且压住上方提示文字——灰框 + 抗锯齿文字被每秒 DU 活区重驱，反复错误驱动
    // 出糊团与重影；二、禁用态文案「常开需充电」有歧义，让人以为蓝牙开着马上
    // 就要没电。删除后武装页不再提供进入常开（window 0）的入口，底层
    // push_sink_arm(0, 0) 能力保留，日后要恢复入口另立裁定。
    // Gen24.5 (the user's ruling: remove the button): the tri-state hold switch
    // used to live here; the whole block is gone. Two reasons: on gen24.4
    // hardware it smeared into a blob over the notice line above - a gray frame
    // plus antialiased text re-driven by the per-second DU live region; and the
    // disabled label "hold needs charger" read as if Bluetooth being on were
    // about to drain the battery. With it gone the arm page no longer offers a
    // way into charger-held (window 0); push_sink_arm(0, 0) stays in the
    // component, and bringing the entry back is a separate ruling.

    EpdRect back = control_rect(2);
    if (s_pressed == 2) ui_draw_pressed_round_rect(fb, back, UI_BTN_RADIUS);
    ui_draw_button(fb, back, "退出接收", false);
    ui_nav_draw(fb, 2);
    // 单词卡盖在武装页之上：模态卡片，卡外点击由 push_gesture 关掉。
    // The word card paints over the arm page: a modal card; taps outside are
    // closed by push_gesture.
    if (s_push_word_card) dict_popup_draw(fb);
}

// 错误码 → 用户看得懂的一句话。/ Error code to one plain sentence.
static const char *push_err_text(int code) {
    switch (code) {
    case PUSH_ERR_NOT_ARMED: return "接收未开启，请在设备上点开始";
    case PUSH_ERR_BAD_FRAME: return "内容格式不符，推送被拒绝";
    case PUSH_ERR_TOO_LARGE: return "内容超过 512 KB 上限";
    case PUSH_ERR_CHECKSUM: return "内容校验失败，请重新发送";
    case PUSH_ERR_NO_CARD: return "TF 卡不可用，无法保存";
    case PUSH_ERR_LOW_BATTERY: return "电量不足且未在充电";
    case PUSH_ERR_BUSY: return "上一条推送还没处理完";
    case PUSH_ERR_NO_DICT: return "设备上没有可用词典，单词卡已取消";
    default: return "推送失败，请重试";
    }
}

// 关掉单词卡并释放其内容。/ Close the word card and release its content.
static void push_word_card_close(void) {
    if (!s_push_word_card) return;
    s_push_word_card = false;
    dict_popup_reset();
}

// 武装成功后的陈事件清扫：closed/start_failed 只在会话走到 IDLE 或待武装超时
// 时补，若它们早于本次武装就是上一回合的遗物（离页 cancel、待武装失败后重进），
// 吃了它，别把用户扔回方法页。/ Stale-event sweep after a successful arm: closed
// and start_failed are posted only when a session reaches IDLE or a pending arm
// times out, so anything posted before this arm belongs to a previous round
// (leaving the page cancels; a failed pending arm followed by a re-entry).
// Swallow it instead of throwing the user back to the method page.
static void push_drain_stale_events(void) {
    (void)push_sink_take_closed();
    (void)push_sink_take_start_failed();
    (void)push_sink_take_open(NULL, 0, NULL);
    (void)push_sink_take_word_card(NULL, 0);
    (void)push_sink_take_err(NULL);
}

// 武装页的活区：广播名卡与状态卡——倒计时/进度/状态只在这两块变。
// gen24.5（按钮糊死根治）：常开按钮**逐出活区**。gen19 起它被并进活区，
// 倒计时每秒的 DU 区域快刷把按钮区域一并反复重驱——抗锯齿灰阶像素被低质量
// 波形一次次错误驱动，糊成一团还出重影（真机 gen24.4 实锤）。按钮自己的
// 更新只走三处：进页整屏 GC16（gen24.4）、手势切开关的全页 GL16、hold 可用
// 态变化时 tick 附加的高质量 GL16 区域刷（见 push_tick）。
// The arm page's live regions: the adv-name card and the status card - the
// countdown, progress and state only ever change in these two.
// Gen24.5 (the smeared-button root fix): the hold button is evicted from the
// live region. Since gen19 it was unioned in, so the per-second DU area
// refresh re-drove the button region over and over - the antialiased gray
// pixels got mis-driven again and again into a smeared, ghosted blob
// (confirmed on gen24.4 hardware). The button now refreshes only in three
// places: the entry full GC16 (gen24.4), the full GL16 page after a tap
// toggles it, and the high-quality GL16 area refresh the tick appends when
// hold availability changes (see push_tick).
static EpdRect push_dynamic_rect(void) {
    return ui_rect_union((EpdRect){36, 168, 612, 116},
                         (EpdRect){36, 308, 612, 224});
}

// 把活区簿记对齐当前推流状态：调用方刚整页画过或刚武装时用，避免下一拍做
// 内容相同的重复刷新。/ Align the live-region bookkeeping with the current push
// state: for callers that just painted the full page or just armed, so the next
// tick does not repeat an identical refresh.
static void push_sync_live_bookkeeping(void) {
    s_push_last_state = push_sink_state();
    s_push_last_left_s = push_sink_window_left_ms() / 1000u;
    uint32_t received = 0, total = 0;
    push_sink_progress(&received, &total);
    s_push_last_recv = received;
    s_push_last_total = total;
    snprintf(s_push_last_adv, sizeof(s_push_last_adv), "%s", push_sink_adv_name());
}

// 把一次 closed 的原因落到提示条与块 6 底账：推送页与方法页两处消费同源。
// Absorb one closed event's reason into the hint and the block-6 ledger - the
// push page and the method page consume it from this one place.
static void absorb_close_why(void) {
    const char *why = push_sink_last_close_why();
    if (!why) return;
    snprintf(s_last_cancel_why, sizeof(s_last_cancel_why), "%s", why);
    snprintf(s_method_message, sizeof(s_method_message), "%s", why);
}

// 武装页轮询：消费 push_sink 的一次性 UI 事件，500ms 节流刷新倒计时与进度。
// The arm page's pump: consume the push sink's one-shot UI events and refresh the
// countdown and progress at a 500 ms cadence.
static app_redraw_t push_tick(app_ctx_t *ctx) {
    // start_failed/closed 决定去留；open/word_card/err 只换页内文案。
    // start_failed/closed decide the page's fate; the rest swap the message.
    // start_failed 先于 closed 查：广播起不来时两个标志都会置位（CANCEL 的
    // ADV_OFF 顺带发 closed），「蓝牙启动失败，请重试」比裸回方法页信息量大，
    // 且两者互斥出现只可能是失败路径，正常收官只置 closed。
    // start_failed is checked before closed: when advertising cannot start,
    // both flags get set (CANCEL's ADV_OFF posts closed on the way out) and
    // the failure message is strictly more informative; the two coexist only
    // on the failure path - a normal close sets closed alone.
    if (push_sink_take_start_failed()) {
        push_word_card_close();
        (void)push_sink_take_closed(); // 吞掉同路的 closed，别闪两帧 / swallow the paired closed flag
        s_view = TRANSFER_METHODS;
        snprintf(s_method_message, sizeof(s_method_message), "蓝牙启动失败，请重试");
        return APP_REDRAW_PAGE;
    }
    if (push_sink_take_closed()) {
        push_word_card_close();
        s_view = TRANSFER_METHODS;
        // gen18：收摊必须带原因——窗口到期/拔电/飞行模式/USB 各说各的话，
        // 用户看得懂下一步该干什么（错误文案要能指路）。
        // gen18: a close-up must carry its reason - window expiry, unplug,
        // flight mode and USB each say their own piece, so the user knows
        // what to do next (error text must point the way).
        // gen24.2 块 5/6：原因同时进底账——用户随后进传书页也看得到。
        // Gen24.2 blocks 5/6: the reason also lands in the ledger, so the
        // user still sees it after entering the transfer page later.
        absorb_close_why();
        return APP_REDRAW_PAGE;
    }
    char title[PUSH_TITLE_MAX];
    uint8_t type = 0;
    if (push_sink_take_open(title, sizeof(title), &type)) {
        // gen25 让路链收官：这次推送是靠「临时卸书」才过闸的，收官就把用户送回
        // 原来那本书（位置由 book_progress 单一真源恢复）；推送内容留在书架，
        // 首页/书架看得到。restore 失败**必须出声**（裁定 #2）：给一句准话 +
        // 方法页保留「返回阅读」入口，绝不静默把书弄丢。
        // Gen25 yield chain finale: this push only cleared the gate because the
        // book was unloaded, so the finale walks the user back to that book
        // (position restored by book_progress, the single source); the pushed
        // text stays on the shelf, visible from home/shelf. A failed restore
        // **must speak** (ruling #2): a definite sentence plus the method page's
        // "back to reading" entry - never silently lose the book.
        if (app_book_yield_active()) {
            extern const app_desc_t app_book; // 与下方开书分支同款 / same shape as the open branch below
            const bool restored = app_book_restore_after_push();
            // B 留在书架：标记它的首次手动打开仍走一次 GC16 打底（真机项 f）。
            // B stays on the shelf: flag its first manual open for one GC16 base
            // (hardware item f).
            app_book_mark_push_first_open();
            snprintf(s_method_message, sizeof(s_method_message),
                     restored ? "已收到《%s》，可在书架查看" : "已收到《%s》；原书未能自动重开，请手动打开",
                     title);
            ESP_LOGW(TAG, "push finale after yield: restore %s", restored ? "ok" : "FAILED");
            ctx->request_app = &app_book;
            return APP_REDRAW_NONE;
        }
        // 虚拟书：标题覆盖成推送标题、清掉旧进度，再切阅读页——「收完统一写
        // push.txt → 一次全刷」的全刷落在 app_book 的开书路径上，武装页不抢。
        // Virtual book: override the display title with the pushed one, drop
        // stale progress, then switch to the reader - the single full refresh
        // belongs to app_book's open path, so the arm page does not steal it.
        // gen24.4（渲染发灰根治）：改走 _clean 变体——全刷必须真正落在阅读
        // 页第一页上。gen24.3 真机报「推送完成后打开第一页也有残影」：跨 app
        // 切页走 APP_REDRAW_PAGE，阅读页第一页原是 GL16 差分，武装页的灰底
        // 卡片墨迹留着。_clean 让 app_book 的 present 对第一页强制整屏 GC16。
        // Gen24.4 (the gray-render root fix): take the _clean variant - the
        // full refresh must truly land on the reader's first page. gen24.3
        // hardware reported "the first page after a completed push also has
        // ghosting": the cross-app switch presents APP_REDRAW_PAGE, and the
        // reader's first page was a GL16 differential that kept the arm page's
        // gray-card ink. _clean makes app_book's present force a screen-wide
        // GC16 for that first page.
        (void)book_title_set(PUSH_TEXT_PATH, title);
        (void)book_progress_forget(PUSH_TEXT_PATH);
        extern const app_desc_t app_book;
        if (app_book_request_open_clean(PUSH_TEXT_PATH)) {
            // gen18（18-a）：开书即交接——标记这次离页，transfer_on_exit 据此
            // 不收摊；摊子由阅读器退出时（book_on_exit）才收。
            // gen18 (18-a): opening the book is the handoff - flag this exit so
            // transfer_on_exit keeps the arm; only leaving the reader closes up.
            s_push_handed_off = true;
            ctx->request_app = &app_book;
            return APP_REDRAW_NONE;
        }
        snprintf(s_push_note, sizeof(s_push_note), "打开推送内容失败，请重试");
        return APP_REDRAW_PAGE;
    }
    char word[PUSH_TITLE_MAX];
    if (push_sink_take_word_card(word, sizeof(word))) {
        // 单词卡：查词并开卡片；未收录或词典不在给一句准话，不黑屏不白卡。
        // Word card: look the word up and open the card; a miss or a missing
        // dictionary gets a definite sentence, never a blank card.
        dict_popup_set_anchor(0, false);
        if (dict_popup_open(word)) {
            s_push_word_card = true;
            return APP_REDRAW_PAGE;
        }
        if (dict_ready()) snprintf(s_push_note, sizeof(s_push_note), "未收录：%s", word);
        else snprintf(s_push_note, sizeof(s_push_note), "词典不可用，无法查词");
        return APP_REDRAW_PAGE;
    }
    int code = 0;
    if (push_sink_take_err(&code)) {
        snprintf(s_push_note, sizeof(s_push_note), "%s", push_err_text(code));
        return APP_REDRAW_PAGE;
    }
    if (ctx->now_ms - s_push_poll_ms < 500) return APP_REDRAW_NONE;
    s_push_poll_ms = ctx->now_ms;
    // gen24.2 R2：回退条件收紧为「非 armed 且非 pending」。pending 是点了
    // 接收、栈正在拉起的合法在途态（app_loop 的 sync 会拉栈，on_stack_sync
    // 续上）——此时闪回方法页，用户看到的就是「点了没反应」，随后 10s 超时
    // 的 start_failed 又烂死在无人消费的方法页（gen24.2 问题 1/2 死锁链的
    // 第二、三环）。 armed 丢失（事件漏接的极端时序）仍照旧放回。
    // Gen24.2 R2: the fallback tightens to "neither armed nor pending". A
    // pending arm is a legitimate in-flight state (receive pressed, the stack
    // is coming up; app_loop's sync pulls it and on_stack_sync completes it) -
    // bouncing back here showed the user a dead tap, and the 10 s timeout's
    // start_failed then rotted in a method page nobody consumed (the second
    // and third links of the gen24.2 problem 1/2 deadlock chain). A genuinely
    // lost armed flag (missed-event extreme timing) still walks back as before.
    if (!push_sink_armed() && !push_sink_pending()) {
        push_word_card_close();
        s_view = TRANSFER_METHODS;
        return APP_REDRAW_PAGE;
    }
    // 活区没动就不刷：倒计时每秒才走一格，30 分钟窗口里每 500ms 一次全页 GL16
    // 等于几千次全闪，防闪烁不许这么干；有动就只重绘活区 + DU 区域更新。
    // Nothing moved, nothing to paint: the countdown steps once a second, so a
    // full GL16 every 500 ms would mean thousands of full flashes across a
    // 30-minute window - the no-flicker ruling forbids that. When something did
    // move, repaint only the live regions and present a DU area update.
    const push_state_t st = push_sink_state();
    const uint32_t left_s = push_sink_window_left_ms() / 1000u;
    uint32_t received = 0, total = 0;
    push_sink_progress(&received, &total);
    const char *adv = push_sink_adv_name();
    if (st == s_push_last_state && left_s == s_push_last_left_s &&
        received == s_push_last_recv && total == s_push_last_total &&
        !strcmp(adv, s_push_last_adv)) return APP_REDRAW_NONE;
    s_push_last_state = st;
    s_push_last_left_s = left_s;
    s_push_last_recv = received;
    s_push_last_total = total;
    snprintf(s_push_last_adv, sizeof(s_push_last_adv), "%s", adv);
    draw_push(ctx->fb);
    // gen24.5：活区只剩广播名卡与状态卡——常开按钮已删除，倒计时每秒的 DU
    // 快刷不会再压到任何静态控件上（糊死根因随按钮一并消失）。
    // Gen24.5: the live region is the adv-name card and the status card only -
    // with the hold button gone, the per-second DU refresh no longer lands on
    // any static control (the smear root cause left with the button).
    s_area = push_dynamic_rect();
    return APP_REDRAW_AREA;
}

// gen24.2 块 5（R3 结构性根修）：方法页轮询也消费一次性事件。
// gen24.2 前方法页 tick 直接 APP_REDRAW_NONE——一切 start_failed/closed
// 到了方法页就烂死：pending 超时失败（问题 1）与关开关收摊（问题 2）的
// 原因永远到不了屏幕，用户看到的就是「点了没反应」。事件在此落
// s_method_message 并整页重绘；极端闭环（pending 中关开关 → cancel →
// closed）由本函数与 push_tick 共同覆盖。
// Gen24.2 block 5 (R3 structural root fix): the method page's pump also
// consumes the one-shot events. Pre-gen24.2 the method tick returned
// APP_REDRAW_NONE outright, so every start_failed/closed that arrived here
// rotted: the pending-arm timeout (problem 1) and the switch-off close-up
// (problem 2) never reached the screen - the user just saw a dead tap. The
// events land in s_method_message with a full repaint; the extreme loop
// (switch off during pending -> cancel -> closed) is covered here and in
// push_tick together.
static app_redraw_t methods_tick(app_ctx_t *ctx) {
    (void)ctx; // 本页轮询只消费事件，不用节流时钟 / this pump only consumes events, no clock needed
    // start_failed 先于 closed 查：pending 超时路径两个标志都会置位
    // （与 push_tick 同序，理由同其注释）。/ start_failed before closed,
    // same order and rationale as push_tick's.
    if (push_sink_take_start_failed()) {
        (void)push_sink_take_closed();
        snprintf(s_method_message, sizeof(s_method_message), "蓝牙启动失败，请重试");
        s_method_inline_enable = false;
        ESP_LOGW(TAG, "methods page: pending arm failed, stack never came up");
        return APP_REDRAW_PAGE;
    }
    if (push_sink_take_closed()) {
        // gen24.2 块 6：原因同时进底账——用户随后进传书页也看得到。
        // Block 6: the reason also lands in the ledger, so the user still
        // sees it after entering the transfer page later.
        absorb_close_why();
        s_method_inline_enable = false;
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

// 武装页手势：常开开关、退出接收、按下反馈。/ The arm page's gestures: the hold
// switch, the exit button and press feedback.
static app_redraw_t push_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    // 单词卡是模态的：开着时所有点击先问卡片——chip 命中切换保存、卡片留着，
    // 其余点击关卡片；按下反馈一律吞掉（卡片盖着的按钮不给人按的错觉）。
    // The word card is modal: while it is open every tap asks the card first -
    // a chip hit toggles the save and keeps the card, any other tap closes it;
    // press feedback is swallowed outright (no illusion of pressing a button
    // the card covers).
    if (s_push_word_card) {
        // 非点击不动屏幕（按下反馈一律吞掉：卡片盖着的按钮不给人按的错觉）；
        // 点击才问 chip，命中切换保存、未命中关卡片，两者都要重绘。
        // Non-taps change nothing on screen (press feedback is swallowed: no
        // illusion of pressing a button the card covers); only a tap asks the
        // chip - a hit toggles the save, a miss closes the card - and either
        // way the page repaints once.
        if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
        (void)dict_popup_tap(ev->x0, ev->y0);
        push_word_card_close();
        s_pressed = -1;
        return APP_REDRAW_PAGE;
    }
    int old = s_pressed;
    int origin = -1;
    // gen24.5：常开按钮去除后武装页只剩「退出接收」一个可点控件。
    // Gen24.5: with the hold button gone the arm page has exactly one tappable
    // control left - "exit receive".
    if (ui_rect_hit(control_rect(2), ev->x0, ev->y0)) origin = 2;
    s_pressed = ev->type == UI_GESTURE_PRESS ? origin : -1;
    if (ev->type != UI_GESTURE_TAP || old < 0 || old != origin) {
        if (old == s_pressed) return APP_REDRAW_NONE;
        render(ctx, ctx->fb);
        s_settle = false;
        s_area = control_rect(2);
        return APP_REDRAW_AREA;
    }
    // 退出接收：cancel 之后 closed 事件会把页面带回方法页。
    // Exit: after the cancel the closed event walks the page back.
    push_sink_cancel();
    return APP_REDRAW_NONE;
}

static void render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    if (s_usb_entry) {
        ui_clear_page(fb);
        bool active = usb_storage_active();
        transfer_header(fb, "USB 读卡", NULL);
        ui_text(fb, 42, 181, 20, "连接状态", EPD_DRAW_ALIGN_LEFT, false);
        EpdRect status = {36, 211, 612, 190};
        ui_fill_round_rect(fb, status, 24, UI_GRAY_WHITE);
        ui_draw_round_rect(fb, status, 24, 0x90);
        const char *state = s_usb_start_pending ? "正在准备 TF 卡" :
            !active ? "USB 读卡未开启" :
            s_usb_host_connected ? "电脑已连接" : "等待电脑连接";
        ui_text(fb, 64, 241, 31, state, EPD_DRAW_ALIGN_LEFT, false);
        if (active) {
            ui_text(fb, 64, 296, 20, "磁盘名称  SD_CARD", EPD_DRAW_ALIGN_LEFT, false);
            char size[64];
            snprintf(size, sizeof(size), "TF 卡容量  %.1f GB", s_usb_capacity_bytes / 1073741824.0);
            ui_text(fb, 64, 337, 20, size, EPD_DRAW_ALIGN_LEFT, false);
        } else ui_text(fb, 64, 309, 20, s_usb_message, EPD_DRAW_ALIGN_LEFT, false);

        ui_text(fb, 42, 450, 20, "文件存放位置", EPD_DRAW_ALIGN_LEFT, false);
        ui_fill_round_rect(fb, (EpdRect){36, 478, 612, 206}, 24, UI_GRAY_WHITE);
        ui_text(fb, 64, 510, 27, "books", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 64, 553, 19, "EPUB、TXT 书籍", EPD_DRAW_ALIGN_LEFT, false);
        ui_hairline(fb, 580, 64, 556, 0xb0);
        ui_text(fb, 64, 597, 27, "fonts", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 64, 640, 19, "TTF、OTF 字体", EPD_DRAW_ALIGN_LEFT, false);

        ui_text(fb, 42, 740, 20, "完成传输", EPD_DRAW_ALIGN_LEFT, false);
        ui_fill_round_rect(fb, (EpdRect){36, 766, 612, 147}, 24, UI_GRAY_WHITE);
        ui_text(fb, 64, 802, 21, "1  在电脑上等待复制完成", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 64, 855, 21, "2  安全弹出 SD_CARD，再结束读卡", EPD_DRAW_ALIGN_LEFT, false);
        ui_fill_round_rect(fb, control_rect(2), 22, s_pressed == 2 ? 0x60 : 0x38);
        ui_text_vc(fb, 342, 1014, 24, active ? "结束 USB 读卡" : "返回文件管理",
                   EPD_DRAW_ALIGN_CENTER, true);
        return;
    }
    if (s_view == TRANSFER_METHODS) { draw_methods(fb); return; }
    if (s_view == TRANSFER_NETWORKS) { draw_networks(fb); return; }
    if (s_view == TRANSFER_PASSWORD) { draw_password(fb); return; }
    if (s_view == TRANSFER_PUSH) { draw_push(fb); return; }
    ui_clear_page(fb);
    transfer_header(fb, s_mode == READ_PICO_TRANSFER_MODE_AP ? "热点传书" : "WiFi 传书", NULL);
    EpdRect connection = {36, 168, 612, 132};
    ui_fill_round_rect(fb, connection, 24, UI_GRAY_WHITE);
    ui_draw_round_rect(fb, connection, 24, 0x98);
    char name[64];
    snprintf(name, sizeof(name), "%s", s_mode == READ_PICO_TRANSFER_MODE_AP ? s_status.ssid : s_status.wifi_ssid);
    if (!name[0]) snprintf(name, sizeof(name), "%s", s_mode == READ_PICO_TRANSFER_MODE_AP ? "kiikoread 热点" : "已保存的 WiFi");
    fit_label(name, 26, 420);
    ui_text(fb, 66, 190, 26, name, EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, 66, 236, 19,
            s_mode == READ_PICO_TRANSFER_MODE_AP ? "口令：" READ_PICO_TRANSFER_PASSWORD :
            s_status.network_ready ? "设备已连接；手机需同网" :
            s_start_pending || s_status.state == READ_PICO_TRANSFER_STARTING ? "正在连接已保存网络…" :
            s_status.wifi_configured ? "已保存，尚未连接" : "尚未选择 WiFi",
            EPD_DRAW_ALIGN_LEFT, false);
    ui_text_vc(fb, 605, 234, 20, s_status.network_ready ? "服务已开启" : "准备中", EPD_DRAW_ALIGN_RIGHT, false);
    int url_px = UI_PX_CAPTION;
    while (url_px > 14 && ttf_text_width_px(url_px, s_status.url) > 560) --url_px;
    if (s_status.network_ready && s_qr_ready)
        ui_wifi_qr_draw(fb, (EpdRect){222, 330, 240, 240});
    else {
        ui_draw_round_rect(fb, (EpdRect){222, 330, 240, 240}, 8, 0xa0);
        ui_text_vc(fb, 342, 450, 22, s_start_pending ? "正在生成二维码…" : "等待网络地址…", EPD_DRAW_ALIGN_CENTER, false);
    }
    if (s_mode == READ_PICO_TRANSFER_MODE_AP) {
        ui_text_vc(fb, 342, 583, 20,
                   s_status.network_ready ? s_status.url : "热点就绪后显示访问地址",
                   EPD_DRAW_ALIGN_CENTER, false);
        for (int i = 0; i < 2; ++i) {
            EpdRect tab = control_rect(i);
            ui_fill_round_rect(fb, tab, 18, (s_qr_url == (i == 1)) ? 0x42 : UI_GRAY_WHITE);
            ui_draw_round_rect(fb, tab, 18, 0x92);
            ui_text_vc(fb, tab.x + tab.width / 2, tab.y + tab.height / 2, 21,
                       i ? "打开传书网页" : "连接 kiikoread 热点", EPD_DRAW_ALIGN_CENTER, s_qr_url == (i == 1));
        }
    } else {
        ui_text_vc(fb, 342, 602, url_px,
                   s_status.network_ready ? s_status.url : "服务启动后显示访问地址", EPD_DRAW_ALIGN_CENTER, false);
        ui_text_vc(fb, 342, 642, 18, "手机或电脑扫码后，即可在浏览器上传内容", EPD_DRAW_ALIGN_CENTER, false);
    }
    draw_status(fb);
    EpdRect back = control_rect(2);
    if (s_pressed == 2) ui_draw_pressed_round_rect(fb, back, UI_BTN_RADIUS);
    ui_draw_button(fb, back,
                   s_mode == READ_PICO_TRANSFER_MODE_AP && s_status.state == READ_PICO_TRANSFER_ERROR && !s_status.network_ready
                       ? "重试热点传书" : "停止传输服务", false);
    ui_nav_draw(fb, 2);
}

// gen19（真机 Bug2）：武装动作收敛成助手，进场（on_enter）与页内点击
// （method_gesture）共用。gen18 只让 on_enter 消费 s_push_requested——用户已在
// 本页内点「手机推送」时 on_enter 不会重跑，flag 挂死、按钮「每次都没反应」，
// 只有退出重进才生效。页面已显示，武装当场执行。
// gen19 (real-hw Bug2): the arming move lives in this helper, shared by entry
// (on_enter) and the in-page tap (method_gesture). gen18 let only on_enter
// consume s_push_requested - tapping "phone push" while already on this page
// never re-ran on_enter, so the flag hung and the button did nothing until a
// leave-and-reenter. The page is already showing: arm right now.
static void push_arm_now(void) {
    const esp_err_t err = push_sink_arm(0, PUSH_ENTRY_WINDOW_MS);
    if (err == ESP_ERR_NOT_ALLOWED) {
        snprintf(s_method_message, sizeof(s_method_message), "电量不足，无法开启接收");
        return;
    }
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        snprintf(s_method_message, sizeof(s_method_message), "接收开启失败（%d）", err);
        return;
    }
    s_view = TRANSFER_PUSH;
    // gen24.4：本视图第一次呈现强制整屏 GC16——方法页切过来同样带着旧墨，
    // 差分打底会留残影。/ Gen24.4: this view's first presentation forces a
    // screen-wide GC16 - switching over from the method page carries old ink
    // too, and a differential landing keeps the ghosting.
    s_push_entry_full_once = true;
    // 新回合干净开局：单词卡收掉、陈事件清扫（离页补的 closed、上一回合
    // 待武装失败补的 start_failed 等）、活区簿记对齐当前值。
    // A clean slate for the new round: drop the word card, sweep stale events
    // and align the live-region bookkeeping.
    // gen24.2 块 6：收摊原因底账只挂到下一次武装为止——新回合开局不再拿
    // 旧原因指路。/ Gen24.2 block 6: the close-up reason ledger hangs only
    // until the next arm - a fresh round stops quoting the stale reason.
    s_last_cancel_why[0] = 0;
    push_word_card_close();
    push_drain_stale_events();
    push_sync_live_bookkeeping();
}

// gen24 多态按钮：「手机推送」行按 push_sink 状态一台戏唱四种姿态，点按动作
// 状态驱动，渲染与判定同源（push_sink_state + ble_pt_locked + ble_pt_mem_gate）。
//   IDLE（干净）              → 过四门后直接武装；BLE 栈未起由 pending 机制静默拉起
//   IDLE（上次失败/自锁）      → 一键自愈：预检过 → 清锁 + 重试，一次到位
//                               预检不过 → 行内提示碎片数字，零等待（不吞 30s 退避）
//   ARMED/WINDOW/COOLDOWN     → 取消，回 IDLE
//   CONNECTED/RECEIVING/PROCESSING → 禁用，传输中不打断（给一句话，不静默）
// gen24.2 前置四门（块 3）：武装前依次查 内存 → 蓝牙总开关 → 飞行模式 →
// USB 存储；任何一门不过都行内给话 + 零武装。gen24.2 前的代码没有开关门——
// 开关关着时按钮进 pending，而 app_loop 的拉栈条件不算 pending，栈永远不起
// 来，10s 超时事件又烂死在无人消费的方法页——「点了没反应」的静默死锁。
// 不假死底线：任何分支结束后方法页其余按钮照常可用、状态可再变更。
// gen24 multi-state button: the phone-push row plays four poses off push_sink state;
// the tap action is state-driven, with rendering and verdict sharing one source
// (push_sink_state + ble_pt_locked + ble_pt_mem_gate).
//   clean IDLE -> pass the four gates, then arm; a down stack is pulled up by
//   the pending mechanism.
//   failed/self-locked IDLE -> one-tap self-heal (clear lock + retry on pass;
//   inline fragment numbers on fail, zero wait).
//   ARMED/WINDOW/COOLDOWN -> cancel back to IDLE.
//   CONNECTED/RECEIVING/PROCESSING -> disabled mid-transfer (with a sentence,
//   never silent).
// gen24.2 front four gates (block 3): before arming, check memory -> BLE
// master switch -> flight mode -> USB storage in order; any refusal speaks
// inline and arms nothing. Pre-gen24.2 code had no switch gate - with the
// switch off the button pended the arm, but app_loop's stack-pull condition
// never counted pending, so the stack never came up and the 10 s timeout
// event rotted in a method page nobody consumed: the silent deadlock of a
// dead tap. The no-freeze bottom line: after any branch the other method
// rows keep working and the state stays mutable.
static void push_button_action(void) {
    const push_state_t st = push_sink_state();
    // 传输中：禁用——但必须给一句话。gen24.2 前这里静默 return，而按压反馈
    // 照旧，看起来能按、按了没反应（铁律违规第 1 号）。
    // Mid-transfer: disabled - but it must speak. Pre-gen24.2 this returned
    // silently while press feedback still played, so the row looked pressable
    // and did nothing (iron-rule violation #1).
    if (st == PUSH_STATE_CONNECTED || st == PUSH_STATE_RECEIVING || st == PUSH_STATE_PROCESSING) {
        snprintf(s_method_message, sizeof(s_method_message), "手机正在推送，完成后自动结束");
        s_method_inline_enable = false;
        return;
    }
    // 已武装/窗口/宽限期：再点即取消（gen18 取消必须带原因）。
    // Already armed / in window / in grace: tap again cancels (gen18: a cancel must
    // carry a reason).
    if (st != PUSH_STATE_IDLE) {
        push_sink_cancel_why("已取消接收");
        s_method_message[0] = 0;
        s_method_inline_enable = false;
        return;
    }
    // IDLE：四门。栈已在运行 → 跳过内存预检直接武装；栈 down → 预检先行。
    // IDLE: the four gates. Stack already running -> skip the memory precheck
    // and arm directly; stack down -> precheck first.
    // gen24.1（D2 冤拒修复）：闸门的语义是预测「NimBLE 能否启动」（控制器与协议栈
    // 任务必须落内部 RAM，跑硬件路径读不到 PSRAM）。栈已经起来时这个问题不存在、
    // 武装也不需要任何新的控制器分配——此处拦截就是冤枉。boot1 实证：BLE 自开机起
    // up，两本大书的碎片窗口（largest 19K）内点按钮被拒，而当时推流根本无需启动
    // 栈。恢复 gen23 语义：栈 up 直接走既有武装路径，栈 down 才预检先行。
    // gen24.1 (D2 unjust-refusal fix): the gate predicts "can NimBLE start" (the
    // controller and stack task must live in internal RAM - they sit on the
    // hardware path and cannot read PSRAM). With the stack already up that
    // question does not exist and the arm needs no new controller allocation, so
    // refusing here is unjust. Boot1 evidence: BLE up since boot, an in-book tap
    // refused inside the two-book fragmentation window (largest 19K) although
    // the push needed no stack start at all. Restores the gen23 semantics:
    // stack up -> take the existing arm path; stack down -> precheck first.
    s_push_requested = false;
    if (!ble_pt_running()) {
        size_t free_internal, largest_internal, free_psram;
        ble_pt_mem_snapshot(&free_internal, &largest_internal, &free_psram);
        if (!ble_pt_mem_gate(free_internal, largest_internal, free_psram)) {
            // gen24.1（D3 仪表）：与 UI 提示同源记日志——真机日志由此可分辨「按钮
            // 预检拒绝」与 start_locked 的「启动拒绝」（后者也打日志），gen24 真机
            // 轮就因缺这一行分不清两种拒绝。
            // gen24.1 (D3 instrumentation): log with the same figures as the UI
            // hint - hardware logs can now tell a button precheck refusal from a
            // start_locked refusal (which also logs); the gen24 hardware round
            // could not tell the two apart for want of this line.
            ESP_LOGW(TAG, "push precheck refused: internal %u B free / %u B largest (gate %u K), psram %u B free",
                     (unsigned)free_internal, (unsigned)largest_internal,
                     (unsigned)(BLE_PT_MIN_LARGEST_INTERNAL / 1024), (unsigned)free_psram);
            // gen24.1（D1 文案说真话）：gen24 原文案「重启设备后可用」被真机证伪——
            // NVS 自锁（tries+why）跨重启存活，重启 alone 并不解锁；且重启后开大书
            // 几秒内 largest 再破闸（boot2：70.4s 开书 → 76.2s 跌到 19K）。boot1
            // 实证离开书籍后 largest 自行恢复（19456 → 31744），故主路径是「退出
            // 当前书籍后重试」，重启降为兜底。
            // gen24.1 (D1 the hint tells the truth): gen24's wording "restart the
            // device to use it" was falsified on hardware - the NVS self-lock
            // (tries+why) survives a restart, so a restart alone does not unlock;
            // and after a restart a big book shreds the largest block below the
            // gate within seconds (boot2: book open at 70.4s -> 19K at 76.2s).
            // Boot1 proved leaving the book recovers the largest block on its own
            // (19456 -> 31744), so the primary path is "exit the current book and
            // retry", with restart as the fallback.
            // gen25（让路链，接线裁定 #2 触发面收窄）：只有「栈 down + 预检不过
            // + 确实有书开着」三条件同时成立才让路——栈 up（D2 直达）、无书、
            // 预检过一律不触发，不让让路变成所有预检失败的默认动作。
            // Gen25 (yield chain, wiring ruling #2 narrowing): yield only when
            // "stack down + precheck failed + a book is really open" all hold -
            // never when the stack is up (D2 goes straight through), never with
            // no book, never when the precheck passes. Yielding must not become
            // the default action for every precheck failure.
            if (app_book_has_open_book()) {
                // 裁定 #1：**先出提示，再异步分拍等**——同步等 done 最长 1000ms
                // 会让按钮按下去整整 1 秒没有 UI 变化（死感）。这里只发非阻塞
                // 停机请求，UI 立刻画出「正在为推送腾出内存…」，让路的收尾交给
                // push_tick 的下一拍（见 s_push_yield_phase）。
                // Ruling #1: **show the hint first, then wait across ticks** -
                // waiting for done synchronously for up to 1000 ms would leave a
                // full second with no UI change (the dead feel). This only issues
                // the non-blocking stop request; the hint paints immediately and
                // push_tick's next tick finishes the yield (see s_push_yield_phase).
                app_book_request_prep_stop();
                s_push_yield_phase = 1;
                s_push_yield_deadline = 0; // 由 push_tick 首拍按 ctx->now_ms 记起点 / push_tick stamps the start on its first tick
                snprintf(s_method_message, sizeof(s_method_message), "正在为推送腾出内存…");
                s_method_inline_enable = false;
                return;
            }
            snprintf(s_method_message, sizeof(s_method_message),
                     "内存碎片不足（最大块 %uK，需 %uK）——退出当前书籍后重试；若仍失败，请重启设备",
                     (unsigned)(largest_internal / 1024),
                     (unsigned)(BLE_PT_MIN_LARGEST_INTERNAL / 1024));
            s_method_inline_enable = false;
            return;
        }
    }
    // gen24.2 门 2（块 3，B 模型）：蓝牙总开关。开关关着时不武装、不进
    // pending——给行内提示 + 【开启】按钮，用户原地写设置项继续，不必跳
    // 设置页。置于内存门之后（执行令定稿顺序），但早于飞行/USB：开关没开
    // 是最高频的拦截，值得第一个给按钮。
    // gen24.2 gate 2 (block 3, model B): the BLE master switch. With it off,
    // nothing arms and nothing pends - an inline hint plus an [enable] button
    // writes the setting in place and continues, with no trip to the settings
    // page. It sits after the memory gate (the finalised order) but before
    // flight/USB: the switch is the most frequent refusal and earns the button.
    if (!app_settings_ble_turner()) {
        ESP_LOGW(TAG, "push gate refused: ble master switch off (settings)");
        snprintf(s_method_message, sizeof(s_method_message), "蓝牙已关闭，点按右侧「开启」后继续");
        s_method_inline_enable = true;
        return;
    }
    // gen24.2 门 3（块 3）：飞行模式。/ Gen24.2 gate 3: flight mode.
    if (app_settings_flight_mode()) {
        ESP_LOGW(TAG, "push gate refused: flight mode on");
        snprintf(s_method_message, sizeof(s_method_message), "飞行模式开启中，请先关闭");
        s_method_inline_enable = false;
        return;
    }
    // gen24.2 门 4（块 3）：USB 存储接管中。/ Gen24.2 gate 4: USB storage owns
    // the card right now.
    if (usb_storage_active()) {
        ESP_LOGW(TAG, "push gate refused: usb storage active");
        snprintf(s_method_message, sizeof(s_method_message), "USB 传书已接管，接收推送暂停");
        s_method_inline_enable = false;
        return;
    }
    // 预检过：一键自愈——上次失败/自锁先清掉（幂等；没锁时调用无害），再走既有
    // 武装路径（push_sink_arm → pending 静默拉栈，gen12 的启动链原样复用）。
    // Precheck passed: one-tap self-heal - clear any failure/lock first (idempotent,
    // harmless when not locked), then take the existing arm path (push_sink_arm ->
    // pending silently brings the stack up; gen12's start chain is reused verbatim).
    ble_pt_reset_failure();
    push_arm_now();
}

// gen24.2 行内【开启】按钮（块 3 + §二 设计要点）：原地写设置项（关→开），
// 再重走四门——门 2 此时已过，内存/飞行/USB 复查，全过则走既有武装路径
// （push_sink_arm → pending 拉栈），不引入第二套启动路径。写入与复查之间
// 无第二份状态：设置项就是单一真源，与设置页手动开启天然收敛。
// Gen24.2's inline [enable] button (block 3 + section 2's design points):
// write the setting in place (off->on), then re-run the four gates - gate 2
// has passed by now, memory/flight/USB are rechecked, and a full pass takes
// the existing arm path (push_sink_arm -> pending pulls the stack). No second
// start path and no second state: the setting itself is the single source of
// truth, converging naturally with a manual flip on the settings page.
static void inline_enable_push(void) {
    s_method_inline_enable = false;
    app_settings_set_ble_turner(true);
    ESP_LOGI(TAG, "inline enable: ble master switch written on, re-running the four gates");
    // 提示条先行更新为「正在启动蓝牙…」——若四门复查失败，下面的
    // push_button_action 会用对应门的提示覆盖它（§二-3 状态迁移）。
    // The hint bar moves to "starting bluetooth..." first - if a gate refuses
    // on recheck, push_button_action below overwrites it with that gate's
    // hint (section 2-3's state migration).
    snprintf(s_method_message, sizeof(s_method_message), "正在启动蓝牙…");
    push_button_action();
}

static void on_enter(app_ctx_t* ctx) {
    (void)ctx;
    display_set_bulk_io(true);
    s_media_lost = false;
    memset(&s_status, 0, sizeof(s_status));
    s_qr_url = s_mode == READ_PICO_TRANSFER_MODE_STA;
    clear_qr();
    memset(&s_root, 0, sizeof(s_root));
    s_heap_before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    s_internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    // gen24 诊断：进传书页记一笔堆快照，口径与 BLE 自愈预检/start_locked 完全一致
    // （内部 free/最大块 + PSRAM free/最大块），真机日志可直接与 `book: open start`
    // 风格的行对表——gen25 定位「长使用周期内部堆布局退化」的直接数据。
    // gen24 diagnostics: snapshot the heap on entry in exactly the units the BLE
    // self-heal precheck and start_locked use (internal free/largest + PSRAM
    // free/largest), so real-hardware logs line up with the `book: open start`
    // style directly - the data gen25 needs to pin down the long-run internal-heap
    // layout degradation.
    {
        size_t free_internal, largest_internal, free_psram;
        ble_pt_mem_snapshot(&free_internal, &largest_internal, &free_psram);
        ESP_LOGI(TAG, "on_enter internal free=%u largest=%u psram free=%u largest=%u",
                 (unsigned)free_internal, (unsigned)largest_internal, (unsigned)free_psram,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    s_start_pending = false;
    s_usb_message[0] = 0;
    s_usb_start_pending = false;
    s_usb_retry_ms = 0;
    s_usb_host_connected = false;
    s_usb_capacity_bytes = 0;
    s_method_message[0] = 0;
    // gen24.2 块 6：取一次收摊原因快照进底账并挂上提示条——push_sink 的
    // s_close_why 会被下一次武装清掉，而「上次为什么结束」在用户下次进
    // 页面时仍有指路价值（窗口到期/拔电/关开关都算）。armed 保活回连
    // （1494 分支）时原因通常是空的，不挂。/ Block 6: snapshot the close-up
    // reason into the ledger and hang it on the hint bar - push_sink's own
    // s_close_why is cleared by the next arm, yet "why it ended last time"
    // still points the way on the next entry (window expiry, unplug and
    // switch-off all count). The keep-alive re-landing (the push_sink_armed
    // branch) usually carries no reason and hangs nothing.
    {
        const char *last_why = push_sink_last_close_why();
        snprintf(s_last_cancel_why, sizeof(s_last_cancel_why), "%s", last_why ? last_why : "");
        if (s_last_cancel_why[0])
            snprintf(s_method_message, sizeof(s_method_message), "上次接收已结束：%s", s_last_cancel_why);
    }
    s_push_note[0] = 0;
    s_push_poll_ms = 0;
    s_push_handed_off = false; // gen18：进场即清交接标记，旧标记不过夜 / gen18: clear the handoff flag on entry - no stale flags overnight
    s_view = TRANSFER_METHODS;
    s_direct_entry = s_usb_requested || s_wifi_upload_requested || s_hotspot_requested;
    s_usb_entry = s_usb_requested;
    s_forget_confirm = false;
    s_scan_pending = false;
    s_network_connect_pending = false;
    clear_password();
    s_session_started = false;
    s_pressed = -1;
    s_mem_panel = false; // gen25：体检面板不跨回合存活 / gen25: the health panel never survives a round
    s_settle = s_any_changed = false;
    // gen24.4：进场先清零强制全刷标记，防上一回合的陈旧标记在本回合某次
    // 普通整页刷新上误放全刷；本回合真落武装页时由下面两个入口重新置位。
    // Gen24.4: clear the force-full flag on entry so a stale flag from the
    // last round cannot fire a spurious full refresh; this round's real arm
    // landings re-set it at the two entries below.
    s_push_entry_full_once = false;
    s_status.mode = s_mode;
    read_pico_transfer_get_saved_wifi(s_status.wifi_ssid, &s_status.wifi_configured);
    s_poll_ms = 0;
    s_free = 0;
    // gen24.2 块 8（gen23 裁定兑现）：deep-link 只跳转、不武装。首页卡片
    // 点击仅把用户带到方法页——四门检查（内存/蓝牙总开关/飞行/USB）只在
    // 传书页「手机推送」按钮显式触发。gen18~gen24.1 此处直接 push_arm_now()
    // ——绕过全部四门，是 gen24.2 问题 1「首次能收、二次死锁」的史前版本
    // （开关关着也照收）；gen23 已裁定只跳转，本版落地。
    // 与 else-if 链解耦（而非占链内一支）：armed 保活中（gen18 18-a：后台
    // 会话活着、手机在宽限期内）点卡片，仍由下面的 push_sink_armed 分支
    // 落武装页接客——同一语义，区别仅在不重新武装、不重置窗口。
    // gen24.2 block 8 (the gen23 ruling paid off): the deep link only
    // navigates, never arms. A home-card tap brings the user to the method
    // page; the four gates (memory / BLE master switch / flight / USB) run
    // only on the explicit "phone push" tap. gen18-gen24.1 called
    // push_arm_now() right here - bypassing every gate, the prehistoric form
    // of gen24.2 problem 1's "first tap works, second deadlocks" (it
    // received even with the switch off). gen23 ruled navigate-only; this
    // generation lands it. Decoupled from the else-if chain (not a chain
    // link): with a keep-alive arm live (gen18 18-a - the session lives in
    // the background, the phone inside the grace window) a card tap still
    // lands on the arm page via the push_sink_armed branch below - same
    // semantics, only without re-arming or resetting the window.
    if (s_push_requested) s_push_requested = false;
    if (s_usb_requested) {
        read_pico_sd_info_t usb_sd = {0};
        if (read_pico_sd_get_info(&usb_sd) == ESP_OK) s_usb_capacity_bytes = usb_sd.capacity_bytes;
        s_usb_requested = false;
        s_view = TRANSFER_HOME;
        s_usb_start_pending = true;
        snprintf(s_usb_message, sizeof(s_usb_message), "正在卸载本机 TF 卡并启动 USB…");
    } else if (s_hotspot_requested) {
        s_hotspot_requested = false;
        if (!start_hotspot_service()) {
            s_view = TRANSFER_HOME;
            s_status.state = READ_PICO_TRANSFER_ERROR;
            snprintf(s_method_message, sizeof(s_method_message), "传输正在结束，请稍后再试");
        }
    } else if (s_wifi_upload_requested) {
        s_wifi_upload_requested = false;
        // 配网页可能留下常驻的仅联网会话；传书前先释放它再启动上传服务。
        // Provisioning may leave a network-only session alive; release it before starting upload.
        if (s_status.wifi_configured && read_pico_transfer_try_stop_if_idle())
            queue_network_start(READ_PICO_TRANSFER_MODE_STA);
        else if (s_status.wifi_configured) {
            s_view = TRANSFER_HOME;
            s_status.state = READ_PICO_TRANSFER_ERROR;
            snprintf(s_method_message, sizeof(s_method_message), "传输正在结束，请稍后再试");
        }
        else if (!enter_networks()) {
            s_view = TRANSFER_NETWORKS;
            s_scan_pending = false;
            snprintf(s_network_message, sizeof(s_network_message), "网络正在结束，请稍后再试");
        }
    } else if (s_wifi_setup_requested) {
        s_wifi_setup_requested = false;
        read_pico_transfer_get_status(&s_status);
        if (s_status.network_ready && s_status.mode == READ_PICO_TRANSFER_MODE_STA) {
            s_session_started = true;
            s_view = TRANSFER_NETWORKS;
            s_saved_configured = s_status.wifi_configured;
            snprintf(s_saved_ssid, sizeof(s_saved_ssid), "%s", s_status.wifi_ssid);
            snprintf(s_network_message, sizeof(s_network_message), "连接成功");
        } else {
            (void)enter_networks();
        }
    } else if (push_sink_armed()) {
        // gen18（18-a 自动切回落点）：sink 正被后台保活——用户在读 push.txt，
        // 手机回连把用户从阅读器弹回本页。直接落武装视图接客，**不重新武装**
        // （会话是活的，ERR(1) 意味着保活失败，验收要抓的就是它）。
        // gen18 (18-a auto-switch-back landing): the sink is background-alive -
        // the user reads push.txt and a returning phone bounced them back here.
        // Land straight on the arm view to receive and **do not re-arm** (the
        // session is live; an ERR(1) would mean the keep-alive failed, which is
        // exactly what acceptance watches for).
        s_view = TRANSFER_PUSH;
        // gen24.4：跨页弹回同样强制一次整屏 GC16——阅读页的墨迹叠不到本页
        // 按钮上。/ Gen24.4: a cross-page bounce-back forces the same single
        // full GC16 - the reader's ink must not stack onto this page's buttons.
        s_push_entry_full_once = true;
        push_word_card_close();
        push_sync_live_bookkeeping();
    } else {
        s_wifi_settings_only = false;
    }
}

static void stop_session(void) {
    clear_qr();
    // 停服后读最终计数，保留本页跨模式文件变更的记录。
    // Join reception before reading counts and retain file changes across mode switches.
    read_pico_transfer_stop();
    read_pico_transfer_get_status(&s_status);
    if (s_session_started && s_status.changed_count) s_any_changed = true;
    s_session_started = false;
}

static bool stop_upload_if_idle(void) {
    if (!read_pico_transfer_try_stop_if_idle()) return false;
    stop_session();
    s_start_pending = false;
    return true;
}

// 必须先等待 HTTP 退出，再清除旧根；不会让在途请求写到另一存储。
// Join HTTP before clearing the old root; in-flight requests never move to another storage.
static void transfer_on_media_lost(app_ctx_t* ctx) {
    (void)ctx;
    if (s_root.is_flash) return;
    if (s_view == TRANSFER_PUSH) push_word_card_close();
    // gen18：媒体丢失照旧收摊（裁决口径）——卡没了，后续 BEGIN 只会在
    // part_open_append 撞 ERR(5)，不如诚实地结束并留痕。
    // gen18: media loss closes up as ruled - with the card gone every later
    // BEGIN just hits ERR(5) in part_open_append, so end it honestly with a trace.
    if (push_sink_armed() || push_sink_state() != PUSH_STATE_IDLE) {
        push_sink_cancel_why("TF 卡已拔出，接收结束");
    }
    stop_session();
    s_start_pending = s_scan_pending = false;
    s_view = s_direct_entry ? TRANSFER_HOME : TRANSFER_METHODS;
    clear_password();
    s_pressed = -1;
    s_free = 0;
    memset(&s_root, 0, sizeof(s_root));
    s_media_lost = true;
    s_settle = false;
}

static void transfer_on_exit(app_ctx_t* ctx) {
    (void)ctx;
    s_usb_start_pending = false;
    // gen19（真机 Bug3）：离页一律不收摊。保活是产品语义——回到首页状态卡
    // 继续显示「等待连接 · 剩 N 分钟」，会话由窗口到期自然收（≤30 分钟），
    // 或系统级入口收（飞行模式/USB/关机/媒体丢失/拔电）。真机实证：推单词卡
    // 后离页走了旧 cancel，状态卡直接「未开启」，与长文开书路径行为分裂。
    // gen19 (real-hw Bug3): leaving the page never tears the session down.
    // Keep-alive is the product semantics - the home status card keeps showing
    // "waiting - N min left"; the window expiry closes it naturally (<=30 min)
    // and system-level entries (flight mode / USB / shutdown / media lost /
    // unplug) close it explicitly. Real-hw proof: the word-card exit hit the
    // old cancel and the card flipped to "off", diverging from the long-text
    // handoff path.
    s_push_handed_off = false;
    if (s_view == TRANSFER_PUSH) push_word_card_close();
    if (usb_storage_active()) {
        esp_err_t err = usb_storage_stop();
        if (err == ESP_OK) s_any_changed = true;
        else ESP_LOGW(TAG, "USB remained active on page exit: %s", esp_err_to_name(err));
    }
    clear_password();
    s_scan_pending = s_network_connect_pending = false;
    read_pico_transfer_status_t latest = {0};
    read_pico_transfer_get_status(&latest);
    bool keep_sta = s_wifi_settings_only && latest.network_ready &&
                    latest.mode == READ_PICO_TRANSFER_MODE_STA;
    if (keep_sta) {
        clear_qr();
        s_session_started = false; // 全局循环接管保活；再次进页会从组件状态恢复。
    } else stop_session();
    s_wifi_settings_only = false;
    s_usb_entry = false;
    s_direct_entry = false;
    display_set_bulk_io(false);
    if (s_any_changed) book_store_notify_changed();
    ESP_LOGI(TAG, "leave keep_sta=%d heap before=%u after=%u internal_before=%u internal_after=%u",
             keep_sta,
             (unsigned)s_heap_before, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)s_internal_before, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

static app_redraw_t on_tick(app_ctx_t* ctx) {
    // gen25 让路链 phase 1 的收尾：prep 已停 → 卸书 → 重过预检 → 武装。
    // 分拍执行是接线裁定 #1 的另一半：这一拍只做一步，UI 在 phase 1 期间已有
    // 提示在屏上，不会出现「按下去没反应」。裁定 #3：任何失败都直接落到重启
    // 兜底提示，不原地重试。/ Gen25 yield chain, finishing phase 1: prep stopped
    // -> unload the book -> re-run the precheck -> arm. Running one step per tick
    // is the other half of wiring ruling #1: the hint is already on screen during
    // phase 1, so the tap never looks dead. Ruling #3: any failure falls straight
    // through to the restart fallback hint - never retried in place.
    if (s_push_yield_phase == 1) {
        if (!s_push_yield_deadline) s_push_yield_deadline = ctx->now_ms + PUSH_YIELD_WAIT_MS;
        if (app_book_prep_stopped()) {
            s_push_yield_phase = 0;
            if (app_book_request_yield_for_push()) {
                size_t free_internal, largest_internal, free_psram;
                ble_pt_mem_snapshot(&free_internal, &largest_internal, &free_psram);
                if (ble_pt_mem_gate(free_internal, largest_internal, free_psram)) {
                    ESP_LOGW(TAG, "yield ok: arming after unload (internal %u B free / %u B largest)",
                             (unsigned)free_internal, (unsigned)largest_internal);
                    push_arm_now();
                    return APP_REDRAW_PAGE;
                }
                ESP_LOGW(TAG, "yield done but gate still fails: internal %u B free / %u B largest",
                         (unsigned)free_internal, (unsigned)largest_internal);
            }
            // 让路失败或让路后仍不过闸 → 重启兜底（裁定 #3 真实文案）。
            // Yield failed or the gate still fails after it -> the restart
            // fallback (ruling #3's honest wording).
            snprintf(s_method_message, sizeof(s_method_message),
                     "内存未能腾出——重启设备后可直接推送");
            s_method_inline_enable = false;
            return APP_REDRAW_PAGE;
        }
        if (ctx->now_ms < s_push_yield_deadline) return APP_REDRAW_NONE; // 提示已在屏上，静默等下一拍 / hint is on screen; quietly wait for the next tick
        s_push_yield_phase = 0;
        snprintf(s_method_message, sizeof(s_method_message),
                 "内存未能腾出——重启设备后可直接推送");
        s_method_inline_enable = false;
        return APP_REDRAW_PAGE;
    }
    if (s_view == TRANSFER_PASSWORD) {
        if (ctx->consumed) return APP_REDRAW_NONE;
        bool held = !ctx->released && ctx->touch && ctx->touch->touched && ctx->touch->count == 1;
        if (ui_keyboard_hold_tick(held, ctx->touch ? ctx->touch->x : 0,
            ctx->touch ? ctx->touch->y : 0, ctx->now_ms) == UI_KEYBOARD_CHANGED) return password_paint(ctx, false, false);
        if (ui_keyboard_idle_tick(ctx->touch && ctx->touch->touched, ctx->now_ms) == UI_KEYBOARD_CHANGED)
            return password_paint(ctx, false, false);
    }
    uint32_t revision = __atomic_load_n(&s_signature_revision, __ATOMIC_ACQUIRE);
    if (revision != s_signature_seen) {
        s_signature_seen = revision;
        return APP_REDRAW_PAGE;
    }
    if (s_usb_entry) {
        if (!s_usb_start_pending) {
            bool connected = usb_storage_connected();
            if (connected == s_usb_host_connected) return APP_REDRAW_NONE;
            s_usb_host_connected = connected;
            return APP_REDRAW_PAGE;
        }
        if (s_usb_retry_ms && ctx->now_ms - s_usb_retry_ms < 250) return APP_REDRAW_NONE;
        s_usb_retry_ms = ctx->now_ms;
        esp_err_t err = usb_storage_start();
        if (err == ESP_ERR_NOT_FINISHED) return APP_REDRAW_NONE;
        s_usb_start_pending = false;
        s_usb_host_connected = usb_storage_connected();
        if (err != ESP_OK)
            snprintf(s_usb_message, sizeof(s_usb_message), "USB 模式启动失败：%s", esp_err_to_name(err));
        return APP_REDRAW_PAGE;
    }
    if (usb_storage_active()) return APP_REDRAW_NONE;
    if (s_view == TRANSFER_PUSH) return push_tick(ctx);
    if (s_view == TRANSFER_METHODS) return methods_tick(ctx);
    if (s_view != TRANSFER_HOME) return network_ui_tick(ctx);
    if (s_start_pending) {
        s_start_pending = false;
        // 传书模式也应在启动无线电前释放可重建字形，避免内部内存不足。
        // Release rebuildable glyphs before starting the radio for uploads too.
        ttf_font_cache_clear();
        esp_err_t err = book_store_upload_root(&s_root);
        if (err == ESP_OK) {
            s_media_lost = false;
            s_free = book_store_free_bytes(&s_root);
            read_pico_transfer_cfg_t cfg = {.mode = s_mode, .root_dir = s_root.path, .is_flash = s_root.is_flash,
                .file_limit = book_store_file_limit(&s_root), .free_bytes_cb = free_bytes, .free_bytes_ctx = &s_root,
                .file_changed_cb = book_progress_forget, .file_deleted_cb = transfer_file_deleted,
                .file_moved_cb = transfer_file_moved,
                .directory_deleted_cb = transfer_directory_deleted,
                .directory_moved_cb = transfer_directory_moved,
                .wallpaper_set_cb = transfer_set_wallpaper,
                .signature_get_cb = transfer_signature_get, .signature_set_cb = transfer_signature_set,
                .title_get_cb = NULL, .title_set_cb = NULL};
            err = read_pico_transfer_start(&cfg);
            s_session_started = err == ESP_OK;
            read_pico_transfer_get_status(&s_status);
            if (err == ESP_OK) s_free = book_store_free_bytes(&s_root);
        }
        s_status.mode = s_mode;
        if (err != ESP_OK) { s_status.state = READ_PICO_TRANSFER_ERROR; s_status.last_error = err; }
        if (err == ESP_OK) prepare_qr();
        else clear_qr();
        ESP_LOGI(TAG, "start root=%s result=%s", s_root.path, esp_err_to_name(err));
        return APP_REDRAW_PAGE;
    }
    if (ctx->consumed || ctx->now_ms - s_poll_ms < 2000) return APP_REDRAW_NONE;
    s_poll_ms = ctx->now_ms;
    read_pico_transfer_service_poll();
    read_pico_transfer_status_t next = {0};
    read_pico_transfer_get_status(&next);
    // 接收期间让屏幕保持静止，网页端显示实时进度；避免刷屏与写卡争用。
    // Keep the panel still during reception; the browser owns live progress.
    if (next.state == READ_PICO_TRANSFER_UPLOADING && s_status.state == READ_PICO_TRANSFER_UPLOADING) {
        s_status.cur_bytes = next.cur_bytes;
        s_status.sta_count = next.sta_count;
        return APP_REDRAW_NONE;
    }
    if (s_status.state == READ_PICO_TRANSFER_ERROR && next.state == READ_PICO_TRANSFER_STOPPED) return APP_REDRAW_NONE;
    bool network_changed = next.network_ready != s_status.network_ready ||
        next.mode != s_status.mode || strcmp(next.ssid, s_status.ssid) ||
        next.wifi_configured != s_status.wifi_configured || strcmp(next.url, s_status.url) ||
        strcmp(next.wifi_ssid, s_status.wifi_ssid);
    if (!network_changed && next.state == s_status.state && next.sta_count == s_status.sta_count &&
        next.cur_bytes == s_status.cur_bytes && next.cur_total == s_status.cur_total &&
        next.done_count == s_status.done_count && next.changed_count == s_status.changed_count && next.last_error == s_status.last_error &&
        !strcmp(next.cur_name, s_status.cur_name)) return APP_REDRAW_NONE;
    s_settle = next.changed_count != s_status.changed_count || next.done_count != s_status.done_count ||
        (s_status.state == READ_PICO_TRANSFER_UPLOADING && next.state != READ_PICO_TRANSFER_UPLOADING);
    s_status = next;
    if (network_changed) prepare_qr();
    if (s_settle) {
        s_free = book_store_free_bytes(&s_root);
        draw_status(ctx->fb);
        s_area = status_rect();
        return APP_REDRAW_AREA;
    }
    if (network_changed) { s_free = book_store_free_bytes(&s_root); return APP_REDRAW_PAGE; }
    draw_status(ctx->fb);
    s_area = status_rect();
    return APP_REDRAW_AREA;
}

static int hit_control(uint16_t x, uint16_t y) {
    if (usb_storage_active()) return ui_rect_hit(control_rect(2), x, y) ? 2 : -1;
    if (s_mode == READ_PICO_TRANSFER_MODE_AP && s_view == TRANSFER_HOME)
        for (int i = 0; i < 2; ++i) if (ui_rect_hit(control_rect(i), x, y)) return i;
    return ui_rect_hit(control_rect(2), x, y) ? 2 : -1;
}

static int hit_method(uint16_t x, uint16_t y) {
    for (int i = 0; i < 3; ++i) if (ui_rect_hit(method_control_rect(i), x, y)) return i;
    return -1;
}

static app_redraw_t stop_usb_for_navigation(app_ctx_t *ctx, bool home) {
    s_usb_start_pending = false;
    if (usb_storage_active()) {
        esp_err_t err = usb_storage_stop();
        if (err != ESP_OK) {
            snprintf(s_usb_message, sizeof(s_usb_message), "USB 写入未完成：%s，请在电脑弹出后重试", esp_err_to_name(err));
            return APP_REDRAW_PAGE;
        }
        s_any_changed = true;
    }
    if (home) ui_nav_request(ctx, 0);
    else if (s_direct_entry) ctx->request_return = true;
    else s_view = TRANSFER_METHODS;
    return ctx->request_return || home ? APP_REDRAW_NONE : APP_REDRAW_PAGE;
}

static app_redraw_t method_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    // gen25 内存体检入口：方法页长按整页打开面板（见面板节的手势边界注释）。
    // Gen25 memory-health entry: a long press anywhere on the method page opens
    // the panel whole-page (see the gesture-boundary note in the panel section).
    if (ev->type == UI_GESTURE_LONG_PRESS) {
        s_mem_panel = true;
        s_pressed = -1;
        return APP_REDRAW_PAGE;
    }
    int old = s_pressed;
    // gen24.2 块 3：行内【开启】按钮——门 2 提示条挂着时才出现，命中即原地
    // 写设置项 + 重走四门，不跳设置页（§二-4 边界：其他门无此控件）。
    // Block 3's inline [enable] button: visible only while the gate-2 hint is
    // up; a hit writes the setting in place and re-runs the four gates with
    // no trip to the settings page (section 2-4's boundary: no other gate
    // carries this control).
    if (s_method_inline_enable && ev->type == UI_GESTURE_TAP &&
        ui_rect_hit(inline_enable_rect(), ev->x, ev->y)) {
        inline_enable_push();
        return APP_REDRAW_PAGE;
    }
    int origin = hit_method(ev->x0, ev->y0);
    s_pressed = ev->type == UI_GESTURE_PRESS ? origin : -1;
    if (ev->type == UI_GESTURE_TAP && old >= 0 && old == origin && hit_method(ev->x, ev->y) == origin) {
        if (origin == 0) {
            if (s_status.wifi_configured) {
                if (stop_upload_if_idle()) {
                    queue_network_start(READ_PICO_TRANSFER_MODE_STA);
                    s_method_message[0] = 0;
                    return APP_REDRAW_PAGE;
                }
            } else if (enter_networks()) return APP_REDRAW_PAGE;
            snprintf(s_method_message, sizeof(s_method_message), "传输正在结束，请稍后再试");
        } else if (origin == 1) {
            if (start_hotspot_service()) return APP_REDRAW_PAGE;
            snprintf(s_method_message, sizeof(s_method_message), "传输正在结束，请稍后再试");
        } else if (origin == 2) {
            // 手机推送：gen24 多态按钮统一入口（状态驱动：武装/取消/自愈/禁用）。
            // gen19（真机 Bug2）起改为当场动作——gen18 只置 flag 等 on_enter 消费，
            // 用户已在页内时 on_enter 不会重跑，flag 挂死按钮无反应。
            // Phone push: gen24's one state-driven entry (arm / cancel / heal /
            // disable). gen19 (real-hw Bug2) made it act right now - gen18's
            // flag-for-on_enter hung forever because on_enter never re-runs while
            // the page is open.
            s_push_requested = false;
            push_button_action();
        }
        return APP_REDRAW_PAGE;
    }
    if (old == s_pressed) return APP_REDRAW_NONE;
    // 按下反馈只刷新单行；整页刷新会取消尚未松手的手势，导致入口无法点击。
    // Refresh only the pressed row; a page redraw cancels the active gesture before its tap.
    render(ctx, ctx->fb);
    s_area = method_control_rect(old >= 0 ? old : s_pressed);
    s_settle = false;
    return APP_REDRAW_AREA;
}

static app_redraw_t on_gesture(app_ctx_t* ctx, const ui_gesture_event_t* ev) {
    if (s_status.state == READ_PICO_TRANSFER_UPLOADING) return APP_REDRAW_NONE;
    // gen25 内存体检面板：模态拦截（mem_panel_gesture）。面板只属于方法页——
    // on_tick 的让路收官（push_arm_now 切武装页）或收摊事件可能在面板开着时
    // 改变视图，届时面板静默让位，武装页/其它视图的手势照常。
    // Gen25 memory-health panel: the modal intercept (mem_panel_gesture). The
    // panel belongs to the method page alone - on_tick's yield finale
    // (push_arm_now switches to the arm view) or close-up events can change the
    // view while it is open, so the panel steps aside silently and the arm
    // page's (or any other view's) gestures carry on.
    if (s_mem_panel && s_view != TRANSFER_METHODS) s_mem_panel = false;
    if (s_mem_panel) return mem_panel_gesture(ctx, ev);
    if (ev->type == UI_GESTURE_TAP && s_view != TRANSFER_PASSWORD && !usb_storage_active()) {
        int tab = ui_nav_hit(ev->x0, ev->y0);
        if (tab >= 0 && tab == ui_nav_hit(ev->x, ev->y)) {
            if (s_view == TRANSFER_HOME && !stop_upload_if_idle()) return APP_REDRAW_NONE;
            // 武装页先收摊再切走：BLE 栈该跟着翻页器设置走，不该为看不见的
            // 推流续着。/ Leave the arm before navigating away: the BLE stack
            // follows the page-turner setting, not a push no longer on screen.
            // gen19（Bug3）：切页不再收摊，保活语义统一（见 transfer_on_exit）。
            // gen19 (Bug3): navigation no longer cancels; keep-alive semantics
            // are unified (see transfer_on_exit).
            if (s_view == TRANSFER_PUSH) push_word_card_close();
            s_pressed = -1;
            ui_nav_request(ctx, tab);
            return APP_REDRAW_NONE;
        }
    }
    if (ev->type == UI_GESTURE_TAP && ev->y0 < 160 && ev->x0 < 120) {
        if (s_usb_entry || usb_storage_active()) return stop_usb_for_navigation(ctx, false);
        if (s_view == TRANSFER_PASSWORD) { clear_password(); s_view = TRANSFER_NETWORKS; return APP_REDRAW_PAGE; }
        // gen19（Bug3）：接收页返回不再收摊（保活语义统一）；词卡收掉即可。
        // gen19 (Bug3): back from the push view no longer cancels (unified
        // keep-alive semantics); just drop the word card.
        if (s_view == TRANSFER_PUSH) { push_word_card_close(); s_view = TRANSFER_METHODS; return APP_REDRAW_PAGE; }
        if (s_view == TRANSFER_NETWORKS) {
            if (s_wifi_settings_only) { ctx->request_return = true; return APP_REDRAW_NONE; }
            if (s_direct_entry) { ctx->request_return = true; return APP_REDRAW_NONE; }
            s_view = TRANSFER_METHODS; return APP_REDRAW_PAGE;
        }
        if (s_view == TRANSFER_HOME) {
            if (!stop_upload_if_idle()) return APP_REDRAW_NONE;
            if (s_direct_entry) { ctx->request_return = true; return APP_REDRAW_NONE; }
            s_view = TRANSFER_METHODS;
            return APP_REDRAW_PAGE;
        }
        ctx->request_return = true;
        return APP_REDRAW_NONE;
    }
    if (s_view == TRANSFER_METHODS) return method_gesture(ctx, ev);
    if (s_view == TRANSFER_PUSH) return push_gesture(ctx, ev);
    if (s_view != TRANSFER_HOME) return provisioning_gesture(ctx, ev);
    int old = s_pressed;
    int origin = hit_control(ev->x0, ev->y0);
    s_pressed = ev->type == UI_GESTURE_PRESS ? origin : -1;
    if (ev->type == UI_GESTURE_TAP && old >= 0 && old == origin &&
        hit_control(ev->x, ev->y) == origin) {
        if (s_usb_entry) {
            return stop_usb_for_navigation(ctx, false);
        }
        if (origin == 0 || origin == 1) {
            bool url = origin == 1;
            if (s_qr_url != url) {
                s_qr_url = url;
                prepare_qr();
                return APP_REDRAW_PAGE;
            }
            return APP_REDRAW_NONE;
        }
        if (origin == 2) {
            if (s_mode == READ_PICO_TRANSFER_MODE_AP && s_status.state == READ_PICO_TRANSFER_ERROR &&
                !s_status.network_ready && start_hotspot_service()) return APP_REDRAW_PAGE;
            // 即使没有切页历史也先停服，取消尚未执行的启动请求。
            // Stop even without navigation history and cancel a queued start.
            if (!stop_upload_if_idle()) return APP_REDRAW_NONE;
            if (s_direct_entry) { ctx->request_return = true; return APP_REDRAW_NONE; }
            s_view = TRANSFER_METHODS;
            return APP_REDRAW_PAGE;
        }
    }
    if (s_pressed == old) return APP_REDRAW_NONE;
    render(ctx, ctx->fb);
    s_settle = false;
    s_area = control_rect(old >= 0 ? old : s_pressed);
    return APP_REDRAW_AREA;
}

static app_redraw_t on_key(app_ctx_t *ctx, int key) {
    // gen25：面板开着时按键只负责关面板，不叠页面动作（与手势的模态语义一致）。
    // Gen25: with the panel open a key only closes it - no page action rides
    // along (the same modal semantics as gestures).
    if (s_mem_panel) {
        s_mem_panel = false;
        return APP_REDRAW_PAGE;
    }
    if (s_usb_entry || usb_storage_active()) return stop_usb_for_navigation(ctx, key == UI_KEY_2);
    if (s_view == TRANSFER_HOME && !stop_upload_if_idle()) return APP_REDRAW_NONE;
    // 武装页：任何离页按键都先收摊，再按既有规则走。/ The arm page: any leaving
    // key closes the arm first, then follows the existing rules.
    if (s_view == TRANSFER_PUSH) {
        push_word_card_close();
        push_sink_cancel();
        if (key == UI_KEY_2) { ui_nav_request(ctx, 0); return APP_REDRAW_NONE; }
        s_view = TRANSFER_METHODS;
        return APP_REDRAW_PAGE;
    }
    if (key == UI_KEY_2) { ui_nav_request(ctx, 0); return APP_REDRAW_NONE; }
    if (s_view == TRANSFER_PASSWORD) { clear_password(); s_view = TRANSFER_NETWORKS; return APP_REDRAW_PAGE; }
    if (s_view == TRANSFER_NETWORKS) {
        if (s_wifi_settings_only || s_direct_entry) { ctx->request_return = true; return APP_REDRAW_NONE; }
        s_view = TRANSFER_METHODS; return APP_REDRAW_PAGE;
    }
    if (s_view == TRANSFER_HOME) {
        if (s_direct_entry) { ctx->request_return = true; return APP_REDRAW_NONE; }
        s_view = TRANSFER_METHODS; return APP_REDRAW_PAGE;
    }
    ctx->request_return = true;
    return APP_REDRAW_NONE;
}

static bool present(app_ctx_t* ctx, app_redraw_t redraw) {
    // gen24.4（渲染发灰根治）：进入武装页的第一次呈现强制整屏 GC16。此前
    // 非 AREA 刷新直接落回 app_loop 的 fast_page（GL16 差分快刷），跨页带入
    // 的旧墨——首页月相钟轮廓——留着叠在按钮上，文字发糊、底色发灰。清晰度
    // 第一：落地只用全刷擦旧底，绝不用低质量局部快刷；倒计时等活区更新仍走
    // 既定 AREA 通道（墨没动就不刷，动也只重绘活区）。
    // Gen24.4 (the gray-render root fix): the arm view's first presentation
    // forces a screen-wide GC16. Non-AREA refreshes used to fall straight back
    // to app_loop's fast_page (a GL16 differential), so ink carried over from
    // the previous page - the home clock outline - stacked onto the buttons,
    // blurring text and graying the base. Clarity first: the landing wipes the
    // old base with a full refresh only, never a low-quality partial one; the
    // countdown's live updates keep using the established AREA path (nothing
    // moved, nothing painted; a move repaints the live region only).
    if (redraw != APP_REDRAW_AREA) {
        if (!s_push_entry_full_once) return false;
        s_push_entry_full_once = false;
        render(ctx, ctx->fb);
        guard_draw_result(ctx->hl, update_display_full(ctx->hl));
        return true;
    }
    if (s_view == TRANSFER_PASSWORD) {
        if (s_input_settle) {
            guard_draw_result(ctx->hl, update_display_area_full_with(ctx->hl, &E0470_WAVEFORM, MODE_GL16, s_area));
            s_input_settle = false;
            return true;
        }
        guard_draw_result(ctx->hl, update_display_area_diff_with(ctx->hl,
            s_input_layout ? &E0470_WAVEFORM : &E0470_FOLLOW_WAVEFORM,
            s_input_layout ? MODE_GL16 : MODE_DU, s_area));
        return true;
    }
    guard_draw_result(ctx->hl, update_display_area_with(ctx->hl, s_settle ? &E0470_WAVEFORM : &E0470_FOLLOW_WAVEFORM,
                      s_settle ? MODE_GL16 : MODE_DU, s_area));
    s_settle = false;
    return true;
}

static bool transfer_menu_handle_enabled(app_ctx_t* ctx) {
    (void)ctx;
    return false;
}

const app_desc_t app_transfer = {
    .on_media_lost = transfer_on_media_lost,
    .title = "传书 Transfer", .detail = "热点或已有 WiFi，浏览器上传", .enter_full = false,
    .on_enter = on_enter, .on_exit = transfer_on_exit, .render = render, .present = present,
    .on_tick = on_tick, .on_gesture = on_gesture, .on_key = on_key, .owns_keys = true,
    .menu_handle_enabled = transfer_menu_handle_enabled,
};
