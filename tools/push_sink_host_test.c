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

#include "push_proto.h"
#include "push_session.h"
#include "ai_ridealong.h"

static int s_checks;
static int s_red;

#define CHECK(cond)                                                      \
    do {                                                                 \
        ++s_checks;                                                      \
        if (!(cond)) {                                                   \
            ++s_red;                                                     \
            printf("RED line %d: %s\n", __LINE__, #cond);                \
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
    CHECK(PUSH_OP_RESERVED_FIRST == PUSH_OP_AI_CAPS_ENABLE + 1); // 0x0A 紧接 0x09，无空洞
    CHECK(PUSH_OP_RESERVED_FIRST > PUSH_OP_AI_RESULT); // 0x08/0x09 已出预留区 / out of the reserved block
    printf("ctrl header: END/RESUME/ABORT decode, bad frames refuse, gen26 0x06/0x07 + gen26.5 0x08/0x09 registered, 0x0A-0x0F reserved ok\n");
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

    // AI_RESULT（0x07）编解码往返：固定 word 槽 + def 长度(u16 LE)。
    // AI_RESULT (0x07) round-trip: fixed word slot plus def length (u16 LE).
    push_ai_result_t r;
    memset(&r, 0, sizeof(r));
    snprintf(r.word, sizeof(r.word), "ephemeral");
    snprintf(r.def, sizeof(r.def), "lasting a very short time");
    uint8_t rf[PUSH_AI_RESULT_MAX];
    size_t rn = push_ai_result_encode(&r, rf, sizeof(rf));
    CHECK(rn == PUSH_AI_RESULT_MAX);
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

int main(void) {
    test_crc32();
    test_ctrl_header();
    test_begin_codec();
    test_data_status_codec();
    test_py_cross_vector();
    test_ai_frames();
    test_ai_ridealong();
    test_happy_path();
    test_word_card();
    test_resume();
    test_adopt();
    test_teeth();
    printf("push_sink_host_test: %d checks, %d red\n", s_checks, s_red);
    return s_red == 0 ? 0 : 1;
}
