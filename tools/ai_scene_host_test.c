/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * M3 情景释义 0x1A 分帧的宿主单测：帧数规划、首帧元数据、UTF-8 边界、
 * 逐帧编解码与整体重组、非法帧拒绝。ASan/UBSan 覆盖缓冲区边界。
 * Host tests for the M3 scene 0x1A segments: frame planning, first-frame
 * metadata, UTF-8 boundaries, per-frame codec + full reassembly, and rejection
 * of malformed frames. ASan/UBSan covers the buffer edges.
 *
 * Build/run: tools/run_ai_scene_host_tests.sh  (prints AI_SCENE_PASS)
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "push_proto.h"
#include "push_proto.c"

static int s_checks;

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++s_checks;                                                            \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

// UTF-8 边界：首字节不能是续字节（10xxxxxx）；末字节后若还有字节，末字节不能是
// 孤立前导。只做这两个必要判定——切片器保证整字符，测试守住它。
// UTF-8 boundary: the first byte must not be a continuation, and the last byte
// must not be a lone lead when more bytes follow.
static void check_utf8_slice(const char *s, size_t n, const char *next, size_t next_n) {
    if (n == 0) return;
    const unsigned char first = (unsigned char)s[0];
    CHECK((first & 0xC0u) != 0x80u);
    if (next_n == 0) return;
    const unsigned char last = (unsigned char)s[n - 1];
    size_t need = 0;
    if ((last & 0x80u) == 0x00u) need = 1;
    else if ((last & 0xE0u) == 0xC0u) need = 2;
    else if ((last & 0xF0u) == 0xE0u) need = 3;
    else if ((last & 0xF8u) == 0xF0u) need = 4;
    else need = 1; // 非法前导：切片器已强制前进 1 字节
    (void)need; // 末字节完整时 n 已含整字符，next 从字符边界起
    CHECK(((unsigned char)next[0] & 0xC0u) != 0x80u);
}

static void test_frame_plan(void) {
    char ctx[PUSH_SCENE_CTX_MAX];
    memset(ctx, 'a', sizeof(ctx));
    const char *word = "jealousies";
    // ASCII：首帧预算 234-(4+10)=220，余 1024-220=804 → 234*4=936 ≥ 804，共 5 帧。
    CHECK(push_scene_frame_count(word, ctx, sizeof(ctx)) == 5);
    CHECK(push_scene_frame_count(word, ctx, 0) == 1);
    CHECK(push_scene_frame_count(word, NULL, 0) == 1);

    // 短上下文一帧；空词也只占 4 字节元数据。
    CHECK(push_scene_frame_count("w", "hello", 5) == 1);
    CHECK(push_scene_frame_count("", ctx, sizeof(ctx)) == 5);
}

static void test_first_frame_meta(void) {
    char ctx[PUSH_SCENE_CTX_MAX];
    memset(ctx, 'x', sizeof(ctx));
    const char *word = "context";
    const uint16_t total = push_scene_frame_count(word, ctx, sizeof(ctx));
    const char *slice = NULL;
    size_t slice_len = 0;
    CHECK(push_scene_frame_at(word, ctx, sizeof(ctx), 0, &slice, &slice_len));
    CHECK(slice_len == PUSH_ST_SCENE_SLOT - (4u + strlen(word)));

    uint8_t frame[PUSH_ST_SCENE_FRAME_MAX];
    const size_t n = push_st_scene_encode(word, 321u, PUSH_SCENE_FLAG_CTX_TRUNCATED,
                                          0, total, slice, slice_len, frame, sizeof(frame));
    CHECK(n == 241);
    CHECK(frame[0] == PUSH_ST_AI_SCENE);
    CHECK(frame[5] == (uint8_t)((4u + strlen(word) + slice_len) & 0xFFu));
    CHECK(frame[6] == (uint8_t)(((4u + strlen(word) + slice_len) >> 8) & 0xFFu));

    push_scene_frame_t f;
    CHECK(push_st_scene_decode(frame, n, &f));
    CHECK(f.seq == 0 && f.total == total);
    CHECK(f.word_len == (uint8_t)strlen(word));
    CHECK(memcmp(f.word, word, f.word_len) == 0);
    CHECK(f.word_off == 321u);
    CHECK(f.flags == PUSH_SCENE_FLAG_CTX_TRUNCATED);
    CHECK(f.len == slice_len);
    CHECK(memcmp(f.ctx, slice, slice_len) == 0);
}

static void test_reassembly_ascii(void) {
    char ctx[PUSH_SCENE_CTX_MAX];
    for (size_t i = 0; i < sizeof(ctx); ++i) ctx[i] = (char)('a' + (i % 26));
    const char *word = "jealousies";
    const uint16_t total = push_scene_frame_count(word, ctx, sizeof(ctx));
    CHECK(total == 5);

    char out[PUSH_SCENE_CTX_MAX + 8];
    size_t used = 0;
    for (uint16_t i = 0; i < total; ++i) {
        const char *slice = NULL;
        size_t slice_len = 0;
        CHECK(push_scene_frame_at(word, ctx, sizeof(ctx), i, &slice, &slice_len));
        uint8_t frame[PUSH_ST_SCENE_FRAME_MAX];
        CHECK(push_st_scene_encode(word, 7u, 0, i, total, slice, slice_len,
                                   frame, sizeof(frame)) == 241);
        push_scene_frame_t f;
        CHECK(push_st_scene_decode(frame, sizeof(frame), &f));
        CHECK(f.seq == i && f.total == total);
        CHECK(f.len == slice_len);
        memcpy(out + used, f.ctx, f.len);
        used += f.len;
    }
    CHECK(used == sizeof(ctx));
    CHECK(memcmp(out, ctx, sizeof(ctx)) == 0);
}

static void test_utf8_boundaries(void) {
    // 中文三字节字符 + ASCII 混排：切片必须落在字符边界，重组后逐字节相等。
    // 3-byte CJK mixed with ASCII: slices must land on character boundaries.
    char ctx[PUSH_SCENE_CTX_MAX];
    size_t n = 0;
    while (n + 3 <= sizeof(ctx)) {
        ctx[n++] = (char)0xE4; ctx[n++] = (char)0xBD; ctx[n++] = (char)0xA0; // 你
        if (n + 1 < sizeof(ctx)) ctx[n++] = ' ';
    }
    const size_t ctx_len = n;
    const char *word = "你";
    const uint16_t total = push_scene_frame_count(word, ctx, ctx_len);
    CHECK(total >= 4 && total <= PUSH_SCENE_MAX_FRAMES);

    char out[PUSH_SCENE_CTX_MAX + 8];
    size_t used = 0;
    for (uint16_t i = 0; i < total; ++i) {
        const char *slice = NULL;
        size_t slice_len = 0;
        CHECK(push_scene_frame_at(word, ctx, ctx_len, i, &slice, &slice_len));
        const char *next = NULL;
        size_t next_len = 0;
        if (i + 1 < total) {
            CHECK(push_scene_frame_at(word, ctx, ctx_len, (uint16_t)(i + 1), &next, &next_len));
        }
        check_utf8_slice(slice, slice_len, next, next_len);
        memcpy(out + used, slice, slice_len);
        used += slice_len;
    }
    CHECK(used == ctx_len);
    CHECK(memcmp(out, ctx, ctx_len) == 0);
}

static void test_reject_malformed(void) {
    uint8_t frame[PUSH_ST_SCENE_FRAME_MAX];
    const char *word = "w";
    CHECK(push_st_scene_encode(word, 0, 0, 0, 1, "abc", 3, frame, sizeof(frame)) == 241);

    push_scene_frame_t f;
    // 短缓冲 / 错 opcode。
    CHECK(!push_st_scene_decode(frame, 100, &f));
    frame[0] = 0x19;
    CHECK(!push_st_scene_decode(frame, sizeof(frame), &f));
    frame[0] = PUSH_ST_AI_SCENE;

    // total=0 / seq>=total / len>槽 三类坏帧。
    put_u16(frame + 3, 0);
    CHECK(!push_st_scene_decode(frame, sizeof(frame), &f));
    put_u16(frame + 3, 1);
    put_u16(frame + 1, 1);
    CHECK(!push_st_scene_decode(frame, sizeof(frame), &f));
    put_u16(frame + 1, 0);
    put_u16(frame + 5, (uint16_t)(PUSH_ST_SCENE_SLOT + 1));
    CHECK(!push_st_scene_decode(frame, sizeof(frame), &f));

    // 编码拒绝非法 total/seq 与超槽切片。
    uint8_t out[PUSH_ST_SCENE_FRAME_MAX];
    CHECK(push_st_scene_encode(word, 0, 0, 0, 0, "x", 1, out, sizeof(out)) == 0);
    CHECK(push_st_scene_encode(word, 0, 0, 2, 2, "x", 1, out, sizeof(out)) == 0);
    CHECK(push_st_scene_encode(word, 0, 0, 0, 1, "x", PUSH_ST_SCENE_SLOT + 1,
                               out, sizeof(out)) == 0);
}

static void test_frame_at_bounds(void) {
    char ctx[64];
    memset(ctx, 'z', sizeof(ctx));
    const char *slice = NULL;
    size_t slice_len = 0;
    CHECK(push_scene_frame_at("w", ctx, sizeof(ctx), 0, &slice, &slice_len));
    CHECK(slice_len == sizeof(ctx));
    CHECK(!push_scene_frame_at("w", ctx, sizeof(ctx), 1, &slice, &slice_len));
}

int main(void) {
    test_frame_plan();
    test_first_frame_meta();
    test_reassembly_ascii();
    test_utf8_boundaries();
    test_reject_malformed();
    test_frame_at_bounds();
    printf("AI_SCENE_PASS checks=%d\n", s_checks);
    return 0;
}
