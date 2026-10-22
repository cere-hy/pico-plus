/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：直接验证触屏配网状态机及输入边界。
 * English: Exercise the device provisioning state machine and input bounds.
 * 冻结：仅供宿主测试，不访问网络。/ Frozen: Host tests only; no network access.
 */
#include "transfer_ui_test_env.h"
#include "../main/apps/app_transfer.c"
// gen24：预检闸门的真身直接进测试（单一真源，边界逐项覆盖）。它零 IDF 依赖。
// Gen24: the precheck gate's real body comes straight into the test (single
// source of truth, bounds covered one by one). It carries no IDF dependency.
#include "../components/ble_page_turner/src/ble_pt_mem.c"
#include <assert.h>
#include "../components/read_pico_transfer/transfer_signature.h"
/* 中文：武装页 Step 4 依赖的宿主替身。app_content_open.h 与 dict_popup.h 的真身
   就在 main/apps/，quote Include 永远先中真身，替身目录遮蔽不了它们；这里按
   usb_storage_* 的同形制在测试文件里出定义与旋钮（dict_lookup.h 在 main/dict/，
   仍由 tools/transfer_ui_stubs/dict_lookup.h 遮蔽）。
   English: the host doubles the arm page's Step 4 needs. The real
   app_content_open.h and dict_popup.h live in main/apps/, so a quote include
   always hits the real ones and the stub directory cannot shadow them; the
   doubles and their knobs therefore live here, same shape as usb_storage_*
   (dict_lookup.h lives in main/dict/ and is still shadowed by the stub). */
const app_desc_t app_book = {0};
static char test_book_open_path[160];
static bool test_book_open_ok = true;
// gen24.4：记录最后一次开书是否走 _clean 变体（推送收官必须走，第一页才有
// 整屏 GC16 擦旧底）。/ Gen24.4: remembers whether the last open took the
// _clean variant (the push finale must, so its first page gets the full GC16).
static bool test_book_open_clean;
bool app_book_request_open(const char *path) {
    if (!path || !test_book_open_ok) return false;
    test_book_open_clean = false;
    snprintf(test_book_open_path, sizeof(test_book_open_path), "%s", path);
    return true;
}
// gen24.4：带一次整屏 GC16 的开书（app_book.c 真身语义的宿主替身）。
// Gen24.4: the host double of app_book.c's real _clean open (one full GC16).
bool app_book_request_open_clean(const char *path) {
    if (!app_book_request_open(path)) return false;
    test_book_open_clean = true;
    return true;
}
static char test_dict_word[64];
static bool test_dict_open_ok = true;
void dict_popup_set_anchor(int col, bool split) { (void)col; (void)split; }
bool dict_popup_open(const char *word) {
    snprintf(test_dict_word, sizeof(test_dict_word), "%s", word ? word : "");
    return test_dict_open_ok;
}
void dict_popup_reset(void) { test_dict_word[0] = 0; }
void dict_popup_draw(uint8_t *fb) { (void)fb; }
dict_popup_tap_t dict_popup_tap(uint16_t x, uint16_t y) { (void)x; (void)y; return DICT_POPUP_TAP_MISSED; }
static bool test_usb_active;
static esp_err_t test_usb_stop_error;
esp_err_t usb_storage_start(void) { test_usb_active = true; return ESP_OK; }
esp_err_t usb_storage_stop(void) {
    if (test_usb_stop_error != ESP_OK) return test_usb_stop_error;
    test_usb_active = false;
    return ESP_OK;
}
bool usb_storage_active(void) { return test_usb_active; }
bool usb_storage_connected(void) { return test_usb_active; }
static void tap(app_ctx_t* ctx, EpdRect rect) {
    ui_gesture_event_t ev = {.type = UI_GESTURE_PRESS, .x0 = rect.x + 2, .y0 = rect.y + 2, .x = rect.x + 2, .y = rect.y + 2};
    app_redraw_t pressed = on_gesture(ctx, &ev);
    // 整页刷新在 app_loop 中会取消活动手势；入口反馈必须保留松手事件。
    // app_loop cancels active gestures on a page refresh; feedback must preserve release.
    assert(pressed != APP_REDRAW_PAGE && pressed != APP_REDRAW_FULL);
    ev.type = UI_GESTURE_TAP;
    on_gesture(ctx, &ev);
}
static void qr_regression(app_ctx_t* ctx) {
    test_status = (read_pico_transfer_status_t){.mode=READ_PICO_TRANSFER_MODE_AP,.state=READ_PICO_TRANSFER_READY,.network_ready=true};
    strcpy(test_status.ssid,"ReadPico-test");
    strcpy(test_status.url,"http://192.168.4.1");
    s_mode = READ_PICO_TRANSFER_MODE_AP;
    on_enter(ctx);
    s_view = TRANSFER_HOME;
    s_qr_url = true;
    ctx->now_ms += 3000;
    on_tick(ctx);
    assert(s_qr_ready && s_qr_url && !strcmp(test_qr_payload,test_status.url));
    tap(ctx, control_rect(0));
    assert(!s_qr_url && !strcmp(test_qr_payload,test_status.ssid));
    tap(ctx, control_rect(1));
    assert(s_qr_url && !strcmp(test_qr_payload,test_status.url));
    int encodes = test_qr_encodes;
    render(ctx,ctx->fb);
    assert(test_qr_encodes == encodes);
    test_status.cur_bytes++;
    ctx->now_ms += 3000;
    on_tick(ctx);
    assert(test_qr_encodes == encodes);
    strcpy(test_status.url,"http://192.168.4.2");
    ctx->now_ms += 3000;
    on_tick(ctx);
    assert(!strcmp(test_qr_payload,test_status.url));
    test_status.network_ready=false;
    ctx->now_ms += 3000;
    on_tick(ctx);
    assert(!s_qr_ready && !test_qr_payload[0]);
    encodes=test_qr_encodes;
    render(ctx,ctx->fb);
    assert(test_qr_encodes==encodes);
    test_status.network_ready=true;
    test_qr_failure=true;
    ctx->now_ms += 3000;
    on_tick(ctx);
    assert(!s_qr_ready);
    test_qr_failure=false;
    queue_network_start(READ_PICO_TRANSFER_MODE_STA);
    assert(!s_qr_ready && !test_qr_payload[0]);
    test_status.mode=READ_PICO_TRANSFER_MODE_STA;
    strcpy(test_status.url,"http://10.20.30.40");
    on_tick(ctx);
    assert(s_qr_url && s_qr_ready && !strcmp(test_qr_payload,test_status.url));
    encodes=test_qr_encodes;
    render(ctx,ctx->fb);
    assert(test_qr_encodes==encodes);
    stop_session();
    assert(!s_qr_ready && !test_qr_payload[0]);
    test_status=(read_pico_transfer_status_t){0};
    s_mode=READ_PICO_TRANSFER_MODE_AP;
    test_qr_encodes=0;
    ctx->now_ms=0;
}
// 页面标记可能因旧会话重连失败而丢失，确认遗忘仍须释放实际射频并取消排队连接。
// A failed restart can lose the page's session flag; forgetting must still stop the radio and queued reconnect.
static void wifi_recovery_regression(app_ctx_t* ctx) {
    test_configured = s_saved_configured = true;
    s_view = TRANSFER_NETWORKS;
    s_wifi_settings_only = true;
    s_session_started = false;
    s_start_pending = s_network_connect_pending = s_scan_pending = false;
    test_status = s_status = (read_pico_transfer_status_t){.mode=READ_PICO_TRANSFER_MODE_STA,
        .state=READ_PICO_TRANSFER_ERROR,.wifi_configured=true};
    test_wifi_running = true;
    int stops = test_stop_count;
    tap(ctx, network_control_rect(6));
    assert(!test_wifi_running && test_stop_count > stops && s_network_connect_pending);
    network_ui_tick(ctx);
    assert(s_session_started && !s_network_connect_pending);
    // 上传中不能遗忘；停止后可以重试。/ Forgetting waits for an active upload, then remains retryable.
    test_wifi_running = true;
    s_session_started = false;
    s_network_connect_pending = true;
    tap(ctx, network_control_rect(11));
    test_busy = true;
    stops = test_stop_count;
    int forgets = test_forget_count;
    tap(ctx, network_control_rect(11));
    assert(s_forget_confirm && test_wifi_running && test_stop_count == stops && test_forget_count == forgets);
    test_busy = false;
    tap(ctx, network_control_rect(11));
    assert(!s_forget_confirm && !test_wifi_running && !test_configured && !s_saved_configured);
    assert(!s_start_pending && !s_network_connect_pending && !s_scan_pending);
    network_ui_tick(ctx);
    assert(!s_session_started && !s_network_connect_pending);
    // 遗忘后重新选择同名网络会进入密码输入。/ Selecting the forgotten network asks for a password again.
    s_network_page = 0;
    tap(ctx, network_control_rect(0));
    assert(s_view == TRANSFER_PASSWORD && !s_password[0]);
    clear_password();
    s_wifi_settings_only = false;
    s_forget_confirm = false;
    test_status = (read_pico_transfer_status_t){0};
}
static void password_caret_regression(app_ctx_t *ctx) {
    clear_password();ui_keyboard_begin(&s_password_input,true);
    assert(s_password_input.cursor==0);
    tap(ctx,(EpdRect){24,1010,30,30}); // 数字页。/ Number panel.
    for(int i=0;i<8;i++)tap(ctx,(EpdRect){24+i*64,740,30,30});
    assert(!strcmp(s_password,"12345678")&&s_password_input.cursor==8);
    EpdRect field=password_control_rect(47);
    tap(ctx,(EpdRect){field.x+17+3*20-2,329,4,4});assert(s_password_input.cursor==3);
    tap(ctx,(EpdRect){24+64,740,30,30});assert(!strcmp(s_password,"123245678")&&s_password_input.cursor==4);
    tap(ctx,password_control_rect(45));assert(s_password_visible&&s_password_input.cursor==4);
    tap(ctx,password_control_rect(45));assert(!s_password_visible&&s_password_input.cursor==4);
    ui_text_edit_place(&s_password_input,3);
    tap(ctx,(EpdRect){600,896,30,30});assert(!strcmp(s_password,"12245678")&&s_password_input.cursor==2);
    ui_text_edit_place(&s_password_input,0);tap(ctx,(EpdRect){600,896,30,30});assert(!strcmp(s_password,"12245678"));
    clear_password();assert(!s_password[0]&&!s_password_input.cursor&&!s_password_visible);
    ui_keyboard_begin(&s_password_input,true);
    strcpy(s_password,"abcdefgh");ui_text_edit_init(&s_password_input,s_password,sizeof(s_password));
    cst836u_touch_t touch={.touched=true,.count=1,.x=610,.y=930};ctx->touch=&touch;ctx->now_ms=1000;
    ui_gesture_event_t held={.type=UI_GESTURE_PRESS,.x0=610,.y0=930,.x=610,.y=930};
    assert(on_gesture(ctx,&held)==APP_REDRAW_AREA && !s_input_layout);
    ctx->consumed=true;assert(on_tick(ctx)==APP_REDRAW_NONE);ctx->consumed=false;
    ctx->now_ms=1499;assert(on_tick(ctx)==APP_REDRAW_NONE&&!strcmp(s_password,"abcdefgh"));
    ctx->now_ms=1500;assert(on_tick(ctx)==APP_REDRAW_AREA&&!strcmp(s_password,"abcdefg"));assert(!s_input_layout);
    ctx->now_ms=1620;assert(on_tick(ctx)==APP_REDRAW_AREA&&!strcmp(s_password,"abcdef"));
    touch.count=2;ctx->now_ms=1740;assert(on_tick(ctx)==APP_REDRAW_NONE&&!strcmp(s_password,"abcdef"));
    touch.count=1;ctx->now_ms=2000;assert(on_tick(ctx)==APP_REDRAW_NONE&&!strcmp(s_password,"abcdef"));
    held.type=UI_GESTURE_PRESS;on_gesture(ctx,&held);ctx->now_ms=2500;assert(on_tick(ctx)==APP_REDRAW_AREA&&!strcmp(s_password,"abcde"));
    held.type=UI_GESTURE_CANCEL;touch.touched=false;on_gesture(ctx,&held);ctx->now_ms=2800;
    assert(on_tick(ctx)==APP_REDRAW_NONE&&!strcmp(s_password,"abcde"));ctx->touch=NULL;ctx->now_ms=0;
    clear_password();ui_keyboard_begin(&s_password_input,true);
}
// 手机推送武装页回归（Step 4）：开虚拟书、单词卡模态、活区刷新与陈事件清扫。
// The phone-push arm page regression (Step 4): the virtual book, the modal word
// card, live-region refresh and the stale-event sweep.
static void push_arm_regression(app_ctx_t *ctx) {
    // 进武装页：默认 30 分钟窗口；on_enter 把活区簿记对齐当前状态，无变化不刷。
    // Entering the arm page: the default 30-minute window; on_enter aligns the
    // live-region bookkeeping with the current state, so no change means no redraw.
    test_push_state = PUSH_STATE_WINDOW;
    snprintf(test_push_adv, sizeof(test_push_adv), "PicoPush-1a2b");
    test_push_left_ms = 30u * 60u * 1000u;
    // gen24.2 块 8：deep-link 只跳转不武装（gen23 裁定兑现）——on_enter 落
    // 方法页，武装由用户显式点「手机推送」行走四门。gen18~gen24.1 此处
    // 自动 push_arm_now() 绕过四门，是问题 1 死锁的史前版本。
    // Block 8: the deep link navigates only (gen23's ruling paid off) -
    // on_enter lands on the method page and the arm waits for the explicit
    // "phone push" tap, which runs the four gates. gen18-gen24.1 auto-armed
    // here, bypassing every gate: problem 1's prehistoric deadlock.
    app_transfer_request_push();
    on_enter(ctx);
    assert(s_view == TRANSFER_METHODS && !test_push_armed);
    // 显式点按钮走四门：先回 IDLE（本节现场是 WINDOW，非 IDLE 时点按是取消）。
    // The explicit tap runs the four gates: back to IDLE first (this case's
    // field is WINDOW, where a tap cancels instead).
    test_push_state = PUSH_STATE_IDLE;
    tap(ctx, method_control_rect(2));
    assert(s_view == TRANSFER_PUSH && test_push_armed);
    assert(test_push_arm_calls == 1 && test_push_arm_window == 30 * 60 * 1000);
    ctx->now_ms = 1000;
    assert(on_tick(ctx) == APP_REDRAW_NONE);
    // 倒计时走动 → 只重绘活区（DU 区域更新）；同值不重复刷。
    // The countdown moves - repaint only the live regions (a DU area update);
    // identical values never repaint twice.
    test_push_left_ms -= 500;
    ctx->now_ms += 500;
    assert(on_tick(ctx) == APP_REDRAW_AREA);
    assert(s_area.x == 36 && s_area.y == 168 && s_area.width == 612 && s_area.height == 496);
    ctx->now_ms += 500;
    assert(on_tick(ctx) == APP_REDRAW_NONE);
    // 推送成功：开虚拟书——标题覆盖成推送标题、清旧进度、切阅读页，且不抢全刷。
    // A push succeeds: open the virtual book - override the display title with
    // the pushed one, drop stale progress, switch to the reader, and do not
    // steal the single full refresh.
    // gen24.4：全刷改走 _clean 变体真正落在阅读页第一页（GL16 差分留残影，
    // 真机 gen24.3 验收实证）。/ Gen24.4: the full refresh now takes the _clean
    // variant so it truly lands on the reader's first page (a GL16 differential
    // kept the ghosting - gen24.3 hardware evidence).
    test_push_open = true;
    snprintf(test_push_title, sizeof(test_push_title), "被推送的书");
    test_push_type = 0;
    test_book_open_path[0] = 0;
    test_book_open_clean = false;
    test_book_title_path[0] = test_book_title[0] = test_progress_forget_path[0] = 0;
    ctx->request_app = NULL;
    assert(on_tick(ctx) == APP_REDRAW_NONE);
    assert(ctx->request_app == &app_book);
    assert(!strcmp(test_book_open_path, "/sdcard/.readpico/push.txt"));
    assert(test_book_open_clean);
    assert(!strcmp(test_book_title_path, "/sdcard/.readpico/push.txt"));
    assert(!strcmp(test_book_title, "被推送的书"));
    assert(!strcmp(test_progress_forget_path, "/sdcard/.readpico/push.txt"));
    // 开书请求失败：留页给准话，不黑屏不跳走。/ The open request fails: stay on
    // the page with a plain sentence, no black screen and no jump.
    test_book_open_ok = false;
    ctx->request_app = NULL;
    test_push_open = true;
    assert(on_tick(ctx) == APP_REDRAW_PAGE);
    assert(s_view == TRANSFER_PUSH && ctx->request_app == NULL);
    assert(!strcmp(s_push_note, "打开推送内容失败，请重试"));
    test_book_open_ok = true;
    // 单词卡命中：查词、开卡；卡片是模态的——按下不刷屏，点击问 chip（宿主恒
    // 未命中）后关卡片。/ A word-card hit: look it up and open the card; the
    // card is modal - a press never repaints, a tap asks the chip (always a
    // miss on the host) and then closes the card.
    s_push_note[0] = 0;
    test_push_word_card = true;
    snprintf(test_push_word, sizeof(test_push_word), "serendipity");
    test_dict_open_ok = true;
    assert(on_tick(ctx) == APP_REDRAW_PAGE);
    assert(s_push_word_card && !strcmp(test_dict_word, "serendipity"));
    ctx->now_ms += 500;
    assert(on_tick(ctx) == APP_REDRAW_NONE);
    ui_gesture_event_t press = {.type = UI_GESTURE_PRESS, .x0 = 300, .y0 = 700, .x = 300, .y = 700};
    assert(on_gesture(ctx, &press) == APP_REDRAW_NONE);
    press.type = UI_GESTURE_TAP;
    assert(on_gesture(ctx, &press) == APP_REDRAW_PAGE);
    assert(!s_push_word_card && !test_dict_word[0]);
    // 未收录：词典在但没这个词；词典不在：另一句准话。两者都不留白卡。
    // A miss: the dictionary is there but the word is not; no dictionary: a
    // different plain sentence. Neither leaves a blank card.
    test_dict_open_ok = false;
    test_dict_ready_flag = true;
    test_push_word_card = true;
    snprintf(test_push_word, sizeof(test_push_word), "zzzznotaword");
    assert(on_tick(ctx) == APP_REDRAW_PAGE);
    assert(!s_push_word_card && !strcmp(s_push_note, "未收录：zzzznotaword"));
    test_dict_ready_flag = false;
    test_push_word_card = true;
    assert(on_tick(ctx) == APP_REDRAW_PAGE);
    assert(!s_push_word_card && !strcmp(s_push_note, "词典不可用，无法查词"));
    // 错误码 → 用户文案；待武装失败 → 回方法页并留话。
    // Error codes to plain sentences; a failed pending arm returns to the
    // method page with a message.
    s_push_note[0] = 0;
    test_push_err = true;
    test_push_err_code = PUSH_ERR_CHECKSUM;
    assert(on_tick(ctx) == APP_REDRAW_PAGE);
    assert(!strcmp(s_push_note, "内容校验失败，请重新发送"));
    test_push_start_failed = true;
    assert(on_tick(ctx) == APP_REDRAW_PAGE);
    assert(s_view == TRANSFER_METHODS && !strcmp(s_method_message, "蓝牙启动失败，请重试"));
    // 插电常开切换：cancel + 重武装（window 0）；cancel 补的 closed 当场吃掉，
    // 页面留住，且不做内容相同的重复刷新。/ The charger-held switch: cancel and
    // re-arm (window 0); the closed posted by the cancel is swallowed on the
    // spot, the page stays, and no identical repeat refresh follows.
    app_transfer_request_push();
    on_enter(ctx);
    assert(s_view == TRANSFER_PUSH);
    test_push_state = PUSH_STATE_WINDOW;
    snprintf(test_push_adv, sizeof(test_push_adv), "PicoPush-1a2b");
    test_push_left_ms = 30u * 60u * 1000u;
    test_pmu_snapshot.charging = true;
    ctx->now_ms += 500;
    assert(on_tick(ctx) == APP_REDRAW_AREA);
    int arms = test_push_arm_calls;
    tap(ctx, control_rect(0));
    assert(test_push_arm_calls == arms + 1 && test_push_arm_window == 0);
    assert(test_push_armed && !test_push_closed && s_view == TRANSFER_PUSH);
    ctx->now_ms += 500;
    assert(on_tick(ctx) == APP_REDRAW_NONE);
    assert(s_view == TRANSFER_PUSH);
    // 未充电时常开按钮不可切。/ The hold button does nothing off the charger.
    test_pmu_snapshot.charging = false;
    arms = test_push_arm_calls;
    tap(ctx, control_rect(0));
    assert(test_push_arm_calls == arms && s_view == TRANSFER_PUSH);
    // 退出接收：cancel 之后 closed 把页面带回方法页。/ Exit receive: after the
    // cancel the closed event walks the page back to the method page.
    tap(ctx, control_rect(2));
    assert(!test_push_armed);
    assert(on_tick(ctx) == APP_REDRAW_PAGE && s_view == TRANSFER_METHODS);
    // 离页再进：nav 切走不再收摊（gen19 Bug3 起保活语义统一， armed 保持、
    // closed 不置）；on_enter 的清扫对陈事件兜底，页面不被立刻扔回方法页。
    // Leaving and returning: a nav-away no longer tears down (gen19 Bug3 unified
    // the keep-alive semantics: armed stays, no closed); on_enter's sweep covers
    // stale events so the page is not thrown straight back to the method page.
    // 本断言在 gen19 改语义时漏同步（测试文件停在 df09529），gen24 补上——
    // 教训同 CI 那条：宿主断言是「实现契约」，策略变更必须同步。
    // This assertion missed its gen19 sync (the test file stopped at df09529);
    // gen24 catches it up - same lesson as the CI fix: a host assertion is an
    // implementation contract and a strategy change must update it.
    // gen24.2 块 8：上一步已主动取消（armed=false），deep-link 不再自动武装，
    // 显式点按钮重新上台。/ Block 8: the previous step cancelled outright
    // (armed=false); the deep link no longer auto-arms, so the explicit tap
    // takes the stage again.
    tap(ctx, method_control_rect(2));
    on_enter(ctx);
    assert(s_view == TRANSFER_PUSH && test_push_armed && !test_push_closed);
    ui_gesture_event_t nav = {.type = UI_GESTURE_TAP, .x0 = 20, .y0 = 1150, .x = 20, .y = 1150};
    assert(on_gesture(ctx, &nav) == APP_REDRAW_NONE);
    assert(test_push_armed && !test_push_closed);
    app_transfer_request_push();
    on_enter(ctx);
    assert(s_view == TRANSFER_PUSH && test_push_armed && !test_push_closed);
    ctx->now_ms += 500;
    assert(on_tick(ctx) == APP_REDRAW_NONE && s_view == TRANSFER_PUSH);
    // 新鲜的 closed（会话自行收官）仍然把页面放回去。/ A fresh closed (the
    // session ended on its own) still walks the page back.
    test_push_armed = false;
    test_push_closed = true;
    assert(on_tick(ctx) == APP_REDRAW_PAGE && s_view == TRANSFER_METHODS);
    // 双保险：武装标志丢了（事件漏接的极端时序）也能把页面放回去。
    // The safety net: a lost armed flag (the extreme timing of a missed event)
    // still walks the page back.
    // gen24.2 块 8：armed=false 时 deep-link 不再自动武装，显式点按钮上台。
    // Block 8: with nothing armed the deep link no longer auto-arms; the
    // explicit tap takes the stage.
    tap(ctx, method_control_rect(2));
    on_enter(ctx);
    assert(s_view == TRANSFER_PUSH);
    test_push_armed = false;
    ctx->now_ms += 500;
    assert(on_tick(ctx) == APP_REDRAW_PAGE && s_view == TRANSFER_METHODS);
    transfer_on_exit(ctx);
}

// gen24 多态按钮 + 预检单一真源回归：四种姿态的点按行为、自愈路径、以及
// 「任何失败路径结束后其他按钮仍可用」的不假死底线。
// Gen24 multi-state button + precheck single-source regression: each pose's tap
// behaviour, the self-heal path, and the no-freeze bottom line (after any failure
// path the other rows keep working).
static void push_multistate_regression(app_ctx_t *ctx) {
    // ---- gate 真身：边界逐项（start_locked 用的就是这一个函数）----
    // The real gate, bounds one by one (the very function start_locked calls).
    assert(ble_pt_mem_gate(90 * 1024, 32 * 1024, 4 * 1024 * 1024));
    assert(!ble_pt_mem_gate(90 * 1024, 19 * 1024, 4 * 1024 * 1024)); // gen23 真机现场 / the gen23 field case
    assert(!ble_pt_mem_gate(90 * 1024, 32 * 1024, 128 * 1024));      // PSRAM 不够 / PSRAM short
    assert(ble_pt_mem_gate(BLE_PT_MIN_FREE_INTERNAL, BLE_PT_MIN_LARGEST_INTERNAL, BLE_PT_MIN_FREE_PSRAM));
    assert(!ble_pt_mem_gate(BLE_PT_MIN_FREE_INTERNAL - 1, BLE_PT_MIN_LARGEST_INTERNAL, BLE_PT_MIN_FREE_PSRAM));
    assert(!ble_pt_mem_gate(BLE_PT_MIN_FREE_INTERNAL, BLE_PT_MIN_LARGEST_INTERNAL - 1, BLE_PT_MIN_FREE_PSRAM));
    assert(!ble_pt_mem_gate(BLE_PT_MIN_FREE_INTERNAL, BLE_PT_MIN_LARGEST_INTERNAL, BLE_PT_MIN_FREE_PSRAM - 1));

    // 干净开局：富余堆、无锁、方法页。/ A clean slate: ample heap, no lock, method page.
    test_push_state = PUSH_STATE_IDLE;
    test_push_armed = false;
    test_ble_locked = false;
    test_ble_free_internal = 90 * 1024;
    test_ble_largest_internal = 32 * 1024;
    test_ble_free_psram = 4 * 1024 * 1024;
    test_ble_reset_calls = 0;
    s_method_message[0] = 0;
    on_enter(ctx);
    assert(s_view == TRANSFER_METHODS);

    // ---- 姿态 1：干净 IDLE → 武装（预检过；reset 幂等顺带清一次）----
    // Pose 1: a clean IDLE arms (precheck passes; the idempotent reset rides along).
    int arms = test_push_arm_calls;
    tap(ctx, method_control_rect(2));
    assert(test_push_arm_calls == arms + 1 && s_view == TRANSFER_PUSH);
    assert(test_ble_reset_calls == 1);
    s_view = TRANSFER_METHODS; // 回方法页（不収摊）/ back to the method page (no teardown)

    // ---- 姿态 2：ARMED/WINDOW → 取消回 IDLE（带原因）----
    // Pose 2: ARMED/WINDOW cancels back to IDLE (with a reason).
    test_push_state = PUSH_STATE_WINDOW;
    test_push_armed = true;
    int cancels = test_push_cancel_calls;
    arms = test_push_arm_calls;
    tap(ctx, method_control_rect(2));
    assert(test_push_cancel_calls == cancels + 1 && test_push_arm_calls == arms);
    assert(!test_push_armed && !strcmp(test_push_close_why, "已取消接收"));
    // gen24.2 R3：取消补的 closed 由方法页消费——原因上屏（「已取消接收」），
    // 不再烂死在队列里。/ R3: the cancel's closed is consumed by the method
    // page - the reason lands on screen instead of rotting in the queue.
    ctx->now_ms += 500;
    assert(on_tick(ctx) == APP_REDRAW_PAGE && !strcmp(s_method_message, "已取消接收"));

    // ---- 姿态 3a：锁死 + 预检不过（gen23 现场：最大块 19K）----
    // 提示带数字、不武装、不清锁、零等待（不进 pending、不吞 30s 退避）。
    // Pose 3a: locked + precheck fails (the gen23 field case: 19K largest) - the
    // prompt carries the numbers, nothing arms, no lock clearing, zero wait (no
    // pending, no 30 s back-off to sit through).
    test_push_state = PUSH_STATE_IDLE;
    test_ble_locked = true;
    test_ble_largest_internal = 19 * 1024;
    s_method_message[0] = 0;
    arms = test_push_arm_calls;
    cancels = test_push_cancel_calls;
    int resets = test_ble_reset_calls;
    tap(ctx, method_control_rect(2));
    assert(test_push_arm_calls == arms && test_push_cancel_calls == cancels);
    assert(test_ble_reset_calls == resets); // 预检不过不清锁 / a failed precheck clears nothing
    assert(s_view == TRANSFER_METHODS);
    assert(strstr(s_method_message, "内存碎片不足") && strstr(s_method_message, "19K") &&
           strstr(s_method_message, "20K"));
    // 不假死：再点一次同样只出提示；on_tick 不挂。/ No freeze: tapping again only
    // re-prompts, and on_tick never hangs.
    tap(ctx, method_control_rect(2));
    assert(test_push_arm_calls == arms && s_view == TRANSFER_METHODS);
    ctx->now_ms += 500;
    assert(on_tick(ctx) == APP_REDRAW_NONE);
    // 不假死：其他按钮仍可用——WiFi 行走配网入口，页面切得动、状态变得了。
    // No freeze: the other rows still work - the WiFi row walks the provisioning
    // entry, the page moves and the state stays mutable.
    s_method_message[0] = 0;
    tap(ctx, method_control_rect(0));
    assert(s_view != TRANSFER_METHODS || s_method_message[0]); // 有反应：切页或留话 / it reacts: moves or speaks
    on_enter(ctx);
    assert(s_view == TRANSFER_METHODS);

    // ---- 姿态 3a'：未锁 + 栈 down + 预检不过（gen24.1 D1：两段式文案）----
    // 与 3a 同一条拒绝路径但未锁：提示带数字，且两段话都落到消息串里——
    // 先让用户卸书（大概率自愈），再兜底重启。不武装、不清锁、零退避。
    // Pose 3a': unlocked + stack down + precheck fails - the gen24.1 D1
    // two-segment prompt. Same refusal path as 3a but unlocked: numbers ride
    // along and BOTH segments land in the string - first ask the user to leave
    // the book (likely self-heals), then fall back to a restart. No arm, no
    // lock clearing, zero back-off.
    test_ble_locked = false;
    test_ble_largest_internal = 19 * 1024;
    s_method_message[0] = 0;
    arms = test_push_arm_calls;
    cancels = test_push_cancel_calls;
    resets = test_ble_reset_calls;
    tap(ctx, method_control_rect(2));
    assert(test_push_arm_calls == arms && test_push_cancel_calls == cancels);
    assert(test_ble_reset_calls == resets);
    assert(strstr(s_method_message, "内存碎片不足") && strstr(s_method_message, "19K") &&
           strstr(s_method_message, "20K"));
    assert(strstr(s_method_message, "退出当前书籍后重试") && strstr(s_method_message, "请重启设备"));
    assert(strstr(s_method_message, "——")); // 两段式分界落进消息串 / the split lands in the string

    // ---- 姿态 3a''：栈 up + 预检不过 → 跳过闸门直接武装（gen24.1 D2）----
    // 栈已在跑时点按钮被 gen24 冤拒（boot1 实证：BLE 自开机 up、碎片窗口内
    // 拦截）。gen24.1 起闸门只拦启动路径：栈 up 跳过预检，恢复 gen23 语义。
    // Pose 3a'': stack up + precheck would fail - skip the gate and arm anyway
    // (gen24.1 D2). gen24 falsely refused the button while the stack was
    // already up (boot1 field data). From gen24.1 the gate guards only the
    // start path: an up stack bypasses the precheck, restoring gen23 semantics.
    test_ble_running = true;
    s_method_message[0] = 0;
    arms = test_push_arm_calls;
    resets = test_ble_reset_calls;
    tap(ctx, method_control_rect(2));
    assert(test_push_arm_calls == arms + 1 && s_view == TRANSFER_PUSH);
    assert(test_ble_reset_calls == resets + 1); // 武装即清锁（自愈顺带）/ arming clears the lock
    assert(!s_method_message[0]);               // 无提示、无冤拒 / no prompt, no false refusal
    test_ble_running = false;                   // 归还：后续用例回到栈 down 语义 / hand back to stack-down semantics
    s_view = TRANSFER_METHODS;

    // ---- 姿态 3b：锁死 + 预检过 → 一键自愈（清锁 + 重试一次到位）----
    // Pose 3b: locked + precheck passes - one-tap self-heal (clear lock + retry).
    test_ble_locked = true;
    test_ble_largest_internal = 32 * 1024;
    s_method_message[0] = 0;
    arms = test_push_arm_calls;
    resets = test_ble_reset_calls;
    tap(ctx, method_control_rect(2));
    assert(test_ble_reset_calls == resets + 1 && !test_ble_locked);
    assert(test_push_arm_calls == arms + 1 && s_view == TRANSFER_PUSH);
    s_view = TRANSFER_METHODS;

    // ---- 姿态 4：CONNECTED/RECEIVING → 禁用（不 arm 不 cancel）----
    // Pose 4: CONNECTED/RECEIVING - disabled (neither arms nor cancels).
    test_push_state = PUSH_STATE_CONNECTED;
    test_push_armed = true;
    arms = test_push_arm_calls;
    cancels = test_push_cancel_calls;
    tap(ctx, method_control_rect(2));
    assert(test_push_arm_calls == arms && test_push_cancel_calls == cancels);
    test_push_state = PUSH_STATE_RECEIVING;
    tap(ctx, method_control_rect(2));
    assert(test_push_arm_calls == arms && test_push_cancel_calls == cancels);
    // 复位，把现场还给后续用例。/ Reset and hand the field back to later cases.
    test_push_state = PUSH_STATE_IDLE;
    test_push_armed = false;
    // gen24.2 旋钮归还：四门默认态（开关开/飞行关/USB 停）+ pending 清零，
    // 后续用例回到 gen24.1 及更早的语义。/ Hand back the gen24.2 knobs: the
    // four gates' defaults (switch on, flight off, USB stopped) and pending
    // cleared, so later cases keep their meaning.
    test_settings_ble_turner = true;
    test_settings_flight_mode = false;
    test_usb_active = false;
    test_push_pending = false;
    s_method_inline_enable = false;
    transfer_on_exit(ctx);
}

// gen24.2 前置四门 + 行内【开启】按钮回归：门 2 开关门提示与原地写设置、
// 内存/飞行/USB 三门拦截各态、以及「pending 中关开关 → cancel_why →
// closed → 方法页显示原因」的极端闭环与块 6 底账。
// The gen24.2 four-gate + inline [enable] regression: the gate-2 switch hint
// and its in-place settings write, each refusal state of the memory/flight/
// USB gates, plus the extreme loop "switch off during pending -> cancel_why
// -> closed -> the method page shows the reason" and the block-6 ledger.
static void push_gate_regression(app_ctx_t *ctx) {
    // 干净开局：富余堆、无锁、栈 down、开关开、无飞行、无 USB、无 pending。
    // A clean slate: ample heap, no lock, stack down, switch on, no flight,
    // no USB, nothing pending.
    test_push_state = PUSH_STATE_IDLE;
    test_push_armed = false;
    test_push_pending = false;
    test_ble_locked = false;
    test_ble_running = false;
    test_ble_free_internal = 90 * 1024;
    test_ble_largest_internal = 32 * 1024;
    test_ble_free_psram = 4 * 1024 * 1024;
    test_ble_reset_calls = 0;
    test_push_arm_calls = 0;
    test_push_cancel_calls = 0;
    test_push_closed = test_push_start_failed = false;
    test_settings_ble_turner = true;
    test_settings_flight_mode = false;
    test_usb_active = false;
    s_method_message[0] = 0;
    s_method_inline_enable = false;
    on_enter(ctx);
    assert(s_view == TRANSFER_METHODS);

    // ---- 门 2：开关关 → 行内提示 + 【开启】按钮、零武装零 pending ----
    // Gate 2: switch off - the inline hint + [enable] button, nothing armed
    // and nothing pended.
    test_settings_ble_turner = false;
    s_method_message[0] = 0;
    int arms = test_push_arm_calls;
    tap(ctx, method_control_rect(2));
    assert(test_push_arm_calls == arms && !test_push_armed && !test_push_pending);
    assert(strstr(s_method_message, "蓝牙已关闭"));
    assert(s_method_inline_enable); // 按钮只在门 2 出现 / the button shows only at gate 2
    // ---- 行内【开启】点按：原地写设置 + 重走四门 + 既有武装路径 ----
    // The inline [enable] tap: write the setting in place, re-run the four
    // gates, take the existing arm path.
    int resets = test_ble_reset_calls;
    tap(ctx, inline_enable_rect());
    assert(test_settings_ble_turner);        // 单一真源已写入 / the single source was written
    assert(test_push_arm_calls == arms + 1); // 重走四门后走既有武装路径 / the arm path ran
    assert(s_view == TRANSFER_PUSH);         // 成功 → 武装视图 / success lands on the arm view
    assert(!s_method_inline_enable);         // 按钮随提示条消失 / the button goes with the hint
    assert(test_ble_reset_calls == resets + 1); // 自愈 reset 顺带一次 / the idempotent heal reset rides along

    // ---- 行内按钮点按但复查不过（飞行门）：提示回退到对应门（§二-3）----
    // The inline tap with a failing recheck (flight gate): the hint falls
    // back to that gate (section 2-3).
    s_view = TRANSFER_METHODS;
    test_settings_ble_turner = false;
    test_settings_flight_mode = true; // 写设置后仍被飞行门拦 / flight still refuses after the write
    s_method_message[0] = 0;
    arms = test_push_arm_calls;
    resets = test_ble_reset_calls;
    tap(ctx, method_control_rect(2));        // 门 2 提示 / gate 2 refuses first
    assert(s_method_inline_enable && strstr(s_method_message, "蓝牙已关闭"));
    tap(ctx, inline_enable_rect());          // 写设置 + 重走四门 → 飞行门不过 / the flight gate now refuses
    assert(test_settings_ble_turner);        // 写入仍然发生 / the write still happened
    assert(test_push_arm_calls == arms && test_ble_reset_calls == resets);
    assert(!s_method_inline_enable);
    assert(strstr(s_method_message, "飞行模式开启中")); // 回对应门提示 / back to that gate's hint
    test_settings_flight_mode = false;

    // ---- 门 3：飞行模式（无行内按钮——§二-4 边界）----
    // Gate 3: flight mode (no inline button - section 2-4's boundary).
    test_settings_ble_turner = true;
    test_settings_flight_mode = true;
    s_method_message[0] = 0;
    arms = test_push_arm_calls;
    tap(ctx, method_control_rect(2));
    assert(test_push_arm_calls == arms && !s_method_inline_enable);
    assert(strstr(s_method_message, "飞行模式开启中"));
    test_settings_flight_mode = false;

    // ---- 门 4：USB 存储接管 ----
    // Gate 4: USB storage owns the card.
    test_usb_active = true;
    s_method_message[0] = 0;
    arms = test_push_arm_calls;
    tap(ctx, method_control_rect(2));
    assert(test_push_arm_calls == arms && !s_method_inline_enable);
    assert(strstr(s_method_message, "USB 传书已接管"));
    test_usb_active = false;

    // ---- 极端闭环：pending 中关开关 → cancel_why → closed → 方法页显示 ----
    // The extreme loop: switch off during pending -> cancel_why -> closed ->
    // the method page shows the reason.
    s_view = TRANSFER_METHODS;
    s_method_message[0] = 0;
    test_push_armed = false;
    test_push_pending = true;            // 模拟 on_stack_sync 未到的窗口 / the pre-sync window
    test_push_cancel_calls = 0;
    push_sink_cancel_why("蓝牙已关闭");  // 设置页/快捷层关开关的同一动作 / the switch-off action
    assert(test_push_cancel_calls == 1 && !test_push_pending && !test_push_armed);
    assert(test_push_closed);            // cancel 从 pending 补 closed / the pending cancel posts closed
    assert(on_tick(ctx) == APP_REDRAW_PAGE); // 方法页消费 closed（R3 根修）/ the method page consumes it
    assert(!strcmp(s_method_message, "蓝牙已关闭"));
    assert(!strcmp(s_last_cancel_why, "蓝牙已关闭")); // 块 6 底账入账 / the ledger takes it
    // 块 6：再进传书页，底账挂上提示条 / Block 6: re-entering hangs the ledger.
    on_enter(ctx);
    assert(!strcmp(s_method_message, "上次接收已结束：蓝牙已关闭"));
    // 武装后底账清除（新回合干净开局）/ The arm clears the ledger (a fresh round).
    test_push_pending = false;
    s_method_message[0] = 0;
    arms = test_push_arm_calls;
    tap(ctx, method_control_rect(2));
    assert(test_push_arm_calls == arms + 1 && s_view == TRANSFER_PUSH && !s_last_cancel_why[0]);
    s_view = TRANSFER_METHODS;
    // 归还现场 / hand the field back.
    test_push_armed = false;
    test_push_pending = false;
    transfer_on_exit(ctx);
}

// gen24.2 R2/R3 回归：pending 期间推送页不闪回、方法页消费 start_failed 与
// closed、以及 start_failed 先于 closed 的查序（pending 超时两标志同置）。
// The gen24.2 R2/R3 regression: the push page holds through pending, the
// method page consumes start_failed and closed, and the start_failed-before-
// closed order (a pending timeout sets both).
static void push_pending_ui_regression(app_ctx_t *ctx) {
    test_push_state = PUSH_STATE_IDLE;
    test_push_armed = false;
    test_push_pending = true; // 点了接收、栈还没起来 / receive pressed, stack still down
    test_push_left_ms = 0;
    test_push_adv[0] = 0;
    s_push_note[0] = 0;
    s_view = TRANSFER_PUSH;
    ctx->now_ms = 5000;
    // 对齐活区簿记（真实路径由 push_arm_now 调）/ align the live bookkeeping
    // (the real path calls it from push_arm_now).
    push_sync_live_bookkeeping();
    // R2：pending 中 tick 不闪回方法页（gen24.2 前此处 500ms 内把用户踢走）
    // R2: a pending tick does not bounce back (pre-gen24.2 the user was
    // kicked out within 500 ms).
    assert(on_tick(ctx) == APP_REDRAW_NONE && s_view == TRANSFER_PUSH);
    // 双保险仍有效：armed 与 pending 双丢才回退 / The safety net still holds:
    // only a double loss (neither armed nor pending) walks back.
    test_push_pending = false;
    ctx->now_ms += 500;
    assert(on_tick(ctx) == APP_REDRAW_PAGE && s_view == TRANSFER_METHODS);
    // R3：方法页消费 start_failed（pending 超时路径）/ R3: the method page
    // consumes start_failed (the pending-timeout path).
    test_push_start_failed = true;
    test_push_closed = true; // 超时路径两标志同置（CANEL 的 ADV_OFF 顺带 closed）
    s_method_message[0] = 0;
    assert(on_tick(ctx) == APP_REDRAW_PAGE);
    assert(!strcmp(s_method_message, "蓝牙启动失败，请重试")); // 先于 closed 查 / checked before closed
    assert(!test_push_closed && !test_push_start_failed);
    // R3：方法页消费 closed（窗口到期/关开关等非主动收摊）/ R3: the method
    // page consumes closed (window expiry, switch-off, ...).
    test_push_closed = true;
    snprintf(test_push_close_why, sizeof(test_push_close_why), "蓝牙已关闭");
    s_method_message[0] = 0;
    assert(on_tick(ctx) == APP_REDRAW_PAGE);
    assert(!strcmp(s_method_message, "蓝牙已关闭") && !strcmp(s_last_cancel_why, "蓝牙已关闭"));
    // 无事件时方法页安静待命 / With no events the method page waits quietly.
    ctx->now_ms += 500;
    assert(on_tick(ctx) == APP_REDRAW_NONE);
    // 归还现场 / hand the field back.
    test_push_pending = false;
    test_push_armed = false;
    test_push_start_failed = test_push_closed = false;
    s_last_cancel_why[0] = 0;
}

int main(void) {
    assert(transfer_signature_valid("",0)&&transfer_signature_valid("中文🙂",10));
    assert(!transfer_signature_valid("a\0b",3)&&!transfer_signature_valid("\xed\xa0\x80",3));
    assert(!transfer_signature_valid("\xc0\xaf",2)&&!transfer_signature_valid("\xf4\x90\x80\x80",4));
    assert(!transfer_signature_valid("\xe4\xb8",2)&&!transfer_signature_valid("a\nb",3));
    strcpy(test_signature,"reader");char signature[96];assert(transfer_signature_get(signature,sizeof(signature))&&!strcmp(signature,"reader"));
    test_signature_fail=true;assert(transfer_signature_set("新签名")==ESP_FAIL&&!strcmp(test_signature,"reader"));
    test_signature_fail=false;assert(transfer_signature_set("新签名")==ESP_OK&&!strcmp(test_signature,"新签名"));
    assert(s_signature_revision==1);s_signature_seen=s_signature_revision;
    uint8_t fb = 0;
    app_ctx_t ctx = {.fb = &fb};
    app_transfer_request_method_picker();
    on_enter(&ctx);
    assert(s_view == TRANSFER_METHODS && !s_wifi_settings_only && !s_start_pending);
    on_tick(&ctx);
    assert(test_qr_encodes == 0);
    qr_regression(&ctx);
    test_configured = true;
    on_enter(&ctx);
    tap(&ctx, method_control_rect(0));
    assert(s_view == TRANSFER_HOME && s_start_pending && !s_scan_pending);
    assert(s_mode == READ_PICO_TRANSFER_MODE_STA);
    on_tick(&ctx);
    tap(&ctx, control_rect(2));
    assert(s_view == TRANSFER_METHODS && !s_start_pending);
    test_configured = false;
    on_enter(&ctx);
    ui_gesture_event_t cancelled = {.type=UI_GESTURE_PRESS, .x0=50, .y0=350, .x=50, .y=350};
    assert(on_gesture(&ctx, &cancelled) == APP_REDRAW_AREA);
    cancelled.type = UI_GESTURE_CANCEL;
    assert(on_gesture(&ctx, &cancelled) == APP_REDRAW_AREA);
    assert(s_view == TRANSFER_METHODS && !s_start_pending && s_pressed == -1);
    for (int i = 44; i < 48; ++i) {
        EpdRect r = password_control_rect(i);
        assert(r.x >= 0 && r.x + r.width <= UI_LOCK_WIDTH);
        assert(r.y >= 0 && r.y + r.height <= UI_LOCK_HEIGHT);
    }
    on_enter(&ctx);
    test_busy = true;
    tap(&ctx, method_control_rect(0));
    assert(s_view == TRANSFER_METHODS);
    test_busy = false;
    tap(&ctx, method_control_rect(0));
    assert(s_view == TRANSFER_NETWORKS && s_scan_pending);
    network_ui_tick(&ctx);
    assert(s_network_count == 8 && !s_scan_pending);
    test_configured = s_saved_configured = true;
    tap(&ctx, (EpdRect){540, UI_NAV_TOP + 16, 20, 20});
    assert(test_nav_index == 3);
    test_nav_index = -1;
    assert(s_view == TRANSFER_NETWORKS);
    tap(&ctx, network_control_rect(11));
    assert(test_forget_count == 0);
    tap(&ctx, network_control_rect(12));
    assert(test_forget_count == 0 && s_saved_configured && !s_forget_confirm);
    tap(&ctx, network_control_rect(11));
    test_forget_error = ESP_FAIL;
    tap(&ctx, network_control_rect(11));
    assert(test_forget_count == 1 && s_saved_configured && s_forget_confirm);
    test_forget_error = ESP_OK;
    tap(&ctx, network_control_rect(11));
    assert(test_forget_count == 2 && !s_saved_configured);
    test_configured = true;
    enter_networks();
    network_ui_tick(&ctx);
    tap(&ctx, network_control_rect(9));
    assert(s_network_page == 1);
    tap(&ctx, network_control_rect(7));
    assert(s_network_page == 0);
    // 断开保留凭据，上传期间拒绝断开，之后仍可重连。
    // Disconnect keeps credentials, refuses an active upload and allows reconnecting.
    test_status.network_ready = s_status.network_ready = true;
    s_session_started = true;
    test_busy = true;
    int stops = test_stop_count;
    tap(&ctx, network_control_rect(6));
    assert(test_stop_count == stops && s_status.network_ready);
    test_busy = false;
    tap(&ctx, network_control_rect(6));
    assert(test_stop_count > stops && !s_session_started && s_saved_configured && test_configured);
    assert(!s_start_pending && !s_network_connect_pending && test_forget_count == 2);
    test_status.network_ready = s_status.network_ready = false;
    tap(&ctx, network_control_rect(6));
    assert(s_start_pending && s_mode == READ_PICO_TRANSFER_MODE_STA);
    s_start_pending = false; s_view = TRANSFER_NETWORKS;
    tap(&ctx, network_control_rect(9));
    assert(s_network_page == 1);
    tap(&ctx, network_control_rect(3));
    assert(s_view == TRANSFER_PASSWORD && !strcmp(s_selected.ssid, "network7"));
    s_view = TRANSFER_NETWORKS; s_network_page = 0;
    tap(&ctx, network_control_rect(0));
    assert(s_view == TRANSFER_PASSWORD && !s_password[0]);
    tap(&ctx, password_control_rect(46));
    assert(test_save_count == 0 && s_view == TRANSFER_PASSWORD);
    password_caret_regression(&ctx);
    ui_gesture_event_t cancel = {.type = UI_GESTURE_PRESS,.x0 = 42,.y0 = 422,.x = 42,.y = 422};
    on_gesture(&ctx, &cancel);
    cancel.type = UI_GESTURE_TAP;
    cancel.x = 650;
    on_gesture(&ctx, &cancel);
    assert(!s_password[0]);
    // 容量及保存失败时保留密码。/ Capacity and preservation after failed saving.
    tap(&ctx,(EpdRect){24,1010,30,30});
    for(int i=0;i<70;i++)tap(&ctx,(EpdRect){24,740,30,30});
    assert(strlen(s_password)==64);
    tap(&ctx,(EpdRect){600,896,30,30});assert(strlen(s_password)==63);
    tap(&ctx, password_control_rect(45));
    assert(s_password_visible);
    test_save_error = ESP_FAIL;
    tap(&ctx, password_control_rect(46));
    assert(s_view == TRANSFER_PASSWORD && strlen(s_password) == 63);
    test_save_error = ESP_OK;
    tap(&ctx, password_control_rect(46));
    assert(s_view == TRANSFER_HOME && s_mode == READ_PICO_TRANSFER_MODE_STA && s_start_pending);
    assert(!s_password[0]);
    assert(strcmp(test_saved_ssid, "中文家庭网络") == 0);
    enter_networks();
    network_ui_tick(&ctx);
    tap(&ctx, network_control_rect(0));
    tap(&ctx,(EpdRect){24,740,30,30});
    tap(&ctx, password_control_rect(44));
    assert(s_view == TRANSFER_NETWORKS && !s_password[0]);
    test_scan_count = 0;
    tap(&ctx, network_control_rect(8));
    network_ui_tick(&ctx);
    assert(!s_network_count && s_network_message[0]);
    test_scan_error = ESP_FAIL;
    tap(&ctx, network_control_rect(8));
    network_ui_tick(&ctx);
    assert(!s_network_count && s_network_message[0]);
    tap(&ctx, network_control_rect(6));
    assert(s_view == TRANSFER_HOME && s_start_pending && s_mode == READ_PICO_TRANSFER_MODE_STA);
    int stops_before = test_stop_count;
    tap(&ctx, control_rect(2));
    assert(test_stop_count == stops_before + 1 && !s_start_pending);
    assert(s_view == TRANSFER_METHODS && !ctx.request_return && ctx.request_app == NULL);
    on_enter(&ctx);
    tap(&ctx, method_control_rect(1));
    on_tick(&ctx);
    test_busy = true;
    stops_before = test_stop_count;
    tap(&ctx, control_rect(2));
    assert(test_stop_count == stops_before && s_view == TRANSFER_HOME && !ctx.request_return);
    tap(&ctx, (EpdRect){540, UI_NAV_TOP + 16, 20, 20});
    assert(test_stop_count == stops_before && test_nav_index == -1);
    test_busy = false;
    test_status.changed_count = 1;
    ctx.now_ms = 3000;
    assert(on_tick(&ctx) == APP_REDRAW_AREA);
    assert(s_status.changed_count == 1);
    s_session_started = true;
    transfer_on_exit(&ctx);
    assert(test_store_changes == 1);
    on_enter(&ctx);
    tap(&ctx, method_control_rect(1));
    on_tick(&ctx);
    s_root.is_flash = false;
    s_free = 123456;
    stops_before = test_stop_count;
    app_transfer.on_media_lost(&ctx);
    assert(test_stop_count == stops_before + 1);
    assert(s_media_lost && !s_free && !s_qr_ready && !s_root.path[0]);
    assert(!s_start_pending && !s_session_started && s_view == TRANSFER_METHODS);
    s_root.is_flash = true;
    app_transfer.on_media_lost(&ctx);
    assert(test_stop_count == stops_before + 1);
    ctx.request_return = false;
    test_configured = true;
    app_transfer_request_wifi_upload();
    on_enter(&ctx);
    assert(s_direct_entry && s_view == TRANSFER_HOME && s_start_pending);
    tap(&ctx, control_rect(2));
    assert(ctx.request_return && s_view == TRANSFER_HOME);
    ctx.request_return = false;
    app_transfer_request_hotspot_start();
    on_enter(&ctx);
    assert(s_direct_entry && s_view == TRANSFER_HOME && s_start_pending);
    tap(&ctx, control_rect(2));
    assert(ctx.request_return && s_view == TRANSFER_HOME);
    ctx.request_return = false;
    app_transfer_request_usb_start();
    on_enter(&ctx);
    assert(s_usb_start_pending && !test_usb_active);
    assert(on_tick(&ctx) == APP_REDRAW_PAGE);
    assert(s_direct_entry && s_usb_entry && s_view == TRANSFER_HOME && test_usb_active);
    test_usb_stop_error = ESP_FAIL;
    tap(&ctx, (EpdRect){36, 79, 50, 50});
    assert(test_usb_active && !ctx.request_return && s_view == TRANSFER_HOME);
    assert(on_key(&ctx, UI_KEY_2) == APP_REDRAW_PAGE && !ctx.request_app);
    assert(test_usb_active && s_usb_message[0]);
    test_usb_stop_error = ESP_OK;
    tap(&ctx, control_rect(2));
    assert(ctx.request_return && !test_usb_active);
    enter_networks();
    test_scan_count = 8; test_scan_error = ESP_OK;
    network_ui_tick(&ctx);
    wifi_recovery_regression(&ctx);
    push_arm_regression(&ctx);
    push_multistate_regression(&ctx);
    push_gate_regression(&ctx);
    push_pending_ui_regression(&ctx);
    puts("transfer_ui_host_test: PASS");
}
