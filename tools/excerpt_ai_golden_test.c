/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 26.9.26 M5-A S26-3 · 0x1A bit2（FLAG_EXCERPT）黄金向量。
 * Golden vectors for the 0x1A bit2 excerpt request.
 *
 * **期望字节是手推的，不是从实现里打印出来再粘回来的。**
 * 规则只有一条：手推的与固件输出不一致时，先怀疑实现。
 * The expected bytes below were computed BY HAND from the protocol layout - not printed
 * out of the encoder and pasted back. When they disagree, suspect the implementation first.
 *
 * 帧布局（spec，docs/PROTOCOL.md 与 push_proto.h 同源，241B 定长、零填充）：
 *   [0]      opcode 0x1A
 *   [1..2]   seq   （LE u16，从 0 起）
 *   [3..4]   total （LE u16）
 *   [5..6]   payload 长度（LE u16）= 4 + word_len + ctx_len（首帧）
 *   [7]      word_len
 *   [8..]    word（word_len 字节，无 NUL）
 *   [+0..+1] word_off（LE u16）
 *   [+2]     flags
 *   [+3..]   ctx 切片
 *   余下补零到 241
 * Frame layout: opcode, seq, total, payload length, word_len, word, word_off, flags,
 * context slice, zero padding up to 241 bytes.
 *
 * Build/run: tools/run_excerpt_ai_golden.sh   (prints EXCERPT_AI_GOLDEN_PASS)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "push_proto.h"

static int s_checks;

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++s_checks;                                                            \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

/// 位号冻结：登记簿写的是 bit2，值就必须是 0x04。改位号 = 改协议，必须走登记簿。
/// The registry says bit2, so the value must be 0x04. Moving the bit is a protocol change.
_Static_assert(PUSH_SCENE_FLAG_EXCERPT == 4u, "FLAG_EXCERPT must stay bit2 (0x04)");
_Static_assert(PUSH_ST_AI_SCENE == 0x1Au, "the excerpt request rides the existing 0x1A opcode");
_Static_assert(PUSH_ST_SCENE_FRAME_MAX == 241u, "0x1A frames are fixed at 241 bytes");

static void dump_diff(const char *what, const uint8_t *got, const uint8_t *want, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (got[i] == want[i]) continue;
        fprintf(stderr, "%s: first mismatch at byte %zu: got 0x%02X, spec says 0x%02X\n",
                what, i, got[i], want[i]);
        return;
    }
    fprintf(stderr, "%s: no byte differs but the compare failed (length?)\n", what);
}

/* ---- 向量 1：单帧、短文本 ---------------------------------------------------
 * word = "读"（U+8BFB → E8 AF BB，3B），word_off = 0，flags = 0x04（bit2），
 * ctx  = "读书破万卷。"（6×3B = 18B）
 *   读 E8 AF BB  书 E4 B9 A6  破 E7 A0 B4  万 E4 B8 87  卷 E5 8D B7  。E3 80 82
 * payload = 4 + 3 + 18 = 25 = 0x19；total = 1。
 * -------------------------------------------------------------------------- */
static void test_single_frame_golden(void) {
    static const char ctx[] = "读书破万卷。";
    CHECK(strlen(ctx) == 18);

    uint8_t want[PUSH_ST_SCENE_FRAME_MAX];
    memset(want, 0, sizeof(want));
    want[0] = 0x1A;
    want[1] = 0x00; want[2] = 0x00;                 // seq = 0
    want[3] = 0x01; want[4] = 0x00;                 // total = 1
    want[5] = 0x19; want[6] = 0x00;                 // payload = 25
    want[7] = 0x03;                                 // word_len = 3
    want[8] = 0xE8; want[9] = 0xAF; want[10] = 0xBB; // "读"
    want[11] = 0x00; want[12] = 0x00;               // word_off = 0
    want[13] = 0x04;                                // flags = bit2
    static const uint8_t ctx_bytes[18] = {
        0xE8, 0xAF, 0xBB, 0xE4, 0xB9, 0xA6, 0xE7, 0xA0, 0xB4,
        0xE4, 0xB8, 0x87, 0xE5, 0x8D, 0xB7, 0xE3, 0x80, 0x82,
    };
    memcpy(want + 14, ctx_bytes, sizeof(ctx_bytes));
    // 14 + 18 = 32；[32,241) 全零（定长零填充）。

    uint8_t got[PUSH_ST_SCENE_FRAME_MAX];
    const size_t n = push_st_scene_encode("读", 0u, PUSH_SCENE_FLAG_EXCERPT, 0u, 1u,
                                          ctx, sizeof(ctx) - 1u, got, sizeof(got));
    CHECK(n == 241);
    const int same = memcmp(got, want, sizeof(want));
    if (same != 0) dump_diff("vector 1", got, want, sizeof(want));
    CHECK(same == 0);

    // 解码回读：字段必须与手推一致，且 word 是 ctx 的前缀（word_off 恒为 0 的语义）。
    push_scene_frame_t f;
    CHECK(push_st_scene_decode(got, n, &f));
    CHECK(f.seq == 0 && f.total == 1);
    CHECK(f.word_len == 3 && memcmp(f.word, "读", 3) == 0);
    CHECK(f.word_off == 0);
    CHECK((f.flags & PUSH_SCENE_FLAG_EXCERPT) != 0);
    CHECK(f.len == 18 && memcmp(f.ctx, ctx, 18) == 0);
    CHECK(memcmp(f.ctx, f.word, f.word_len) == 0);
}

/* ---- 向量 2：两帧、首帧塞满 -------------------------------------------------
 * word = "ab"（2B）→ 首帧 ctx 预算 = 234 - (4 + 2) = 228。
 * ctx = 228 个 'x' + "灯"（E7 81 AF）共 231B → 2 帧：
 *   帧0：seq=0 total=2 payload=4+2+228=234=0xEA，ctx 恰好填满 [13,241)
 *   帧1：seq=1 total=2 payload=3，ctx = E7 81 AF，其后补零
 * -------------------------------------------------------------------------- */
static void test_two_frames_golden(void) {
    char ctx[231];
    memset(ctx, 'x', 228);
    ctx[228] = (char)0xE7; ctx[229] = (char)0x81; ctx[230] = (char)0xAF; // "灯"

    CHECK(push_scene_frame_count("ab", ctx, sizeof(ctx)) == 2);

    /* 帧 0：手推 */
    uint8_t want0[PUSH_ST_SCENE_FRAME_MAX];
    memset(want0, 'x', sizeof(want0)); // 先铺满，再逐字节改头；[13,241) 本来就是 228 个 'x'
    want0[0] = 0x1A;
    want0[1] = 0x00; want0[2] = 0x00;   // seq = 0
    want0[3] = 0x02; want0[4] = 0x00;   // total = 2
    want0[5] = 0xEA; want0[6] = 0x00;   // payload = 234
    want0[7] = 0x02;                    // word_len = 2
    want0[8] = 'a';  want0[9] = 'b';
    want0[10] = 0x00; want0[11] = 0x00; // word_off = 0
    want0[12] = 0x04;                   // flags = bit2

    /* 帧 1：手推 */
    uint8_t want1[PUSH_ST_SCENE_FRAME_MAX];
    memset(want1, 0, sizeof(want1));
    want1[0] = 0x1A;
    want1[1] = 0x01; want1[2] = 0x00;   // seq = 1
    want1[3] = 0x02; want1[4] = 0x00;   // total = 2
    want1[5] = 0x03; want1[6] = 0x00;   // payload = 3
    want1[7] = 0xE7; want1[8] = 0x81; want1[9] = 0xAF; // "灯"

    uint8_t got[PUSH_ST_SCENE_FRAME_MAX];
    const char *slice = NULL;
    size_t slice_len = 0;

    CHECK(push_scene_frame_at("ab", ctx, sizeof(ctx), 0, &slice, &slice_len));
    CHECK(slice_len == 228);
    CHECK(push_st_scene_encode("ab", 0u, PUSH_SCENE_FLAG_EXCERPT, 0u, 2u,
                               slice, slice_len, got, sizeof(got)) == 241);
    if (memcmp(got, want0, sizeof(want0)) != 0) dump_diff("vector 2 frame 0", got, want0, sizeof(want0));
    CHECK(memcmp(got, want0, sizeof(want0)) == 0);

    CHECK(push_scene_frame_at("ab", ctx, sizeof(ctx), 1, &slice, &slice_len));
    CHECK(slice_len == 3);
    CHECK(push_st_scene_encode("ab", 0u, PUSH_SCENE_FLAG_EXCERPT, 1u, 2u,
                               slice, slice_len, got, sizeof(got)) == 241);
    if (memcmp(got, want1, sizeof(want1)) != 0) dump_diff("vector 2 frame 1", got, want1, sizeof(want1));
    CHECK(memcmp(got, want1, sizeof(want1)) == 0);
}

/* ---- 向量 3：标志位只动 bit2 ------------------------------------------------
 * 截断（bit0）+ 整段（bit2）同时为真时，字节就是 0x05；
 * 后续帧的 payload 里没有 flags 字段，写多少都不该出现——这里验证它确实不存在。
 * -------------------------------------------------------------------------- */
static void test_flag_composition(void) {
    static const char ctx[] = "abc";
    uint8_t got[PUSH_ST_SCENE_FRAME_MAX];
    const uint8_t flags = (uint8_t)(PUSH_SCENE_FLAG_EXCERPT | PUSH_SCENE_FLAG_CTX_TRUNCATED);
    CHECK(flags == 0x05);
    CHECK(push_st_scene_encode("a", 0u, flags, 0u, 1u, ctx, 3, got, sizeof(got)) == 241);
    // 首帧：out[7]=word_len，out[8..8+wl)=word，其后 word_off 两字节，再一个字节才是 flags
    // → flags 的偏移是 7 + 1 + wl + 2 = 10 + wl，wl=1 时落在 out[11]。
    CHECK(got[11] == 0x05);

    push_scene_frame_t f;
    CHECK(push_st_scene_decode(got, sizeof(got), &f));
    CHECK(f.flags == 0x05);
    CHECK((f.flags & PUSH_SCENE_FLAG_SENTENCE_FALLBACK) == 0);
}

int main(void) {
    test_single_frame_golden();
    test_two_frames_golden();
    test_flag_composition();
    printf("EXCERPT_AI_GOLDEN_PASS checks=%d\n", s_checks);
    return 0;
}
