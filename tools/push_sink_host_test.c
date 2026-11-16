/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：push_sink 纯核（帧编解码 + 会话状态机）的宿主回归。断言有牙：坏魔数、
 * 坏 CRC、seq 跳号、total_len 超限、未武装写入、token 不符、拔电退出全部必须红。
 * English: host regression for the push_sink pure core (frame codec + session
 * machine). The assertions bite: bad magic, bad CRC, seq gaps, oversized
 * total_len, unarmed writes, token mismatch and unplug must all be refused.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "push_part.h"

#include "push_proto.h"
#include "push_session.h"
#include "ai_ridealong.h"
#include "ai_segasm.h"
#include "push_tts.h"
// gen26.6.1 D-1：APK 真实线格式帧 fixture（tools/gen_apk_frame_fixture.sh 由
// Java PushProto 现产；缺失即编译失败——没有 fixture 的协议往返不做数）。
// gen26.6.1 D-1: the APK's real wire-frame fixture (generated from the Java
// PushProto by tools/gen_apk_frame_fixture.sh; a missing fixture fails the build -
// a protocol round-trip without the peer's frame does not count).
#include "apk_ai_result_frame.h"

static int s_checks;
static int s_red;
// 当前用例号：变异轮要能指名「红落在哪一条」，而不是只知道"某条红了"。
// The running case id, so a mutation round can name WHICH case went red instead
// of only knowing that something did.
static const char *s_case = "-";

#define CHECK(cond)                                                      \
    do {                                                                 \
        ++s_checks;                                                      \
        if (!(cond)) {                                                   \
            ++s_red;                                                     \
            printf("RED [%s] line %d: %s\n", s_case, __LINE__, #cond);    \
        }                                                                \
    } while (0)

static push_session_t s_sess;
static push_event_t s_evt;

// 喂事件的小助手：清零参数结构只填需要的字段。/ Event helper: clear the struct, fill only what is needed.
static uint32_t feed(push_evt_t evt) {
    memset(&s_evt, 0, sizeof(s_evt));
    return push_session_event(&s_sess, evt, &s_evt);
}

static uint32_t feed_arm(uint16_t token, uint32_t window_ms, uint32_t cooldown_ms, uint32_t now_ms) {
    memset(&s_evt, 0, sizeof(s_evt));
    s_evt.token = token;
    s_evt.window_ms = window_ms;
    s_evt.cooldown_ms = cooldown_ms;
    s_evt.now_ms = now_ms;
    return push_session_event(&s_sess, PUSH_EVT_ARM, &s_evt);
}

static uint32_t feed_tick(uint32_t now_ms) {
    memset(&s_evt, 0, sizeof(s_evt));
    s_evt.now_ms = now_ms;
    return push_session_event(&s_sess, PUSH_EVT_TICK, &s_evt);
}

static uint32_t feed_begin(const push_begin_t *b) {
    memset(&s_evt, 0, sizeof(s_evt));
    s_evt.begin = b;
    return push_session_event(&s_sess, PUSH_EVT_BEGIN, &s_evt);
}

static uint32_t feed_data(uint16_t seq, size_t len) {
    static const uint8_t payload[PUSH_DATA_PAYLOAD_MAX];
    memset(&s_evt, 0, sizeof(s_evt));
    s_evt.data = payload;
    s_evt.data_len = len;
    s_evt.seq = seq;
    return push_session_event(&s_sess, PUSH_EVT_DATA, &s_evt);
}

static uint32_t feed_verify(bool ok, uint32_t now_ms) {
    memset(&s_evt, 0, sizeof(s_evt));
    s_evt.verify_ok = ok;
    s_evt.now_ms = now_ms;
    return push_session_event(&s_sess, PUSH_EVT_VERIFY, &s_evt);
}

static push_begin_t make_begin(uint32_t total, uint16_t token) {
    push_begin_t b;
    memset(&b, 0, sizeof(b));
    b.total_len = total;
    b.crc32 = 0xdeadbeefu;
    b.session_id = 0x12345678u;
    b.token = token;
    b.type = PUSH_TYPE_TEXT;
    snprintf(b.title, sizeof(b.title), "测试长文");
    return b;
}

/* ---- 编解码 / codec ---- */

static void test_crc32(void) {
    // zlib 标准向量。/ The standard zlib vector.
    CHECK(push_crc32("123456789", 9) == 0xCBF43926u);
    CHECK(push_crc32("", 0) == 0u);
    const char *hi = "墨水屏推流";
    CHECK(push_crc32(hi, strlen(hi)) == push_crc32(hi, strlen(hi)));
    CHECK(push_crc32("a", 1) != push_crc32("b", 1));

    // 流式续算：分段喂与整块一次算必须逐位相同（512KB 级正文靠它进 ATT 缓冲）。
    // Streaming: chunked feeding must match the one-shot result bit for bit (a
    // 512 KB body relies on this to stay out of one contiguous buffer).
    static const char chunk_a[] = "ReadPico push stream ";
    static const char chunk_b[] = "serendipity 墨水屏";
    static const char chunk_c[] = "";
    const size_t la = sizeof(chunk_a) - 1, lb = sizeof(chunk_b) - 1;
    uint8_t whole[256];
    memcpy(whole, chunk_a, la);
    memcpy(whole + la, chunk_b, lb);
    const uint32_t one = push_crc32(whole, la + lb);
    const uint32_t chained = push_crc32_continue(push_crc32_continue(push_crc32_continue(0u, chunk_a, la), chunk_b, lb), chunk_c, 0);
    CHECK(chained == one);
    // 空段直通、首段传 0 等价一次性。/ Empty chunks pass through; a leading 0 equals the one-shot.
    CHECK(push_crc32_continue(0u, whole, la + lb) == one);
    CHECK(push_crc32_continue(0u, chunk_c, 0) == 0u);
    CHECK(push_crc32_continue(one, chunk_c, 0) == one);
    printf("crc32: vectors + streaming chain ok\n");
}

static void test_ctrl_header(void) {
    // END / RESUME_QUERY / ABORT 的公共头：魔数+版本+长度四项。
    // The shared header of END / RESUME_QUERY / ABORT: magic, version, length.
    const uint8_t ok_end[4] = {0x50, 0x43, PUSH_OP_END, PUSH_VERSION};
    const uint8_t ok_resume[4] = {0x50, 0x43, PUSH_OP_RESUME_QUERY, PUSH_VERSION};
    const uint8_t ok_abort[4] = {0x50, 0x43, PUSH_OP_ABORT, PUSH_VERSION};
    uint8_t op = 0xFF;
    CHECK(push_ctrl_header_decode(ok_end, sizeof(ok_end), &op) && op == PUSH_OP_END);
    CHECK(push_ctrl_header_decode(ok_resume, sizeof(ok_resume), &op) && op == PUSH_OP_RESUME_QUERY);
    CHECK(push_ctrl_header_decode(ok_abort, sizeof(ok_abort), &op) && op == PUSH_OP_ABORT);
    // 有牙：短帧/坏魔数/坏版本全拒。/ Bite: short, bad magic and bad version all refuse.
    CHECK(!push_ctrl_header_decode(ok_end, 3, &op));
    uint8_t bad[4];
    memcpy(bad, ok_end, 4); bad[0] = 0x51;
    CHECK(!push_ctrl_header_decode(bad, 4, &op));
    memcpy(bad, ok_end, 4); bad[1] = 0x44;
    CHECK(!push_ctrl_header_decode(bad, 4, &op));
    memcpy(bad, ok_end, 4); bad[3] = PUSH_VERSION + 1;
    CHECK(!push_ctrl_header_decode(bad, 4, &op));
    // gen25 预留区登记（0x06..0x0F，见 push_proto.h 变更记录）：帧头解码器必须
    // 原样放行 op（gen26 启用时不必改解码层），且预留段与 STATUS 段（0x10 起）
    // 零重叠——重叠了 gen26 第一天就会撞号。
    // Gen26 reservation registry (0x08-0x0F now; 0x06/0x07 taken by gen26, see push_proto.h): the
    // header decoder must pass the op through untouched (gen26 enables one
    // without touching the decode layer), and the reserved block must not
    // overlap the STATUS block (0x10 up) - an overlap would collide on day one.
    for (unsigned reserved = PUSH_OP_RESERVED_FIRST; reserved <= PUSH_OP_RESERVED_LAST; ++reserved) {
        const uint8_t frame[4] = {0x50, 0x43, (uint8_t)reserved, PUSH_VERSION};
        op = 0xFF;
        CHECK(push_ctrl_header_decode(frame, sizeof(frame), &op) && op == reserved);
        CHECK(reserved < PUSH_ST_READY); // 预留区与 STATUS 段零重叠 / no overlap with STATUS
    }
    CHECK(PUSH_OP_AI_QUERY == PUSH_OP_SET_TIME + 1); // 0x06 紧接 SET_TIME
    // gen26.5：0x08/0x09 由 gen26.5 启用（分帧/使能），预留区首顺移 0x0A，仍无空洞。
    // gen26.5: 0x08/0x09 taken by gen26.5 (segments/enable); the reserved block
    // now starts at 0x0A, still gap-free.
    CHECK(PUSH_OP_RESERVED_FIRST > PUSH_OP_AI_RESULT); // 0x08/0x09 已出预留区 / out of the reserved block
    // gen30 伴读案（R0 冻结件 1）：0x0A/0x0B 登记为 SUMMARY/TTS_DONE，预留区首再顺移
    // 到 0x0C；0x1B/0x1C 落在 STATUS 段。步 3 起哨位裁定方案 B 生效：CTRL 方向新帧
    // 按 op 首字节裸帧走白名单分派（见 test_gen30_bare_dispatch），帧头解码器仍原样
    // 放行 op（长度不符的畸形帧继续走魔数校验并落 ERR(2)）；未登记 op 仍 default→ERR(2)。
    // gen30 read-along (R0 frozen item 1): 0x0A/0x0B are SUMMARY/TTS_DONE and the
    // reserved block starts at 0x0C; 0x1B/0x1C live in the STATUS space. From step 3
    // plan B (sentinel ruling) applies: bare CTRL frames take a whitelist dispatch
    // (see test_gen30_bare_dispatch); the header decoder still passes the op through
    // so malformed frames fall through to the magic check and ERR(2).
    CHECK(PUSH_OP_SUMMARY == 0x0A && PUSH_OP_TTS_DONE == 0x0B);
    CHECK(PUSH_OP_RESERVED_FIRST == PUSH_OP_TTS_DONE + 1); // 0x0C 紧接 0x0B，无空洞
    CHECK(PUSH_ST_MODE_SET == 0x1B && PUSH_ST_PAGE_BEGIN == 0x1C);
    CHECK(PUSH_ST_PAGE_BEGIN >= PUSH_ST_READY); // 新 STATUS 帧在 STATUS 段内 / inside STATUS space
    printf("ctrl header: END/RESUME/ABORT decode, bad frames refuse, gen26 0x06/0x07 + gen26.5 0x08/0x09 + gen30 0x0A/0x0B registered, 0x0C-0x0F reserved, gen30 STATUS 0x1B/0x1C ok\n");
}

/* 哨位裁定方案 B 生效条件 3：魔数帧与 gen30 裸帧零碰撞 + 长度闸门。
 * 白名单只有两条（0x0A/241B、0x0B/4B）；其余（含畸形新帧）一律 false，
 * 交回现制 4 B 魔数校验 → ERR(2)。纯函数 push_gen30_ctrl_bare_op 由宿主逐字节验。
 * Sentinel ruling plan B condition 3: magic/bare zero-collision + length gate.
 * Only two whitelist entries; everything else falls through to the legacy
 * 4 B magic check and ERR(2). */
static void test_gen30_bare_dispatch(void) {
    uint8_t op = 0xFF;
    uint8_t summary[PUSH_OP_SUMMARY_LEN];
    memset(summary, 0, sizeof(summary));
    summary[0] = PUSH_OP_SUMMARY;
    uint8_t done[PUSH_OP_TTS_DONE_LEN] = {PUSH_OP_TTS_DONE, 0x34, 0x12, 0u};
    // 精确命中两条 / both entries hit exactly
    CHECK(push_gen30_ctrl_bare_op(summary, sizeof(summary), &op) && op == PUSH_OP_SUMMARY);
    CHECK(push_gen30_ctrl_bare_op(done, sizeof(done), &op) && op == PUSH_OP_TTS_DONE);
    // 长度闸门：短一字节 / 长一字节 / 空指针 / 零长度全部拒绝
    CHECK(!push_gen30_ctrl_bare_op(summary, sizeof(summary) - 1, &op));
    CHECK(!push_gen30_ctrl_bare_op(done, sizeof(done) - 1, &op));
    CHECK(!push_gen30_ctrl_bare_op(done, sizeof(done) + 1, &op));
    CHECK(!push_gen30_ctrl_bare_op(NULL, sizeof(summary), &op));
    CHECK(!push_gen30_ctrl_bare_op(summary, 0, &op));
    // 魔数零碰撞：现制 4 B 头帧（首字节 0x50）与预留区帧一律不进白名单
    const uint8_t legacy[4] = {0x50, 0x43, PUSH_OP_END, PUSH_VERSION};
    CHECK(!push_gen30_ctrl_bare_op(legacy, sizeof(legacy), &op));
    const uint8_t reserved[4] = {0x50, 0x43, PUSH_OP_RESERVED_FIRST, PUSH_VERSION};
    CHECK(!push_gen30_ctrl_bare_op(reserved, sizeof(reserved), &op));
    // 首字节是 0x0A 但长度不符 → false（长度闸门对新帧同样有牙，仍走魔数校验）
    const uint8_t short_summary[4] = {PUSH_OP_SUMMARY, 0x50, 0x43, PUSH_VERSION};
    CHECK(!push_gen30_ctrl_bare_op(short_summary, sizeof(short_summary), &op));
    // 取值级再验一遍（编译期断言在 push_proto.h，这里防的是「常量改了断言没跟上」）
    CHECK(PUSH_MAGIC0 != PUSH_OP_SUMMARY && PUSH_MAGIC0 != PUSH_OP_TTS_DONE);
    CHECK(PUSH_MAGIC1 != PUSH_OP_SUMMARY && PUSH_MAGIC1 != PUSH_OP_TTS_DONE);
    CHECK(PUSH_OP_SUMMARY_LEN <= PUSH_GEN30_WIRE_CAP);
    CHECK(PUSH_OP_TTS_DONE_LEN <= PUSH_GEN30_WIRE_CAP);
    // 方案 A 的算术反证：4 B 魔数头 + 241 B = 245 B > 244 B wire 帽（故裁定 B）
    CHECK(PUSH_CTRL_HDR_LEN + PUSH_OP_SUMMARY_LEN > PUSH_GEN30_WIRE_CAP);
    printf("gen30 bare dispatch: magic zero-collision + length gate ok\n");
}

static void test_begin_codec(void) {
    push_begin_t b = make_begin(300000, 4821);
    uint8_t frame[PUSH_BEGIN_LEN + 8];
    memset(frame, 0xAA, sizeof(frame));
    const size_t n = push_begin_encode(&b, frame, sizeof(frame));
    CHECK(n == PUSH_BEGIN_LEN);
    // 字节级布局：魔数/op/ver/type/token/total/crc/session/title 起点。
    // Byte-level layout: magic, op, ver, type, token, total, crc, session, title.
    CHECK(frame[0] == 0x50 && frame[1] == 0x43);
    CHECK(frame[2] == PUSH_OP_BEGIN);
    CHECK(frame[3] == PUSH_VERSION);
    CHECK(frame[4] == PUSH_TYPE_TEXT);
    CHECK(frame[5] == 0xD5 && frame[6] == 0x12); // token 4821 = 0x12D5 LE
    CHECK(frame[7] == 0xE0 && frame[8] == 0x93 && frame[9] == 0x04); // 300000 = 0x000493E0 LE
    CHECK(memcmp(frame + 19, "测试长文", strlen("测试长文") + 1) == 0);

    push_begin_t out;
    CHECK(push_begin_decode(frame, n, &out));
    CHECK(out.total_len == 300000 && out.crc32 == 0xdeadbeefu && out.session_id == 0x12345678u);
    CHECK(out.token == 4821 && out.type == PUSH_TYPE_TEXT);
    CHECK(strcmp(out.title, "测试长文") == 0);

    // 有牙：短缓冲/坏魔数/坏 op/坏版本全部拒收。
    // Bite: short buffer, bad magic, bad op and bad version are all refused.
    CHECK(!push_begin_decode(frame, PUSH_BEGIN_LEN - 1, &out));
    uint8_t bad[PUSH_BEGIN_LEN];
    memcpy(bad, frame, PUSH_BEGIN_LEN);
    bad[0] = 0x51;
    CHECK(!push_begin_decode(bad, PUSH_BEGIN_LEN, &out));
    memcpy(bad, frame, PUSH_BEGIN_LEN);
    bad[2] = PUSH_OP_END;
    CHECK(!push_begin_decode(bad, PUSH_BEGIN_LEN, &out));
    memcpy(bad, frame, PUSH_BEGIN_LEN);
    bad[3] = 9;
    CHECK(!push_begin_decode(bad, PUSH_BEGIN_LEN, &out));
    CHECK(!push_begin_encode(&b, frame, PUSH_BEGIN_LEN - 1));
    printf("begin codec: round-trip + byte layout + refusals ok\n");
}

static void test_data_status_codec(void) {
    uint8_t frame[8] = {0x07, 0x00, 0x03, 'a', 'b', 'c', 0, 0};
    uint16_t seq = 0;
    const uint8_t *payload = NULL;
    size_t len = 0;
    CHECK(push_data_decode(frame, sizeof(frame), &seq, &payload, &len));
    CHECK(seq == 7 && len == 3 && payload == frame + 3);
    CHECK(payload[0] == 'a' && payload[2] == 'c');
    // len 字段超过缓冲：拒。/ A len field beyond the buffer is refused.
    CHECK(!push_data_decode(frame, 5, &seq, &payload, &len));
    CHECK(!push_data_decode(frame, 2, &seq, &payload, &len));

    uint8_t out[PUSH_STATUS_MAX];
    CHECK(push_status_encode(PUSH_ST_READY, NULL, 0, out, sizeof(out)) == 1 && out[0] == PUSH_ST_READY);
    const uint8_t body[4] = {1, 2, 3, 4};
    CHECK(push_status_encode(PUSH_ST_ERR, body, 4, out, sizeof(out)) == 5);
    CHECK(out[0] == PUSH_ST_ERR && out[4] == 4);
    CHECK(push_status_encode(PUSH_ST_ERR, body, 13, out, sizeof(out)) == 0); // body 超长 / body too long
    CHECK(push_status_encode(PUSH_ST_ERR, body, 4, out, 4) == 0);            // cap 不够 / cap too small
    printf("data/status codec ok\n");
}

/* ---- 状态机 / state machine ---- */

static void test_happy_path(void) {
    push_session_init(&s_sess);
    CHECK(feed_arm(4821, 1800000, 60000, 1000) == PUSH_ACT_ADV_ON);
    CHECK(s_sess.state == PUSH_STATE_ARMED);
    CHECK(feed(PUSH_EVT_ADV_UP) == 0);
    CHECK(s_sess.state == PUSH_STATE_WINDOW);
    CHECK(feed(PUSH_EVT_CONNECT) == 0);
    CHECK(s_sess.state == PUSH_STATE_CONNECTED);

    push_begin_t b = make_begin(600, 4821);
    CHECK(feed_begin(&b) == PUSH_ACT_READY);
    CHECK(s_sess.state == PUSH_STATE_RECEIVING);
    CHECK(s_sess.received == 0 && s_sess.next_seq == 0);

    CHECK(feed_data(0, 241) == (PUSH_ACT_APPEND | PUSH_ACT_PROGRESS));
    CHECK(s_sess.received == 241 && s_sess.next_seq == 1);
    CHECK(feed_data(1, 241) == (PUSH_ACT_APPEND | PUSH_ACT_PROGRESS));
    CHECK(feed_data(2, 118) == (PUSH_ACT_APPEND | PUSH_ACT_PROGRESS));
    CHECK(s_sess.received == 600);

    CHECK(feed(PUSH_EVT_END) == PUSH_ACT_VERIFY);
    CHECK(s_sess.state == PUSH_STATE_PROCESSING);
    CHECK(feed_verify(true, 5000) == (PUSH_ACT_COMMIT | PUSH_ACT_OPEN_BOOK));
    CHECK(s_sess.state == PUSH_STATE_COOLDOWN);
    // 宽限时长随 ARM 注入（不再由外部手改簿记）。
    // The grace length rides in with ARM (no more poking the bookkeeping by hand).
    CHECK(s_sess.cooldown_ms == 60000 && s_sess.cooldown_at_ms == 5000);

    // 宽限里手机断连：回窗口继续广播，簿记保留可再推。
    // A disconnect during grace: back to the window, bookkeeping survives.
    CHECK(feed(PUSH_EVT_DISCONNECT) == 0);
    CHECK(s_sess.state == PUSH_STATE_WINDOW && s_sess.received == 600 && s_sess.has_part);

    // 宽限到期回窗口；窗口到期收摊。/ Grace expiring returns to the window; an expired window closes.
    CHECK(feed_tick(5000 + 60000) == 0);
    CHECK(s_sess.state == PUSH_STATE_WINDOW);
    CHECK(feed_tick(1000 + 1800000) == PUSH_ACT_ADV_OFF);
    CHECK(s_sess.state == PUSH_STATE_IDLE);
    printf("happy path: IDLE->ARMED->WINDOW->CONNECTED->RECEIVING->PROCESSING->COOLDOWN->WINDOW->IDLE ok\n");
}

static void test_word_card(void) {
    push_session_init(&s_sess);
    feed_arm(777, 1800000, 0, 0);
    feed(PUSH_EVT_ADV_UP);
    feed(PUSH_EVT_CONNECT);
    push_begin_t b;
    memset(&b, 0, sizeof(b));
    b.type = PUSH_TYPE_WORD;
    b.token = 777;
    b.session_id = 55;
    snprintf(b.title, sizeof(b.title), "serendipity");
    CHECK(feed_begin(&b) == PUSH_ACT_READY);
    CHECK(s_sess.state == PUSH_STATE_RECEIVING);
    // 单词卡 total_len 必须为 0：非 0 即拒。/ A word card must carry no body.
    push_begin_t bad = b;
    bad.total_len = 10;
    push_session_init(&s_sess);
    feed_arm(777, 1800000, 0, 0);
    feed(PUSH_EVT_ADV_UP);
    feed(PUSH_EVT_CONNECT);
    CHECK(feed_begin(&bad) == PUSH_ACT_ERR);
    CHECK(s_sess.last_err == PUSH_ERR_BAD_FRAME);

    push_session_init(&s_sess);
    feed_arm(777, 1800000, 0, 0);
    feed(PUSH_EVT_ADV_UP);
    feed(PUSH_EVT_CONNECT);
    CHECK(feed_begin(&b) == PUSH_ACT_READY);
    feed(PUSH_EVT_END);
    CHECK(feed_verify(true, 100) == (PUSH_ACT_COMMIT | PUSH_ACT_WORD_CARD));
    printf("word card: ERR on stray body, COMMIT|WORD_CARD on success ok\n");
}

static void test_resume(void) {
    push_session_init(&s_sess);
    feed_arm(4821, 1800000, 0, 0);
    feed(PUSH_EVT_ADV_UP);
    feed(PUSH_EVT_CONNECT);
    push_begin_t b = make_begin(600, 4821);
    feed_begin(&b);
    feed_data(0, 241);
    feed_data(1, 241);
    CHECK(s_sess.received == 482);

    // 断连回窗口，会话簿记保留。/ Disconnect returns to the window; bookkeeping survives.
    CHECK(feed(PUSH_EVT_DISCONNECT) == 0);
    CHECK(s_sess.state == PUSH_STATE_WINDOW);
    CHECK(s_sess.received == 482 && s_sess.has_part);

    // 重连后问断点：RESUME 且计数不动，同时进 RECEIVING 好让续发的 DATA 进门。
    // Reconnect and ask: RESUME with counters intact, and into RECEIVING so the
    // DATA that follows the offset is accepted.
    CHECK(feed(PUSH_EVT_CONNECT) == 0);
    CHECK(feed(PUSH_EVT_RESUME_QUERY) == PUSH_ACT_RESUME);
    CHECK(s_sess.received == 482);
    CHECK(s_sess.state == PUSH_STATE_RECEIVING);
    CHECK(feed_data(2, 118) == (PUSH_ACT_APPEND | PUSH_ACT_PROGRESS));
    CHECK(s_sess.received == 600 && s_sess.next_seq == 3);

    // 同会话重发 BEGIN 要从 CONNECTED 进：断一次再连，BEGIN 不重置、直接续。
    // The same-session BEGIN path is reached from CONNECTED: drop and reconnect,
    // and the repeated BEGIN resumes without resetting the counters.
    CHECK(feed(PUSH_EVT_DISCONNECT) == 0);
    CHECK(s_sess.state == PUSH_STATE_WINDOW && s_sess.received == 600);
    CHECK(feed(PUSH_EVT_CONNECT) == 0);
    CHECK(feed_begin(&b) == PUSH_ACT_RESUME);
    CHECK(s_sess.state == PUSH_STATE_RECEIVING);
    CHECK(s_sess.received == 600 && s_sess.next_seq == 3);

    // 跨重启恢复：簿记装回后接着收。/ Across a reboot: reload bookkeeping and continue.
    push_session_t fresh;
    push_session_init(&fresh);
    push_session_resume(&fresh, &b, 482, 2);
    CHECK(fresh.received == 482 && fresh.next_seq == 2 && fresh.has_part);
    memset(&s_evt, 0, sizeof(s_evt));
    s_evt.data = (const uint8_t *)"x";
    s_evt.data_len = 118;
    s_evt.seq = 2;
    push_session_t *keep = &s_sess;
    s_sess = fresh;
    CHECK(push_session_event(&s_sess, PUSH_EVT_DATA, &s_evt) == (PUSH_ACT_APPEND | PUSH_ACT_PROGRESS));
    CHECK(s_sess.received == 600);
    s_sess = *keep;
    printf("resume: disconnect/reconnect/RESUME/same-session BEGIN/cross-reboot reload ok\n");
}

static void test_adopt(void) {
    // adopt：预装簿记但不动状态。/ adopt: preloads bookkeeping and leaves the state alone.
    push_session_init(&s_sess);
    push_begin_t b = make_begin(600, 4821);
    push_session_adopt(&s_sess, &b, 482, 2);
    CHECK(s_sess.state == PUSH_STATE_IDLE);   // IDLE 原样不动 / IDLE untouched
    CHECK(s_sess.received == 482 && s_sess.next_seq == 2 && s_sess.has_part);

    // received 为 0 等于没装。/ received == 0 installs nothing.
    push_session_init(&s_sess);
    push_session_adopt(&s_sess, &b, 0, 0);
    CHECK(s_sess.state == PUSH_STATE_IDLE && !s_sess.has_part && s_sess.received == 0);

    // 装完之后走完整武装链，手机问断点 → RECEIVING + RESUME，随后 DATA 必须被收。
    // After adopting, run the full arm chain: the phone asks the resume point, gets
    // RECEIVING + RESUME, and its next DATA packet must be accepted.
    push_session_init(&s_sess);
    push_session_adopt(&s_sess, &b, 482, 2);
    feed_arm(4821, 1800000, 0, 0);
    feed(PUSH_EVT_ADV_UP);
    feed(PUSH_EVT_CONNECT);
    CHECK(feed(PUSH_EVT_RESUME_QUERY) == PUSH_ACT_RESUME);
    CHECK(s_sess.state == PUSH_STATE_RECEIVING);
    CHECK(feed_data(2, 118) == (PUSH_ACT_APPEND | PUSH_ACT_PROGRESS));
    CHECK(s_sess.received == 600 && s_sess.next_seq == 3);
    // 正文收满后 END 直接过校验关。/ With the body complete, END passes verification.
    CHECK(feed(PUSH_EVT_END) == PUSH_ACT_VERIFY);
    CHECK(s_sess.state == PUSH_STATE_PROCESSING);
    CHECK(feed_verify(true, 10) == (PUSH_ACT_COMMIT | PUSH_ACT_OPEN_BOOK));

    // 没有半成品时问断点：RESUME(offset 0) 但留在 CONNECTED 等 BEGIN。
    // Asking without a partial: RESUME(offset 0) but stay in CONNECTED for a BEGIN.
    push_session_init(&s_sess);
    feed_arm(4821, 1800000, 0, 0);
    feed(PUSH_EVT_ADV_UP);
    feed(PUSH_EVT_CONNECT);
    CHECK(feed(PUSH_EVT_RESUME_QUERY) == PUSH_ACT_RESUME);
    CHECK(s_sess.state == PUSH_STATE_CONNECTED && !s_sess.has_part);
    printf("adopt: preload/resume-query->RECEIVING/DATA-after-resume ok\n");
}

/* ---- 16e/16f/16g：.part 账本与欠账（commit 5 的机器证明）----
 * 这三条是 commit 4 §21.6 自己记下的缺口：当时「账本起点」与「欠账顺序」只有代码
 * 与不变式，没有可指的用例。存储层抽到 push_part.c 之后它们才编得进宿主。
 *
 * These three are the gap commit 4 recorded for itself: the ledger's origin and
 * the owed packet's order had code and an invariant but no nameable case. They
 * only became buildable here after the storage layer moved to push_part.c.
 */

static long file_len(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static bool file_eq(const char *path, size_t off, const uint8_t *want, size_t n) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = fseek(f, (long)off, SEEK_SET) == 0;
    // 分块比：一次读进 256 B 的栈缓冲，别拿 n 当读长（10000 B 的前缀会直接溢出）。
    // Chunked: never use n as the read length - a 10,000 B prefix would overflow.
    uint8_t buf[256];
    size_t done = 0;
    while (ok && done < n) {
        const size_t chunk = (n - done) < sizeof(buf) ? (n - done) : sizeof(buf);
        ok = fread(buf, 1, chunk, f) == chunk && memcmp(buf, want + done, chunk) == 0;
        done += chunk;
    }
    fclose(f);
    return ok;
}

// 16e：续传起点。M121b 的靶是 push_part_adopt() 里的 `s_part_len = received`。
// 16e: the resume origin. M121b's target is `s_part_len = received` in adopt.
static void test_part_origin_adopt(void) {
    s_case = "16e";
    const char *dir = "/tmp/push_part_16e";
    const char *path = "/tmp/push_part_16e/push.part";
    uint8_t old[10000];
    uint8_t pkt[241];
    for (size_t i = 0; i < sizeof(old); i++) old[i] = (uint8_t)(i * 31u + 7u);
    for (size_t i = 0; i < sizeof(pkt); i++) pkt[i] = (uint8_t)(0xA0u + (i & 0x0Fu));

    push_part_test_reset(dir);
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f) {
        CHECK(fwrite(old, 1, sizeof(old), f) == sizeof(old));
        fclose(f);
    }

    // 上次收到 10000 B，账本必须同一时刻就到 10000。
    CHECK(push_part_adopt(10000) == PUSH_PART_ADOPT_OK);
    CHECK(push_part_len() == 10000);
    CHECK(file_len(path) == 10000);

    // 续传首包短写 3 B：回退的 truncate 参数必须是 10000，不是 0。
    // 参数是 0 的话，前 10000 B 会被整段抹掉——这正是 M121b 复刻的现场。
    push_part_test_short_next_write(3);
    CHECK(push_part_append(pkt, sizeof(pkt)) == true);
    CHECK(push_part_test_last_truncate() == 10000);
    CHECK(file_len(path) == 10241);
    CHECK(file_eq(path, 0, old, sizeof(old)));
    CHECK(file_eq(path, 10000, pkt, sizeof(pkt)));

    // 另两态：文件比账本短 = 损坏；卡不在 = 簿记原样留着。
    CHECK(push_part_adopt(20000) == PUSH_PART_ADOPT_CORRUPT);
    push_part_test_reset("/tmp/push_part_16e_nc");
    CHECK(push_part_adopt(100) == PUSH_PART_ADOPT_NO_CARD);
    printf("16e part origin (adopt): ledger 10000 kept, truncate=10000, prefix intact ok\n");
}

// 16f：全新会话起点。M121c 的靶是 push_part_open_fresh() 里的 `s_part_len = 0`。
// 16f: the fresh-session origin. M121c's target is `s_part_len = 0` in open_fresh.
static void test_part_origin_fresh(void) {
    s_case = "16f";
    const char *dir = "/tmp/push_part_16f";
    const char *path = "/tmp/push_part_16f/push.part";
    uint8_t old[10000];
    uint8_t pkt[241];
    for (size_t i = 0; i < sizeof(old); i++) old[i] = (uint8_t)(i * 17u + 1u);
    for (size_t i = 0; i < sizeof(pkt); i++) pkt[i] = 0x5Au;

    push_part_test_reset(dir);
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f) {
        CHECK(fwrite(old, 1, sizeof(old), f) == sizeof(old));
        fclose(f);
    }
    CHECK(push_part_adopt(10000) == PUSH_PART_ADOPT_OK);

    // 新会话：文件截到 0，账本必须一起归零。若账本残留 10000，下面的回退会把
    // 文件延长成一段 0 填充（truncate(10000) 在 241 B 的文件上就是延长）。
    CHECK(push_part_open_fresh() == true);
    CHECK(push_part_len() == 0);
    CHECK(file_len(path) == 0);

    push_part_test_short_next_write(3);
    CHECK(push_part_append(pkt, sizeof(pkt)) == true);
    CHECK(push_part_test_last_truncate() == 0);
    CHECK(file_len(path) == 241);
    CHECK(file_eq(path, 0, pkt, sizeof(pkt)));
    printf("16f part origin (fresh): ledger 0, truncate=0, no zero-padding ok\n");
}

// 16g：欠账顺序。M120 的靶是 part_append_locked() 开头的 push_part_drain()。
// 16g: the owed packet's order. M120's target is push_part_drain() at the top of
// part_append_locked().
static void test_part_owed_order(void) {
    s_case = "16g";
    const char *dir = "/tmp/push_part_16g";
    const char *path = "/tmp/push_part_16g/push.part";
    uint8_t C[241], A[241], B[100], D[241];
    memset(C, 0xCC, sizeof(C));
    memset(A, 0xAA, sizeof(A));
    memset(B, 0xBB, sizeof(B));
    memset(D, 0xDD, sizeof(D));

    push_part_test_reset(dir);
    CHECK(push_part_open_fresh() == true);
    CHECK(push_part_append(C, sizeof(C)) == true);
    CHECK(file_len(path) == 241);

    // A 连败两次（第一次触发重试，第二次把整包推进欠账槽）：返回值仍是 true，
    // 数据没丢，只是不在盘上。
    push_part_test_short_writes(241, 2);
    CHECK(push_part_append(A, sizeof(A)) == true);
    CHECK(file_len(path) == 241); // 只有 C

    // 新包 B 到达：欠账必须先落盘，B 只能排在 A 后面。
    CHECK(push_part_append(B, sizeof(B)) == true);
    CHECK(file_len(path) == 241 + 241 + 100);
    CHECK(file_eq(path, 241, A, sizeof(A)));
    CHECK(file_eq(path, 482, B, sizeof(B)));

    // END 前没有新包替它触发，必须能显式补上（push_part_drain 的第二个调用点）。
    push_part_test_short_writes(241, 2);
    CHECK(push_part_append(D, sizeof(D)) == true);
    CHECK(file_len(path) == 582); // D 还在槽里
    CHECK(push_part_drain() == true);
    CHECK(file_len(path) == 582 + 241);
    CHECK(file_eq(path, 582, D, sizeof(D)));
    printf("16g part owed order: owed lands before the next packet, explicit drain ok\n");
}

// 16h：重试成功不重复不缺（出包门第 4 条「注入三案」之 1）。
// 短写一次 = 瞬时忙；回退到本包起点重写一次必须救回来，且结果与「没失败过」
// 逐字节相同：长度精确（多一字节就是重复）、前缀不动、偏移对得上。
// 16h: a successful retry neither duplicates nor drops (injection case 1 of 3).
// One short write = a transiently busy card; rewinding to this packet's start and
// writing once more must land byte-identical to a clean run: exact length (one
// extra byte is a duplicate), untouched prefix, correct offset.
static void test_part_retry_clean(void) {
    s_case = "16h";
    const char *dir = "/tmp/push_part_16h";
    const char *path = "/tmp/push_part_16h/push.part";
    uint8_t C[100], A[241];
    for (size_t i = 0; i < sizeof(C); i++) C[i] = 0xCC;
    for (size_t i = 0; i < sizeof(A); i++) A[i] = (uint8_t)(i * 7u + 3u);

    push_part_test_reset(dir);
    CHECK(push_part_open_fresh() == true);
    // 起点故意非 0：这样才能证明回退到的是「本包起点」而不是 0。
    // The origin is deliberately non-zero: that is what proves the rewind targets
    // this packet's start and not zero.
    CHECK(push_part_append(C, sizeof(C)) == true);
    CHECK(file_len(path) == 100);

    push_part_test_short_next_write(1);
    CHECK(push_part_append(A, sizeof(A)) == true);
    CHECK(push_part_test_last_truncate() == 100);

    CHECK(file_len(path) == 341);        // 多一字节 = 重复 / one extra byte = duplicate
    CHECK(file_eq(path, 0, C, sizeof(C)));
    CHECK(file_eq(path, 100, A, sizeof(A)));
    CHECK(push_part_len() == 341);
    printf("16h part retry clean: one short write recovered with exact length, no duplicate, no gap ok\n");
}

// 16i：槽接不住 = 会话必须终止（注入三案之 3）。返回值 false 就是调用方的
// CANCEL+ERR 依据；配套三件事必须同时成立：槽作废（不重试）、句柄关闭（否则
// adopt 的 truncate 被单开挡住）、半包能被下次 adopt 截回。
// 16i: when the slot cannot take it the session must end (injection case 3). A
// false return is what CANCEL+ERR keys off; three things must hold together: the
// slot is voided (no retry), the handle is closed (or adopt's truncate hits the
// single-open rule), and the partial bytes are cut back by the next adopt.
static void test_part_owed_giveup(void) {
    s_case = "16i";
    const char *dir = "/tmp/push_part_16i";
    const char *path = "/tmp/push_part_16i/push.part";
    uint8_t C[241], A[241];
    memset(C, 0xCC, sizeof(C));
    memset(A, 0xAA, sizeof(A));

    push_part_test_reset(dir);
    CHECK(push_part_open_fresh() == true);
    CHECK(push_part_append(C, sizeof(C)) == true);
    CHECK(file_len(path) == 241);

    // A 连败两次进槽（返回值仍 true：数据在槽里，没丢）。
    push_part_test_short_writes(241, 2);
    CHECK(push_part_append(A, sizeof(A)) == true);
    CHECK(file_len(path) == 241);

    // 槽补写也失败 → false，这是「接不住」的唯一信号。
    push_part_test_short_next_write(1);
    CHECK(push_part_drain() == false);
    CHECK(push_part_is_open() == false);          // 句柄必须关 / handle must be closed
    const long after_fail = file_len(path);
    // 槽作废：再 drain 既不重试也不再报错，且**一个字节都不许再落盘**——
    // 只看返回值不够（短写注入只生效一次，第二次会「成功」并掩盖槽没作废）。
    // Voided: another drain neither retries nor fails again, and not a single byte
    // more may land - the return value alone is not enough (the short-write
    // injection fires once, so a second attempt would "succeed" and hide a slot
    // that was never voided). M124 is exactly that shape.
    CHECK(push_part_drain() == true);
    CHECK(file_len(path) == after_fail);

    // 短写落进文件的那 240 B 半包，由下次 ARM 的 adopt 按 NVS 记账截回。
    // The 240 partial bytes the short write left behind are cut back by the next
    // ARM's adopt, which truncates to the NVS ledger.
    CHECK(push_part_adopt(241) == PUSH_PART_ADOPT_OK);
    CHECK(file_len(path) == 241);
    CHECK(file_eq(path, 0, C, sizeof(C)));
    printf("16i part owed giveup: drain fails cleanly, slot voided, handle closed, partial cut back by adopt ok\n");
}

static void test_teeth(void) {
    // 有牙 1：seq 跳号拒收且不落盘。/ Tooth 1: a seq gap is refused and never lands.
    push_session_init(&s_sess);
    feed_arm(1, 1800000, 0, 0);
    feed(PUSH_EVT_ADV_UP);
    feed(PUSH_EVT_CONNECT);
    push_begin_t b = make_begin(600, 1);
    feed_begin(&b);
    feed_data(0, 241);
    CHECK(feed_data(5, 241) == PUSH_ACT_ERR);
    CHECK(s_sess.last_err == PUSH_ERR_BAD_FRAME);
    CHECK(s_sess.received == 241 && s_sess.next_seq == 1);
    CHECK(s_sess.state == PUSH_STATE_RECEIVING);

    // 有牙 2：坏 CRC 拒收，回 CONNECTED 保留 .part。/ Tooth 2: a bad CRC refuses and keeps the partial.
    feed(PUSH_EVT_END);
    CHECK(s_sess.state == PUSH_STATE_PROCESSING);
    CHECK(feed_verify(false, 10) == PUSH_ACT_ERR);
    CHECK(s_sess.last_err == PUSH_ERR_CHECKSUM);
    CHECK(s_sess.state == PUSH_STATE_CONNECTED);
    CHECK(s_sess.has_part && s_sess.received == 241);

    // 有牙 3：长度不符同样拒。/ Tooth 3: a length mismatch refuses too.
    feed(PUSH_EVT_END);
    CHECK(feed_verify(true, 11) == PUSH_ACT_ERR);
    CHECK(s_sess.received == 241);

    // 有牙 4：total_len 超硬上限。/ Tooth 4: total_len over the hard cap.
    push_begin_t huge = make_begin(PUSH_TOTAL_MAX + 1, 1);
    CHECK(feed_begin(&huge) == PUSH_ACT_ERR);
    CHECK(s_sess.last_err == PUSH_ERR_TOO_LARGE);
    // total_len 为 0 的正文帧同样拒。/ A zero-length body frame is refused too.
    push_begin_t zero = make_begin(0, 1);
    CHECK(feed_begin(&zero) == PUSH_ACT_ERR);
    CHECK(s_sess.last_err == PUSH_ERR_BAD_FRAME);

    // 有牙 5：token 不符拒收（proximity 模型的闸门）。
    // Tooth 5: a token mismatch is refused (the proximity-model gate).
    push_begin_t wrong = make_begin(600, 9999);
    CHECK(feed_begin(&wrong) == PUSH_ACT_ERR);
    CHECK(s_sess.last_err == PUSH_ERR_BAD_FRAME);

    // 有牙 6：未武装/未连接时写入零动作。/ Tooth 6: writes outside a session do nothing.
    push_session_init(&s_sess);
    CHECK(feed_data(0, 10) == 0);
    CHECK(feed(PUSH_EVT_END) == 0);
    CHECK(feed(PUSH_EVT_RESUME_QUERY) == 0);
    CHECK(s_sess.received == 0 && s_sess.state == PUSH_STATE_IDLE);
    feed_arm(1, 1800000, 0, 0);
    feed(PUSH_EVT_ADV_UP);
    CHECK(feed_data(0, 10) == 0); // WINDOW 态未连接：拒。/ Not connected yet: refused.
    CHECK(s_sess.received == 0);

    // 有牙 7：窗口超时与拔电即退。/ Tooth 7: window timeout and unplug both exit.
    CHECK(feed_tick(1800000) == PUSH_ACT_ADV_OFF);
    CHECK(s_sess.state == PUSH_STATE_IDLE);
    feed_arm(1, 1800000, 0, 0);
    feed(PUSH_EVT_ADV_UP);
    CHECK(feed(PUSH_EVT_UNPLUG) == PUSH_ACT_ADV_OFF);
    CHECK(s_sess.state == PUSH_STATE_IDLE);

    // 有牙 8：取消清场带 DROP。/ Tooth 8: cancel tears down with DROP.
    feed_arm(1, 1800000, 0, 0);
    feed(PUSH_EVT_ADV_UP);
    feed(PUSH_EVT_CONNECT);
    feed_begin(&b);
    feed_data(0, 100);
    CHECK(feed(PUSH_EVT_CANCEL) == (PUSH_ACT_ADV_OFF | PUSH_ACT_DROP));
    CHECK(s_sess.state == PUSH_STATE_IDLE);

    // 充电常开：window_ms=0 永不到期。/ Charger-held: window_ms == 0 never expires.
    push_session_init(&s_sess);
    feed_arm(1, 0, 5000, 0);
    feed(PUSH_EVT_ADV_UP);
    CHECK(feed_tick(0xFFFFFFFFu) == 0);
    CHECK(s_sess.state == PUSH_STATE_WINDOW);
    CHECK(feed(PUSH_EVT_UNPLUG) == PUSH_ACT_ADV_OFF); // 拔电即退，不转窗口。/ Unplug exits outright.

    // 常开 + 传输中拔电：UNPLUG 不打断传输，推完进宽限，宽限到期直接收摊
    // （没有窗口可回）。不然拔了电还挂着广播，用户以为退了其实没退。
    // Held + unplug mid-transfer: UNPLUG never interrupts the transfer; after it
    // the grace expires straight into IDLE (there is no window to return to).
    // Otherwise the radio would keep advertising after the unplug while the user
    // believes it stopped.
    push_session_init(&s_sess);
    feed_arm(9, 0, 5000, 0);
    feed(PUSH_EVT_ADV_UP);
    feed(PUSH_EVT_CONNECT);
    push_begin_t hb = make_begin(600, 9);
    CHECK(feed_begin(&hb) == PUSH_ACT_READY);
    feed_data(0, 241);
    feed_data(1, 241);
    feed_data(2, 118);
    CHECK(feed(PUSH_EVT_END) == PUSH_ACT_VERIFY);
    CHECK(s_sess.state == PUSH_STATE_PROCESSING);
    CHECK(feed_verify(true, 100) == (PUSH_ACT_COMMIT | PUSH_ACT_OPEN_BOOK));
    CHECK(s_sess.state == PUSH_STATE_COOLDOWN);
    // 宽限未到：挂着。/ Before the grace expires: it stays.
    CHECK(feed_tick(100 + 4999) == 0);
    CHECK(s_sess.state == PUSH_STATE_COOLDOWN);
    // 宽限一到：收摊，不回窗口。/ Grace expiring: closes up, no window return.
    CHECK(feed_tick(100 + 5000) == PUSH_ACT_ADV_OFF);
    CHECK(s_sess.state == PUSH_STATE_IDLE);

    // 非法转移保持原状。/ Illegal transitions hold state.
    push_session_init(&s_sess);
    feed_arm(1, 1800000, 0, 0);
    feed(PUSH_EVT_ADV_UP);
    feed(PUSH_EVT_CONNECT);
    feed_begin(&b);
    CHECK(feed(PUSH_EVT_ADV_UP) == 0);
    CHECK(s_sess.state == PUSH_STATE_RECEIVING);
    CHECK(feed(PUSH_EVT_CONNECT) == 0);
    CHECK(s_sess.state == PUSH_STATE_RECEIVING);
    printf("teeth: seq gap/bad CRC/length/total cap/token/unarmed/timeout/unplug/cancel/charger-hold all refuse ok\n");
}

/* ---- py<->C 跨语言 golden vector ----
   下列字节全部由 tools/android/push_pico.py 的 --hex 模式生成（token=0x1A2B,
   type=TEXT, session_id=1, title="跨语言向量✓", 正文 300 字节 = 0xf4ab327b）。
   「协议冻结」若只是人眼读源，那么真机轮就是唯一的跨语言验证；这里把 py 吐的
   帧钉成 C 侧解码断言，py<->C 对表从人眼升级为机器级。最强的那一条在末尾：
   把两包 DATA 的载荷拼回正文，用 C 的 push_crc32 重算，与 py 写进 BEGIN 的
   crc32 字段必须逐位相同——CRC、长度、分包边界、seq 语义四者一次性对死。
   ---- py<->C cross-language golden vector ----
   The bytes below come from push_pico.py --hex (token=0x1A2B, type=TEXT,
   session_id=1, title="跨语言向量✓", 300-byte body, crc 0xf4ab327b). If the
   frozen protocol were only eyeballed against the source, the hardware round
   would be the sole cross-language check; pinning the frames Python emits as C
   decode assertions upgrades py<->C from eyeball to machine. The strongest one
   is last: concatenate both DATA payloads back into the body, recompute with C's
   push_crc32 and require bit-exact equality with the crc32 Python wrote into
   BEGIN - CRC, length, chunk boundaries and seq semantics pinned in one shot. */
static void test_py_cross_vector(void) {
    static const uint8_t begin[83] = {
        0x50, 0x43, 0x01, 0x01, 0x00, 0x2b, 0x1a, 0x2c, 0x01, 0x00, 0x00, 0x7b,
        0x32, 0xab, 0xf4, 0x01, 0x00, 0x00, 0x00, 0xe8, 0xb7, 0xa8, 0xe8, 0xaf,
        0xad, 0xe8, 0xa8, 0x80, 0xe5, 0x90, 0x91, 0xe9, 0x87, 0x8f, 0xe2, 0x9c,
        0x93,
        // 余 46 字节标题区补零 / remaining 46 title bytes are zero-padded
    };
    // 标题区不足 83 的补零由编译器完成，这里显式拼足，避免依赖静默行为。
    // Padding is explicit rather than relying on silent zero-init.
    uint8_t frame[83];
    memcpy(frame, begin, sizeof(begin));
    memset(frame + sizeof(begin), 0, sizeof(frame) - sizeof(begin));

    push_begin_t b;
    CHECK(push_begin_decode(frame, sizeof(frame), &b));
    CHECK(b.token == 0x1A2B);
    CHECK(b.type == PUSH_TYPE_TEXT);
    CHECK(b.total_len == 300u);
    CHECK(b.crc32 == 0xf4ab327bu);
    CHECK(b.session_id == 1u);
    CHECK(strcmp(b.title, "跨语言向量✓") == 0);

    // py 生成的完整 DATA 帧（含载荷）：push_data_decode 会校验载荷确实在
    // 缓冲区里（len >= 3 + n），只喂 3 字节头是解不出来的——所以嵌全帧。
    // Full DATA frames from Python (payload included): push_data_decode
    // checks the payload is really present (len >= 3 + n), so a bare
    // 3-byte header cannot decode - the whole frame is embedded.
    static const uint8_t data0[244] = {
        0x00, 0x00, 0xf1, 0x52, 0x65, 0x61, 0x64, 0x50, 0x69, 0x63, 0x6f, 0x20,
        0x70, 0x79, 0x2d, 0x43, 0x20, 0x76, 0x65, 0x63, 0x74, 0x6f, 0x72, 0x2e,
        0x20, 0x52, 0x65, 0x61, 0x64, 0x50, 0x69, 0x63, 0x6f, 0x20, 0x70, 0x79,
        0x2d, 0x43, 0x20, 0x76, 0x65, 0x63, 0x74, 0x6f, 0x72, 0x2e, 0x20, 0x52,
        0x65, 0x61, 0x64, 0x50, 0x69, 0x63, 0x6f, 0x20, 0x70, 0x79, 0x2d, 0x43,
        0x20, 0x76, 0x65, 0x63, 0x74, 0x6f, 0x72, 0x2e, 0x20, 0x52, 0x65, 0x61,
        0x64, 0x50, 0x69, 0x63, 0x6f, 0x20, 0x70, 0x79, 0x2d, 0x43, 0x20, 0x76,
        0x65, 0x63, 0x74, 0x6f, 0x72, 0x2e, 0x20, 0x52, 0x65, 0x61, 0x64, 0x50,
        0x69, 0x63, 0x6f, 0x20, 0x70, 0x79, 0x2d, 0x43, 0x20, 0x76, 0x65, 0x63,
        0x74, 0x6f, 0x72, 0x2e, 0x20, 0x52, 0x65, 0x61, 0x64, 0x50, 0x69, 0x63,
        0x6f, 0x20, 0x70, 0x79, 0x2d, 0x43, 0x20, 0x76, 0x65, 0x63, 0x74, 0x6f,
        0x72, 0x2e, 0x20, 0x52, 0x65, 0x61, 0x64, 0x50, 0x69, 0x63, 0x6f, 0x20,
        0x70, 0x79, 0x2d, 0x43, 0x20, 0x76, 0x65, 0x63, 0x74, 0x6f, 0x72, 0x2e,
        0x20, 0x52, 0x65, 0x61, 0x64, 0x50, 0x69, 0x63, 0x6f, 0x20, 0x70, 0x79,
        0x2d, 0x43, 0x20, 0x76, 0x65, 0x63, 0x74, 0x6f, 0x72, 0x2e, 0x20, 0x52,
        0x65, 0x61, 0x64, 0x50, 0x69, 0x63, 0x6f, 0x20, 0x70, 0x79, 0x2d, 0x43,
        0x20, 0x76, 0x65, 0x63, 0x74, 0x6f, 0x72, 0x2e, 0x20, 0x52, 0x65, 0x61,
        0x64, 0x50, 0x69, 0x63, 0x6f, 0x20, 0x70, 0x79, 0x2d, 0x43, 0x20, 0x76,
        0x65, 0x63, 0x74, 0x6f, 0x72, 0x2e, 0x20, 0x52, 0x65, 0x61, 0x64, 0x50,
        0x69, 0x63, 0x6f, 0x20, 0x70, 0x79, 0x2d, 0x43, 0x20, 0x76, 0x65, 0x63,
        0x74, 0x6f, 0x72, 0x2e,
    };

    static const uint8_t data1[62] = {
        0x01, 0x00, 0x3b, 0x20, 0x52, 0x65, 0x61, 0x64, 0x50, 0x69, 0x63, 0x6f,
        0x20, 0x70, 0x79, 0x2d, 0x43, 0x20, 0x76, 0x65, 0x63, 0x74, 0x6f, 0x72,
        0x2e, 0x20, 0x52, 0x65, 0x61, 0x64, 0x50, 0x69, 0x63, 0x6f, 0x20, 0x70,
        0x79, 0x2d, 0x43, 0x20, 0x76, 0x65, 0x63, 0x74, 0x6f, 0x72, 0x2e, 0x20,
        0x52, 0x65, 0x61, 0x64, 0x50, 0x69, 0x63, 0x6f, 0x20, 0x70, 0x79, 0x2d,
        0x43, 0x20,
    };
    uint16_t seq; const uint8_t *pl; size_t n;
    CHECK(push_data_decode(data0, sizeof(data0), &seq, &pl, &n));
    CHECK(seq == 0 && n == 241);
    CHECK(pl == data0 + 3);            // 载荷紧跟 3 字节头 / payload follows the 3-byte header
    CHECK(push_data_decode(data1, sizeof(data1), &seq, &pl, &n));
    CHECK(seq == 1 && n == 59);
    CHECK(241 + 59 == 300); // 两包拼回 total_len / the two chunks sum to total_len
    // 拼回的正文用 C 的 CRC32 算，须等于 py BEGIN 里的 crc32 字段。
    // The concatenated body, hashed with C's CRC32, must equal the crc32 field
    // Python wrote into BEGIN.
    uint32_t crc_reassembled = push_crc32(data0 + 3, 241);
    crc_reassembled = push_crc32_continue(crc_reassembled, data1 + 3, 59);
    CHECK(crc_reassembled == b.crc32);

    // CTRL 公共头 / CTRL shared header
    static const uint8_t end_f[4] = {0x50, 0x43, 0x02, 0x01};
    static const uint8_t rq_f[4] = {0x50, 0x43, 0x03, 0x01};
    uint8_t op;
    CHECK(push_ctrl_header_decode(end_f, sizeof(end_f), &op) && op == PUSH_OP_END);
    CHECK(push_ctrl_header_decode(rq_f, sizeof(rq_f), &op) && op == PUSH_OP_RESUME_QUERY);

    // 端到端 CRC：py 的正文原文用 C 的 CRC32 重算，须等于 py 写进 BEGIN 的值。
    // End-to-end CRC: recompute Python's body with C's CRC32; must equal the
    // value Python wrote into BEGIN.
    static const char body[] = "ReadPico py-C vector. ";
    uint32_t c = 0;
    // 正文是上面 22 字节短语的重复后截断到 300 字节，逐段喂验证链式等价。
    // The body is that 22-byte phrase repeated then truncated to 300; feed it in
    // pieces to also exercise the streaming chain.
    size_t fed = 0;
    while (fed < 300) {
        const size_t take = (300 - fed) < sizeof(body) - 1 ? (300 - fed) : sizeof(body) - 1;
        c = push_crc32_continue(c, body, take);
        fed += take;
    }
    CHECK(fed == 300);
    CHECK(c == 0xf4ab327bu);          // C 的 CRC32 == py 的 CRC32
    CHECK(c == b.crc32);              // 且与 py BEGIN 帧里的字段逐位相同

    printf("py<->C golden vector: BEGIN/DATA/CTRL decode + CRC end-to-end ok\n");
}

/* ---- AI 查词帧（gen26，0x06/0x07 + STATUS 0x17/0x18）---- */
/* AI lookup frames (gen26, 0x06/0x07 + STATUS 0x17/0x18). */

static void test_ai_frames(void) {
    // AI_QUERY（0x06）编解码往返 + 字节布局。
    // AI_QUERY (0x06) round-trip plus byte layout.
    push_ai_query_t q;
    memset(&q, 0, sizeof(q));
    snprintf(q.word, sizeof(q.word), "serendipity");
    uint8_t qf[PUSH_AI_QUERY_MAX];
    size_t n = push_ai_query_encode(&q, qf, sizeof(qf));
    CHECK(n == PUSH_AI_QUERY_MAX);
    CHECK(qf[0] == 0x50 && qf[1] == 0x43);
    CHECK(qf[2] == PUSH_OP_AI_QUERY);
    CHECK(qf[3] == PUSH_VERSION);
    CHECK(qf[4] == (uint8_t)strlen("serendipity"));
    push_ai_query_t q2;
    memset(&q2, 0, sizeof(q2));
    CHECK(push_ai_query_decode(qf, n, &q2));
    CHECK(strcmp(q2.word, "serendipity") == 0);

    // AI_RESULT（0x07）：编码产**冻结的 241B 线格式帧**（不是随 PUSH_AI_DEF_MAX
    // 膨胀的 839B——那正是 gen26.6.1 D-1 的陷阱宏 PUSH_AI_RESULT_MAX）。
    // AI_RESULT (0x07): encoding emits the FROZEN 241B wire frame (not the
    // PUSH_AI_DEF_MAX-derived 839B - that was the D-1 trap macro).
    push_ai_result_t r;
    memset(&r, 0, sizeof(r));
    snprintf(r.word, sizeof(r.word), "ephemeral");
    snprintf(r.def, sizeof(r.def), "lasting a very short time");
    uint8_t rf[PUSH_AI_RESULT_WIRE_LEN];
    size_t rn = push_ai_result_encode(&r, rf, sizeof(rf));
    CHECK(rn == PUSH_AI_RESULT_WIRE_LEN);
    CHECK(rn == 241u);
    CHECK(rf[2] == PUSH_OP_AI_RESULT);
    CHECK(rf[4] == (uint8_t)strlen("ephemeral"));
    const size_t def_off = 5u + PUSH_AI_WORD_MAX;
    const size_t dn = (size_t)rf[def_off] | ((size_t)rf[def_off + 1] << 8);
    CHECK(dn == strlen("lasting a very short time"));
    push_ai_result_t r2;
    memset(&r2, 0, sizeof(r2));
    CHECK(push_ai_result_decode(rf, rn, &r2));
    CHECK(strcmp(r2.word, "ephemeral") == 0);
    CHECK(strcmp(r2.def, "lasting a very short time") == 0);
    // 超长释义在 0x07 上按**线格式槽 170** 收敛（不按容量 768，否则写越界）。
    // A long definition clamps to the WIRE slot (170), not to the 768-byte capacity.
    push_ai_result_t rl;
    memset(&rl, 0, sizeof(rl));
    snprintf(rl.word, sizeof(rl.word), "longdef");
    memset(rl.def, 'x', 400);
    rl.def[400] = 0;
    uint8_t rlf[PUSH_AI_RESULT_WIRE_LEN];
    CHECK(push_ai_result_encode(&rl, rlf, sizeof(rlf)) == PUSH_AI_RESULT_WIRE_LEN);
    CHECK(((size_t)rlf[def_off] | ((size_t)rlf[def_off + 1] << 8)) == PUSH_AI_DEF_WIRE_MAX - 1u);

    // ---- gen26.6.1 D-1 回归：解码必须由「APK 真实帧」验证 ----
    // D-1 regression: decoding must be validated against the APK's REAL frame.
    // fixture 由 Java 侧 PushProto.encodeAiResult() 现产（tools/gen_apk_frame_fixture.sh），
    // 手写字节一律作废——过去用 PUSH_AI_RESULT_MAX 缓冲自洽（839B）正是盲区根因。
    // The fixture is produced by the Java PushProto (tools/gen_apk_frame_fixture.sh);
    // hand-written bytes are void - the old 839B buffer-sized round-trip was the blind spot.
    CHECK(sizeof(APK_AI_RESULT_SHORT) == PUSH_AI_RESULT_WIRE_LEN);
    push_ai_result_t ra;
    memset(&ra, 0, sizeof(ra));
    CHECK(push_ai_result_decode(APK_AI_RESULT_SHORT, sizeof(APK_AI_RESULT_SHORT), &ra));
    CHECK(strcmp(ra.word, APK_AI_RESULT_SHORT_WORD) == 0);
    CHECK(strcmp(ra.def, APK_AI_RESULT_SHORT_DEF) == 0);
    // 169B 满载：0x07 单帧帽的满载侧必须接受。/ 169B, the loaded side of the cap.
    CHECK(sizeof(APK_AI_RESULT_169B) == PUSH_AI_RESULT_WIRE_LEN);
    push_ai_result_t rb;
    memset(&rb, 0, sizeof(rb));
    CHECK(push_ai_result_decode(APK_AI_RESULT_169B, sizeof(APK_AI_RESULT_169B), &rb));
    CHECK(strcmp(rb.word, APK_AI_RESULT_169B_WORD) == 0);
    CHECK(strcmp(rb.def, APK_AI_RESULT_169B_DEF) == 0);
    CHECK(strlen(rb.def) == 169u);
    // 170B 越界侧：0x07 def 槽无 NUL 容身处，声明 170 的帧必须拒收。
    // The boundary's other side: a 0x07 frame claiming dn=170 has no room for the
    // NUL, so it must be rejected.
    uint8_t bad_frame[PUSH_AI_RESULT_WIRE_LEN];
    memcpy(bad_frame, APK_AI_RESULT_SHORT, sizeof(bad_frame));
    bad_frame[def_off] = (uint8_t)(PUSH_AI_DEF_WIRE_MAX & 0xFF);
    bad_frame[def_off + 1] = (uint8_t)((PUSH_AI_DEF_WIRE_MAX >> 8) & 0xFF);
    push_ai_result_t rc;
    memset(&rc, 0, sizeof(rc));
    CHECK(!push_ai_result_decode(bad_frame, sizeof(bad_frame), &rc));
    // 短一字节的帧（240B）同样拒收：守卫锚的是冻结线格式长度。
    // One byte short is rejected too: the guard is pinned to the frozen wire length.
    CHECK(!push_ai_result_decode(APK_AI_RESULT_SHORT, PUSH_AI_RESULT_WIRE_LEN - 1u, &rc));

    // STATUS 0x17（设备→手机，请算释义）编码。
    // STATUS 0x17 (device to phone: please define) encode.
    uint8_t sq[2u + PUSH_AI_WORD_MAX];
    size_t sqn = push_st_ai_query_encode("ubiquitous", sq, sizeof(sq));
    CHECK(sqn == 2u + PUSH_AI_WORD_MAX);
    CHECK(sq[0] == PUSH_ST_AI_QUERY);
    CHECK(sq[1] == (uint8_t)strlen("ubiquitous"));

    // 能力位图（STATUS 0x18）编解码往返。/ Capability bitmap (STATUS 0x18) round-trip.
    push_caps_t caps = {0};
    caps.bits = PUSH_CAP_AI_QUERY;
    uint8_t cf[5];
    CHECK(push_caps_encode(&caps, cf, sizeof(cf)) == 5);
    CHECK(cf[0] == PUSH_ST_CAPS);
    CHECK(((uint32_t)cf[1] | ((uint32_t)cf[2] << 8) | ((uint32_t)cf[3] << 16) | ((uint32_t)cf[4] << 24)) == PUSH_CAP_AI_QUERY);
    push_caps_t caps2 = {0};
    CHECK(push_caps_decode(cf, sizeof(cf), &caps2));
    CHECK(caps2.bits == PUSH_CAP_AI_QUERY);

    // 有牙：坏魔数/坏 op/坏版本/短帧全拒。/ Bite: bad magic, op, version and short frame all refuse.
    uint8_t bad[PUSH_AI_QUERY_MAX];
    memcpy(bad, qf, sizeof(bad)); bad[0] = 0x51;
    CHECK(!push_ai_query_decode(bad, sizeof(bad), &q2));
    memcpy(bad, qf, sizeof(bad)); bad[2] = PUSH_OP_END;
    CHECK(!push_ai_query_decode(bad, sizeof(bad), &q2));
    memcpy(bad, qf, sizeof(bad)); bad[3] = PUSH_VERSION + 1;
    CHECK(!push_ai_query_decode(bad, sizeof(bad), &q2));
    CHECK(!push_ai_query_decode(qf, PUSH_AI_QUERY_MAX - 1, &q2));

    // ---- gen26.5：上下文档（0x06 ctx / STATUS 0x17 ctx）----
    // gen26.5: contextual frames (0x06 ctx / STATUS 0x17 ctx).

    // AI_QUERY ctx（77B）往返 + 字节布局。/ AI_QUERY ctx (77B) round-trip + layout.
    push_ai_query_ctx_t cq;
    memset(&cq, 0, sizeof(cq));
    snprintf(cq.word, sizeof(cq.word), "individuation");
    cq.book_id = 0x9e3779b9u; // 书名 FNV-1a 哈希的示意值 / illustrative book-name hash
    cq.pos = 0x0000f3a0u;
    uint8_t cqf[PUSH_AI_CTX_QUERY_MAX];
    size_t cqn = push_ai_query_ctx_encode(&cq, cqf, sizeof(cqf));
    CHECK(cqn == PUSH_AI_CTX_QUERY_MAX);
    CHECK(cqn == 77u);
    CHECK(cqf[2] == PUSH_OP_AI_QUERY);
    CHECK(cqf[4] == (uint8_t)strlen("individuation"));
    const size_t ctx_off = 5u + PUSH_AI_WORD_MAX;
    CHECK(((uint32_t)cqf[ctx_off] | ((uint32_t)cqf[ctx_off + 1] << 8) |
           ((uint32_t)cqf[ctx_off + 2] << 16) | ((uint32_t)cqf[ctx_off + 3] << 24)) == cq.book_id);
    CHECK(((uint32_t)cqf[ctx_off + 4] | ((uint32_t)cqf[ctx_off + 5] << 8) |
           ((uint32_t)cqf[ctx_off + 6] << 16) | ((uint32_t)cqf[ctx_off + 7] << 24)) == cq.pos);
    push_ai_query_ctx_t cq2;
    memset(&cq2, 0, sizeof(cq2));
    CHECK(push_ai_query_ctx_decode(cqf, cqn, &cq2));
    CHECK(strcmp(cq2.word, "individuation") == 0);
    CHECK(cq2.book_id == cq.book_id && cq2.pos == cq.pos);

    // len 分流互斥：旧 69B 帧喂 ctx 解码必须拒（严格等长）。/ Length dispatch:
    // the legacy 69B frame must be refused by the strict ctx decoder.
    CHECK(!push_ai_query_ctx_decode(qf, n, &cq2));
    // 反向宽容路径钉死：ctx 77B 帧喂 legacy 解码返 true，词正确、上下文静默丢失
    // （旧解码器不会被新帧搞崩）。Reverse tolerance pinned: the legacy decoder
    // accepts a 77B frame, decodes the word correctly, silently drops context.
    push_ai_query_t ql;
    memset(&ql, 0, sizeof(ql));
    CHECK(push_ai_query_decode(cqf, cqn, &ql));
    CHECK(strcmp(ql.word, "individuation") == 0);

    // STATUS 0x17 ctx（74B 体）编码 + 布局。/ STATUS 0x17 ctx (74B body) encode + layout.
    uint8_t scq[PUSH_ST_AI_CTX_QUERY_LEN];
    size_t scqn = push_st_ai_query_ctx_encode("parent-child", 0x12345678u, 0x00000042u, scq, sizeof(scq));
    CHECK(scqn == PUSH_ST_AI_CTX_QUERY_LEN);
    CHECK(scqn == 74u);
    CHECK(scq[0] == PUSH_ST_AI_QUERY);
    CHECK(scq[1] == (uint8_t)strlen("parent-child"));
    const size_t sc_off = 2u + PUSH_AI_WORD_MAX;
    CHECK(((uint32_t)scq[sc_off] | ((uint32_t)scq[sc_off + 1] << 8) |
           ((uint32_t)scq[sc_off + 2] << 16) | ((uint32_t)scq[sc_off + 3] << 24)) == 0x12345678u);
    CHECK(((uint32_t)scq[sc_off + 4] | ((uint32_t)scq[sc_off + 5] << 8) |
           ((uint32_t)scq[sc_off + 6] << 16) | ((uint32_t)scq[sc_off + 7] << 24)) == 0x42u);

    // ---- gen26.5：释义分帧（0x08）----
    // gen26.5: segmented definitions (0x08).

    // 单帧（seg_total==1，语义等价 0x07）。/ Single frame (seg_total==1 == plain 0x07).
    push_ai_seg_t sg;
    memset(&sg, 0, sizeof(sg));
    snprintf(sg.word, sizeof(sg.word), "uncooperative");
    sg.seg_idx = 0;
    sg.seg_total = 1;
    snprintf(sg.seg, sizeof(sg.seg), "not willing to help or agree");
    uint8_t sf[PUSH_AI_SEG_FRAME_MAX];
    size_t sfn = push_ai_seg_encode(&sg, sf, sizeof(sf));
    CHECK(sfn == PUSH_AI_SEG_FRAME_MAX);
    CHECK(sfn == 241u);
    CHECK(sf[2] == PUSH_OP_AI_RESULT_SEG);
    const size_t seg_off = 5u + PUSH_AI_WORD_MAX;
    CHECK(((uint32_t)sf[seg_off] | ((uint32_t)sf[seg_off + 1] << 8)) == 0u);      // idx
    CHECK(((uint32_t)sf[seg_off + 2] | ((uint32_t)sf[seg_off + 3] << 8)) == 1u);  // total
    CHECK(((uint32_t)sf[seg_off + 4] | ((uint32_t)sf[seg_off + 5] << 8)) == strlen("not willing to help or agree"));
    push_ai_seg_t sg2;
    memset(&sg2, 0, sizeof(sg2));
    CHECK(push_ai_seg_decode(sf, sfn, &sg2));
    CHECK(strcmp(sg2.word, "uncooperative") == 0);
    CHECK(sg2.seg_idx == 0 && sg2.seg_total == 1);
    CHECK(strcmp(sg2.seg, "not willing to help or agree") == 0);

    // 多帧重组前站：3 帧序列逐帧往返 + 拼接等价原文（Step7 重组器的协议层验证）。
    // Multi-frame reassembly front-stop: a 3-frame sequence round-trips frame by
    // frame and concatenates back to the original (protocol layer of Step7).
    static const char *long_def = "The quality of being unwilling to cooperate or "
        "agree with others, often seen in children who resist peer or parent "
        "influence and assert their own will strongly.";
    const size_t total_len = strlen(long_def);
    const size_t per = (total_len + 2) / 3; // 3 帧均分 / split across 3 frames
    CHECK(per < PUSH_AI_SEG_MAX);
    char reassembled[512];
    size_t got = 0;
    for (size_t i = 0; i < 3; ++i) {
        push_ai_seg_t part;
        memset(&part, 0, sizeof(part));
        snprintf(part.word, sizeof(part.word), "uncooperative");
        part.seg_idx = (uint16_t)i;
        part.seg_total = 3;
        size_t take = total_len - i * per;
        if (take > per) take = per;
        memcpy(part.seg, long_def + i * per, take);
        part.seg[take] = 0;
        uint8_t pf[PUSH_AI_SEG_FRAME_MAX];
        CHECK(push_ai_seg_encode(&part, pf, sizeof(pf)) == PUSH_AI_SEG_FRAME_MAX);
        push_ai_seg_t part2;
        memset(&part2, 0, sizeof(part2));
        CHECK(push_ai_seg_decode(pf, PUSH_AI_SEG_FRAME_MAX, &part2));
        CHECK(part2.seg_idx == i && part2.seg_total == 3);
        CHECK(strlen(part2.seg) == take);
        memcpy(reassembled + got, part2.seg, take);
        got += take;
    }
    reassembled[got] = 0;
    CHECK(got == total_len);
    CHECK(strcmp(reassembled, long_def) == 0);

    // 分帧有牙：序号越界/总数为 0/短帧/坏 op 全拒。/ Segment teeth: bad index,
    // zero total, short frame and bad op all refuse.
    CHECK(!push_ai_seg_encode(&(push_ai_seg_t){.word = "x", .seg_idx = 1, .seg_total = 1, .seg = "y"}, sf, sizeof(sf)));
    CHECK(!push_ai_seg_encode(&(push_ai_seg_t){.word = "x", .seg_idx = 0, .seg_total = 0, .seg = "y"}, sf, sizeof(sf)));
    CHECK(!push_ai_seg_decode(sf, PUSH_AI_SEG_FRAME_MAX - 1, &sg2));
    {
        uint8_t tmp[PUSH_AI_SEG_FRAME_MAX];
        CHECK(push_ai_seg_encode(&sg, tmp, sizeof(tmp)) == PUSH_AI_SEG_FRAME_MAX);
        tmp[2] = PUSH_OP_AI_RESULT;
        CHECK(!push_ai_seg_decode(tmp, sizeof(tmp), &sg2));
        CHECK(push_ai_seg_encode(&sg, tmp, sizeof(tmp)) == PUSH_AI_SEG_FRAME_MAX);
        tmp[seg_off + 1] = 0x01; // seg_idx=0x0100 > total → 越界 / index out of range
        CHECK(!push_ai_seg_decode(tmp, sizeof(tmp), &sg2));
    }

    // ---- gen26.5：能力使能帧（0x09，8B）----
    // gen26.5: capability enable frame (0x09, 8B).

    push_caps_t en;
    en.bits = PUSH_CAP_AI_CONTEXT | PUSH_CAP_AI_MULTI;
    uint8_t ef[PUSH_AI_CAPS_ENABLE_LEN];
    CHECK(push_ai_caps_enable_encode(&en, ef, sizeof(ef)) == PUSH_AI_CAPS_ENABLE_LEN);
    CHECK(ef[2] == PUSH_OP_AI_CAPS_ENABLE);
    push_caps_t en2;
    CHECK(push_ai_caps_enable_decode(ef, sizeof(ef), &en2));
    CHECK(en2.bits == (PUSH_CAP_AI_CONTEXT | PUSH_CAP_AI_MULTI));
    // 严格 8B：7B/9B 都拒。/ Strict 8B: 7B and 9B both refuse.
    CHECK(!push_ai_caps_enable_decode(ef, 7u, &en2));
    CHECK(!push_ai_caps_enable_decode(ef, 9u, &en2));
    {
        uint8_t bad_en[PUSH_AI_CAPS_ENABLE_LEN];
        memcpy(bad_en, ef, sizeof(bad_en)); bad_en[2] = PUSH_OP_AI_QUERY;
        CHECK(!push_ai_caps_enable_decode(bad_en, sizeof(bad_en), &en2));
        memcpy(bad_en, ef, sizeof(bad_en)); bad_en[3] = PUSH_VERSION + 1;
        CHECK(!push_ai_caps_enable_decode(bad_en, sizeof(bad_en), &en2));
    }

    // ---- gen26.5：0x17 发送决策（协商分流唯一出口）----
    // gen26.5: the 0x17 emission decision (the single dispatch exit point).
    //
    // 这是「默认 legacy + APK 显式使能」的协议层落点：设备未收到 0x09 前永远发
    // 66B 旧帧，旧 APK 零感知；使能后且确有书内上下文才升 74B。
    // This is where "legacy by default + explicit APK enable" lands in the protocol
    // layer: until 0x09 arrives the device always emits the 66B frame, which an old
    // APK reads without noticing; only after enable, and only with a real in-book
    // context, does it upgrade to 74B.
    uint8_t em[PUSH_ST_AI_CTX_QUERY_LEN]; // 74B：两种格式共用一槽 / one slot fits both
    size_t emn;

    // ① 未使能（旧 APK / 断连后）→ 66B legacy，即便有上下文也不升档。
    // Not enabled (old APK / after disconnect) -> 66B legacy even with context.
    emn = push_st_ai_query_emit("ephemeral", 0x9e3779b9u, 0x1234u, false, em, sizeof(em));
    CHECK(emn == PUSH_ST_AI_LEGACY_QUERY_LEN);
    CHECK(emn == 66u);
    CHECK(em[0] == PUSH_ST_AI_QUERY);

    // ② 未使能 + 无上下文 → 仍然 66B。/ Not enabled, no context -> still 66B.
    emn = push_st_ai_query_emit("ephemeral", 0u, 0u, false, em, sizeof(em));
    CHECK(emn == 66u);

    // ③ 使能 + 有书上下文 → 74B。/ Enabled + in-book context -> 74B.
    emn = push_st_ai_query_emit("ephemeral", 0x9e3779b9u, 0x1234u, true, em, sizeof(em));
    CHECK(emn == PUSH_ST_AI_CTX_QUERY_LEN);
    CHECK(emn == 74u);
    {
        // 用旧解码器的读法（op + wlen + 词）解 74B 上下文档：词必须正确——这就是
        // 新旧互连的「新设备 → 旧 APK」半环（Python/Java 侧同名断言已各自钉死）。
        // Read the 74B frame the legacy way (op + wlen + word): the word must be
        // intact - the "new device -> old APK" half of interop (the Python/Java
        // sides pin the same claim in their own tests).
        const size_t wn = em[1];
        CHECK(wn == strlen("ephemeral"));
        CHECK(memcmp(em + 2, "ephemeral", wn) == 0);
        // 词槽之后是 8B 上下文（book_id + pos，小端）；旧 APK 视而不见，新 APK 读得出。
        // After the word slot come 8B of context (book_id + pos, LE): invisible to
        // an old APK, readable by a new one.
        const uint32_t got_book = (uint32_t)em[2u + PUSH_AI_WORD_MAX] |
            ((uint32_t)em[2u + PUSH_AI_WORD_MAX + 1u] << 8) |
            ((uint32_t)em[2u + PUSH_AI_WORD_MAX + 2u] << 16) |
            ((uint32_t)em[2u + PUSH_AI_WORD_MAX + 3u] << 24);
        CHECK(got_book == 0x9e3779b9u);
    }

    // ④ 使能但无书上下文（词典弹窗）→ 退回 66B，不编造上下文。
    // Enabled but no book context (dictionary popup) -> back to 66B; no invented context.
    emn = push_st_ai_query_emit("ephemeral", 0u, 0u, true, em, sizeof(em));
    CHECK(emn == 66u);

    // ⑤ cap 有牙：ctx 需要 74、legacy 需要 66，少一字节就返 0（绝不发半帧）。
    // Cap teeth: ctx needs 74, legacy needs 66; one byte short returns 0.
    CHECK(push_st_ai_query_emit("ephemeral", 1u, 1u, true, em, 73u) == 0u);
    CHECK(push_st_ai_query_emit("ephemeral", 1u, 1u, false, em, 65u) == 0u);
    CHECK(push_st_ai_query_emit("ephemeral", 1u, 1u, false, em, 66u) == 66u);
    CHECK(push_st_ai_query_emit(NULL, 1u, 1u, true, em, sizeof(em)) == 0u);
    CHECK(push_st_ai_query_emit("ephemeral", 1u, 1u, true, NULL, sizeof(em)) == 0u);

    // ---- gen26.5：TTS 页文本（STATUS 0x19，241B 定长）----
    // gen26.5: TTS page text (STATUS 0x19, fixed 241B).
    //
    // 帧长断言先行（登记纪律）：0x08 那次 243>241 就是靠这道断言在登记期擒住的。
    // Frame-length assertion first (registration discipline): this is the assertion
    // that caught 0x08's 243 > 241 at registration time.
    CHECK(PUSH_ST_TTS_TEXT_FRAME_MAX == 241u);
    CHECK(PUSH_ST_TTS_TEXT_FRAME_MAX <= 244u); // STATUS notify 帽 = MTU 247 - 3 ATT
    CHECK(PUSH_TTS_TEXT_MAX == 234u);
    CHECK(PUSH_ST_TTS_TEXT == 0x19);

    // 整页一帧（total==1）。/ Whole page in one frame.
    push_tts_text_t tt;
    memset(&tt, 0, sizeof(tt));
    tt.seq = 0;
    tt.total = 1;
    snprintf(tt.text, sizeof(tt.text), "It was a bright cold day in April.");
    uint8_t tf[PUSH_ST_TTS_TEXT_FRAME_MAX];
    CHECK(push_st_tts_text_encode(&tt, tf, sizeof(tf)) == PUSH_ST_TTS_TEXT_FRAME_MAX);
    CHECK(tf[0] == PUSH_ST_TTS_TEXT);
    CHECK(((uint32_t)tf[1] | ((uint32_t)tf[2] << 8)) == 0u);   // seq
    CHECK(((uint32_t)tf[3] | ((uint32_t)tf[4] << 8)) == 1u);   // total
    CHECK(((uint32_t)tf[5] | ((uint32_t)tf[6] << 8)) == strlen("It was a bright cold day in April."));
    push_tts_text_t tt2;
    memset(&tt2, 0, sizeof(tt2));
    CHECK(push_st_tts_text_decode(tf, sizeof(tf), &tt2));
    CHECK(tt2.seq == 0 && tt2.total == 1);
    CHECK(strcmp(tt2.text, "It was a bright cold day in April.") == 0);
    CHECK(tt2.len == strlen("It was a bright cold day in April."));

    // 3 帧重组前站：拼接 == 原文（Step10 实现前的协议层验证）。
    // Three-frame reassembly front-stop: concatenation == original (protocol layer).
    static const char *page_text = "The clocks were striking thirteen. Winston "
        "Smith, his chin nuzzled into his breast in an effort to escape the "
        "vile wind, slipped quickly through the glass doors of Victory Mansions.";
    const size_t pt_len = strlen(page_text);
    const size_t pt_per = (pt_len + 2) / 3;
    CHECK(pt_per < PUSH_TTS_TEXT_MAX);
    char pt_back[512];
    size_t pt_got = 0;
    for (size_t i = 0; i < 3; ++i) {
        push_tts_text_t part;
        memset(&part, 0, sizeof(part));
        part.seq = (uint16_t)i;
        part.total = 3;
        size_t take = pt_len - i * pt_per;
        if (take > pt_per) take = pt_per;
        memcpy(part.text, page_text + i * pt_per, take);
        part.text[take] = 0;
        uint8_t pf[PUSH_ST_TTS_TEXT_FRAME_MAX];
        CHECK(push_st_tts_text_encode(&part, pf, sizeof(pf)) == PUSH_ST_TTS_TEXT_FRAME_MAX);
        push_tts_text_t pb;
        memset(&pb, 0, sizeof(pb));
        CHECK(push_st_tts_text_decode(pf, sizeof(pf), &pb));
        CHECK(pb.seq == i && pb.total == 3);
        CHECK(strlen(pb.text) == take);
        memcpy(pt_back + pt_got, pb.text, take);
        pt_got += take;
    }
    pt_back[pt_got] = 0;
    CHECK(pt_got == pt_len);
    CHECK(strcmp(pt_back, page_text) == 0);

    // 有牙：序号越界 / 总数为 0 / 短帧 / 坏 op / len 越界全拒。
    // Teeth: bad index, zero total, short frame, bad op, overlong len all refuse.
    CHECK(!push_st_tts_text_encode(&(push_tts_text_t){.seq = 1, .total = 1, .text = "x"}, tf, sizeof(tf)));
    CHECK(!push_st_tts_text_encode(&(push_tts_text_t){.seq = 0, .total = 0, .text = "x"}, tf, sizeof(tf)));
    CHECK(!push_st_tts_text_decode(tf, PUSH_ST_TTS_TEXT_FRAME_MAX - 1, &tt2));
    {
        uint8_t tmp[PUSH_ST_TTS_TEXT_FRAME_MAX];
        CHECK(push_st_tts_text_encode(&tt, tmp, sizeof(tmp)) == PUSH_ST_TTS_TEXT_FRAME_MAX);
        tmp[0] = PUSH_ST_CAPS;                       // 坏 op / bad op
        CHECK(!push_st_tts_text_decode(tmp, sizeof(tmp), &tt2));
        CHECK(push_st_tts_text_encode(&tt, tmp, sizeof(tmp)) == PUSH_ST_TTS_TEXT_FRAME_MAX);
        tmp[5] = 0xFF; tmp[6] = 0xFF;                // len = 65535 ≥ 槽 / overlong
        CHECK(!push_st_tts_text_decode(tmp, sizeof(tmp), &tt2));
        CHECK(push_st_tts_text_encode(&tt, tmp, sizeof(tmp)) == PUSH_ST_TTS_TEXT_FRAME_MAX);
        tmp[2] = 0x01;                               // seq = 0x0100 ≥ total / out of range
        CHECK(!push_st_tts_text_decode(tmp, sizeof(tmp), &tt2));
    }

    printf("ai frames: query/result/status 0x17/caps round-trip + bad-header teeth ok\n");
    printf("tts text gen26.5: STATUS 0x19 fixed 241 (single + 3-frame reassembly) + teeth ok\n");
    printf("ai frames gen26.5: ctx 77/74, seg 241 (single+multi reassembly), enable 8, len-dispatch ok\n");
    printf("ai emit gen26.5: legacy-by-default dispatch (66/74 by enable+context) + cap teeth ok\n");
}

/* ---- 0x08 释义分帧重组器（gen26.5 Step7，纯逻辑）---- */
/* 0x08 definition reassembler (gen26.5 Step7, pure logic). */
static void test_ai_segasm(void) {
    ai_segasm_t a;
    const char *text = NULL;
    size_t len = 0;

    // 单帧（total==1）立即完成，语义等价 0x07。/ A single frame completes at once.
    ai_segasm_reset(&a);
    push_ai_seg_t one;
    memset(&one, 0, sizeof(one));
    snprintf(one.word, sizeof(one.word), "ephemeral");
    one.seg_total = 1;
    snprintf(one.seg, sizeof(one.seg), "lasting a very short time");
    CHECK(ai_segasm_feed(&a, &one, &text, &len) == AI_SEGASM_DONE);
    CHECK(len == strlen("lasting a very short time"));
    CHECK(strcmp(text, "lasting a very short time") == 0);
    CHECK(strcmp(a.word, "ephemeral") == 0);

    // 3 帧顺序重组：拼接 == 原文。/ Three frames in order: concatenation == original.
    static const char *full = "The quality of being unwilling to cooperate or agree "
        "with others, often seen in children who resist peer or parent influence "
        "and assert their own will strongly, even when it costs them dearly.";
    const size_t flen = strlen(full);
    const size_t per = (flen + 2) / 3; // 3 帧均分 / across three frames
    CHECK(per < PUSH_AI_SEG_MAX);
    ai_segasm_reset(&a);
    ai_segasm_status_t st = AI_SEGASM_PENDING;
    for (size_t i = 0; i < 3; ++i) {
        push_ai_seg_t part;
        memset(&part, 0, sizeof(part));
        snprintf(part.word, sizeof(part.word), "uncooperative");
        part.seg_idx = (uint16_t)i;
        part.seg_total = 3;
        size_t take = flen - i * per;
        if (take > per) take = per;
        memcpy(part.seg, full + i * per, take);
        part.seg[take] = 0;
        st = ai_segasm_feed(&a, &part, &text, &len);
        CHECK(st == (i < 2 ? AI_SEGASM_PENDING : AI_SEGASM_DONE));
    }
    CHECK(st == AI_SEGASM_DONE);
    CHECK(len == flen);
    CHECK(strcmp(text, full) == 0); // 重组无损 / lossless reassembly

    // 有牙①：首帧不是 seq==0 → 拒（没有在建条目可续）。
    // Teeth 1: a first frame whose seq != 0 is refused (nothing in flight).
    ai_segasm_reset(&a);
    push_ai_seg_t mid;
    memset(&mid, 0, sizeof(mid));
    snprintf(mid.word, sizeof(mid.word), "w");
    mid.seg_idx = 1;
    mid.seg_total = 3;
    snprintf(mid.seg, sizeof(mid.seg), "tail");
    CHECK(ai_segasm_feed(&a, &mid, &text, &len) == AI_SEGASM_REJECT);
    CHECK(!a.active); // 且已重置 / and reset

    // 有牙②：跳号 / 重复序号 → 拒并重置。/ Teeth 2: a gap or repeat is refused + reset.
    ai_segasm_reset(&a);
    push_ai_seg_t p0;
    memset(&p0, 0, sizeof(p0));
    snprintf(p0.word, sizeof(p0.word), "w");
    p0.seg_idx = 0;
    p0.seg_total = 3;
    snprintf(p0.seg, sizeof(p0.seg), "aaa");
    CHECK(ai_segasm_feed(&a, &p0, &text, &len) == AI_SEGASM_PENDING);
    push_ai_seg_t p2;
    memset(&p2, 0, sizeof(p2));
    snprintf(p2.word, sizeof(p2.word), "w");
    p2.seg_idx = 2; // 跳过了 1 / skipped 1
    p2.seg_total = 3;
    snprintf(p2.seg, sizeof(p2.seg), "ccc");
    CHECK(ai_segasm_feed(&a, &p2, &text, &len) == AI_SEGASM_REJECT);

    // 有牙③：中途换词 → 拒（两个词的释义绝不拼在一起）。/ Teeth 3: a word change.
    ai_segasm_reset(&a);
    CHECK(ai_segasm_feed(&a, &p0, &text, &len) == AI_SEGASM_PENDING);
    push_ai_seg_t other;
    memset(&other, 0, sizeof(other));
    snprintf(other.word, sizeof(other.word), "other");
    other.seg_idx = 1;
    other.seg_total = 3;
    snprintf(other.seg, sizeof(other.seg), "bbb");
    CHECK(ai_segasm_feed(&a, &other, &text, &len) == AI_SEGASM_REJECT);

    // 有牙④：total 中途变化 → 拒。/ Teeth 4: total changes mid-stream.
    ai_segasm_reset(&a);
    CHECK(ai_segasm_feed(&a, &p0, &text, &len) == AI_SEGASM_PENDING);
    push_ai_seg_t retotal;
    memset(&retotal, 0, sizeof(retotal));
    snprintf(retotal.word, sizeof(retotal.word), "w");
    retotal.seg_idx = 1;
    retotal.seg_total = 4; // 与首帧的 3 不一致 / disagrees with the head frame's 3
    snprintf(retotal.seg, sizeof(retotal.seg), "bbb");
    CHECK(ai_segasm_feed(&a, &retotal, &text, &len) == AI_SEGASM_REJECT);

    // 有牙⑤：seq==0 重发 = 开新条（旧条目作废，不拼接）。
    // Teeth 5: re-sending seq==0 starts fresh (the old item is dropped, never spliced).
    ai_segasm_reset(&a);
    CHECK(ai_segasm_feed(&a, &p0, &text, &len) == AI_SEGASM_PENDING);
    push_ai_seg_t again;
    memset(&again, 0, sizeof(again));
    snprintf(again.word, sizeof(again.word), "w");
    again.seg_idx = 0;
    again.seg_total = 1;
    snprintf(again.seg, sizeof(again.seg), "zzz");
    CHECK(ai_segasm_feed(&a, &again, &text, &len) == AI_SEGASM_DONE);
    CHECK(strcmp(text, "zzz") == 0); // 只有新条的内容 / only the new item's text

    // 有牙⑥：超上限 → OVERFLOW 且重置（绝不截断成半截释义）。
    // Teeth 6: over the cap -> OVERFLOW and reset (never a truncated definition).
    ai_segasm_reset(&a);
    push_ai_seg_t big;
    memset(&big, 0, sizeof(big));
    snprintf(big.word, sizeof(big.word), "w");
    big.seg_idx = 0;
    big.seg_total = 64; // 与续帧一致，否则走的是「total 变了」分支而非溢出分支
    memset(big.seg, 'x', PUSH_AI_SEG_MAX - 1); // 单帧塞满 / fill one frame
    big.seg[PUSH_AI_SEG_MAX - 1] = 0;
    CHECK(ai_segasm_feed(&a, &big, &text, &len) == AI_SEGASM_PENDING);
    CHECK(a.len == PUSH_AI_SEG_MAX - 1);
    // 续到超过 AI_SEGASM_MAX / keep feeding past AI_SEGASM_MAX
    ai_segasm_status_t over = AI_SEGASM_PENDING;
    for (size_t i = 1; i < 64 && over == AI_SEGASM_PENDING; ++i) {
        push_ai_seg_t more;
        memset(&more, 0, sizeof(more));
        snprintf(more.word, sizeof(more.word), "w");
        more.seg_idx = (uint16_t)i;
        more.seg_total = 64;
        memset(more.seg, 'y', PUSH_AI_SEG_MAX - 1);
        more.seg[PUSH_AI_SEG_MAX - 1] = 0;
        over = ai_segasm_feed(&a, &more, &text, &len);
    }
    CHECK(over == AI_SEGASM_OVERFLOW);
    CHECK(!a.active && a.len == 0);

    printf("ai segasm gen26.5: single + 3-frame reassembly == original, seq0 new item, "
           "gap/word-change/retotal/overflow all refuse ok\n");
}

/* ---- 搭车离线队列（gen26 块 2）---- */
/* Ride-along offline queue (gen26 block 2). */

static void test_ai_ridealong(void) {
    const uint16_t cap = 4;
    uint8_t buf[512];
    CHECK(ai_raloc_buf_size(cap) == (size_t)6 + (size_t)cap * (1u + (size_t)PUSH_AI_WORD_MAX));
    CHECK(ai_raloc_buf_size(cap) <= sizeof(buf));
    // init 新建 + 容量/count 正确；缓冲不足 init 拒。
    // init fresh, cap/count correct; too-small buffer refuses.
    CHECK(ai_raloc_init(buf, sizeof(buf), cap));
    CHECK(ai_raloc_cap(buf) == cap);
    CHECK(ai_raloc_count(buf) == 0);
    CHECK(!ai_raloc_init(buf, 5, cap));

    // 入队 + 去重 + peek。/ enqueue, dedup, peek.
    CHECK(ai_raloc_enqueue(buf, sizeof(buf), "apple"));
    CHECK(ai_raloc_count(buf) == 1);
    CHECK(!ai_raloc_enqueue(buf, sizeof(buf), "apple")); // 去重 / dedup
    CHECK(ai_raloc_count(buf) == 1);
    CHECK(ai_raloc_enqueue(buf, sizeof(buf), "banana"));
    CHECK(ai_raloc_count(buf) == 2);
    char w[96];
    CHECK(ai_raloc_peek(buf, w, sizeof(w)));
    CHECK(strcmp(w, "apple") == 0);

    // 出队前移：apple 出、banana 提前、count 递减、空队列拒。
    // dequeue shifts forward: apple out, banana promoted, count down, empty refuses.
    CHECK(ai_raloc_dequeue(buf, w, sizeof(w)));
    CHECK(strcmp(w, "apple") == 0);
    CHECK(ai_raloc_count(buf) == 1);
    CHECK(ai_raloc_peek(buf, w, sizeof(w)));
    CHECK(strcmp(w, "banana") == 0);
    CHECK(ai_raloc_dequeue(buf, w, sizeof(w)));
    CHECK(strcmp(w, "banana") == 0);
    CHECK(ai_raloc_count(buf) == 0);
    CHECK(!ai_raloc_dequeue(buf, w, sizeof(w)));

    // 填满到容量，超容量拒。/ fill to capacity, over-capacity refuses.
    ai_raloc_clear(buf);
    CHECK(ai_raloc_count(buf) == 0);
    for (int i = 0; i < (int)cap; ++i) {
        char tmp[8];
        snprintf(tmp, sizeof(tmp), "w%d", i);
        CHECK(ai_raloc_enqueue(buf, sizeof(buf), tmp));
    }
    CHECK(ai_raloc_count(buf) == cap);
    CHECK(!ai_raloc_enqueue(buf, sizeof(buf), "overflow"));

    // 续载：同 cap 续载保留条目，异 cap 拒。/ resume: same cap keeps entries,
    // mismatched cap refuses.
    CHECK(ai_raloc_init(buf, sizeof(buf), cap));
    CHECK(ai_raloc_count(buf) == cap);
    CHECK(!ai_raloc_init(buf, sizeof(buf), (uint16_t)(cap + 1)));
    CHECK(ai_raloc_init(buf, sizeof(buf), cap));
    CHECK(ai_raloc_count(buf) == cap);
    ai_raloc_clear(buf);
    CHECK(ai_raloc_cap(buf) == cap); // clear 保留容量 / clear preserves capacity
    printf("ai ridealong: init/resume/enqueue dedup/peek/dequeue shift/clear/full ok\n");
}

/* ---- TTS 页文本帧切分计划（gen26.5 Step10，纯逻辑）---- */
/* TTS page-text frame plan (gen26.5 Step10, pure logic). */
static void test_tts_plan(void) {
    // 预算与槽的关系：帧预算 233 = 槽 234 保 NUL。/ Budget vs slot: 233 = 234 minus NUL.
    CHECK(PUSH_TTS_FRAME_BUDGET == 233u);
    CHECK(PUSH_TTS_FRAME_BUDGET == PUSH_TTS_TEXT_MAX - 1u);

    // 空文本 = 1 帧 0 字节。/ Empty text = one 0-byte frame.
    CHECK(push_tts_frame_count("", 0) == 1);
    const char *slice = NULL;
    size_t slice_len = 99;
    CHECK(push_tts_frame_at("", 0, 0, &slice, &slice_len));
    CHECK(slice_len == 0);
    CHECK(!push_tts_frame_at("", 0, 1, &slice, &slice_len));

    // UTF-8 安全前进的单元行为。/ Unit behaviour of the UTF-8 advance.
    CHECK(push_tts_utf8_advance("abc", 3, 2) == 2);           // ASCII 截到预算
    CHECK(push_tts_utf8_advance("abc", 3, 99) == 3);          // 短文本全收
    CHECK(push_tts_utf8_advance("\xE4\xB8\xAD\xE6\x96\x87", 6, 1) == 0); // 中=3B，预算1放不下
    CHECK(push_tts_utf8_advance("\xE4\xB8\xAD\xE6\x96\x87", 6, 3) == 3); // 恰放一个
    CHECK(push_tts_utf8_advance("\xE4\xB8\xAD\xE6\x96\x87", 6, 4) == 3); // 第2个放不下，收刀
    CHECK(push_tts_utf8_advance("\xE4\xB8\xAD\xE6\x96\x87", 6, 6) == 6); // 两个都收
    CHECK(push_tts_utf8_advance("\x80\x80", 2, 2) == 0);      // 首字节即续字节：不前进
    CHECK(push_tts_utf8_advance("\xF0\x9F\x98\x80z", 5, 4) == 4); // 4B emoji 恰收
    CHECK(push_tts_utf8_advance("\xF0\x9F\x98\x80z", 5, 3) == 0); // emoji 放不下就整颗不收

    // 短页一帧。/ A short page is one frame.
    const char *page = "It was a bright cold day in April.";
    CHECK(push_tts_frame_count(page, strlen(page)) == 1);
    CHECK(push_tts_frame_at(page, strlen(page), 0, &slice, &slice_len));
    CHECK(slice_len == strlen(page));

    // 长 ASCII 页：帧数 = ceil(len/233)，切片拼回原文。
    // A long ASCII page: ceil(len/233) frames, slices reassemble to the original.
    static char big[1000];
    for (size_t i = 0; i < sizeof(big) - 1; ++i) big[i] = (char)('a' + (i % 26));
    const size_t big_len = sizeof(big) - 1;
    CHECK(push_tts_frame_count(big, big_len) == (big_len + 232) / 233);
    {
        char back[sizeof(big)];
        size_t got = 0;
        const size_t total = push_tts_frame_count(big, big_len);
        for (size_t i = 0; i < total; ++i) {
            CHECK(push_tts_frame_at(big, big_len, i, &slice, &slice_len));
            CHECK(slice_len > 0 && slice_len <= PUSH_TTS_FRAME_BUDGET);
            memcpy(back + got, slice, slice_len);
            got += slice_len;
        }
        CHECK(got == big_len);
        CHECK(memcmp(back, big, big_len) == 0);
        CHECK(!push_tts_frame_at(big, big_len, total, &slice, &slice_len)); // 越界
    }

    // 中文页：每颗 3 字节，帧内颗数 = 233/3 = 77 颗余 2；拼回原文且无一颗被劈。
    // A Chinese page: 3 bytes each, 77 characters plus a 2-byte remainder per
    // frame; reassembly matches and no character is ever split.
    static char zh[1000];
    size_t zh_len = 0;
    while (zh_len + 3 <= sizeof(zh) - 1) {
        zh[zh_len++] = (char)0xE4;
        zh[zh_len++] = (char)0xB8;
        zh[zh_len++] = (char)0xAD;
    }
    CHECK(push_tts_frame_count(zh, zh_len) == (zh_len + 232) / 233);
    {
        static char back[sizeof(zh)];
        size_t got = 0;
        const size_t total = push_tts_frame_count(zh, zh_len);
        for (size_t i = 0; i < total; ++i) {
            CHECK(push_tts_frame_at(zh, zh_len, i, &slice, &slice_len));
            CHECK(slice_len % 3 == 0); // 帧内恒为整颗 / whole characters only
            memcpy(back + got, slice, slice_len);
            got += slice_len;
        }
        CHECK(got == zh_len);
        CHECK(memcmp(back, zh, zh_len) == 0);
    }

    // 贴脸用例：233 字节整好一帧（不多出一帧 0 字节）。
    // Exact-fit case: 233 bytes is exactly one frame (no trailing empty frame).
    static char exact[233];
    memset(exact, 'x', sizeof(exact));
    CHECK(push_tts_frame_count(exact, sizeof(exact)) == 1);
    CHECK(push_tts_frame_at(exact, sizeof(exact), 0, &slice, &slice_len));
    CHECK(slice_len == 233);
    // 234 字节：第二帧只有 1 字节。/ 234 bytes: the second frame carries 1 byte.
    static char exact2[234];
    memset(exact2, 'y', sizeof(exact2));
    CHECK(push_tts_frame_count(exact2, sizeof(exact2)) == 2);
    CHECK(push_tts_frame_at(exact2, sizeof(exact2), 1, &slice, &slice_len));
    CHECK(slice_len == 1);

    printf("tts plan: budget 233, utf8-safe split, empty=1 frame, exact-fit edges ok\n");
}

/* ---- 26.11.5：三拍判据与「拔卡时手机还连着」 ---- */

// B77 的真值表。原判据 `state == PUSH_STATE_IDLE` 把「连着」当「没停」，于是
// APK 常驻的那台机器上拔卡永远不卸载。这一组的每一行都对应一个真机形状。
static void test_quiesce_predicate(void) {
    s_case = "quiesce-predicate";
    // 26.11.4 现场：手机连着（CONNECTED）、没在传、零文件 -> 第 1 拍就必须放行。
    CHECK(push_quiesce_idle(PUSH_STATE_CONNECTED, false, 0));
    // 其余「握着链路或倒计时、不握介质」的状态同样放行。
    CHECK(push_quiesce_idle(PUSH_STATE_IDLE, false, 0));
    CHECK(push_quiesce_idle(PUSH_STATE_ARMED, false, 0));
    CHECK(push_quiesce_idle(PUSH_STATE_WINDOW, false, 0));
    CHECK(push_quiesce_idle(PUSH_STATE_COOLDOWN, false, 0));
    // 真在传输：仍然等（放宽不能放到数据路径上）。
    CHECK(!push_quiesce_idle(PUSH_STATE_RECEIVING, false, 0));
    CHECK(!push_quiesce_idle(PUSH_STATE_PROCESSING, false, 0));
    // 任何状态 + 还开着一个 FILE* 都不许放行（B51：只数 .part 会漏掉 verify 读句柄）。
    for (int st = PUSH_STATE_IDLE; st <= PUSH_STATE_COOLDOWN; ++st)
        CHECK(!push_quiesce_idle((push_state_t)st, false, 1u));
    // 待武装 = 入口还开着，不算停。
    CHECK(!push_quiesce_idle(PUSH_STATE_IDLE, true, 0));
    CHECK(!push_quiesce_idle(PUSH_STATE_CONNECTED, true, 0));
    printf("quiesce predicate: link!=medium, transfer still waits, any FILE* still waits\n");
}

// 最危险的一档（审计点 3）：**传输中拔卡**。RECEIVING、.part 开着、手机正发包。
// detach 的两拍必须把它收拾到 IDLE：DISCONNECT 回窗口（.part 关、簿记存），
// 再 CANCEL 收摊（不带 DROP——卡已不在，删 .part 只会换来一串 EIO）。
// 收完之后 files 归零，三拍必须成功；半成品簿记保留，插回后照常重挂。
static void test_media_detach_mid_transfer(void) {
    s_case = "media-detach-mid-transfer";
    push_session_init(&s_sess);
    CHECK(feed_arm(0x1234, 60000, 5000, 1000) == PUSH_ACT_ADV_ON);
    CHECK(feed(PUSH_EVT_ADV_UP) == 0);
    CHECK(feed(PUSH_EVT_CONNECT) == 0);
    push_begin_t b = make_begin(4096, 0x1234);
    CHECK(feed_begin(&b) & PUSH_ACT_READY);
    CHECK(feed_data(0, 128) & PUSH_ACT_APPEND);
    CHECK(s_sess.state == PUSH_STATE_RECEIVING);
    CHECK(s_sess.has_part);

    // detach 第一拍：DISCONNECT -> 窗口。
    CHECK(feed(PUSH_EVT_DISCONNECT) == 0);
    CHECK(s_sess.state == PUSH_STATE_WINDOW);
    // detach 第二拍：CANCEL -> IDLE（只有 ADV_OFF，没有 DROP）。
    const uint32_t act = feed(PUSH_EVT_CANCEL);
    CHECK(act == PUSH_ACT_ADV_OFF);
    CHECK(!(act & PUSH_ACT_DROP));
    CHECK(s_sess.state == PUSH_STATE_IDLE);
    // 会话收干净了：三拍必须放行。
    CHECK(push_quiesce_idle(s_sess.state, false, 0));
    // 簿记还在（.part 在卡上），插回后 probe/remount 照常处理。
    CHECK(s_sess.has_part);

    // 对照：手机连着但**没在传**时拔卡，一拍就够（26.11.4 的那条路径）。
    push_session_init(&s_sess);
    CHECK(feed_arm(0x1234, 60000, 5000, 1000) == PUSH_ACT_ADV_ON);
    CHECK(feed(PUSH_EVT_ADV_UP) == 0);
    CHECK(feed(PUSH_EVT_CONNECT) == 0);
    CHECK(feed(PUSH_EVT_DISCONNECT) == 0);
    CHECK(s_sess.state == PUSH_STATE_WINDOW);
    CHECK(push_quiesce_idle(s_sess.state, false, 0));
    CHECK(feed(PUSH_EVT_CANCEL) == PUSH_ACT_ADV_OFF);
    CHECK(s_sess.state == PUSH_STATE_IDLE);
    printf("media detach: mid-transfer and connected-idle both settle to IDLE in two beats\n");
}

int main(void) {
    test_crc32();
    test_ctrl_header();
    test_gen30_bare_dispatch(); // 哨位裁定方案 B：零碰撞 + 长度闸门 / plan B: zero-collision + length gate
    test_begin_codec();
    test_data_status_codec();
    test_py_cross_vector();
    test_ai_frames();
    test_ai_ridealong();
    test_ai_segasm();
    test_tts_plan();
    test_happy_path();
    test_word_card();
    test_resume();
    test_adopt();
    test_part_origin_adopt();
    test_part_origin_fresh();
    test_part_owed_order();
    // 出包门第 4 条「注入三案」：1 = 重试成功不重复不缺（16h）、2 = 欠账保留整包
    // （16g 的连败进槽 + 顺序 + 显式 drain）、3 = 槽接不住 → false/CANCEL+ERR（16i）。
    // Gate #4's three injection cases: 1 = clean retry (16h), 2 = owed keeps the whole
    // packet (16g), 3 = slot cannot take it -> false/CANCEL+ERR (16i).
    test_part_retry_clean();
    test_part_owed_giveup();
    test_quiesce_predicate();
    test_media_detach_mid_transfer();
    test_teeth();
    printf("push_sink_host_test: %d checks, %d red\n", s_checks, s_red);
    return s_red == 0 ? 0 : 1;
}
