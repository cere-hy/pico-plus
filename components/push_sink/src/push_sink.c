/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：push_sink 的 NimBLE 胶水层。职责边界照设计草案 §2：本文件只做「薄胶水」——
 * 帧编解码、状态机转移、CRC 校验、断点续传逻辑全部在纯核（push_proto/push_session，
 * 宿主可测）；这里只负责把 NimBLE 回调翻译成事件、按返回的动作位掩码执行副作用
 * （写 .part、NVS 记账、notify、广播、连接参数）。绝不碰 framebuffer，不做文本
 * 清洗——字节原样落盘，CRC 契约才成立；清洗在手机侧（push_pico.py）。
 * English: the NimBLE glue of push_sink. Per design draft section 2 this file is
 * deliberately thin: frame codec, state-machine transitions, CRC checks and the
 * resume arithmetic all live in the host-testable pure core; here NimBLE
 * callbacks are translated into events and the returned action bits are executed
 * as side effects (partial writes, NVS bookkeeping, notify, advertising,
 * connection parameters). No framebuffer access and no text cleaning - bytes land
 * as received so the CRC contract holds; cleaning happens phone-side.
 *
 * 锁序：s_lock → ble_hs_lock，单向。所有 NimBLE 事件回调都在不持 ble_hs_lock 的
 * 上下文里调用（ATT 写路径有 BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task()) 为证），
 * 因此互斥锁用 FreeRTOS mutex——VERIFY 回读 512KB 算 CRC 要几百毫秒，自旋锁会
 * 把主循环和看门狗一起掐住。
 * Lock order: s_lock -> ble_hs_lock, one-way. Every NimBLE event callback runs
 * without ble_hs_lock held (the ATT write path asserts exactly that), so the
 * mutex is a FreeRTOS mutex: the VERIFY read-back of a 512 KB body takes hundreds
 * of milliseconds and a spinlock would choke the main loop and the watchdog.
 */

#include "push_part.h"
#include "push_sink.h"

#include "pico_bootbb.h" // 31b 第 1 条补账：hold 留痕进黑匣子 / hold leaves a blackbox note

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_attr.h" // EXT_RAM_BSS_ATTR：26.11.3-C2 起长文本槽落 PSRAM
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "pico_heapdiag.h"

#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_uuid.h"
#include "os/os_mbuf.h"

#include "read_pico_pmu.h"

#include "ai_ridealong.h"
#include "ai_segasm.h"
#include "push_tts.h"

static const char *TAG = "push_sink";

/* ---- 常量 / Constants ---- */

// GATT 服务 UUID（草案 §3.1，写死；特征取尾字节 5f11/5f12/5f13）。
// The GATT service UUID (draft section 3.1); characteristics are ...5f11/5f12/5f13.
#define PUSH_SVC_UUID_TAIL 0x10u
#define PUSH_DATA_UUID_TAIL 0x11u
#define PUSH_CTRL_UUID_TAIL 0x12u
#define PUSH_STATUS_UUID_TAIL 0x13u

// 广播间隔：武装期 1 秒一发，手机扫得到又不吵。/ Advertising interval: once a
// second while armed - findable by the phone without being noisy.
#define PUSH_ADV_ITVL 1600u

// 传输期连接参数（草案 §3.6）：15ms/latency 0；结束后回退 500ms/latency 4 省电。
// Transfer-time connection parameters (draft section 3.6): 15 ms / latency 0;
// back to 500 ms / latency 4 afterwards to save power.
#define PUSH_CONN_ITVL_FAIR 12u   // 15ms / 1.25ms
#define PUSH_CONN_ITVL_SLOW 400u  // 500ms / 1.25ms
#define PUSH_CONN_LATENCY_FAST 0u
#define PUSH_CONN_LATENCY_SLOW 4u
#define PUSH_CONN_SUP_TIMEOUT 800u // 8s：与 central 侧保底一致 / matches the central-side floor

// PROGRESS 通知节流：每包都发会把 ATT 通道淹掉。/ PROGRESS notify throttle:
// one per packet would flood the ATT channel.
#define PUSH_PROGRESS_MIN_GAP_MS 400u

// 提交段封送（26.9.30 commit 2）：BLE 回调只投递请求，四拍由主循环在帧间执行。
// 2 s 上界的起点是**投递时刻**（s_ui.commit_req_ms），不是取出时刻——从取出算起
// 的上界会随主循环每帧重置，等于没有上界。
// Commit hand-off: the BLE callback only posts; the four-beat dance runs on the main
// loop between frames. The 2 s cap is anchored at POST time (s_ui.commit_req_ms),
// not pickup time - a bound measured from pickup would restart every frame and bound
// nothing.
#define PUSH_COMMIT_WAIT_MS 2000u
#define PUSH_COMMIT_CARRY_OPEN 0x01u // 同拍带的 OPEN_BOOK / carried OPEN_BOOK
#define PUSH_COMMIT_CARRY_WORD 0x02u // 同拍带的 WORD_CARD / carried WORD_CARD

// NVS 会话检查点节流：每 64 包落一次，断连/END 必落。/ NVS checkpoint cadence:
// every 64 packets, plus mandatory on disconnect and END.
#define PUSH_NVS_CHECKPOINT_EVERY 64u

// 武装窗口默认时长：UI「接收推送」的默认档，也是「要常开但没在充电」时的
// 软着陆（组件层防御，防武装挂成永不到期）。/ Default arm-window length: the
// UI's default slot, also the soft landing for a hold request off the charger
// (a glue-side guard against a permanent arm).
#define PUSH_WINDOW_DEFAULT_MS 1800000u

// 续推宽限：推送成功后回窗口前给手机的缓冲，让 DONE/EVT_READ 有时间往返。
// Post-push grace: the buffer between a completed push and the return to the
// window, giving DONE/EVT_READ time to make the round trip.
#define PUSH_COOLDOWN_MS 8000u

// 电量门槛（草案 §4.2）：低于 10% 且未充电拒绝武装与推送。/ Battery floor
// (draft section 4.2): below 10 percent off the charger, arm and push are refused.
#define PUSH_BATT_FLOOR_PERCENT 10

// 充电状态刷新节流（poll 内 I2C 交易）/ charge-state refresh cadence (I2C traffic inside poll)
#define PUSH_PMU_REFRESH_MS 1000u

// NVS 命名空间与键。/ NVS namespace and key.
#define PUSH_NVS_NS "rp_pushsink"
#define PUSH_NVS_KEY_SESS "sess"
#define PUSH_NVS_KEY_UNREAD "unread" // gen21 未读位（独立 u8 key，不动 sess blob 布局）
#define PUSH_NVS_KEY_AI "ai_en"      // gen26 AI 查词开关位（独立 u8 key）
#define PUSH_NVS_KEY_SCENE "sc_en"   // M3 情景解读开关位（独立 u8 key，缺省开）
#define PUSH_NVS_MAGIC 0x50534E31u // 'P','S','N','1'

// .part 回读算 CRC 的分块大小。/ Chunk size for the partial-file CRC read-back.
#define PUSH_CRC_CHUNK 512u

/* ---- UUID 声明（静态存储期）/ UUID declarations (static storage) ---- */

// BLE_UUID128_INIT 按小端（与字符串相反的顺序）取 16 字节。UUID 字符串
// e2f5a1c0-9b7d-4e3f-8a61-3c9d2b7e5f10 逆序即下面的初值；尾字节 0x10/0x11/0x12/0x13
// 落在小端数组的第 0 位（字符串的最后一位）。
// BLE_UUID128_INIT takes the 16 bytes little-endian (reverse of the string). The
// string e2f5a1c0-9b7d-4e3f-8a61-3c9d2b7e5f10 reverses to the values below; the
// tail byte 0x10/0x11/0x12/0x13 lands at little-endian index 0.
#define PUSH_UUID128_TAIL(t)                                                   \
    {                                                                           \
        .u = {.type = BLE_UUID_TYPE_128},                                       \
        .value = {(t), 0x5fu, 0x7eu, 0x2bu, 0x9du, 0x3cu, 0x61u, 0x8au, 0x3fu, \
                  0x4eu, 0x7du, 0x9bu, 0xc0u, 0xa1u, 0xf5u, 0xe2u},              \
    }

static const ble_uuid128_t s_svc_uuid = PUSH_UUID128_TAIL(PUSH_SVC_UUID_TAIL);
static const ble_uuid128_t s_data_uuid = PUSH_UUID128_TAIL(PUSH_DATA_UUID_TAIL);
static const ble_uuid128_t s_ctrl_uuid = PUSH_UUID128_TAIL(PUSH_CTRL_UUID_TAIL);
static const ble_uuid128_t s_status_uuid = PUSH_UUID128_TAIL(PUSH_STATUS_UUID_TAIL);

/* ---- 模块状态 / Module state ---- */

// NVS 会话元数据：与 .part 长度对账的锚。/ NVS session metadata: the anchor that
// reconciles against the partial-file length.
typedef struct {
    uint32_t magic;
    uint32_t received;
    uint16_t next_seq;
    uint16_t reserved;
    push_begin_t begin;
} push_nvs_session_t;

static SemaphoreHandle_t s_lock;      // 互斥锁：见文件头的锁序说明 / the mutex, see the lock-order note
static bool s_inited;

static push_session_t s_sess;         // 状态机簿记 / the state machine

static bool s_pending_arm;            // 栈未就绪时的待武装 / an arm waiting for the stack
static uint16_t s_pending_token;
static uint32_t s_pending_window_ms;
static uint32_t s_pending_at_ms;
static bool s_charger_held;           // 充电时常开：poll 看守拔电 / charger-held: poll watches for unplug

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint8_t s_own_addr_type;
static bool s_gatt_queued;              // 服务定义已排进 NimBLE 注册队列（本启动周期）
                                         // the definitions are queued this start cycle
static uint16_t s_data_handle, s_ctrl_handle, s_status_handle;
static bool s_stack_up;                 // 栈是否在运行（钩子维护，替代对 ble_pt_running 的依赖）
                                         // whether the stack runs (kept by the hooks, no ble_page_turner dependency)
static bool s_status_subscribed;        // CCCD 订阅态（BLE_GAP_EVENT_SUBSCRIBE 维护）
                                         // CCCD subscription state (maintained by BLE_GAP_EVENT_SUBSCRIBE)

static char s_adv_name[32];           // 当前广播名 / the current advertising name
// gen22 蓝牙校时：手机下发的真值暂存在这里，由 main 层（time_service）消费后
// 做漂移学习并同步 PMU——组件不反向调用 main。/ Clock truth from the phone lands
// here; main (time_service) consumes it for drift learning and PMU sync - the
// component never calls back into main.
static uint32_t s_remote_utc;
static bool s_remote_utc_pending;
static char s_close_why[48];          // gen18：最近一次收摊原因（UI 取回展示）
                                     // gen18: the most recent close-up reason (read back by the UI)

static uint32_t s_last_prog_ms;       // PROGRESS 节流时钟 / the PROGRESS throttle clock
static uint32_t s_pkts_since_ckpt;    // 距上次 NVS 检查点的包数 / packets since the last NVS checkpoint
static uint32_t s_pmu_refresh_ms;     // 上次 PMU 刷新时刻 / last PMU refresh time
static uint8_t s_last_status[PUSH_STATUS_MAX]; // 最近一条 STATUS（读特征的回退通道）
static size_t s_last_status_len;               // the last STATUS (the read fallback)

/* ---- gen30 伴读（R0 冻结件 1 · 方案 B 裸帧）：收帧对账与 take 槽 ----------
 * 照 gen22 s_remote_utc / gen26 s_ui.ai_def 的「组件只存、app 轮询取」体例：
 * 零新任务、零队列、组件不反向调用 main。长文本槽落 PSRAM（EXT_RAM_BSS_ATTR，
 * 26.11.3-C2 先例），内部 RAM 只增几个标量。
 * 页号对账基准 = 最近一次 PAGE_BEGIN 发出的页号；SUMMARY/TTS_DONE 的 page_no
 * 与它不符一律丢弃（R0 §一对账规则），迟到回执不会误刷右栏、误翻页。
 *
 * gen30 companion: receive-side reconciliation + take slots. The component
 * stores, the app polls - no new task/queue and never a call back into main.
 * The long text slot lives in PSRAM; only scalars land in internal RAM. The
 * reconciliation base is the page_no of the last PAGE_BEGIN emitted; mismatching
 * SUMMARY/TTS_DONE frames are dropped (R0 frozen item 1), so a late receipt can
 * never cross pages.
 */
static uint16_t s_companion_page_no;        // 最近 PAGE_BEGIN 的页号 / last PAGE_BEGIN page_no
static bool s_companion_page_valid;         // 是否已发过 PAGE_BEGIN / a PAGE_BEGIN has been emitted
static bool s_companion_conn_fair;          // 伴读活跃期 FAIR 档 / companion-active FAIR latch
static EXT_RAM_BSS_ATTR char s_companion_summary_text[PUSH_SUMMARY_TOTAL_MAX + 1u]; // PSRAM 摘要槽
/* 多帧拼装暂存（修复轮二轮）：同一页的 idx 连续片先落这里，收齐才搬进上面的交付槽。
   两条槽都在 PSRAM，内部 RAM 仍只增几个标量（与「组件只存、app 轮询取」同体例）。 */
static EXT_RAM_BSS_ATTR char s_summary_acc[PUSH_SUMMARY_TOTAL_MAX + 1u];
static uint16_t s_summary_acc_len;          // 已拼字节数 / bytes accumulated
static uint16_t s_summary_acc_page;         // 暂存所属页号 / page this accumulator belongs to
static uint8_t s_summary_acc_next;          // 下一个期望的 idx / next expected idx
static uint8_t s_companion_summary_mode, s_companion_summary_idx, s_companion_summary_total;
static uint16_t s_companion_summary_page;
static uint16_t s_companion_summary_len;
static bool s_companion_summary_pending;    // 一条未取的有效 SUMMARY / one valid summary waiting
static uint16_t s_companion_tts_page;
static uint8_t s_companion_tts_status;
static bool s_companion_tts_pending;        // 一条未取的有效 TTS_DONE / one valid receipt waiting

/* ---- 31b 第 1 条：三拍封送（freeze / thaw） -------------------------------
 * 冻结不是「暂停接收」，是「三拍区间内不许任何会话进来」——掉卡时 ARM 的
 * adopt 会 truncate 一个正在被卸载的卷（扩句 B 的原话）。三个量缺一不可：
 *
 *   s_freeze_flags 入口闸 + 毒卷标记 + 一次性事件，压在同一个字节里：
 *                  FRZ_FROZEN 三处判点读它（on_ctrl_write / on_data_write / try_adopt）
 *                  FRZ_HOLD   毒卷显式保持：闩锁全败后，看门狗必须放过它（那不是卡死）
 *                  FRZ_TIMEOUT 看门狗强制解冻的一次性事件，等 app 取走写黑匣子
 *   s_frozen_ms    冻结时刻；看门狗触发时**改写为已冻结毫秒数**（复用，省 4 B）
 *   s_verify_depth **判据②的测量值**：do_verify 的 FILE* 是局部变量，函数返回即
 *                  消失，外部根本无从观测。进出各加减一次，它才从「顺序推断」
 *                  变成「可查询事实」——这正是 B51 被降格时给的方向。
 *
 * 用 s_ui_lock（自旋锁）保护：BLE 回调栈上也要读，互斥锁在那里不能阻塞。
 * The freeze is not "pause receiving": no session may enter while the three-beat
 * runs. s_verify_depth is the measurement that turns the verify FILE* from an
 * ordering inference into a fact an outsider can read.
 */
// 三个布尔压成一个标志字节、深度压成 uint8、时长复用 s_frozen_ms：
// 这不是「省内存美学」，是 31b 的 +32 B 预算（第 2 条已用掉 16 B）。四个量分开写
// 时段增量为 32 B，合并后 16 B——正好把整批压在预算内，不用回去重议。
// Three booleans into one flag byte, the depth into a uint8, and the duration
// reuses s_frozen_ms: the 31b budget is +32 B and item 2 already spent 16. Split
// apart the section grew by 32 B; merged it grows by 16 and the batch fits.
#define FRZ_FROZEN  0x01u
#define FRZ_HOLD    0x02u
#define FRZ_TIMEOUT 0x04u
static uint8_t s_freeze_flags;
static uint32_t s_frozen_ms;   // 冻结时刻；看门狗触发时改写为「已冻结多久」/ start, then the elapsed
static uint8_t s_verify_depth;

// 看门狗：闩锁最长 9 s（3 × (2 s quiesce + 1 s 间隔)）+ 卸卷余量。
// 超时只抓「终态枚举漏项 / 流程卡死」，不碰合法的毒卷保持态。
// Watchdog: the latch can legitimately take 9 s, plus room for the release. It
// only catches a missing terminal branch or a stuck flow - never the legal
// poisoned-volume hold.
#define PUSH_FREEZE_WATCHDOG_MS 20000u

// gen14：notify 空口失败重投。/ gen14: retry when a notify fails on air.
// ble_gatts_notify_custom 返回 0 只代表 HCI 命令入了队，真正的空口结果在
// BLE_GAP_EVENT_NOTIFY_TX 的 status；两个失败点过去都只打 LOGD（release 不可见），
// 真机症状正是「设备明明处理了，手机就是等不到 STATUS」——gen13 485E 第二周期
// 与第三方调试助手「收: 0」都指向这里。现在失败升级 LOGW，并用 esp_timer 延迟
// 100ms 重发 s_last_status，每个连接周期 3 次配额。
// A zero return from ble_gatts_notify_custom only means the HCI command was
// queued; the real air result is NOTIFY_TX's status. Both failure points used
// to log at LOGD only (invisible in release) - exactly the "the device clearly
// processed it, yet the phone waits forever" symptom seen on gen13 hardware
// (485E second cycle) and with the third-party debugger ("received: 0").
// Failures now escalate to LOGW and re-send s_last_status after 100 ms via
// esp_timer, three attempts per connection cycle.
static esp_timer_handle_t s_notify_retry_timer;
static uint8_t s_notify_retry_left;

// ACT_APPEND 需要的刚收载荷：写回调的帧缓冲在回调期间有效。/ The just-received
// payload ACT_APPEND needs: the write callback's frame buffer lives for the call.
static const uint8_t *s_rx_payload;
static size_t s_rx_len;

// UI 一次性事件槽（自旋锁保护：拷贝进出都是几条指令）/ one-shot UI slots (a
// spinlock suffices: each copy is a few instructions).
static portMUX_TYPE s_ui_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    bool open;
    char title[PUSH_TITLE_MAX];
    uint8_t type;
    bool word_card; // 单词卡待取标志（区别于下面的单词正文）/ the pending word-card flag (distinct from the word text below)
    char word[PUSH_TITLE_MAX];
    int err;
    bool closed;
    bool start_failed;
    bool committed; // gen19（Bug5）：一次正文已落盘，待首页消费以刷新“最近推送” / a body committed, pending home consumption to refresh "last push"
    char committed_title[PUSH_TITLE_MAX];
    uint8_t committed_type;
    bool unread; // gen21：正文已落盘且尚未被打开（首页「有新内容」徽章）/ a body landed and not yet opened (home "new content" badge)
    // 26.9.30 commit 2：提交段的投递槽。BLE 回调写、主循环读即清，两者都在
    // s_ui_lock 下；commit_carry 暂存同拍扣下的 OPEN_BOOK / WORD_CARD——
    // 提交还没发生就开书，打开的是上一本（gen15 真机现象）。
    // The commit hand-off slot: written by the BLE callback, read-and-cleared by the
    // main loop, both under s_ui_lock. commit_carry parks the OPEN_BOOK / WORD_CARD
    // bits withheld in the same beat - opening before the commit lands shows the
    // PREVIOUS book (the gen15 hardware symptom).
    bool commit_request;
    uint32_t commit_req_ms; // 投递时刻，2 s 上界的起点 / post time, the 2 s anchor
    uint8_t commit_carry;   // PUSH_COMMIT_CARRY_* / carried UI bits
    // 26.9.30 commit 4（审计附注 1）：一次 take 只许一条终态出口。
    // take 成功置位；run_commit 或 abort 任一条路都必须清掉，第二条到达者是 no-op。
    // 没有它，死线外的 abort 只清 carry 不清「谁还欠这次提交」——泵每帧都进，
    // 下一帧 now > deadline 仍成立，要么每帧重复 abort（重复 reply 破坏会话），
    // 要么读者恰在此间释放、顺着 run_commit 走下去，abort 被静默绕过。
    // One take, exactly one terminal exit. Set by a successful take; cleared by
    // run_commit OR abort, and a second arrival is a no-op. Without it the abort
    // outside the deadline cleared only the carry, not "who still owes this
    // commit" - the pump runs every frame, so now > deadline keeps holding: either
    // a repeated abort (a second reply wrecks the session) or the reader releases
    // in between and run_commit runs anyway, silently bypassing the abort.
    bool commit_live;
    bool ai_def;        // gen26：一条释义待展示（块 4 卡片框架入口）/ a definition pending display (block 4 card entry)
    char ai_word[PUSH_AI_WORD_MAX];    // 待展示单词 / the word to show
    // 待展示释义**不在本结构里**（26.11.3-C2）：它装的是 8 KB 级的段落答案，会把这具
    // 内部 RAM 结构整个撑大 7 KB，而内部 bss 只剩 112 B。见 s_ui_ai_def_text。
    // The pending definition is NOT here (26.11.3-C2): it holds kilobyte-scale passage
    // answers and would grow this internal-RAM struct by 7 KB with 112 B of bss left.
    // See s_ui_ai_def_text.
} s_ui;

static bool (*s_dict_probe)(void); // 词典可用性探针 / the dictionary-availability probe

/* ---- gen26 AI 查词（蓝牙代理 + 搭车协议）---- */
/* gen26 AI lookup (BLE proxy + ride-along). */

// 搭车离线队列缓冲（设备侧 PSRAM，由 main 挂接）。**gen26.6 起退役**：离线
// miss 词不再入队（见 push_sink_ai_tap_word_ctx 内注释），队列恒空，无回流。
// 保留原因：0x06 协议位「登记即冻结」、ai_raloc 宿主测试照跑、app_main 挂接
// 链路不动——死代码面最小化，等下一轮再议整体拆除。
// The ride-along offline buffer (PSRAM on device, attached by main).
// **Retired in gen26.6**: offline misses no longer enqueue (see the comment in
// push_sink_ai_tap_word_ctx), the queue stays empty and nothing reflows. Kept
// because the 0x06 opcode is frozen, the ai_raloc host tests still run and the
// app_main attach path stays untouched - minimal dead surface; full removal is
// a decision for a later round.
static uint8_t *s_ai_buf;
static size_t s_ai_buf_len;
static uint16_t s_ai_cap;
static bool s_ai_ready;   // 缓冲已挂接且 init 成功 / buffer attached and inited
static bool s_ai_enabled; // 运行期开关（NVS 持久化）/ runtime on/off (NVS-persisted)
static bool s_scene_enabled = true; // M3 情景解读（缺省开，NVS 覆盖）/ M3 scene lookup (default on)
// gen26.5 协商状态：APK 用 CTRL 0x09 声明它支持的能力位。**默认 0（未使能）**——
// 设备收使能前 STATUS 0x17 一律 66B legacy，旧 APK 不发 0x09 即零感知兼容；
// 断连清零，重连后等 APK 重发（对端进程可能被杀，能力声明不可跨连接继承）。
// gen26.5 negotiation state: the APK declares what it supports via CTRL 0x09.
// **Defaults to 0** - until then, STATUS 0x17 stays 66B legacy, so an old APK
// that never sends 0x09 notices nothing. Cleared on disconnect: the peer process
// may have been killed, so a capability declaration never survives a connection.
static uint32_t s_apk_caps;
// 设备侧上下文源是否就绪（Step6 由阅读器注册 book_id/pos 采集后置 true）。
// 不就绪就不在 0x18 里报 CAP_AI_CONTEXT——不虚报能力。
// Is the device-side context source ready? (Step6 registers the book_id/pos
// collector.) While false, 0x18 never advertises CAP_AI_CONTEXT - no false claims.
static bool s_ai_ctx_ready;
// 待展示释义槽（26.11.3-C2）：移出 s_ui、落 PSRAM 的**唯一理由**是尺寸——8,192 B
// 在内部 RAM 无处安放。读写仍在 s_ui_lock 的临界区内，两条路径（写 ai_surface、
// 读 push_sink_take_ai_def）语义与从前逐字等价，变的只有它躺在哪块内存里。
// 临界区变长的代价要说清：8 KB memcpy 期间中断关闭约 0.2 ms，占 7.5 ms 连接间隔的
// 3%——可接受，但它是这次搬家唯一的时序代价，记在交付件而不是埋在代码里。
// The pending-definition slot (26.11.3-C2): moved out of s_ui and into PSRAM for one
// reason - size; 8,192 B has nowhere to live in internal RAM. Reads and writes still
// happen inside the s_ui_lock critical section, so both paths are word-for-word what
// they were; only the address space changed. The cost of the longer critical section
// is stated rather than buried: ~0.2 ms with interrupts off during the 8 KB copy, 3%
// of a 7.5 ms connection interval - acceptable, and it is the only timing price.
static EXT_RAM_BSS_ATTR char s_ui_ai_def_text[PUSH_AI_DEF_MAX];

// gen26.5 Step7：0x08 分帧重组器（纯逻辑模块，宿主有断言；设备侧只管喂帧与取结果）。
// 26.11.3-C2：整具搬 PSRAM——它内嵌 8,192 B 缓冲，与上面同一笔账。
// gen26.5 Step7: the 0x08 reassembler (pure module with host assertions; the device
// side only feeds frames and takes the result). 26.11.3-C2: the whole struct lives in
// PSRAM - it embeds an 8,192 B buffer, same ledger as above.
static EXT_RAM_BSS_ATTR ai_segasm_t s_ai_segasm;

// 预取释义缓存（块 3）：最近回流的释义，点词命中即秒出卡，不挤空口。
// 26.11.3-C2：按**词义**预算尺寸化（PUSH_AI_CARD_MAX），不跟传输槽走。它只服务
// 「点词秒出卡」一条路径，装的东西恒为一条词义；跟到 8192 就是 8 条 × 7 KB 的谎。
// Prefetch cache (block 3): recent definitions; a tap on a cached word renders
// instantly without touching the radio. 26.11.3-C2: sized by the WORD budget
// (PUSH_AI_CARD_MAX), not the transport slot - it only ever feeds the instant
// word card, so following the slot would be eight 7 KB lies.
#define AI_CACHE_N 8u
typedef struct {
    char word[PUSH_AI_WORD_MAX];
    char def[PUSH_AI_CARD_MAX];
    bool used;
} ai_cache_t;
static ai_cache_t s_ai_cache[AI_CACHE_N];
_Static_assert(sizeof(s_ai_cache) < 16u * 1024u,
               "26.11.3-C2: the prefetch cache must stay a word-sized cache");
static uint8_t s_ai_cache_next;

/* ---- 小助手 / Small helpers ---- */

// 定义在后的静态函数：adv_start 与 GATT 表要提前引用。/ Static functions defined
// later: adv_start and the GATT table reference them ahead of their definitions.
static void exec_actions(uint32_t act);
static int push_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg);
static int push_gap_cb(struct ble_gap_event *event, void *arg);
static void ui_post_start_failed(void); // adv_start 失败即报，UI 别显假倒计时 / report an adv failure, no fake countdown

static uint32_t ms_now(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static void ensure_init(void) {
    if (s_inited) return;
    s_lock = xSemaphoreCreateMutex();
    push_session_init(&s_sess);
    // gen21：开机恢复未读位（独立 u8 key；读不到按 0）。
    // gen21: restore the unread flag at boot (standalone u8 key; absent means 0).
    {
        nvs_handle_t h;
        if (nvs_open(PUSH_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
            uint8_t u = 0;
            if (nvs_get_u8(h, PUSH_NVS_KEY_UNREAD, &u) == ESP_OK) s_ui.unread = u != 0;
            // gen26：AI 查词开关位（缺省关，首次开过才持久化为 1）。
            // gen26: the AI-lookup enable bit (defaults off; persisted as 1 only
            // after the user turns it on at least once).
            uint8_t ai = 0;
            if (nvs_get_u8(h, PUSH_NVS_KEY_AI, &ai) == ESP_OK) s_ai_enabled = ai != 0;
            // M3：情景开关缺省开——没写过这个 key 就保持初始 true，写过才以 NVS 为准。
            // M3: scene lookup defaults ON - absent key keeps the initial true, and
            // only a written value overrides it.
            uint8_t sc = 0;
            if (nvs_get_u8(h, PUSH_NVS_KEY_SCENE, &sc) == ESP_OK) s_scene_enabled = sc != 0;
            nvs_close(h);
        }
    }
    s_inited = true;
}

static bool dict_available(void) { return s_dict_probe ? s_dict_probe() : true; }

// 电量闸门：充电中永远放行（裁定 d：充电常开绕过低电门槛）。/ Battery gate: charging
// always passes (ruling d: charger-held bypasses the low-battery floor).
static bool battery_ok(void) {
    const pmu_snapshot_t *p = read_pico_pmu_get();
    if (!p || !p->present) return true; // 读不到 PMU 不挡路 / an unreadable PMU does not block
    if (pmu_battery_charging(p)) return true;
    const int percent = pmu_battery_percent(p);
    return percent < 0 || percent >= PUSH_BATT_FLOOR_PERCENT;
}

static bool charging_now(void) {
    const pmu_snapshot_t *p = read_pico_pmu_get();
    return p && p->present && pmu_battery_charging(p);
}

/* ---- NVS 会话 / NVS session ---- */

static void nvs_save_session(void) {
    nvs_handle_t h;
    if (nvs_open(PUSH_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    push_nvs_session_t nv;
    memset(&nv, 0, sizeof(nv));
    nv.magic = PUSH_NVS_MAGIC;
    nv.received = s_sess.received;
    nv.next_seq = s_sess.next_seq;
    nv.begin = s_sess.begin;
    nvs_set_blob(h, PUSH_NVS_KEY_SESS, &nv, sizeof(nv));
    nvs_commit(h);
    nvs_close(h);
}

static bool nvs_load_session(push_nvs_session_t *out) {
    nvs_handle_t h;
    if (nvs_open(PUSH_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    push_nvs_session_t nv;
    size_t len = sizeof(nv);
    const bool ok = nvs_get_blob(h, PUSH_NVS_KEY_SESS, &nv, &len) == ESP_OK &&
                    len == sizeof(nv) && nv.magic == PUSH_NVS_MAGIC;
    nvs_close(h);
    if (ok && out) *out = nv;
    return ok;
}

static void nvs_clear_session(void) {
    nvs_handle_t h;
    if (nvs_open(PUSH_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, PUSH_NVS_KEY_SESS);
    nvs_commit(h);
    nvs_close(h);
}

/* ---- gen21 未读位 / gen21 unread flag ---- */

// 仅写 NVS；s_ui.unread 本体由调用方在自旋锁内改。/ NVS write only; the caller
// flips s_ui.unread itself inside the spinlock.
static void nvs_save_unread(bool v) {
    nvs_handle_t h;
    if (nvs_open(PUSH_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, PUSH_NVS_KEY_UNREAD, v ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

// 置位/清除共用：变了才写 NVS（幂等）。/ Shared set/clear: NVS write only on
// change (idempotent).
static void unread_set(bool v) {
    bool changed = false;
    portENTER_CRITICAL(&s_ui_lock);
    if (s_ui.unread != v) {
        s_ui.unread = v;
        changed = true;
    }
    portEXIT_CRITICAL(&s_ui_lock);
    if (changed) nvs_save_unread(v);
}

bool push_sink_unread(void) {
    ensure_init();
    bool v;
    portENTER_CRITICAL(&s_ui_lock);
    v = s_ui.unread;
    portEXIT_CRITICAL(&s_ui_lock);
    return v;
}

void push_sink_clear_unread(void) {
    ensure_init();
    unread_set(false);
}

/* ---- .part 文件 / The partial file ---- */
// 26.9.30 commit 5：存储层搬到 push_part.c（账本 / 欠账槽 / 四步回退）。
// 搬家的理由只有一个：那两条不变式（账本起点在 ARM 时刻就位、欠账先于新包）
// 必须能被宿主用例咬住，而本文件依赖 BLE/NVS/esp_timer，宿主编不进去。
// 行为一字未改，只是从这里消失。
// Commit 5 moved the storage layer to push_part.c. One reason only: the two
// invariants must be bitable by a host case, and this file needs BLE/NVS/esp_timer
// which the host cannot build. Nothing else changed - the code simply left.

/* ---- STATUS 通知与应答 / STATUS notify and replies ---- */

static void status_remember(const uint8_t *frame, size_t n) {
    if (n > PUSH_STATUS_MAX) n = PUSH_STATUS_MAX;
    memcpy(s_last_status, frame, n);
    s_last_status_len = n;
}

// gen14：重投三件套。/ gen14: the retry trio.
// 锁序注意：cancel 只翻旗（DISCONNECT 持 s_lock 调它，不能在这里 esp_timer_stop——
// timer 回调要拿同一把锁，会死锁）；fire 持锁发完再 unlock 后 delete（oneshot
// 已触发，ESP-IDF 允许回调内删自己）。
// Lock-order note: cancel only flips a flag (DISCONNECT calls it under s_lock and
// must not esp_timer_stop here - the timer callback takes the same lock and would
// deadlock); fire sends under the lock, then deletes after unlocking (the one-shot
// has already fired and ESP-IDF allows deleting it from inside its callback).
static void notify_retry_fire(void *arg);
static bool notify_status(const uint8_t *frame, size_t n);

static void notify_retry_schedule(void) {
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    if (s_notify_retry_left == 0) return;     // 配额用尽：诊断日志已留痕 / quota spent
    if (s_notify_retry_timer != NULL) return; // 已在排队 / already queued
    const esp_timer_create_args_t args = {
        .callback = &notify_retry_fire,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "pp_notify",
    };
    if (esp_timer_create(&args, &s_notify_retry_timer) != ESP_OK) {
        s_notify_retry_timer = NULL;
        return;
    }
    if (esp_timer_start_once(s_notify_retry_timer, 100000) != ESP_OK) { // 100 ms
        esp_timer_delete(s_notify_retry_timer);
        s_notify_retry_timer = NULL;
        return;
    }
    --s_notify_retry_left;
}

static void notify_retry_fire(void *arg) {
    (void)arg;
    lock();
    esp_timer_handle_t t = s_notify_retry_timer;
    s_notify_retry_timer = NULL; // 先摘下：notify_status 失败会重新排 / detach first
    const bool alive = s_conn_handle != BLE_HS_CONN_HANDLE_NONE
                       && s_last_status_len > 0;
    if (alive) notify_status(s_last_status, s_last_status_len); // 失败会再排
    unlock();
    if (t != NULL) esp_timer_delete(t);
}

static void notify_retry_cancel(void) {
    // 只翻旗：timer 到点若发现没连接或没配额就自我了解。/ Flag only: the timer
    // dissolves itself when it finds no connection or no quota left.
    s_notify_retry_left = 0;
}

// 乐观通知：NimBLE 的 notify 不检查 CCCD（ble_gatts_notify_custom 直接读属性发
// 通知），订阅态只用来记日志。没订阅的手机按规范应忽略通知；只读的客户端走
// 读 PUSH_STATUS 的回退通道（on_access 里回 s_last_status）。
// Optimistic notify: NimBLE does not check the CCCD when notifying; the
// subscription flag is for logging only. A phone that never subscribed should
// ignore notifications per spec, and read-only clients use the read fallback on
// PUSH_STATUS, which serves s_last_status.
//
// gen26.3③ 返回值语义：true=帧已交付（或已排入重投的同一帧之外的真正成功），
// false=未交付（无连接/未订阅/mbuf 失败/rc≠0，均已排重投或本就无从投）。
// 调用方据此决定是否消费发送物——搭车队列 dequeue-on-success 靠它。
// gen26.3 item-3 return semantics: true means delivered; false means not
// delivered (no connection / not subscribed / mbuf failure / rc!=0 - each
// already schedules a retry or had nothing to deliver). Callers use it to
// decide whether to consume what they sent - the ride-along queue's
// dequeue-on-success relies on it.
static bool notify_status(const uint8_t *frame, size_t n) {
    status_remember(frame, n);
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return false;
    if (!s_status_subscribed) {
        ESP_LOGD(TAG, "status 0x%02x dropped: peer not subscribed", frame[0]);
        return false;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(frame, (uint16_t)n);
    if (!om) {
        // gen14：mbuf 领不到过去是静默 return——真机就是「处理了但没通知」。
        // gen14: a failed mbuf allocation used to return silently - on hardware
        // that is exactly "processed but never notified".
        ESP_LOGW(TAG, "notify 0x%02x: mbuf alloc failed, retry scheduled", frame[0]);
        notify_retry_schedule();
        return false;
    }
    const int rc = ble_gatts_notify_custom(s_conn_handle, s_status_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "notify 0x%02x rc=%d, retry scheduled", frame[0], rc);
        notify_retry_schedule();
        return false;
    }
    return true;
}

static void reply_ready(void) {
    uint8_t body[4];
    body[0] = (uint8_t)(s_sess.begin.session_id);
    body[1] = (uint8_t)(s_sess.begin.session_id >> 8);
    body[2] = (uint8_t)(s_sess.begin.session_id >> 16);
    body[3] = (uint8_t)(s_sess.begin.session_id >> 24);
    uint8_t frame[PUSH_STATUS_MAX];
    const size_t n = push_status_encode(PUSH_ST_READY, body, sizeof(body), frame, sizeof(frame));
    if (n) notify_status(frame, n);
}

static void reply_resume(void) {
    uint8_t body[6];
    body[0] = (uint8_t)(s_sess.received);
    body[1] = (uint8_t)(s_sess.received >> 8);
    body[2] = (uint8_t)(s_sess.received >> 16);
    body[3] = (uint8_t)(s_sess.received >> 24);
    body[4] = (uint8_t)(s_sess.next_seq);
    body[5] = (uint8_t)(s_sess.next_seq >> 8);
    uint8_t frame[PUSH_STATUS_MAX];
    const size_t n = push_status_encode(PUSH_ST_RESUME, body, sizeof(body), frame, sizeof(frame));
    if (n) notify_status(frame, n);
}

static void reply_done(void) {
    uint8_t body[4];
    body[0] = (uint8_t)(s_sess.begin.total_len);
    body[1] = (uint8_t)(s_sess.begin.total_len >> 8);
    body[2] = (uint8_t)(s_sess.begin.total_len >> 16);
    body[3] = (uint8_t)(s_sess.begin.total_len >> 24);
    uint8_t frame[PUSH_STATUS_MAX];
    const size_t n = push_status_encode(PUSH_ST_DONE, body, sizeof(body), frame, sizeof(frame));
    if (n) notify_status(frame, n);
}

static void reply_err(int code) {
    const uint8_t body[1] = {(uint8_t)code};
    uint8_t frame[PUSH_STATUS_MAX];
    const size_t n = push_status_encode(PUSH_ST_ERR, body, 1, frame, sizeof(frame));
    if (n) notify_status(frame, n);
    // UI 也展示一条：手机不在手上时用户看得见为什么失败。/ The UI shows one too:
    // the user sees why it failed even without the phone in hand.
    portENTER_CRITICAL(&s_ui_lock);
    s_ui.err = code;
    portEXIT_CRITICAL(&s_ui_lock);
}

// 三合取③的入口闸：冻结期间任何会话入口都回 BUSY。返回 true = 已挡下。
// 为什么是 BUSY 而不是 NOT_ARMED：会话可能还在（只是暂时不许进）。BUSY 让手机
// 退避重试；NOT_ARMED 会让它以为用户取消了，直接放弃这次推送。
// The entry gate for conjunct 3. BUSY, not NOT_ARMED: the session may still
// exist, and BUSY makes the phone back off; NOT_ARMED reads as "user cancelled".
static bool entry_frozen_reply(void) {
    portENTER_CRITICAL(&s_ui_lock);
    const bool frozen = (s_freeze_flags & FRZ_FROZEN) != 0;
    portEXIT_CRITICAL(&s_ui_lock);
    if (frozen) reply_err(PUSH_ERR_BUSY);
    return frozen;
}

static void reply_progress(void) {
    // KB 粒度：>64KB 也不溢出，进度条够用。/ KB granularity: no overflow past
    // 64 KB, enough for a progress bar.
    const uint16_t cur = (uint16_t)(s_sess.received >> 10);
    const uint16_t total = (uint16_t)(s_sess.begin.total_len >> 10);
    uint8_t body[4];
    body[0] = (uint8_t)cur;
    body[1] = (uint8_t)(cur >> 8);
    body[2] = (uint8_t)total;
    body[3] = (uint8_t)(total >> 8);
    uint8_t frame[PUSH_STATUS_MAX];
    const size_t n = push_status_encode(PUSH_ST_PROGRESS, body, sizeof(body), frame, sizeof(frame));
    if (n) notify_status(frame, n);
}

/* ---- 连接参数 / Connection parameters ---- */

static void conn_params_apply(uint16_t itvl, uint16_t latency) {
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    // gen30 伴读（R0 摊开项·conn 档位）：伴读活跃期全程 FAIR 15ms，以模式为档位
    // 边界。既有 session 驱动的 DONE / ADV_OFF 回落 SLOW 不得把伴读档位带走；
    // 退出伴读时 push_sink_companion_conn(false) 清标志并显式回 SLOW。
    // gen30 companion: active tier stays FAIR for the whole mode. The legacy
    // session-driven SLOW fallback must not steal it; leaving companion clears
    // the latch and explicitly returns to SLOW.
    if (itvl == PUSH_CONN_ITVL_SLOW && s_companion_conn_fair) return;
    // 外设角色请求参数更新，手机可拒；拒了也不影响传输，只是慢一点。
    // The peripheral requests an update; the phone may refuse. A refusal only
    // costs speed, never correctness.
    const struct ble_gap_upd_params p = {
        .itvl_min = itvl,
        .itvl_max = itvl,
        .latency = latency,
        .supervision_timeout = PUSH_CONN_SUP_TIMEOUT,
    };
    const int rc = ble_gap_update_params(s_conn_handle, &p);
    if (rc != 0) ESP_LOGD(TAG, "conn params rc=%d", rc);
}

/* ---- 广播 / Advertising ---- */

static void adv_stop(void) {
    if (!ble_gap_adv_active()) return;
    (void)ble_gap_adv_stop();
}

// 起广播；成功即补喂 ADV_UP。栈没起来时由调用方走 pending 路径，不会到这里。
// Start advertising; feed ADV_UP on success. With the stack down the caller takes
// the pending path and never reaches here.
//
// gen13 教训（真机回归）：adv data 硬上限 31 字节（BLE_HCI_MAX_ADV_DATA_LEN），
// NimBLE 序列化超出即 EMSGSIZE 拒收。gen12 曾把 128-bit 服务 UUID 塞进 adv
// data：flags(3) + 完整名「PicoPush-XXXX」(15) + UUID(18) = 36B > 31，
// ble_gap_adv_set_fields 失败 → 广播从未起来，而武装已成功、UI 照显倒计时
// （假武装，任何扫描器都搜不到）。修法：adv data 只放 flags + 名字（= gen11
// 实证可用的 18B），服务 UUID 移居 scan response（独立 31B 载荷，主动扫描的
// nRF Connect / 安卓扫描器 / 本项目 APK 都能在扫描记录里看到 e2f5a1c0…）。
// gen13 lesson (real regression): adv data is hard-capped at 31 bytes
// (BLE_HCI_MAX_ADV_DATA_LEN); NimBLE rejects overflow with EMSGSIZE. gen12
// put the 128-bit UUID in adv data (3 + 15 + 18 = 36 > 31), so set-fields
// failed, advertising never started, yet the arm had already succeeded and
// the UI showed a countdown with no radio underneath (a fake arm invisible
// to every scanner). Adv data now carries flags + name only (the proven 18
// bytes of gen11); the UUID rides the scan response (a separate 31-byte
// payload delivered to active scanners).
static void adv_start(void) {
    // gen26.6.3：已连接时不起广播——广播在连上那一刻就停了（ble_gap_adv_active()
    // 为假），而会话状态非 IDLE（CONNECTED/ARMED…），于是 tick/断连/ADV_ON 三条
    // 恢复路径都会在「已连接」时反复 adv_start()，每次被 NimBLE 拒绝（真机日志里
    // `ble_gap_adv_validate rc=6` + `adv start rc=6` 每 2s 一条）。这不影响连接，
    // 但纯属空转噪声；连接在就没必要广播，重连广播由断连事件负责。
    // gen26.6.3: do not advertise while connected - advertising stopped the moment
    // the link came up (ble_gap_adv_active() is false) while the session state is
    // non-IDLE (CONNECTED/ARMED/...), so the tick / disconnect / ADV_ON recovery
    // paths all called adv_start() every ~2s and NimBLE refused each time
    // (`ble_gap_adv_validate rc=6` + `adv start rc=6` in the on-device log). The
    // link is unaffected, but it is pure idle noise; a live link needs no
    // advertising, and the reconnect broadcast is the disconnect event's job.
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) return;
    snprintf(s_adv_name, sizeof(s_adv_name), "%s%04X", PUSH_ADV_NAME_PREFIX, s_sess.token);
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)s_adv_name;
    fields.name_len = (uint8_t)strlen(s_adv_name);
    fields.name_is_complete = 1;
    // adv data 预算（钉死，防再犯）：flags(3) + name ≤ 13+2 = 18B，余量 ≥ 13B。
    // 再想往 adv data 加字段前，先算总长 ≤ 31，否则就是这个坑。
    // Adv-data budget (pinned against recurrence): flags(3) + name(15) = 18B,
    // leaving >= 13B. Size any future adv-data field against the 31B cap.
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        // 失败不留假武装：取消武装并让 UI 收 start_failed（「蓝牙启动失败，
        // 请重试」），而不是一个没有射频支撑的倒计时。
        // No fake arm on failure: cancel and surface start_failed to the UI
        // instead of a countdown with no radio underneath.
        ESP_LOGW(TAG, "adv set fields rc=%d", rc);
        exec_actions(push_session_event(&s_sess, PUSH_EVT_CANCEL, NULL));
        ui_post_start_failed();
        return;
    }
    // 服务 UUID 放 scan response（0x07 Complete List of 128-bit UUIDs，18B）。
    // The service UUID rides the scan response (0x07 complete 128-bit list).
    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.uuids128 = &s_svc_uuid;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv rsp set fields rc=%d", rc);
        exec_actions(push_session_event(&s_sess, PUSH_EVT_CANCEL, NULL));
        ui_post_start_failed();
        return;
    }
    const struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = PUSH_ADV_ITVL,
        .itvl_max = PUSH_ADV_ITVL,
        .channel_map = 7,
        .filter_policy = 0,
        .high_duty_cycle = 0,
    };
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params,
                           push_gap_cb, NULL);
    if (rc != 0) {
        // 广播起不来（控制器被占等）：取消本次武装，UI 收 start_failed。
        // Advertising failed to start (controller busy): cancel the arm and let
        // the UI take start_failed.
        ESP_LOGW(TAG, "adv start rc=%d", rc);
        exec_actions(push_session_event(&s_sess, PUSH_EVT_CANCEL, NULL));
        ui_post_start_failed();
        return;
    }
    ESP_LOGI(TAG, "advertising as %s (svc in scan rsp)", s_adv_name);
    // 26.9.13 P0-d：广播起来这一刻的堆快照。26.9.12 现场最刺眼的一条就是「面板判红
    // 14.0 K，`advertising as` 却照打」——两行之间没有任何堆数字，读完只能猜闸门
    // 是「判过了」还是「没参与」。这一份让两者可分辨：largest ≥ 闸门 = 真判过；
    // largest < 闸门 = 走的不是闸门（见 push-bypass 站点）。
    // 26.9.13 P0-d: heap at the moment advertising comes up. The most jarring pair in
    // the 26.9.12 field log was the panel reading red at 14.0 K while `advertising as`
    // printed anyway, with no heap figures between them - leaving "the gate passed" and
    // "the gate never took part" indistinguishable. This dump separates them: largest at
    // or above the gate means it really judged; below it means some other path armed
    // (see the push-bypass site).
    pico_heap_dump("push-adv", "push_sink:adv_start");
    if (s_sess.state == PUSH_STATE_ARMED) {
        exec_actions(push_session_event(&s_sess, PUSH_EVT_ADV_UP, NULL));
    }
}

/* ---- GATT 服务 / The GATT service ---- */

// 注册时序（gen12 修复，源码实证）：NimBLE 的 ble_hs_start()（host/src/ble_hs.c）
// 在 host 启动路径里**一次性**调用 ble_gatts_start()，然后才 ble_hs_sync() 触发
// sync 回调；ble_gatts_start() 消费 ble_gatts_add_svcs() 排进的定义队列，末尾
// 还把队列整个释放。所以定义必须在 host 任务启动**之前**排队（start_locked 里
// nimble_port_init() 之后、xTaskCreatePinnedToCore 之前，见 push_sink_gatt_register
// 的调用点）；排在 sync 回调里（旧代码的位置）等于排在一场已经散会的队列后面，
// add_svcs 返回 0 也不会有任何属性进表——手机端正是「能连上、Services 页空白」。
// Registration timing (gen12 fix, source-verified): NimBLE's ble_hs_start()
// (host/src/ble_hs.c) calls ble_gatts_start() exactly once on the host start
// path and only then ble_hs_sync() fires the sync callback; ble_gatts_start()
// consumes the queue filled by ble_gatts_add_svcs() and frees it afterwards.
// Definitions must therefore be queued BEFORE the host task starts (in
// start_locked, between nimble_port_init() and xTaskCreatePinnedToCore - see the
// push_sink_gatt_register call site). Queueing inside the sync callback (where
// the old code did it) is queueing after the meeting has adjourned: add_svcs
// returns 0 yet nothing enters the attribute table - exactly the phone-side
// symptom "connects fine, Services page blank".

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &s_data_uuid.u,
                .access_cb = push_access_cb,
                // 特性位是 F_WRITE（WWR，写请求）。实证修正记录（gen12，见
                // docs/M1-audit.md 第二节）：F_WRITE_NO_RSP 从未在本仓库固件源码
                // 出现——所谓「WWR → WNR 漂移」从未发生，位一直是 WWR，ATT 逐包
                // 确认从未退场。NimBLE 的 ATT 服务端对两种写一视同仁（ble_att_svr.c
                // 的 ble_att_svr_rx_write_no_rsp 与 ble_att_svr_rx_write 都汇入
                // write_handle → ha_cb，MYNEWT_VAL_BLE_ATT_SVR_WRITE_NO_RSP=1，
                // 已实证），故手机端 WNR 写与手动 WWR 写都能到达 push_access_cb；
                // 但位上的 WWR 就是 WWR（不是"仅发现时看到的属性"），手机端默认
                // 走 WWR：ATT 逐包回执为第一道防线，RESUME 为第二道。
                // Keep F_WRITE (WWR) here. Verified correction (gen12, see
                // docs/M1-audit.md section 2): F_WRITE_NO_RSP never appears in
                // this repo's firmware source - the claimed "WWR -> WNR drift"
                // never happened, the bit has always been WWR and per-packet
                // ATT confirmation never left. NimBLE's ATT server treats both
                // write ops alike (ble_att_svr_rx_write_no_rsp and rx_write
                // both funnel into write_handle -> ha_cb, BLE_ATT_SVR_WRITE_NO_RSP
                // = 1, source-verified), so both phone-side WNR and manual WWR
                // writes reach push_access_cb; the bit simply is WWR, and the
                // phone defaults to WWR (per-packet ATT acks first line of
                // defense, RESUME second).
                .flags = BLE_GATT_CHR_F_WRITE,
                .val_handle = &s_data_handle,
            },
            {
                .uuid = &s_ctrl_uuid.u,
                .access_cb = push_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE,
                .val_handle = &s_ctrl_handle,
            },
            {
                .uuid = &s_status_uuid.u,
                .access_cb = push_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_status_handle,
            },
            {0},
        },
    },
    {0},
};

// 把服务定义排进注册队列：先 count_cfg（累加上限，供 start 分配属性表池），
// 再 add_svcs（仅排队，真正注册发生在 ble_gatts_start）。两步都查返回码。
// Queue the definitions: count_cfg first (accumulates the limits the start
// uses to size the attribute-table pools), then add_svcs (queueing only;
// registration happens in ble_gatts_start). Both return codes are checked.
static int queue_svcs(void) {
    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "count cfg rc=%d", rc);
        return rc;
    }
    rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "add svcs rc=%d", rc);
        return rc;
    }
    return 0;
}

// 三个句柄是否都已就位（注册成功的实证；0 不是合法 ATT 句柄）。
// Whether all three handles landed (the proof of registration; 0 is not a
// legal ATT handle).
static bool gatt_registered(void) {
    return s_data_handle != 0 && s_ctrl_handle != 0 && s_status_handle != 0;
}

esp_err_t push_sink_gatt_register(void) {
    ensure_init();
    lock();
    // 每个启动周期排一次：start_locked 的失败回退会走 nimble_port_deinit()
    // 把队列清空（ble_gatts_stop -> ble_gatts_free_svc_defs），下一周期重排。
    // Queue once per start cycle: start_locked's rollback runs
    // nimble_port_deinit(), which empties the queue (ble_gatts_stop ->
    // ble_gatts_free_svc_defs), so the next cycle queues afresh.
    if (queue_svcs() != 0) {
        unlock();
        return ESP_FAIL;
    }
    s_gatt_queued = true;
    unlock();
    ESP_LOGI(TAG, "gatt queued (registered when the host starts)");
    return ESP_OK;
}

// sync 回调里的兜底：host 自己的 ble_gatts_start() 跑完之后核对句柄，没注册上
// 就显式补一次 start（源码路径上不该发生——AUTO_START 下 stage2 必定消费队列；
// 真发生了说明排队失败或注册失败，日志会喊）。仅当队列里没有定义时才补排队。
// Fallback in the sync callback: after the host's own ble_gatts_start() has
// run, verify the handles and, if the service never landed, call
// ble_gatts_start() explicitly. On the source-verified path this cannot
// trigger (stage2 always consumes the queue under AUTO_START); if it does,
// queueing or registration failed and the log says so. Re-queue only when the
// definitions are not already queued.
static void gatt_ensure_registered(void) {
    if (gatt_registered()) return;
    if (!s_gatt_queued) {
        if (queue_svcs() == 0) s_gatt_queued = true;
    }
    const int rc = ble_gatts_start();
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts start rc=%d", rc);
        return;
    }
    if (gatt_registered()) {
        ESP_LOGI(TAG, "gatt up (explicit start): data=%u ctrl=%u status=%u",
                 (unsigned)s_data_handle, (unsigned)s_ctrl_handle, (unsigned)s_status_handle);
    } else {
        ESP_LOGE(TAG, "gatt still down after explicit start: data=%u ctrl=%u status=%u",
                 (unsigned)s_data_handle, (unsigned)s_ctrl_handle, (unsigned)s_status_handle);
    }
}

/* ---- 动作执行 / Action execution ---- */

// UI 一次性事件投递。/ Post the one-shot UI events.
static void ui_post_open(const char *title, uint8_t type) {
    portENTER_CRITICAL(&s_ui_lock);
    s_ui.open = true;
    snprintf(s_ui.title, sizeof(s_ui.title), "%s", title ? title : "");
    s_ui.type = type;
    portEXIT_CRITICAL(&s_ui_lock);
}

static void ui_post_word(const char *word) {
    portENTER_CRITICAL(&s_ui_lock);
    s_ui.word_card = true;
    snprintf(s_ui.word, sizeof(s_ui.word), "%s", word ? word : "");
    portEXIT_CRITICAL(&s_ui_lock);
}

// gen18：收摊原因簿记。close_why_set 由各收摊路径调用；ui_post_closed 对没有
// 显设原因的路径补默认值——closed 事件到 UI 永远带得出一条人话。
// gen18: the close-up reason ledger. close_why_set is called by each close-up
// path; ui_post_closed fills a default for paths that set none - the closed
// event always carries a human sentence to the UI.
static void close_why_set(const char *why) {
    snprintf(s_close_why, sizeof(s_close_why), "%s", why ? why : "");
}

static void ui_post_closed(void) {
    if (s_close_why[0] == 0) close_why_set("接收已结束");
    portENTER_CRITICAL(&s_ui_lock);
    s_ui.closed = true;
    portEXIT_CRITICAL(&s_ui_lock);
}

static void ui_post_start_failed(void) {
    portENTER_CRITICAL(&s_ui_lock);
    s_ui.start_failed = true;
    portEXIT_CRITICAL(&s_ui_lock);
}

// gen19（Bug5）：正文落盘完成的一 Shot 投递。单词卡不写 push.txt，不触发——
// 它的“最近”不该被一个孤词覆盖。/ Post the one-shot after a body lands. Word
// cards write no push.txt, so they are skipped - a bare word must not clobber
// the "last push" line.
static void ui_post_committed(const char *title, uint8_t type) {
    portENTER_CRITICAL(&s_ui_lock);
    s_ui.committed = true;
    snprintf(s_ui.committed_title, sizeof(s_ui.committed_title), "%s", title ? title : "");
    s_ui.committed_type = type;
    portEXIT_CRITICAL(&s_ui_lock);
}

// 26.9.30 commit 2：COMMIT 只投递。这个函数可以在 BLE 回调栈上跑——它不碰任何
// 文件、不 reply、不读会话；真正的四拍留给 push_sink_run_commit()。
// COMMIT posts only. Safe on the BLE callback stack: no file I/O, no reply, no
// session read; the four beats wait for push_sink_run_commit().
static void ui_post_commit_request(uint8_t carry) {
    const uint32_t now = ms_now(); // 临界区外取时：ms_now 不该在关中断的窗里跑
    portENTER_CRITICAL(&s_ui_lock);
    s_ui.commit_request = true;
    s_ui.commit_req_ms = now;
    s_ui.commit_carry = carry;
    portEXIT_CRITICAL(&s_ui_lock);
}

// VERIFY：回读 .part 分段算 CRC，与 BEGIN 的 crc32/length 对账后喂 VERIFY 事件。
// VERIFY: read the partial back in chunks, reconcile against the BEGIN crc32 and
// length, then feed the VERIFY event.
static void do_verify(void) {
    // 判据②的测量点：verify 的 FILE* 是局部变量，函数返回即消失，外部无从观测。
    // 进出各加减一次，它才成为「可查询事实」——B51 被降格时给的方向正是这个。
    // **单出口**是这个计数的生命线：do_verify 全函数只有末尾一处返回。哪天有人在
    // 中途加 return，计数就会虚高、quiesce 永远等不到 0，症状又是「push 永久死」。
    // 静态牙 B55 咬这一条（含一枪「中途 return 不减」的变异）。
    // The measurement point for conjunct 2. Single-exit is the lifeline: one
    // mid-function return and the count never returns to zero.
    portENTER_CRITICAL(&s_ui_lock);
    ++s_verify_depth;
    portEXIT_CRITICAL(&s_ui_lock);

    // s_part 为空说明簿记与文件脱节：直接判负（VERIFY 失败回 CONNECTED 保留
    // .part，行为安全）。fflush(NULL) 是 UB，绝不能让指针裸奔。
    // A NULL partial means the bookkeeping lost the file: fail the verdict
    // (which returns to CONNECTED and keeps the partial - safe). fflush(NULL)
    // is UB; never let a bare pointer run.
    // gen15 根因修复：ESP-IDF 的 FATFS VFS **没有注册 .flush_p**（vfs_fat.c 全文零
    // "flush"），fflush() 只把 newlib 缓冲推进 f_write —— 不触发 f_sync；且 sdkconfig
    // 实证 `CONFIG_FATFS_IMMEDIATE_FSYNC is not set`，f_write 后也不补 f_sync。
    // 于是**目录项的 size 字段永不更新**，紧随其后的 stat()（读目录项）与 fopen("rb")
    // 回读拿到的全是过期值：长度对账必然不等、CRC 必然不符 —— 这就是真机 err=4
    // (PUSH_ERR_CHECKSUM) 的来处。宿主 Linux 的 write() 即时更新 inode size，
    // 168 项宿主测试因此永远测不出来（same blind spot as the file layer itself）。
    // 修法：回读前 fclose（f_close 内部走 f_sync，目录项落盘），顺带消掉 FATFS
    // FS_LOCK=0 下「同一文件两次打开」的非法打开（ffconf.h 明文禁止）。
    // gen15 root-cause fix: ESP-IDF's FATFS VFS registers **no .flush_p** (zero
    // "flush" in vfs_fat.c), so fflush() only pushes the newlib buffer into f_write
    // without f_sync, and `CONFIG_FATFS_IMMEDIATE_FSYNC` is off (sdkconfig) so
    // f_write does not sync either. The **directory entry never learns the new
    // size**, so the stat() and fopen("rb") readback right after it read stale
    // values: the length reconciliation can never match and the CRC can never
    // agree - the source of the on-hardware err=4 (PUSH_ERR_CHECKSUM). Linux
    // updates the inode size on write(), which is why 168 host checks missed it.
    // Fix: fclose before the readback (f_close runs f_sync internally), which
    // also removes the duplicate open that ffconf.h forbids under FS_LOCK=0.
    bool ok = push_part_is_open();
    if (ok) {
        push_part_close(); // f_close -> f_sync：目录项落盘 / commits the directory entry
    }
    const long sz = push_part_size();
    uint32_t crc_file = 0;
    if (ok && sz != (long)s_sess.received) ok = false; // 文件与记账不符 / file disagrees with bookkeeping
    if (ok && sz == (long)s_sess.begin.total_len) {
        FILE *r = fopen(PUSH_PART_PATH, "rb");
        if (!r) {
            ok = false;
        } else {
            uint8_t buf[PUSH_CRC_CHUNK];
            uint32_t crc = 0;
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), r)) > 0) crc = push_crc32_continue(crc, buf, n);
            const bool read_ok = ferror(r) == 0;
            (void)fclose(r);
            crc_file = crc;
            ok = read_ok && crc == s_sess.begin.crc32;
        }
    } else if (ok) {
        ok = false; // 长度不等：保留 .part 等续传 / length mismatch: keep the partial for resume
    }
    // gen15 诊断：VERIFY 五项判据全量留痕，一次真机即可把败因归位到具体口径。
    // gen15 diagnostics: log every VERIFY criterion so one hardware run pins the
    // failure to a specific quantity.
    if (!ok) {
        ESP_LOGW(TAG, "verify FAIL: sz=%ld received=%u total=%u crc_file=0x%08x crc_want=0x%08x",
                 sz, (unsigned)s_sess.received, (unsigned)s_sess.begin.total_len,
                 (unsigned)crc_file, (unsigned)s_sess.begin.crc32);
    } else {
        ESP_LOGI(TAG, "verify ok: sz=%ld crc=0x%08x", sz, (unsigned)crc_file);
    }
    // 判定前落一盘 NVS：此刻崩溃，重连后 RESUME_也能续上。/ Checkpoint before the
    // verdict: a crash here still resumes after reconnect.
    nvs_save_session();
    push_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.verify_ok = ok;
    exec_actions(push_session_event(&s_sess, PUSH_EVT_VERIFY, &evt));

    // 与开头的 ++ 成对。放在 exec_actions 之后：整段 verify（含判定后的状态机
    // 迁移）都算「verify 在跑」，比「FILE* 活着」更宽——宽只会让 quiesce 多等
    // 几毫秒，窄才会放过一个正在读卡的句柄。
    // Paired with the ++ above. Deliberately after exec_actions: the whole verify
    // counts, which is wider than "the FILE* is alive" - wider only costs a few
    // ms of waiting, narrower would let a live reader through.
    portENTER_CRITICAL(&s_ui_lock);
    --s_verify_depth;
    portEXIT_CRITICAL(&s_ui_lock);
}

// COMMIT：正文原子改名成 push.txt；单词卡不落盘（title 即单词，UI 直接查词典）。
// 返回 false = 提交失败（错误已回手机），调用方必须拦住开书动作。
// ---- 提交舞蹈的前置：把 .bak 收敛掉 ----------------------------------------
// .bak 只有一个合法存在理由：上一次提交在"旧书已挪开、新书没就位"之间被打断。
// 两种残留要分开处置，**判据是成品在不在**，不是"哪一步失败"：
//   text 在  -> bak 是更旧的一份，删掉（旧书正在位，bak 只是垃圾）；
//   text 不在 -> bak 就是用户唯一的书，rename 回来（这就是"先捡后清"）。
// 裸的 remove(BAK) 会在第二种情形删掉唯一幸存的书 —— 26.9.30 审计补丁一的原话
// 按字面执行就是这个结果，所以这里必须按"成品尚在"分支。
//
// The only legitimate reason for .bak to exist is an interrupted commit. The
// decision key is **whether the product exists**, not which step failed:
// text present -> .bak is the older copy, delete it; text absent -> .bak is the
// user's only book, rename it back.
static bool recover_bak_if_needed(void) {
    struct stat st;
    if (stat(PUSH_TEXT_PATH, &st) == 0) return true;   // 成品在：bak 是垃圾，交给调用方清
    if (errno != ENOENT) {
        // 卷掉了 / IO 错：不猜，原样退出。这一支单独写，是为了不让 spurious
        // stat 失败被记成"捡回失败"，那会把排错引到错的文件上。
        ESP_LOGE(TAG, "recover: stat %s errno=%d", PUSH_TEXT_PATH, errno);
        return false;
    }
    if (stat(PUSH_BAK_PATH, &st) != 0) {
        if (errno == ENOENT) return true;              // 两者都不在：首推，正常
        ESP_LOGE(TAG, "recover: stat %s errno=%d", PUSH_BAK_PATH, errno);
        return false;
    }
    if (rename(PUSH_BAK_PATH, PUSH_TEXT_PATH) != 0) {
        ESP_LOGE(TAG, "recover: rename bak->text errno=%d", errno);
        return false;
    }
    ESP_LOGW(TAG, "recover: orphan %s restored to %s", PUSH_BAK_PATH, PUSH_TEXT_PATH);
    return true;
}

// 四拍：把 .part 变成 push.txt，旧书全程不删（挪到 .bak，最后才清）。
// 返回 false = 失败；调用方负责 reply_err，且此时旧书**一定还在**（原位或在 .bak）。
// Four beats: .part becomes push.txt and the old book is never removed while the
// new one is not in place. Returns false = failed; the caller replies the error
// and the old book is guaranteed to still exist (in place, or in .bak).
static bool commit_dance(void) {
    // 拍 0：先捡后清，把状态收敛到"text 在 / bak 无"这一个干净前置态。
    // 所有分支都从这里出发，所以四拍的前置条件是常量，不是调用者保证的。
    if (!recover_bak_if_needed()) return false;
    if (remove(PUSH_BAK_PATH) != 0 && errno != ENOENT) {
        ESP_LOGE(TAG, "commit: clear stale bak errno=%d", errno);
        return false;
    }
    // 拍 1：旧书挪开（不删）。首推时 text 不存在，ENOENT 是正常路径。
    if (rename(PUSH_TEXT_PATH, PUSH_BAK_PATH) != 0 && errno != ENOENT) {
        ESP_LOGE(TAG, "commit: park text errno=%d", errno);
        return false;
    }
    // 拍 2：新书就位。这一拍失败是唯一会动到"用户唯一那份书"的分支，
    // 所以回滚必须判返回值、且日志必须指名 .bak —— 现场只有"推送失败"一句时，
    // 没人知道书还在。
    if (rename(PUSH_PART_PATH, PUSH_TEXT_PATH) != 0) {
        const int e = errno;                           // 回滚会覆盖 errno，先存
        ESP_LOGE(TAG, "commit: install part errno=%d, rolling back", e);
        if (rename(PUSH_BAK_PATH, PUSH_TEXT_PATH) != 0) {
            ESP_LOGE(TAG, "commit: ROLLBACK FAILED errno=%d - old book is at %s, "
                     "next commit or boot recovers it", errno, PUSH_BAK_PATH);
        } else {
            ESP_LOGW(TAG, "commit: rolled back, old book restored");
        }
        errno = e;
        return false;
    }
    // 拍 3：清 BAK。失败不致命——下一次提交的拍 0 会再清一次。
    if (remove(PUSH_BAK_PATH) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "commit: clear bak after install errno=%d (self-heals next commit)", errno);
    }
    return true;
}

// COMMIT: atomically rename the body to push.txt; word cards never land on disk
// (the title is the word and the UI looks it up directly). Returns false = the
// commit failed (the error is already replied); the caller must then suppress
// the book-open action.
static bool do_commit(void) {
    push_part_close();
    bool ok = true;
    if (s_sess.begin.type == PUSH_TYPE_WORD) {
        (void)remove(PUSH_PART_PATH);
        // 词典中途消失：不出空卡，回 ERR(9)（裁定 b：不做降级渲染）。
        // The dictionary vanished mid-push: no empty card, ERR(9) instead
        // (ruling b: no degraded rendering).
        if (!dict_available()) {
            nvs_clear_session();
            reply_err(PUSH_ERR_NO_DICT);
            return false;
        }
    } else {
        // 26.9.30 起这里不再 remove 成品：旧书挪到 .bak、新书就位后才清 .bak。
        // 原写法（gen16 修 EEXIST 时引入）是 `remove(push.txt)` 再 rename，
        // 改名失败就把旧书弄丢了 —— 26.9.30 调查确认那是"文件消失"的唯一机制。
        // 历史注释见 git：`commit rename: File exists` 是 gen15 的真机铁证。
        // Since 26.9.30 the product is never removed here: the old book parks in
        // .bak and is deleted only after the new one lands. The gen16 fix for
        // EEXIST (remove-then-rename) is exactly what lost the old book.
        ok = commit_dance();
    }
    nvs_clear_session();
    if (!ok) {
        // 改名失败（掉卡）：.part 留着可人工恢复，回 ERR(5)。
        // Rename failed (card yanked): keep the partial for manual recovery and
        // reply ERR(5).
        reply_err(PUSH_ERR_NO_CARD);
        return false;
    }
    conn_params_apply(PUSH_CONN_ITVL_SLOW, PUSH_CONN_LATENCY_SLOW);
    reply_done();
    return true;
}

static void exec_actions_body(uint32_t act) {
    if (act & PUSH_ACT_DROP) {
        push_part_discard();
        nvs_clear_session();
        s_pkts_since_ckpt = 0;
    }
    if (act & PUSH_ACT_READY) {
        if (!push_part_open_fresh()) {
            // .part 建不起来：取消会话回 ERR(5)，状态机不留半吊子簿记。
            // P0-2a ④：这里过去零日志，卡坏时现场只能看到"推送失败"。
            // The partial cannot be created: cancel with ERR(5) so the machine
            // keeps no half-born bookkeeping. P0-2a ④: this used to log nothing,
            // leaving "push failed" as the only field evidence.
            ESP_LOGE(TAG, "part open(%s) errno=%d", PUSH_PART_PATH, errno);
            exec_actions(push_session_event(&s_sess, PUSH_EVT_CANCEL, NULL));
            reply_err(PUSH_ERR_NO_CARD);
            return;
        }
        // 全新会话的账本归零在 push_part_open_fresh() 内部（M121c 的靶）；
        // 这里只剩检查点计数。
        // The fresh-session ledger reset lives inside push_part_open_fresh() (that
        // is M121c's target); only the checkpoint counter is left here.
        s_pkts_since_ckpt = 0;
        nvs_save_session();
        conn_params_apply(PUSH_CONN_ITVL_FAIR, PUSH_CONN_LATENCY_FAST);
        reply_ready();
    }
    if (act & PUSH_ACT_RESUME) {
        // gen16（审计裁定落地）：gen15 起手机 sid 逐次唯一，正常流程**不该**走到
        // 自动 RESUME——走到即意味着 sid 被复活/复用（或崩溃恢复的合法续传）。
        // 该分支保留为诊断通道：留全字段警告供追查，绝不静默。
        // gen16 (audit ruling landed): sids are unique per push since gen15, so
        // the automatic RESUME path should never trigger in normal flow - if it
        // does, a sid was revived/reused (or this is a legit post-crash resume).
        // The branch stays as a diagnostic channel: log everything, never silently.
        ESP_LOGW(TAG, "resume path taken: sid=%08lx offset=%u next_seq=%u",
                 (unsigned long)s_sess.begin.session_id,
                 (unsigned)s_sess.received, (unsigned)s_sess.next_seq);
        reply_resume();
    }
    if (act & PUSH_ACT_APPEND) {
        // 欠账先补：顺序追加一旦乱序就是永久损坏。补不上就终止会话，绝不
        // 「先把新包写了再说」。
        // Drain first: append-only storage is permanently corrupt once out of
        // order, so a stuck owed packet ends the session - never "write the new
        // one first and sort it out later".
        if (!push_part_drain()) {
            exec_actions(push_session_event(&s_sess, PUSH_EVT_CANCEL, NULL));
            reply_err(PUSH_ERR_NO_CARD);
            return;
        }
        if (!push_part_append(s_rx_payload, s_rx_len)) {
            // 写盘失败：簿记已经前进，唯独取消能让 offset 重新诚实。
            // P0-2a ②：设备端必须留痕——"失败而不留痕"本身就是缺陷。
            // The write failed: the counters already moved, and only a cancel
            // makes the offset honest again. P0-2a ②: the device must leave a
            // trace - a silent failure is a defect of its own.
            ESP_LOGE(TAG, "append seq=%u errno=%d, %u B lost at offset %u",
                     (unsigned)s_sess.next_seq, errno, (unsigned)s_rx_len,
                     (unsigned)push_part_len());
            exec_actions(push_session_event(&s_sess, PUSH_EVT_CANCEL, NULL));
            reply_err(PUSH_ERR_NO_CARD);
            return;
        }
        if (++s_pkts_since_ckpt >= PUSH_NVS_CHECKPOINT_EVERY) {
            s_pkts_since_ckpt = 0;
            // gen15：NVS 与 .part 的**可见长度**必须同步推进。FATFS 下可见长度只有
            // f_sync 才会回写目录项，所以这里补一次 push_part_sync()。顺序固定为「先 NVS
            // 后 sync」：崩溃后 .part 的可见长度只会落后于记账、绝不会超前，续传安全。
            // gen15: NVS and the **visible length** of the partial must advance
            // together. On FATFS only f_sync rewrites the directory entry, hence
            // push_part_sync(). Order is fixed - NVS first, then sync - so after a crash
            // the visible length can only lag behind the counters, never run ahead.
            nvs_save_session();
            if (!push_part_sync()) {
                // P0-2a ③：检查点的 fflush+fsync 失败过去被吞。这里只警告不终止：
                // 文件本身还在，续传锚是 NVS 记账，最坏是退回上一个检查点重收。
                // P0-2a ③: a failed checkpoint sync used to vanish. Warn, do not
                // abort: the file is still there and the resume anchor is the NVS
                // bookkeeping, so the worst case is re-receiving from the previous
                // checkpoint.
                ESP_LOGW(TAG, "checkpoint sync failed, errno=%d", errno);
            }
        }
    }
    if (act & PUSH_ACT_PROGRESS) {
        const uint32_t now = ms_now();
        if (now - s_last_prog_ms >= PUSH_PROGRESS_MIN_GAP_MS) {
            s_last_prog_ms = now;
            reply_progress();
        }
    }
    if (act & PUSH_ACT_VERIFY) do_verify();
    if (act & PUSH_ACT_COMMIT) {
        // 26.9.30 commit 2（封送）：提交段从这里搬走了。BLE 回调只投递一个请求，
        // 主循环（app_loop → transfer_pump_commit → push_sink_run_commit）在帧间
        // 安全点执行四拍。理由不是洁癖：rename/remove 跑在 BLE 回调栈上，和读者
        // 的 fread 抢同一个卷，而回调栈上根本没有「读者已释放」这个前提。
        // gen16 的约束照旧：提交没成就没有书可开——所以同拍携带的 OPEN_BOOK /
        // WORD_CARD 必须一起扣下，由执行体在成功之后补发。此前 rename 失败时
        // OPEN_BOOK 照样把**旧的** push.txt 弹出来（gen15 真机现象：「弹出的永远
        // 是首次写入的内容」）。扣下而不是丢弃：成功路径仍然要开书。
        // Commit moved out (hand-off): the BLE callback only posts; the main loop
        // runs the four beats between frames. Not fastidiousness - rename/remove on
        // the BLE callback stack contends with the reader's fread for the same
        // volume, and that stack carries no "reader released" precondition.
        // gen16 still holds: no commit, no book to open - so the carried
        // OPEN_BOOK / WORD_CARD bits are withheld here and re-posted by the executor
        // on success. Withheld, not dropped: the success path still opens the book.
        const uint8_t carry =
            (uint8_t)(((act & PUSH_ACT_OPEN_BOOK) ? PUSH_COMMIT_CARRY_OPEN : 0u) |
                      ((act & PUSH_ACT_WORD_CARD) ? PUSH_COMMIT_CARRY_WORD : 0u));
        ui_post_commit_request(carry);
        act &= ~(uint32_t)(PUSH_ACT_OPEN_BOOK | PUSH_ACT_WORD_CARD);
    }
    if (act & PUSH_ACT_ERR) {
        // gen14 诊断：session 层拒绝（token/type/total_len/跳号/校验）留痕。
        // gen14 diagnostics: session-level refusals (token/type/total_len/seq
        // gap/checksum) leave a trace too.
        ESP_LOGW(TAG, "session refused err=%d", s_sess.last_err);
        reply_err(s_sess.last_err);
    }
    if (act & PUSH_ACT_ADV_ON) adv_start();
    if (act & PUSH_ACT_ADV_OFF) {
        adv_stop();
        conn_params_apply(PUSH_CONN_ITVL_SLOW, PUSH_CONN_LATENCY_SLOW);
        s_adv_name[0] = 0;
        ui_post_closed();
    }
    if (act & PUSH_ACT_OPEN_BOOK) ui_post_open(s_sess.begin.title, s_sess.begin.type);
    if (act & PUSH_ACT_WORD_CARD) ui_post_word(s_sess.begin.title);
}

// 所有 BLE 回调与主循环都经由这里进状态机。收口放在 wrapper 而不是 body 的
// 每个 return 前：事件出口有 8 个、还会增加，而"之后还会不会写 .part"是状态的
// 函数——判状态比判事件稳（同族：摘抄四分法判"卡上可见内容"而不是"哪个调用失败"）。
// All callers enter the state machine through here. The close is in the wrapper
// rather than before every return in the body: there are eight event exits and
// counting, while "will .part be written again" is a function of the state.
static void exec_actions(uint32_t act) {
    exec_actions_body(act);
    // 只关不删：.part 与 NVS 簿记都要留给续传（关闭时 f_close 会 f_sync，
    // 把可见长度推进到已写字节，对续传只有好处）。
    // Close without removing: both .part and the NVS bookkeeping are what resume
    // depends on. f_close runs f_sync, which brings the visible length up to the
    // bytes actually written - strictly helpful for a resume.
    if (s_sess.state != PUSH_STATE_RECEIVING && s_sess.state != PUSH_STATE_PROCESSING) {
        push_part_close();
    }
}

/* ---- 26.9.30 commit 2：提交段的封送出口 / the commit hand-off ---- */

bool push_sink_take_commit_request(void) {
    if (s_lock == NULL) return false; // 未 init：没有人投递过请求
    bool pending = false;
    bool expired = false;
    lock();
    if (s_ui.commit_request) {
        // 读即清：失败不回写。app 层若此刻不能提交（commit 3 的读者释放），重试
        // 的所有权在它手里；把请求塞回这个槽会把「一次投递」变成「无限重投」，
        // 而 2 s 上界正是用来禁止无界重投的。
        // Read-and-clear: a failure is never written back. If the app layer cannot
        // commit right now (commit 3's reader release) it owns the retry - putting
        // the request back would turn one hand-off into an unbounded re-post, which
        // is exactly what the 2 s cap forbids.
        s_ui.commit_request = false;
        expired = (uint32_t)(ms_now() - s_ui.commit_req_ms) >= PUSH_COMMIT_WAIT_MS;
        pending = !expired;
        // 只有真正交到 app 层手里的这一次才算「活的提交」；过期作废的不置位。
        // Only a hand-off that really reaches the app layer counts as live; an
        // expired post is void and must not open the gate.
        if (pending) s_ui.commit_live = true;
    }
    unlock();
    if (expired) {
        // 超时 = 主循环 2 s 没取走（泵没跑，或被长任务堵住）。回 ERR(7) 而不是
        // ERR(5)：手机端必须能分开「忙」与「无卡」，否则现场只能靠猜。
        // Expiry means the main loop did not pick it up within 2 s (pump stalled or
        // blocked). Reply ERR(7), not ERR(5): the phone must be able to tell "busy"
        // from "no card", or the field has nothing but guesses.
        ESP_LOGW(TAG, "commit request expired after %u ms -> ERR(BUSY)",
                 (unsigned)PUSH_COMMIT_WAIT_MS);
        reply_err(PUSH_ERR_BUSY);
    }
    return pending;
}

bool push_sink_run_commit(void) {
    if (s_lock == NULL) return false;
    char title[PUSH_TITLE_MAX];
    uint8_t type = 0;
    lock();
    // 26.9.30 commit 4（审计附注 1 第 2 条）：首行自证凭据。
    // 四拍只许在「这一次 take 仍欠着终态」的会话里跑；没有凭据就 no-op——否则
    // 任何一条误调（或 abort 之后的补跑）都会在没有任何推送的情况下 rename /
    // remove，那正是「幽灵提交」的形态。凭据在这里消费（不是留到末尾），
    // 保证同一次 take 只可能有一次 run_commit。
    // First line: prove the session is actually in the commit phase. The four
    // beats may only run while this take still owes a terminal exit; without the
    // proof it is a no-op - otherwise any stray call (or a re-run after abort)
    // would rename/remove with no push in flight, which is exactly a ghost
    // commit. The ticket is consumed here, not at the end, so one take can never
    // produce two runs.
    if (!s_ui.commit_live) {
        unlock();
        ESP_LOGW(TAG, "run_commit without a live request -> no-op");
        return false;
    }
    s_ui.commit_live = false;
    // 先拷出来：do_commit() 会清会话簿记，标题与类型不该在它之后再从 s_sess 读。
    // Copy first: do_commit() clears the session bookkeeping, so the title and type
    // must not be read from s_sess afterwards.
    snprintf(title, sizeof(title), "%s", s_sess.begin.title);
    type = s_sess.begin.type;
    const uint8_t carry = s_ui.commit_carry;
    s_ui.commit_carry = 0;
    const bool ok = do_commit();
    if (ok) {
        if (type != PUSH_TYPE_WORD) {
            ui_post_committed(title, type); // gen19：首页「最近推送」同步
            unread_set(true);               // gen21：落盘即置未读（单词卡不置位）
        }
        // 同拍扣下的两个 UI 位在这里补发——成功才发，失败一个字都不发。
        // The two withheld UI bits are re-posted here, and only on success.
        if (carry & PUSH_COMMIT_CARRY_OPEN) ui_post_open(title, type);
        if (carry & PUSH_COMMIT_CARRY_WORD) ui_post_word(title);
    }
    unlock();
    return ok;
}

// 26.9.30 commit 3：请求已被 take 取走、但 app 层判断这一拍不能提交（读者的书
// 关不干净）。**不回写请求**——回写会把一次投递变回无限重投，2 s 上界正是用来
// 禁止它的；app 层持有自己的有界重试，放大到死线外就调到这里，明确回 ERR(7)。
// 扣下的两个 UI 位随之作废：失败一个字都不发（与 run_commit 的失败分支同一条）。
// The request was already taken, but the app layer says this frame cannot commit
// (the reader's book will not close cleanly). The request is **never** written
// back - that would turn one post into unbounded re-posting, which the 2 s cap
// exists to forbid; the app layer owns its bounded retry and lands here once past
// its deadline, replying ERR(7) explicitly. The two withheld UI bits die with it:
// a failure sends nothing, same as run_commit's failure branch.
void push_sink_commit_abort(int err) {
    if (s_lock == NULL) return;
    lock();
    // 26.9.30 commit 4（审计附注 1 第 3 条）：幂等。
    // 没有活着的请求 = 这次提交已经有终态了（run_commit 或上一次 abort），
    // 再回一个 ERR 就是第二次破坏会话：手机侧会把一个已完成/已失败的提交
    // 再记一笔失败，现场也无法分辨「真忙」与「重复 abort」。
    // Idempotent: no live request means this commit already reached a terminal
    // exit (run_commit, or an earlier abort). A second ERR would wreck the
    // session a second time - the phone would log another failure for a commit
    // that already ended, and the field could not tell "really busy" from a
    // duplicate abort.
    if (!s_ui.commit_live) {
        unlock();
        ESP_LOGW(TAG, "commit abort ignored: no live request (err=%d)", err);
        return;
    }
    s_ui.commit_live = false;
    s_ui.commit_carry = 0;
    unlock();
    ESP_LOGW(TAG, "commit aborted by the app layer: err=%d", err);
    reply_err(err);
}

/* ---- CTRL / DATA 写入处理 / CTRL and DATA write handling ---- */

// BEGIN 的胶水前置闸门：词典（单词卡）、电量、.part 可建。都在喂状态机之前，
// 拒绝时状态机原地不动。/ Glue-side pre-checks for BEGIN: dictionary (word cards),
// battery and a creatable partial - all before the machine sees the frame, so a
// refusal leaves the machine untouched.
static int begin_precheck(const push_begin_t *b) {
    if (b->type == PUSH_TYPE_WORD && !dict_available()) return PUSH_ERR_NO_DICT;
    if (!battery_ok()) return PUSH_ERR_LOW_BATTERY;
    if (!push_part_probe()) return PUSH_ERR_NO_CARD;
    return PUSH_ERR_NONE;
}

/* ---- gen26 AI 查词：代理层 + 搭车协议 ---- */
/* gen26 AI lookup: the proxy layer plus the ride-along protocol. */

// 安全复制：最多 cap-1 字节 + 末尾补 0；绕开 -Werror=stringop-truncation。
// Bounded copy: at most cap-1 bytes plus a trailing NUL; sidesteps the
// stringop-truncation -Werror (strncpy trips it on GCC 15.2).
static void ai_str_put(char *dst, size_t cap, const char *src) {
    size_t n = strnlen(src, cap - 1);
    memcpy(dst, src, n);
    dst[n] = 0;
}

// 26.9.9 诊断：把 UTF-8 文本截到至多 max_bytes 字节并退到字符边界，末尾补 NUL。
// 只给 0x07 取证日志的 def_head 用，不参与任何产品路径：多字节字符被切一半时日志里
// 会出现半个汉字，审计拿到的就是乱码而不是证据。
// 26.9.9 diagnostics: truncate UTF-8 text to at most max_bytes, backing off to a
// character boundary, then NUL-terminate. Used only by the 0x07 evidence log's
// def_head and by nothing on the product path: a multi-byte rune cut in half turns
// the auditor's evidence into mojibake.
static void ai_log_utf8_head(char *dst, size_t cap, const char *src, size_t max_bytes) {
    if (dst == NULL || cap == 0) return;
    if (src == NULL) { dst[0] = 0; return; }
    size_t n = strnlen(src, cap - 1);
    if (n > max_bytes) n = max_bytes;
    // src[n] 落在续字节上 = 切进了字符内部，回退到该字符首字节之后。
    // A continuation byte at src[n] means we cut inside a rune: back off to just
    // past that rune's lead byte.
    while (n > 0 && ((unsigned char)src[n] & 0xC0u) == 0x80u) --n;
    memcpy(dst, src, n);
    dst[n] = 0;
}

// 进预取缓存（块 3）：环形覆盖，最近回流的释义优先命中。
// Into the prefetch cache (block 3): ring overwrite, newest definitions win.
static void ai_cache_put(const char *word, const char *def) {
    ai_cache_t *e = &s_ai_cache[s_ai_cache_next];
    ai_str_put(e->word, sizeof(e->word), word);
    ai_str_put(e->def, sizeof(e->def), def);
    e->used = true;
    s_ai_cache_next = (uint8_t)((s_ai_cache_next + 1u) % AI_CACHE_N);
}

static const char *ai_cache_find(const char *word) {
    for (uint8_t i = 0; i < AI_CACHE_N; ++i) {
        if (s_ai_cache[i].used && strcmp(s_ai_cache[i].word, word) == 0) return s_ai_cache[i].def;
    }
    return NULL;
}

// 把一条释义交给 UI 槽（块 4 框架入口）。在 s_ui_lock 下写，UI 轮询取走。
// Hand a definition to the UI slot (block 4 framework entry); the UI polls it.
static void ai_surface(const char *word, const char *def) {
    portENTER_CRITICAL(&s_ui_lock);
    s_ui.ai_def = true;
    ai_str_put(s_ui.ai_word, sizeof(s_ui.ai_word), word);
    ai_str_put(s_ui_ai_def_text, sizeof(s_ui_ai_def_text), def);
    portEXIT_CRITICAL(&s_ui_lock);
}

// 连接后上报能力位图（STATUS 0x18）；APK 据此显隐查词入口。
// Report the capability bitmap (STATUS 0x18) after connect; the APK gates the
// entry on it.
static void ai_send_caps(void) {
    push_caps_t caps = {0};
    if (s_ai_enabled) caps.bits |= PUSH_CAP_AI_QUERY;
    // gen26.5：上下文能力只在确有上下文源时报（不虚报）；MULTI 等 Step7 重组器
    // 落地后再报——报了就得收 0x08，收不了就是骗对端。
    // gen26.5: advertise CONTEXT only with a real context source; MULTI waits for
    // the Step7 reassembler - advertising it means we must accept 0x08.
    if (s_ai_ctx_ready) caps.bits |= PUSH_CAP_AI_CONTEXT;
    // MULTI：0x08 重组器已落地，收得住长释义，故如实上报（报了就得收）。
    // MULTI: the 0x08 reassembler exists, so long definitions are accepted.
    if (s_ai_enabled) caps.bits |= PUSH_CAP_AI_MULTI;
    // M3：情景位只在「AI 开 + 情景开」时报——设备确实能发 0x1A 才敢报，报了就得能发。
    // M3: advertise SCENE only with both switches on - claim it only when the
    // device can actually emit 0x1A.
    if (s_ai_enabled && s_scene_enabled) {
        caps.bits |= PUSH_CAP_AI_SCENE;
        // M3-B：设备自己能渲染情景视图，如实上报（APK 据此决定要不要显示入口）。
        // M3-B: the device can render the scene view; advertise it so the APK can
        // decide whether to offer the entry.
        caps.bits |= PUSH_CAP_AI_SCENE_VIEW;
    }
    uint8_t frame[5];
    const size_t n = push_caps_encode(&caps, frame, sizeof(frame));
    if (n) notify_status(frame, n);
}

// 把离线队列里的待查词逐个回流（STATUS 0x17），出队保持紧凑。
// Flush the offline queue word by word (STATUS 0x17), staying compact.
//
// **gen26.6 起恒为空转**：入队路径已随裁定移除（push_sink_ai_tap_word_ctx），
// 队列永无生产者，本函数实际不再回流任何词。保留是为 0x06 冻结协议位与
// ai_on_subscribed 的调用形状不动；dequeue-on-success 语义随之失去意义。
// **A permanent no-op since gen26.6**: the enqueue path was removed by ruling
// (push_sink_ai_tap_word_ctx), so the queue never gets a producer and this
// flushes nothing. Kept for the frozen 0x06 opcode and ai_on_subscribed's call
// shape; dequeue-on-success is moot along with it.
//
// gen26.3③ dequeue-on-success（从 M2 提前）：notify 确认成功才出队；失败
// （未订阅/mbuf 不足/rc≠0）时队首词留队，帧本身已由 notify_retry_schedule
// 排入 100ms 重投，下次订阅事件或重连后继续 flush——杜绝 gen26.2 注释挂账的
// 「帧丢词被吃、离线队列被静默清空」。
// gen26.3 item-3 dequeue-on-success (pulled forward from M2): dequeue only
// after notify confirms success; on failure (unsubscribed / mbuf shortage /
// rc!=0) the head word stays queued, its frame is already scheduled for a
// 100 ms retry by notify_retry_schedule, and the next subscribe event or
// reconnect resumes the flush - ending the gen26.2-noted "dropped frame,
// eaten word, offline queue silently emptied".
// APK 已声明支持上下文 **且** 设备确有上下文源可填，才走 74B 上下文档。
// Both sides must agree: the APK declared CONTEXT and we can actually supply one.
static bool ai_ctx_enabled(void) {
    return s_ai_ctx_ready && (s_apk_caps & PUSH_CAP_AI_CONTEXT) != 0u;
}

static void ai_flush_queue(void) {
    if (!s_ai_ready) return;
    char w[PUSH_AI_WORD_MAX + 1];
    // gen26.5 自擒：原实现把 sizeof(w)=65 传给 push_st_ai_query_encode（它要 66）
    // → 恒返回 0 → 离线搭车队列**一个词都回不了流**（该路径无宿主断言覆盖，真机
    // 才可见）。帧槽按最大帧 74B 开，legacy 66B 装得下，两种格式共用一槽。
    // gen26.5 self-catch: the old call passed sizeof(w)=65 to
    // push_st_ai_query_encode (needs 66) -> always 0 -> the offline ride-along
    // queue never flushed a single word (no host assertion covered this path).
    // The slot is sized for the largest frame (74B); legacy 66B fits too.
    uint8_t frame[PUSH_ST_AI_CTX_QUERY_LEN];
    for (uint16_t guard = 0; ai_raloc_count(s_ai_buf) > 0 && guard <= s_ai_cap; ++guard) {
        if (!ai_raloc_peek(s_ai_buf, w, sizeof(w))) break;
        // 离线队列只存词（无书上下文）→ book_id=0 → 恒走 legacy，不编造上下文。
        // The queue holds words only (no book context) -> book_id=0 -> always legacy.
        const size_t n = push_st_ai_query_emit(w, 0u, 0u, ai_ctx_enabled(), frame, sizeof(frame));
        if (!n) break;
        if (!notify_status(frame, n)) break; // 未交付：词留队 / not delivered: word stays
        if (!ai_raloc_dequeue(s_ai_buf, w, sizeof(w))) break;
    }
}

// 订阅建立：上报能力 + 回流离线队列（gen26.6 起回流恒空）。
// On subscription: report capability and flush the offline queue (the flush is
// a permanent no-op since gen26.6).
//
// gen26.2 根因修复：CAPS **不能**在 BLE CONNECT 事件里做。手机的 CCCD 订阅比
// 连接建立晚 ~470ms（日志两会话实证），而 notify_status() 对未订阅的 peer 直接
// 丢帧（LOGD，release 不可见）；本函数若在 connect 时跑，CAPS 会被丢。订阅事件
// 是 peer 可收帧的首个时刻，故 CAPS 在此交付。（ai_flush_queue 已成空转：gen26.6
// 移除入队后队列无生产者，保留调用只为形状不动。）
// gen26.2 root-cause fix: CAPS must NOT run in the BLE CONNECT event. The
// phone's CCCD subscription lands ~470 ms after the connection is established
// (measured in two sessions on hardware), and notify_status() drops frames for
// a peer that has not subscribed (LOGD, invisible in release); running at
// connect time would drop CAPS. The subscribe event is the first moment the
// peer can receive, so CAPS is delivered here. (ai_flush_queue is now a no-op:
// the enqueue path was removed in gen26.6, so the queue has no producer; the
// call is kept only to leave the shape untouched.)
static void ai_on_subscribed(void) {
    if (!s_ai_enabled) return;
    ai_send_caps();
    ai_flush_queue();
}

// 释义回流（0x07）：进预取缓存 + 交 UI 槽出卡。/ Definition result (0x07): into
// the prefetch cache and to the UI slot.
static void ai_on_result(const char *word, const char *def) {
    ai_cache_put(word, def);
    ai_surface(word, def);
}

/* ---- gen26 AI 查词：公开 API（main / UI 调用）---- */
/* gen26 AI lookup: the public API (called by main / the UI). */

void push_sink_attach_ai_buffer(uint8_t *buf, size_t len, uint16_t cap) {
    s_ai_buf = buf;
    s_ai_buf_len = len;
    s_ai_cap = cap;
    s_ai_ready = buf && ai_raloc_init(buf, len, cap);
    if (buf && !s_ai_ready) {
        ESP_LOGW(TAG, "ai ride-along buffer attach failed (len=%u cap=%u)", (unsigned)len, (unsigned)cap);
    }
}

bool push_sink_ai_enabled(void) { return s_ai_enabled; }

void push_sink_set_ai_enabled(bool on) {
    s_ai_enabled = on;
    nvs_handle_t h = 0;
    if (nvs_open(PUSH_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, PUSH_NVS_KEY_AI, on ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

void push_sink_set_scene_enabled(bool on) {
    if (s_scene_enabled == on) return;
    s_scene_enabled = on;
    nvs_handle_t h = 0;
    if (nvs_open(PUSH_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, PUSH_NVS_KEY_SCENE, on ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    // 能力位随开关变化，重报一次让 APK 立刻看到（与 context_ready 同一理由）。
    // The capability bit follows the switch; re-advertise so the APK sees it now.
    if (s_status_subscribed) {
        lock();
        ai_send_caps();
        unlock();
    }
}

bool push_sink_scene_enabled(void) { return s_scene_enabled; }

// M3-B：对面的 APK 是否真能出四节情景分析（bit4）。入口显示用这个谓词，不要用
// 本地开关——本地开着但对面是 v1.6.1 时，点进去只会等一场空。
// M3-B: whether the peer APK can actually answer with four sections (bit4). Use
// this for the entry's visibility, not the local switch: with v1.6.1 on the other
// side, tapping would only wait for an answer that never comes.
bool push_sink_ai_scene_view_enabled(void) {
    return (s_apk_caps & PUSH_CAP_AI_SCENE_VIEW) != 0;
}

void push_sink_ai_set_context_ready(bool ready) {
    s_ai_ctx_ready = ready;
    // 就绪状态变了要重报能力：APK 可能正等着 CONTEXT 位才发 0x09。
    // Re-advertise on change: the APK may be waiting on the CONTEXT bit.
    if (ready && s_status_subscribed) {
        lock();
        ai_send_caps();
        unlock();
    }
}

uint32_t push_sink_apk_caps(void) { return s_apk_caps; }

void push_sink_ai_tap_word(const char *word) {
    push_sink_ai_tap_word_ctx(word, 0u, 0u); // 无书上下文（词典弹窗等）/ no book context
}

void push_sink_ai_tap_word_ctx(const char *word, uint32_t book_id, uint32_t pos) {
    if (!word || !*word) return;
    if (!s_ai_enabled) return;
    // gen26.6.2 离线缓存命中：**预取缓存是设备自己的记忆，不是补查**——命中即本地出卡，
    // **与连接态无关**（离线 + 已缓存词同样秒出）。它与「断连不入队、错过让它过去」
    // 同哲学：缓存是已经拿到的答案，补查才是去要新答案。故这一查必须排在 live 门之前，
    // 顺序颠倒即回归缺陷（wiring 守门 + 变异钉死）。/ gen26.6.2 offline cache hit: the
    // prefetch cache is the device's OWN memory, not a re-query - a hit surfaces the
    // card locally REGARDLESS of link state. Same philosophy as "no offline queue, let
    // missed words pass": the cache is an answer already received, a re-query asks for a
    // new one. So this lookup must run BEFORE the live gate; flipping the order is the
    // regression (pinned by the wiring guard and a mutation).
    {
        const char *cached = ai_cache_find(word);
        if (cached) {
            // 26.9.9 取证：命中即出卡、**不发 0x17**——这一行是「为何反复点都一样」的
            // 直接证据：第 2、3 次点击日志里没有 notify 时，只有它能说明卡从哪来。
            // 26.9.9 evidence: a hit renders WITHOUT sending 0x17 - this line is the
            // direct proof for "why every tap shows the same thing": with no notify on
            // the 2nd/3rd tap, only this explains where the card came from.
            ESP_LOGI(TAG, "ai tap: cache=HIT klen=%u key=\"%s\" dlen=%u",
                     (unsigned)strlen(word), word, (unsigned)strlen(cached));
            ai_surface(word, cached);
            return; // 命中预取缓存：秒出卡
        }
        ESP_LOGI(TAG, "ai tap: cache=MISS klen=%u key=\"%s\"",
                 (unsigned)strlen(word), word);
    }
    lock();
    const bool live = s_conn_handle != BLE_HS_CONN_HANDLE_NONE && s_status_subscribed;
    if (live) {
        // gen26.5：帧格式由协商结果决定（未使能 → 66B legacy，旧 APK 零感知）。
        // gen26.5: the frame format follows the negotiation (legacy until enabled).
        uint8_t frame[PUSH_ST_AI_CTX_QUERY_LEN];
        const size_t n = push_st_ai_query_emit(word, book_id, pos, ai_ctx_enabled(),
                                               frame, sizeof(frame));
        if (n) notify_status(frame, n); // 即时：STATUS 0x17
    }
    // gen26.6 裁定：离线 miss 词**直接丢弃，不再入搭车队列**。FGS 落地后息屏
    // 也能查词，断线补查的窗口价值归零——「错过的词让它过去」。UI 侧 15s 超时
    // 回退词典照旧（离线本就发不出 0x17，弹卡超时行为前后一致），重连后零回流
    // 零打扰。该设计在无 FGS 时代是合理决策，条件变了自然收敛（账本已记）。
    // 队列基础设施保留（0x06 协议位「登记即冻结」、ai_raloc 宿主测试照跑），
    // 只是从此没有生产者——ai_flush_queue 恒为空转。
    // gen26.6 ruling: an offline miss is **dropped outright, no longer queued**
    // for the ride-along reflow. With the FGS keep-alive landed, screen-off
    // lookups work, so the offline re-query window is worthless - a missed word
    // is allowed to pass. The UI still times out after 15s into the dictionary
    // fallback (offline never sent 0x17 anyway, so the card behavior is
    // identical), and reconnects stay silent. The design was reasonable in the
    // pre-FGS era and retires naturally. The queue infrastructure stays (the
    // frozen 0x06 opcode, the ai_raloc host tests) - just with no producer, so
    // ai_flush_queue is a permanent no-op.
    unlock();
}

bool push_sink_take_ai_def(char *word, size_t wcap, char *def, size_t dcap) {
    bool got = false;
    if (!word || wcap == 0 || !def || dcap == 0) return false;
    portENTER_CRITICAL(&s_ui_lock);
    if (s_ui.ai_def) {
        s_ui.ai_def = false;
        got = true;
        ai_str_put(word, wcap, s_ui.ai_word);
        ai_str_put(def, dcap, s_ui_ai_def_text);
    }
    portEXIT_CRITICAL(&s_ui_lock);
    return got;
}

/* ---- gen30 伴读：CTRL 裸帧白名单分派（哨位裁定方案 B）---- */
/* Read-along gen30: bare CTRL frame whitelist dispatch (sentinel ruling, plan B). */
/*
 * 白名单 + 长度闸门都在纯函数 push_gen30_ctrl_bare_op() 里（宿主有逐字节断言）：
 * 只有首字节 0x0A/0x0B 且总长恰等 241/4 B 才进这里；其余一律交回现制 4 B
 * 魔数校验、失败 → ERR(2)。现制帧首字节固定 0x50，与白名单零交集，故既有
 * CTRL 路径逐字不变，「未登记 op 走 ERR(2)」纪律零破坏。
 * 有效帧按 R0 §一对账规则核对 page_no（基准 = 最近 PAGE_BEGIN），不符静默丢弃；
 * 合规帧落 take 槽，由 app 层轮询消费（渲染 / 自动翻页），组件不反向调用 main。
 *
 * The whitelist and length gate live in the pure push_gen30_ctrl_bare_op() (host
 * asserts bite byte by byte): only a 0x0A/0x0B first byte with an exact 241/4 B
 * length gets here; everything else falls through to the legacy 4 B magic check
 * and ERR(2). Legacy frames start with 0x50 (disjoint), so the existing CTRL path
 * is unchanged. Valid frames are reconciled against the last PAGE_BEGIN page_no
 * and dropped on mismatch; accepted ones land in take slots for the app to poll.
 */
static bool gen30_ctrl_dispatch(const uint8_t *data, size_t len) {
    uint8_t op = 0u;
    if (!push_gen30_ctrl_bare_op(data, len, &op)) return false;
    if (op == PUSH_OP_TTS_DONE) {
        // op1 + page_no2(LE) + status1 = 4 B
        const uint16_t page_no = (uint16_t)data[1] | ((uint16_t)data[2] << 8);
        const uint8_t status = data[3];
        portENTER_CRITICAL(&s_ui_lock);
        const bool valid = s_companion_page_valid && page_no == s_companion_page_no;
        if (valid) {
            s_companion_tts_page = page_no;
            s_companion_tts_status = status;
            s_companion_tts_pending = true;
        }
        portEXIT_CRITICAL(&s_ui_lock);
        ESP_LOGI(TAG, "gen30 tts_done: page=%u status=%u %s",
                 (unsigned)page_no, (unsigned)status,
                 valid ? "accepted" : "dropped(page mismatch)");
        return true;
    }
    // op == PUSH_OP_SUMMARY：op1+mode1+page_no2+idx1+total1+dlen2+text233 = 241 B
    const uint8_t mode = data[1];
    const uint16_t page_no = (uint16_t)data[2] | ((uint16_t)data[3] << 8);
    const uint8_t idx = data[4], total = data[5];
    const uint16_t dlen = (uint16_t)data[6] | ((uint16_t)data[7] << 8);
    const uint8_t *text = data + 8;
    const uint16_t clen = dlen > PUSH_SUMMARY_TEXT_MAX
                              ? (uint16_t)PUSH_SUMMARY_TEXT_MAX : dlen;
    portENTER_CRITICAL(&s_ui_lock);
    const bool valid = s_companion_page_valid && page_no == s_companion_page_no;
    bool delivered = false, rejected = false;
    if (valid) {
        if (total < 1u || idx >= total) {
            rejected = true;    /* 畸形 idx/total：不收 / malformed shard header */
        } else {
            /* 只认「从 0 开始、idx 连续」的片。idx==0 或与暂存断链都从零重来：
               手机重发整段时不会接在上一次的半句后面。乱序、缺片、超长一律
               丢弃整段——半句摘要比没有更糟（会冒充完整结论）。
               Only shards starting at 0 and continuing in order are accepted. A
               fresh idx==0 or a broken chain restarts the accumulator, so a
               re-sent summary never appends to an orphaned half-sentence. */
            if (idx == 0u || s_summary_acc_page != page_no || s_summary_acc_next != idx) {
                s_summary_acc_len = 0;
                s_summary_acc_page = page_no;
                s_summary_acc_next = 0;
            }
            if (idx == s_summary_acc_next &&
                (size_t)s_summary_acc_len + (size_t)clen <= PUSH_SUMMARY_TOTAL_MAX) {
                memcpy(s_summary_acc + s_summary_acc_len, text, clen);
                s_summary_acc_len = (uint16_t)(s_summary_acc_len + clen);
                s_summary_acc_next = (uint8_t)(idx + 1u);
                if (s_summary_acc_next >= total) {
                    memcpy(s_companion_summary_text, s_summary_acc, s_summary_acc_len);
                    s_companion_summary_text[s_summary_acc_len] = 0;
                    s_companion_summary_len = s_summary_acc_len;
                    s_companion_summary_mode = mode;
                    s_companion_summary_idx = idx;
                    s_companion_summary_total = total;
                    s_companion_summary_page = page_no;
                    s_companion_summary_pending = true;
                    delivered = true;
                }
            } else {
                rejected = true;    /* 乱序 / 跨页残片 / 超出总量帽 */
            }
        }
    }
    portEXIT_CRITICAL(&s_ui_lock);
    ESP_LOGI(TAG, "gen30 summary: page=%u mode=%u idx=%u/%u dlen=%u %s",
             (unsigned)page_no, (unsigned)mode, (unsigned)idx, (unsigned)total,
             (unsigned)dlen,
             !valid ? "dropped(page mismatch)"
                    : delivered ? "accepted(complete)"
                    : rejected ? "dropped(shard)"
                    : "shard buffered");
    return true;
}

static void on_ctrl_write(const uint8_t *data, size_t len) {
    // 31b 第 1 条：三拍（卸卷/格式化/深睡）期间入口冻结。ARM 的 adopt 会 truncate
    // 一个正在被卸载的卷，而它是无句柄操作——判据②的计数看不见它，只能靠这道闸。
    // Three-beat window: the gate is closed. ARM's adopt truncates with no
    // handle, which conjunct 2 can never see - only this guard can stop it.
    if (entry_frozen_reply()) return;
    // gen30 哨位裁定方案 B：CTRL 方向新帧按 op 首字节裸帧，白名单分派。仅
    // {0x0A,0x0B} 且长度恰等定长才命中；其余（含畸形新帧）一律继续走现制
    // 4 B 魔数校验，失败 → ERR(2)。既有 CTRL 路径逐字不变。
    if (gen30_ctrl_dispatch(data, len)) return;
    uint8_t op;
    if (!push_ctrl_header_decode(data, len, &op)) {
        // gen14 诊断：把拒帧现场全打出来——真机一测便知是 len 短了、魔数歪了还是
        // 版本对不上。gen13 真机 ERR(2) 无法归位到具体检查（静态对表全绿），补此日志。
        // gen14 diagnostics: dump the rejected frame's context - one real-hardware
        // run tells a short len from a bad magic or version. gen13's on-hardware
        // ERR(2) could not be attributed to a specific check (all static tables
        // agreed), so this log exists.
        ESP_LOGW(TAG, "ctrl hdr rejected: len=%u head=%02x %02x %02x %02x",
                 (unsigned)len, len > 0 ? data[0] : 0, len > 1 ? data[1] : 0,
                 len > 2 ? data[2] : 0, len > 3 ? data[3] : 0);
        reply_err(PUSH_ERR_BAD_FRAME);
        return;
    }
    // gen22 蓝牙校时：SET_TIME 走在校准通道上，不受「必须武装」限制——手机只要连上
    // 就能把真值带进来。这是没有 Wi-Fi 的场景里唯一的高频校时通道（PMU 无外部晶振、
    // 深睡靠内部 RC 走时，一天能快几分钟）。BEGIN 帧布局保持冻结，故用独立操作码。
    // gen22 time-over-BLE: SET_TIME rides its own channel and is NOT gated on being
    // armed - any connected phone can bring the truth in. With no WiFi it is the only
    // high-frequency calibration path (the PMU has no crystal and its internal RC gains
    // minutes a day in sleep). The BEGIN layout stays frozen, hence a separate opcode.
    if (op == PUSH_OP_SET_TIME) {
        if (len < PUSH_CTRL_HDR_LEN + 4u) { reply_err(PUSH_ERR_BAD_FRAME); return; }
        const uint8_t *body = data + PUSH_CTRL_HDR_LEN;
        uint32_t utc = (uint32_t)body[0] | ((uint32_t)body[1] << 8) |
                       ((uint32_t)body[2] << 16) | ((uint32_t)body[3] << 24);
        if (utc < 1704067200u || utc > 4102444799u) { reply_err(PUSH_ERR_BAD_FRAME); return; }
        s_remote_utc = utc;
        s_remote_utc_pending = true;
        struct timeval tv = {.tv_sec = (time_t)utc, .tv_usec = 0};
        (void)settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "clock set from phone: %lu", (unsigned long)utc);
        return;
    }
    // gen26 AI 查词通道：与 SET_TIME 同属「连上即可用」旁路，不受「必须武装」限制。
    // 0x06 = 手机→设备触发离线队列回流（gen26.6 起回流恒空，仅回一个空转）；
    // 0x07 = 手机→设备单词 + 释义回流。
    // gen26 AI lookup: like SET_TIME, usable on connect, not gated on armed.
    // 0x06 = phone to device, flush the offline queue (a no-op since gen26.6 -
    // the queue has no producer); 0x07 = phone to device, word + definition.
    if (op == PUSH_OP_AI_QUERY) {
        ai_flush_queue();
        return;
    }
    if (op == PUSH_OP_AI_RESULT) {
        // gen26.6.4：静态化。此结构随 PUSH_AI_DEF_MAX 尺寸化（832→1264B，+432B），
        // 若留在栈上会压 NimBLE host 任务栈；GATT 写回调在 host 任务上串行执行，无重入。
        // gen26.6.4: static - the struct scales with PUSH_AI_DEF_MAX (+432B) and the
        // GATT write callback runs serially on the NimBLE host task (no re-entry).
        static push_ai_result_t r;
        if (!push_ai_result_decode(data, len, &r)) {
            // 26.9.9：失败路径也留现场——帧长 + 头四字节足以区分「短帧」「魔数歪」
            // 「版本不符」，而这三类在 26.9.8 的真机排障里全靠猜。
            // 26.9.9: the failure path keeps its scene too - frame length plus the
            // first four bytes separates short-frame / bad-magic / bad-version,
            // which on 26.9.8 hardware were pure guesswork.
            ESP_LOGW(TAG, "ai rx: op=07 len=%u decode=FAIL head=%02x %02x %02x %02x",
                     (unsigned)len, len > 0 ? data[0] : 0, len > 1 ? data[1] : 0,
                     len > 2 ? data[2] : 0, len > 3 ? data[3] : 0);
            reply_err(PUSH_ERR_BAD_FRAME);
            return;
        }
        // 26.9.9 取证：**解码之后**的 word/def 实测值。26.9.8 只能拿「设备显示了什么」
        // 反推「APK 发了什么」，而解码截断或字段错位时这两者并不等价——本条把设备真正
        // 收到的词与释义首段钉进现场，与 APK 日志对时即可定责。长度一律字节数：中文一
        // 个字 3 字节，字符数会把截断抹平。
        // 26.9.9 evidence: the DECODED word/def as actually received. On 26.9.8 the only
        // way to infer "what the APK sent" was "what the device showed", and the two
        // diverge under truncation or field misalignment. This line pins what the device
        // really got, so a timestamp join with the APK log assigns blame. Lengths are
        // bytes throughout (one CJK rune = 3 bytes; char counts hide truncation).
        {
            char def_head[40];
            ai_log_utf8_head(def_head, sizeof(def_head), r.def, 32u);
            ESP_LOGI(TAG,
                     "ai rx: op=07 len=%u decode=OK wlen=%u word=\"%s\" dlen=%u def_head=\"%s\"",
                     (unsigned)len, (unsigned)strlen(r.word), r.word,
                     (unsigned)strlen(r.def), def_head);
        }
        ai_on_result(r.word, r.def);
        return;
    }
    // gen26.5 释义分帧（0x08）：手机把长释义切帧回传，设备重组后出卡。重组器是
    // 纯逻辑模块（ai_segasm，宿主有断言），这里只做解码 + 喂帧 + 取结果。
    // gen26.5 segmented definition (0x08): the phone slices a long definition across
    // frames and the device reassembles it. The reassembler is pure logic (ai_segasm,
    // host-asserted); this site only decodes, feeds and takes the result.
    if (op == PUSH_OP_AI_RESULT_SEG) {
        push_ai_seg_t seg;
        if (!push_ai_seg_decode(data, len, &seg)) {
            ESP_LOGW(TAG, "ai seg decode rejected: len=%u", (unsigned)len);
            ai_segasm_reset(&s_ai_segasm);
            reply_err(PUSH_ERR_BAD_FRAME);
            return;
        }
        const char *text = NULL;
        size_t tlen = 0;
        const ai_segasm_status_t st = ai_segasm_feed(&s_ai_segasm, &seg, &text, &tlen);
        if (st == AI_SEGASM_DONE) {
            ai_on_result(s_ai_segasm.word, text); // 收齐才出卡（不出半截释义）
            ai_segasm_reset(&s_ai_segasm);
        } else if (st == AI_SEGASM_REJECT) {
            ESP_LOGW(TAG, "ai seg rejected: idx=%u/%u word=%s (out of order or word changed)",
                     (unsigned)seg.seg_idx, (unsigned)seg.seg_total, seg.word);
            reply_err(PUSH_ERR_BAD_FRAME);
        } else if (st == AI_SEGASM_OVERFLOW) {
            ESP_LOGW(TAG, "ai seg overflow: > %u bytes (whole item dropped)", (unsigned)AI_SEGASM_MAX);
            reply_err(PUSH_ERR_TOO_LARGE);
        }
        return;
    }
    // gen26.5 能力使能（0x09）：与 SET_TIME / AI 通道同模型——连上即可用，不受
    // 「必须武装」限制。APK 订阅 STATUS 后发一次，声明它支持哪些设备能力；
    // 设备据此决定 0x17 帧格式（见 push_st_ai_query_emit）。断连清零见 DISCONNECT。
    // gen26.5 capability enable (0x09): same model as SET_TIME / the AI channel -
    // usable on connect, not gated on armed. The APK sends it once after
    // subscribing, declaring which device capabilities it supports; that decides
    // the 0x17 frame format (push_st_ai_query_emit). Cleared on DISCONNECT.
    if (op == PUSH_OP_AI_CAPS_ENABLE) {
        push_caps_t caps;
        if (!push_ai_caps_enable_decode(data, len, &caps)) {
            ESP_LOGW(TAG, "ai caps enable rejected: len=%u head=%02x %02x %02x %02x",
                     (unsigned)len, len > 0 ? data[0] : 0, len > 1 ? data[1] : 0,
                     len > 2 ? data[2] : 0, len > 3 ? data[3] : 0);
            reply_err(PUSH_ERR_BAD_FRAME);
            return;
        }
        // 只认本代已知的协商位：未来 APK 声明新位时旧固件不会被未知位污染
        // （未知位一律忽略，行为与「未声明」一致）。
        // Only the negotiation bits this build knows: a future APK declaring new
        // bits cannot pollute old firmware - unknown bits are ignored, i.e. behave
        // exactly like "not declared".
        // M4-A 自纠：新增 SCENE 位时**必须同步本掩码**——漏了就等于「APK 声明了也被
        // 静默清掉」，而且因为这里打印的是采纳值，版本判据会被一起污染（v1.5.2 的
        // 0x03 与 v1.6.0 的 0x0b 过滤后逐字相同，都打印成 0x02，上两轮据此误判过）。
        // 现在 raw 与 adopted 双打印：raw 定 APK 版本，adopted 定本固件实际认可的能力。
        // M4-A correction: adding the SCENE bit **must** update this mask - omitting it
        // means "declared then silently cleared", and because the log printed the
        // adopted value it poisoned the version evidence too (v1.5.2's 0x03 and
        // v1.6.0's 0x0b both print as 0x02 after filtering; two rounds mis-read it).
        // Both raw and adopted are now printed: raw tells the APK version, adopted
        // tells what this firmware actually honours.
        s_apk_caps = caps.bits & (PUSH_CAP_AI_CONTEXT | PUSH_CAP_AI_MULTI | PUSH_CAP_AI_SCENE |
                                  PUSH_CAP_AI_SCENE_VIEW);
        ESP_LOGI(TAG, "apk caps enable: raw=0x%08x adopted=0x%08x (ctx=%d multi=%d scene=%d)",
                 (unsigned)caps.bits, (unsigned)s_apk_caps,
                 (s_apk_caps & PUSH_CAP_AI_CONTEXT) ? 1 : 0,
                 (s_apk_caps & PUSH_CAP_AI_MULTI) ? 1 : 0,
                 (s_apk_caps & PUSH_CAP_AI_SCENE) ? 1 : 0);
        return;
    }
    // 未武装：任何 CTRL 写入都回 ERR(1)，状态机看都不看。/ Unarmed: every CTRL
    // write gets ERR(1) without the machine ever seeing it.
    if (s_sess.state == PUSH_STATE_IDLE) {
        reply_err(PUSH_ERR_NOT_ARMED);
        return;
    }
    switch (op) {
    case PUSH_OP_BEGIN: {
        // 传输中再开新会话：ERR(7)，让手机等当前这条完事。/ A new session mid-transfer:
        // ERR(7); the phone waits for the current one to finish.
        if (s_sess.state == PUSH_STATE_RECEIVING || s_sess.state == PUSH_STATE_PROCESSING) {
            reply_err(PUSH_ERR_BUSY);
            return;
        }
        push_begin_t b;
        if (!push_begin_decode(data, len, &b)) {
            // gen14 诊断：len 不足 83、魔数/op/版本错都会落这里。打出现场。
            // gen14 diagnostics: a len under 83 or a bad magic/op/version lands
            // here; print the scene.
            ESP_LOGW(TAG, "begin decode rejected: len=%u head=%02x %02x %02x %02x",
                     (unsigned)len, data[0], data[1], data[2], data[3]);
            reply_err(PUSH_ERR_BAD_FRAME);
            return;
        }
        const int err = begin_precheck(&b);
        if (err != PUSH_ERR_NONE) {
            // gen17：err=9 的措辞必须诚实——它只说明「词典探针此刻判定不可用」，
            // **不等于卡上没有 dict.bin**（gen16 就栽在这句上，把用户支去查卡）。
            // 词典侧的 dict_open 会另行打印尝试的完整绝对路径与 errno，两边对着看。
            // gen17: the err=9 wording must be honest - it only means "the
            // dictionary probe says unavailable right now", **not** "there is no
            // dict.bin on the card" (gen16 tripped on exactly that and sent the
            // user to inspect their card). The dictionary side logs the full
            // absolute path attempted plus errno separately; read the two together.
            if (err == PUSH_ERR_NO_DICT) {
                ESP_LOGW(TAG, "dict probe unavailable (NOT proof the card lacks a dict) "
                              "type=%u token=%04x total=%u",
                         (unsigned)b.type, (unsigned)b.token, (unsigned)b.total_len);
            } else {
                ESP_LOGW(TAG, "begin precheck err=%d type=%u token=%04x total=%u",
                         err, (unsigned)b.type, (unsigned)b.token, (unsigned)b.total_len);
            }
            reply_err(err);
            return;
        }
        push_event_t evt;
        memset(&evt, 0, sizeof(evt));
        evt.begin = &b;
        exec_actions(push_session_event(&s_sess, PUSH_EVT_BEGIN, &evt));
        return;
    }
    case PUSH_OP_END:
        // 判定进行中再催：ERR(7)，判定只要几百毫秒。/ A second END while the verdict
        // is in flight: ERR(7); verification takes only a few hundred ms.
        if (s_sess.state == PUSH_STATE_PROCESSING) {
            reply_err(PUSH_ERR_BUSY);
            return;
        }
        // 最后一包若还欠着，必须在收官之前补上：END 之后不再有 DATA，欠账槽
        // 里那 241 B 就是这篇的全部缺口。
        // A packet still owed must land before the closing beat: no DATA follows
        // END, so those 241 B would be the article's only hole.
        if (!push_part_drain()) {
            exec_actions(push_session_event(&s_sess, PUSH_EVT_CANCEL, NULL));
            reply_err(PUSH_ERR_NO_CARD);
            return;
        }
        exec_actions(push_session_event(&s_sess, PUSH_EVT_END, NULL));
        return;
    case PUSH_OP_RESUME_QUERY:
        // 传输中问进度：直接拿当前 offset 答，不经过状态机（它会当成非法转移）。
        // A progress query mid-transfer: answer with the current offset directly;
        // the machine would treat it as an illegal transition.
        if (s_sess.state == PUSH_STATE_RECEIVING || s_sess.state == PUSH_STATE_PROCESSING) {
            reply_resume();
            return;
        }
        exec_actions(push_session_event(&s_sess, PUSH_EVT_RESUME_QUERY, NULL));
        return;
    case PUSH_OP_ABORT:
        // 手机放弃本次会话：等同取消，.part 作废。/ The phone abandons the session:
        // same as cancel, the partial is discarded.
        exec_actions(push_session_event(&s_sess, PUSH_EVT_CANCEL, NULL));
        return;
    default:
        // gen14 诊断：op 不在 1/2/3/4。/ gen14: op outside 1/2/3/4.
        ESP_LOGW(TAG, "ctrl op %u rejected: len=%u", (unsigned)op, (unsigned)len);
        reply_err(PUSH_ERR_BAD_FRAME);
        return;
    }
}

static void on_data_write(const uint8_t *data, size_t len) {
    if (entry_frozen_reply()) return; // 31b：三拍期间入口冻结 / frozen entry gate
    if (s_sess.state == PUSH_STATE_IDLE) {
        reply_err(PUSH_ERR_NOT_ARMED);
        return;
    }
    // 只在 RECEIVING 收正文；其余态说明手机不按牌理出牌，明确拒绝而不是装死。
    // Body packets are taken only while RECEIVING; any other state means the
    // phone is out of order, so refuse explicitly instead of ignoring it.
    if (s_sess.state != PUSH_STATE_RECEIVING) {
        // gen14 诊断：非 RECEIVING 态收到 DATA，打状态与来帧。
        // gen14 diagnostics: DATA in a non-RECEIVING state; log state and frame.
        ESP_LOGW(TAG, "data rejected: state=%u len=%u head=%02x %02x %02x",
                 (unsigned)s_sess.state, (unsigned)len,
                 len > 0 ? data[0] : 0, len > 1 ? data[1] : 0, len > 2 ? data[2] : 0);
        reply_err(PUSH_ERR_BAD_FRAME);
        return;
    }
    uint16_t seq;
    const uint8_t *payload;
    size_t n;
    if (!push_data_decode(data, len, &seq, &payload, &n)) {
        // gen14 诊断：len<3 或声明长于实长。/ gen14: len<3 or a declared length
        // longer than the actual payload.
        ESP_LOGW(TAG, "data decode rejected: len=%u head=%02x %02x %02x",
                 (unsigned)len, len > 0 ? data[0] : 0, len > 1 ? data[1] : 0,
                 len > 2 ? data[2] : 0);
        reply_err(PUSH_ERR_BAD_FRAME);
        return;
    }
    s_rx_payload = payload;
    s_rx_len = n;
    push_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.data = payload;
    evt.data_len = n;
    evt.seq = seq;
    exec_actions(push_session_event(&s_sess, PUSH_EVT_DATA, &evt));
    s_rx_payload = NULL;
    s_rx_len = 0;
}

/* ---- GATT 访问回调 / The GATT access callback ---- */

static int push_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)arg;
    ensure_init();
    lock();
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && attr_handle == s_status_handle) {
        // 读回退通道：把最近一条 STATUS 原文交出去。/ The read fallback: hand out
        // the last STATUS verbatim.
        const int rc = os_mbuf_append(ctxt->om, s_last_status, (uint16_t)s_last_status_len);
        unlock();
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        unlock();
        return 0; // CCCD 由 NimBLE 内部处理，不进这里 / the CCCD stays inside NimBLE
    }
    // 扁平化这一帧：MTU 247 以内，512 字节缓冲足够且永不需要截断。
    // Flatten the frame: within MTU 247, a 512-byte buffer suffices and never truncates.
    uint8_t buf[512];
    uint16_t len = os_mbuf_len(ctxt->om);
    if (len > sizeof(buf)) len = (uint16_t)sizeof(buf);
    int rc = os_mbuf_copydata(ctxt->om, 0, len, buf);
    if (rc != 0) {
        unlock();
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (attr_handle == s_data_handle) on_data_write(buf, len);
    else if (attr_handle == s_ctrl_handle) on_ctrl_write(buf, len);
    unlock();
    return 0;
}

/* ---- GAP 事件 / GAP events ---- */

static int push_gap_cb(struct ble_gap_event *event, void *arg) {
    (void)arg;
    ensure_init();
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            // 手机没连上：广播还在，等下一个。/ The phone did not connect: advertising
            // stays up for the next attempt.
            ESP_LOGI(TAG, "connect failed: %d", event->connect.status);
            return 0;
        }
        lock();
        s_conn_handle = event->connect.conn_handle;
        s_notify_retry_left = 3; // gen14：每个连接周期 3 次重投配额 / 3 retries per cycle
        ESP_LOGI(TAG, "phone connected");
        exec_actions(push_session_event(&s_sess, PUSH_EVT_CONNECT, NULL));
        // gen26.2：CAPS + 回流**不在此处**做——此刻 peer 尚未订阅，notify 全丢
        // （详见 ai_on_subscribed 注释）。改在 SUBSCRIBE 事件交付。
        // gen26.2: CAPS + flush are NOT done here - the peer has not subscribed
        // yet and every notify would be dropped (see ai_on_subscribed). They are
        // delivered in the SUBSCRIBE event instead.
        unlock();
        // 26.9.13 P0-d：连接建立后的堆快照。放在 unlock 之后：上面那段持着 s_lock，
        // printf 几十行不该占着这把锁。真机上「连上就断」若源于进连接时的分配失败，
        // 这一份是唯一能对上号的定格（NimBLE 自己的 alloc 失败不打堆数字）。
        // 26.9.13 P0-d: heap at connection establishment, after unlock - the section
        // above holds s_lock and dozens of printf lines have no business holding it.
        // If "connects then drops" is an allocation failure at connect time, this is
        // the only freeze-frame that lines up (NimBLE's own alloc failure prints no
        // heap figures).
        pico_heap_dump("push-connect", "push_sink:connect");
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected: %d", event->disconnect.reason);
        // 26.9.13 P0-d：断开这一刻的堆快照。26.9.12 的日志里**没有任何断开行**，
        // 于是「连接失效」到底是分配失败、8 s 监督超时还是应用收摊，只能猜三选一。
        // 这里把 reason 与当时的 free/largest 定格在同一行附近：reason=8（监督超时）
        // 与 reason=19/22（对端/本地主动断）在判读上是完全不同的两条路，而 largest
        // 低于 NimBLE 单次分配需求则指向「控制器分配失败」。**先有这一行，才允许谈
        // 是否要动 BLE 参数**——参数盲调是本仓库反复付学费的同一类错误。
        // 26.9.13 P0-d: the heap at the instant of disconnect. The 26.9.12 log had NO
        // disconnect line at all, so "the link fails sometimes" could only be guessed
        // among three causes. Freezing reason and free/largest together separates them:
        // reason=8 (supervision timeout) and reason=19/22 (peer/local terminated) are
        // different paths, and a largest block below NimBLE's allocation need points at
        // controller allocation failure. **This line has to exist before anyone touches
        // a BLE parameter** - blind parameter tuning is the mistake this repo keeps
        // paying tuition for.
        pico_heap_dump("push-disc", "push_sink:disconnect");
        lock();
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_status_subscribed = false;
        // gen26.5：能力声明随连接作废。对端进程可能已被杀，能力不可跨连接继承——
        // 重连后 0x17 一律回到 legacy，等 APK 重新发 0x09（旧 APK 不发即恒 legacy）。
        // gen26.5: the capability declaration dies with the connection. The peer
        // process may be gone, so capabilities never carry over - after reconnect
        // 0x17 falls back to legacy until the APK re-sends 0x09.
        s_apk_caps = 0u;
        ai_segasm_reset(&s_ai_segasm); // 半截重组不跨连接 / no half item survives
        // gen14：断开即撤重投配额，防 timer 到点往下一个连接周期野发。
        // gen14: drop the retry quota on disconnect so the timer cannot fire
        // into the next connection cycle.
        notify_retry_cancel();
        exec_actions(push_session_event(&s_sess, PUSH_EVT_DISCONNECT, NULL));
        // 断连即存档：.part 已逐包落盘，NVS 记账对齐到这一刻，跨重启续传的锚点。
        // Checkpoint on drop: the partial is already flushed per packet and the
        // NVS bookkeeping aligns to this moment - the anchor for cross-reboot resume.
        if (s_sess.has_part) nvs_save_session();
        // 窗口还开着就把广播找回来，手机要能重连。/ The window is still open: bring
        // advertising back so the phone can reconnect.
        if (s_sess.state != PUSH_STATE_IDLE && !ble_gap_adv_active()) adv_start();
        unlock();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_status_handle) {
            s_status_subscribed = event->subscribe.cur_notify != 0;
            ESP_LOGI(TAG, "status notify %s (reason %u)",
                     s_status_subscribed ? "on" : "off", (unsigned)event->subscribe.reason);
            if (s_status_subscribed) {
                // gen26.2：订阅是 peer 可收帧的首个时刻——CAPS + 回流离线队列改在
                // 此交付（connect 时做会被全丢 + 吃词，根因见 ai_on_subscribed）。
                // gen26.2: subscription is the first moment the peer can receive -
                // CAPS + the offline-queue flush are delivered here (doing it at
                // connect dropped every frame and ate the words; root cause in
                // ai_on_subscribed).
                lock();
                ai_on_subscribed();
                unlock();
            }
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu %u", (unsigned)event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        // reason 0 = 连上后正常收官；其余（被抢占/异常）且仍在武装期就重起广播。
        // reason 0 is the normal close after a connect; anything else while still
        // armed restarts advertising.
        if (event->adv_complete.reason != 0 && s_sess.state != PUSH_STATE_IDLE) {
            ESP_LOGW(TAG, "adv complete reason %d, restarting", event->adv_complete.reason);
            lock();
            if (s_sess.state != PUSH_STATE_IDLE && !ble_gap_adv_active()) adv_start();
            unlock();
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_TX:
        if (event->notify_tx.status != 0) {
            // gen14：空口发送失败过去只打 LOGD（release 不可见）——真机症状正是
            // 「设备处理了、手机等不到 STATUS 超时」。升级 LOGW 并排重投。
            // gen14: an on-air failure used to log at LOGD only (invisible in
            // release) - exactly the "device processed it, the phone times out
            // waiting for STATUS" symptom. Escalate to LOGW and schedule a retry.
            ESP_LOGW(TAG, "notify tx failed status=%d conn=%u attr=%u, retry scheduled",
                     event->notify_tx.status, (unsigned)event->notify_tx.conn_handle,
                     (unsigned)event->notify_tx.attr_handle);
            lock();
            notify_retry_schedule();
            unlock();
        }
        return 0;

    default:
        return 0;
    }
}

/* ---- 断点恢复 / Resume bookkeeping ---- */

// 重新武装时把 NVS 里的半成品会话装回来：文件与记账对账（文件长于记账就切到
// 记账处——多出来的字节是没落 NVS 的包，手机会重发；短于记账视为损坏，清场）。
// 卡没挂载时（stat 失败）什么都不动：NVS 簿记是跨重启的锚，不能因为卡迟到
// 一会儿就撕票，等卡回来了下次武装再对账。
// On re-arm, load a partial session back from NVS, reconciling file against
// bookkeeping (a file longer than bookkeeping is cut back to it - the extra bytes
// are packets that never reached NVS and the phone will resend them; a shorter
// file is corruption, so clear everything). With the card unmounted (stat fails)
// nothing is touched: the NVS bookkeeping is the cross-reboot anchor and must
// not be torn up for a late card; reconcile on the next arm once it is back.
static void try_adopt(void) {
    // 31b：冻结期间不 adopt。truncate 无句柄，句柄计数看不见它（扩句 B 的原话）。
    // No adopt while frozen: the truncate carries no handle, so the file count
    // can never see it.
    if (push_sink_frozen()) return;
    push_nvs_session_t nv;
    // .bak 捡回先于会话判定：.bak 是上一次提交被打断留下的旧书，与 .part 的
    // 会话簿记无关。失败不阻断后续——.part 的处置不依赖 .bak 的归宿。
    // Recover .bak before looking at the session: it belongs to an interrupted
    // commit, not to the .part bookkeeping. A failure here does not block.
    if (!recover_bak_if_needed()) ESP_LOGW(TAG, "boot: bak recovery failed, continuing");
    if (!nvs_load_session(&nv)) {
        push_part_discard(); // 孤儿 .part：没有会话就不留 / an orphan partial: no session, no file
        return;
    }
    // 26.9.30 commit 5：文件与账本的对齐整个下沉到 push_part_adopt()，三态显式。
    // 「账本在 ARM 时刻就位」那一行现在住在 push_part.c 里——宿主用例 16e 能咬住
    // 它，而它留在这里时谁也咬不住（本文件编不进宿主）。
    // Commit 5: the file/ledger alignment moved into push_part_adopt() with three
    // explicit outcomes. The "ledger in place at ARM time" line now lives in
    // push_part.c, where host case 16e can bite it; here nothing could.
    const push_part_adopt_t adopted = push_part_adopt(nv.received);
    if (adopted == PUSH_PART_ADOPT_NO_CARD) return; // 簿记原样留着 / bookkeeping stays
    if (adopted == PUSH_PART_ADOPT_CORRUPT) {
        nvs_clear_session();
        push_part_discard();
        return;
    }
    push_session_adopt(&s_sess, &nv.begin, nv.received, nv.next_seq);
    ESP_LOGI(TAG, "adopted session %u at %u B, next seq %u",
             (unsigned)nv.begin.session_id, (unsigned)nv.received, (unsigned)nv.next_seq);
}

/* ---- 栈钩子 / Stack hooks ---- */

void push_sink_on_stack_sync(void) {
    ensure_init();
    lock();
    s_stack_up = true;
    (void)ble_hs_id_infer_auto(0, &s_own_addr_type);
    // 常驻注册已把定义排在 host 启动之前，host 自己的 ble_gatts_start() 此时
    // 应当已经消费；这里实证核对句柄，万一没注册上就显式补一次 start。
    // The resident registration queued the definitions before the host
    // started, so the host's own ble_gatts_start() should have consumed them
    // by now; verify the handles and, failing that, start explicitly.
    gatt_ensure_registered();
    if (s_pending_arm) {
        // 待武装续上：ARM（→ADV_ON→广播→ADV_UP→WINDOW）。
        // Complete a pending arm: ARM -> ADV_ON -> advertising -> ADV_UP -> WINDOW.
        s_pending_arm = false;
        push_event_t evt;
        memset(&evt, 0, sizeof(evt));
        evt.token = s_pending_token;
        evt.window_ms = s_pending_window_ms;
        evt.cooldown_ms = PUSH_COOLDOWN_MS;
        evt.now_ms = ms_now();
        exec_actions(push_session_event(&s_sess, PUSH_EVT_ARM, &evt));
    } else if (s_sess.state != PUSH_STATE_IDLE) {
        // 栈被 WiFi/USB 抢占过：先让过期的窗口自己收官，还活着就把广播找回来。
        // The stack was preempted by WiFi/USB: let an expired window close itself,
        // then bring advertising back if the session still lives.
        push_event_t evt;
        memset(&evt, 0, sizeof(evt));
        evt.now_ms = ms_now();
        exec_actions(push_session_event(&s_sess, PUSH_EVT_TICK, &evt));
        if (s_sess.state != PUSH_STATE_IDLE && !ble_gap_adv_active()) adv_start();
    }
    unlock();
}

void push_sink_on_stack_stop(void) {
    ensure_init();
    lock();
    // gen18：武装期栈被拆（网络租约或任何漏网入口）必须留痕——广播丢了，
    // 主循环 sync 会按 armed 把栈找回来，但这一幕不许无声无息地过去。
    // gen18: a stack teardown while armed (a network lease or any missed
    // entry) must leave a trace - the advertisement is lost and the loop's
    // sync brings the stack back per armed, but this moment must not pass
    // silently.
    if (s_sess.state != PUSH_STATE_IDLE) {
        ESP_LOGW(TAG, "BLE stack stopped while armed (state=%d) - advertisement lost, sync restores it",
                 (int)s_sess.state);
    }
    s_stack_up = false;
    adv_stop();
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_status_subscribed = false;
    s_gatt_queued = false;
    s_data_handle = s_ctrl_handle = s_status_handle = 0;
    // 状态机与 .part/NVS 都不动：栈回来后续传。/ Neither the machine nor the
    // partial/NVS session is touched: resume works after the stack returns.
    unlock();
}

bool push_sink_turner_paused(void) {
    ensure_init();
    lock();
    const bool paused = s_sess.state != PUSH_STATE_IDLE;
    unlock();
    return paused;
}

/* ---- 公共 API / Public API ---- */

/* ---- 三拍封送 / Three-beat quiesce (31b 第 1 条) ---- */

// 判据②的读数：`.part` 写句柄 + verify 读句柄。两处都必须数——只数 .part 会
// 漏掉 do_verify 的 fopen("rb")，那正是 B51 当年被判「不可见」的地方。
// 调用方须持 s_ui_lock。
// The reading for conjunct 2: both the .part writer and the verify reader. Only
// counting the writer is exactly the blind spot B51 was demoted for.
static unsigned live_files_locked(void) {
    unsigned files = s_verify_depth;
    if (push_part_is_open()) ++files;
    return files;
}

bool push_sink_quiesce(unsigned timeout_ms) {
    ensure_init();
    const uint32_t started = ms_now();
    portENTER_CRITICAL(&s_ui_lock);
    s_freeze_flags = FRZ_FROZEN; // 新的三拍区间：上一轮的毒卷标记与旧事件一并作废
    s_frozen_ms = started;
    portEXIT_CRITICAL(&s_ui_lock);

    // 先把「冻结」写在前面、等待放在后面：入口从这一刻起就关了，等待期间进来的
    // BLE 写会被三处判点挡下。顺序反过来会留一个「等待中还能开新会话」的洞。
    // Freeze first, wait second: the gate closes at this instant, so anything the
    // BLE stack delivers while we wait hits the three guards.
    for (;;) {
        lock();
        const int state = (int)s_sess.state;
        const int pending = (int)s_pending_arm;
        const bool pending_arm = s_pending_arm;
        unlock();
        // 两把锁各管各的：会话状态在 s_lock 下读，FILE* 计数在 s_ui_lock 下读。
        // 先各自取值、再合成判据——判据本身是纯函数（B77 咬它），不跨锁。
        // Two locks, two readings: session state under s_lock, FILE* count under
        // s_ui_lock; they are combined afterwards by a pure predicate (B77).
        portENTER_CRITICAL(&s_ui_lock);
        const unsigned files = live_files_locked();
        portEXIT_CRITICAL(&s_ui_lock);
        // 26.11.5：判据搬进 push_session.c 的纯函数，宿主测试可全边覆盖。
        // 「在传」按枚举序 = >= RECEIVING；CONNECTED 只握着链路，不握介质。
        // 26.11.5: the predicate moved into a pure function so the host test can cover
        // every edge. "Mid-transfer" is enum-ordered (>= RECEIVING); CONNECTED holds
        // a link, not the medium.
        const bool idle = push_quiesce_idle((push_state_t)state, pending_arm, files);

        const uint32_t waited = ms_now() - started;
        if (idle) {
            ESP_LOGI(TAG, "quiesce ok in %u ms (state=%d files=%u)", (unsigned)waited, state, files);
            return true;
        }
        if (waited >= timeout_ms) {
            // 留痕：谁没停、卡在哪。真机验收要看这一行。/ Who refused to stop.
            ESP_LOGW(TAG, "quiesce timeout after %u ms: state=%d pending_arm=%d files=%u",
                     (unsigned)waited, state, pending, files);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void push_sink_thaw(const char *why) {
    portENTER_CRITICAL(&s_ui_lock);
    const bool was = (s_freeze_flags & FRZ_FROZEN) != 0;
    s_freeze_flags = 0; // 解冻同时清掉毒卷标记：下一次三拍从干净状态起算
    portEXIT_CRITICAL(&s_ui_lock);
    if (was) ESP_LOGI(TAG, "thaw: %s", why ? why : "?");
}

/* ---- 26.11.5 介质离开 / 插回：链路的所有权与对称义务 ---- */
// 谁拥有链路，谁负责断；断了就要负责把它接回来。
// Whoever owns the link owns cutting it - and owns reconnecting it.
//
// 场景（26.11.4 真机）：手机 APK 常驻、BLE 长连，用户拔卡。手机还连着，
// 于是 quiesce 的原判据（state == IDLE）永远不成立——判据本身在 26.11.5 已修
// （push_quiesce_idle），但只放行卸载还不够：卷走了，这个会话再收到 BEGIN 就会
// 往不存在的介质里写。所以要当场把链路与广播一起收掉，语义也干净：「卡走了，
// 这个会话没有意义了」。
// The 26.11.4 field scene: a resident APK keeps the link up while the user pulls
// the card, so the old quiesce predicate never held (fixed in 26.11.5). Letting the
// unmount through is still not enough - with the volume gone, a later BEGIN would
// write into nothing. So the link and the advertisement are dropped together.
//
// 顺序：先让状态机走 DISCONNECT（关 .part、回窗口、存 NVS 簿记），再 CANCEL 到
// IDLE（停下广播），最后 terminate 链路。**不能只发 DISCONNECT**：会话停在 WINDOW，
// 真机的 GAP 断连回调会按「state != IDLE」把广播重新拉起来，手机立刻能再连上一个
// 正在被卸载的卷——那是把故障挪个位置，不是修。
// Order: DISCONNECT first (closes .part, returns to the window, saves the NVS
// bookkeeping), then CANCEL to IDLE (advertising off), then terminate the link.
// DISCONNECT alone is not enough: the session would sit in WINDOW and the real GAP
// disconnect callback would restart advertising for a volume that is being unmounted.
static bool s_media_detached; // 我们断过链，插回时要还一次可连性 / we cut it, we owe a reconnect

void push_sink_detach_media(const char *why) {
    ensure_init();
    lock();
    const uint16_t handle = s_conn_handle;
    const bool live = handle != BLE_HS_CONN_HANDLE_NONE || s_sess.state != PUSH_STATE_IDLE;
    if (live) s_media_detached = true;
    // 与真机 GAP 断连回调同一套清扫，只是由我们主动发起。
    // The same sweep the real GAP disconnect does, only initiated by us.
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_status_subscribed = false;
    s_apk_caps = 0u;
    ai_segasm_reset(&s_ai_segasm);
    notify_retry_cancel();
    exec_actions(push_session_event(&s_sess, PUSH_EVT_DISCONNECT, NULL));
    // 断连即存档：.part 已逐包落盘，簿记对齐到这一刻（同 DISCONNECT 回调）。
    // Checkpoint on drop: the partial is flushed per packet, bookkeeping aligns here.
    if (s_sess.has_part) nvs_save_session();
    // 回不到 IDLE 就再推一拍 CANCEL——**不带 DROP**：卡已不在，删 .part 只会换来
    // 一串 EIO；.part 留在卡上，插回后 probe/remount 照常处理。
    // A second beat to IDLE, without DROP: the card is gone, removing .part only buys
    // EIO noise; the partial stays on the card and the remount handles it.
    if (s_sess.state != PUSH_STATE_IDLE) exec_actions(push_session_event(&s_sess, PUSH_EVT_CANCEL, NULL));
    adv_stop();
    const int state_after = (int)s_sess.state;
    unlock();
    if (handle != BLE_HS_CONN_HANDLE_NONE) {
        // 0x13 = remote user terminated connection（同 ble_page_turner 的用法）。
        const int rc = ble_gap_terminate(handle, 0x13);
        ESP_LOGW(TAG, "detach media: link terminated rc=%d state=%d (%s)", rc, state_after,
                 why ? why : "?");
    } else {
        ESP_LOGI(TAG, "detach media: no live link, session=%d (%s)", state_after, why ? why : "?");
    }
}

bool push_sink_resume_after_media(void) {
    ensure_init();
    lock();
    const bool owed = s_media_detached;
    s_media_detached = false;
    const push_state_t st = s_sess.state;
    const bool pending = s_pending_arm;
    const bool stack = s_stack_up;
    if (owed && stack && st != PUSH_STATE_IDLE && !ble_gap_adv_active()) adv_start();
    unlock();
    if (!owed) return false;
    if (st == PUSH_STATE_IDLE && !pending) {
        // 会话已经收官（窗口到期或被取消）：重开一个默认窗口，手机才连得回来。
        // 用与传书页进场相同的窗口长度，语义是「恢复被我们打断的那一回合」。
        // The round already closed (window expired or cancelled): reopen a default
        // window so the phone can reconnect - same length as the transfer page's
        // entry window, since this restores the round we interrupted.
        const esp_err_t err = push_sink_arm(0, PUSH_WINDOW_DEFAULT_MS);
        ESP_LOGI(TAG, "re-arm after media: %s", esp_err_to_name(err));
        return err == ESP_OK;
    }
    ESP_LOGI(TAG, "re-arm after media: session=%d still live, advertising restored", (int)st);
    return true;
}


void push_sink_quiesce_hold(const char *why, push_hold_why_t why_code) {
    portENTER_CRITICAL(&s_ui_lock);
    s_freeze_flags = FRZ_FROZEN | FRZ_HOLD;
    s_frozen_ms = ms_now(); // 重新起算：这是「有意保持」，不是「卡了 20 s」/ a deliberate hold, not a stuck flow
    const unsigned still_open = live_files_locked();
    portEXIT_CRITICAL(&s_ui_lock);
    ESP_LOGW(TAG, "quiesce hold: %s (live files=%u, frozen until a remount edge or a reboot)",
             why ? why : "?", still_open);
    // 进黑匣子：串口日志会随会话消失，这一条要活到下次开机。
    // 带 b = 当时还开着的 FILE* 数——它是「卡在哪一拍」的直接读数。
    // Into the blackbox: the UART capture dies with the session, this has to survive to
    // the next boot. `b` is the live FILE* count, i.e. which beat was still holding on.
    pico_bootbb_note(PICO_BOOTBB_NOTE_PUSH_HOLD, (uint8_t)why_code, (uint16_t)still_open);
}

bool push_sink_frozen(void) {
    portENTER_CRITICAL(&s_ui_lock);
    const bool frozen = (s_freeze_flags & FRZ_FROZEN) != 0;
    portEXIT_CRITICAL(&s_ui_lock);
    return frozen;
}

unsigned push_sink_live_files(void) {
    portENTER_CRITICAL(&s_ui_lock);
    const unsigned files = live_files_locked();
    portEXIT_CRITICAL(&s_ui_lock);
    return files;
}

bool push_sink_take_freeze_timeout(unsigned *frozen_ms) {
    portENTER_CRITICAL(&s_ui_lock);
    const bool had = (s_freeze_flags & FRZ_TIMEOUT) != 0;
    if (had) {
        s_freeze_flags &= (uint8_t)~FRZ_TIMEOUT;
        if (frozen_ms) *frozen_ms = s_frozen_ms; // 看门狗已把它改写成时长
    }
    portEXIT_CRITICAL(&s_ui_lock);
    return had;
}

void push_sink_init(void) {
    ensure_init();
    lock();
    if (s_sess.state == PUSH_STATE_IDLE && !s_pending_arm) try_adopt();
    unlock();
}

void push_sink_set_dict_probe(bool (*probe)(void)) { s_dict_probe = probe; }

// gen22 防串台：出厂 MAC 的后两字节就是这台设备的固定代号（PicoPush-1A2B）。
// 读不到 MAC 时退回随机数——绝不让取地址失败变成武装不了。
// gen22 anti-crosstalk: the factory MAC's last two bytes are this unit's stable tag
// (PicoPush-1A2B). If the MAC cannot be read, fall back to random - never let an
// address read failure turn into "cannot arm".
static uint16_t mac_token(void) {
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        ESP_LOGW(TAG, "mac read failed; falling back to a random token");
        return (uint16_t)(esp_random() & 0xFFFFu);
    }
    return (uint16_t)(((uint16_t)mac[4] << 8) | (uint16_t)mac[5]);
}

esp_err_t push_sink_arm(uint16_t token, uint32_t window_ms) {
    ensure_init();
    lock();
    if (s_sess.state != PUSH_STATE_IDLE || s_pending_arm) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    // 电量闸门与充电常开判定（裁定 d）：充电中放行并强制无超时。PMU 不应声
    // 不挡路（battery_ok 对读不到的快照放行），刷新失败只模糊常开判定。
    // The battery gate and charger-held decision (ruling d): charging passes and
    // forces a timeout-free window. A silent PMU never blocks (battery_ok passes
    // an unreadable snapshot); a failed refresh only blurs the held verdict.
    (void)read_pico_pmu_poll();
    if (!battery_ok()) {
        unlock();
        return ESP_ERR_NOT_ALLOWED;
    }
    s_charger_held = window_ms == 0 && charging_now();
    if (window_ms == 0 && !s_charger_held) {
        // 没在充电却要常开：软着陆到默认窗口。UI 只在充电时给这个选项，这里是
        // 组件层防御——真放行 0 出去，武装就永不到期，拔电也说不清。
        // A hold request off the charger: land softly on the default window. The
        // UI offers this only while charging; this is the glue-side guard - a
        // zero window that slips through never expires and unplug stops meaning
        // anything.
        ESP_LOGW(TAG, "hold requested off the charger, falling back to %u ms",
                 (unsigned)PUSH_WINDOW_DEFAULT_MS);
        window_ms = PUSH_WINDOW_DEFAULT_MS;
    }
    // gen22 防串台：token 改由出厂 MAC 后两字节派生（固定且全局唯一），取代随机数。
    // 广播名 = PicoPush-%04X(token)，所以名字自动变成 PicoPush-1A2B 这类固定代号，
    // 咖啡馆里两台设备不会重名、手机也不会推错人。BEGIN 帧仍校验 token 与窗口期
    // 广播名一致——Token 校验完整保留，只是值从随机变成 MAC 派生。
    // gen22 anti-crosstalk: derive the token from the factory MAC's last two bytes
    // (fixed and globally unique) instead of a random number. The adv name is
    // PicoPush-%04X(token), so it becomes a stable tag like PicoPush-1A2B: two
    // devices in a cafe can never share a name and a phone cannot push to the wrong
    // panel. BEGIN still checks the token against the window's adv name - the token
    // check is fully retained, only its value changed from random to MAC-derived.
    if (token == 0) token = mac_token();
    if (token == 0) token = 1; // 0 留给「未武装」语义 / 0 means unarmed, keep it out
    s_close_why[0] = 0; // 新回合干净开局：收摊原因不留旧话 / a fresh round starts with no stale close-up reason
    try_adopt();
    if (s_stack_up) {
        push_event_t evt;
        memset(&evt, 0, sizeof(evt));
        evt.token = token;
        evt.window_ms = window_ms;
        evt.cooldown_ms = PUSH_COOLDOWN_MS;
        evt.now_ms = ms_now();
        exec_actions(push_session_event(&s_sess, PUSH_EVT_ARM, &evt));
    } else {
        // 栈还没起：进待武装，app_loop 的 sync 会把栈拉起来，on_stack_sync 续上。
        // The stack is still down: pend the arm; app_loop's sync brings the stack
        // up and on_stack_sync completes it.
        s_pending_arm = true;
        s_pending_token = token;
        s_pending_window_ms = window_ms;
        s_pending_at_ms = ms_now();
    }
    unlock();
    return ESP_OK;
}

void push_sink_cancel(void) {
    push_sink_cancel_why("已退出接收");
}

// gen18 排雷核心：取消必须带原因。日志行 `armed canceled by X` 是覆盖表的
// 实证点——真机上拔充电线、开飞行模式、进 USB 存储各应出现一次。
// The gen18 mine-sweeping core: a cancel must carry a reason. The
// "armed canceled by X" log line is the coverage table's proof point - on
// hardware, unplugging the charger, flight mode and USB storage each show one.
void push_sink_cancel_why(const char *why) {
    ensure_init();
    lock();
    close_why_set(why);
    if (s_sess.state != PUSH_STATE_IDLE || s_pending_arm) {
        ESP_LOGI(TAG, "armed canceled by %s (state=%d pending=%d)",
                 why ? why : "?", (int)s_sess.state, (int)s_pending_arm);
    }
    if (s_pending_arm) {
        s_pending_arm = false;
        s_charger_held = false;
        ui_post_closed();
        unlock();
        return;
    }
    exec_actions(push_session_event(&s_sess, PUSH_EVT_CANCEL, NULL));
    unlock();
}

const char *push_sink_last_close_why(void) {
    return s_close_why[0] ? s_close_why : NULL;
}

bool push_sink_armed(void) {
    ensure_init();
    lock();
    const bool armed = s_sess.state != PUSH_STATE_IDLE;
    unlock();
    return armed;
}

// gen24.2：待武装查询——UI 的 R2 回退判定与「正在启动蓝牙…」渲染同源。
// Gen24.2: the pending-arm query - the UI's R2 fallback verdict and the
// "starting bluetooth..." rendering share this one source.
bool push_sink_pending(void) {
    ensure_init();
    lock();
    const bool pending = s_pending_arm;
    unlock();
    return pending;
}

push_state_t push_sink_state(void) {
    ensure_init();
    lock();
    const push_state_t st = s_sess.state;
    unlock();
    return st;
}

// gen22：取走一次手机下发的 UTC 真值（一次性）。main 层拿到后登记漂移样本并同步 PMU。
// gen22: take one phone-supplied UTC truth (one-shot). Main records the drift sample
// and syncs the PMU with it.
bool push_sink_take_time_sync(uint32_t *utc) {
    if (!utc) return false;
    ensure_init();
    lock();
    bool taken = s_remote_utc_pending;
    if (taken) {
        *utc = s_remote_utc;
        s_remote_utc_pending = false;
    }
    unlock();
    return taken;
}

const char *push_sink_adv_name(void) {
    ensure_init();
    // 展示用途：不加锁直接交指针。名字在武装后写一次（adv_start）、收官时清空
    // （整句置 0），读者最坏看到旧名或新旧 token 混合的几个字符——纯展示，
    // 不影响协议（手机按收到的广播包解析，不走这块内存）。
    // Display use: hand out the pointer without the lock. The name is written
    // once after arming and cleared wholesale on wrap-up; a reader sees the old
    // name or a few mixed old/new token characters at worst - display only, the
    // protocol never reads this memory (the phone parses the advertised packet).
    return s_adv_name;
}

uint32_t push_sink_window_left_ms(void) {
    ensure_init();
    lock();
    const uint32_t left = push_session_window_left_ms(&s_sess, ms_now());
    unlock();
    return left;
}

void push_sink_progress(uint32_t *received, uint32_t *total) {
    ensure_init();
    lock();
    if (received) *received = s_sess.received;
    if (total) *total = s_sess.begin.total_len;
    unlock();
}

void push_sink_poll(void) {
    ensure_init();
    const uint32_t now = ms_now();

    // 31b：冻结看门狗。三拍区间正常 ≤ 9 s（3 × (2 s quiesce + 1 s 间隔)）+ 卸卷余量；
    // 超过 20 s 说明终态枚举漏了项，或者流程卡死。31a 五环死锁的教训就是：没有保底，
    // 症状是「只有重启能出」。毒卷保持态（s_freeze_hold）是**有意**的冻结，放过。
    // The freeze watchdog. 20 s means a missing terminal branch or a stuck flow.
    // A poisoned-volume hold is deliberate and is passed over.
    portENTER_CRITICAL(&s_ui_lock);
    const bool freeze_expired = (s_freeze_flags & FRZ_FROZEN) != 0 &&
                                (s_freeze_flags & FRZ_HOLD) == 0 &&
                                (now - s_frozen_ms >= PUSH_FREEZE_WATCHDOG_MS);
    uint32_t forced_ms = 0;
    if (freeze_expired) {
        forced_ms = now - s_frozen_ms;
        s_frozen_ms = forced_ms;          // 复用：此刻它存的是时长，供 take 取走
        s_freeze_flags = FRZ_TIMEOUT;     // 解冻 + 置一次性事件（HOLD 一并清掉）
    }
    portEXIT_CRITICAL(&s_ui_lock);
    if (freeze_expired) {
        ESP_LOGW(TAG, "freeze watchdog: forced thaw after %u ms (no terminal state reached)",
                 (unsigned)forced_ms);
    }

    lock();
    // 待武装超时：栈起不来就放弃，UI 收 start_failed。/ Pending-arm timeout: give
    // up when the stack never comes up and let the UI take start_failed.
    if (s_pending_arm && now - s_pending_at_ms >= PUSH_PENDING_ARM_TIMEOUT_MS) {
        s_pending_arm = false;
        s_charger_held = false;
        // gen24.2（D3 同源）：pending 超时必须留日志行——真机据此分辨「点了
        // 没反应」是 pending 超时还是别的原因；此前该路径静默入队，事件又
        // 烂死在无人消费的方法页，屏幕零变化（gen24.2 问题 1/2 现场）。
        // Gen24.2 (same source as D3): a pending-arm timeout must log - on
        // hardware this line tells "dead tap" apart from other causes; the
        // path used to enqueue silently into a method page nobody consumed,
        // leaving the screen frozen (the gen24.2 problem 1/2 field).
        ESP_LOGW(TAG, "pending arm timed out after %u ms, stack never came up",
                 (unsigned)PUSH_PENDING_ARM_TIMEOUT_MS);
        ui_post_start_failed();
    }
    if (s_sess.state != PUSH_STATE_IDLE) {
        push_event_t evt;
        memset(&evt, 0, sizeof(evt));
        evt.now_ms = now;
        exec_actions(push_session_event(&s_sess, PUSH_EVT_TICK, &evt));
    }
    // 充电常开：拔电即退（裁定 d）。传输中（RECEIVING/PROCESSING）不打断，
    // 推完进 COOLDOWN/WINDOW 后的下一轮 poll 自然收摊。
    // Charger-held: unplug exits outright (ruling d). A transfer in flight is not
    // interrupted; the next poll after COOLDOWN/WINDOW closes up.
    if (s_charger_held && now - s_pmu_refresh_ms >= PUSH_PMU_REFRESH_MS) {
        s_pmu_refresh_ms = now;
        (void)read_pico_pmu_poll();
        if (!charging_now()) {
            s_charger_held = false;
            // 原因在判定拔电时就设上：UNPLUG 事件与「宽限到期收官」两条路
            // 都靠它给 UI 一句人话。/ The reason is set the moment the unplug
            // is detected: both the UNPLUG event and the grace-expiry wrap-up
            // rely on it for a human sentence to the UI.
            close_why_set("充电线已拔出，接收结束");
            if (s_sess.state == PUSH_STATE_WINDOW || s_sess.state == PUSH_STATE_CONNECTED ||
                s_sess.state == PUSH_STATE_COOLDOWN) {
                exec_actions(push_session_event(&s_sess, PUSH_EVT_UNPLUG, NULL));
            }
        }
    }
    unlock();
}

bool push_sink_take_open(char *title, size_t cap, uint8_t *type) {
    ensure_init();
    bool got = false;
    portENTER_CRITICAL(&s_ui_lock);
    if (s_ui.open) {
        got = true;
        s_ui.open = false;
        if (title && cap) snprintf(title, cap, "%s", s_ui.title);
        if (type) *type = s_ui.type;
    }
    portEXIT_CRITICAL(&s_ui_lock);
    return got;
}

bool push_sink_take_committed(char *title, size_t cap, uint8_t *type) {
    ensure_init();
    bool got = false;
    portENTER_CRITICAL(&s_ui_lock);
    if (s_ui.committed) {
        got = true;
        s_ui.committed = false;
        if (title && cap) snprintf(title, cap, "%s", s_ui.committed_title);
        if (type) *type = s_ui.committed_type;
    }
    portEXIT_CRITICAL(&s_ui_lock);
    return got;
}

bool push_sink_take_word_card(char *word, size_t cap) {
    ensure_init();
    bool got = false;
    portENTER_CRITICAL(&s_ui_lock);
    if (s_ui.word_card) {
        got = true;
        s_ui.word_card = false;
        if (word && cap) snprintf(word, cap, "%s", s_ui.word);
    }
    portEXIT_CRITICAL(&s_ui_lock);
    return got;
}

bool push_sink_take_err(int *code) {
    ensure_init();
    bool got = false;
    portENTER_CRITICAL(&s_ui_lock);
    if (s_ui.err != 0) {
        got = true;
        if (code) *code = s_ui.err;
        s_ui.err = 0;
    }
    portEXIT_CRITICAL(&s_ui_lock);
    return got;
}

bool push_sink_take_closed(void) {
    ensure_init();
    bool got = false;
    portENTER_CRITICAL(&s_ui_lock);
    if (s_ui.closed) {
        got = true;
        s_ui.closed = false;
    }
    portEXIT_CRITICAL(&s_ui_lock);
    return got;
}

bool push_sink_take_start_failed(void) {
    ensure_init();
    bool got = false;
    portENTER_CRITICAL(&s_ui_lock);
    if (s_ui.start_failed) {
        got = true;
        s_ui.start_failed = false;
    }
    portEXIT_CRITICAL(&s_ui_lock);
    return got;
}

void push_sink_notify_read(void) {
    ensure_init();
    lock();
    uint8_t frame[PUSH_STATUS_MAX];
    const size_t n = push_status_encode(PUSH_ST_EVT_READ, NULL, 0, frame, sizeof(frame));
    if (n) notify_status(frame, n);
    unlock();
}

void push_sink_notify_battery(void) {
    ensure_init();
    lock();
    const pmu_snapshot_t *p = read_pico_pmu_get();
    int percent = p && p->present ? pmu_battery_percent(p) : -1;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    const uint8_t body[1] = {(uint8_t)percent};
    uint8_t frame[PUSH_STATUS_MAX];
    const size_t n = push_status_encode(PUSH_ST_BATT, body, 1, frame, sizeof(frame));
    if (n) notify_status(frame, n);
    unlock();
}

/* ---- gen26.5 TTS 听书（STATUS 0x19 页文本）---- */
/* gen26.5 TTS read-aloud (STATUS 0x19 page text). */

bool push_sink_link_up(void) {
    ensure_init();
    // 链路可用 = 有连接 && 对端已订阅 STATUS（未订阅时 notify 全丢，发了也白发）。
    // Link up = connected AND the peer subscribed to STATUS (notifying an
    // unsubscribed peer just burns mbufs).
    lock();
    const bool up = s_conn_handle != BLE_HS_CONN_HANDLE_NONE && s_status_subscribed;
    unlock();
    return up;
}

bool push_sink_tts_send_page(const char *text, size_t len) {
    ensure_init();
    if (!text && len) return false;
    // gen26.6.2：空页帧（len=0 = 「静默/停止朗读」）留一条串口日志。关听书开关（D2A）
    // 与重连补发（D2B）这两条停止路径此前**零日志**，真机只能靠听感、审计拿不到证据——
    // 停止是用户可感知的行为，必须有可查落点。（本轮因热区/离线缓存修复必须重编，
    // 打点零成本搭车；此前为它单独重编不划算故未做。）
    // gen26.6.2: log the empty-page frame (len=0 = silence / stop). The switch-off
    // (D2A) and reconnect-resend (D2B) stop paths logged nothing before, leaving only
    // ear-witness and no audit evidence - stopping is user-visible and needs a trace.
    // (This round rebuilds anyway for the hot zone / offline cache, so the log rides
    // along for free; a rebuild just for it was not worth it.)
    if (len == 0) {
        ESP_LOGI(TAG, "tts page stop: empty frame (peer silence; switch off or resend)");
    }
    lock();
    const size_t total = push_tts_frame_count(text, len);
    bool ok = true;
    for (size_t i = 0; i < total && ok; ++i) {
        const char *slice = NULL;
        size_t slice_len = 0;
        if (!push_tts_frame_at(text, len, i, &slice, &slice_len)) {
            ok = false; // 计划器自相矛盾：防御性失败，调用方下轮重发
            break;
        }
        push_tts_text_t t;
        memset(&t, 0, sizeof(t));
        t.seq = (uint16_t)i;
        t.total = (uint16_t)total;
        t.len = (uint16_t)slice_len;
        memcpy(t.text, slice, slice_len); // 切分器保证 slice_len ≤ 233 < 槽
        t.text[slice_len] = 0;
        uint8_t frame[PUSH_ST_TTS_TEXT_FRAME_MAX];
        const size_t n = push_st_tts_text_encode(&t, frame, sizeof(frame));
        if (!n || !notify_status(frame, n)) {
            // 任一帧未交付即整页失败：不投「缺帧的页」——对端重组器靠
            // seq/total 对账，缺帧页本就该整页作废；下轮从头（seq==0）重发，
            // 对端在 seq==0 处自然重置，无半页残留。/ Any undelivered frame fails
            // the whole page: never air a gapped page - the peer's reassembler
            // reconciles by seq/total and a gapped page must be dropped whole;
            // the next round resends from seq==0, where the peer resets anyway.
            ESP_LOGW(TAG, "tts page frame %u/%u not delivered", (unsigned)i, (unsigned)total);
            ok = false;
        }
    }
    unlock();
    return ok;
}

/* ---- gen30 伴读：STATUS 方向两帧 + CTRL 方向 take 槽 + conn 档位 ---------- */
/* Read-along gen30: two STATUS frames, two CTRL take slots, connection tier. */
/*
 * 本段只提供「设备端协议能力」：发帧、收帧对账、落 take 槽、连接档位。调用时机
 * （进/出伴读动作、翻页流、模式切换、TTS_DONE 消费）属产品码接线，步 3 熔断
 * 简报另案报裁，本段不预设、不反向调用 main。
 * This section supplies device-side protocol capability only. The wiring timing
 * (enter/exit, page stream, mode switch, receipt consumption) is product-code
 * glue, reported separately in the step-3 circuit-breaker brief.
 */

bool push_sink_companion_page_begin(uint16_t page_no, uint8_t mode) {
    ensure_init();
    // op1 + page_no2(LE) + mode1 + reserved1 = 5 B
    const uint8_t body[4] = {(uint8_t)page_no, (uint8_t)(page_no >> 8), mode, 0u};
    uint8_t frame[PUSH_STATUS_MAX];
    const size_t n = push_status_encode(PUSH_ST_PAGE_BEGIN, body, sizeof(body),
                                        frame, sizeof(frame));
    if (!n) return false;
    // 对账基准先落再投递：帧序即页序，回包到达时基准必已就位；同时清掉上一页
    // 未取的回包，防旧页 SUMMARY/TTS_DONE 在新页被取走。
    portENTER_CRITICAL(&s_ui_lock);
    s_companion_page_no = page_no;
    s_companion_page_valid = true;
    s_companion_summary_pending = false;
    s_companion_tts_pending = false;
    /* 换页即弃半段：多帧摘要不跨页拼装，旧页残留片绝不能接进新页。 */
    s_summary_acc_len = 0;
    s_summary_acc_next = 0;
    portEXIT_CRITICAL(&s_ui_lock);
    return notify_status(frame, n);
}

bool push_sink_companion_mode_set(uint8_t mode, uint8_t source) {
    ensure_init();
    // op1 + mode1 + source1 + reserved2 = 5 B
    const uint8_t body[4] = {mode, source, 0u, 0u};
    uint8_t frame[PUSH_STATUS_MAX];
    const size_t n = push_status_encode(PUSH_ST_MODE_SET, body, sizeof(body),
                                        frame, sizeof(frame));
    if (!n) return false;
    return notify_status(frame, n);
}

bool push_sink_tts_enable_send(bool on) {
    ensure_init();
    // op1 + enable1 = 2 B。修复轮解耦：把「听书开关」作为独立事实发给手机，
    // 0x19 从此只承载文本，手机端「念/不念」由本帧决定，摘要链与朗读彻底解耦。
    // Repair-round decoupling: the listening switch travels as its own fact, so
    // 0x19 carries text only and playback is decided by this bit; the summary
    // chain is fully independent of read-aloud.
    const uint8_t body[1] = {on ? 1u : 0u};
    uint8_t frame[PUSH_STATUS_MAX];
    const size_t n = push_status_encode(PUSH_ST_TTS_ENABLE, body, sizeof(body),
                                        frame, sizeof(frame));
    if (!n) return false;
    const bool sent = notify_status(frame, n);
    /* 现场判读用：开关到底把哪个方向发下去了，一行看清。上一轮没有这条，
       只能靠手感判「接反」，白跑一轮。 */
    ESP_LOGI(TAG, "tts_enable: on=%d sent=%d", on ? 1 : 0, sent ? 1 : 0);
    return sent;
}

bool push_sink_take_companion_summary(uint8_t *mode, uint16_t *page_no,
                                      uint8_t *idx, uint8_t *total,
                                      char *text, size_t cap) {
    ensure_init();
    bool got = false;
    portENTER_CRITICAL(&s_ui_lock);
    if (s_companion_summary_pending) {
        s_companion_summary_pending = false;
        got = true;
        if (mode) *mode = s_companion_summary_mode;
        if (page_no) *page_no = s_companion_summary_page;
        if (idx) *idx = s_companion_summary_idx;
        if (total) *total = s_companion_summary_total;
        if (text && cap) {
            const size_t n = s_companion_summary_len < cap - 1u
                                 ? s_companion_summary_len : cap - 1u;
            memcpy(text, s_companion_summary_text, n);
            text[n] = 0;
        }
    }
    portEXIT_CRITICAL(&s_ui_lock);
    return got;
}

bool push_sink_take_companion_tts_done(uint16_t *page_no, uint8_t *status) {
    ensure_init();
    bool got = false;
    portENTER_CRITICAL(&s_ui_lock);
    if (s_companion_tts_pending) {
        s_companion_tts_pending = false;
        got = true;
        if (page_no) *page_no = s_companion_tts_page;
        if (status) *status = s_companion_tts_status;
    }
    portEXIT_CRITICAL(&s_ui_lock);
    return got;
}

void push_sink_companion_conn(bool on) {
    ensure_init();
    portENTER_CRITICAL(&s_ui_lock);
    s_companion_conn_fair = on;
    portEXIT_CRITICAL(&s_ui_lock);
    conn_params_apply(on ? PUSH_CONN_ITVL_FAIR : PUSH_CONN_ITVL_SLOW,
                      on ? PUSH_CONN_LATENCY_FAST : PUSH_CONN_LATENCY_SLOW);
}

bool push_sink_ai_scene_send(const char *word, const char *ctx, size_t ctx_len,
                             uint16_t word_off, uint8_t flags) {
    if (!word || !*word) return false;
    if (!ctx || ctx_len == 0 || ctx_len > PUSH_SCENE_CTX_MAX) return false;
    if (word_off > ctx_len) return false;
    // 三道门，缺一不发：AI 总开关 / 情景开关 / APK 已声明 CAP_AI_SCENE。
    // 老 APK 不发 0x09 → 位不置 → 设备零 0x1A，零感知兼容（与 0x19 同一模型）。
    // Three gates, all required: AI switch, scene switch, and the APK's
    // CAP_AI_SCENE declaration. An old APK never sends 0x09, so the bit stays
    // clear and the device emits no 0x1A - silent compatibility (same model as 0x19).
    if (!s_ai_enabled || !s_scene_enabled) return false;
    // bit3 = APK 认识 0x1A；bit4 = APK 真能出四节分析。两个都要：只认 bit3 的
    // v1.6.1 会「收下请求、永不回答」，用户看到的是卡片一直转到超时。
    // bit3 = the APK understands 0x1A; bit4 = it can actually answer with four
    // sections. Both are required: v1.6.1 declares bit3 only and would take the
    // request without ever answering - a card spinning until timeout.
    if (!(s_apk_caps & PUSH_CAP_AI_SCENE)) return false;
    if (!(s_apk_caps & PUSH_CAP_AI_SCENE_VIEW)) return false;

    lock();
    const bool live = s_conn_handle != BLE_HS_CONN_HANDLE_NONE && s_status_subscribed;
    if (!live) {
        unlock();
        return false;
    }
    const uint16_t total = push_scene_frame_count(word, ctx, ctx_len);
    bool ok = true;
    for (uint16_t i = 0; i < total && ok; ++i) {
        const char *slice = NULL;
        size_t slice_len = 0;
        if (!push_scene_frame_at(word, ctx, ctx_len, i, &slice, &slice_len)) {
            ok = false; // 计划器自相矛盾（防御性失败）
            break;
        }
        uint8_t frame[PUSH_ST_SCENE_FRAME_MAX];
        const size_t n = push_st_scene_encode(word, word_off, flags, i, total,
                                              slice, slice_len, frame, sizeof(frame));
        if (!n || !notify_status(frame, n)) {
            // 与 0x19 同口径：不投缺帧请求，对端在 seq==0 重置。
            // Same stance as 0x19: never air a gapped request; the peer resets at seq==0.
            ESP_LOGW(TAG, "scene frame %u/%u not delivered", (unsigned)i, (unsigned)total);
            ok = false;
        }
    }
    unlock();
    if (ok) {
        // 真机 smoke 的装锚：帧数、词长、偏移、标志位一次可查。
        // On-device anchor: frames, word length, offset and flags in one line.
        ESP_LOGI(TAG, "scene send: word=%u bytes off=%u frames=%u flags=0x%02x",
                 (unsigned)strnlen(word, PUSH_SCENE_WORD_MAX), (unsigned)word_off,
                 (unsigned)total, (unsigned)flags);
    }
    return ok;
}
