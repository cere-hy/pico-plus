/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：push_proto.h 的实现。字节序显式小端，不用 struct  packing——跨编译器与
 * 宿主/目标一致，ASan 宿主测试逐字节对表。
 * English: implementation of push_proto.h. Explicit little-endian fields and no
 * struct packing: identical across compilers and between host and target, so the
 * ASan host test compares byte for byte.
 *
 * 冻结：同 push_proto.h。/ Frozen: same contract as push_proto.h.
 */

#include "push_proto.h"

#include <stdbool.h>
#include <string.h>

/* ---- CRC32 ---- */

static uint32_t s_crc_table[256];
static bool s_crc_ready;

static void crc_table_build(void) {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        s_crc_table[i] = c;
    }
    s_crc_ready = true;
}

uint32_t push_crc32(const void *data, size_t len) { return push_crc32_continue(0u, data, len); }

uint32_t push_crc32_continue(uint32_t crc, const void *data, size_t len) {
    if (!s_crc_ready) crc_table_build();
    // 传入的是"已出盒"值，先退回内部态再喂字节，出口再封箱。
    // The incoming value is already finalised; undo that, feed, then seal again.
    uint32_t c = crc ^ 0xFFFFFFFFu;
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; ++i) c = s_crc_table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ---- 小端助手 / little-endian helpers ---- */

static void put_u16(uint8_t *out, uint16_t v) {
    out[0] = (uint8_t)(v & 0xFFu);
    out[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_u32(uint8_t *out, uint32_t v) {
    out[0] = (uint8_t)(v & 0xFFu);
    out[1] = (uint8_t)((v >> 8) & 0xFFu);
    out[2] = (uint8_t)((v >> 16) & 0xFFu);
    out[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint16_t get_u16(const uint8_t *in) {
    return (uint16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8));
}

static uint32_t get_u32(const uint8_t *in) {
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

/* ---- BEGIN / BEGIN codec ---- */

size_t push_begin_encode(const push_begin_t *begin, uint8_t *out, size_t cap) {
    if (!begin || !out || cap < PUSH_BEGIN_LEN) return 0;
    memset(out, 0, PUSH_BEGIN_LEN);
    out[0] = PUSH_MAGIC0;
    out[1] = PUSH_MAGIC1;
    out[2] = PUSH_OP_BEGIN;
    out[3] = PUSH_VERSION;
    out[4] = begin->type;
    put_u16(out + 5, begin->token);
    put_u32(out + 7, begin->total_len);
    put_u32(out + 11, begin->crc32);
    put_u32(out + 15, begin->session_id);
    // 标题截断保 0：调用方给的 title 必须已以 0 结尾，这里只防越界。
    // Truncate with NUL: callers pass NUL-terminated titles; this only guards the copy.
    size_t n = strnlen(begin->title, PUSH_TITLE_MAX);
    if (n >= PUSH_TITLE_MAX) n = PUSH_TITLE_MAX - 1;
    memcpy(out + 19, begin->title, n);
    return PUSH_BEGIN_LEN;
}

bool push_begin_decode(const uint8_t *in, size_t len, push_begin_t *begin) {
    if (!in || !begin || len < PUSH_BEGIN_LEN) return false;
    if (in[0] != PUSH_MAGIC0 || in[1] != PUSH_MAGIC1 || in[2] != PUSH_OP_BEGIN) return false;
    if (in[3] != PUSH_VERSION) return false;
    memset(begin, 0, sizeof(*begin));
    begin->type = in[4];
    begin->token = get_u16(in + 5);
    begin->total_len = get_u32(in + 7);
    begin->crc32 = get_u32(in + 11);
    begin->session_id = get_u32(in + 15);
    memcpy(begin->title, in + 19, PUSH_TITLE_MAX);
    begin->title[PUSH_TITLE_MAX - 1] = 0;
    return true;
}

/* ---- DATA / DATA codec ---- */

bool push_data_decode(const uint8_t *in, size_t len, uint16_t *seq, const uint8_t **payload, size_t *payload_len) {
    if (!in || len < PUSH_DATA_HDR_LEN) return false;
    const size_t n = (size_t)in[2];
    if (len < PUSH_DATA_HDR_LEN + n) return false;
    if (seq) *seq = get_u16(in);
    if (payload) *payload = in + PUSH_DATA_HDR_LEN;
    if (payload_len) *payload_len = n;
    return true;
}

/* ---- CTRL 帧头 / CTRL header ---- */

bool push_ctrl_header_decode(const uint8_t *in, size_t len, uint8_t *op) {
    if (!in || len < 4u) return false;
    if (in[0] != PUSH_MAGIC0 || in[1] != PUSH_MAGIC1) return false;
    if (in[3] != PUSH_VERSION) return false;
    if (op) *op = in[2];
    return true;
}

/* ---- gen30 伴读：CTRL 裸帧白名单分派（哨位裁定方案 B）---- */
/* Read-along gen30: bare CTRL frame whitelist dispatch (sentinel ruling, plan B). */
/*
 * 白名单只有两条：op 必须是 0x0A/0x0B，且总长必须恰等于该帧 R0 定长（241/4 B）。
 * 长度不符（哪怕首字节是 0x0A）一律返回 false，交回现制 4 B 魔数校验并落 ERR(2)——
 * 这就是「长度闸门」，也是「未登记 op 走 ERR(2)」纪律零破坏的关键：0x0A/0x0B 已
 * 登记，但只有合规裸帧才被认，畸形帧仍按坏帧处理。现制帧首字节固定 0x50，
 * 与本白名单零交集，故既有 CTRL 路径逐字不变（编译期零碰撞断言见 push_proto.h）。
 *
 * The whitelist has exactly two entries: op is 0x0A/0x0B and the total length
 * equals that frame's frozen R0 length (241/4 B). Any other length - even with a
 * 0x0A first byte - returns false, falls through to the legacy 4 B magic check and
 * lands in ERR(2): this is the length gate, and why the "unregistered op -> ERR(2)"
 * discipline survives (0x0A/0x0B are registered, but only well-formed bare frames
 * are accepted). Legacy frames start with 0x50, disjoint from this whitelist, so
 * the existing CTRL path is byte-for-byte unchanged (compile-time non-collision
 * asserts in push_proto.h).
 */
bool push_gen30_ctrl_bare_op(const uint8_t *in, size_t len, uint8_t *op) {
    if (!in || len == 0u) return false;
    if (in[0] == (uint8_t)PUSH_OP_SUMMARY && len == PUSH_OP_SUMMARY_LEN) {
        if (op) *op = (uint8_t)PUSH_OP_SUMMARY;
        return true;
    }
    if (in[0] == (uint8_t)PUSH_OP_TTS_DONE && len == PUSH_OP_TTS_DONE_LEN) {
        if (op) *op = (uint8_t)PUSH_OP_TTS_DONE;
        return true;
    }
    return false;
}

/* ---- STATUS / STATUS codec ---- */

size_t push_status_encode(uint8_t opcode, const uint8_t *body, size_t body_len, uint8_t *out, size_t cap) {
    if (!out || body_len > 12u) return 0;
    if (cap < 1u + body_len) return 0;
    out[0] = opcode;
    if (body_len) memcpy(out + 1, body, body_len);
    return 1u + body_len;
}

/* ---- AI 查词帧 / AI lookup frames ---- */

size_t push_ai_query_encode(const push_ai_query_t *q, uint8_t *out, size_t cap) {
    if (!q || !out || cap < PUSH_AI_QUERY_MAX) return 0;
    memset(out, 0, PUSH_AI_QUERY_MAX);
    out[0] = PUSH_MAGIC0;
    out[1] = PUSH_MAGIC1;
    out[2] = PUSH_OP_AI_QUERY;
    out[3] = PUSH_VERSION;
    size_t n = strnlen(q->word, PUSH_AI_WORD_MAX);
    if (n >= PUSH_AI_WORD_MAX) n = PUSH_AI_WORD_MAX - 1; // 保 0 / keep NUL
    out[4] = (uint8_t)n;
    memcpy(out + 5, q->word, n);
    return PUSH_AI_QUERY_MAX;
}

bool push_ai_query_decode(const uint8_t *in, size_t len, push_ai_query_t *q) {
    if (!in || !q || len < PUSH_AI_QUERY_MAX) return false;
    if (in[0] != PUSH_MAGIC0 || in[1] != PUSH_MAGIC1 || in[2] != PUSH_OP_AI_QUERY) return false;
    if (in[3] != PUSH_VERSION) return false;
    memset(q, 0, sizeof(*q));
    const size_t n = in[4];
    if (n >= PUSH_AI_WORD_MAX) return false; // 长度越界 / length out of range
    memcpy(q->word, in + 5, n);
    q->word[n] = 0;
    return true;
}

size_t push_ai_result_encode(const push_ai_result_t *r, uint8_t *out, size_t cap) {
    // 产冻结的 241B 线格式帧：槽位恒 170（不随 PUSH_AI_DEF_MAX 膨胀，见
    // PUSH_AI_RESULT_WIRE_LEN 注释里的 D-1 事故）。/ Emit the frozen 241B wire
    // frame: a constant 170-byte slot (it does NOT grow with PUSH_AI_DEF_MAX - see
    // the D-1 incident in PUSH_AI_RESULT_WIRE_LEN's comment).
    if (!r || !out || cap < PUSH_AI_RESULT_WIRE_LEN) return 0;
    memset(out, 0, PUSH_AI_RESULT_WIRE_LEN);
    out[0] = PUSH_MAGIC0;
    out[1] = PUSH_MAGIC1;
    out[2] = PUSH_OP_AI_RESULT;
    out[3] = PUSH_VERSION;
    size_t wn = strnlen(r->word, PUSH_AI_WORD_MAX);
    if (wn >= PUSH_AI_WORD_MAX) wn = PUSH_AI_WORD_MAX - 1;
    // 释义长度按**线格式槽 170** 钳制：帧只有 241B，按容量 768 取值会写越界。
    // Clamp to the WIRE slot (170): the frame is only 241B, sizing by the 768-byte
    // capacity would write out of bounds.
    size_t dn = strnlen(r->def, PUSH_AI_DEF_WIRE_MAX);
    if (dn >= PUSH_AI_DEF_WIRE_MAX) dn = PUSH_AI_DEF_WIRE_MAX - 1;
    out[4] = (uint8_t)wn;
    memcpy(out + 5, r->word, wn);
    // word 区固定占 PUSH_AI_WORD_MAX 槽位（填满 0），释义长度(u16 LE)接在后面。
    // The word field occupies a fixed PUSH_AI_WORD_MAX slot (zero-filled); the
    // definition length (u16 LE) follows.
    const size_t off = 5u + PUSH_AI_WORD_MAX;
    put_u16(out + off, (uint16_t)dn);
    memcpy(out + off + 2u, r->def, dn);
    return PUSH_AI_RESULT_WIRE_LEN;
}

bool push_ai_result_decode(const uint8_t *in, size_t len, push_ai_result_t *r) {
    // 最小长度锚**冻结线格式** 241B（不是随容量膨胀的 PUSH_AI_RESULT_MAX——
    // 那就是 gen26.6.1 D-1：把最大当最小，短释义全灭）。
    // The minimum length is pinned to the FROZEN wire frame (241B), not to the
    // capacity-derived PUSH_AI_RESULT_MAX - that was gen26.6.1 D-1: a maximum used
    // as a minimum killed every short definition.
    if (!in || !r || len < PUSH_AI_RESULT_WIRE_LEN) return false;
    if (in[0] != PUSH_MAGIC0 || in[1] != PUSH_MAGIC1 || in[2] != PUSH_OP_AI_RESULT) return false;
    if (in[3] != PUSH_VERSION) return false;
    memset(r, 0, sizeof(*r));
    const size_t wn = in[4];
    if (wn >= PUSH_AI_WORD_MAX) return false;
    memcpy(r->word, in + 5, wn);
    r->word[wn] = 0;
    const size_t off = 5u + PUSH_AI_WORD_MAX;
    const size_t dn = get_u16(in + off);
    // 0x07 的 def 槽冻结 170：dn=170 装不下 NUL，属协议违规，拒收。
    // The 0x07 def slot is frozen at 170: dn=170 leaves no room for the NUL, so a
    // frame claiming it is a protocol violation and is rejected.
    if (dn >= PUSH_AI_DEF_WIRE_MAX) return false;
    memcpy(r->def, in + off + 2u, dn);
    r->def[dn] = 0;
    return true;
}

size_t push_st_ai_query_encode(const char *word, uint8_t *out, size_t cap) {
    if (!word || !out || cap < 2u + PUSH_AI_WORD_MAX) return 0;
    memset(out, 0, 2u + PUSH_AI_WORD_MAX);
    out[0] = PUSH_ST_AI_QUERY;
    size_t n = strnlen(word, PUSH_AI_WORD_MAX);
    if (n >= PUSH_AI_WORD_MAX) n = PUSH_AI_WORD_MAX - 1;
    out[1] = (uint8_t)n;
    memcpy(out + 2, word, n);
    return 2u + PUSH_AI_WORD_MAX;
}

/* ---- gen26.5 上下文档 / contextual frames (gen26.5) ---- */

size_t push_ai_query_ctx_encode(const push_ai_query_ctx_t *q, uint8_t *out, size_t cap) {
    if (!q || !out || cap < PUSH_AI_CTX_QUERY_MAX) return 0;
    memset(out, 0, PUSH_AI_CTX_QUERY_MAX);
    out[0] = PUSH_MAGIC0;
    out[1] = PUSH_MAGIC1;
    out[2] = PUSH_OP_AI_QUERY;
    out[3] = PUSH_VERSION;
    size_t n = strnlen(q->word, PUSH_AI_WORD_MAX);
    if (n >= PUSH_AI_WORD_MAX) n = PUSH_AI_WORD_MAX - 1;
    out[4] = (uint8_t)n;
    memcpy(out + 5, q->word, n);
    const size_t off = 5u + PUSH_AI_WORD_MAX;
    put_u32(out + off, q->book_id);
    put_u32(out + off + 4u, q->pos);
    return PUSH_AI_CTX_QUERY_MAX;
}

bool push_ai_query_ctx_decode(const uint8_t *in, size_t len, push_ai_query_ctx_t *q) {
    if (!in || !q || len != PUSH_AI_CTX_QUERY_MAX) return false; // 严格 77B；旧 69B 帧走 legacy 解码
    if (in[0] != PUSH_MAGIC0 || in[1] != PUSH_MAGIC1 || in[2] != PUSH_OP_AI_QUERY) return false;
    if (in[3] != PUSH_VERSION) return false;
    memset(q, 0, sizeof(*q));
    const size_t n = in[4];
    if (n >= PUSH_AI_WORD_MAX) return false;
    memcpy(q->word, in + 5, n);
    q->word[n] = 0;
    const size_t off = 5u + PUSH_AI_WORD_MAX;
    q->book_id = get_u32(in + off);
    q->pos = get_u32(in + off + 4u);
    return true;
}

size_t push_st_ai_query_ctx_encode(const char *word, uint32_t book_id, uint32_t pos,
                                   uint8_t *out, size_t cap) {
    if (!word || !out || cap < PUSH_ST_AI_CTX_QUERY_LEN) return 0;
    memset(out, 0, PUSH_ST_AI_CTX_QUERY_LEN);
    out[0] = PUSH_ST_AI_QUERY;
    size_t n = strnlen(word, PUSH_AI_WORD_MAX);
    if (n >= PUSH_AI_WORD_MAX) n = PUSH_AI_WORD_MAX - 1;
    out[1] = (uint8_t)n;
    memcpy(out + 2, word, n);
    put_u32(out + 2u + PUSH_AI_WORD_MAX, book_id);
    put_u32(out + 2u + PUSH_AI_WORD_MAX + 4u, pos);
    return PUSH_ST_AI_CTX_QUERY_LEN;
}

/* ---- gen26.5 释义分帧 / segmented definitions (gen26.5) ---- */

size_t push_st_ai_query_emit(const char *word, uint32_t book_id, uint32_t pos,
                             bool ctx_enabled, uint8_t *out, size_t cap) {
    if (!word || !out) return 0;
    // 上下文档只在「APK 声明支持」且「确有书内上下文」时发出；其余一律 legacy。
    // Contextual only when the APK opted in AND a real in-book context exists.
    if (ctx_enabled && book_id != 0u) {
        return push_st_ai_query_ctx_encode(word, book_id, pos, out, cap);
    }
    return push_st_ai_query_encode(word, out, cap);
}

size_t push_ai_seg_encode(const push_ai_seg_t *s, uint8_t *out, size_t cap) {
    if (!s || !out || cap < PUSH_AI_SEG_FRAME_MAX) return 0;
    if (s->seg_total == 0 || s->seg_idx >= s->seg_total) return 0; // 序号须在界内
    memset(out, 0, PUSH_AI_SEG_FRAME_MAX);
    out[0] = PUSH_MAGIC0;
    out[1] = PUSH_MAGIC1;
    out[2] = PUSH_OP_AI_RESULT_SEG;
    out[3] = PUSH_VERSION;
    size_t wn = strnlen(s->word, PUSH_AI_WORD_MAX);
    if (wn >= PUSH_AI_WORD_MAX) wn = PUSH_AI_WORD_MAX - 1;
    out[4] = (uint8_t)wn;
    memcpy(out + 5, s->word, wn);
    const size_t off = 5u + PUSH_AI_WORD_MAX;
    put_u16(out + off, s->seg_idx);
    put_u16(out + off + 2u, s->seg_total);
    size_t dn = strnlen(s->seg, PUSH_AI_SEG_MAX);
    if (dn >= PUSH_AI_SEG_MAX) dn = PUSH_AI_SEG_MAX - 1; // 保 NUL，与 word/def 槽同构
    put_u16(out + off + 4u, (uint16_t)dn);
    memcpy(out + off + 6u, s->seg, dn);
    return PUSH_AI_SEG_FRAME_MAX;
}

bool push_ai_seg_decode(const uint8_t *in, size_t len, push_ai_seg_t *s) {
    if (!in || !s || len != PUSH_AI_SEG_FRAME_MAX) return false;
    if (in[0] != PUSH_MAGIC0 || in[1] != PUSH_MAGIC1 || in[2] != PUSH_OP_AI_RESULT_SEG) return false;
    if (in[3] != PUSH_VERSION) return false;
    memset(s, 0, sizeof(*s));
    const size_t wn = in[4];
    if (wn >= PUSH_AI_WORD_MAX) return false;
    memcpy(s->word, in + 5, wn);
    s->word[wn] = 0;
    const size_t off = 5u + PUSH_AI_WORD_MAX;
    s->seg_idx = get_u16(in + off);
    s->seg_total = get_u16(in + off + 2u);
    const size_t dn = get_u16(in + off + 4u);
    if (s->seg_total == 0 || s->seg_idx >= s->seg_total) return false; // 序号须在界内
    if (dn >= PUSH_AI_SEG_MAX) return false; // 长度越界（槽保 NUL，同 word/def 模式）
    memcpy(s->seg, in + off + 6u, dn);
    s->seg[dn] = 0;
    return true;
}

/* ---- gen26.5 能力使能帧 / capability enable frame (gen26.5) ---- */

size_t push_ai_caps_enable_encode(const push_caps_t *caps, uint8_t *out, size_t cap) {
    if (!caps || !out || cap < PUSH_AI_CAPS_ENABLE_LEN) return 0;
    out[0] = PUSH_MAGIC0;
    out[1] = PUSH_MAGIC1;
    out[2] = PUSH_OP_AI_CAPS_ENABLE;
    out[3] = PUSH_VERSION;
    put_u32(out + 4, caps->bits);
    return PUSH_AI_CAPS_ENABLE_LEN;
}

bool push_ai_caps_enable_decode(const uint8_t *in, size_t len, push_caps_t *caps) {
    if (!in || !caps || len != PUSH_AI_CAPS_ENABLE_LEN) return false; // 严格 8B
    if (in[0] != PUSH_MAGIC0 || in[1] != PUSH_MAGIC1 || in[2] != PUSH_OP_AI_CAPS_ENABLE) return false;
    if (in[3] != PUSH_VERSION) return false;
    caps->bits = get_u32(in + 4);
    return true;
}

/* ---- M3 情景释义分帧 / scene lookup segments (M3) ---- */

// UTF-8 安全切分：从 text 起最多 budget 字节，整字符边界收刀。首字节非法返回 0，
// 调用方强制前进 1 字节保进度（与 0x19 的分片器同一策略）。
// UTF-8 safe slice: at most budget bytes, cut on a character boundary. An illegal
// lead byte returns 0 and the caller forces 1 byte of progress (same policy as 0x19).
static size_t scene_utf8_fit(const char *text, size_t len, size_t budget) {
    if (!text || budget == 0) return 0;
    if (len > budget) len = budget;
    size_t n = 0;
    while (n < len) {
        const unsigned char c = (unsigned char)text[n];
        size_t char_len = 0;
        if ((c & 0x80u) == 0x00u) char_len = 1;
        else if ((c & 0xE0u) == 0xC0u) char_len = 2;
        else if ((c & 0xF0u) == 0xE0u) char_len = 3;
        else if ((c & 0xF8u) == 0xF0u) char_len = 4;
        else break;
        if (n + char_len > len) break;
        n += char_len;
    }
    return n;
}

// 词长钳到 63：留一格给 decode 侧的 NUL 语义（槽 64）。/ Clamp to 63, keeping the slot NUL-safe.
static size_t scene_word_len(const char *word) {
    if (!word) return 0;
    size_t n = strnlen(word, PUSH_SCENE_WORD_MAX);
    if (n >= PUSH_SCENE_WORD_MAX) n = PUSH_SCENE_WORD_MAX - 1u;
    return n;
}

/// 首帧 ctx 预算 = 234 - (word_len1 + word + off2 + flags1)。/ First-frame context budget.
static size_t scene_first_budget(const char *word) {
    return PUSH_ST_SCENE_SLOT - (4u + scene_word_len(word));
}

uint16_t push_scene_frame_count(const char *word, const char *ctx, size_t ctx_len) {
    if (!ctx || ctx_len == 0) return 1; // 空上下文一帧（调用方决定发不发）/ empty = one frame
    size_t off = 0;
    uint16_t count = 0;
    size_t budget = scene_first_budget(word);
    while (off < ctx_len && count < PUSH_SCENE_MAX_FRAMES) {
        const size_t step = scene_utf8_fit(ctx + off, ctx_len - off, budget);
        off += step ? step : 1;
        ++count;
        budget = PUSH_ST_SCENE_SLOT;
    }
    return count ? count : 1;
}

bool push_scene_frame_at(const char *word, const char *ctx, size_t ctx_len, uint16_t i,
                         const char **slice, size_t *slice_len) {
    if (!ctx) return false;
    size_t off = 0;
    uint16_t idx = 0;
    size_t budget = scene_first_budget(word);
    while (off < ctx_len && idx < PUSH_SCENE_MAX_FRAMES) {
        const size_t step = scene_utf8_fit(ctx + off, ctx_len - off, budget);
        const size_t take = step ? step : 1;
        if (idx == i) {
            if (slice) *slice = ctx + off;
            if (slice_len) *slice_len = take;
            return true;
        }
        off += take;
        ++idx;
        budget = PUSH_ST_SCENE_SLOT;
    }
    return false;
}

size_t push_st_scene_encode(const char *word, uint16_t word_off, uint8_t flags,
                            uint16_t seq, uint16_t total,
                            const char *slice, size_t slice_len,
                            uint8_t *out, size_t cap) {
    if (!out || cap < PUSH_ST_SCENE_FRAME_MAX) return 0;
    if (total == 0 || total > PUSH_SCENE_MAX_FRAMES || seq >= total) return 0;
    const size_t wl = scene_word_len(word);
    if (slice_len > PUSH_ST_SCENE_SLOT) return 0;
    const size_t payload = (seq == 0) ? (4u + wl + slice_len) : slice_len;
    if (payload > PUSH_ST_SCENE_SLOT) return 0;

    memset(out, 0, PUSH_ST_SCENE_FRAME_MAX);
    out[0] = PUSH_ST_AI_SCENE;
    put_u16(out + 1, seq);
    put_u16(out + 3, total);
    put_u16(out + 5, (uint16_t)payload);

    uint8_t *p = out + 7;
    if (seq == 0) {
        p[0] = (uint8_t)wl;
        if (wl) memcpy(p + 1, word, wl);
        put_u16(p + 1 + wl, word_off);
        p[3 + wl] = flags;
        if (slice_len) memcpy(p + 4 + wl, slice, slice_len);
    } else if (slice_len) {
        memcpy(p, slice, slice_len);
    }
    return PUSH_ST_SCENE_FRAME_MAX;
}

bool push_st_scene_decode(const uint8_t *in, size_t len, push_scene_frame_t *f) {
    if (!in || !f || len < PUSH_ST_SCENE_FRAME_MAX) return false;
    if (in[0] != PUSH_ST_AI_SCENE) return false;
    memset(f, 0, sizeof(*f));
    f->seq = get_u16(in + 1);
    f->total = get_u16(in + 3);
    const uint16_t payload = get_u16(in + 5);
    if (f->total == 0 || f->total > PUSH_SCENE_MAX_FRAMES || f->seq >= f->total) return false;
    if (payload > PUSH_ST_SCENE_SLOT) return false;
    const uint8_t *p = in + 7;
    if (f->seq == 0) {
        if (payload < 4) return false;
        const uint8_t wl = p[0];
        if (wl >= PUSH_SCENE_WORD_MAX || 4u + wl > payload) return false;
        f->word_len = wl;
        if (wl) memcpy(f->word, p + 1, wl);
        f->word_off = get_u16(p + 1 + wl);
        f->flags = p[3 + wl];
        f->len = (uint16_t)(payload - (4u + wl));
        if (f->len) memcpy(f->ctx, p + 4 + wl, f->len);
    } else {
        f->len = payload;
        if (payload) memcpy(f->ctx, p, payload);
    }
    return true;
}

/* ---- 能力位图 / Capability bitmap ---- */

size_t push_st_tts_text_encode(const push_tts_text_t *t, uint8_t *out, size_t cap) {
    if (!t || !out || cap < PUSH_ST_TTS_TEXT_FRAME_MAX) return 0;
    if (t->total == 0 || t->seq >= t->total) return 0; // 序号须在界内
    memset(out, 0, PUSH_ST_TTS_TEXT_FRAME_MAX);
    out[0] = PUSH_ST_TTS_TEXT;
    put_u16(out + 1, t->seq);
    put_u16(out + 3, t->total);
    size_t n = strnlen(t->text, PUSH_TTS_TEXT_MAX);
    if (n >= PUSH_TTS_TEXT_MAX) n = PUSH_TTS_TEXT_MAX - 1; // 保 NUL，与 word/def/seg 槽同构
    put_u16(out + 5, (uint16_t)n);
    memcpy(out + 7, t->text, n);
    return PUSH_ST_TTS_TEXT_FRAME_MAX;
}

bool push_st_tts_text_decode(const uint8_t *in, size_t len, push_tts_text_t *t) {
    if (!in || !t || len != PUSH_ST_TTS_TEXT_FRAME_MAX) return false; // 严格定长
    if (in[0] != PUSH_ST_TTS_TEXT) return false;
    memset(t, 0, sizeof(*t));
    t->seq = get_u16(in + 1);
    t->total = get_u16(in + 3);
    const size_t n = get_u16(in + 5);
    if (t->total == 0 || t->seq >= t->total) return false;
    if (n >= PUSH_TTS_TEXT_MAX) return false;
    memcpy(t->text, in + 7, n);
    t->text[n] = 0;
    t->len = (uint16_t)n;
    return true;
}

size_t push_caps_encode(const push_caps_t *caps, uint8_t *out, size_t cap) {
    if (!caps || !out || cap < 5u) return 0; // op(1) + u32(4)
    out[0] = PUSH_ST_CAPS;
    put_u32(out + 1, caps->bits);
    return 5u;
}

bool push_caps_decode(const uint8_t *in, size_t len, push_caps_t *caps) {
    if (!in || !caps || len < 5u) return false;
    if (in[0] != PUSH_ST_CAPS) return false;
    caps->bits = get_u32(in + 1);
    return true;
}
