/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * M3-B 情景视图宿主测试：**零桩**——只测纯逻辑（待命态 + 引用条取句）。
 * 理由：这两块决定「这条 0x08 归不归情景视图」「引用条引的是不是那句」，
 * 错了就是把 A 页的分析贴到 B 页上、或引错句子；必须在宿主逐条钉死，
 * 不能等真机碰运气。
 * Host test for the M3-B scene view: ZERO stubs - it only exercises pure logic
 * (the pending state and the quote-sentence extraction). Those two decide whether
 * a 0x08 belongs to the scene view and whether the strip quotes the right
 * sentence; getting them wrong pins page A's analysis onto page B or quotes the
 * wrong line, so they are pinned here line by line rather than found on hardware.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "scene_req.h"

static int s_checks;
static int s_failed;

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++s_checks;                                                            \
        if (!(cond)) {                                                         \
            ++s_failed;                                                        \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);             \
        }                                                                      \
    } while (0)

/* ---------------- 一、待命态 / the pending state ---------------- */

static void test_arm_and_claim(void) {
    scene_req_t r;
    memset(&r, 0xFF, sizeof(r)); // 脏内存起手：arm 必须整块重置 / start dirty
    scene_req_arm(&r, "word", 12, 4, 7, 1000);
    CHECK(r.armed);
    CHECK(strcmp(r.word, "word") == 0);
    CHECK(r.word_off == 12 && r.word_len == 4);
    CHECK(r.page == 7 && r.deadline_ms == 1000);

    // 命中：认领成功并销号，第二次认领必须失败（一次机会）。
    // Hit: claim succeeds and disarms; a second claim must fail (one shot).
    CHECK(scene_req_claim(&r, "word", 7, 900));
    CHECK(!r.armed);
    CHECK(!scene_req_claim(&r, "word", 7, 900));
}

static void test_arm_rejects_empty(void) {
    scene_req_t r;
    scene_req_arm(&r, "x", 0, 1, 0, 10);
    CHECK(r.armed);
    scene_req_arm(&r, "", 0, 0, 0, 10); // 空词：无法关联 / nothing to correlate
    CHECK(!r.armed);
    scene_req_arm(&r, NULL, 0, 0, 0, 10);
    CHECK(!r.armed);
}

static void test_word_mismatch_keeps_armed(void) {
    // 用户点了别的词：这条答案不是我的，但**别销号**——我自己的那条还在路上。
    // Another word's answer is not mine, and the request stays armed: my own is
    // still in flight.
    scene_req_t r;
    scene_req_arm(&r, "mine", 0, 4, 3, 1000);
    CHECK(!scene_req_claim(&r, "other", 3, 500));
    CHECK(r.armed);
    CHECK(scene_req_claim(&r, "mine", 3, 500));
}

static void test_page_mismatch_disarms(void) {
    // 页不配＝用户已经翻页在读别处：销号，别让这条答案再掉进释义卡。
    // Page mismatch means the user turned away: disarm, so the answer cannot fall
    // through and land a definition card either.
    scene_req_t r;
    scene_req_arm(&r, "w", 0, 1, 5, 1000);
    CHECK(!scene_req_claim(&r, "w", 6, 500));
    CHECK(!r.armed);
}

static void test_expired_never_claims(void) {
    scene_req_t r;
    scene_req_arm(&r, "w", 0, 1, 5, 1000);
    // 期限边界含等号：now == deadline 仍可认领。
    // The deadline is inclusive: now == deadline still claims.
    CHECK(scene_req_claim(&r, "w", 5, 1000));

    scene_req_arm(&r, "w", 0, 1, 5, 1000);
    CHECK(!scene_req_claim(&r, "w", 5, 1001)); // 过期：同词也不认 / expired: not even the same word
    CHECK(!r.armed);                           // 顺手销号 / disarmed on the way out
}

static void test_expire_query_is_pure(void) {
    scene_req_t r;
    scene_req_arm(&r, "w", 0, 1, 5, 1000);
    CHECK(!scene_req_expired(&r, 1000));
    CHECK(scene_req_expired(&r, 1001));
    CHECK(r.armed); // 查询不改状态 / a query mutates nothing
    CHECK(!scene_req_expire(&r, 1000));
    CHECK(r.armed); // 未到期不销号 / not due, still armed
    CHECK(scene_req_expire(&r, 1001));
    CHECK(!r.armed);
}

static void test_disarm(void) {
    scene_req_t r;
    scene_req_arm(&r, "w", 0, 1, 5, 1000);
    scene_req_disarm(&r);
    CHECK(!r.armed);
    CHECK(!scene_req_claim(&r, "w", 5, 500));
}

/* ---------------- 二、引用条取句 / quote-sentence ---------------- */

// 判定切片首字节不是续字节（即没有从半个字符中间开始）。末字节不做「非续字节」
// 断言——多字节字符的末字节本来就是续字节，那样写是条恒真的假断言（单边洞）。
// The slice must not START on a continuation byte. The tail is deliberately not
// asserted to be a lead byte: a multi-byte glyph ends in a continuation byte, and
// asserting otherwise is a vacuous check (the one-sided-hole family).
static bool starts_on_char_boundary(const char* s, size_t off, size_t len) {
    if (len == 0) return true;
    return ((unsigned char)s[off] & 0xC0) != 0x80;
}

// 子串查找（自己写，不依赖 memmem 的 GNU 扩展）。/ Substring search without GNU memmem.
static bool contains(const char* hay, size_t hlen, const char* needle, size_t nlen) {
    if (nlen == 0 || hlen < nlen) return false;
    for (size_t i = 0; i + nlen <= hlen; ++i) {
        if (memcmp(hay + i, needle, nlen) == 0) return true;
    }
    return false;
}

static void test_quote_sentence_basic(void) {
    const char* ctx = "第一句话。第二句含目标词。第三句话。";
    size_t off = 0, len = 0;
    bool fb = false;
    const char* word = "目标词";
    const size_t woff = (size_t)(strstr(ctx, word) - ctx);
    const size_t n = scene_req_quote_sentence(ctx, strlen(ctx), woff, strlen(word), 40, &off, &len, &fb);
    CHECK(n == len);
    CHECK(len > strlen(word));
    // 必须是「第二句含目标词。」整句：不含前一句、不含后一句。
    // Must be the whole middle sentence: neither neighbour bleeds in.
    CHECK(memcmp(ctx + off, "第二句含目标词。", len) == 0);
    CHECK(!fb);
}

static void test_quote_sentence_word_at_edge(void) {
    const char* ctx = "开头。尾部词";
    const size_t woff = (size_t)(strstr(ctx, "尾部词") - ctx);
    size_t off = 0, len = 0;
    bool fb = false;
    (void)scene_req_quote_sentence(ctx, strlen(ctx), woff, strlen("尾部词"), 40, &off, &len, &fb);
    CHECK(memcmp(ctx + off, "尾部词", len) == 0); // 到 ctx 末尾也能收句 / clamps at the ctx edge
}

static void test_quote_no_boundary_sets_fallback(void) {
    const char* ctx = "没有任何句号的上下文";
    size_t off = 0, len = 0;
    bool fb = false;
    const size_t n = scene_req_quote_sentence(ctx, strlen(ctx), 6, 6, 40, &off, &len, &fb);
    CHECK(n == strlen(ctx));
    CHECK(fb); // 无句界：整段 + fallback 标志 / no boundary: whole ctx + flag
}

static void test_quote_truncates_around_word(void) {
    // 长句（无标点）里取 12 字：词必须完整保留、且总字数不超预算。
    // A 12-char budget on a long punctuation-free sentence: the word survives whole
    // and the total stays within budget.
    const char* ctx = "一二三四五六七八九十一二三四五六七八九十目标词一二三四五六七八九十";
    const size_t woff = (size_t)(strstr(ctx, "目标词") - ctx);
    size_t off = 0, len = 0;
    bool fb = false;
    (void)scene_req_quote_sentence(ctx, strlen(ctx), woff, strlen("目标词"), 12, &off, &len, &fb);
    CHECK(len >= strlen("目标词"));
    CHECK(contains(ctx + off, len, "目标词", strlen("目标词")));
    // 码位数 ≤ 12
    size_t chars = 0;
    for (size_t i = 0; i < len;) {
        const unsigned char c = (unsigned char)ctx[off + i];
        size_t step = c < 0x80 ? 1 : (c >= 0xF0 ? 4 : (c >= 0xE0 ? 3 : 2));
        i += step;
        ++chars;
    }
    CHECK(chars <= 12);
    CHECK(starts_on_char_boundary(ctx, off, len));
}

static void test_quote_uses_cjk_terminators(void) {
    // ！和？同样收句——只认「。」会跨句引用。/ ！ and ？ end sentences too.
    const char* ctx = "真的吗？目标词在这里！后面还有";
    const size_t woff = (size_t)(strstr(ctx, "目标词") - ctx);
    size_t off = 0, len = 0;
    bool fb = false;
    (void)scene_req_quote_sentence(ctx, strlen(ctx), woff, strlen("目标词"), 40, &off, &len, &fb);
    CHECK(memcmp(ctx + off, "目标词在这里！", len) == 0);
}

static void test_quote_degenerate(void) {
    size_t off = 9, len = 9;
    bool fb = true;
    CHECK(scene_req_quote_sentence(NULL, 0, 0, 0, 10, &off, &len, &fb) == 0);
    CHECK(off == 0 && len == 0 && !fb);
    // 偏移越界不得读越界（ASan 会抓）。/ An out-of-range offset must not read past the end.
    const char* ctx = "短";
    CHECK(scene_req_quote_sentence(ctx, strlen(ctx), 99, 99, 10, &off, &len, &fb) == strlen(ctx));
}

int main(void) {
    test_arm_and_claim();
    test_arm_rejects_empty();
    test_word_mismatch_keeps_armed();
    test_page_mismatch_disarms();
    test_expired_never_claims();
    test_expire_query_is_pure();
    test_disarm();
    test_quote_sentence_basic();
    test_quote_sentence_word_at_edge();
    test_quote_no_boundary_sets_fallback();
    test_quote_truncates_around_word();
    test_quote_uses_cjk_terminators();
    test_quote_degenerate();

    if (s_failed) {
        printf("AI_SCENE_VIEW_FAIL checks=%d failed=%d\n", s_checks, s_failed);
        return 1;
    }
    printf("AI_SCENE_VIEW_PASS checks=%d\n", s_checks);
    return 0;
}
