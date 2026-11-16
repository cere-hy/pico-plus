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
///   gen26.5 0x19       TTS 听书（M2 Step10；三端登记即冻结）：
///                      · 0x19 PUSH_ST_TTS_TEXT：设备→手机，当前页纯文本（STATUS
///                        notify 方向）。布局：op(1)+seq(u16 LE)+total(u16 LE)+
///                        len(u16 LE)+text[234]，零填充定长 **241B**（帧帽 244 内，
///                        与 CTRL 载荷同宽，留 3B 诊断余量）；text 槽保留 NUL，
///                        有效字节 ≤233；UTF-8 字符边界由编码方保证。
///                      · **不复用 0x08**：0x08 是手机→设备的释义分帧（CTRL 写方向），
///                        方向不同、语义不同，混用会让设备端解码器互相误收。
///                      · **不加 CAPS 协商位**：设备主动发，手机收不收得住由 APK 版本
///                        定——旧 APK 对未知 STATUS notify 帧本就丢弃（waitStatus
///                        实证），零协商需求，不过度设计。
///                      · 编号说明：开工令原拟 0x0A，但 0x0A 属 CTRL 预留区（手机→
///                        设备方向空间，PUSH_OP_RESERVED_FIRST）。设备→手机帧落在
///                        STATUS 空间（0x10 起）才守住「一个字节辨方向」的诊断纪律，
///                        故取 STATUS 空间下一个空位 **0x19**（0x10..0x18 已用）。
///                        0x0A 继续留作 CTRL 预留。
///   gen30   0x1B/0x1C  伴读案（30 批，视听读三位一体；R0 冻结件 1）：四处新 opcode
///   + 0x0A/0x0B        按「一代一行登记即冻结」登记。STATUS 0x1B/0x1C 为设备→手机，
///                      CTRL 0x0A/0x0B 为手机→设备，守住「一个字节辨方向」。
///                      · MODE_SET(STATUS 0x1B)：op1+mode1+source1+reserved2 = 5B。
///                        mode 0=外语原著(默认)/1=小说网文/2=社科非虚构/3=古文诗词；
///                        source 0=设备菜单/2=初始化（1=APK 端预留，一期不启用）。
///                      · PAGE_BEGIN(STATUS 0x1C)：op1+page_no2(LE)+mode1+reserved1 = 5B。
///                        每页 0x19 流前置的页号对账键；mode 与 0x1B 不一致时按本帧覆盖并记日志。
///                      · SUMMARY(CTRL 0x0A)：op1+mode1+page_no2+idx1+total1+dlen2+
///                        text233 = 241B；手机→设备摘要回包，同构 0x08 分帧，一期恒
///                        total=1，二期多卡片用 idx/total 扩展、不改帧。
///                      · TTS_DONE(CTRL 0x0B)：op1+page_no2+status1 = 4B；
///                        status 0=播完/1=用户中断/2=失败；设备按 NVS 设置决定是否消费。
///                      · 页号对账：PAGE_BEGIN/SUMMARY/TTS_DONE 的 page_no ≠ 设备当前页一律丢弃。
///                      · 封帧口径（哨位裁定 2026-11-10 19:03 · 方案 B 裸帧对齐）：
///                        四帧一律按冻结表 op 首字节起算裸帧；CTRL 方向为 gen30 新帧
///                        开白名单分派——仅首字节 ∈ {0x0A,0x0B} 且总长恰等 241/4 B
///                        才走裸帧，其余一律走现制 4B 魔数校验、失败→ERR(2)。
///                        现制帧首字节固定 0x50 与白名单零交集，分派确定性成立；
///                        wire 帽 244 B（MTU 247 − 3 写属性头）下裸帧 241 ≤ 244，
///                        零 reinterpret、零改 R0 字段算术与 _Static_assert。
///                      · 0x19 等既有冻结帧一字节不动；未登记 op 仍走 default→ERR(2)，
///                        0x0A/0x0B 已登记故不再属「未登记」。Java/Python 参考实现同代
///                        同步常量与裸帧口径；APK 发送侧编解码函数体属 APK 工作包（并行）。
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
///   gen26.5 0x19       TTS read-aloud (M2 Step10; registered and frozen across
///                      all three ends):
///                      · 0x19 PUSH_ST_TTS_TEXT: device to phone, the current page's
///                        plain text (a STATUS notify). Layout: op(1) + seq(u16 LE) +
///                        total(u16 LE) + len(u16 LE) + text[234], zero-padded to a
///                        fixed **241 B** (inside the 244 cap, same width as a CTRL
///                        payload, 3 B of diagnostic headroom); the slot keeps its
///                        NUL, so <= 233 valid bytes; the encoder owns UTF-8
///                        character boundaries.
///                      · **Not 0x08**: that is the phone-to-device definition
///                        segment (a CTRL write) - different direction, different
///                        meaning; sharing would make decoders mis-accept.
///                      · **No CAPS bit**: the device pushes, and whether the phone
///                        copes is an APK-version matter; old APKs already drop
///                        unknown STATUS notify frames (measured via waitStatus), so
///                        there is nothing to negotiate - no over-engineering.
///                      · Numbering note: the directive proposed 0x0A, but 0x0A sits
///                        in the CTRL reservation (phone-to-device space,
///                        PUSH_OP_RESERVED_FIRST). Device-to-phone frames belong to
///                        the STATUS space (from 0x10) to keep the "one byte tells
///                        you the direction" diagnostic rule, so the next free
///                        STATUS slot **0x19** is taken (0x10..0x18 are used).
///                        0x0A stays reserved for CTRL.
///   gen30   0x1B/0x1C  Read-along companion (batch 30, R0 frozen item 1): four new
///   + 0x0A/0x0B        opcodes registered "one generation, one line, registration freezes".
///                      STATUS 0x1B/0x1C are device-to-phone, CTRL 0x0A/0x0B are
///                      phone-to-device, preserving the "one byte tells direction" rule.
///                      · MODE_SET (STATUS 0x1B): op1+mode1+source1+reserved2 = 5 B;
///                        mode 0=foreign (default)/1=novel/2=social/3=classical;
///                        source 0=device menu/2=init (1=APK reserved, unused in phase 1).
///                      · PAGE_BEGIN (STATUS 0x1C): op1+page_no2(LE)+mode1+reserved1 = 5 B;
///                        precedes each 0x19 stream, the page-number reconciliation key;
///                        a mode mismatch with 0x1B is overridden by this frame and logged.
///                      · SUMMARY (CTRL 0x0A): op1+mode1+page_no2+idx1+total1+dlen2+
///                        text233 = 241 B; phone-to-device summary, shaped like 0x08,
///                        total=1 in phase 1, multi-card via idx/total in phase 2, no frame change.
///                      · TTS_DONE (CTRL 0x0B): op1+page_no2+status1 = 4 B;
///                        status 0=finished/1=user abort/2=failed; the device decides
///                        whether to consume it from its NVS setting.
///                      · Page reconciliation: any PAGE_BEGIN/SUMMARY/TTS_DONE whose
///                        page_no differs from the device's current page is dropped.
///                      · Framing (sentinel ruling 2026-11-10 19:03, plan B / bare-frame
///                        alignment): all four frames are bare op-first frames per the
///                        frozen table. CTRL-direction gen30 frames get a whitelist
///                        dispatch - only a first byte in {0x0A,0x0B} with an exact
///                        241/4 B length takes the bare path; everything else goes through
///                        the legacy 4 B magic check, failing to ERR(2). Legacy frames
///                        start with 0x50, disjoint from the whitelist, so dispatch is
///                        deterministic. The 244 B wire cap (MTU 247 - 3 attribute
///                        header) admits the 241 B bare frame: zero reinterpret, no
///                        change to R0 field arithmetic or its _Static_asserts.
///                      · All existing frozen frames (0x19 etc.) stay byte-unchanged;
///                        unregistered ops still take default -> ERR(2), and 0x0A/0x0B
///                        are registered hence no longer "unregistered". Java/Python
///                        references sync constants and bare-frame codec now; the APK
///                        send-side codec bodies belong to the APK work package (parallel).
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
    PUSH_OP_SUMMARY = 0x0A,        ///< gen30 伴读：手机→设备摘要回包（241B 定长，同构 0x08 分帧）/
                                  ///< gen30: phone-to-device AI summary (fixed 241 B)
    PUSH_OP_TTS_DONE = 0x0B,       ///< gen30 伴读：手机→设备播完回执（4B）/
                                  ///< gen30: phone-to-device playback-done receipt (4 B)
    PUSH_OP_RESERVED_FIRST = 0x0C, ///< gen30 顺移：0x0A/0x0B 已由伴读案启用，预留区从 0x0C 起 /
                                  ///< gen30: 0x0A/0x0B taken by read-along, reservation now starts at 0x0C
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
    PUSH_ST_TTS_TEXT = 0x19, ///< gen26.5：设备→手机，当前页纯文本（TTS 听书，跟随模式）/ device to phone: page text for read-aloud
    PUSH_ST_AI_SCENE = 0x1A, ///< M3：设备→手机，情景释义请求（词 + 上下文 + 词偏移，分片）/ device to phone: scene request (word + context + offset, segmented)
    PUSH_ST_MODE_SET = 0x1B,  ///< gen30 伴读：设备→手机，模式广播（5B）/ device to phone: mode broadcast (5 B)
    PUSH_ST_PAGE_BEGIN = 0x1C, ///< gen30 伴读：设备→手机，页号对账前置帧（5B）/ device to phone: page-begin marker (5 B)
    PUSH_ST_TTS_ENABLE = 0x1D, ///< gen30 伴读修复轮：设备→手机，朗读使能（2B：op+使能位）/ device to phone: read-aloud enable (2 B)
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
#define PUSH_AI_WORD_MAX 64u ///< 单词上限 / word cap

// 26.11.3-C2（B 路线拆槽）：缓冲尺寸必须跟着它装的**东西**走，不跟最大的那个家伙走。
// 26.11.3-D 之前只有一个 DEF_MAX，于是「结果槽 / 卡片缓存 / 弹卡段缓冲 / 生词批槽」
// 四类语义完全不同的缓冲共用一个数——把段落抬到 8192 就等于让 8 条词义缓存各白占
// 7 KB（55,936 B，见交付件内存账）。拆成两条之后每一条都有自己的上限，谁也不替谁
// 说谎：纸条尺寸 = 它要装的内容的上界。
//
// 26.11.3-C2 (the B-route slot split): a buffer is sized by what it HOLDS, not by the
// largest of its neighbours. Before this there was one DEF_MAX and four semantically
// different buffers shared it, so widening passages to 8192 would have made eight
// word-definition cache entries each waste 7 KB. Two budgets now: each buffer states
// its own ceiling and nobody lies for anybody else.
#define PUSH_AI_CARD_MAX 1200u ///< 词义路径上限：卡片 / 预取缓存 / 弹卡段缓冲 / 生词批槽，承接 APK 1152B 帽 / word-path cap (cards, cache, popup segments, vocab batch)
#define PUSH_AI_DEF_MAX  8192u ///< 传输 / 显示槽：段落解析（bit2）也走这条槽，8192 ≈ 2,700 汉字，同时是 0x08 重组器与 ai_reader 的共同上界 / transport and display slot, passages included

// gen26.6.4 契约（编译期钉死）：词义路径必须容下 1152B 的 APK 帽余量，缩回旧的 768 会让
// 新预算下的长释义在协议层被截断。任何缩槽改动在此编译失败——常量族的有牙护栏。
// gen26.6.4 contract (pinned at compile time): the word path must hold the APK's 1152B
// cap; shrinking back to 768 truncates the new budget at the protocol layer.
_Static_assert(PUSH_AI_CARD_MAX >= 1200u,
               "gen26.6.4: PUSH_AI_CARD_MAX must stay >= 1200 (APK 1152B cap)");
// 26.11.3-C2：槽 ≥ 卡片是**语义**要求（词义也要走同一条结果槽），不是余量裕度。
// 26.11.3-C2: slot >= card is a semantic requirement (word answers ride the same slot),
// not a headroom figure.
_Static_assert(PUSH_AI_DEF_MAX >= PUSH_AI_CARD_MAX,
               "26.11.3-C2: the result slot must hold a card-sized answer too");

/// AI_QUERY（0x06）CTRL 载荷：手机→设备，触发离线队列回流或即时查询。
/// AI_QUERY (0x06) CTRL payload: phone to device, flush the offline queue or ask now.
typedef struct {
    char word[PUSH_AI_WORD_MAX]; ///< 待查单词（UTF-8，NUL 结尾）/ the word, NUL-terminated
} push_ai_query_t;

/// AI_RESULT（0x07）CTRL 载荷：手机→设备，单词 + 释义。/ AI_RESULT (0x07): word + definition.
typedef struct {
    char word[PUSH_AI_WORD_MAX]; ///< 单词 / the word
    // 26.11.3-C2：0x07 **只载线格式**（def ≤169B，见 PUSH_AI_DEF_WIRE_MAX），本槽因此
    // 按词义预算尺寸化、不跟传输槽膨胀——8192 会让这具静态解码目标白占 7 KB。
    // 26.11.3-C2: the 0x07 frame carries wire-format text only (def <= 169B), so this
    // slot is sized by the word-path budget and does NOT follow the transport slot -
    // doing so would waste 7 KB on a static decode target.
    char def[PUSH_AI_CARD_MAX]; ///< 释义文本（UTF-8，NUL 结尾）/ the definition, NUL-terminated
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

/// **0x07 线格式（冻结）**：def 槽 170 字节（169 + NUL），整帧恒为 241 B。
/// 它跟随的是 **APK 侧 `PushProto.AI_DEF_MAX` / `AI_RESULT_MAX`**，
/// **绝不跟随 `PUSH_AI_DEF_MAX`**——后者是设备端缓冲容量（D3 后 768）。
/// The 0x07 WIRE FORMAT (FROZEN): a 170-byte def slot (169 + NUL), always a 241B
/// frame. It tracks the APK's `PushProto.AI_DEF_MAX` / `AI_RESULT_MAX`, **never**
/// `PUSH_AI_DEF_MAX` - the latter is the device-side buffer capacity (768 since D3).
///
/// gen26.6.1 D-1（真机自擒）：D3 把 `PUSH_AI_DEF_MAX` 170→768 后，原
/// `PUSH_AI_RESULT_MAX`（随它膨胀到 839）被 decode 守卫当作**最小长度**——
/// APK 恒发的 241B 冻结帧于是 `241 < 839` 恒拒，短释义（≤169B，走 0x07 直发）
/// 全灭、长释义（走 0x08）正常。根因是「容量常量」与「线格式常量」被 DEF_MAX
/// 同化。自此两链分家：容量链管缓冲（SEG ≥ DEF_MAX ≥ 显示容量），线格式链管协议
/// （WIRE_LEN = 241，静态断言钉死）。`PUSH_AI_RESULT_MAX` 随之删除——它正是
/// 那个把最大当最小用的陷阱宏。
/// gen26.6.1 D-1 (caught on hardware): after D3 widened `PUSH_AI_DEF_MAX` to 768,
/// `PUSH_AI_RESULT_MAX` (which grew with it to 839) was used by the decode guard as
/// a MINIMUM length, so the APK's constant 241B frozen frame failed `241 < 839` and
/// every short definition (<=169B, the 0x07 direct path) was rejected while long
/// ones (0x08) worked. Root cause: a capacity constant and a wire-format constant
/// assimilated by DEF_MAX. The two chains are now separate: the capacity chain
/// governs buffers (SEG >= DEF_MAX >= display capacity), the wire chain governs the
/// protocol (WIRE_LEN = 241, pinned by static assert). `PUSH_AI_RESULT_MAX` is
/// deleted - it was the very macro used as a minimum.
#define PUSH_AI_DEF_WIRE_MAX 170u
#define PUSH_AI_RESULT_WIRE_LEN \
    (PUSH_CTRL_HDR_LEN + 1u + PUSH_AI_WORD_MAX + 2u + PUSH_AI_DEF_WIRE_MAX) // = 241
_Static_assert(PUSH_AI_RESULT_WIRE_LEN == 241u,
               "0x07 wire frame is frozen at 241B (tracks the APK, NOT PUSH_AI_DEF_MAX)");
_Static_assert(PUSH_AI_DEF_WIRE_MAX == 170u,
               "0x07 wire def slot is frozen at 170B (169 + NUL)");
// 容量必须容得下一条线格式释义（两链唯一合法的交汇点：容量 ≥ 线格式）。
// 26.11.3-C2：交汇点钉在**词义路径**上——0x07 的解码目标就是 push_ai_result_t，
// 它装的东西恒为线格式，故这一条对着 CARD 断言，不对着传输槽。
// The only legal contact between the chains: capacity must hold a wire-sized def.
// 26.11.3-C2: the contact is pinned at the WORD path - the 0x07 decode target is
// push_ai_result_t and it only ever holds wire-format text, so this asserts CARD.
_Static_assert(PUSH_AI_CARD_MAX >= PUSH_AI_DEF_WIRE_MAX,
               "gen26.6.1: the card slot must hold a wire-sized definition");

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

/* ---- TTS 页文本分帧（gen26.5，STATUS 0x19）---- */
/* TTS page-text segments (gen26.5, STATUS 0x19). */

#define PUSH_TTS_TEXT_MAX 234u ///< 页文本槽（UTF-8 字节）：241 - op1 - seq2 - total2 - len2 / page-text slot

/// TTS_TEXT（0x19）STATUS 载荷：当前页纯文本（可分片）。
/// TTS_TEXT (0x19) STATUS payload: the current page's plain text (segmentable).
typedef struct {
    uint16_t seq;                   ///< 本帧序号，从 0 起 / this frame's index, 0-based
    uint16_t total;                 ///< 总帧数；==1 即整页一帧 / total frames; 1 = one frame per page
    uint16_t len;                   ///< 本帧 text 有效字节数（≤ PUSH_TTS_TEXT_MAX-1）/ valid bytes
    char text[PUSH_TTS_TEXT_MAX];   ///< 本帧文本片段（UTF-8，边界由编码方保证）/ this frame's slice
} push_tts_text_t;

/// STATUS 0x19 定长帧（无 CTRL 头）：= 241，落在 notify 帽 244 内。
/// Fixed STATUS 0x19 frame (no CTRL header): 241, inside the 244 notify cap.
#define PUSH_ST_TTS_TEXT_FRAME_MAX (1u + 2u + 2u + 2u + PUSH_TTS_TEXT_MAX) ///< = 241

/// gen26.5 编 STATUS 0x19 页文本（241B 零填充定长）。/ Encode a STATUS 0x19 page text (241B).
size_t push_st_tts_text_encode(const push_tts_text_t *t, uint8_t *out, size_t cap);
/// gen26.5 解 STATUS 0x19（241B 定长；校验 seq < total、total ≥ 1、len < 槽）。
/// Decode STATUS 0x19 (fixed 241B; validates seq < total, total >= 1, len < slot).
bool push_st_tts_text_decode(const uint8_t *in, size_t len, push_tts_text_t *t);

/* ---- 情景释义上下文分帧（M3，STATUS 0x1A）---- */
/* Scene-lookup context segments (M3, STATUS 0x1A). */

/// 上下文纯文本上限（UTF-8 字节，本页 ±1 页、词居中）。/ Context cap in UTF-8 bytes.
#define PUSH_SCENE_CTX_MAX 1024u
/// 被点词上限（与 PUSH_AI_WORD_MAX 同源）。/ Tapped word cap (same as PUSH_AI_WORD_MAX).
#define PUSH_SCENE_WORD_MAX 64u
/// 本帧上下文槽：241 - op1 - seq2 - total2 - len2。/ Per-frame context slot.
#define PUSH_ST_SCENE_SLOT 234u
/// 首帧元数据：word_len(1) + word(≤64) + word_off(2) + flags(1)。/ First-frame metadata.
#define PUSH_ST_SCENE_META_MAX (1u + PUSH_SCENE_WORD_MAX + 2u + 1u) ///< = 68
/// STATUS 0x1A 定长帧：= 241，落在 notify 244 帽内（与 0x19 同构）。/ Fixed 0x1A frame.
#define PUSH_ST_SCENE_FRAME_MAX (1u + 2u + 2u + 2u + PUSH_ST_SCENE_SLOT) ///< = 241
/// 帧数上界：首帧最坏 166B + 4×234B = 1102 ≥ 1024。/ Frame upper bound.
#define PUSH_SCENE_MAX_FRAMES 5u

/// flags 位。/ Flag bits.
#define PUSH_SCENE_FLAG_CTX_TRUNCATED  (1u << 0) ///< 上下文被截断（1024B 协议上限或发送侧缓冲）/ context truncated (the 1024 B cap or the sender's buffer)
#define PUSH_SCENE_FLAG_SENTENCE_FALLBACK (1u << 1) ///< 句界未找到、退回页界 / sentence boundary fell back to the page edge
/// 26.9.26 M5-A S26-3：整段摘抄解析。APK 见 bit2 时把 word 当「首词锚点」、ctx 当
/// 「整段待解析文本」，走整段解析而不是单词情景卡。登记与联调凭证见 docs/PROTOCOL.md。
/// Excerpt analysis: with bit2 the APK treats word as the leading-word anchor and ctx as
/// the whole passage. Registry and interop credentials live in docs/PROTOCOL.md.
#define PUSH_SCENE_FLAG_EXCERPT        (1u << 2)

/// 0x1A 单帧载荷（解码结果）。首帧（seq==0）带 word/word_off/flags；后续帧三者为零。
/// Decoded 0x1A frame. The first frame (seq==0) carries word/word_off/flags;
/// later frames leave those three zeroed.
typedef struct {
    uint16_t seq;                    ///< 本帧序号，从 0 起 / this frame's index, 0-based
    uint16_t total;                  ///< 总帧数 / total frames
    uint16_t len;                    ///< 本帧 ctx 有效字节 / valid context bytes
    uint8_t word_len;                ///< 首帧词长，后续帧为 0 / word length (first frame only)
    char word[PUSH_SCENE_WORD_MAX];  ///< 首帧词（无 NUL 语义，按 word_len 取）/ word (first frame)
    uint16_t word_off;               ///< 词在完整 ctx 内的字节偏移 / word offset inside the full context
    uint8_t flags;                   ///< PUSH_SCENE_FLAG_* / flag bits
    char ctx[PUSH_ST_SCENE_SLOT];    ///< 本帧上下文片段 / this frame's context slice
} push_scene_frame_t;

_Static_assert(PUSH_ST_SCENE_FRAME_MAX == 241u, "0x1A frame must stay 241B");
_Static_assert(PUSH_ST_SCENE_META_MAX <= PUSH_ST_SCENE_SLOT, "first-frame meta must fit the slot");
_Static_assert(PUSH_SCENE_CTX_MAX <= PUSH_ST_SCENE_SLOT * PUSH_SCENE_MAX_FRAMES - PUSH_ST_SCENE_META_MAX,
               "5 frames must carry the full 1024B context");

/// 编 STATUS 0x1A（241B 零填充定长）。slice 为本帧 ctx 片段，slice_len ≤ 234-meta（首帧）。
/// Encode a STATUS 0x1A (fixed 241B, zero-padded).
size_t push_st_scene_encode(const char *word, uint16_t word_off, uint8_t flags,
                            uint16_t seq, uint16_t total,
                            const char *slice, size_t slice_len,
                            uint8_t *out, size_t cap);
/// 解 STATUS 0x1A（241B 定长；校验 seq < total、total ≤ PUSH_SCENE_MAX_FRAMES、len ≤ 槽）。
/// Decode a STATUS 0x1A (fixed 241B; validates seq/total/len).
bool push_st_scene_decode(const uint8_t *in, size_t len, push_scene_frame_t *f);

/// 分片计划：给定词与完整 context，返回需要的帧数（1..PUSH_SCENE_MAX_FRAMES）。
/// 首帧预算 = 234 - (4 + word_len)，后续帧预算 = 234；按 UTF-8 字符边界切分。
/// Frame plan: frames needed for word+context; the first frame's budget is
/// 234-(4+word_len), later frames get 234. Slices land on UTF-8 boundaries.
uint16_t push_scene_frame_count(const char *word, const char *ctx, size_t ctx_len);
/// 取第 i 帧的 ctx 切片；i 越界或参数非法返回 false。
/// Fetch frame i's context slice; false when i is out of range.
bool push_scene_frame_at(const char *word, const char *ctx, size_t ctx_len, uint16_t i,
                         const char **slice, size_t *slice_len);

/* ---- 能力位图（gen26 能力协商）---- */
/* Capability bitmap (gen26 capability negotiation). */

typedef struct {
    uint32_t bits; ///< 能力位掩码 / capability bitmask
} push_caps_t;

#define PUSH_CAP_AI_QUERY  (1u << 0) ///< 设备支持 AI 查词（0x06/0x07 + 0x17）/ device supports AI lookup
#define PUSH_CAP_AI_CONTEXT (1u << 1) ///< gen26.5：0x17/0x06 带书标识+位置上下文 / gen26.5: contextual 0x17/0x06
#define PUSH_CAP_AI_MULTI   (1u << 2) ///< gen26.5：0x08 释义分帧 / gen26.5: segmented definitions (0x08)
#define PUSH_CAP_AI_SCENE_VIEW (1u << 4) ///< M3-B：APK 能出四节情景分析（回答 0x1A）；未置位设备不发 0x1A，入口也不显示 / M3-B: APK answers 0x1A with four sections; the device stays silent without it
#define PUSH_CAP_AI_SCENE   (1u << 3) ///< M3：APK 支持情景释义（0x1A 上下文 + 情景提示词）；未置位设备不发 0x1A / M3: APK understands scene lookup (0x1A); device stays silent without it

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

/* ---- gen30 伴读：CTRL 裸帧白名单分派（哨位裁定方案 B）---- */
/* Read-along gen30: bare CTRL frame whitelist dispatch (sentinel ruling, plan B). */

/// 仅当首字节为已登记的 gen30 CTRL opcode（0x0A SUMMARY / 0x0B TTS_DONE）且总长
/// 恰等于该帧定长（241 / 4 B）时返回 true 并交出 op；否则一律 false，由调用方继续
/// 走现制 4 B 公共头校验（失败→ERR(2)）。纯函数、无状态，宿主可逐字节断言。
/// 与现制帧首字节 0x50 零交集，故分派确定性成立：既有 CTRL 路径逐字不变。
/// Returns true only for a registered gen30 CTRL opcode (0x0A / 0x0B) whose total
/// length equals that frame's fixed length (241 / 4 B); everything else returns
/// false and the caller falls through to the legacy 4 B header check (ERR(2) on
/// failure). Pure and stateless for host assertions. Disjoint from the legacy 0x50
/// first byte, so dispatch is deterministic and the existing CTRL path stays intact.
bool push_gen30_ctrl_bare_op(const uint8_t *in, size_t len, uint8_t *op);

/* ---- STATUS 编解码 / STATUS codec ---- */

#define PUSH_STATUS_MAX 16u ///< STATUS 帧上限 / STATUS frame cap

/// 编码一条 STATUS：opcode + 至多 12 字节 body，返回帧长（0 表示 cap 不够）。
/// Encode one STATUS: opcode plus up to 12 body bytes; returns frame length (0 when out of cap).
size_t push_status_encode(uint8_t opcode, const uint8_t *body, size_t body_len, uint8_t *out, size_t cap);

/* ---- 伴读案帧（gen30，R0 冻结件 1；登记层常量）---- */
/* Read-along companion frames (gen30, R0 frozen item 1; registry-layer constants). */
/*
 * 本节登记 R0 冻结的「opcode 编号 + 定长 + 字段宽度 + 枚举值」，全部为编译期常量。
 * 步 1 为纯登记层；步 3 起按哨位裁定方案 B 启用：四帧一律 op 首字节裸帧，
 * CTRL 方向由 push_gen30_ctrl_bare_op() 白名单分派（见下方声明与 push_proto.c），
 * 现制 4 B 公共头路径一字节不动。封帧口径已裁定，不再属「未定死」项。
 *
 * This section registers the frozen opcode numbers, frame lengths, field widths
 * and enum values as compile-time constants. Step 1 was registry-only; from step 3
 * plan B (sentinel ruling) is in force: all four are bare op-first frames and the
 * CTRL direction is dispatched by the push_gen30_ctrl_bare_op() whitelist (see the
 * declaration below and push_proto.c). The legacy 4 B header path is byte-unchanged.
 */

/// 四模式 Prompt 编号（设备只传编号；模板与确定性截断全在 APK 端）。/ Four prompt mode ids.
enum {
    PUSH_RIDE_MODE_FOREIGN = 0,  ///< 外语原著（默认）/ foreign original (default)
    PUSH_RIDE_MODE_NOVEL = 1,    ///< 小说网文 / web novel
    PUSH_RIDE_MODE_SOCIAL = 2,   ///< 社科非虚构 / social non-fiction
    PUSH_RIDE_MODE_CLASSIC = 3,  ///< 古文诗词 / classical poetry
    PUSH_RIDE_MODE_COUNT = 4,    ///< 初始模式集大小 / initial mode-set size
};

/// MODE_SET.source 来源。/ MODE_SET source.
enum {
    PUSH_MODE_SRC_DEVICE = 0,    ///< 设备菜单切换 / device menu
    /* 1 = APK 端预留，一期不启用 / 1 reserved for APK, unused in phase 1 */
    PUSH_MODE_SRC_INIT = 2,      ///< 连接后初始化同步 / post-connect init sync
};

/// TTS_DONE.status 播完状态。/ TTS_DONE status.
enum {
    PUSH_TTS_DONE_FINISHED = 0,  ///< 播完 / finished
    PUSH_TTS_DONE_ABORT = 1,     ///< 用户中断 / user aborted
    PUSH_TTS_DONE_FAILED = 2,    ///< 失败 / failed
};

/// 四帧定长（R0 冻结表，op 首字节起算）。/ Four fixed frame lengths (op-first, frozen R0).
#define PUSH_ST_MODE_SET_LEN   5u    ///< op1+mode1+source1+reserved2
#define PUSH_ST_PAGE_BEGIN_LEN 5u    ///< op1+page_no2(LE)+mode1+reserved1
#define PUSH_ST_TTS_ENABLE_LEN 2u    ///< op1+enable1（修复轮解耦）/ decoupling enable bit
#define PUSH_OP_SUMMARY_LEN    241u  ///< op1+mode1+page_no2+idx1+total1+dlen2+text233
#define PUSH_OP_TTS_DONE_LEN   4u    ///< op1+page_no2+status1

/// SUMMARY 文本槽（UTF-8 字节）/ SUMMARY text slot (UTF-8 bytes).
#define PUSH_SUMMARY_TEXT_MAX 233u
/// 多帧摘要拼装上限（修复轮三轮：250 字帽）。250 个汉字约 750 B，分四帧；
/// 取 1024 B 给英文、标点与 emoji 留余量，设备侧拼接缓冲因此是 1025 B（PSRAM 静态）。
/// 分片语义：同一 page_no 下 idx 从 0 连续递增，收齐 idx+1==total 才交付；
/// 乱序或缺片一律丢弃，等下一次翻页重新开始（不猜、不补、不显示半句）。
/// Multi-frame summary assembly cap (round 3: 250-char cap). 250 CJK chars ≈ 750 B
/// over four frames; 1024 B leaves room for Latin text, punctuation and emoji.
/// Sharding: under one page_no, idx increments from 0 and only idx+1==total
/// delivers; out-of-order or missing shards are dropped wholesale.
#define PUSH_SUMMARY_TOTAL_MAX 1024u
/// 页号字段为 u16 LE / page_no is a little-endian u16.
#define PUSH_PAGE_NO_BYTES 2u
/// CTRL 写属性 wire 帽：MTU 247 − 3 B 写属性头 = 244 B（哨位裁定方案 B 的算术依据；
/// 方案 A 的 4B 头 + 241B = 245B 正由此超帽 1B）。/ CTRL write wire cap.
#define PUSH_GEN30_WIRE_CAP 244u

/* 冻结表算术自洽（编译期钉死，防登记漂移）/ Frozen-table arithmetic pinned at compile time. */
_Static_assert(1u + 1u + 1u + 2u == PUSH_ST_MODE_SET_LEN, "gen30 MODE_SET must stay 5 B");
_Static_assert(1u + PUSH_PAGE_NO_BYTES + 1u + 1u == PUSH_ST_PAGE_BEGIN_LEN, "gen30 PAGE_BEGIN must stay 5 B");
_Static_assert(1u + 1u == PUSH_ST_TTS_ENABLE_LEN, "gen30 TTS_ENABLE must stay 2 B");
_Static_assert(1u + 1u + PUSH_PAGE_NO_BYTES + 1u + 1u + 2u + PUSH_SUMMARY_TEXT_MAX == PUSH_OP_SUMMARY_LEN,
               "gen30 SUMMARY fields must sum to 241 B");
_Static_assert(1u + PUSH_PAGE_NO_BYTES + 1u == PUSH_OP_TTS_DONE_LEN, "gen30 TTS_DONE must stay 4 B");
_Static_assert(PUSH_RIDE_MODE_COUNT == 4u, "gen30 initial mode set is four");
/* 预留区顺移零空洞、STATUS 段零重叠；新帧编号紧接既有帧。 */
_Static_assert(PUSH_OP_SUMMARY == PUSH_OP_AI_CAPS_ENABLE + 1, "gen30 SUMMARY 0x0A follows 0x09");
_Static_assert(PUSH_OP_TTS_DONE == PUSH_OP_SUMMARY + 1, "gen30 TTS_DONE 0x0B follows 0x0A");
_Static_assert(PUSH_OP_RESERVED_FIRST == PUSH_OP_TTS_DONE + 1, "gen30 reserved block starts at 0x0C");
/* 两侧分属 CTRL/STATUS 两个匿名 enum，显式转整数比较，避开 -Wenum-compare。 */
_Static_assert((int)PUSH_OP_RESERVED_LAST < (int)PUSH_ST_READY, "gen30 CTRL reserved block must not overlap STATUS");
_Static_assert(PUSH_ST_MODE_SET == PUSH_ST_AI_SCENE + 1, "gen30 STATUS 0x1B follows 0x1A");
_Static_assert(PUSH_ST_PAGE_BEGIN == PUSH_ST_MODE_SET + 1, "gen30 STATUS 0x1C follows 0x1B");
/* 修复轮：朗读使能紧跟页号对账帧，顺移零空洞（0x1D）。 */
_Static_assert(PUSH_ST_TTS_ENABLE == PUSH_ST_PAGE_BEGIN + 1, "gen30 STATUS 0x1D follows 0x1C");
/* 方案 B 白名单确定性（哨位裁定生效条件 1/3）：现制魔数与两枚裸帧 op 零碰撞、
 * 裸帧不超 wire 帽。两条都钉在编译期，宿主侧另有运行时长度闸门断言。 */
_Static_assert(PUSH_MAGIC0 != PUSH_OP_SUMMARY && PUSH_MAGIC0 != PUSH_OP_TTS_DONE,
               "gen30 bare CTRL first byte must never collide with legacy magic0");
_Static_assert(PUSH_MAGIC1 != PUSH_OP_SUMMARY && PUSH_MAGIC1 != PUSH_OP_TTS_DONE,
               "gen30 bare CTRL first byte must never collide with legacy magic1");
_Static_assert(PUSH_OP_SUMMARY_LEN <= PUSH_GEN30_WIRE_CAP &&
               PUSH_OP_TTS_DONE_LEN <= PUSH_GEN30_WIRE_CAP,
               "gen30 bare CTRL frames must fit the 244 B wire cap");

#ifdef __cplusplus
}
#endif
