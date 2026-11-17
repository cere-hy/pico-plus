/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：push_sink 的胶水层公共 API。纯核（push_proto/push_session）不含任何外设
 * 依赖；本头文件面向的调用方只有两处：ble_page_turner（栈生命周期与射频互斥的
 * 钩子）与 main（武装入口、主循环轮询、UI 一次性事件消费）。
 * English: the public API of the push_sink glue. The pure core
 * (push_proto/push_session) touches no hardware; this header serves exactly two
 * kinds of callers: ble_page_turner (stack-lifecycle and radio-mutex hooks) and
 * main (the arm entry, the main-loop pump and the one-shot UI events).
 *
 * 冻结：GATT 服务与广播名格式随设计草案 §3.1/§6.5；改 UUID 或字段先改草案。
 * Frozen: the GATT service and adv-name format follow design draft sections
 * 3.1/6.5; document any change before touching the code.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "push_proto.h"
#include "push_session.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 常量 / Constants ---- */

/// 武装期广播名前缀，后缀是 4 位十六进制 token（鉴权要素，见草案 §6.5）。
/// Advertising-name prefix while armed; the suffix is the 4-hex-digit token
/// (the auth element, draft section 6.5).
#define PUSH_ADV_NAME_PREFIX "PicoPush-"

/// .part 暂存与成品路径：TF 卡隐藏目录，设备自动创建（用户可见文档只说到
/// `.readpico` 这一层，路径细节仅作注解）。
/// Staging and product paths in the card's hidden directory, created by the
/// device (user-facing docs mention only the .readpico level).
#define PUSH_DIR "/sdcard/.readpico"
#define PUSH_PART_PATH PUSH_DIR "/push.part"
#define PUSH_TEXT_PATH PUSH_DIR "/push.txt"
/// 旧书的暂存名：提交时先 rename 到这里，新书就位成功才删。成品路径永不 remove
/// （见 26.9.30 不变式），所以"旧书消失"只可能发生在 .bak 上的两种岔路，
/// 而 .bak 还能被下一次提交或开机捡回。
/// The previous book parks here during commit; it is removed only after the new
/// one lands. The product path is never removed, so an old book can only be
/// missing while it sits in .bak - and .bak is recoverable.
#define PUSH_BAK_PATH PUSH_DIR "/push.bak"

/// 待武装超时：点了接收但栈迟迟起不来（内存闸门/失败退避）就放弃，
/// 别让界面干等。/ Pending-arm timeout: give up when the stack never comes up
/// (memory gate or back-off) instead of leaving the UI spinning.
#define PUSH_PENDING_ARM_TIMEOUT_MS 10000u

/* ---- 生命周期 / Lifecycle ---- */

/// 初始化（建互斥锁、清会话）。app_main 启动时调一次，之后所有入口都可重入。
/// Initialise (mutex, session). Called once from app_main; every entry is
/// re-entrant afterwards.
void push_sink_init(void);

/// 注册词典可用性探针（main 用 dict_ready() 注册；不注册则视为可用，
/// 便于组件独立工作与宿主测试）。/ Register the dictionary-availability probe
/// (main passes dict_ready(); unregistered means "available").
void push_sink_set_dict_probe(bool (*probe)(void));

/* ---- 武装 / Arming ---- */

/// 用户点「接收推送」。token 传 0 由本模块随机生成（读 push_sink_adv_name()
/// 取回展示）；window_ms 传 0 表示充电时常开（无超时、拔电即退，裁定 d）。
/// 栈还没起来时进入「待武装」，栈就绪后由 on_stack_sync 自动续上。
/// The user pressed receive. token == 0 generates a random one (read it back
/// with push_sink_adv_name()); window_ms == 0 means charger-held (no timeout,
/// unplug exits, ruling d). With the stack still down this becomes a pending
/// arm that on_stack_sync completes.
esp_err_t push_sink_arm(uint16_t token, uint32_t window_ms);

/// 用户取消（UI 上的退出按钮）。/ The user cancelled (the UI's exit button).
void push_sink_cancel(void);

/// 带原因取消（gen18 排雷）：任何武装期 BLE 拆除都必须留痕——
/// 「armed canceled by X」日志 + 原因可被 UI 取回展示，静默失败零容忍。
/// Cancel with a reason (gen18 mine-sweeping): every armed-period BLE
/// teardown must leave a trace - an "armed canceled by X" log plus a reason
/// the UI can read back and show. Zero tolerance for silent failure.
void push_sink_cancel_why(const char *why);

/// 最近一次收摊的原因串（窗口到期/拔电/取消等）；无原因为 NULL。
/// The reason string of the most recent close-up (window expiry, unplug,
/// cancel, ...); NULL when there is none.
const char *push_sink_last_close_why(void);

/// 是否处于武装窗口的任意非 IDLE 态：栈必须保持运行、翻页器让位。
/// Whether the machine is in any non-IDLE armed state: the stack must stay up
/// and the page-turner yields.
bool push_sink_armed(void);

/// gen24.2：是否处于「待武装」——点了接收但 BLE 栈还没起来（app_loop 的
/// sync 正把它拉起，on_stack_sync 会续上）。这是合法的在途状态，不是
/// 会话结束：UI 必须留在推送页显示「正在启动蓝牙…」，闪回方法页就是
/// gen24.2 前的静默死锁（点了没反应 + 超时失败无人消费）。
/// Gen24.2: whether an arm is pending - receive was pressed but the BLE stack
/// is still coming up (app_loop's sync is pulling it; on_stack_sync completes
/// it). This is a legitimate in-flight state, NOT a finished session: the UI
/// must stay on the push page showing "starting bluetooth..." - bouncing back
/// to the method page was gen24.2's silent deadlock (a dead tap plus an
/// unconsumed timeout failure).
bool push_sink_pending(void);

/// 当前状态（UI 轮询刷新用）。/ Current state, for the UI to poll.
push_state_t push_sink_state(void);

/// 当前广播名（ARMED 之后有效）；未武装时返回空串。
/// The current advertising name (valid once ARMED); empty when not armed.
const char *push_sink_adv_name(void);

/// 窗口剩余毫秒；0 表示无超时或已退出。/ Milliseconds left in the window; 0
/// means no timeout or already closed.
uint32_t push_sink_window_left_ms(void);

/// 26.11.31 词典卡 AI 键武装：是否有挂起词等待连接建成后自动发查询。
/// UI 据此在词典卡显示「等待手机连接…」挂起提示；连接建成 flush 或取消/超时后为 false。
bool push_sink_dict_ai_pending(void);

/// 传输进度（RECEIVING 时有效）。/ Transfer progress, valid while RECEIVING.
void push_sink_progress(uint32_t *received, uint32_t *total);

/* ---- 栈钩子（ble_page_turner 调用）/ Stack hooks (called by ble_page_turner) ---- */

/// 栈同步完成（on_sync 末尾调）：实证核对 GATT 注册结果（没注册上就显式补一次
/// ble_gatts_start），续上待武装或把中断的广播找回来。
/// Stack sync done (called at the end of on_sync): verify the GATT
/// registration empirically (calling ble_gatts_start() explicitly if it never
/// landed), complete a pending arm and bring back an interrupted advertisement.
void push_sink_on_stack_sync(void);

/// 常驻注册 GATT 服务：把服务定义排进 NimBLE 的注册队列。必须在
/// nimble_port_init() 之后、host 任务创建之前调用——NimBLE 的 ble_hs_start()
/// 在 host 启动时一次性调用 ble_gatts_start() 消费该队列，且发生在 sync 回调
/// 之前；排在 sync 回调里的定义永远排不上（gen12 修复：手机端「能连上、
/// Services 页空白」的根因）。失败不影响翻页器：日志留证，sync 里还会兜底。
/// Register the GATT service residently: queue the definitions into NimBLE's
/// registration queue. Must be called after nimble_port_init() and before the
/// host task is created: NimBLE's ble_hs_start() calls ble_gatts_start()
/// exactly once at host start, before the sync callback; definitions queued in
/// the sync callback never make it (gen12 fix - the root cause of the phone
/// symptom "connects fine, Services page blank"). Failure does not affect the
/// page-turner: it is logged and the sync path retries as a fallback.
esp_err_t push_sink_gatt_register(void);

/// 栈即将拆除（stop_locked 开头调）：停广播、作废句柄。.part 与 NVS 会话保留，
/// 供栈回来后续传。/ The stack is about to be torn down (called at the start of
/// stop_locked): stop advertising and invalidate handles. The partial file and
/// the NVS session survive for resume after the stack returns.
void push_sink_on_stack_stop(void);

/// 武装期为真：ble_page_turner 的 poll 据此暂停翻页器连接（裁定 c）——有链路
/// 就地断但保留重连目标，退出武装自动接回。/ True while armed: ble_page_turner's
/// poll pauses the page-turner link (ruling c) - a live link drops in place with
/// its reconnect target kept, so leaving arm reconnects automatically.
bool push_sink_turner_paused(void);

/* ---- 三拍封送 / Three-beat quiesce (31b 第 1 条) ---- */

/// SD 侧要动卷之前调（掉卡卸载 / 格式化 / USB 挂载 / 深睡）。冻结会话入口，
/// 并等待三合取同时成立：**状态机空闲 ∧ BLE 路径存活 FILE\* 数为 0 ∧ 入口已冻结**。
///
/// 返回 true = 可以动卷，**冻结保持**，直到 app 层观测到三拍终态后调
/// `push_sink_thaw()`；返回 false = 有一方 2 s 内没停，调用方**不得动卷**，
/// 冻结同样保持（仍在三拍区间内，闩锁会重试；全败则调 `push_sink_quiesce_hold()`）。
///
/// 为什么超时不解冻：解冻会让新会话进来的那一刻正好撞上正在进行的卸卷——正是三拍
/// 要避免的东西。解冻只有两条权威通道，都由 app 层观测后打（SD 只发布事实，
/// 组件不反向读 SD）：
///   ① 卸卷完成（三拍终态成功）；
///   ② 插回后重新挂载（mount 边沿，解冻的**主通道**，不是兜底）。
///
/// 超时不留悬：冻结超过 `PUSH_FREEZE_WATCHDOG_MS` 且未到任何终态，poll 会强制
/// 解冻并通过 `push_sink_take_freeze_timeout()` 交出一条事件（app 写黑匣子）。
/// Call before touching the volume. Freezes the session entry and waits for the
/// conjunction: machine idle AND zero live FILE* on the BLE path AND entry frozen.
/// true = safe to move the volume, and the freeze STAYS until the app sees a
/// three-beat terminal state and calls push_sink_thaw(). false = someone did not
/// stop within the timeout; do NOT move the volume, and the freeze stays too.
bool push_sink_quiesce(unsigned timeout_ms);

/// 解冻（app 层在观测到终态后调用）。幂等；未冻结时静默。
/// Thaw, called by the app after observing a terminal state. Idempotent.
void push_sink_thaw(const char *why);

/* ---- 介质离开 / 插回：链路的所有权与对称义务（26.11.5） ---- */

/// 介质要走时，由 push_sink 自己把链路与广播一起收掉。**必须在 `push_sink_quiesce()`
/// 之前调用**（序关系由 B78 咬住）：只放行卸载而不收链路，手机下一次 BEGIN 会写向
/// 一个不存在的卷；只发 DISCONNECT 而不落到 IDLE，GAP 回调又会按「state != IDLE」
/// 把广播重新拉起来，等于把故障挪了个位置。
///
/// 收尾顺序写死：DISCONNECT（关 `.part`、存 NVS 簿记）→ CANCEL 到 IDLE（广播停）→
/// `ble_gap_terminate`。返回后 `push_sink_state()` 为 IDLE。
///
/// 代价（R1 申报）：拔卡瞬间正在等的 AI 回流（0x08）再也回不来，走既有超时口径
/// 诚实报「未收到」，不出假卡。罕见、可重试、失败诚实。
/// Cut the link and the advertisement when the medium goes, **before** the quiesce
/// call (B78 pins the order). Returns with the session machine in IDLE.
/// Cost: an in-flight AI reply (0x08) is lost and surfaces through the existing
/// timeout path as "not received" - rare, retryable, honest.
void push_sink_detach_media(const char *why);

/// 插回边沿：把上一次 `push_sink_detach_media()` 欠下的**可连性**还回去。
///
/// 没有它，detach 会把「插回不认卡」修成「插回认卡但手机再也连不上」——31a 家族
/// 的第三个入口（同一个终点、又一个入口）。会话还活着就把广播找回来；已经收官
/// （窗口到期/被取消）就按默认窗口重新武装。
///
/// 只在真的断过链时动作（内部欠账标志），所以正常插卡不会平白开一个接收窗口。
/// The reattach edge repays the connectivity the detach took away. Without it,
/// "the card is not recognised" would become "the card works but the phone can
/// never connect again" - the third entrance to the same 31a dead end. Only acts
/// when a detach actually happened, so a normal insert opens no window.
bool push_sink_resume_after_media(void);

/// hold 的原因码：写进黑匣子 `PICO_BOOTBB_NOTE_PUSH_HOLD` 的 `a` 字段。
/// **数值是账本的一部分**（日志里只出现数字），改语义必须先改这里。
/// 为什么要码：串口日志会丢，会话一结束就只剩黑匣子里那 3 个字节；没有原因码，
/// 下次开机看到「冻结保持」也还是不知道是哪一拍烂了。
/// Reason code for a hold, stored in the blackbox `a` field. The numbers are ledger
/// entries: the blackbox keeps only bytes, so the meaning has to live here.
typedef enum {
    PUSH_HOLD_WHY_QUIESCE = 1,  ///< 三合取一直等不到（卡在谁身上由 `b` 说明）
                                ///< the conjunction never closed; `b` says who was late
    PUSH_HOLD_WHY_RELEASE = 2,  ///< 卸卷一直失败（卷在，但放不掉）
                                ///< unmount kept failing: the volume is there but will not go
} push_hold_why_t;

/// 显式标记「保持冻结」：闩锁全败 = 卷挂着但逻辑已失效（毒卷）。此时解冻等于
/// 给新会话开一扇通往毒卷的门，所以**合法地**保持冻结，并留一条显式标记，
/// 让看门狗知道这不是卡死。只有插回挂载边沿或重启能出去。
///
/// 留痕：本函数**一定**写一条黑匣子（`PICO_BOOTBB_NOTE_PUSH_HOLD`，a = why_code，
/// b = 此刻仍未关闭的 FILE\\* 数）。只有日志没有黑匣子的「失败」在下次开机时等于没发生。
/// Explicitly hold the freeze: every latch retry failed, the volume is mounted
/// but logically poisoned. Thawing would open the gate onto it, so the freeze is
/// kept on purpose and marked. This call ALWAYS records a blackbox note: a failure
/// that only ever reached the UART did not happen as far as the next boot is concerned.
void push_sink_quiesce_hold(const char *why, push_hold_why_t why_code);

/// 当前是否冻结（诊断 + 宿主牙）。
bool push_sink_frozen(void);

/// BLE 路径当前存活的 FILE\* 数：`.part` 写句柄（经 push_part 只读 API）+
/// verify 读句柄深度。这是三合取②的**测量值**，不是推断。
/// Live FILE* count on the BLE path: the .part writer (via push_part's read-only
/// API) plus the verify reader depth. This is the measured value for conjunct 2.
unsigned push_sink_live_files(void);

/// 取走一次看门狗强制解冻事件（app 写黑匣子用）。frozen_ms 为被强制解冻前
/// 已冻结的毫秒数。
/// Take the one-shot watchdog event; frozen_ms is how long it had been frozen.
bool push_sink_take_freeze_timeout(unsigned *frozen_ms);

/* ---- 主循环 / Main loop ---- */

/// 主循环每轮调一次：喂 TICK 驱动窗口/宽限到期，看守充电常开的拔电即退，
/// 兼管待武装超时。/ Called once per main-loop iteration: feeds TICK for window
/// and grace expiry, watches the charger-held unplug exit and the pending-arm
/// timeout.
void push_sink_poll(void);

/// 提交段封送（26.9.30）：BLE 回调里的 COMMIT 只投递请求，不碰存储、不 reply。
/// 主循环每帧调一次取走；**读即清**（失败不回写，重试的所有权在 app 层），
/// 投递超过 2 s 未被取走则丢弃并回 ERR(BUSY)——上界从投递时刻算起。
/// The commit hand-off: COMMIT inside the BLE callback only posts - it touches no
/// storage and replies nothing. The main loop takes it once per frame;
/// read-and-clear (a failure is never written back - the app layer owns the retry),
/// and a request older than 2 s is dropped with ERR(BUSY), measured from post time.
bool push_sink_take_commit_request(void);

/// 执行四拍（`part_close` + rename 舞蹈），成功后补发被扣下的 OPEN_BOOK /
/// WORD_CARD 与「最近推送」/未读位。只在主循环帧间安全点调用。返回 false 时
/// 错误已经 reply 过，调用方只需保持旧书可读。
/// Runs the four beats, then re-posts the withheld OPEN_BOOK / WORD_CARD plus the
/// "last push" and unread bits. Main-loop safe point only. On false the error has
/// already been replied; the caller only has to keep the old book readable.
bool push_sink_run_commit(void);

/// 请求已取走但这一拍不能提交（commit 3：读者的书关不干净，越过死线后由 app 层
/// 调用）。**不回写请求**，只作废扣下的两个 UI 位并明确回 err（通常 ERR(BUSY)）。
/// Taken but not committable this frame (commit 3: the reader's book will not
/// close cleanly; the app layer calls this past its deadline). The request is
/// never written back; the withheld UI bits die and `err` is replied explicitly.
void push_sink_commit_abort(int err);

/* ---- UI 一次性事件 / One-shot UI events ---- */

/// 推送成功：取走开书请求（title 与 type 一并带出）。/ Push succeeded: take the
/// open-book request (title and type come with it).
bool push_sink_take_open(char *title, size_t cap, uint8_t *type);

/// 正文已成功落盘（COMMIT 完成）的一次性事件。首页状态卡据此把“最近推送”
/// 标题同步成最新内容并触发局部重绘——否则连续推送时该文字会卡在首推旧值。
/// 标题取自本次 BEGIN 的 title，type 为 PUSH_TYPE_*；单词卡（PUSH_TYPE_WORD）
/// 不写 push.txt，不会触发此事件。
/// One-shot raised when a body has just committed. The home status card uses
/// it to sync the "last push" title to the newest content and repaint - without
/// it a continuous push would freeze the line on the first push's old title.
/// The title comes from this push's BEGIN and type is PUSH_TYPE_*; word cards
/// (PUSH_TYPE_WORD) write no push.txt and never raise this event.
bool push_sink_take_committed(char *title, size_t cap, uint8_t *type);

/// 单词卡推送成功：取走单词（title 即单词）。/ Word-card push: take the word
/// (the title is the word).
bool push_sink_take_word_card(char *word, size_t cap);

/// 取走一条要展示给用户的错误（码见 push_proto.h 的 PUSH_ERR_*）。
/// Take an error to show the user (codes in push_proto.h).
bool push_sink_take_err(int *code);

/// 武装结束（窗口超时/取消/拔电/完成收摊）：UI 应退出武装页。
/// The armed window closed (timeout, cancel, unplug or wrap-up): the UI leaves
/// the arm page.
bool push_sink_take_closed(void);

/// 待武装失败（栈起不来）。/ A pending arm failed (the stack never came up).
bool push_sink_take_start_failed(void);

/* ---- 反向通道 / Reverse channel ---- */

/// 用户在设备上标记已读：notify EVT_READ（惊喜2，手机端清剪贴板的触发点）。
/// The user marked it read on the device: notify EVT_READ (surprise 2, the
/// phone-side clipboard clear trigger).
void push_sink_notify_read(void);

/// gen21 未读位：正文 COMMIT 落盘即置位（单词卡不置位），打开 push.txt 即清除；
/// NVS 持久化，重启不丢。驱动首页状态条的反色「有新内容」徽章。
/// gen21 unread flag: set when a body commit lands (word cards never set it),
/// cleared when push.txt is opened; persisted in NVS across reboots. Drives the
/// home status bar's inverted "new content" badge.
bool push_sink_unread(void);
void push_sink_clear_unread(void);

/// 通知当前电量百分比（BATT）。/ Notify the current battery percent.
void push_sink_notify_battery(void);

/// gen22 蓝牙校时：取走一次手机下发的 UTC 真值（一次性，非线程重复消费）。
/// 设备侧已在收到时写入系统时钟；本 API 交给 main 层登记漂移样本并同步 PMU。
/// gen22 time-over-BLE: take one phone-supplied UTC truth (one-shot). The device
/// already applied it to the system clock; main uses this to record the drift
/// sample and sync the PMU.
bool push_sink_take_time_sync(uint32_t *utc);

/* ---- gen26 AI 查词（蓝牙代理 + 搭车协议）---- */
/* gen26 AI lookup (BLE proxy + ride-along). */

/// 挂接搭车离线队列缓冲（设备侧 PSRAM）。main 启动期分配后调一次；长度不足
/// 仅记日志，AI 离线缓存不可用但不影响推送。
/// Attach the ride-along offline buffer (PSRAM on device); main calls once at
/// boot. A too-small buffer only logs - the offline cache is unavailable but
/// pushing is unaffected.
void push_sink_attach_ai_buffer(uint8_t *buf, size_t len, uint16_t cap);

/// 运行期开关 AI 查词（NVS 持久化）：关掉后不再上报能力位图、不再主动发起查词，
/// 已回流释义仍走预取缓存。/ Toggle AI lookup (NVS-persisted): off drops the
/// capability bitmap and proactive queries; cached definitions still serve.
void push_sink_set_ai_enabled(bool on);

/// 当前是否启用 AI 查词。/ Current AI-lookup enable state.
bool push_sink_ai_enabled(void);

/// 用户在设备上点了一个词（UI 调用）：命中预取缓存秒出卡；连上即请手机算
/// （STATUS 0x17）；离线则入搭车队列，下次连接回流。
/// The user tapped a word (UI): a cache hit renders at once; a connected tap
/// asks the phone (STATUS 0x17); an offline tap queues for the ride-along.
void push_sink_ai_tap_word(const char *word);

/// gen26.5 带上下文点词：book_id = TF 卡书名 FNV-1a（0 = 无书上下文），pos = 章内
/// 字节偏移。帧格式由协商结果决定——APK 未用 0x09 声明 CAP_AI_CONTEXT（或 book_id
/// 为 0）时一律发 66B legacy，对旧 APK 零感知。
/// gen26.5 contextual tap: book_id = FNV-1a of the book name (0 = none), pos = byte
/// offset in the chapter. The frame format follows the negotiation - unless the APK
/// declared CAP_AI_CONTEXT via 0x09 (and book_id != 0), a 66B legacy frame goes out,
/// which old APKs read without noticing anything.
void push_sink_ai_tap_word_ctx(const char *word, uint32_t book_id, uint32_t pos);

/// gen26.5 上下文源就绪开关：阅读器挂接 book_id/pos 采集后置 true。未就绪时 0x18
/// 能力上报**不带** CAP_AI_CONTEXT（不虚报能力），0x17 恒 legacy。
/// gen26.5 context-source readiness: the reader sets it once it can supply
/// book_id/pos. While false, 0x18 never advertises CAP_AI_CONTEXT (no false
/// claims) and 0x17 stays legacy.
void push_sink_ai_set_context_ready(bool ready);

/// gen26.5 当前 APK 声明的能力位（0 = 未使能，旧 APK 或断连后）。真机排障用。
/// gen26.5 capabilities the APK declared (0 = not enabled: old APK or after
/// disconnect). For on-hardware diagnosis.
uint32_t push_sink_apk_caps(void);

/// 取走一条待展示的释义（块 4 卡片框架入口）；UI 轮询消费，返回 true 表示有。
/// Take a pending definition to render (block 4 card framework entry); polled
/// by the UI, true means one was available.
bool push_sink_take_ai_def(char *word, size_t wcap, char *def, size_t dcap);

/* ---- gen26.5 TTS 听书（STATUS 0x19 页文本）---- */
/* gen26.5 TTS read-aloud (STATUS 0x19 page text). */

/// 链路可用 = 有连接 && 对端已订阅 STATUS。TTS 跟随轮询与朗读面板的元信息行
/// 共用这一个判据（未订阅时 notify 全丢，判据必须诚实）。
/// Link up = connected AND the peer subscribed to STATUS. The TTS follow poll
/// and the read-aloud panel's meta row share this one predicate (notifying an
/// unsubscribed peer just burns mbufs, so the predicate must be honest).
bool push_sink_link_up(void);

/// 把一页纯文本切成 0x19 帧序列推给手机（跟随模式的核心动作）。返回是否全部
/// 帧交付：任一帧未交付（未连接/未订阅/mbuf/rc≠0）即整页失败——不投缺帧页，
/// 对端在 seq==0 处自然重置，调用方下轮从头重发即可。文本须已由调用方完成域内
/// 清洗（图片占位等）；切分只保证 UTF-8 字符边界（push_tts.c）。
/// Split one page of plain text into a 0x19 frame sequence for the phone (the
/// core act of follow mode). Returns whether every frame was delivered: any
/// undelivered frame fails the whole page - a gapped page is never aired, the
/// peer resets at seq==0 and the caller simply resends from the top next round.
/// The text must already be domain-cleaned by the caller (image placeholders
/// etc.); the splitter only guarantees UTF-8 boundaries (push_tts.c).
bool push_sink_tts_send_page(const char *text, size_t len);

/* ---- M3 情景释义（STATUS 0x1A 上下文分帧）---- */
/* M3 scene lookup (STATUS 0x1A context segments). */

/// 情景解读开关（NVS 持久化，缺省开）。关掉后不再上报 CAP_AI_SCENE、不发 0x1A；
/// 现有 0x17 词典释义链路零变化。/ Scene-lookup switch (NVS-persisted, on by
/// default). Off drops CAP_AI_SCENE and 0x1A; the existing 0x17 path is untouched.
void push_sink_set_scene_enabled(bool on);
bool push_sink_scene_enabled(void);
/// M3-B：对面 APK 是否声明了「能出四节情景分析」（CAP bit4）。入口显示用它，
/// 而不是本地开关——本地开关只表示用户愿意，不表示对面答得上来。
/// M3-B: whether the peer APK declared "answers with four sections" (CAP bit4).
/// Use it for the entry's visibility instead of the local switch, which only says
/// the user would like it, not that the peer can answer.
bool push_sink_ai_scene_view_enabled(void);

/// 发送一次情景请求：word（≤63B）+ ctx（≤1024B，词在 ctx 内的字节偏移 word_off）
/// + flags（PUSH_SCENE_FLAG_*）。发送前置条件全不满足时返回 false 且一帧不发：
/// AI 关 / 情景关 / APK 未声明 CAP_AI_SCENE / 链路未订阅 / 参数非法。
/// 任一帧未交付即整体失败（与 0x19 同口径：不投缺帧请求，对端在 seq==0 重置）。
/// Send one scene request. Returns false and airs nothing unless every
/// precondition holds: AI on, scene on, APK declared CAP_AI_SCENE, link
/// subscribed, arguments valid. Any undelivered frame fails the whole request
/// (same stance as 0x19: never air a gapped request; the peer resets at seq==0).
bool push_sink_ai_scene_send(const char *word, const char *ctx, size_t ctx_len,
                             uint16_t word_off, uint8_t flags);

/* ---- gen30 伴读（R0 冻结件 1 · 方案 B 裸帧）：设备端链路能力（步 3）---- */
/* Read-along gen30 (R0 frozen item 1, plan B bare frames): device link capability. */

/// 发一帧 PAGE_BEGIN(STATUS 0x1C)：每页 0x19 流前置的页号对账键。调用后组件把
/// page_no 记为对账基准，后续 SUMMARY/TTS_DONE 页号不符一律丢弃；同时清掉上一页
/// 未取的回包。返回帧是否投递成功。/ Emit PAGE_BEGIN; it becomes the reconciliation
/// base and clears stale receipts; returns whether the frame was delivered.
bool push_sink_companion_page_begin(uint16_t page_no, uint8_t mode);
/// 发一帧 MODE_SET(STATUS 0x1B)：模式广播（source 取 PUSH_MODE_SRC_*）。
/// Emit MODE_SET, the mode broadcast (source is a PUSH_MODE_SRC_* value).
bool push_sink_companion_mode_set(uint8_t mode, uint8_t source);
/// 取一条通过页号对账的 SUMMARY；无待取回包返回 false。text 由调用方给缓冲，
/// 组件按 cap 截断并补 NUL（右栏渲染与二次截断属步 4 显示层，本函数只交付文本）。
/// Take one page-reconciled SUMMARY; text is copied into the caller's buffer.
/// Rendering / second-pass truncation belongs to step 4 (display layer).
bool push_sink_take_companion_summary(uint8_t *mode, uint16_t *page_no,
                                      uint8_t *idx, uint8_t *total,
                                      char *text, size_t cap);
/// 取一条通过页号对账的 TTS_DONE；无待取回执返回 false。设备端按 NVS 设置决定
/// 是否消费（自动翻页或忽略），组件不查询使能态（R0 拍板格 5：设备端自治）。
/// Take one page-reconciled TTS_DONE. The device side decides whether to consume
/// it from its NVS setting; the component never queries that flag.
bool push_sink_take_companion_tts_done(uint16_t *page_no, uint8_t *status);
/// 伴读活跃期连接档位：on=true 请求 FAIR 15ms 并抑制既有 session 驱动的 SLOW
/// 回落；on=false 解除抑制并回 SLOW 500ms（R0「以模式为档位边界，不做页级抖动」）。
/// Companion-active connection tier: on requests FAIR 15 ms and suppresses the
/// legacy session-driven SLOW fallback; off releases it and returns to SLOW.
void push_sink_companion_conn(bool on);
/// 发一帧 TTS_ENABLE(STATUS 0x1D)：朗读使能（op1 + 使能位1 = 2 B）。
/// 修复轮解耦用：设备端把「听书开关」这一事实单独告知手机，手机据此只控制声音；
/// 页文本推送（0x19）本身不再承载「念不念」的语义，摘要链因此与朗读解耦。
/// 返回帧是否投递成功；链路未就绪时返回 false，调用方在下次变化时重发。
/// Emit TTS_ENABLE (STATUS 0x1D, 2 B): the read-aloud enable bit. Decoupling fact
/// from the repair round - the device tells the phone the listening switch state
/// separately, so 0x19 carries text only and the summary chain is independent of
/// playback. Returns whether the frame was delivered.
bool push_sink_tts_enable_send(bool on);

#ifdef __cplusplus
}
#endif
