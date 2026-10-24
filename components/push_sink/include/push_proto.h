/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：手机 BLE 推流的帧格式与 CRC32。纯 C11，不依赖 ESP-IDF——字节序全部显式
 * 小端序列化，宿主与目标逐字段一致，tools/push_sink_host_test.c 直接对表。
 * English: frame format and CRC32 for phone BLE push. Pure C11 with no ESP-IDF
 * dependency: every field is explicitly little-endian so the host test in
 * tools/push_sink_host_test.c compares byte for byte.
 *
 * 冻结：帧布局一旦发布不得单方面改字段；加字段只能扩尾部保留区。
 * Frozen: never change field layout unilaterally once shipped; extend the tail only.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 常量 / Constants ---- */

#define PUSH_MAGIC0 0x50u ///< 'P' / 'P'
#define PUSH_MAGIC1 0x43u ///< 'C' / 'C'
#define PUSH_VERSION 1u   ///< 帧格式版本 / frame format version

#define PUSH_TITLE_MAX 64u  ///< 标题 UTF-8 上限（含结尾 0）/ title cap including NUL
#define PUSH_DATA_PAYLOAD_MAX 241u ///< MTU 247 时每包载荷：3 写属性头 + 3 帧头 / payload at MTU 247
#define PUSH_TOTAL_MAX (512u * 1024u) ///< 正文硬上限；超了 ERR(3) / hard body cap, ERR(3) beyond

/// CTRL 操作码。/ CTRL opcodes.
///
/// ---- 协议变更记录（每新增/改动 opcode 必须登记；BEGIN 帧布局已冻结） ----
/// Protocol change log (every opcode addition/change lands here; the BEGIN layout is frozen).
///   gen14  0x01..0x04  BEGIN/END/RESUME_QUERY/ABORT（BEGIN 帧 83B 定长，此后冻结）
///   gen22  0x05        SET_TIME：手机下发 UTC 秒（4B LE）校准设备时钟。
///                      · 独立于「必须武装」闸门之外：连上即可校时（有意设计——校时
///                        不携带 token，采用「连上即信任」模型；无 Wi-Fi 场景的唯一
///                        高频校时通道，PMU 无外部晶振、深睡靠内部 RC 走时会漂）。
///                      · sanity：长度 <8B 或 UTC 落在 1704067200..4102444799
///                        （2024-01-01~2100-01-01）之外 → ERR(2)，时钟不动。
///                      · 参考实现：PushProto.encodeSetTime（Java）、
///                        push_pico.py encode_set_time / --set-time（Python）已同步。
///   gen22  0x05        SET_TIME: the phone pushes UTC seconds (4 B LE) to set the clock.
///                      · Deliberately outside the "must be armed" gate - any connection
///                        may set the time (by design; it carries no token, i.e. a
///                        "connected means trusted" model) because it is the only
///                        high-frequency path when there is no WiFi.
///                      · Sanity: len < 8 B or UTC outside 1704067200..4102444799
///                        -> ERR(2) and the clock is untouched.
///                      · Reference implementations kept in sync: PushProto.encodeSetTime
///                        (Java) and push_pico.py encode_set_time / --set-time (Python).
///   gen25  0x08..0x0F  CTRL 预留区续（0x06/0x07 已由 gen26 启用）——为 gen26 代理层
///                      三件套（搭车协议 / 预取释义 / 通用代理层）整段留号。
///                      · 登记即冻结：任何一代要启用某个 0x06+ opcode，必须先在本
///                        变更记录登记一代一行（格式同 gen14/gen22）、同步 Java/
///                        Python 参考实现，再落实现；未登记的 opcode 即使帧头
///                        合法也按未知 op 走 push_sink.c 的 default 分支 →
///                        ERR(2)，会话状态机一拍不动（宿主测试有断言）。
///                      · 刻意选 ERR(2) 而非静默忽略：静默会让手机以为设备收下
///                        了，重试/续传语义全乱。
///   gen25  0x08..0x0F  the reserved CTRL block continues (0x06/0x07 taken by gen26) -
///                      a whole range set aside for gen26's proxy trio (the
///                      ride-along protocol, definition prefetch and the general
///                      proxy layer).
///                      · Registration freezes: any generation enabling a 0x06+
///                        opcode must first land one change-log line here (same
///                        shape as gen14/gen22), sync the Java/Python reference
///                        implementations, and only then implement; an
///                        unregistered opcode takes push_sink.c's default branch
///                        as an unknown op -> ERR(2) with the session machine
///                        untouched (asserted in the host test).
///                      · ERR(2) over a silent drop on purpose: silence would make
///                        the phone believe the device took it, wrecking retry and
///                        resume semantics.
///   gen26  0x06..0x07  AI 查词（蓝牙代理 + 搭车协议首个插件）：
///                      · 0x06 PUSH_OP_AI_QUERY：手机→设备，触发离线队列回流 /
///                        即时查询请求（设备此前已用 STATUS 0x17 把待查词通知手机）。
///                      · 0x07 PUSH_OP_AI_RESULT：手机→设备，单词 + 释义文本回传。
///                      · 设备→手机方向走 STATUS 0x17（PUSH_ST_AI_QUERY）：用户点词，
///                        请手机算释义；手机算完用 0x07 回传，设备出释义卡。
///                      · 能力协商：设备连接后用 STATUS 0x18（PUSH_ST_CAPS）上报
///                        能力位图，APK 据此显示/隐藏查词入口（见 push_caps_t）。
///                      · 登记即冻结：本行即登记；帧布局见 push_proto.c 的 ai 编解码。
///                        Java/Python 参考实现随本代同步（PushProto.encodeAi*、
///                        push_pico.py）。0x08..0x0F 仍预留（预取/扩展卡等）。
///   gen26  0x06..0x07  AI word lookup (first plugin of the BLE proxy + ride-along):
///                      · 0x06 PUSH_OP_AI_QUERY: phone to device, flush the offline
///                        queue or ask now (the device already notified the word via
///                        STATUS 0x17).
///                      · 0x07 PUSH_OP_AI_RESULT: phone to device, word + definition.
///                      · Device to phone uses STATUS 0x17 (PUSH_ST_AI_QUERY): the
///                        user tapped a word, the phone computes, replies with 0x07.
///                      · Capability negotiation: on connect the device reports a
///                        bitmap via STATUS 0x18 (PUSH_ST_CAPS); the APK gates the
///                        entry on it (see push_caps_t).
///                      · Registered and frozen: this line is the registry; the frame
///                        layout lives in push_proto.c's ai codec. Java/Python
///                        references sync this generation. 0x08..0x0F stay reserved.
///   gen26.5 0x08/0x09   AI 语境释义 + 多帧 + 能力使能（M2 收官轮，三端登记即冻结）：
///                      · 0x17/0x06 扩上下文：word[64] + book_id(u32 LE) + pos(u32 LE)
///                        （书标识 = TF 卡书名 FNV-1a 哈希，pos = 章内字节偏移）；
///                        旧 66B 帧按 len 分流继续受理（向后兼容，见 ai 编解码注释）。
///                      · 0x08 PUSH_OP_AI_RESULT_SEG：手机→设备，AI 释义分帧回传。
///                        布局：头(4)+wlen(1)+word[64]+seg_idx(u16 LE)+seg_total(u16 LE)
///                        +dlen(u16 LE)+seg[166]，零填充定长 241B（帧帽恰尽）；
///                        UTF-8 字符边界切分；seg_total==1 时语义等价 0x07。
///                      · 0x07 保留不删（老 APK 单帧路）；新 APK 优先 0x08。
///                      · 能力位：PUSH_CAP_AI_CONTEXT(1<<1) / PUSH_CAP_AI_MULTI(1<<2)，
///                        经 0x18 上报；老 APK 不置位 → 设备逐字节走 M1 老路。
///                      · 0x09 PUSH_OP_AI_CAPS_ENABLE（手机→设备，8B = 头4 + bits
///                        u32 LE）：APK 订阅 STATUS 后发一次，声明手机侧支持位。
///                        双向协商闭环——设备收使能前 0x17 一律 66B 旧帧；收到且
///                        CONTEXT 置位才发 74B 上下文档；旧 APK 永远不发 0x09 →
///                        永远收旧帧，零感知兼容；断连清零重发；与 0x05 同模型，
///                        连上即可（不受「必须武装」闸门限制）。
///                      · 0x07 不加上下文回显（241B 帧帽零余量）：设备侧「最近查询
///                        上下文」缓存匹配关连。
///                      · 登记即冻结：本行即登记；帧布局见 push_proto.c 的 ai 编解码
///                        （gen26.5 节）。Java/Python 参考实现同代同步。
///   gen26.5 0x08        AI contextual definitions + multi-frame (M2 closing round,
///                      registered and frozen across all three ends):
///                      · 0x17/0x06 gain context: word[64] + book_id(u32 LE) +
///                        pos(u32 LE) (book id = FNV-1a of the TF-card book name,
///                        pos = byte offset within the chapter); the old 66 B frame
///                        stays accepted by length dispatch (see the ai codec notes).
///                      · 0x08 PUSH_OP_AI_RESULT_SEG: phone to device, segmented
///                        definition. seg_idx/seg_total (u16 LE) + a ≤170 B slice cut
///                        on UTF-8 boundaries; seg_total==1 is equivalent to 0x07.
///                      · 0x07 stays (old single-frame path); new APKs prefer 0x08.
///                      · Caps: PUSH_CAP_AI_CONTEXT(1<<1) / PUSH_CAP_AI_MULTI(1<<2)
///                        reported via 0x18; an old APK without them keeps the
///                        byte-identical M1 path.
///                      · 0x09 PUSH_OP_AI_CAPS_ENABLE (phone to device, 8 B = 4-byte
///                        header + bits u32 LE): sent once after the APK subscribes
///                        to STATUS, declaring the phone-side supported bits. This
///                        closes the negotiation loop - before the enable arrives
///                        the device only ever emits the 66 B legacy 0x17; only with
///                        CONTEXT set does it emit the 74 B contextual frame. An old
///                        APK never sends 0x09, so it always gets legacy frames:
///                        zero-perception compatibility. The enable state clears on
///                        disconnect (re-sent on reconnect) and, like 0x05, works
///                        without an armed window.
///                      · 0x07 carries no context echo (241 B frame is exactly full):
///                        the device correlates via its last-query context cache.
///                      · Registered and frozen: this line is the registry; layouts
///                        live in push_proto.c's ai codec (gen26.5 section). Java and
///                        Python references sync this generation.
enum {
    PUSH_OP_BEGIN = 0x01,         ///< 开始一次会话 / start a session
    PUSH_OP_END = 0x02,           ///< 正文结束，请求校验 / body done, ask for verification
    PUSH_OP_RESUME_QUERY = 0x03,  ///< 重连后问断点 / ask for the resume point after reconnect
    PUSH_OP_ABORT = 0x04,         ///< 放弃本次会话 / abandon the session
    PUSH_OP_SET_TIME = 0x05,      ///< gen22：手机下发 UTC 秒（4 字节小端）校准设备时钟 /
                                  ///< phone pushes a UTC second count (4 bytes LE) to set the clock
    PUSH_OP_AI_QUERY = 0x06,       ///< gen26：AI 查词请求（手机→设备：触发离线队列回流 / AI word-query request)
    PUSH_OP_AI_RESULT = 0x07,      ///< gen26：AI 释义回传（手机→设备：单词 + 释义文本 / AI definition result)
    PUSH_OP_AI_RESULT_SEG = 0x08,  ///< gen26.5：AI 释义分帧回传（手机→设备；seg_total==1 等价 0x07）/
                                  ///< gen26.5: segmented AI definition (seg_total==1 == 0x07)
    PUSH_OP_AI_CAPS_ENABLE = 0x09, ///< gen26.5：手机→设备 AI 能力使能（bits u32 LE；收前 0x17 一律旧帧）/
                                  ///< gen26.5: phone-to-device AI capability enable (legacy 0x17 until seen)
    PUSH_OP_RESERVED_FIRST = 0x0A, ///< gen25 预留区续（0x08/0x09 已由 gen26.5 启用）/
                                  ///< gen25 reservation continues (0x08/0x09 taken by gen26.5)
    PUSH_OP_RESERVED_LAST = 0x0F,  ///< gen25 预留区终点（STATUS 从 0x10 起，零重叠）/
                                   ///< gen25 reservation end (STATUS starts at 0x10, no overlap)
};

/// CTRL 帧头字节数：magic(2) + op(1) + version(1)。/ CTRL header bytes: magic(2)+op(1)+version(1).
#define PUSH_CTRL_HDR_LEN 4u

/// STATUS 操作码（设备→手机）。/ STATUS opcodes (device to phone).
enum {
    PUSH_ST_READY = 0x10,    ///< 会话已接受，可以发数据 / session accepted, send data
    PUSH_ST_RESUME = 0x11,  ///< 断点：从 offset 续、seq 从 next_seq 起 / resume point
    PUSH_ST_DONE = 0x12,    ///< 校验通过，已开书 / verified and opened
    PUSH_ST_ERR = 0x13,     ///< 拒绝，body 是错误码 / rejected, body is the error code
    PUSH_ST_EVT_READ = 0x14, ///< 用户在设备上标记已读 / the user marked it read on the device
    PUSH_ST_BATT = 0x15,    ///< 电量百分比 / battery percent
    PUSH_ST_PROGRESS = 0x16, ///< 接收进度 / receive progress
    PUSH_ST_AI_QUERY = 0x17, ///< gen26：设备→手机，用户点词请算释义 / device to phone: define this word
    PUSH_ST_CAPS = 0x18,     ///< gen26：设备→手机，能力位图上报（连接后）/ device to phone: capability bitmap
};

/// 正文类型。/ Body types.
enum {
    PUSH_TYPE_TEXT = 0,  ///< 纯文本 / plain text
    PUSH_TYPE_MARKDOWN = 1, ///< Markdown / Markdown
    PUSH_TYPE_HTML = 2,   ///< HTML / HTML
    PUSH_TYPE_URL = 3,    ///< 单个 URL，M1.5 抓取 / one URL, fetch in M1.5
    PUSH_TYPE_WORD = 4,   ///< 单词卡：title 即单词 / word card: title is the word
};

/// 错误码（STATUS ERR 的 body）。/ Error codes carried in STATUS ERR bodies.
enum {
    PUSH_ERR_NONE = 0,
    PUSH_ERR_NOT_ARMED = 1,     ///< 不在武装窗口 / not inside an armed window
    PUSH_ERR_BAD_FRAME = 2,     ///< 魔数/版本/长度非法 / bad magic, version or length
    PUSH_ERR_TOO_LARGE = 3,     ///< total_len 超上限 / total_len over the cap
    PUSH_ERR_CHECKSUM = 4,      ///< CRC32 不符 / CRC32 mismatch
    PUSH_ERR_NO_CARD = 5,       ///< TF 卡不可用 / card unavailable
    PUSH_ERR_LOW_BATTERY = 6,   ///< 电量不足且未充电 / low battery, not charging
    PUSH_ERR_BUSY = 7,          ///< 已有一次推送在处理 / a push is already in flight
    PUSH_ERR_FETCH = 8,         ///< URL 抓取失败（M1.5）/ URL fetch failed (M1.5)
    PUSH_ERR_NO_DICT = 9,       ///< 单词卡但设备无词典 / word card but no dictionary
};

/// BEGIN 帧（83 字节，全部小端）。/ BEGIN frame (83 bytes, all little-endian).
typedef struct {
    uint32_t total_len; ///< 正文字节数；URL/单词卡为 0 / body bytes; 0 for URL and word
    uint32_t crc32;     ///< 正文 CRC32；URL/单词卡为 0 / body CRC32; 0 for URL and word
    uint32_t session_id; ///< 会话标识，断点续传对账 / session id for resume matching
    uint16_t token;     ///< 广播名后缀，须与窗口期广播名一致 / adv-name suffix, must match the window
    uint8_t type;       ///< PUSH_TYPE_* / PUSH_TYPE_*
    char title[PUSH_TITLE_MAX]; ///< UTF-8，截断保 0 / UTF-8, truncated with NUL
} push_begin_t;

#define PUSH_BEGIN_LEN 83u ///< BEGIN 帧字节数 / BEGIN frame size
#define PUSH_DATA_HDR_LEN 3u ///< DATA 帧头：seq(2)+len(1) / DATA frame header

/* ---- AI 查词帧（gen26，0x06/0x07 + STATUS 0x17）---- */
/* AI lookup frames (gen26, 0x06/0x07 + STATUS 0x17). */

/// 单词 / 释义文本上限（UTF-8 字节，含结尾 0）。/ Word and definition text caps.
#define PUSH_AI_WORD_MAX 64u  ///< 单词上限 / word cap
#define PUSH_AI_DEF_MAX  170u ///< 单帧释义上限（≤ MTU 247 载荷，留头空间）/ one-frame def cap

/// AI_QUERY（0x06）CTRL 载荷：手机→设备，触发离线队列回流或即时查询。
/// AI_QUERY (0x06) CTRL payload: phone to device, flush the offline queue or ask now.
typedef struct {
    char word[PUSH_AI_WORD_MAX]; ///< 待查单词（UTF-8，NUL 结尾）/ the word, NUL-terminated
} push_ai_query_t;

/// AI_RESULT（0x07）CTRL 载荷：手机→设备，单词 + 释义。/ AI_RESULT (0x07): word + definition.
typedef struct {
    char word[PUSH_AI_WORD_MAX]; ///< 单词 / the word
    char def[PUSH_AI_DEF_MAX];   ///< 释义文本（UTF-8，NUL 结尾）/ the definition, NUL-terminated
} push_ai_result_t;

/// 编 AI_QUERY 帧：out 至少 `PUSH_AI_QUERY_MAX` 字节，返回写入字节数（0 表示 cap 不够）。
/// Encode an AI_QUERY frame: out needs `PUSH_AI_QUERY_MAX` bytes; returns bytes written (0 on cap miss).
size_t push_ai_query_encode(const push_ai_query_t *q, uint8_t *out, size_t cap);
/// 解 AI_QUERY 帧（含 4 字节 CTRL 头）。坏头/超长返 false。
/// Decode an AI_QUERY frame (4-byte CTRL header included). false on bad header or overlong body.
bool push_ai_query_decode(const uint8_t *in, size_t len, push_ai_query_t *q);

/// 编 AI_RESULT 帧。/ Encode an AI_RESULT frame.
size_t push_ai_result_encode(const push_ai_result_t *r, uint8_t *out, size_t cap);
/// 解 AI_RESULT 帧（含 4 字节 CTRL 头）。/ Decode an AI_RESULT frame.
bool push_ai_result_decode(const uint8_t *in, size_t len, push_ai_result_t *r);

/// 设备→手机查询通知（STATUS 0x17）：用户点了单词，请手机算释义。
/// Device-to-phone query notify (STATUS 0x17): the user tapped a word, compute it.
size_t push_st_ai_query_encode(const char *word, uint8_t *out, size_t cap);

/// AI 帧字节上限（含 4 字节 CTRL 头 / 含 1 字节 op 的 STATUS）。/ AI frame caps (CTRL header / STATUS op included).
#define PUSH_AI_QUERY_MAX  (PUSH_CTRL_HDR_LEN + 1u + PUSH_AI_WORD_MAX)
#define PUSH_AI_RESULT_MAX (PUSH_CTRL_HDR_LEN + 1u + PUSH_AI_WORD_MAX + 2u + PUSH_AI_DEF_MAX)

/* ---- AI 语境帧（gen26.5，0x17/0x06 扩上下文）---- */
/* AI contextual frames (gen26.5, 0x17/0x06 with context). */

/// 上下文查询（STATUS 0x17 / CTRL 0x06 扩展负载）：词 + 书标识 + 章内位置。
/// Contextual query payload (STATUS 0x17 / CTRL 0x06 extended): word + book id + position.
typedef struct {
    char word[PUSH_AI_WORD_MAX]; ///< 待查单词 / the word
    uint32_t book_id;            ///< TF 卡书名 FNV-1a 哈希；0 = 无书上下文 / FNV-1a of the book name; 0 = none
    uint32_t pos;                ///< 章内字节偏移；0 = 未知 / byte offset in the chapter; 0 = unknown
} push_ai_query_ctx_t;

/// AI 帧字节上限（gen26.5 带上下文版本，含 4 字节 CTRL 头 / 含 1 字节 op 的 STATUS）。
/// Frame caps for the gen26.5 contextual versions (CTRL header / STATUS op included).
#define PUSH_AI_CTX_QUERY_MAX   (PUSH_CTRL_HDR_LEN + 1u + PUSH_AI_WORD_MAX + 4u + 4u) ///< = 77
#define PUSH_ST_AI_CTX_QUERY_LEN (1u + 1u + PUSH_AI_WORD_MAX + 4u + 4u)              ///< = 74（STATUS 0x17 帧全长：op+wlen+word+ctx）
#define PUSH_ST_AI_LEGACY_QUERY_LEN (1u + 1u + PUSH_AI_WORD_MAX)                     ///< = 66（旧 0x17 帧全长；len 分流：66→旧路，74→新路）

/// gen26.5 编 AI_QUERY 上下文档（CTRL 0x06，77B 零填充）。/ Encode a contextual AI_QUERY (gen26.5, 77B).
size_t push_ai_query_ctx_encode(const push_ai_query_ctx_t *q, uint8_t *out, size_t cap);
/// gen26.5 解 AI_QUERY 上下文档（严格 77B；旧 69B 帧喂本函数返 false，走 legacy 解码）。
/// Decode a contextual AI_QUERY (strict 77B; a legacy 69B frame returns false here).
bool push_ai_query_ctx_decode(const uint8_t *in, size_t len, push_ai_query_ctx_t *q);

/// gen26.5 编 STATUS 0x17 上下文档（74B 体：op+wlen+word+book_id+pos）。
/// Encode a contextual STATUS 0x17 (gen26.5, 74B body: op+wlen+word+book_id+pos).
size_t push_st_ai_query_ctx_encode(const char *word, uint32_t book_id, uint32_t pos,
                                   uint8_t *out, size_t cap);

/// gen26.5 设备侧 0x17 发送决策（唯一出口）：按「APK 是否使能 CONTEXT」+「有无真实
/// 上下文」选帧格式。**默认 legacy**——旧 APK 从不发 0x09，故永远走 66B 旧路，零感知。
/// Device-side 0x17 emission decision (the single exit point): legacy vs contextual.
/// **Legacy by default** — an old APK never sends 0x09, so it always gets the 66B
/// frame and notices nothing.
///
/// 决策表 / decision table：
///   - ctx_enabled == false                    → 66B legacy（旧 APK / 未使能 / 断连后）
///   - ctx_enabled == true 且 book_id != 0     → 74B ctx（词 + 书 + 位置）
///   - ctx_enabled == true 且 book_id == 0     → 66B legacy（词典弹窗等无书场景：不编造上下文）
/// out 至少 PUSH_ST_AI_CTX_QUERY_LEN（74）字节以容纳两种帧；返回帧长，0 = 参数/cap 非法。
size_t push_st_ai_query_emit(const char *word, uint32_t book_id, uint32_t pos,
                             bool ctx_enabled, uint8_t *out, size_t cap);

/* ---- AI 释义分帧（gen26.5，0x08）---- */
/* AI definition segments (gen26.5, 0x08). */

#define PUSH_AI_SEG_MAX 166u ///< 分帧释义槽（UTF-8 字节）：241 帧帽 - 头4 - wlen1 - word64 - idx2 - total2 - dlen2 / segment slot cap

/// AI_RESULT_SEG（0x08）CTRL 载荷：分帧回传释义。/ AI_RESULT_SEG (0x08) payload: segmented definition.
typedef struct {
    char word[PUSH_AI_WORD_MAX]; ///< 单词（每帧都带，重组端按词关连）/ the word (every frame; correlation key)
    uint16_t seg_idx;            ///< 本帧序号，从 0 起 / this frame's index, 0-based
    uint16_t seg_total;          ///< 总帧数；==1 时语义等价 0x07 / total frames; 1 == plain 0x07
    uint16_t dlen;               ///< 本帧 seg 有效字节数（≤ PUSH_AI_SEG_MAX）/ valid bytes in seg this frame
    char seg[PUSH_AI_SEG_MAX];   ///< 本帧释义片段（UTF-8，边界由编码方保证）/ this frame's slice
} push_ai_seg_t;

/// AI 分帧字节上限（含 4 字节 CTRL 头；零填充定长）。/ Segmented frame cap (CTRL header included; zero-padded).
#define PUSH_AI_SEG_FRAME_MAX (PUSH_CTRL_HDR_LEN + 1u + PUSH_AI_WORD_MAX + 2u + 2u + 2u + PUSH_AI_SEG_MAX) ///< = 241

/// gen26.5 编 AI_RESULT_SEG（0x08，241B 零填充定长）。/ Encode an AI_RESULT_SEG (gen26.5, 241B).
size_t push_ai_seg_encode(const push_ai_seg_t *s, uint8_t *out, size_t cap);
/// gen26.5 解 AI_RESULT_SEG（241B 定长；校验 idx < total、total ≥ 1）。
/// Decode an AI_RESULT_SEG (fixed 241B; validates idx < total and total >= 1).
bool push_ai_seg_decode(const uint8_t *in, size_t len, push_ai_seg_t *s);

/* ---- 能力位图（gen26 能力协商）---- */
/* Capability bitmap (gen26 capability negotiation). */

typedef struct {
    uint32_t bits; ///< 能力位掩码 / capability bitmask
} push_caps_t;

#define PUSH_CAP_AI_QUERY  (1u << 0) ///< 设备支持 AI 查词（0x06/0x07 + 0x17）/ device supports AI lookup
#define PUSH_CAP_AI_CONTEXT (1u << 1) ///< gen26.5：0x17/0x06 带书标识+位置上下文 / gen26.5: contextual 0x17/0x06
#define PUSH_CAP_AI_MULTI   (1u << 2) ///< gen26.5：0x08 释义分帧 / gen26.5: segmented definitions (0x08)

/// 编能力位图 STATUS 帧（op 0x18 + 4 字节小端位掩码）。/ Encode a capability bitmap STATUS frame.
size_t push_caps_encode(const push_caps_t *caps, uint8_t *out, size_t cap);
/// 解能力位图 STATUS 帧。/ Decode a capability bitmap STATUS frame.
bool push_caps_decode(const uint8_t *in, size_t len, push_caps_t *caps);

/// AI_CAPS_ENABLE（0x09）帧字节数：头(4) + bits(u32 LE)。/ AI_CAPS_ENABLE (0x09) frame size: header + bits.
#define PUSH_AI_CAPS_ENABLE_LEN (PUSH_CTRL_HDR_LEN + 4u) ///< = 8

/// gen26.5 编 AI_CAPS_ENABLE（CTRL 0x09，8B）。/ Encode an AI_CAPS_ENABLE (gen26.5, CTRL 0x09, 8B).
size_t push_ai_caps_enable_encode(const push_caps_t *caps, uint8_t *out, size_t cap);
/// gen26.5 解 AI_CAPS_ENABLE（严格 8B）。/ Decode an AI_CAPS_ENABLE (strict 8B).
bool push_ai_caps_enable_decode(const uint8_t *in, size_t len, push_caps_t *caps);

/* ---- CRC32 ---- */

/// IEEE 反射式 CRC32（多项式 0xEDB88320），与 zlib 同款。/ Reflected IEEE CRC32 (poly 0xEDB88320), same as zlib.
uint32_t push_crc32(const void *data, size_t len);

/// 流式 CRC32：crc 传上一段的返回值（首段传 0），可分段喂 512KB 级正文而不必
/// 整块驻留内存。链式调用等价于对拼接后的一次运算：continue(continue(0,a),b)
/// 等于 continue(0, a||b)，也等于 push_crc32(a||b)。空段是合法的直通。
/// Streaming CRC32: pass the previous return value (0 for the first chunk) so a
/// 512 KB body can be fed in pieces without holding it all in RAM. Chaining is
/// associative: continue(continue(0,a),b) == continue(0, a||b) == push_crc32(a||b);
/// an empty chunk is a legal pass-through.
uint32_t push_crc32_continue(uint32_t crc, const void *data, size_t len);

/* ---- BEGIN 编解码 / BEGIN codec ---- */

/// 序列化 BEGIN 到 out（至少 PUSH_BEGIN_LEN 字节），返回写入字节数。
/// Serialise a BEGIN into out (PUSH_BEGIN_LEN bytes); returns bytes written.
size_t push_begin_encode(const push_begin_t *begin, uint8_t *out, size_t cap);

/// 反序列化 len 字节到 begin。坏魔数/坏版本/长度不足返回 false。
/// Deserialise len bytes into begin; false on bad magic, bad version or short buffer.
bool push_begin_decode(const uint8_t *in, size_t len, push_begin_t *begin);

/* ---- DATA 帧头 / DATA frame header ---- */

/// 取出 DATA 帧的 seq 与载荷长度；帧非法返回 false。
/// Read seq and payload length from a DATA frame; false when the frame is malformed.
bool push_data_decode(const uint8_t *in, size_t len, uint16_t *seq, const uint8_t **payload, size_t *payload_len);

/* ---- CTRL 帧头 / CTRL frame header ---- */

/// 校验 CTRL 帧公共头（魔数 + 版本 + 长度 ≥4）并取出 opcode。END / RESUME_QUERY /
/// ABORT 无载荷，走这一路；BEGIN 另有 push_begin_decode 解析全文。
/// Validate the shared CTRL header (magic + version + length >= 4) and extract the
/// opcode. END / RESUME_QUERY / ABORT carry no payload and take this path; BEGIN
/// is parsed in full by push_begin_decode instead.
bool push_ctrl_header_decode(const uint8_t *in, size_t len, uint8_t *op);

/* ---- STATUS 编解码 / STATUS codec ---- */

#define PUSH_STATUS_MAX 16u ///< STATUS 帧上限 / STATUS frame cap

/// 编码一条 STATUS：opcode + 至多 12 字节 body，返回帧长（0 表示 cap 不够）。
/// Encode one STATUS: opcode plus up to 12 body bytes; returns frame length (0 when out of cap).
size_t push_status_encode(uint8_t opcode, const uint8_t *body, size_t body_len, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
