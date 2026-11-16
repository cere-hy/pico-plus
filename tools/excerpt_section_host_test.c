/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：26.11.4 切节函数宿主测试。三态（有标记 / 无标记 / 空节）逐一取证，
 * 外加四条边界：旧节序、超长句边界截、无句末硬切、折行不破单行契约。
 * English: 26.11.4 host tests for the section cutter. The three states plus four
 * edges: old section order, sentence-boundary cut, no-terminator hard cut, folding.
 *
 * 为什么「旧节序」是一条独立用例：26.11.4 把【原文对照】提到首节只改了 APK 模板，
 * 设备一行未动。能这么干是因为切割按标记定位、与节的先后无关——这条用例就是那
 * 句「与序无关」的实证，不是注释里的承诺。
 * Why "old order" is its own case: moving the section to the front changed only the
 * APK template. That works because the cut locates by marker, not by position - and
 * this case is the evidence, not a comment.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "excerpt_section.h"

static int g_checks;
static int g_fails;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_fails;                                                     \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                    \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
        }                                                                  \
    } while (0)

/// 标记前是否落在句末（ASCII 三枚或 UTF-8 全角四枚）。
static bool ends_on_terminator(const char* s, size_t n, size_t tail) {
    const size_t end = n > tail ? n - tail : 0;
    if (end == 0) return false;
    const char c = s[end - 1];
    if (c == '.' || c == '!' || c == '?') return true;
    if (end >= 3) {
        const unsigned char* p = (const unsigned char*)s + end - 3;
        if (p[0] == 0xE3 && p[1] == 0x80 && p[2] == 0x82) return true; // 。
        if (p[0] == 0xEF && p[1] == 0xBC &&
            (p[2] == 0x81 || p[2] == 0x9F || p[2] == 0x9B)) return true; // ！ ？ ；
    }
    return false;
}

static const char* k_answer_cur_order =
    "【原文对照】\n"
    "The house stood at the end of the lane.\n"
    "那座房子立在巷子尽头。\n"
    "\n"
    "Nobody had lived in it for years.\n"
    "已经好些年没人住了。\n"
    "\n"
    "【段意】\n"
    "讲一处荒宅。\n"
    "\n"
    "【关键表达】\n"
    "`stood` —— 立着，暗示静止。\n";

static const char* k_answer_old_order =
    "【段意】\n"
    "讲一处荒宅。\n"
    "\n"
    "【脉络】\n"
    "1. 〔The house …〕先说位置。\n"
    "\n"
    "【原文对照】\n"
    "The house stood at the end of the lane.\n"
    "那座房子立在巷子尽头。\n"
    "\n"
    "【作用】\n"
    "交代场景。\n";

static void test_found_current_order(void) {
    char out[700];
    size_t n = 0;
    bool cut = true;
    const int st = excerpt_section_pick(k_answer_cur_order, strlen(k_answer_cur_order), out,
                                        sizeof(out), &n, &cut);
    CHECK(st == EXCERPT_SECTION_OK, "status=%d", st);
    CHECK(!cut, "must not be cut; n=%zu", n);
    CHECK(n == strlen(out), "out_len %zu vs strlen %zu", n, strlen(out));
    CHECK(strstr(out, "The house stood") == out, "starts with the first sentence, got %.40s", out);
    CHECK(strstr(out, "已经好些年没人住了。") != NULL, "second translation missing");
    CHECK(strstr(out, "【段意】") == NULL, "leaked the next section");
    CHECK(strstr(out, "讲一处荒宅") == NULL, "leaked 段意 body");
    CHECK(strchr(out, '\n') == NULL, "must be one line: %s", out);
}

static void test_found_old_order(void) {
    char out[700];
    size_t n = 0;
    bool cut = false;
    const int st = excerpt_section_pick(k_answer_old_order, strlen(k_answer_old_order), out,
                                        sizeof(out), &n, &cut);
    CHECK(st == EXCERPT_SECTION_OK, "old order must still resolve; status=%d", st);
    CHECK(strstr(out, "The house stood") == out, "old order body missing: %.40s", out);
    CHECK(strstr(out, "【作用】") == NULL, "leaked the trailing section");
}

static void test_no_marker(void) {
    static const char* k_cn_only = "【段意】\n这段话在讲逻辑。\n\n【作用】\n承上启下。\n";
    char out[700];
    size_t n = 123;
    bool cut = true;
    const int st = excerpt_section_pick(k_cn_only, strlen(k_cn_only), out, sizeof(out), &n, &cut);
    CHECK(st == EXCERPT_SECTION_NO_MARKER, "status=%d", st);
    CHECK(n == 0, "out_len must be zeroed on failure, got %zu", n);
    CHECK(!cut, "cut must be false on failure");
}

static void test_empty_section(void) {
    static const char* k_empty = "【原文对照】\n\n【段意】\n空。\n";
    char out[700];
    size_t n = 0;
    bool cut = false;
    const int st = excerpt_section_pick(k_empty, strlen(k_empty), out, sizeof(out), &n, &cut);
    CHECK(st == EXCERPT_SECTION_EMPTY, "status=%d", st);
}

static void test_fold_breaks_nothing(void) {
    static const char* k_ragged =
        "【原文对照】\n  He   ran,\t\n\nthen   stopped.  \n"
        "他跑了几步，然后停下。\n";
    char out[700];
    size_t n = 0;
    bool cut = false;
    const int st = excerpt_section_pick(k_ragged, strlen(k_ragged), out, sizeof(out), &n, &cut);
    CHECK(st == EXCERPT_SECTION_OK, "status=%d", st);
    CHECK(strcmp(out, "He ran, then stopped. 他跑了几步，然后停下。") == 0, "folded: [%s]", out);
    CHECK(out[0] != ' ', "leading whitespace must not survive");
    CHECK(strstr(out, "  ") == NULL, "no double spaces: [%s]", out);
    CHECK(strchr(out, '\t') == NULL && strchr(out, '\r') == NULL && strchr(out, '\n') == NULL,
          "no control whitespace: [%s]", out);
}

static void test_cut_at_sentence_boundary(void) {
    // 造一段超过 694 B 的英文对照，句末在 room 之内。
    static char big[2048];
    static char out[700];
    size_t at = 0;
    at += (size_t)snprintf(big + at, sizeof(big) - at, "%s\n", EXCERPT_SECTION_MARKER);
    for (int i = 0; i < 12; ++i) {
        at += (size_t)snprintf(big + at, sizeof(big) - at,
                               "Sentence number %d is long enough to matter here. 第%d句够长了。\n\n",
                               i, i);
    }
    size_t n = 0;
    bool cut = false;
    const int st = excerpt_section_pick(big, strlen(big), out, sizeof(out), &n, &cut);
    CHECK(st == EXCERPT_SECTION_OK, "status=%d", st);
    CHECK(cut, "must report the cut");
    CHECK(n <= sizeof(out) - 1, "n=%zu overflows", n);
    CHECK(n > 400, "a sentence-boundary cut should keep most of the budget, n=%zu", n);
    const size_t tail = strlen(EXCERPT_SECTION_CUT);
    CHECK(n >= tail && strcmp(out + n - tail, EXCERPT_SECTION_CUT) == 0, "marker at the tail: ...%s",
          n > 12 ? out + n - 12 : out);
    // 标记之前必须是句末标点，不许是字母中间。
    CHECK(ends_on_terminator(out, n, tail), "cut must land on a terminator: ...%.16s",
          n > 20 ? out + n - 20 : out);
}

static void test_cut_cjk_boundary(void) {
    static char big[4096];
    static char out[700];
    size_t at = 0;
    at += (size_t)snprintf(big + at, sizeof(big) - at, "%s\n", EXCERPT_SECTION_MARKER);
    for (int i = 0; i < 40; ++i) {
        at += (size_t)snprintf(big + at, sizeof(big) - at, "第%d句中文内容，写得足够长一些。", i);
    }
    size_t n = 0;
    bool cut = false;
    const int st = excerpt_section_pick(big, strlen(big), out, sizeof(out), &n, &cut);
    CHECK(st == EXCERPT_SECTION_OK, "status=%d", st);
    CHECK(cut, "must report the cut");
    const size_t tail = strlen(EXCERPT_SECTION_CUT);
    CHECK(n >= tail && strcmp(out + n - tail, EXCERPT_SECTION_CUT) == 0, "marker at the tail");
    CHECK(ends_on_terminator(out, n, tail), "CJK cut must end on a full-width stop");
}

static void test_hard_cut_without_terminator(void) {
    static char big[1024];
    static char out[120];
    size_t at = 0;
    at += (size_t)snprintf(big + at, sizeof(big) - at, "%s\n", EXCERPT_SECTION_MARKER);
    for (int i = 0; i < 40; ++i) at += (size_t)snprintf(big + at, sizeof(big) - at, "word ");
    size_t n = 0;
    bool cut = false;
    const int st = excerpt_section_pick(big, strlen(big), out, sizeof(out), &n, &cut);
    CHECK(st == EXCERPT_SECTION_OK, "status=%d", st);
    CHECK(cut, "must report the cut");
    CHECK(n <= sizeof(out) - 1, "n=%zu", n);
    CHECK(strcmp(out + n - strlen(EXCERPT_SECTION_CUT), EXCERPT_SECTION_CUT) == 0,
          "marker must still be there: [%s]", out);
}

static void test_inline_mention_is_not_a_boundary(void) {
    // 节内正文引述了「【原文对照】」这个字符串，但它不在行首——不能当节尾。
    // A quoted mention inside the body must not end the section.
    static const char* k_quoted =
        "【原文对照】\n"
        "They asked what 【原文对照】 meant.\n"
        "他们问【原文对照】是什么意思。\n"
        "\n"
        "【段意】\n"
        "问句。\n";
    char out[700];
    size_t n = 0;
    bool cut = false;
    const int st = excerpt_section_pick(k_quoted, strlen(k_quoted), out, sizeof(out), &n, &cut);
    CHECK(st == EXCERPT_SECTION_OK, "status=%d", st);
    CHECK(strstr(out, "meant.") != NULL, "the mention must not truncate the body: [%s]", out);
    CHECK(strstr(out, "【段意】") == NULL, "next section still excluded");
}

static void test_len_without_nul(void) {
    // 调用方给的长度才是真值：把 NUL 之后的内容当作不存在。
    // The caller's length is the truth; bytes past it do not exist.
    static const char k_buf[] = "【原文对照】\nkeep me.\n丢掉我。【段意】\n";
    char out[700];
    size_t n = 0;
    bool cut = false;
    const size_t keep = strlen("【原文对照】\nkeep me.\n");
    const int st = excerpt_section_pick(k_buf, keep, out, sizeof(out), &n, &cut);
    CHECK(st == EXCERPT_SECTION_OK, "status=%d", st);
    CHECK(strcmp(out, "keep me.") == 0, "must stop at def_len: [%s]", out);
}

int main(void) {
    test_found_current_order();
    test_found_old_order();
    test_no_marker();
    test_empty_section();
    test_fold_breaks_nothing();
    test_cut_at_sentence_boundary();
    test_cut_cjk_boundary();
    test_hard_cut_without_terminator();
    test_inline_mention_is_not_a_boundary();
    test_len_without_nul();

    printf("excerpt-section: %d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
