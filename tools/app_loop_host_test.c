/* SPDX-License-Identifier: Apache-2.0
 * 中文：运行真实主循环，用确定性触摸和任务延时退出验证调度。
 * English: Run the real loop with deterministic touch and task-delay exit.
 */
#include "app_loop.h"
#include "ui_gesture.h"
#include "ui_dualgesture.h"
#include "ui_menu.h"
#include "ui_power_dialog.h"
#include "ui_quick_menu.h"
#include "boot_state.h"
// gen26 AI 释义轮询的 API/尺寸宏来源（PUSH_AI_WORD_MAX 等）。
// Source of the gen26 AI poll API and size macros (PUSH_AI_WORD_MAX etc.).
#include "push_sink.h"
// 26.11.1：两拍提交泵（真身进宿主，用例 20 直接驱动它）。
// The two-beat commit pump - case 20 drives the real body.
#include "transfer_commit.h"
// 26.11.16 电量修复批：桩里新增 pmu_snapshot_t 与两个电量读数。
#include "read_pico_pmu.h"
// 26.11.17 首页手势批：纯判定模块（空白 / 双指上滑 / 长按归属）。
#include "home_gesture.h"

// 用例 20 的控制量：main 在前、桩区在后，故定义放这里（桩函数只读写它们）。
// Case 20's controls: main comes before the shim block, so they are defined here and
// the shims only read/write them.
static bool commit_take_hit;          // take 是否命中 / whether take hits
static bool commit_release_ok;        // 关书是否成功 / whether the reader lets go
static bool commit_run_ok;            // 四拍是否成功 / whether the four beats succeed
static int commit_run_calls, commit_abort_calls, commit_abort_err;
static bool commit_waiting_at_abort;  // abort 那一刻等待态是否已归零 / was the flag cleared at abort
static bool commit_abort_err_ok;      // abort 收到的是否 ERR(BUSY) / did abort get ERR(BUSY)
uint8_t app_settings_system_contrast(void) {return 100;}
void pico_boot_ready(void) {}
void app_book_cover_mode_changed(void) {}
void app_home_cover_mode_changed(void) {}
esp_err_t pico_boot_save_resume(const pico_resume_t *resume) { (void)resume; return ESP_OK; }
void pico_boot_clear_resume(void) {}
bool app_book_resume_context(char *p,size_t c,bool *f) { (void)p;(void)c;(void)f;return false; }
#include "read_pico_transfer.h"
#include "ble_page_turner.h"
#include "ui_nav_layout.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

// 横屏旋转替身：默认竖屏 INVERTED_PORTRAIT（设备实际方向），用例可切换；逻辑帧
// 尺寸随方向变（横屏 1216x684、竖屏 684x1216），与 epdiy 的 epd_width/epd_height
// 语义一致，app_loop 的横屏映射直接依赖这两个尺寸。
// Rotation shim: portrait INVERTED_PORTRAIT (the device's real orientation) by
// default; cases may switch it. Logical frame sizes follow the rotation
// (landscape 1216x684, portrait 684x1216), matching epdiy semantics, which
// app_loop's landscape remap relies on.
enum EpdRotation host_rotation = EPD_ROT_INVERTED_PORTRAIT;
enum EpdRotation epd_get_rotation(void) { return host_rotation; }
// 与 epdiy 真实语义一致：纯状态设置。/ Matches the real epdiy semantics: a pure
// state set. Lets lock/exit fallback cases drive and observe the reset.
void epd_set_rotation(enum EpdRotation rotation) { host_rotation = rotation; }
int epd_width(void) { return host_rotation == EPD_ROT_LANDSCAPE ? 1216 : 684; }
int epd_height(void) { return host_rotation == EPD_ROT_LANDSCAPE ? 684 : 1216; }

const app_desc_t app_book = {.title="book"};
const app_desc_t app_transfer = {.title="transfer"};
static int power_open_draws, power_controls_draws, power_final_draws;
static int power_trigger_step = -1, shutdowns, restarts;
static bool staged_shutdown;
EpdRect ui_power_dialog_rect(void) {return (EpdRect){0,0,684,1216};}
void ui_power_dialog_draw(uint8_t* fb,const ui_power_dialog_t *dialog) {(void)fb;(void)dialog;++power_open_draws;}
void ui_power_dialog_draw_controls(uint8_t* fb,const ui_power_dialog_t *dialog) {(void)fb;(void)dialog;++power_controls_draws;}
void ui_power_final_draw(uint8_t* fb,bool restarting) {(void)fb;(void)restarting;++power_final_draws;}

static jmp_buf done;
// 26.9.7：两条触点各自带坐标（x1,y1 缺省 0,0 = 单指形态）。幽灵 TAP 的复现要用
// 真双指（判据要求两点各自有效），只给 count 不够。
// 26.9.7: both contacts carry coordinates (x1,y1 default 0,0 = single-finger shape).
// Reproducing the ghost TAP needs a real two-finger pose - the predicate wants each
// point valid, so count alone is not enough.
typedef struct { int x, y, count, error; int x1, y1; } sample_t;
static sample_t samples[32];
static int sample_count, step, ticks, enters, exits, touch_calls, keys[3], events[16];
static int menus, highlights, restores, fulls, mode, tick_consumed[32];
static int long_keys;
static uint8_t idle_minutes;
static int idle_locks, before_locks, lock_feedback_calls, lock_badges, wake_clean_requests;
static bool feedback_enabled;
static bool online_busy;
static bool ble_enabled, saved_wifi, upload_busy, click_enabled, click_active;
static read_pico_transfer_status_t transfer_status;
static int quick_draws, quick_gl, quick_du, wifi_starts, wifi_stops, ble_resets, click_presses, click_restores;
// push_sink 调度面可编程替身状态：默认未武装；cancel_why 记录见桩定义处注释。
// Programmable push_sink scheduling-shim state: unarmed by default; the
// cancel_why recorder is documented at its stub definition.
static bool push_armed;
static int push_cancel_calls;
static char push_cancel_why[64];
// gen26 AI 释义卡轮询替身：take 一次一取（模拟 UI 槽被取空），ready 的可受理性
// 可编程（false=有面板开着/非阅读态，阅读器不收）。M1 引入 app_loop 轮询分支时
// 漏同步这两个桩，app_loop/book_management 两个 runner 编译红到 gen26.3 才暴露
// ——与 gen21 cancel_why 桩同型第三次，教训见下方桩定义处。
// gen26 AI definition poll shims: the take drains the slot once (mimicking the
// UI slot emptying) and acceptance is programmable (false = a panel is open or
// not reading, so the reader refuses). M1 added the poll branch without these
// stubs and two runners stayed compile-red until gen26.3 - the third incident
// of the same shape as the gen21 cancel_why stub; see the note there.
static bool ai_take_hit, ai_ready_ok = true;
static int ai_presents, ai_ready_calls;
// M3-B 情景认领桩的状态。/ M3-B scene-claim stub state.
static int scene_claim_calls;
int vocab_stub_set_def_calls; // M3-B：生词批调用计数 / scene-batch counter
static bool scene_claim_ok;
static char scene_claim_word[64];
static int ai_full_presents;
static char ai_seen_word[PUSH_AI_WORD_MAX + 1], ai_seen_def[PUSH_AI_DEF_MAX + 1];
// M2 Step9 接线桩：双 GC16 档位查询与详解活跃谓词。ai_def_full_ok 的一次性
// 消费语义与真实 app_book_ai_def_full() 逐字一致（取一次即清），守门用例因此
// 能验「开详解当轮回流走 FULL、下一轮回流自动回落」而不必真跑 app_book。
// M2 Step9 wiring shims: the double-GC16 gear query and the detail-active
// predicate. ai_def_full_ok consumes itself exactly like the real
// app_book_ai_def_full(), so the gatekeeper can assert "the opening flow takes
// FULL and the next one falls back to PAGE" without running app_book itself.
static bool ai_def_full_ok;
static bool ai_detail_active;
// 出卡观测点：挂到 current->present 上，只数 PAGE 呈现（AI 分支专用参数；
// 首帧 FULL 与 tick 尾部 NONE 不计）。返回 false 让 app_present 继续走常规
// 呈现路径，不改变被测行为。
// Card-present observation: hangs off current->present and counts only PAGE
// presents (the AI branch's argument; the startup FULL and tick-tail NONE are
// not counted). Returning false lets app_present continue down its normal
// path, changing nothing.
static bool ai_present(app_ctx_t *c, app_redraw_t r) {
    (void)c;
    if (r == APP_REDRAW_PAGE) ++ai_presents;
    // M2 Step9：FULL 单独计数——开详解的回流必须整屏打底（双 GC16 的 open 侧），
    // 与首帧 FULL 混在一起就分不清「谁触发的全刷」。
    // M2 Step9: count FULL separately - the detail-opening flow must lay a full
    // base (the open side of the double GC16); mixing it with the startup FULL
    // would hide who triggered the refresh.
    else if (r == APP_REDRAW_FULL) ++ai_full_presents;
    return false;
}
// M2 Step9：按键调用步记录。豁免守门要分辨「压下即调」与「抬手才调」——只数
// 次数两者都是 1，步号才能把 defer 语义与豁免语义分开。
// M2 Step9: record the loop step of each key call. The exemption gatekeeper
// must tell "called on press" from "called on release" - both count 1, so only
// the step number separates the deferred semantics from the exempted one.
static int key_steps[3];

static app_redraw_t long_key(app_ctx_t* c, int k) { assert(k==UI_KEY_2); ++long_keys; c->request_menu=true; return APP_REDRAW_NONE; }
static app_redraw_t response;
static bool request_on_touch, request_on_tick, menu_on_tick, menu_on_touch, cancel_clobber;
static bool home_on_touch, home_on_tick, home_on_key, start_second;
static int home_enters, home_renders, last_menu_leaf, rendered_leaf;
static const app_desc_t* menu_background;
static app_desc_t first, second;
int ui_nav_hit(uint16_t x,uint16_t y){return y>=UI_NAV_TOUCH_TOP&&y<1216&&x<684?x/171:-1;}
void ui_nav_request(app_ctx_t *ctx,int index){ctx->request_app=index==0?&first:&second;}
static bool full_tick, lock_due, font_due, shake_activity;
static int64_t time_offset, time_step;
static int du_areas, gl_areas;
static bool main_fast, main_water;
int app_settings_main_refresh_mode(void) {return main_water?APP_MAIN_REFRESH_WATER:main_fast?APP_MAIN_REFRESH_FAST:APP_MAIN_REFRESH_NORMAL;}
static int shelf_exit_arms;
static int nav_arms, nav_draws, nav_full_arms, nav_area_draws;
static const void *nav_armed;
static bool main_source_visible, main_target_visible;
static bool media_test, sd_font, saved_sd_font;
static bool mounted_steps[32], present_steps[32];
static int media_lost, media_ready, builtin_opens, font_opens, probes, loss_step, remounts;
static int cover_discards;  // B42：插拔边沿作废封面代次的次数 / hotplug discards
// 31b 第 1 条：三拍封送的可控桩（quiesce 是否成功、调用次数、解冻/毒卷标记次数）。
// 顺序证据用 remounts_before_quiesce 采样——事后问区分不出「先 quiesce 后 remount」
// 与「先 remount 后 quiesce」，与 commit 4 的 abort 采样同一条纪律。
// Three-beat controls. Ordering is sampled at call time, not asked afterwards -
// afterwards cannot tell before from after (same discipline as commit 4's abort).
static int quiesce_calls, thaw_calls, hold_calls, remounts_before_quiesce;
// 26.11.5：介质离开/插回次数（桩在文件后段，计数声明须在前）。
// 26.11.5: media detach/reattach counters (the stubs live further down).
static unsigned detach_calls, resume_calls;
static bool resume_ok = true;
static bool quiesce_ok;
// 2026-11-01 批复条件③：在第 N 拍「关掉那个永不关的 FILE*」（quiesce 转为成功）。
// 负值 = 本用例不恢复。/ Resume point for the never-closing-FILE* case.
static int release_resume_step;
int E0470_WAVEFORM;
int E0470_FOLLOW_WAVEFORM;
enum EpdDrawError update_display_area_diff_with(EpdiyHighlevelState*h,const void*w,int m,EpdRect a) {
    if (a.height==UI_QUICK_HEIGHT) {assert(a.x==0&&a.y==0&&a.width==684);++quick_du;}
    if (a.y==1120) assert(w==&E0470_FOLLOW_WAVEFORM&&m==MODE_DU&&a.height==48&&a.width==50);
    return update_display_area_with(h,w,m,a);
}
enum EpdDrawError update_display_area_full_with(EpdiyHighlevelState*h,const void*w,int m,EpdRect a) {
    assert(a.x==0&&a.y==0&&a.width==684&&a.height==UI_QUICK_HEIGHT);++quick_gl;
    return update_display_area_with(h,w,m,a);
}
void ui_quick_menu_draw(uint8_t *fb,bool wifi,bool bluetooth) {(void)fb;(void)wifi;(void)bluetooth;++quick_draws;}
bool app_settings_ble_turner(void) {return ble_enabled;}
void app_settings_set_ble_turner(bool enabled) {ble_enabled=enabled;}
void ble_pt_reset_failure(void) {++ble_resets;}
bool ble_pt_pop_key(ble_pt_event_t *e) {(void)e;return false;}
bool ble_pt_pop_raw(ble_pt_raw_t *e) {(void)e;return false;}
void read_pico_transfer_get_status(read_pico_transfer_status_t *s) {*s=transfer_status;}
int read_pico_transfer_get_saved_wifi(char *ssid,bool *configured) {strcpy(ssid,"saved");*configured=saved_wifi;return ESP_OK;}
int read_pico_transfer_start(const read_pico_transfer_cfg_t *cfg) {assert(cfg->mode==READ_PICO_TRANSFER_MODE_STA&&cfg->network_only);++wifi_starts;transfer_status.mode=cfg->mode;transfer_status.state=1;return ESP_OK;}
bool read_pico_transfer_try_stop_if_idle(void) {if(upload_busy)return false;++wifi_stops;transfer_status.state=0;return true;}
void ui_click_feedback_reset(void) {click_active=false;}
void ui_click_feedback_begin(uint8_t *fb) {(void)fb;ui_click_feedback_reset();}
bool ui_click_feedback_active(void) {return click_active;}
bool ui_click_feedback_press(uint8_t *fb,int x,int y,EpdRect *r) {(void)fb;if(!click_enabled||y!=1141)return false;(void)x;*r=(EpdRect){60,1120,50,48};click_active=true;++click_presses;return true;}
bool ui_click_feedback_release(uint8_t *fb,EpdRect *r) {(void)fb;if(!click_active)return false;click_active=false;++click_restores;*r=(EpdRect){60,1120,50,48};return true;}
bool ui_click_feedback_cancel_at(int x,int y) {return x>120||y!=1141;}
int cst836u_read(void* h, cst836u_touch_t* t) {
    (void)h;
    sample_t s = samples[step];
    *t = (cst836u_touch_t){ .touched=s.count>0, .count=s.count, .x=s.x, .y=s.y };
    t->points[0].active=s.count>0;
    t->points[0].x=(uint16_t)s.x; t->points[0].y=(uint16_t)s.y;
    if (s.count>=2) { t->points[1].active=true; t->points[1].x=(uint16_t)s.x1; t->points[1].y=(uint16_t)s.y1; }
    return s.error;
}
int cst836u_get_info(void* h,cst836u_info_t* i) {(void)h;(void)i;return 0;}
// M 批 D/C 插点先问 INT 电平：脚本里「本拍有触点」即低电平，其余高电平。
// 与 cst836u_read 同源，所以插点看到的电平与主循环看到的手指永远一致。
// D/C hook asks the INT level first: low exactly when the scripted sample has contact,
// same source as cst836u_read, so the hook and the loop can never disagree.
int cst836u_int_level(void* h) {(void)h;return samples[step].count>0?0:1;}
const char* esp_err_to_name(int e) {(void)e;return "test";}
int64_t esp_timer_get_time(void) {return time_offset+(int64_t)(step+1)*time_step;}
void vTaskDelay(int ms) {(void)ms;if (++step>=sample_count) longjmp(done,1);}
bool continuous_du_init(void) {return true;}
void display_request_full_refresh(void) {++wake_clean_requests;}
void guard_draw_result(EpdiyHighlevelState* h,enum EpdDrawError e) {(void)h;assert(e==0);}
enum EpdDrawError update_display_area_with(EpdiyHighlevelState*h,const void*w,int m,EpdRect a) {(void)h;(void)w;(void)a;nav_area_draws+=nav_armed!=NULL;nav_armed=NULL;if(m==MODE_DU)du_areas++;else if(m==MODE_GL16)gl_areas++;return 0;}
enum EpdDrawError update_display_full(EpdiyHighlevelState*h) {(void)h;fulls++;nav_full_arms+=nav_armed!=NULL;nav_armed=NULL;return 0;}
enum EpdDrawError update_display_mode(EpdiyHighlevelState*h,int m) {(void)h;(void)m;mode++;return 0;}
enum EpdDrawError update_display_mode_diff(EpdiyHighlevelState*h,int m) {(void)h;(void)m;mode++;return 0;}
enum EpdDrawError update_display_fast_page(EpdiyHighlevelState*h) {(void)h;mode++;nav_draws+=nav_armed!=NULL;nav_armed=NULL;return 0;}
enum EpdDrawError update_display_white(EpdiyHighlevelState*h) {(void)h;return 0;}
bool display_take_white_exit(void) {return false;}
void display_main_transition_cancel(void) {nav_armed=NULL;}
void display_main_transition_arm(const void* owner, bool shelf_exit, bool changing_page) {(void)changing_page;display_main_transition_cancel();nav_armed=owner;++nav_arms;shelf_exit_arms+=shelf_exit;}
void display_main_transition_disarm(void) {nav_armed=NULL;}
void rails_idle_check(int64_t n) {(void)n;}
// 26.11.16 电量修复批：差刷路径要画那一小块，宿主不需要真像素。
void ui_fill_rect_fast(EpdRect r, uint8_t g, uint8_t* fb) {(void)r;(void)g;(void)fb;}
void ui_nav_status(uint8_t* fb) {(void)fb;}
// 26.11.18 节拍批：主循环取分钟走这个接口。宿主默认「未校时」(-1)，分钟支不触发，
// 现有用例逐条不变；要测整条刷新时把 host_clock_minute 设成一个 epoch 分钟。
// Host clock: default -1 (uncalibrated) keeps every existing case unchanged; set it to a
// real epoch minute to exercise the strip path.
int64_t host_clock_minute = -1;
int64_t ui_nav_clock_minute(void) { return host_clock_minute; }
static pmu_snapshot_t g_pmu = {true, 80, false};
const pmu_snapshot_t *read_pico_pmu_get(void) {return &g_pmu;}
int pmu_battery_percent(const pmu_snapshot_t *s) {return s->soc;}
bool pmu_battery_charging(const pmu_snapshot_t *s) {return s->charging;}
// 26.11.19 解耦批：采样支要能在「无按键」下独立跑，故 ready 与按键结果都可配。
// 默认值保持老行为（ready 跟 lock_due 走、按键恒 SHORT），既有用例逐条不变。
// 26.11.19: the sampling branch must run with no keys, so both ready and the key
// result are configurable. Defaults keep the old behaviour; existing cases unchanged.
bool pmu_ready_always;
read_pico_pmu_key_action_t pmu_key_action = READ_PICO_PMU_KEY_SHORT;
bool read_pico_pmu_ready(void) {return pmu_ready_always||lock_due||step==power_trigger_step;}
read_pico_pmu_key_action_t read_pico_pmu_take_key_action(uint16_t* held_out) {
    if(held_out)*held_out=100;
    if(step==power_trigger_step) return READ_PICO_PMU_KEY_LONG;
    return pmu_key_action;
}
// 26.11.19 解耦批：采样支直接调到它。计数给 K1 用（无按键时仍按 200 ms 推进），
// 桩不做真 I2C，也不改 g_pmu——值链的「新不新鲜」在真机上验，宿主只验节拍存在。
// 26.11.19: the sampling branch calls this directly. The count feeds K1 (cadence
// without keys). The shim does no real I2C and leaves g_pmu alone - freshness is
// a hardware question; the host only proves the cadence exists.
unsigned pmu_poll_calls;
int64_t pmu_poll_at[64];
esp_err_t read_pico_pmu_poll(void) {if(pmu_poll_calls<64)pmu_poll_at[pmu_poll_calls]=esp_timer_get_time();++pmu_poll_calls;return ESP_OK;}
void enter_lock_and_sleep(EpdiyHighlevelState*h,int64_t*t,void*a,void*tp,bool reader) {(void)tp;(void)h;(void)t;(void)a;(void)reader;assert(!nav_armed);++idle_locks;lock_due=false;time_offset+=1000000;}
void app_lock_feedback(EpdiyHighlevelState*h) {(void)h;++lock_feedback_calls;if(feedback_enabled){assert(!before_locks&&!idle_locks);++lock_badges;}}
void app_lock_wait_key_idle(int ms) {(void)ms;}
esp_err_t app_enter_host_sleep(app_sleep_mode_t mode) {assert(mode==APP_SLEEP_OFF);++shutdowns;longjmp(done,1);}
void app_restart_host(void) {++restarts;longjmp(done,1);}
void epd_poweroff(void) {}
const char* app_settings_font_path(void) {return "builtin";}
bool app_settings_reader_power_turn(void) {return false;}
bool app_settings_staged_shutdown(void) {return staged_shutdown;}
bool ttf_font_path_is_builtin(const char*p) {(void)p;return !font_due&&!saved_sd_font;}
bool ttf_font_ready(void) {return true;}
bool ttf_font_is_builtin(void) {return !sd_font;}
const char* ttf_font_path(void) {return "other";}
int ttf_font_open(const char*p) {(void)p;font_opens++;sd_font=true;return 0;}
int ttf_font_open_builtin(void) {assert(media_lost>0);builtin_opens++;sd_font=font_due=false;return 0;}
// 26.9.17 T5：与 `read_pico_sd_get_info()` 的分支顺序对齐——`!present` 优先返回
// NOT_FOUND，只有「卡在位且没挂载」才返回 INVALID_STATE。旧 stub 从不返回
// NOT_FOUND，于是「拔卡其实是 NOT_FOUND、不是 INVALID_STATE」这个关键事实
// 在宿主侧从未被验证过，`!*invalidated` 那道自锁门也就没人拦得住。
// 26.9.17 T5: mirror the real branch order - `!present` yields NOT_FOUND first,
// and only "card in place, not mounted" yields INVALID_STATE. The old stub never
// returned NOT_FOUND, so the fact that a removal is NOT_FOUND rather than
// INVALID_STATE was never exercised here, and nothing stopped the `!*invalidated`
// deadlock from landing.
// P1-1：判死之后，快照必须是「卡在（CD 的事实）、卷不可用（I/O 的事实）」。
// 桩复刻这条契约，不复制判据本身——计数与墙钟的真身在 sd_probe_host_test。
static bool io_dead;
// 26.9.31a P0-1(e)：硬错误（超时等）走看门狗；INVALID_STATE 仍走兜底探测，
// 两条路必须分开测——把它们混成一个码，「看门狗在不在」就测不出来。
// 26.9.31a: hard errors (timeouts) exercise the watchdog; INVALID_STATE still goes through
// the fallback probe. Keep them distinct or the watchdog can never be observed here.
static bool sd_hard_error;
int read_pico_sd_get_info(read_pico_sd_info_t*i) {
    // 合流后两条桩语义叠在同一函数上，判序有意义：**判死优先**。
    // I/O 判死那一刻，CD 的事实（present=true）与卷的事实（mounted=false）同时成立，
    // 后面那些「按步表取值」的分支说的都是判死之前的稳态。31a 的 sd_hard_error
    // 只影响「卡在位、没挂载」那一条返回值，两者不争同一个分支。
    // Merged: the I/O-death stub wins. At the death tick both facts hold (CD present,
    // volume gone); sd_hard_error only changes the "present but unmounted" return.
    // 合流修正（§13.12a 靶子为假）：旧桩这里 `return ESP_ERR_NOT_FOUND`，而真件在判死后
    // 返回的是 **INVALID_STATE**——`observe_media_locked()` 在锁存期把快照重写成
    // `{present=1, mounted=0, error=INVALID_STATE}`（sd_probe 实测 259）。
    // 差一个码的后果：`app_loop` 的兜底分支写 `err != ESP_ERR_NOT_FOUND`，
    // 桩喂 NOT_FOUND 时它不进兜底，于是「判死之后看门狗还来不来」这条路径
    // 在宿主侧从来没被走到过。桩按真件改正，那条路径才第一次被测到。
    // The old stub returned NOT_FOUND where the component returns INVALID_STATE
    // (observe_media_locked rewrites the snapshot while latched). app_loop's fallback
    // tests `err != ESP_ERR_NOT_FOUND`, so the stub skipped it and "does the watchdog
    // still touch the card after death" was never exercised on the host side.
    if (io_dead) { i->present = true; i->mounted = false; return ESP_ERR_INVALID_STATE; }
    i->present = media_test ? present_steps[step] : font_due;
    i->mounted = media_test ? mounted_steps[step] : font_due;
    if (!i->present) return ESP_ERR_NOT_FOUND;
    if (i->mounted) return ESP_OK;
    return sd_hard_error ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
}
int read_pico_sd_start_probe(void) {probes++;return ESP_ERR_NOT_FINISHED;}
// S26-4：摘抄挂载让行的接线取证。**只看边沿**——插卡那一拍叫一次，之后稳定挂载
// 一拍都不许再叫（每拍都叫等于每拍把窗口往后推，队列永远等不到能落盘的那一拍）。
// S26-4: evidence that a mount arms the excerpt yield window. Edge only: once when the
// card arrives, never again while it stays mounted (re-arming every tick would push the
// window forward forever and the queue would never flush).
// E-2（§19.4）：让行的锚点从状态边沿换成挂载代号。桩替身必须同时复刻两件事——
// **每拍都调**（调用点不变）与**同代号不重开窗口**（幂等在被调方）。只复刻前者，
// 断言就退化成「函数被调用过」；只复刻后者，就看不见调用频率。两个数分开记：
// `excerpt_mount_calls` = 调用次数（应当每拍递增），`excerpt_yield_rearms` = 真正开窗次数
// （应当只在新代号上 +1）。幂等语义的真身在 excerpt_store_host_test 的 case_mount_yield
// 里独立验证，这里只是把它当契约复刻，不是第二份实现。
// E-2: the stub reproduces both halves of the contract - called every turn, re-arms only
// on a new epoch. Call count and re-arm count are separate on purpose; the real idempotence
// lives in excerpt_store and is verified by its own host case.
static int excerpt_mount_calls, excerpt_mount_step, excerpt_yield_rearms;
static uint32_t excerpt_mount_at, excerpt_yield_epoch;
// 组件侧代号桩：按拍查表，用例用它表达「第 N 拍挂载，之后在位」。
// 32 格：P1-1 那条用例要 16 拍（跨看门狗 8 s 档），8 格装不下——扩窗时 ASAN 当场
// 报了 global-buffer-overflow，这是「改测试窗口忘了改桩数组」的现成教材。
// 32 slots: the P1-1 case now runs 16 ticks to reach the watchdog's 8 s rung.
static uint32_t epoch_steps[32];
uint32_t read_pico_sd_mount_epoch(void) {return media_test ? epoch_steps[step] : 0u;}
// P1-1 接线：①每拍都被调（计数）；②第 `io_dead_after` 次调用返回一次 NOT_FOUND，
// 此后 get_info 落成「卡在、卷不可用」。两次失败之间的真实节流不在这里复刻。
static int io_watch_calls, io_dead_after = -1;
int read_pico_sd_watch_io(uint8_t *fails, uint16_t *ms) {
    ++io_watch_calls;
    if (io_dead_after <= 0 || io_watch_calls < io_dead_after)
        return (media_test && mounted_steps[step]) ? ESP_OK : ESP_ERR_INVALID_STATE;
    io_dead_after = 0;
    io_dead = true;
    fprintf(stderr,"DEAD step=%d calls=%d mounted_step=%d\n",step,io_watch_calls,(int)mounted_steps[step]);
    if (fails) *fails = 8u;
    if (ms) *ms = 940u;
    return ESP_ERR_NOT_FOUND;
}
void excerpt_store_mount_yield(uint32_t mount_epoch, uint32_t now_ms) {
    ++excerpt_mount_calls;
    if (mount_epoch == excerpt_yield_epoch) return;   // 幂等：同代号不重开
    excerpt_yield_epoch = mount_epoch;
    ++excerpt_yield_rearms;
    excerpt_mount_step = step;
    excerpt_mount_at = now_ms;
}
int read_pico_sd_remount(void) {remounts++;return 0;}
static void lost(app_ctx_t*c) {(void)c;media_lost++;loss_step=step;fprintf(stderr,"LOST cb step=%d\n",step);}
static void ready(app_ctx_t*c) {(void)c;media_ready++;}
EpdRect ui_content_refresh_area(void) {return (EpdRect){0,0,684,1000};}
const app_desc_t* app_home_page(void) {return &first;}
const app_desc_t* app_at(int i) {return i==0?&first:i==1?&second:NULL;}
int app_index_of(const app_desc_t* app) {return app==&first?0:app==&second?1:-1;}
int ui_key_hit_test(uint16_t x,uint16_t y) {return y>=1300 && x<480?x/160:-1;}
bool ui_menu_handle_hit_test(uint16_t x,uint16_t y) {return x>600 && y>1100 && y<1300;}
int ui_menu_leaf_for_app(const app_desc_t*a) {return a==&second?1:0;}
int ui_menu_leaf_count(void) {return 2;}
void ui_draw_menu_page(uint8_t*f,const app_desc_t*a,int l) {(void)f;menu_background=a;last_menu_leaf=l;menus++;}
int ui_menu_hit_test(uint16_t x,uint16_t y,int l) {(void)l;if(x==550&&y==500)return UI_MENU_HIT_NEXT;return x<500&&y>=100&&y<300?(y-100)/100:-1;}
bool ui_menu_row_rect(int l,int r,EpdRect*out) {(void)l;*out=(EpdRect){0,r*100+100,500,100};return r>=0&&r<2;}
void ui_draw_menu_row_pressed(uint8_t*f,const app_desc_t*a,int l,int r,bool p) {(void)f;(void)a;(void)l;(void)r;if(p)highlights++;else restores++;}
static void enter(app_ctx_t*c) {(void)c;enters++;}
static void leave(app_ctx_t*c) {(void)c;exits++;}
static void render(app_ctx_t*c,uint8_t*f) {(void)c;(void)f;}
// 模拟 app_book render() 的旋转自设（app_book.c:3252 的 want 计算）：阅读视图
// 处于横屏态时把物理旋转设回横屏。守门用例二用它断言取消路径确实重走 render()
// 而不是帧缓冲区域恢复式（后者旋转停留竖屏而像素是横屏内容，触摸映射错乱）。
// Mimics app_book render()'s rotation self-set (the want computation at
// app_book.c:3252): a landscape reading view sets the panel rotation back to
// landscape. Gatekeeper 2 uses it to assert the cancel path really repaints
// through render() instead of restoring a framebuffer region (which would
// leave rotation portrait over landscape pixels and misalign touch).
static int landscape_renders;
static void landscape_render(app_ctx_t*c,uint8_t*f) {(void)c;(void)f;++landscape_renders;epd_set_rotation(EPD_ROT_LANDSCAPE);}
static void home_enter(app_ctx_t*c) {c->leaf=0;enters++;home_enters++;}
static void home_render(app_ctx_t*c,uint8_t*f) {(void)f;rendered_leaf=c->leaf;home_renders++;}
static app_redraw_t touch(app_ctx_t*c,const cst836u_touch_t*t) {
    (void)t;touch_calls++;assert(c->pressed&&c->consumed);
    if(request_on_touch)c->request_app=&second;
    if(menu_on_touch)c->request_menu=true;
    if(home_on_touch)c->request_return=true;
    return response;
}
static app_redraw_t gesture(app_ctx_t*c,const ui_gesture_event_t*e) {
    events[e->type]++;
    if(e->type==UI_GESTURE_CANCEL&&cancel_clobber) {c->request_app=NULL;c->request_menu=true;c->request_return=false;}
    if(e->type==UI_GESTURE_PRESS&&request_on_touch)c->request_app=&second;
    if(e->type==UI_GESTURE_PRESS&&home_on_touch)c->request_return=true;
    return response;
}
// 双指事件计数：幽灵 TAP 用例要确认 RETRACT 真的到了页面（撤销来自它，不是巧合）。
// Two-finger event counter: the ghost TAP case must confirm RETRACT really reached the
// page - the revocation comes from it, not from luck.
static int dual_events[8], dual_aborts;
static app_redraw_t dual_ev(app_ctx_t*c,const ui_dual_event_t*e) {
    (void)c;
    if(e->type>=0&&e->type<8) dual_events[e->type]++;
    if(e->abort!=UI_DUAL_ABORT_NONE) dual_aborts++;
    return APP_REDRAW_NONE;
}
static app_redraw_t key(app_ctx_t*c,int k) {keys[k]++;key_steps[k]=step;if(home_on_key)c->request_return=true;return APP_REDRAW_NONE;}
// 记录 TAP 的逻辑坐标，供横屏旋转公式做端到端断言。
// Record TAP logical coords for end-to-end landscape rotation assertions.
static int tap_x0, tap_y0, tap_x, tap_y;
static app_redraw_t coord_gesture(app_ctx_t*c,const ui_gesture_event_t*e) {
    (void)c;
    events[e->type]++;
    if(e->type==UI_GESTURE_TAP){tap_x0=e->x0;tap_y0=e->y0;tap_x=e->x;tap_y=e->y;}
    return APP_REDRAW_NONE;
}
static app_redraw_t tick(app_ctx_t*c) {
    ticks++;tick_consumed[step]=c->consumed;
    if (release_resume_step == step) quiesce_ok = true;
    if (shake_activity && step == 0) c->user_activity = true;
    if(request_on_tick)c->request_app=&second;
    if(menu_on_tick)c->request_menu=true;
    if(home_on_tick){c->request_return=true;home_on_tick=false;}
    return full_tick ? APP_REDRAW_FULL : APP_REDRAW_NONE;
}
static app_redraw_t consume_power(app_ctx_t*c) {(void)c;lock_due=false;return APP_REDRAW_DONE;}
static void reset(void) {
    power_open_draws=power_controls_draws=power_final_draws=shutdowns=restarts=0;power_trigger_step=-1;staged_shutdown=false;
    ble_enabled=saved_wifi=upload_busy=click_enabled=click_active=false;
    transfer_status=(read_pico_transfer_status_t){0};
    quick_draws=quick_gl=quick_du=wifi_starts=wifi_stops=ble_resets=click_presses=click_restores=0;
    push_armed=false;push_cancel_calls=0;push_cancel_why[0]='\0';
    ai_take_hit=false;ai_ready_ok=true;ai_presents=ai_ready_calls=ai_full_presents=0;
    scene_claim_calls=0;scene_claim_ok=false;scene_claim_word[0]=0;vocab_stub_set_def_calls=0;
    ai_def_full_ok=false;ai_detail_active=false;
    ai_seen_word[0]=ai_seen_def[0]='\0';
    main_fast=main_water=false;shelf_exit_arms=0;idle_minutes=0;idle_locks=before_locks=lock_feedback_calls=lock_badges=wake_clean_requests=0;feedback_enabled=false;online_busy=false;
    nav_arms=nav_draws=nav_full_arms=nav_area_draws=0;nav_armed=NULL;
    main_source_visible=main_target_visible=true;
    media_test=sd_font=saved_sd_font=false;sd_hard_error=false;media_lost=media_ready=builtin_opens=font_opens=probes=0;remounts=0;loss_step=-1;
    quiesce_calls=thaw_calls=hold_calls=0;remounts_before_quiesce=-1;quiesce_ok=true;release_resume_step=-1;
    detach_calls=resume_calls=0;resume_ok=true;
    excerpt_mount_calls=excerpt_mount_step=excerpt_yield_rearms=0;excerpt_mount_at=excerpt_yield_epoch=0;
    io_dead=false;io_watch_calls=0;io_dead_after=-1;cover_discards=0;
    for(int ei=0;ei<32;++ei) epoch_steps[ei]=0;
    memset(mounted_steps,0,sizeof(mounted_steps));memset(present_steps,0,sizeof(present_steps));
    long_keys=0;pmu_poll_calls=0;pmu_ready_always=false;pmu_key_action=READ_PICO_PMU_KEY_SHORT;memset(pmu_poll_at,0,sizeof(pmu_poll_at));
    landscape_renders=0;host_rotation=EPD_ROT_INVERTED_PORTRAIT;tap_x0=tap_y0=tap_x=tap_y=-1;
    home_on_touch=home_on_tick=home_on_key=start_second=false;
    home_enters=home_renders=0;rendered_leaf=-1;last_menu_leaf=-1;menu_background=NULL;
    full_tick=lock_due=font_due=shake_activity=false;time_offset=0;time_step=10000;du_areas=gl_areas=0;
    memset(dual_events,0,sizeof(dual_events));dual_aborts=0;
    memset(samples,0,sizeof(samples));memset(keys,0,sizeof(keys));memset(key_steps,0,sizeof(key_steps));memset(events,0,sizeof(events));memset(tick_consumed,0,sizeof(tick_consumed));
    sample_count=step=ticks=enters=exits=touch_calls=menus=highlights=restores=fulls=mode=0;
    request_on_touch=request_on_tick=menu_on_tick=menu_on_touch=cancel_clobber=false;response=APP_REDRAW_NONE;
    first=(app_desc_t){.title="first",.render=render,.on_enter=enter,.on_exit=leave,.on_touch=touch,.on_tick=tick,.on_key=key};
    second=(app_desc_t){.title="second",.render=render,.on_enter=enter};
}
static void add(int x,int y,int count,int error) {samples[sample_count++]=(sample_t){x,y,count,error,0,0};}
// 双指样本：第一点 (x0,y0)、第二点 (x1,y1)。/ Two-finger sample at both points.
static void add2(int x0,int y0,int x1,int y1) {samples[sample_count++]=(sample_t){x0,y0,2,0,x1,y1};}
// 27 起底栏命中走 40ms 收敛窗（8×5ms），窗内不切页、窗后才写一次槽；
// 宿主一拍就是一个 tick，所以每个导航用例末尾要补够空拍，否则断言测的是「没走完的窗」。
// Since 27 a bottom-bar hit settles over 8 ticks, so every nav case needs idle ticks
// after the press - otherwise the assertion measures an unfinished window.
static void idle(int n) {while(n-- > 0) add(0,0,0,0);}
static void run(void) {app_loop_config_t cfg={.first_app=start_second?&second:&first};if(!setjmp(done))app_loop_run(&cfg);}

// ---- 26.11.19 解耦批：K1 采样节拍（无按键也刷新）----
// 这组用例咬的是「值链的节拍归主循环」这件事本身：按键恒 NONE 时采样照走，
// 且相邻间隔就是 200 ms。解耦之前这里会是 0 次——因为采样寄生在取键里。
// K1: the value chain keeps its own cadence. With no key at all, sampling still
// advances every 200 ms; before decoupling this count would be zero.
static void pmu_bus_cases(void) {
    // K1-a：在总线上、无按键、每拍 100 ms。
    reset();
    pmu_ready_always = true;
    pmu_key_action = READ_PICO_PMU_KEY_NONE;
    time_step = 100000;                 // 100 ms/拍
    sample_count = 13;
    run();
    assert(pmu_poll_calls >= 5);        // 1.2 s 内至少 5 次
    for (unsigned i = 1; i < pmu_poll_calls; i++) {
        const int64_t gap = pmu_poll_at[i] - pmu_poll_at[i - 1];
        assert(gap >= 200000 && gap <= 300000);   // 一节拍 = 200 ms（容一拍抖动）
    }

    // K1-b：PMU 不在总线上（ready 假）→ 零采样。门是门，不是「有枣没枣打一杆」。
    reset();
    pmu_key_action = READ_PICO_PMU_KEY_NONE;
    time_step = 100000;
    sample_count = 13;
    run();
    assert(pmu_poll_calls == 0);
}
static app_redraw_t consumed_return_tick(app_ctx_t*c) { if(step==0){c->leaf=7;c->request_app=&second;}else c->request_return=true;return APP_REDRAW_NONE; }
static app_redraw_t return_tick(app_ctx_t*c) { c->request_return=true; return APP_REDRAW_NONE; }
static app_redraw_t origin_tick(app_ctx_t*c) { c->leaf=7; if(step==0 && request_on_tick)c->request_app=&second; return APP_REDRAW_NONE; }
// ---- 26.11.17 首页手势批：接线 desc（与 app_dashboard.c 的胶水同形）----
// 中文：这里刻意不直调判定函数，而是把它挂在 desc 上，由真 app_loop 用合成拍喂
// 真识别器——「接线本身」也是被测对象。判定函数本身在 home_gesture.c，页面侧只剩
// 三行 switch，那三行由 check_home_gesture.py 的结构牙钉住。
// English: the decider is attached to a desc on purpose - the real loop feeds the real
// recognizer with synthetic ticks, so the wiring itself is under test. The page keeps a
// three-line switch, pinned by check_home_gesture.py's structural tooth.
static int hg_view;              // 模拟 s_view / mirrors s_view
static home_dual_seq_t hg_seq;
static bool hg_push_ready;       // 推件箱是否有内容 / whether push.txt is openable
static int hg_open_push, hg_fallback, hg_transfer_opens, hg_component_opens, hg_taps;
static int hg_steps_events, hg_last_steps, hg_dual_calls, hg_last_type;

static app_redraw_t hg_dual(app_ctx_t *c, const ui_dual_event_t *e) {
    (void)c;
    ++hg_dual_calls; hg_last_type = (int)e->type;
    if (e->type == UI_DUAL_EVT_FONT_STEP) { ++hg_steps_events; hg_last_steps = e->steps; }
    if (home_dual_wants_push(hg_view, &hg_seq, e)) {
        if (home_push_route(hg_push_ready) == HOME_PUSH_OPEN) ++hg_open_push;
        else ++hg_fallback;                 // 诚实失败：送到能推书的地方
    }
    return APP_REDRAW_NONE;
}
static app_redraw_t hg_gesture(app_ctx_t *c, const ui_gesture_event_t *e) {
    (void)c;
    if (e->type >= 0 && e->type < 16) events[e->type]++;
    if (e->type == UI_GESTURE_TAP) { ++hg_taps; return APP_REDRAW_NONE; }
    if (e->type != UI_GESTURE_LONG_PRESS) return APP_REDRAW_NONE;
    switch (home_long_decide(hg_view, e->x0, e->y0)) {
        case HOME_LONG_COMPONENT: ++hg_component_opens; break;
        case HOME_LONG_TRANSFER: ++hg_transfer_opens; break;
        default: break;
    }
    return APP_REDRAW_NONE;
}
static void hg_setup(void) {
    reset();
    start_second = false;   // run() 必须用 first（挂着手势回调的那一个）
    hg_view = HOME_VIEW_ROOT; hg_push_ready = true;
    hg_seq.fired = false;
    hg_open_push = hg_fallback = hg_transfer_opens = hg_component_opens = hg_taps = 0;
    hg_steps_events = 0; hg_last_steps = 0; hg_dual_calls = 0; hg_last_type = -1;
    first.on_gesture = hg_gesture;
    first.on_dual_gesture = hg_dual;
}
// 一次双指上滑的合成拍（dy = -200，每拍 100 ms → 0.67 px/ms，过速度门）。
// Synthetic two-finger swipe up: dy = -200 over 300 ms = 0.67 px/ms, past the speed gate.
static void hg_swipe_up(void) {
    time_step = 100000;   // 微秒：100 ms/拍（esp_timer_get_time 的单位是 us）
    add(200, 700, 1, 0);
    add2(200, 700, 300, 700);   // 双指落定 → RETRACT
    add2(200, 600, 300, 600);
    add2(200, 500, 300, 500);
    add(0, 0, 0, 0);            // 抬手判定 → FONT_STEP steps=-1
    add(0, 0, 0, 0);
}
static void home_gesture_cases(void) {
    // D1 方向锚：上滑必须是 steps < 0，且打开动作恰好一次。
    // D1 direction anchor: an upward swipe must carry steps < 0 and open exactly once.
    hg_setup();
    hg_swipe_up();
    run();
    assert(hg_steps_events == 1);
    assert(hg_last_steps < 0);
    assert(hg_open_push == 1 && hg_fallback == 0);

    // D2 序列去重：同一接触序列里的第二个 FONT_STEP 必须落空（识别器重判时的保护）。
    // D2 latch: a second FONT_STEP in the same contact sequence must not open twice.
    {
        home_dual_seq_t seq = {0};
        ui_dual_event_t ev = {0};
        ev.type = UI_DUAL_EVT_FONT_STEP; ev.steps = -1;
        assert(home_dual_wants_push(HOME_VIEW_ROOT, &seq, &ev));
        ev.steps = -2;   // 长滑吐出更大档位，仍是同一次动作
        assert(!home_dual_wants_push(HOME_VIEW_ROOT, &seq, &ev));
        assert(seq.fired);
    }

    // D3 下滑 / 横滑 / RETRACT 各一拍：零动作（RETRACT 只清序列）。
    // D3 downward swipe, horizontal swipe, RETRACT: no action; RETRACT only clears.
    hg_setup();
    time_step = 100000;   // 微秒：100 ms/拍（esp_timer_get_time 的单位是 us）
    add(200, 500, 1, 0);
    add2(200, 500, 300, 500);
    add2(200, 600, 300, 600);
    add2(200, 700, 300, 700);   // 下滑 dy=+200 → steps>0
    add(0, 0, 0, 0);
    add(0, 0, 0, 0);
    run();
    assert(hg_steps_events == 1 && hg_last_steps > 0 && hg_open_push == 0);

    hg_setup();
    time_step = 100000;   // 微秒：100 ms/拍（esp_timer_get_time 的单位是 us）
    add(200, 600, 1, 0);
    add2(200, 600, 300, 600);
    add2(500, 600, 600, 600);
    add2(700, 600, 800, 600);   // 横滑 → ORIENT
    add(0, 0, 0, 0);
    add(0, 0, 0, 0);
    run();
    assert(hg_open_push == 0 && hg_steps_events == 0);

    {
        home_dual_seq_t seq = {0};
        ui_dual_event_t ev = {0};
        seq.fired = true;
        ev.type = UI_DUAL_EVT_RETRACT;
        assert(!home_dual_wants_push(HOME_VIEW_ROOT, &seq, &ev));
        assert(!seq.fired);   // 序列边界清掉了本序列的触发记录
    }

    // D4 轻碰（<40 px）→ 抬净 → 单击立即可达。双指合并窗内不得产生 FONT_STEP。
    // D4 a light touch (under 40 px) then a tap: no FONT_STEP, and the tap lands.
    hg_setup();
    time_step = 100000;   // 微秒：100 ms/拍（esp_timer_get_time 的单位是 us）
    add(200, 600, 1, 0);
    add2(200, 600, 300, 600);
    add2(210, 600, 310, 600);   // 10 px，远小于 40 px 死区
    add(0, 0, 0, 0);
    add(0, 0, 0, 0);
    add(200, 400, 1, 0);
    add(0, 0, 0, 0);
    add(0, 0, 0, 0);
    run();
    assert(hg_steps_events == 0 && hg_open_push == 0);
    assert(hg_taps == 1);

    // D5 推件箱为空：走回退（传书页），不静默、不崩。
    // D5 empty inbox: fall back to the transfer page instead of answering with silence.
    hg_setup();
    hg_push_ready = false;
    hg_swipe_up();
    run();
    assert(hg_open_push == 0 && hg_fallback == 1);

    // D6 负例：不足阈值的按住不触发——这条把「阈值真的存在」钉住（改成 1 ms 就红）。
    // D6 negative: a hold under the threshold does nothing, pinning that a threshold exists.
    hg_setup();
    time_step = 100000;
    add(400, 1020, 1, 0);
    add(400, 1020, 1, 0);
    add(400, 1020, 1, 0);   // 300 ms
    add(0, 0, 0, 0);
    add(0, 0, 0, 0);
    run();
    assert(hg_transfer_opens == 0 && events[UI_GESTURE_LONG_PRESS] == 0);

    // D6 空白长按 ≥ 阈值：打开传书页恰好一次。
    // D6 long press on blank: opens the transfer page exactly once.
    hg_setup();
    time_step = 100000;   // 微秒：100 ms/拍（esp_timer_get_time 的单位是 us）
    for (int i = 0; i < 7; ++i) add(400, 1020, 1, 0);   // 空白：避开五热区与 tab
    add(0, 0, 0, 0);
    run();
    assert(hg_transfer_opens == 1 && hg_component_opens == 0);

    // D7 五热区中心 + 一个边界点：只走各自的原动作，零误触传书页。
    // 左卡的原动作是组件浏览，其余四块原本就没有长按动作。
    // D7 the five hot zones plus one edge point: each keeps its own action, never transfer.
    {
        struct { int x, y; int expect_transfer, expect_component; } pts[] = {
            {342, 369, 0, 0},  // 继续阅读卡中心
            {342, 603, 0, 0},  // 推送卡中心
            {579, 118, 0, 0},  // 右上角（包络框）中心
            {150, 40, 0, 0},   // 状态栏信息带（时钟）：26.11.17 收窄后不算空白
            {620, 40, 0, 0},   // 状态栏信息带（电量笔）
            {342, 84, 0, 0},   // 信息带下沿上方 4 px（仍在带内）
            {184, 822, 0, 1},  // 左卡中心 → 组件浏览
            {501, 822, 0, 0},  // 右卡中心
            {24, 172, 0, 0},   // 继续卡左上边界（含）
            {24, 1004, 0, 1},  // 左卡左下边界（含，y=640+365-1）
        };
        for (unsigned i = 0; i < sizeof(pts) / sizeof(pts[0]); ++i) {
            hg_setup();
            time_step = 100000;   // 微秒：100 ms/拍（esp_timer_get_time 的单位是 us）
            for (int k = 0; k < 7; ++k) add(pts[i].x, pts[i].y, 1, 0);
            add(0, 0, 0, 0);
            run();
            assert(hg_transfer_opens == 0);
            assert(hg_component_opens == pts[i].expect_component);
        }
    }

    // D8 网格采样：热区内全 false、空白带全 true、信息带全 false（用产品常量，
    // 不是测试自己造的表）。信息带在 26.11.17 收窄后单列——它既不是热区，
    // 也不许算空白，所以不能并进 blank_false 那一格。
    // D8 grid sweep: false inside every hot zone, true on the blank belts, false on the
    // info band. After the 26.11.17 narrowing the band is its own class: neither hot nor
    // blank, so it must not be folded into the blank_false count.
    {
        int inside_true = 0, blank_false = 0, info_true = 0;
        for (int y = 0; y < 1080; y += 20) {
            for (int x = 0; x < 684; x += 20) {
                bool blank = home_blank_hit(HOME_VIEW_ROOT, x, y);
                bool hot = false;
                const EpdRect zones[] = {HOME_HIT_CONTINUE, HOME_HIT_PUSH, HOME_HIT_CORNER,
                                         HOME_HIT_LEFT, HOME_HIT_RIGHT};
                for (unsigned z = 0; z < sizeof(zones) / sizeof(zones[0]); ++z) {
                    const EpdRect r = zones[z];
                    if (x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height) hot = true;
                }
                const bool info = x >= HOME_RESERVED_INFO.x &&
                                  x < HOME_RESERVED_INFO.x + HOME_RESERVED_INFO.width &&
                                  y >= HOME_RESERVED_INFO.y &&
                                  y < HOME_RESERVED_INFO.y + HOME_RESERVED_INFO.height;
                if (hot && blank) ++inside_true;
                if (!hot && !info && !blank) ++blank_false;
                if (info && blank) ++info_true;
            }
        }
        assert(inside_true == 0 && blank_false == 0 && info_true == 0);
        assert(!home_blank_hit(HOME_VIEW_ROOT, 342, 1090));      // tab 带
        assert(!home_blank_hit(HOME_VIEW_ROOT, 40, 40));         // 状态栏：时钟
        assert(!home_blank_hit(HOME_VIEW_ROOT, 620, 40));        // 状态栏：电量笔
        assert(home_blank_hit(HOME_VIEW_ROOT, 400, 88));         // 带下沿第一行即空白
        assert(home_blank_hit(HOME_VIEW_ROOT, 400, 1020));
        assert(!home_blank_hit(HOME_VIEW_TIME, 400, 1020));      // 非主视图
    }

    // D9 长按计时中第二指落下：单指识别器被撤销，长按不发，页面无残留动作。
    // D9 a second finger lands mid-long-press: the recognizer is cancelled, the long
    // press never fires, and the page performs nothing.
    hg_setup();
    time_step = 100000;   // 微秒：100 ms/拍（esp_timer_get_time 的单位是 us）
    add(400, 1020, 1, 0);
    add(400, 1020, 1, 0);
    add(400, 1020, 1, 0);       // 300 ms，尚未到 500 ms
    add2(400, 1020, 500, 1020); // 第二指落下 → RETRACT → cancel_gesture
    add2(400, 1020, 500, 1020);
    add(0, 0, 0, 0);
    add(0, 0, 0, 0);
    run();
    assert(events[UI_GESTURE_CANCEL] == 1);
    assert(events[UI_GESTURE_LONG_PRESS] == 0);
    assert(hg_transfer_opens == 0);

    // D10 抬净后 200 ms 内单击：单指通道收到（抑制解除、不是永久饿死）。
    // D10 a tap within 200 ms of both fingers lifting: the single-finger path gets it.
    hg_setup();
    hg_swipe_up();
    add(200, 400, 1, 0);
    add(0, 0, 0, 0);
    add(0, 0, 0, 0);
    run();
    assert(hg_taps == 1);

    // D11 其他视图做同样手势：零动作（视图门控在判定函数里，不在页面里）。
    // D11 the same gesture on other views: nothing happens - the gate lives in the
    // decider, not in the page.
    const int views[] = {HOME_VIEW_TIME, HOME_VIEW_GOAL, HOME_VIEW_COMPONENT};
    for (unsigned i = 0; i < sizeof(views) / sizeof(views[0]); ++i) {
        hg_setup();
        hg_view = views[i];
        hg_swipe_up();
        run();
        assert(hg_open_push == 0 && hg_fallback == 0);
    }
    puts("PASS: 26.11.17 home gestures: up-swipe opens the newest push exactly once (steps<0 anchored), down/horizontal/RETRACT do nothing, an empty inbox falls back honestly, blank long-press opens the transfer page while five hot zones keep their own actions, and a tap lands right after both fingers lift");
}
static void home_case(void) {
    reset();start_second=true;
    second=first;second.title="transfer";
    first.on_enter=home_enter;first.render=home_render;first.on_tick=NULL;first.on_touch=NULL;
}
static bool product_menu_disabled(app_ctx_t *ctx) {(void)ctx;return false;}
static void before_lock(app_ctx_t *ctx) {(void)ctx;++before_locks;}
static bool main_first(app_ctx_t* ctx) {(void)ctx;return main_source_visible;}
static bool main_second(app_ctx_t* ctx) {(void)ctx;return main_target_visible;}
static void enter_subview(app_ctx_t* ctx) {enter(ctx);main_target_visible=false;}
static void enable_main_tabs(void) {main_fast=true;first.main_page_visible=main_first;second.main_page_visible=main_second;}
static app_redraw_t nav_return_tick(app_ctx_t* ctx) {
    if(step==0)ctx->request_app=&second;
    else if(step==1)ctx->request_return=true;
    return APP_REDRAW_NONE;
}
static app_redraw_t main_late_tick(app_ctx_t* ctx) {(void)ctx;return step == 1 ? response : APP_REDRAW_NONE;}
int main(void) {
    pmu_bus_cases();
    home_gesture_cases();
    // 第一帧报点立即切页，尚未抬手也能响应；同次停留和释放不重复。
    // The first contact switches before lift; its subsequent hold and release never repeat navigation.
    for(int policy=0;policy<3;++policy) {
        reset();enable_main_tabs();main_fast=policy==1;main_water=policy==2;
        add(200,UI_NAV_TOUCH_TOP,1,0);idle(10);run();assert(nav_draws==1&&enters==2&&!touch_calls);
        reset();enable_main_tabs();main_fast=policy==1;main_water=policy==2;
        add(200,1080,1,0);add(232,1092,1,0);add(232,1092,1,0);add(0,0,0,0);idle(10);run();
        assert(nav_draws==1&&enters==2&&!touch_calls);
        // 27 起收敛窗内的两指只认最后一指：两指只切一次（旧语义是各切一次、共 2 次）。
        reset();enable_main_tabs();main_fast=policy==1;main_water=policy==2;
        add(85,1080,1,0);add(0,0,0,0);add(200,1080,1,0);add(0,0,0,0);idle(10);run();
        assert(nav_draws==1&&enters==2&&!touch_calls);
        // 同一格不重复指：上游「tab != 当前索引」把同格指挡在窗外的，所以本窗最后一指
        // 仍是 Books；这一格测的是那条上游守卫没有因为仲裁层接入而失效。
        reset();enable_main_tabs();main_fast=policy==1;main_water=policy==2;
        add(200,1080,1,0);add(0,0,0,0);add(85,1080,1,0);add(0,0,0,0);idle(10);run();
        assert(nav_draws==1&&enters==2&&!touch_calls);
    }
    reset();enable_main_tabs();add(85,1080,1,0);add(0,0,0,0);run();assert(!nav_draws&&enters==1);
    reset();enable_main_tabs();first.on_gesture=gesture;add(200,1080,2,0);add(0,0,0,0);run();assert(!nav_draws);
    reset();enable_main_tabs();add(200,1080,1,-1);add(0,0,0,0);run();assert(!nav_draws);
    reset();enable_main_tabs();main_source_visible=false;first.on_gesture=gesture;
    add(200,1080,1,0);run();assert(!nav_draws&&enters==1);
    puts("PASS: root tabs act on first contact in all modes, enlarged top edge, one-shot holds, rapid taps, active-tab no-op, subview/multitouch/read-error guards");
    reset();add(10,1141,1,0);
    app_ctx_t loading={.tp=(void*)1};
    // 28 轮：D 层不再直接 ui_nav_request 写槽——命中折叠进仲裁单槽，由主循环尾部
    // 收敛时一次性写入唯一的家。所以这里断言「采到了、但此刻还没写槽」。
    assert(app_poll_main_navigation(&loading)&&!loading.request_app&&loading.user_activity);
    // 同一只按住的手再问一次不算新按下（自建边沿）：不写槽、不再喂仲裁。
    assert(!app_poll_main_navigation(&loading));
    // 抬手：INT 电平回高 → 边沿复位（真机走同一条分支）。
    samples[0]=(sample_t){.x=0,.y=0,.count=0};assert(!app_poll_main_navigation(&loading));
    // index==1 屏蔽保留：第二格底栏不计导航。
    samples[0]=(sample_t){.x=200,.y=1141,.count=1};assert(!app_poll_main_navigation(&loading));
    samples[0]=(sample_t){.x=0,.y=0,.count=0};(void)app_poll_main_navigation(&loading);
    samples[0]=(sample_t){.x=10,.y=1141,.count=2};assert(!app_poll_main_navigation(&loading));
    samples[0]=(sample_t){.x=0,.y=0,.count=0};(void)app_poll_main_navigation(&loading);
    samples[0]=(sample_t){.x=10,.y=1141,.count=1,.error=-1};assert(!app_poll_main_navigation(&loading));

    reset();add(100,400,1,0);add(110,400,1,0);add(0,0,0,0);run();
    assert(touch_calls==1&&ticks==3&&!tick_consumed[0]);
    reset();response=APP_REDRAW_AREA;add(100,400,1,0);run();assert(touch_calls==1&&tick_consumed[0]);
    reset();first.on_gesture=gesture;response=APP_REDRAW_AREA;add(100,400,1,0);add(0,0,0,0);run();
    assert(!touch_calls&&events[UI_GESTURE_PRESS]==1&&events[UI_GESTURE_TAP]==1&&tick_consumed[0]);
    reset();first.on_gesture=gesture;add(100,400,1,0);add(0,0,0,9);add(100,400,1,0);add(0,0,0,0);run();
    assert(events[UI_GESTURE_PRESS]==1&&events[UI_GESTURE_CANCEL]==1&&!events[UI_GESTURE_TAP]);
    reset();request_on_touch=true;menu_on_touch=true;add(100,400,1,0);run();assert(enters==2&&exits==1&&ticks==0&&!menus);
    reset();request_on_tick=true;menu_on_tick=true;add(0,0,0,0);run();assert(enters==2&&exits==1&&ticks==1&&!menus);
    reset();first.on_gesture=gesture;request_on_touch=true;cancel_clobber=true;add(100,400,1,0);run();
    assert(enters==2&&events[UI_GESTURE_CANCEL]==1&&!menus);
    reset();menu_on_tick=true;add(0,0,0,0);add(0,0,0,0);run();assert(menus==1&&ticks==1);
    reset();first.owns_keys=true;for(int k=0;k<3;k++){add(k*160+80,1500,1,0);add(0,0,0,0);}run();
    assert(keys[0]==1&&keys[1]==1&&keys[2]==1&&!menus&&fulls==1);
    reset();add(240,1500,1,0);add(0,0,0,0);add(400,1500,1,0);run();assert(!keys[1]&&!keys[2]&&menus==1&&fulls==2);
    // 横屏条纹区例外：实体键条纹点（y>=UI_KEY_AREA_TOP）原样放行不旋转，屏内点按
    // lx=ty、ly=h-1-tx 旋转并钳位。条纹区坐标在两种方向下语义相同、旋转必须跳过，
    // 是这台设备触屏模型里最容易踩的坑——回归：曾把 (80,1500) 旋成屏内 (1215,603)，
    // 三个实体键与菜单全部失效。
    // Landscape strip exception: key-strip points (y>=UI_KEY_AREA_TOP) pass
    // through unrotated; on-screen points rotate by lx=ty, ly=h-1-tx and clamp.
    // Strip coords mean the same in both orientations, so rotation must skip
    // them - the easiest trap in this device's touch model (regression:
    // (80,1500) once rotated into on-screen (1215,603), killing all three
    // keys and the menu).
    reset();host_rotation=EPD_ROT_LANDSCAPE;first.owns_keys=true;
    for(int k=0;k<3;k++){add(k*160+80,1500,1,0);add(0,0,0,0);}run();
    assert(keys[0]==1&&keys[1]==1&&keys[2]==1&&!menus&&fulls==1);
    reset();host_rotation=EPD_ROT_LANDSCAPE;add(240,1500,1,0);add(0,0,0,0);add(400,1500,1,0);run();
    assert(!keys[1]&&!keys[2]&&menus==1&&fulls==2);
    // 分界行本身（y==UI_KEY_AREA_TOP）仍算条纹点不旋转；低一行（y=1299）是屏内点，
    // 旋转成 (1215,603) 后按不到键——分界点上下各验一侧，防止边界被悄悄挪动。
    // The split row itself (y==UI_KEY_AREA_TOP) stays unrotated; one row below
    // (y=1299) is on-screen, rotates to (1215,603) and misses every key - both
    // sides of the split are checked so the boundary cannot silently shift.
    reset();host_rotation=EPD_ROT_LANDSCAPE;first.owns_keys=true;add(80,1300,1,0);add(0,0,0,0);run();
    assert(keys[0]==1);
    reset();host_rotation=EPD_ROT_LANDSCAPE;first.owns_keys=true;add(80,1299,1,0);add(0,0,0,0);run();
    assert(!keys[0]);
    // 屏内点按 lx=ty、ly=h-1-tx 旋转：原生 (100,500) -> 逻辑 (500,583)；越界钳位：
    // 原生 (50,1216) 的 lx=1216 钳到 1215，ly=683-50=633。
    // On-screen points rotate by lx=ty, ly=h-1-tx: native (100,500) -> logical
    // (500,583); clamping: native (50,1216) has lx=1216 clamped to 1215, ly=633.
    reset();host_rotation=EPD_ROT_LANDSCAPE;first.on_gesture=coord_gesture;
    add(100,500,1,0);add(110,500,1,0);add(0,0,0,0);run();
    assert(events[UI_GESTURE_TAP]==1&&tap_x0==500&&tap_y0==583&&tap_x==500&&tap_y==573);
    reset();host_rotation=EPD_ROT_LANDSCAPE;first.on_gesture=coord_gesture;
    add(50,1216,1,0);add(60,1216,1,0);add(0,0,0,0);run();
    assert(events[UI_GESTURE_TAP]==1&&tap_x0==1215&&tap_y0==633&&tap_x==1215&&tap_y==623);
    // 竖屏（默认方向）整条映射恒等：同一个屏内点坐标原样透传。
    // Portrait (the default orientation) is an identity passthrough.
    reset();first.on_gesture=coord_gesture;
    add(100,500,1,0);add(110,500,1,0);add(0,0,0,0);run();
    assert(events[UI_GESTURE_TAP]==1&&tap_x0==100&&tap_y0==500&&tap_x==110&&tap_y==500);
    puts("landscape touch remap: key-strip points pass through unrotated (split row included), on-screen points rotate by lx=ty/ly=h-1-tx with clamping, portrait stays identical passed");
    reset();add(650,1150,1,0);add(0,0,0,0);add(100,220,1,0);add(0,0,0,0);run();
    assert(highlights==1&&restores==1&&enters==2&&exits==1);
    reset();add(650,1150,1,0);add(0,0,0,0);add(100,220,1,0);add(550,220,1,0);add(100,220,1,0);add(0,0,0,0);run();
    assert(highlights==1&&restores==1&&enters==1&&!exits);
    reset();add(650,1150,1,0);add(0,0,0,0);add(100,220,1,0);add(0,0,0,9);run();assert(restores==1&&enters==1);
    reset();first.on_gesture=gesture;full_tick=true;add(100,400,1,0);add(0,0,0,0);run();
    assert(events[UI_GESTURE_CANCEL]==1&&!events[UI_GESTURE_TAP]);
    reset();first.on_gesture=gesture;font_due=true;time_offset=4000000;add(100,400,1,0);add(0,0,0,0);run();
    // 开机首帧前已加载保存字体，因此首个手势无需再被延迟字体切换取消。
    // The saved font is ready before the first frame, so the first gesture is no longer cancelled.
    assert(events[UI_GESTURE_PRESS]==1&&events[UI_GESTURE_TAP]==1&&font_opens==1);
    reset();first.owns_keys=true;add(650,1150,1,0);add(0,0,0,0);add(240,1500,1,0);add(0,0,0,0);add(400,1500,1,0);run();
    assert(!keys[1]&&!keys[2]&&fulls==2&&menus==2);
    reset();add(650,1150,1,0);add(0,0,0,0);
    for(int i=0;i<3;i++){add(100,220,1,0);add(550,220,1,0);add(0,0,0,0);}run();
    assert(du_areas==6&&gl_areas==1);
    reset();time_step=1000000;add(650,1150,1,0);add(0,0,0,0);add(100,220,1,0);add(550,220,1,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(du_areas==2&&gl_areas==1);
    reset();first.on_gesture=gesture;time_step=1000000;lock_due=true;
    add(100,400,1,0);add(100,400,1,0);add(100,400,1,0);add(0,0,0,0);run();
    assert(!lock_due&&events[UI_GESTURE_CANCEL]==1&&!events[UI_GESTURE_TAP]&&tick_consumed[2]);
    reset();first.owns_keys=true;first.on_key_long=long_key;time_step=200000;
    add(240,1500,1,0);add(240,1500,1,0);add(240,1500,1,0);add(240,1500,1,0);add(240,1500,1,0);add(0,0,0,0);run();
    assert(keys[1]==1&&long_keys==1&&menus==1);
    reset();first.owns_keys=true;first.on_key_long=long_key;time_step=200000;
    add(240,1500,1,0);add(0,0,0,0);run();assert(keys[1]==1&&!long_keys&&!menus);
    reset();first.owns_keys=true;first.on_key_long=long_key;time_step=300000;
    add(240,1500,1,0);add(80,1500,1,0);add(240,1500,1,0);add(240,1500,1,0);run();assert(!long_keys);
    reset();first.owns_keys=true;first.on_key_long=long_key;time_step=300000;
    add(240,1500,1,0);add(0,0,0,9);add(240,1500,1,0);add(240,1500,1,0);run();assert(!long_keys);
    reset();first.owns_keys=true;first.on_key_long=long_key;time_step=1000000;lock_due=true;
    add(0,0,0,0);add(240,1500,1,0);add(240,1500,1,0);add(240,1500,1,0);add(0,0,0,0);run();assert(!lock_due&&!long_keys);
    home_case();home_on_tick=true;add(0,0,0,0);add(0,0,0,0);run();
    assert(exits==0&&home_enters==0&&menus==1&&last_menu_leaf==1&&menu_background==&second);
    assert(home_renders==0&&ticks==1&&fulls==1&&mode==1);
    home_case();home_on_tick=true;add(0,0,0,0);add(400,1500,1,0);add(0,0,0,0);run();
    assert(exits==0&&home_enters==0&&menus==1&&home_renders==0);
    home_case();home_on_touch=true;request_on_touch=true;menu_on_touch=true;add(100,400,1,0);run();
    assert(exits==0&&home_enters==0&&menus==1&&home_renders==0&&ticks==0);
    home_case();second.on_gesture=gesture;home_on_touch=true;cancel_clobber=true;response=APP_REDRAW_PAGE;add(100,400,1,0);run();
    assert(exits==0&&home_enters==0&&menus==1&&events[UI_GESTURE_CANCEL]==1&&home_renders==0);
    home_case();second.owns_keys=true;second.on_key_long=long_key;home_on_key=true;time_step=300000;
    add(240,1500,1,0);add(240,1500,1,0);add(240,1500,1,0);add(240,1500,1,0);run();
    assert(exits==0&&home_enters==0&&menus==1&&!long_keys&&ticks==0);
    reset();first.render=home_render;first.on_tick=origin_tick;second.on_tick=return_tick;second.on_exit=leave;
    add(0,0,0,0);add(650,1150,1,0);add(0,0,0,0);add(550,500,1,0);add(0,0,0,0);
    add(100,220,1,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(exits==2&&enters==3&&last_menu_leaf==1&&menu_background==&first&&home_renders==1);
    reset();first.render=home_render;first.on_tick=origin_tick;second.on_tick=return_tick;second.on_exit=leave;
    add(0,0,0,0);add(650,1150,1,0);add(0,0,0,0);add(550,500,1,0);add(0,0,0,0);
    add(100,220,1,0);add(0,0,0,0);add(0,0,0,0);add(400,1500,1,0);run();
    assert(exits==2&&enters==3&&rendered_leaf==7&&home_renders==2);
    reset();request_on_tick=true;first.render=home_render;first.on_tick=origin_tick;second.on_tick=return_tick;second.on_exit=leave;
    add(0,0,0,0);add(0,0,0,0);run();
    assert(exits==2&&enters==3&&!menus&&home_renders==2&&rendered_leaf==7);
    reset();first.render=home_render;first.on_enter=home_enter;first.on_tick=origin_tick;second.on_tick=return_tick;second.on_exit=leave;
    add(0,0,0,0);add(650,1150,1,0);add(0,0,0,0);add(550,500,1,0);add(0,0,0,0);
    add(100,220,1,0);add(0,0,0,0);add(0,0,0,0);
    add(100,220,1,0);add(0,0,0,0);add(0,0,0,0);add(400,1500,1,0);run();
    assert(exits==4&&enters==5&&last_menu_leaf==1&&menu_background==&first&&home_renders==2&&rendered_leaf==7);
    reset();start_second=true;second.on_tick=origin_tick;second.on_exit=leave;second.on_enter=home_enter;second.render=home_render;first.on_tick=return_tick;
    add(0,0,0,0);add(650,1150,1,0);add(0,0,0,0);add(100,120,1,0);add(0,0,0,0);add(0,0,0,0);add(400,1500,1,0);run();
    assert(exits==2&&enters==3&&last_menu_leaf==1&&menu_background==&second&&home_renders==2&&rendered_leaf==7);
    reset();first.on_tick=consumed_return_tick;second.on_tick=return_tick;second.on_exit=leave;
    add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(exits==2&&enters==3&&menus==1&&last_menu_leaf==0&&menu_background==&first);
    // 拔卡先关资源再回退字体，取消本轮按键，持续无卡不重复通知。
    // Removal closes consumers before fallback, drops the current key and notifies only once.
    reset();media_test=sd_font=true;present_steps[0]=mounted_steps[0]=true;time_step=600000;first.on_media_lost=lost;
    add(0,0,0,0);add(80,1500,1,0);add(0,0,0,0);run();
    assert(media_lost==1&&loss_step==1&&builtin_opens==1&&!keys[0]&&fulls==2);
    // 后台页即使被菜单遮盖也释放资源，仍保留菜单视图。
    // A menu-covered page still releases resources and retains the menu view.
    reset();media_test=sd_font=true;present_steps[0]=mounted_steps[0]=true;time_step=600000;first.on_media_lost=lost;
    add(650,1150,1,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(media_lost==1&&builtin_opens==1&&menus==2&&ticks==0);
    // 延迟挂载后拔卡同样被捕获，使用内置字体无需再打开一次。
    // Removal after a late mount is detected without reopening an already built-in font.
    reset();media_test=true;present_steps[1]=mounted_steps[1]=true;time_step=600000;first.on_media_lost=lost;
    add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(media_lost==1&&loss_step==2&&!builtin_opens);
    // 首次无卡时后插卡应触发探测，挂载完成只通知页面一次。
    // First insertion after an empty boot probes, then notifies the active page once.
    reset();media_test=true;time_step=600000;first.on_media_ready=ready;
    present_steps[1]=present_steps[2]=present_steps[3]=true;
    mounted_steps[2]=mounted_steps[3]=true;
    add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(probes>=1&&media_ready==1&&!media_lost);
    // 手势和长按在拔卡边界取消，不能把旧按住状态变成新的动作。
    // Removal cancels gestures and holds without turning an old contact into another action.
    reset();media_test=true;present_steps[0]=mounted_steps[0]=true;time_step=600000;first.on_media_lost=lost;first.on_gesture=gesture;
    add(100,400,1,0);add(100,400,1,0);add(0,0,0,0);run();
    assert(media_lost==1&&events[UI_GESTURE_CANCEL]==1&&!events[UI_GESTURE_TAP]);
    reset();media_test=true;present_steps[0]=mounted_steps[0]=true;time_step=600000;first.on_media_lost=lost;first.owns_keys=true;first.on_key_long=long_key;
    add(240,1500,1,0);add(240,1500,1,0);add(240,1500,1,0);run();
    assert(media_lost==1&&keys[1]==1&&!long_keys);
    // 卡不在位时不空转探测：没有卡就是 NOT_FOUND，兜底分支不该被点着。
    // 这条不是「失效后永不探测」——那正是 26.9.16 把缺陷写成规格的那句话。
    // No idle probing while the slot is empty: no card means NOT_FOUND, so the
    // fallback never arms. This is NOT "never probe after invalidation" - that
    // sentence was the 26.9.16 spec that encoded the bug.
    reset();media_test=sd_font=saved_sd_font=true;present_steps[0]=mounted_steps[0]=true;time_step=4000000;first.on_media_lost=lost;
    add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(media_lost==1&&builtin_opens==1&&!probes&&!font_opens);
    reset();media_test=sd_font=saved_sd_font=true;present_steps[0]=mounted_steps[0]=present_steps[2]=mounted_steps[2]=true;time_step=4000000;first.on_media_lost=lost;
    add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(media_lost==2&&builtin_opens==2&&!probes&&font_opens==1);
    // 26.9.17 T1/T2：热插拔闭环——拔卡即卸载（remount 被调且只调一次），
    // 插回自动重探测（probes 起），挂载成功通知页面（on_media_ready）。
    // 26.9.17 T1/T2: the hot-plug loop - removal releases the volume (remount is
    // called, exactly once), reinsertion re-probes on its own, and a successful
    // mount notifies the page.
    reset();media_test=true;time_step=600000;
    present_steps[0]=mounted_steps[0]=true;
    present_steps[2]=present_steps[3]=true;mounted_steps[3]=true;
    first.on_media_lost=lost;first.on_media_ready=ready;
    add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);run();
    // 合流：thaw_calls>=2 是 31b 三拍的判据（边沿一次 + 卸载成功一次），保留。
    assert(media_lost==1&&remounts==1&&probes>=1&&media_ready>=1&&thaw_calls>=2);
    // 26.11.5：拔卡那一拍收一次链路（detach），插回那一拍还一次可连性（resume）。
    // 这两条断言咬的是**对称**：只断不还，手机再也连不上（31a 家族第三入口）。
    // 26.11.5: one detach on removal, one reattach on insertion - symmetry is the point.
    assert(detach_calls==1&&resume_calls>=1);
    // 31b 第 1 条：三拍顺序。quiesce 必须在 remount 之前（采样为 0 次 remount），
    // 卸载成功后解冻一次。顺序反了就是「先抽走卷、再问别人停没停」。
    // Three-beat order: quiesce precedes remount (sampled: zero remounts so far),
    // and a successful unmount thaws once.
    reset();media_test=true;time_step=600000;
    present_steps[0]=mounted_steps[0]=true;
    first.on_media_lost=lost;
    add(0,0,0,0);add(0,0,0,0);run();
    assert(media_lost==1&&quiesce_calls==1&&remounts==1&&remounts_before_quiesce==0&&thaw_calls==1);
    // 31b P1-1：CD 说「在」、I/O 说「不在」。判死那一次之后，**同一拍**必须走 lost
    // （组件把 mounted 落成 0，get_info 立刻反映），三拍照常，且只报一次——
    // 若判死只写进一个没人读的标志，真机上就是「卡拔了、阅读器还在读一张没了的卡」。
    // P1-1: CD says present, I/O says gone. The same tick must reach lost, exactly once.
    reset();media_test=true;time_step=1000000;
    present_steps[0]=mounted_steps[0]=true;
    first.on_media_lost=lost;
    // 16 拍 × 1 s：判死在第 1 拍，之后要跨过看门狗第一档 8 s 才看得见第二次 remount。
    // 4 拍只能看见 lost 那一拍——**断言写成 ==1 的时候，这条路径本来就不在窗口里**。
    // 16 ticks x 1 s: death lands on tick 1; the watchdog's first rung is 8 s away,
    // so a 4-tick window could only ever see the lost tick.
    for(int i=0;i<16;i++)add(0,0,0,0);
    io_dead_after=3;   // 第 0 拍 poll_media 被调两次（初始化 + 首拍），第 1 拍才判死
    run();
    // `remounts>=2` 是收紧：第 1 次是 lost 那拍的三拍，第 2 次起是看门狗接手。
    // 它咬的不是「码是多少」，而是「判死之后还有没有人碰这张卡」——
    // 桩按真件返回 INVALID_STATE 之后这条才第一次可测（旧桩喂 NOT_FOUND，
    // 兜底分支直接跳过，断言写成 ==1 也永远绿）。
    // >=2 is tighter: beat 1 is the lost tick, beat 2+ is the watchdog. The assertion is
    // not about which code; it is about whether anyone still touches the card after death.
    // **看门狗在 app_loop 侧叫 `start_probe`，不叫 `remount`**（实测：14 次 probe / 1 次 remount）。
    // remount 只在 lost 那条路上，此后每一拍走的是兜底探测——组件内部再决定「真探一次」
    // 还是「先卸卷」。所以判据是 `probes>=2`：判死之后还有人在碰这张卡。
    // 上一版把判据写成 remounts，是把两条不同的路当成一条（推断当事实第 3 次）。
    // The watchdog on the app side is start_probe, not remount (measured: 14 vs 1).
    // remount belongs to the lost tick; every tick after goes through the fallback probe.
    // So the criterion is probes>=2: after death, someone still touches the card.
    assert(media_lost==1&&remounts==1&&probes>=2&&io_watch_calls>=4);
    // B42：插拔边沿走的是 discard（作废旧封面），不是 quiesce(0)（宣布收口却不等）。
    assert(cover_discards>=1);

    // 31b 第 1 条：quiesce 失败 = 一次 remount 都不许发生。这条断言要是绿得起来，
    // 说明三拍只是个仪式——卷照样被抽走了。
    // A failed quiesce means zero remounts. If this passes with remounts != 0, the
    // three beats are a ritual and the volume leaves anyway.
    reset();media_test=true;time_step=600000;
    present_steps[0]=mounted_steps[0]=true;
    first.on_media_lost=lost;
    quiesce_ok=false;
    add(0,0,0,0);add(0,0,0,0);run();
    assert(media_lost==1&&quiesce_calls>=1&&remounts==0&&thaw_calls==0);

    // 2026-11-01 批复条件③：push 的 FILE* 永不关 —— 冻结 + 重试，**绝不放弃卸载**。
    // 旧写法在第 3 次失败就把 `s_release_pending` 置 false（「放弃」）；31a 环④证明
    // 那会让 `card` 永非空、之后每次 mount 都以 `stale card` 被拒。两段观测：
    //   ① 等不到时一次都不许动卷，且跨档留痕**恰好一次**（bootbb 不逐拍刷）；
    //   ② 句柄一关（下一拍 quiesce 成功），立刻走**带闸**的卸载 → 解冻。
    // `remounts_before_quiesce == remounts-1` 咬的是「路径没绕闸」：看门狗那条路
    // 不过 quiesce，旧写法恢复后只能走它，这个采样会露馅。
    // Ruling 3: a FILE* that never closes means frozen + retry, never "give up".
    // While it refuses: zero remounts, exactly one hold mark. Once it stops: the next
    // attempt unmounts through the gate (sampled at call time, not asked afterwards).
    // 先排空上一场景遗留的闩锁：它是函数内 static，`reset()` 摸不到。这一拍
    // quiesce 成功 → remount 成功 → pending 归零；随后重新开账，计数才干净。
    // Drain the previous case's latch first: it is a function-static that `reset()`
    // cannot reach. One successful beat clears it; then the books are reopened.
    reset();media_test=true;time_step=12000000;
    present_steps[0]=mounted_steps[0]=true;
    first.on_media_lost=lost;
    add(0,0,0,0);run();
    reset();media_test=true;time_step=12000000; // 12 s/拍：慢档 10 s 节拍每拍到期
    present_steps[0]=mounted_steps[0]=true;
    first.on_media_lost=lost;
    quiesce_ok=false;
    release_resume_step=15;
    for(int i=0;i<17;i++)add(0,0,0,0);
    run();
    assert(media_lost==1);
    assert(remounts==1);                 // 只有恢复那一拍动过卷
    assert(hold_calls==1);               // 跨档留痕一次，不逐拍刷 bootbb
    assert(thaw_calls>=1);               // 卸载成功后解冻（HOLD 一并归零）
    assert(quiesce_calls>=5);            // 三次快速之后慢档继续，没停在第 3 次
    assert(remounts_before_quiesce==remounts-1); // 最后一次 remount 前刚过闸
    release_resume_step=-1;
    // 26.9.31a P0-1(e)：看门狗永不放弃。三次快速重试（8 s 间隔）用完后必须继续重试，
    // 26.9.29 的「上限 3 次」在这里恰好停在 3 次——之后除了重启，再没有人碰这张卡。
    // 32 steps × 12 s：前三次 8 s 间隔，之后转 10 s 看门狗节拍。
    // 26.9.31a P0-1(e): the watchdog never gives up. After the three fast retries (8 s
    // apart) it keeps going; 26.9.29's "cap 3" stops dead at three and nothing touches the
    // card again until a reboot. 32 steps x 12 s: three fast, then the 10 s watchdog.
    //
    // 合流注意：`sd_hard_error`（TIMEOUT）是必须的。走 INVALID_STATE 会落到兜底探测
    // 分支，那条路 31b 加了三拍门（`!*invalidated` 之外还有 quiesce），测的就不是
    // 看门狗节拍了。这正是 31a 那条注释说的「两条路必须分开测」，合流后依然成立。
    reset();media_test=true;sd_hard_error=true;time_step=12000000;
    for(int i=0;i<32;i++)present_steps[i]=true;
    add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(remounts>=5);
    reset();idle_minutes=1;time_step=60000000;first.on_before_lock=before_lock;
    add(0,0,0,0);add(0,0,0,0);run();
    assert(idle_locks==1&&before_locks==1&&!push_cancel_calls); // Real scheduler flushes before automatic lock. 自动锁屏（manual=false）从不取消武装（gen18 豁免）；武装期自动锁屏另被 idle_blocked 整体阻断，见下方 gen21 守门。
    // 横屏下自动锁：prepare_lock 先把物理旋转复位竖屏，锁屏/壁纸资产才不画在
    // 横屏物理屏上（真机 bug：长按电源睡眠后壁纸横置）；本就竖屏时保持不动。
    // Auto lock under landscape: prepare_lock resets the panel to portrait first,
    // so lock/wallpaper assets never land on a landscape frame (hardware bug:
    // sideways sleep wallpaper after a long power press); portrait stays put.
    reset();idle_minutes=1;time_step=60000000;host_rotation=EPD_ROT_LANDSCAPE;first.on_before_lock=before_lock;
    add(0,0,0,0);add(0,0,0,0);run();
    assert(idle_locks==1&&before_locks==1&&host_rotation==EPD_ROT_INVERTED_PORTRAIT);
    reset();idle_minutes=1;time_step=60000000;first.on_before_lock=before_lock;
    add(0,0,0,0);add(0,0,0,0);run();
    assert(idle_locks==1&&host_rotation==EPD_ROT_INVERTED_PORTRAIT);
    reset();idle_minutes=1;time_step=60000000;shake_activity=true;
    add(0,0,0,0);run();assert(!idle_locks);
    reset();idle_minutes=1;time_step=60000000;shake_activity=true;
    add(0,0,0,0);add(0,0,0,0);run();assert(idle_locks==1&&wake_clean_requests==1); /* Activity is consumed once, not latched. */
    reset();idle_minutes=0;time_step=600000000;
    add(0,0,0,0);add(0,0,0,0);run();assert(!idle_locks);
    reset();idle_minutes=1;time_step=600000000;online_busy=true;
    add(0,0,0,0);add(0,0,0,0);run();assert(!idle_locks);
    reset();idle_minutes=1;time_step=600000000;first.holds_pmu=true;
    add(0,0,0,0);add(0,0,0,0);run();assert(!idle_locks);
    reset();idle_minutes=1;time_step=30000000;
    add(100,400,1,0);add(100,400,1,0);add(0,0,0,0);add(0,0,0,0);run();assert(!idle_locks);
    reset();first.owns_keys=true;first.on_gesture=gesture;first.menu_handle_enabled=product_menu_disabled;
    add(636,1150,1,0);add(0,0,0,0);run();
    assert(!menus&&events[UI_GESTURE_PRESS]==1&&events[UI_GESTURE_TAP]==1);
    // 首帧保持全刷；切页一次完成，按住、松开、空闲都不安排第二次刷新。
    // Retain the initial full update; finish navigation once without another draw on hold, release or idle.
    reset();enable_main_tabs();add(0,0,0,0);run();assert(!nav_arms&&!nav_draws&&!nav_full_arms);
    reset();enable_main_tabs();request_on_touch=true;add(100,400,1,0);add(100,400,1,0);run();
    assert(nav_arms==1&&nav_draws==1&&!nav_armed);
    reset();enable_main_tabs();request_on_touch=true;add(100,400,1,0);add(100,400,1,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(nav_arms==1&&nav_draws==1&&!nav_armed&&!nav_full_arms);
    reset();enable_main_tabs();first.on_tick=nav_return_tick;add(0,0,0,0);add(0,0,0,0);second.on_tick=return_tick;add(0,0,0,0);run();
    assert(nav_arms==2&&nav_draws==2&&!nav_armed&&shelf_exit_arms==1);
    // 目录延后完成重绘和同页局部更新也获得固定底栏许可，没有额外 idle 推屏。
    // Deferred directory completion and local repaint receive frozen-navigation permission without an idle redraw.
    reset();enable_main_tabs();first.on_tick=nav_return_tick;second.on_tick=main_late_tick;
    response=APP_REDRAW_PAGE;add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(nav_draws==2&&nav_arms==2&&!nav_armed&&!nav_full_arms);
    reset();enable_main_tabs();first.on_tick=nav_return_tick;second.on_tick=main_late_tick;
    response=APP_REDRAW_AREA;add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(nav_draws==1&&nav_area_draws==1&&nav_arms==2&&!nav_armed&&!nav_full_arms);
    // 子视图、非主页面保留普通出口，显式全刷优先。
    // Subviews and non-main pages retain ordinary output; explicit full refresh has priority.
    reset();enable_main_tabs();main_source_visible=false;request_on_touch=true;add(100,400,1,0);add(0,0,0,0);run();assert(!nav_arms&&!nav_draws);
    reset();enable_main_tabs();second.on_enter=enter_subview;request_on_touch=true;add(100,400,1,0);add(0,0,0,0);run();assert(!nav_arms&&!nav_draws);
    reset();enable_main_tabs();second.main_page_visible=NULL;request_on_touch=true;add(100,400,1,0);add(0,0,0,0);run();assert(!nav_arms&&!nav_draws);
    reset();enable_main_tabs();second.enter_full=true;request_on_touch=true;add(100,400,1,0);add(0,0,0,0);run();assert(nav_arms==1&&nav_full_arms==1&&!nav_draws);
    reset();enable_main_tabs();main_fast=false;request_on_touch=true;add(100,400,1,0);add(0,0,0,0);run();
    assert(nav_arms==1&&nav_draws==1&&!shelf_exit_arms&&!nav_full_arms);
    reset();enable_main_tabs();main_fast=false;first.on_tick=nav_return_tick;second.on_tick=main_late_tick;
    response=APP_REDRAW_AREA;add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(nav_draws==1&&nav_area_draws==1&&nav_arms==2&&!nav_full_arms);
    reset();enable_main_tabs();lock_due=true;time_step=3000000;add(0,0,0,0);add(0,0,0,0);run();
    assert(idle_locks==1&&!nav_arms&&!nav_draws);
    puts("main-tab scheduler: single immediate draw, no hold/release/idle redraw, rapid return, root/subview boundaries and full-output priority passed");
    // 跑真实边缘识别和主循环，验证不会漏给底层页面或按住重复动作。
    // Run the real edge recognizer and loop to reject underlay leakage and repeated hold actions.
    reset();first.on_gesture=gesture;
    add(300,20,1,0);add(300,110,1,0);add(300,120,1,0);add(0,0,0,0);
    add(300,500,1,0);add(0,0,0,0);add(100,400,1,0);add(0,0,0,0);run();
    assert(quick_draws==1&&quick_gl==2&&!quick_du);
    assert(events[UI_GESTURE_PRESS]==1&&events[UI_GESTURE_TAP]==1);
    reset();main_fast=true;saved_wifi=true;
    add(300,20,1,0);add(300,110,1,0);add(0,0,0,0);
    add(68,156,1,0);add(68,156,1,0);add(0,0,0,0);
    add(68,156,1,0);add(0,0,0,0);run();
    assert(wifi_starts==1&&wifi_stops==1&&quick_gl==3&&!quick_du&&!touch_calls);
    reset();upload_busy=true;transfer_status=(read_pico_transfer_status_t){.state=1,.mode=READ_PICO_TRANSFER_MODE_STA};
    add(300,20,1,0);add(300,110,1,0);add(0,0,0,0);add(68,156,1,0);add(0,0,0,0);run();
    assert(!wifi_starts&&!wifi_stops&&transfer_status.state==1);
    reset();add(300,20,1,0);add(300,110,1,0);add(0,0,0,0);add(68,156,1,0);add(0,0,0,0);run();
    assert(!wifi_starts&&!wifi_stops&&!touch_calls);
    reset();add(300,20,1,0);add(300,110,1,0);add(0,0,0,0);
    add(205,156,1,0);add(0,0,0,0);add(205,156,1,0);add(0,0,0,0);run();
    assert(!ble_enabled&&ble_resets==1&&!touch_calls);
    // gen24.2 块 2：快捷层关蓝牙 = 设置页关蓝牙的同一语义——cancel_why
    // 「蓝牙已关闭」必须随行（armed/pending 收摊），否则快捷层成为 B 模型
    // 的静默后门。/ Block 2: the quick layer's switch-off is the settings
    // page's switch-off - cancel_why "bluetooth off" must ride along (the
    // armed/pending close-up), or the quick layer is model B's silent back
    // door.
    assert(push_cancel_calls==1&&!strcmp(push_cancel_why,"蓝牙已关闭"));
    reset();first.on_before_lock=before_lock;
    add(300,20,1,0);add(300,110,1,0);add(0,0,0,0);
    add(342,156,1,0);add(0,0,0,0);add(479,156,1,0);add(0,0,0,0);run();
    assert(idle_locks==1&&before_locks==1&&fulls>=2&&!touch_calls);
    for(int policy=0;policy<3;++policy){
        reset();main_fast=policy==1;main_water=policy==2;first.on_gesture=gesture;click_enabled=true;
        add(85,1141,1,0);add(85,1141,1,0);add(0,0,0,0);run();
        assert(click_presses==1&&click_restores==1&&!click_active&&ticks==1&&du_areas==2&&!gl_areas);
        reset();enable_main_tabs();main_fast=policy==1;main_water=policy==2;click_enabled=true;request_on_touch=true;
        // 27 起切页在收敛窗之后，这里要补够空拍：回显仍是按下/抬起各一次（du_areas==2），
        // 切页只在窗尾发生一次（nav_draws==1）。
        add(200,1141,1,0);add(0,0,0,0);idle(10);run();
        assert(click_presses==1&&click_restores==1&&!click_active&&du_areas==2&&!gl_areas&&nav_draws==1);
    }
    reset();click_enabled=true;add(85,1141,1,0);add(85,1141,2,0);add(0,0,0,0);run();
    assert(click_presses==1&&click_restores==1&&!click_active);
    reset();click_enabled=true;add(85,1141,1,0);add(0,0,0,1);add(0,0,0,0);run();
    assert(click_presses==1&&click_restores==1&&!click_active);
    puts("quick layer/click scheduler: modal ownership, local BW/gray, one saved-network connect/disconnect, upload/no-credential guards, BLE toggle, full/lock and release/multitouch/read-error restoration passed");
    // 反馈必须早于页面保存/后台收尾；下一页与自动休眠不显示主动锁屏徽标。
    // Acknowledgement precedes saves/joins; consumed page turns and idle sleep omit it.
    for(int policy=0;policy<3;++policy){
        reset();main_fast=policy==1;main_water=policy==2;feedback_enabled=true;
        first.on_before_lock=before_lock;lock_due=true;time_step=3000000;
        add(0,0,0,0);add(0,0,0,0);run();
        assert(lock_badges==1&&lock_feedback_calls==1&&before_locks==1&&idle_locks==1);
        reset();feedback_enabled=true;first.on_power_short=consume_power;lock_due=true;time_step=3000000;
        add(0,0,0,0);add(0,0,0,0);run();assert(!lock_badges&&!lock_feedback_calls&&!before_locks&&!idle_locks);
        reset();feedback_enabled=true;idle_minutes=1;time_step=60000000;first.on_before_lock=before_lock;
        add(0,0,0,0);add(0,0,0,0);run();assert(idle_locks==1&&before_locks==1&&!lock_feedback_calls&&!lock_badges);
    }
    puts("PASS: manual lock acknowledgement precedes page save/reader joins in all three modes; consumed power turns and auto sleep omit the badge");
    reset();power_trigger_step=1;time_step=3000000;
    add(0,0,0,0);add(0,0,0,0);add(128,374,1,0);add(240,374,1,0);add(556,374,1,0);add(0,0,0,0);run();
    assert(power_open_draws==1&&power_controls_draws==2&&shutdowns==1&&!restarts&&power_final_draws==1&&!touch_calls&&ticks==1&&du_areas==2&&gl_areas==1);
    reset();power_trigger_step=1;time_step=3000000;staged_shutdown=true;
    add(0,0,0,0);add(0,0,0,0);add(128,374,1,0);add(556,374,1,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(idle_locks==1&&wake_clean_requests==1&&!shutdowns&&!restarts&&!power_final_draws);
    // gen21 守门（手动电源键推翻 gen18 豁免）：武装期手动息屏不拦截——静默取消
    // 武装（cancel_why 恰一次且 why=="manual sleep"）后正常走锁+息屏闭环， resumed
    // 卡回 IDLE；自动锁屏则被武装整体阻断（M1.5 关屏保活），一次都不取消。此桩自
    // gen21（2ea51cb）引入调用起漏同步定义，宿主链接失败躺三代（gen21-23），
    // 与 gen19 transfer_ui 断言脱节同型：宿主桩是实现契约，策略变更必须同步。
    // gen21 gatekeeper (manual power key outranks the gen18 exemption): while
    // armed, manual sleep never refuses - it silently disarms (exactly one
    // cancel_why, why=="manual sleep") then completes the lock-and-sleep loop;
    // auto lock stays blocked wholesale by the arm (M1.5 screen-off keepalive)
    // and never cancels. This stub missed its definition since gen21 (2ea51cb)
    // and the host link failed for three generations (gen21-23), the same
    // shape as gen19's transfer_ui assertion drift: host stubs are the
    // implementation contract - policy changes must be mirrored in.
    reset();power_trigger_step=1;time_step=3000000;staged_shutdown=true;push_armed=true;
    add(0,0,0,0);add(0,0,0,0);add(128,374,1,0);add(556,374,1,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(idle_locks==1&&wake_clean_requests==1&&!shutdowns&&!restarts&&!power_final_draws);
    assert(push_cancel_calls==1&&strcmp(push_cancel_why,"manual sleep")==0);
    reset();idle_minutes=1;time_step=60000000;push_armed=true;first.on_before_lock=before_lock;
    add(0,0,0,0);add(0,0,0,0);run();
    assert(!idle_locks&&!before_locks&&!push_cancel_calls); // 武装阻断自动锁屏（保活），且不取消武装
    reset();power_trigger_step=1;time_step=3000000;
    add(0,0,0,0);add(0,0,0,0);add(238,960,1,0);add(0,0,0,0);run();
    assert(restarts==1&&!shutdowns&&power_final_draws==1);
    reset();enable_main_tabs();power_trigger_step=1;time_step=3000000;
    add(0,0,0,0);add(0,0,0,0);add(446,960,1,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(!shutdowns&&!restarts&&!power_final_draws&&nav_arms==0&&mode==1);
    for(int fault=0;fault<2;++fault){
        reset();power_trigger_step=1;time_step=3000000;
        add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);add(128,374,1,0);add(556,374,1,0);add(556,374,fault?2:0,fault?0:1);add(0,0,0,0);run();
        assert(!shutdowns&&!restarts&&power_controls_draws==2&&!touch_calls);
    }
    reset();power_trigger_step=1;time_step=3000000;
    add(0,0,0,0);add(128,374,1,0);add(556,374,1,0);add(0,0,0,0);run();
    assert(power_open_draws==1&&!power_controls_draws&&!shutdowns&&!restarts);
    // 守门用例一：横屏长按电源打开关机对话框，断言对话框分支已把物理旋转复位
    // 竖屏——对话框是第 7 个系统画面，并入「系统画面一律竖屏」不变式（真机
    // round-2 缺陷：横屏下对话框没有转过来、取消按钮踩空无法显示）。
    // Gatekeeper 1: a landscape long power press opens the shutdown dialog with
    // the panel rotation already reset to portrait - the dialog is the 7th
    // system screen and joins the "system frames are portrait" invariant
    // (hardware round-2 defect: the dialog did not turn, and the cancel button
    // could not be shown or hit).
    reset();host_rotation=EPD_ROT_LANDSCAPE;power_trigger_step=1;time_step=3000000;
    add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(power_open_draws==1&&gl_areas==1&&host_rotation==EPD_ROT_INVERTED_PORTRAIT);
    // 守门用例二（用户裁决生死线）：横屏打开对话框 → 取消 → 返回阅读。断言取消
    // 路径重走当前视图 render()（其内部按视图状态自设旋转回横屏并整页重绘），
    // 而不是帧缓冲区域恢复式——后者会让旋转停留竖屏而像素是横屏内容、触摸映射
    // 错乱，等于修一个 bug 埋一个 bug。启动首帧与取消各调一次 render，故计 2。
    // Gatekeeper 2 (the ruling's lifeline): landscape → dialog → cancel → back
    // to reading. Assert the cancel path repaints through the current view's
    // render(), which self-sets rotation back to landscape per view state and
    // fully repaints - not a framebuffer region restore, which would leave
    // rotation portrait over landscape pixels and misalign touch, trading one
    // bug for another. Startup frame and cancel each call render once, hence 2.
    reset();host_rotation=EPD_ROT_LANDSCAPE;first.render=landscape_render;power_trigger_step=1;time_step=3000000;
    add(0,0,0,0);add(0,0,0,0);add(446,960,1,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(power_open_draws==1&&landscape_renders==2&&host_rotation==EPD_ROT_LANDSCAPE&&!shutdowns&&!restarts);
    // 守门用例三（gen26.3 补 M1 漏桩）：AI 释义卡轮询三分支——
    // ①槽有货且阅读器收：当 tick 出卡（present=首帧+卡=2）、词与释义原样透传、
    //   该 tick 的 press 被丢弃（touch_calls 不增）——释义到达与触摸同一 tick
    //   撞车的窗口由这次丢弃吸收，不产生半幅触摸；
    // ②阅读器不收（ai_ready_ok=false，有面板开着/非阅读态）：不出卡、不吞触摸，
    //   正常走完当 tick（释义按设计留在预取缓存）；
    // ③槽空：轮询零副作用。
    // Gatekeeper 3 (gen26.3 backfills the stubs M1 missed): the AI definition
    // poll's three branches - (1) slot filled and accepted: the card presents
    // this tick (presents = startup + card = 2), word/def pass through
    // verbatim, and that tick's press is discarded (touch_calls stays put) -
    // the def-arrival/touch same-tick collision window is absorbed by the
    // discard, never a half-dispatched touch; (2) refused (a panel open or not
    // reading): no card, no swallowed touch, the tick runs normally (the
    // definition stays in the prefetch cache by design); (3) empty slot: zero
    // side effects.
    reset();first.present=ai_present;ai_take_hit=true;time_step=600000;
    add(100,400,1,0);add(0,0,0,0);run();
    assert(ai_ready_calls==1&&!strcmp(ai_seen_word,"serf")&&!strcmp(ai_seen_def,"a person bound to the land"));
    assert(ai_presents==1&&!touch_calls&&ticks==1); // 出卡 tick 跳过 on_tick 与触摸分发
    reset();first.present=ai_present;ai_take_hit=true;ai_ready_ok=false;time_step=600000;
    add(100,400,1,0);add(0,0,0,0);run();
    assert(ai_ready_calls==1&&ai_presents==0&&touch_calls==1&&ticks==2); // 不收：不出卡、不吞触摸
    reset();first.present=ai_present;ai_take_hit=false;time_step=600000;
    add(100,400,1,0);add(0,0,0,0);run();
    assert(ai_ready_calls==0&&ai_presents==0&&touch_calls==1); // 槽空：零副作用
    // 守门用例三·情景（M3-B）：同一条 0x08 若被情景待命态认领，**只开视图**——
    // 不走释义卡、不进 AI 生词批（340 字四节分析落库会冒充释义，审计裁定的
    // 内容边界）。认领是一次性的：认了就不该再有释义卡动作。
    // Gatekeeper 3-scene (M3-B): when the scene pending state claims the 0x08, the
    // answer ONLY opens the view - no definition card and no AI vocabulary batch
    // (logging the 340-char analysis would impersonate a definition; the ruling's
    // content boundary). The claim is one-shot: no card action may follow.
    reset();first.present=ai_present;ai_take_hit=true;scene_claim_ok=true;time_step=600000;
    add(100,400,1,0);add(0,0,0,0);run();
    assert(scene_claim_calls==1&&!strcmp(scene_claim_word,"serf"));
    assert(ai_ready_calls==0);            // 已认领：不走释义卡 / claimed: no card
    assert(vocab_stub_set_def_calls==0);  // 且不进生词批 / and no batch write
    // 进入情景卡是**整屏 GC16**（版面跨度 = 词卡→近整屏），FULL 与 PAGE 分开计数
    // 正是为了钉住这一点：若哪天退化成局部刷，这条立刻红。
    // Entering the scene card is a FULL GC16 (the card -> near-full-screen jump);
    // FULL and PAGE are counted apart precisely to pin that - a regression to a
    // partial update turns this red at once.
    // 首帧本身还有一次 FULL（进页整刷），故闭环是 2：起始 1 + 情景卡 1。
    // The first frame itself is a FULL (page entry), so the closed form is 2:
    // startup 1 + scene card 1.
    assert(ai_full_presents==2&&ai_presents==0);
    // 守门用例四（M2 Step9 接线）：双 GC16 档位 + KEY_2 豁免五断言——
    // ①开详解（ai_def_full_ok=true）：回流呈现走 FULL，首帧 FULL + 回流 FULL = 2、
    //   零 PAGE——词面→详解页的版面跨度大，局部刷新会留残影，必须整屏 GC16 打底；
    // ②普通回流（ai_def_full_ok=false）：维持 PAGE 档位（首帧 FULL = 1 + 卡 PAGE = 1），
    //   与守门用例三①同口径——档位查询不波及非详解回流；
    // ③KEY_2 豁免关（defer_middle_short + 非详解态）：短按压无动作，on_key 在抬手
    //   步才调（key_steps[1]==1）——延迟菜单语义原样，豁免没有偷改常态路径；
    // ④KEY_2 豁免开（详解态）：压下即调 on_key 关闭（key_steps[1]==0）且不挂
    //   held_key——抬手不补刀（否则详解已关，KEY_2 会落到原书工具面板多弹一层）；
    // ⑤豁免开 + 长按（超 UI_LONG_PRESS_MS）：照样只关一次（long_keys==0）——
    //   「长按也=关闭」由压路直达满足，无需挂键等抬手。
    // Gatekeeper 4 (M2 Step9 wiring): the double GC16 gear + the KEY_2 exemption,
    // five assertions - (1) detail opening (ai_def_full_ok=true): the return flow
    // presents FULL, startup FULL + return FULL = 2 with zero PAGE - the word-card
    // -> detail-page layout jump is wide enough that a partial update would ghost,
    // so a full GC16 base is mandatory; (2) an ordinary flow (flag false): keeps
    // the PAGE gear (startup FULL = 1 + card PAGE = 1), same as gatekeeper 3 case 1
    // - the gear query leaves non-detail flows untouched; (3) exemption off
    // (defer_middle_short, no detail session): the press does nothing and on_key
    // fires on the release step (key_steps[1]==1) - deferred-menu semantics are
    // unchanged, the exemption does not quietly rewrite the normal path; (4)
    // exemption on (detail session live): on_key fires on the press step
    // (key_steps[1]==0) and held_key is not armed - no second call on release
    // (the detail view would already be closed and KEY_2 would land on the
    // source tools panel, popping an extra layer); (5) exemption on with a long
    // press (past UI_LONG_PRESS_MS): still closes exactly once (long_keys==0) -
    // "long press also means close" is satisfied by the press path directly.
    reset();first.present=ai_present;ai_take_hit=true;ai_def_full_ok=true;time_step=600000;
    add(100,400,1,0);add(0,0,0,0);run();
    assert(ai_ready_calls==1&&ai_presents==0&&ai_full_presents==2);
    reset();first.present=ai_present;ai_take_hit=true;ai_def_full_ok=false;time_step=600000;
    add(100,400,1,0);add(0,0,0,0);run();
    assert(ai_full_presents==1&&ai_presents==1); // 普通回流维持 PAGE 档位
    reset();first.owns_keys=true;first.defer_middle_short=true;first.on_key_long=long_key;time_step=100000;
    add(240,1500,1,0);add(0,0,0,0);run();
    assert(keys[1]==1&&key_steps[1]==1&&!long_keys&&!menus); // 豁免关：抬手才执行
    reset();first.owns_keys=true;first.defer_middle_short=true;first.on_key_long=long_key;ai_detail_active=true;time_step=100000;
    add(240,1500,1,0);add(0,0,0,0);run();
    assert(keys[1]==1&&key_steps[1]==0&&!long_keys&&!menus); // 豁免开：压下即关，抬手不补刀
    reset();first.owns_keys=true;first.defer_middle_short=true;first.on_key_long=long_key;ai_detail_active=true;time_step=1000000;
    add(240,1500,1,0);add(0,0,0,0);run();
    assert(keys[1]==1&&key_steps[1]==0&&!long_keys); // 长按也只关一次
    // 26.9.7 幽灵 TAP（审计裁定①）：单指按下 → 双指落定（RETRACT）→ 两指抬起 →
    // 不得补一个 TAP。根因是「饥饿不是撤销」：双指期间 `!ui_dual_engaged` 守卫把拍子
    // 全挡住，单指识别器不再被喂，却从没收到撤销，那条陈旧 PRESS 一直挂着；两指抬起后
    // engaged 归假，识别器被喂到 count==0 的拍子，就把它按 TAP 收尾 → action_at()。
    // 真机 round-4：29 笔紧贴判定后 7-61ms（中位 24ms），人手做不到。
    // 这条用例同时是 M22 的靶子：把 RETRACT 那行的 cancel_gesture 拿掉，TAP 立刻回来。
    // 26.9.7 ghost TAP (adjudication 1): a single press -> two fingers land (RETRACT) ->
    // both lift -> no TAP may follow. Root cause "starvation is not revocation": while two
    // fingers own the touch the `!ui_dual_engaged` guard skips every tick, so the single-
    // finger recognizer stops being fed but is never told to forget; the stale PRESS sits
    // there and, once both lift, a count==0 tick closes it as a TAP -> action_at().
    // Round 4 measured 29 of these 7-61ms after the verdict (median 24ms). This case is
    // also M22's target: remove the cancel_gesture call and the TAP comes straight back.
    reset();
    first.on_gesture=gesture;
    first.on_dual_gesture=dual_ev;
    // 真机形态的关键在**第 3 拍**：抬起过渡帧 count 仍是 2、坐标已清零（两个有效点
    // 凑不齐）→ 双指层在这一拍就收尾了，而 `touch.touched` 仍是真，**抬手边沿还没来**。
    // 下一拍才是 count==0 的释放边沿——那一刻双指层已经回 IDLE、engaged 归假，单指层
    // 重新被喂，陈旧 PRESS 撞上 released 就按 TAP 收尾。这条 1 拍延迟正是真机日志里
    // 7-61ms（中位 24ms）的来源；只给「直接 count==0」的序列复现不出来。
    // The hardware shape hinges on tick 3: the lift transition still reports count==2 with
    // cleared coordinates (no valid pair), so the dual layer finishes there while
    // `touch.touched` is still true - the release edge has not arrived yet. The NEXT tick
    // is the count==0 edge, and by then the dual layer is back to IDLE, engaged is false,
    // the single-finger layer is fed again, and the stale PRESS closes on `released` as a
    // TAP. That one-tick lag is exactly the 7-61ms (median 24ms) seen on hardware; a
    // sequence that jumps straight to count==0 cannot reproduce it.
    add(200,400,1,0);            // step0 单指落下 → PRESS
    add2(200,400,300,400);       // step1 第二指落定 → RETRACT（双指成立）
    add2(200,400,300,400);       // step2 双指按住（无位移）
    add(200,400,2,0);            // step3 过渡帧：count 仍 2、只有一点有坐标 → 双指收尾
    add(0,0,0,0);                // step4 真正的释放边沿：陈旧 PRESS 若还在，这一拍收 TAP
    add(0,0,0,0);                // step5 余量（TAP 可能需要下一拍才结算）
    run();
    assert(events[UI_GESTURE_CANCEL]==1);   // 撤销真的发生了（不是「恰好什么都没发生」）
    assert(events[UI_GESTURE_TAP]==0);      // 幽灵 TAP 归零
    assert(dual_events[UI_DUAL_EVT_RETRACT]==1);
    // 双指快点一下（畸形输入，本轮新增验收项）：融合窗内抬手 → 有名字的 slip 中止。
    reset();
    first.on_gesture=gesture;
    first.on_dual_gesture=dual_ev;
    add(200,400,1,0);
    add2(200,400,300,400);
    add(0,0,0,0);
    add(0,0,0,0);
    add(0,0,0,0);
    run();
    assert(events[UI_GESTURE_TAP]==0&&events[UI_GESTURE_CANCEL]==1);
    assert(dual_events[UI_DUAL_EVT_RETRACT]==1);
    // 中止**不上页面**：on_dual_gesture 只收 fired 事件，中止只写日志——这条断言把
    // 「静默」钉成结构（它没有别的去处），也让「加日志」不会被误读成「多了一条通路」。
    // An abort never reaches the page: on_dual_gesture only receives fired events, so
    // an abort exists only as a log line. Pinning that keeps "silent" structural.
    assert(dual_aborts==0);
    puts("PASS: 26.9.7 ghost TAP: two fingers landing now revoke the stale single-finger PRESS, so no TAP is replayed on lift; a quick two-finger tap aborts by name");

    puts("PASS: M2 Step9 wiring: detail-opening return flow takes the FULL gear, ordinary flows keep PAGE; the KEY_2 exemption fires on press without arming held_key, while deferred-menu semantics stay intact outside a detail session");
    puts("PASS: gen26 AI definition poll: filled+accepted presents the card and drops the tick's press, refused keeps normal dispatch, empty slot has zero side effects");
    puts("PASS: real loop and power recognizer: DU-only contact, release commits off/staged sleep/restart once, cancel restores full navigation, touch/multitouch errors and entry-held suppression");
    puts("PASS: shutdown dialog gatekeepers: landscape long-press resets the dialog branch to portrait, and cancel repaints through render() to restore landscape");
    puts("PASS: gen21 manual-sleep gatekeeper: armed manual sleep silently disarms (one cancel_why \"manual sleep\") then sleeps; armed auto lock stays blocked with no cancel");

    // S26-4 / E-2：插卡只**开一次窗**。三个样本 = 无卡 / 插卡 / 继续在位。
    // 判据从「边沿被叫了几次」换成「窗口被开了几次」：现在调用点是每拍都调（E-2 要求
    // 观测与写卡贴在一起），幂等由被调方守。`if (ready)` 那种写法会在这里数出 2 次开窗，
    // 队列也就永远等不到能落盘的那一拍——所以这条断言守的正是同一个坑，只是换了计数口。
    // S26-4 / E-2: the window opens exactly once. The call site now fires every turn (E-2
    // wants observation adjacent to the write), and idempotence lives in the callee. The
    // `if (ready)` shape would still count 2 re-arms here - same trap, different counter.
    reset();media_test=true;time_step=600000;
    add(0,0,0,0);
    present_steps[1]=mounted_steps[1]=true;epoch_steps[1]=1u;
    present_steps[2]=mounted_steps[2]=true;epoch_steps[2]=1u;
    add(0,0,0,0);add(0,0,0,0);
    run();
    assert(excerpt_yield_rearms==1&&excerpt_mount_step==1&&excerpt_mount_at==1200u);
    assert(excerpt_mount_calls>1);   // 每拍都调，但只开一次窗 / called every turn, armed once
    // 开机就带着卡：**这里的行为变了，且是有意的**。旧判据「没有『获得』边沿所以不打扰」
    // 是状态差分的副产物，不是 S26-4 的意图——S26-4 要躲的是**书架扫描**，而开机那一段
    // 恰好是扫描最密集的时候。代号是事件：开机首挂 = 1，照样开窗。队列此时为空，代价为零。
    // A card present at boot now DOES arm the window. The old "no edge, no yield" reading was
    // a side effect of state differencing, not S26-4's intent - S26-4 exists to dodge the
    // shelf scan, which is densest right after boot. An event has no such special case.
    reset();media_test=true;time_step=600000;
    present_steps[0]=mounted_steps[0]=true;present_steps[1]=mounted_steps[1]=true;
    epoch_steps[0]=1u;epoch_steps[1]=1u;
    add(0,0,0,0);add(0,0,0,0);
    run();
    assert(excerpt_yield_rearms==1);
    puts("PASS: 26.9.27 S26-4 / E-2 mount yield: a mount arms the window exactly once even though the call site fires every turn; a card already present at boot arms it too (the epoch is an event, and boot-time is exactly when the shelf scan runs); staying mounted never re-arms it");

    // 26.11.1 用例 20：两拍提交泵的真身。出包门第 3 条 7b 的 busy 路径真机没走到
    // （`dropped after` 0 / `ERR(7)` 0），宿主这边原是空桩——两头都没覆盖。用户的
    // 拒绝（「连翻 20 次不是人干的事」）把这条路封成唯一合法通道：宿主跑真身。
    // 四段覆盖：正常两拍 / 下一帧重试成功 / 越过死线 abort / 无请求空转。
    // Case 20: the two-beat pump, real body. 7b's busy path never fired on hardware
    // and the host side was an empty stub - neither end covered it. The user's refusal
    // ("tapping 20 times is not a human action") left this as the only legitimate
    // channel. Four segments: happy beats / retry succeeds next frame / past-deadline
    // abort / idle with no request.
    commit_take_hit=true; commit_release_ok=true; commit_run_ok=true;
    commit_run_calls=commit_abort_calls=0; time_step=0; time_offset=0;
    transfer_commit_pump();
    assert(commit_run_calls==1 && commit_abort_calls==0);
    assert(!transfer_commit_waiting() && transfer_commit_beat()==1);
    // 下一帧重试成功：第一次关不掉（拍号 1→2），第二帧关掉 → 走拍 2/2 并跑四拍。
    // Retry succeeds next frame: first frame cannot close (beat 1->2), the second
    // closes -> beat 2/2 runs the four beats.
    commit_take_hit=true; commit_release_ok=false; commit_run_ok=true;
    commit_run_calls=commit_abort_calls=0; time_offset=0;
    transfer_commit_pump();
    assert(transfer_commit_waiting() && transfer_commit_beat()==2 && commit_run_calls==0);
    // esp_timer_get_time() 是微秒（真语义），毫秒数要乘 1000。
    // esp_timer_get_time() is microseconds; milliseconds must be multiplied by 1000.
    commit_release_ok=true; time_offset=1000*1000;
    transfer_commit_pump();
    assert(!transfer_commit_waiting() && commit_run_calls==1 && commit_abort_calls==0);
    // 越过死线：1500 ms 到点仍关不掉 → 清等待态 + abort(ERR_BUSY)，四拍一次都不跑。
    // Past the deadline: still held at 1500 ms -> clear the flag and abort(ERR_BUSY),
    // without running the four beats even once.
    commit_take_hit=true; commit_release_ok=false; commit_run_ok=true; commit_run_calls=0;
    commit_abort_calls=0; commit_waiting_at_abort=true; time_offset=0;
    transfer_commit_pump();                       // 拍 1，等
    assert(transfer_commit_waiting() && transfer_commit_beat()==2);
    time_offset=1400*1000; transfer_commit_pump(); // 死线内，再等一帧
    assert(transfer_commit_waiting() && transfer_commit_beat()==3 && commit_abort_calls==0);
    time_offset=1500*1000; transfer_commit_pump(); // 到点 → 放弃
    assert(commit_abort_calls==1 && commit_abort_err_ok);
    assert(!commit_waiting_at_abort && !transfer_commit_waiting());
    assert(commit_run_calls==0);
    // 无请求空转：take 不命中就什么都不做，等待态不会被凭空置起。
    // Idle with no request: nothing happens and the flag is never set from thin air.
    commit_take_hit=false; commit_run_calls=commit_abort_calls=0;
    transfer_commit_pump(); transfer_commit_pump();
    assert(!transfer_commit_waiting() && commit_run_calls==0 && commit_abort_calls==0);
    puts("PASS: 26.11.1 case 20 commit pump (real body): happy two beats, retry succeeds next frame, past-deadline abort clears the wait flag before aborting with ERR(BUSY) and never runs the four beats, idle frames do nothing");
    puts("app_loop: 60 scheduler scenarios passed, including automatic lock, save callback, input reset, busy guards, the shutdown dialog rotation gatekeepers, the gen21 manual-sleep push-arm gatekeeper, the 26.9.17 hot-plug loop (removal releases, reinsertion re-probes) and the 31b P1-1 I/O-dead path (CD present, I/O gone, same-tick lost, B42 discard on the edge), plus the release latch that never gives up (a FILE* that never closes keeps it frozen with exactly one hold mark and unmounts through the gate the moment it stops)");
    return 0;
}

/* 中文：硬件 BLE 与联网任务替身。/ English: Hardware BLE and online worker shims. */
void ble_pt_sync(bool enabled) { (void)enabled; }
void ble_pt_poll(void) {}
bool pico_online_busy(void) { return online_busy; }

/* 中文：push_sink 调度面替身。宿主默认未武装：翻页器连接、睡眠调度与对话框
   逻辑因此不受推流影响；武装页的渲染路径由 transfer_ui 回归单独覆盖。
   English: scheduling shims for push_sink. The host defaults to unarmed, so the
   page-turner link, sleep scheduling and dialog logic stay unaffected; the arm
   page's rendering paths are covered by the transfer_ui regression alone. */
bool push_sink_armed(void) { return push_armed; }
// 31b 第 1 条：三拍桩。quiesce 成功与否由用例控制——失败那一路必须证明
// 「一次 remount 都不许发生」，那正是三拍存在的理由。
// The three-beat stub. The failure case must prove zero remounts: that is the
// whole point of the three beats.
bool push_sink_quiesce(unsigned timeout_ms) {
    (void)timeout_ms;
    ++quiesce_calls;
    remounts_before_quiesce = remounts;
    return quiesce_ok;
}
void push_sink_thaw(const char *why) { (void)why; ++thaw_calls; }
void push_sink_quiesce_hold(const char *why, push_hold_why_t why_code) {
    (void)why;
    (void)why_code; // 桩只看次数；原因码由结构牙 B56 咬 / count only; the code is held by B56
    ++hold_calls;
}
// 26.11.5：介质离开/插回这一对。桩只记次数——「谁先谁后」由结构牙 B78 咬，
// 「断了就还」由真机第 4 档咬；宿主这里只要链接过得去、调用次数可数。
// 26.11.5: the media detach/reattach pair. The stub counts calls only - the order is
// held by B78 and "cut then repay" by field step 4; the host just needs to link.
void push_sink_detach_media(const char *why) { (void)why; ++detach_calls; }
bool push_sink_resume_after_media(void) { ++resume_calls; return resume_ok; }
bool push_sink_frozen(void) { return false; }
unsigned push_sink_live_files(void) { return 0; }
bool push_sink_take_freeze_timeout(unsigned *frozen_ms) { (void)frozen_ms; return false; }
void push_sink_poll(void) {}
// 26.9.30 commit 2 / 26.11.1：提交泵的两个链接点。
// commit 2 时这里只是空桩（「行为断言留给用例 20」，而用例 20 一直没落）；26.11.1
// 补课时把泵的真身编进来（main/apps/transfer_commit.c），下面三个 push_sink 入口和
// app_book_release_hold 全部改成**可控桩**——宿主因此能跑真身的两拍状态机，而不是
// 给测试另写一套复制品（复制品会漂移，见 transfer_commit.h 头注）。
// Commit 2 left this an empty stub ("behaviour assertions come with case 20", which
// never landed). The 2026-11-01 top-up builds the pump's real body in
// (main/apps/transfer_commit.c) and turns the three push_sink entries plus
// app_book_release_hold into controllable stubs - so the host runs the real two-beat
// machine instead of a test-only copy (copies drift, see transfer_commit.h).
// 控制量与前面顶部同一组（定义在文件上方，此处不重复声明）。
// The controls are the group defined near the top; nothing is redeclared here.
bool push_sink_take_commit_request(void) {
    const bool hit = commit_take_hit;
    commit_take_hit = false;          // 读即清，与真实 take 一致 / read-and-clear like the real take
    return hit;
}
bool push_sink_run_commit(void) { ++commit_run_calls; return commit_run_ok; }
void push_sink_commit_abort(int err) {
    ++commit_abort_calls;
    commit_abort_err = err;
    commit_abort_err_ok = (err == PUSH_ERR_BUSY);
    // 26.9.30 commit 4 核销钉的第 1 条：abort 之前泵内等待态必须已经归零。
    // 在这里采样，而不是事后问——事后问区分不出「先清后 abort」与「abort 后清」。
    // Audit nail #1 (commit 4): the wait flag must already be zero when abort is
    // called. Sampled here, not afterwards - afterwards cannot tell "clear then
    // abort" from "abort then clear".
    commit_waiting_at_abort = transfer_commit_waiting();
}
bool app_book_release_hold(const char *path) {
    assert(path && strstr(path, "push.txt"));
    return commit_release_ok;
}
void transfer_pump_commit(void) { transfer_commit_pump(); }
// gen26 AI 释义回流替身（M1 漏同步、两 runner 编译红躺到 gen26.3 补）：
// take 命中即取空（与真实 UI 槽语义一致），ready 的可受理性由 ai_ready_ok 编程。
// gen26 AI definition return shims (M1 missed these and two runners stayed
// compile-red until gen26.3): a take hit drains the slot (matching the real UI
// slot), acceptance is programmed by ai_ready_ok.
bool push_sink_take_ai_def(char *word, size_t wcap, char *def, size_t dcap) {
    if (!ai_take_hit) return false;
    ai_take_hit = false;
    snprintf(word, wcap, "serf");
    snprintf(def, dcap, "a person bound to the land");
    return true;
}
// M3-B：情景认领桩。默认永不认领（返回 false），让既有释义回流用例照旧走
// ai_def_ready；专门的情景用例把它打开并断言「认领后不进生词批」。
// M3-B: the scene-claim stub. False by default so existing definition-return
// cases still reach ai_def_ready; the scene case turns it on and asserts that a
// claimed answer never enters the vocabulary batch.
bool app_book_scene_def_ready(const char *word, const char *def) {
    (void)def;
    ++scene_claim_calls;
    scene_claim_word[0] = 0;
    if (word) snprintf(scene_claim_word, sizeof(scene_claim_word), "%s", word);
    return scene_claim_ok;
}
bool app_book_ai_def_ready(const char *word, const char *def) {
    ++ai_ready_calls;
    snprintf(ai_seen_word, sizeof(ai_seen_word), "%s", word);
    snprintf(ai_seen_def, sizeof(ai_seen_def), "%s", def);
    return ai_ready_ok;
}
// M2 Step9：双 GC16 档位查询桩。一次性消费语义与 app_book.c 的真实 getter
// 一致——app_loop 的回流点每轮回流只查一次，查完即清，故守门可以用两次
// 回流验证「开详解 FULL、随后回落 PAGE」。非消费式会让第二轮回流错误地
// 再吃一次 FULL，守门立刻红。
// M2 Step9: the double-GC16 gear query stub. Its one-shot consumption mirrors
// the real getter in app_book.c - app_loop's return flow queries it once per
// flow and it clears on read, so the gatekeeper can drive two flows and see
// "detail-opening takes FULL, the next falls back to PAGE". A non-consuming
// stub would wrongly feed the second flow another FULL and turn the gate red.
bool app_book_ai_def_full(void) {
    const bool full = ai_def_full_ok;
    ai_def_full_ok = false;
    return full;
}
// M2 Step9：详解活跃谓词桩（app_loop 的 KEY_2 豁免查询）。非一次性——豁免
// 判定发生在每次 owns_keys 按键分发，会话期内每次查询都须如实作答。
// M2 Step9: the detail-active predicate stub (app_loop's KEY_2 exemption
// query). Not one-shot - the exemption is evaluated on every owns_keys key
// dispatch, so every query during a session must answer truthfully.
bool app_book_ai_detail_active(void) { return ai_detail_active; }
// gen21：手动息屏经 prepare_lock(manual=true) 调 cancel_why("manual sleep")。
// 记录调用次数与 why 文本，供守门用例断言「武装期手动息屏静默取消武装」。
// 此桩自 gen21（2ea51cb）起漏同步补定义，宿主链接失败躺了三代——教训同
// gen19：实现契约变更必须同步进宿主桩，否则红灯静默躺代。
// gen21: manual sleep reaches cancel_why("manual sleep") through
// prepare_lock(manual=true). Record the call count and why text so the
// gatekeeper can assert "manual sleep while armed silently disarms". This
// stub missed its definition since gen21 (2ea51cb) and the host link failed
// for three generations - same lesson as gen19: implementation-contract
// changes must be mirrored into the host stubs, or red lights lie dormant.
void push_sink_cancel_why(const char *why) {
    ++push_cancel_calls;
    snprintf(push_cancel_why, sizeof(push_cancel_why), "%s", why);
}

uint8_t app_settings_auto_lock_minutes(void){return idle_minutes;}
uint32_t ble_pt_input_serial(void){return 0;}


void crash_note_log(void){}

void app_time_service_poll(bool idle) { (void)idle; }

void ui_avatar_invalidate(void) {}
bool book_cover_jobs_quiesce(unsigned timeout){(void)timeout;return true;}
// B42：插拔边沿只作废旧一代封面。桩计数，供「边沿必须作废」的接线断言。
void book_cover_jobs_discard(void){++cover_discards;}
void book_lock_collage_invalidate(void) {}

bool ble_pt_connected(void) { return false; }
