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

#include "push_sink.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_uuid.h"
#include "os/os_mbuf.h"

#include "read_pico_pmu.h"

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

static FILE *s_part;                  // 打开中的 .part（append 模式）/ the open partial (append mode)
static uint32_t s_last_prog_ms;       // PROGRESS 节流时钟 / the PROGRESS throttle clock
static uint32_t s_pkts_since_ckpt;    // 距上次 NVS 检查点的包数 / packets since the last NVS checkpoint
static uint32_t s_pmu_refresh_ms;     // 上次 PMU 刷新时刻 / last PMU refresh time

static uint8_t s_last_status[PUSH_STATUS_MAX]; // 最近一条 STATUS（读特征的回退通道）
static size_t s_last_status_len;               // the last STATUS (the read fallback)

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
} s_ui;

static bool (*s_dict_probe)(void); // 词典可用性探针 / the dictionary-availability probe

/* ---- 小助手 / Small helpers ---- */

// 定义在后的静态函数：adv_start 与 GATT 表要提前引用。/ Static functions defined
// later: adv_start and the GATT table reference them ahead of their definitions.
static void exec_actions(uint32_t act);
static int push_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg);
static int push_gap_cb(struct ble_gap_event *event, void *arg);

static uint32_t ms_now(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static void ensure_init(void) {
    if (s_inited) return;
    s_lock = xSemaphoreCreateMutex();
    push_session_init(&s_sess);
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

/* ---- .part 文件 / The partial file ---- */

static void part_close(void) {
    if (s_part) {
        (void)fflush(s_part);
        (void)fclose(s_part);
        s_part = NULL;
    }
}

// 追加模式打开：不存在则创建，存在则接着写（续传靠这个）。
// Open in append mode: create when missing, continue when present (resume relies on it).
static bool part_open_append(void) {
    if (s_part) return true;
    if (mkdir(PUSH_DIR, 0777) && errno != EEXIST) return false;
    s_part = fopen(PUSH_PART_PATH, "ab");
    return s_part != NULL;
}

// 截断新建：一次全新会话的起点。/ Truncating open: the start of a brand-new session.
static bool part_open_fresh(void) {
    part_close();
    if (mkdir(PUSH_DIR, 0777) && errno != EEXIST) return false;
    s_part = fopen(PUSH_PART_PATH, "wb");
    return s_part != NULL;
}

static bool part_append(const uint8_t *data, size_t len) {
    if (!part_open_append()) return false;
    if (len == 0) return true;
    const size_t n = fwrite(data, 1, len, s_part);
    (void)fflush(s_part); // 逐包落盘：崩溃后 .part 不会比 NVS 记账多出未落地的字节
                          // Flush per packet: after a crash the file never runs ahead of NVS
    return n == len;
}

static long part_size(void) {
    struct stat st;
    if (stat(PUSH_PART_PATH, &st) != 0) return -1;
    return (long)st.st_size;
}

static void part_discard(void) {
    part_close();
    (void)remove(PUSH_PART_PATH);
}

/* ---- STATUS 通知与应答 / STATUS notify and replies ---- */

static void status_remember(const uint8_t *frame, size_t n) {
    if (n > PUSH_STATUS_MAX) n = PUSH_STATUS_MAX;
    memcpy(s_last_status, frame, n);
    s_last_status_len = n;
}

// 乐观通知：NimBLE 的 notify 不检查 CCCD（ble_gatts_notify_custom 直接读属性发
// 通知），订阅态只用来记日志。没订阅的手机按规范应忽略通知；只读的客户端走
// 读 PUSH_STATUS 的回退通道（on_access 里回 s_last_status）。
// Optimistic notify: NimBLE does not check the CCCD when notifying; the
// subscription flag is for logging only. A phone that never subscribed should
// ignore notifications per spec, and read-only clients use the read fallback on
// PUSH_STATUS, which serves s_last_status.
static void notify_status(const uint8_t *frame, size_t n) {
    status_remember(frame, n);
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    if (!s_status_subscribed) {
        ESP_LOGD(TAG, "status 0x%02x dropped: peer not subscribed", frame[0]);
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(frame, (uint16_t)n);
    if (!om) return;
    const int rc = ble_gatts_notify_custom(s_conn_handle, s_status_handle, om);
    if (rc != 0) ESP_LOGD(TAG, "notify 0x%02x rc=%d", frame[0], rc);
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
static void adv_start(void) {
    snprintf(s_adv_name, sizeof(s_adv_name), "%s%04X", PUSH_ADV_NAME_PREFIX, s_sess.token);
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)s_adv_name;
    fields.name_len = (uint8_t)strlen(s_adv_name);
    fields.name_is_complete = 1;
    // 广播数据里带服务 UUID：nRF Connect 在扫描列表就能看到 e2f5a1c0…，
    // 连上之前先确认对面就是推流服务（gen12 与常驻注册一并补上）。
    // Advertise the service UUID: nRF Connect shows e2f5a1c0... in the scan
    // list, confirming this is the push service even before connecting
    // (gen12, alongside the resident registration).
    fields.uuids128 = &s_svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    if (ble_gap_adv_set_fields(&fields) != 0) {
        ESP_LOGW(TAG, "adv set fields failed");
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
    const int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params,
                                     push_gap_cb, NULL);
    if (rc != 0) {
        // 广播起不来（控制器被占等）：取消本次武装，UI 收 start_failed。
        // Advertising failed to start (controller busy): cancel the arm and let
        // the UI take start_failed.
        ESP_LOGW(TAG, "adv start rc=%d", rc);
        exec_actions(push_session_event(&s_sess, PUSH_EVT_CANCEL, NULL));
        return;
    }
    ESP_LOGI(TAG, "advertising as %s", s_adv_name);
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

static void ui_post_closed(void) {
    portENTER_CRITICAL(&s_ui_lock);
    s_ui.closed = true;
    portEXIT_CRITICAL(&s_ui_lock);
}

static void ui_post_start_failed(void) {
    portENTER_CRITICAL(&s_ui_lock);
    s_ui.start_failed = true;
    portEXIT_CRITICAL(&s_ui_lock);
}

// VERIFY：回读 .part 分段算 CRC，与 BEGIN 的 crc32/length 对账后喂 VERIFY 事件。
// VERIFY: read the partial back in chunks, reconcile against the BEGIN crc32 and
// length, then feed the VERIFY event.
static void do_verify(void) {
    // s_part 为空说明簿记与文件脱节：直接判负（VERIFY 失败回 CONNECTED 保留
    // .part，行为安全）。fflush(NULL) 是 UB，绝不能让指针裸奔。
    // A NULL partial means the bookkeeping lost the file: fail the verdict
    // (which returns to CONNECTED and keeps the partial - safe). fflush(NULL)
    // is UB; never let a bare pointer run.
    bool ok = s_part != NULL && fflush(s_part) == 0;
    const long sz = part_size();
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
            ok = read_ok && crc == s_sess.begin.crc32;
        }
    } else if (ok) {
        ok = false; // 长度不等：保留 .part 等续传 / length mismatch: keep the partial for resume
    }
    // 判定前落一盘 NVS：此刻崩溃，重连后 RESUME_也能续上。/ Checkpoint before the
    // verdict: a crash here still resumes after reconnect.
    nvs_save_session();
    push_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.verify_ok = ok;
    exec_actions(push_session_event(&s_sess, PUSH_EVT_VERIFY, &evt));
}

// COMMIT：正文原子改名成 push.txt；单词卡不落盘（title 即单词，UI 直接查词典）。
// COMMIT: atomically rename the body to push.txt; word cards never land on disk
// (the title is the word and the UI looks it up directly).
static void do_commit(void) {
    part_close();
    bool ok = true;
    if (s_sess.begin.type == PUSH_TYPE_WORD) {
        (void)remove(PUSH_PART_PATH);
        // 词典中途消失：不出空卡，回 ERR(9)（裁定 b：不做降级渲染）。
        // The dictionary vanished mid-push: no empty card, ERR(9) instead
        // (ruling b: no degraded rendering).
        if (!dict_available()) {
            nvs_clear_session();
            reply_err(PUSH_ERR_NO_DICT);
            return;
        }
    } else {
        ok = rename(PUSH_PART_PATH, PUSH_TEXT_PATH) == 0;
        if (!ok) ESP_LOGE(TAG, "commit rename: %s", strerror(errno));
    }
    nvs_clear_session();
    if (!ok) {
        // 改名失败（卡满/掉卡）：.part 留着可人工恢复，回 ERR(5)。
        // Rename failed (full or missing card): keep the partial for manual
        // recovery and reply ERR(5).
        reply_err(PUSH_ERR_NO_CARD);
        return;
    }
    conn_params_apply(PUSH_CONN_ITVL_SLOW, PUSH_CONN_LATENCY_SLOW);
    reply_done();
}

static void exec_actions(uint32_t act) {
    if (act & PUSH_ACT_DROP) {
        part_discard();
        nvs_clear_session();
        s_pkts_since_ckpt = 0;
    }
    if (act & PUSH_ACT_READY) {
        if (!part_open_fresh()) {
            // .part 建不起来：取消会话回 ERR(5)，状态机不留半吊子簿记。
            // The partial cannot be created: cancel the session with ERR(5) so
            // the machine keeps no half-born bookkeeping.
            exec_actions(push_session_event(&s_sess, PUSH_EVT_CANCEL, NULL));
            reply_err(PUSH_ERR_NO_CARD);
            return;
        }
        s_pkts_since_ckpt = 0;
        nvs_save_session();
        conn_params_apply(PUSH_CONN_ITVL_FAIR, PUSH_CONN_LATENCY_FAST);
        reply_ready();
    }
    if (act & PUSH_ACT_RESUME) reply_resume();
    if (act & PUSH_ACT_APPEND) {
        if (!part_append(s_rx_payload, s_rx_len)) {
            // 写盘失败：簿记已经前进，唯独取消能让 offset 重新诚实。
            // The write failed: the counters already moved, and only a cancel
            // makes the offset honest again.
            exec_actions(push_session_event(&s_sess, PUSH_EVT_CANCEL, NULL));
            reply_err(PUSH_ERR_NO_CARD);
            return;
        }
        if (++s_pkts_since_ckpt >= PUSH_NVS_CHECKPOINT_EVERY) {
            s_pkts_since_ckpt = 0;
            nvs_save_session();
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
    if (act & PUSH_ACT_COMMIT) do_commit();
    if (act & PUSH_ACT_ERR) reply_err(s_sess.last_err);
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

/* ---- CTRL / DATA 写入处理 / CTRL and DATA write handling ---- */

// BEGIN 的胶水前置闸门：词典（单词卡）、电量、.part 可建。都在喂状态机之前，
// 拒绝时状态机原地不动。/ Glue-side pre-checks for BEGIN: dictionary (word cards),
// battery and a creatable partial - all before the machine sees the frame, so a
// refusal leaves the machine untouched.
static int begin_precheck(const push_begin_t *b) {
    if (b->type == PUSH_TYPE_WORD && !dict_available()) return PUSH_ERR_NO_DICT;
    if (!battery_ok()) return PUSH_ERR_LOW_BATTERY;
    if (!part_open_append()) return PUSH_ERR_NO_CARD;
    return PUSH_ERR_NONE;
}

static void on_ctrl_write(const uint8_t *data, size_t len) {
    uint8_t op;
    if (!push_ctrl_header_decode(data, len, &op)) {
        reply_err(PUSH_ERR_BAD_FRAME);
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
            reply_err(PUSH_ERR_BAD_FRAME);
            return;
        }
        const int err = begin_precheck(&b);
        if (err != PUSH_ERR_NONE) {
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
        reply_err(PUSH_ERR_BAD_FRAME);
        return;
    }
}

static void on_data_write(const uint8_t *data, size_t len) {
    if (s_sess.state == PUSH_STATE_IDLE) {
        reply_err(PUSH_ERR_NOT_ARMED);
        return;
    }
    // 只在 RECEIVING 收正文；其余态说明手机不按牌理出牌，明确拒绝而不是装死。
    // Body packets are taken only while RECEIVING; any other state means the
    // phone is out of order, so refuse explicitly instead of ignoring it.
    if (s_sess.state != PUSH_STATE_RECEIVING) {
        reply_err(PUSH_ERR_BAD_FRAME);
        return;
    }
    uint16_t seq;
    const uint8_t *payload;
    size_t n;
    if (!push_data_decode(data, len, &seq, &payload, &n)) {
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
        ESP_LOGI(TAG, "phone connected");
        exec_actions(push_session_event(&s_sess, PUSH_EVT_CONNECT, NULL));
        unlock();
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected: %d", event->disconnect.reason);
        lock();
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_status_subscribed = false;
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
            ESP_LOGD(TAG, "notify tx status %d", event->notify_tx.status);
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
    push_nvs_session_t nv;
    if (!nvs_load_session(&nv)) {
        part_discard(); // 孤儿 .part：没有会话就不留 / an orphan partial: no session, no file
        return;
    }
    const long sz = part_size();
    if (sz < 0) return; // 卡不在：簿记原样留着 / no card: the bookkeeping stays as is
    if ((uint32_t)sz < nv.received) {
        nvs_clear_session();
        part_discard();
        return;
    }
    if ((uint32_t)sz > nv.received && truncate(PUSH_PART_PATH, (off_t)nv.received) != 0) {
        nvs_clear_session();
        part_discard();
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

void push_sink_init(void) {
    ensure_init();
    lock();
    if (s_sess.state == PUSH_STATE_IDLE && !s_pending_arm) try_adopt();
    unlock();
}

void push_sink_set_dict_probe(bool (*probe)(void)) { s_dict_probe = probe; }

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
    if (token == 0) token = (uint16_t)(esp_random() & 0xFFFFu);
    if (token == 0) token = 1; // 0 留给「未武装」语义 / 0 means unarmed, keep it out
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
    ensure_init();
    lock();
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

bool push_sink_armed(void) {
    ensure_init();
    lock();
    const bool armed = s_sess.state != PUSH_STATE_IDLE;
    unlock();
    return armed;
}

push_state_t push_sink_state(void) {
    ensure_init();
    lock();
    const push_state_t st = s_sess.state;
    unlock();
    return st;
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
    lock();
    // 待武装超时：栈起不来就放弃，UI 收 start_failed。/ Pending-arm timeout: give
    // up when the stack never comes up and let the UI take start_failed.
    if (s_pending_arm && now - s_pending_at_ms >= PUSH_PENDING_ARM_TIMEOUT_MS) {
        s_pending_arm = false;
        s_charger_held = false;
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
