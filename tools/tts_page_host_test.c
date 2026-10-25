/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：TTS 页文本抽取 + NVS 开关的宿主回归。断言有牙：U+FFFC 必须滤、空白必须
 * 折叠成单空格、首尾必须修、区间非法必须 0、cap 必须不溢（ASan 兜底）、开关
 * 缺省关且怪值也按关（与 autosave 的怪值按开刻意相反）、写盘必须真写真 commit。
 * English: host regression for TTS page-text extraction and the NVS switch. The
 * assertions bite: U+FFFC must go, blanks must collapse to single spaces, ends
 * must trim, invalid ranges must yield 0, the cap must not overflow (ASan backs
 * this), the switch defaults off with odd values also off (deliberately opposite
 * to autosave's odd-is-on), and writes must really write and commit.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

int tts_test_nvs_enabled = -1;    /* -1 = 键不存在 / key absent */
int tts_test_nvs_written = -1;    /* 没写过 / never written */
int tts_test_nvs_commits = 0;
int tts_test_nvs_open_fails = 0;

#include "tts_page.h"
#include "tts_page.c"

static int checks, reds;
#define CHECK(cond)                                                    \
    do {                                                               \
        ++checks;                                                      \
        if (!(cond)) {                                                 \
            ++reds;                                                    \
            printf("RED line %d: %s\n", __LINE__, #cond);              \
        }                                                              \
    } while (0)

static char out[TTS_PAGE_TEXT_MAX];

static size_t ex(const char *text, size_t start, size_t end) {
    return tts_page_extract(text, strlen(text), start, end, out, sizeof(out));
}

static void reset_nvs(void) {
    tts_test_nvs_enabled = -1;
    tts_test_nvs_written = -1;
    tts_test_nvs_commits = 0;
    tts_test_nvs_open_fails = 0;
}

/* ---- 抽取 / Extraction ---- */

static void test_extract_basic(void) {
    // 纯文本原样搬。/ Plain text moves verbatim.
    size_t n = ex("It was a bright cold day in April.", 0, 34);
    CHECK(n == 34);
    CHECK(strcmp(out, "It was a bright cold day in April.") == 0);

    // 中文与多字节序列完整。/ Chinese and multi-byte sequences stay whole.
    const char *zh = "这是一段中文正文，带有标点。";
    n = ex(zh, 0, strlen(zh));
    CHECK(n == strlen(zh));
    CHECK(strcmp(out, zh) == 0);

    // 区间只取一段。/ A sub-range is taken verbatim.
    n = ex("0123456789", 2, 5);
    CHECK(n == 3);
    CHECK(strcmp(out, "234") == 0);
}

static void test_extract_fffc(void) {
    // U+FFFC 图片占位（EF BF BC）必须剔除，且不引入空格。
    // The U+FFFC placeholder (EF BF BC) must be dropped without inserting a space.
    const char *with_img = "图前文字\xEF\xBF\xBC图后文字";
    size_t n = ex(with_img, 0, strlen(with_img));
    CHECK(strcmp(out, "图前文字图后文字") == 0);
    CHECK(n == strlen("图前文字图后文字"));

    // 段首即图：不留下任何痕迹。/ An image at the range start leaves no trace.
    n = ex("\xEF\xBF\xBC正文", 0, strlen("\xEF\xBF\xBC正文"));
    CHECK(strcmp(out, "正文") == 0);

    // 段尾即图：同样干净。/ An image at the range end is equally clean.
    n = ex("正文\xEF\xBF\xBC", 0, strlen("正文\xEF\xBF\xBC"));
    CHECK(strcmp(out, "正文") == 0);

    // 整页只有图：0 字节（调用方据此跳过推送）。/ An image-only page: 0 bytes.
    n = ex("\xEF\xBF\xBC\xEF\xBF\xBC", 0, 6);
    CHECK(n == 0);
    CHECK(out[0] == 0);

    // 图夹在空白之间：不叠双空格。/ An image between blanks does not stack spaces.
    n = ex("甲 \xEF\xBF\xBC 乙", 0, strlen("甲 \xEF\xBF\xBC 乙"));
    CHECK(strcmp(out, "甲 乙") == 0);
}

static void test_extract_blanks(void) {
    // 段间多空行（含 \r\n）折叠为单空格。/ Blank runs collapse to one space.
    size_t n = ex("第一段。\r\n\n\n第二段。", 0, strlen("第一段。\r\n\n\n第二段。"));
    CHECK(strcmp(out, "第一段。 第二段。") == 0);

    // 首尾空白修剪。/ Leading and trailing blanks trim away.
    n = ex("  正文  ", 0, strlen("  正文  "));
    CHECK(strcmp(out, "正文") == 0);
    n = ex("\n\t 正文 \v\f", 0, strlen("\n\t 正文 \v\f"));
    CHECK(strcmp(out, "正文") == 0);

    // 全角空格 U+3000（E3 80 80）同等折叠。/ The ideographic space is treated the same.
    n = ex("甲\xE3\x80\x80\xE3\x80\x80乙", 0, strlen("甲\xE3\x80\x80\xE3\x80\x80乙"));
    CHECK(strcmp(out, "甲 乙") == 0);

    // 全空白页：0 字节。/ An all-blank page: 0 bytes.
    n = ex("  \n\t ", 0, strlen("  \n\t "));
    CHECK(n == 0);
}

static void test_extract_ranges(void) {
    const char *t = "ABCDEF";
    // 非法区间全 0。/ Invalid ranges all yield 0.
    CHECK(ex(t, 3, 3) == 0);       // start == end
    CHECK(ex(t, 4, 3) == 0);       // start > end
    CHECK(tts_page_extract(t, 6, 0, 7, out, sizeof(out)) == 0); // end > len
    // 区间贴边合法。/ Edge-touching ranges are legal.
    CHECK(ex(t, 0, 6) == 6);
    CHECK(strcmp(out, "ABCDEF") == 0);
    CHECK(ex(t, 5, 6) == 1);
    CHECK(strcmp(out, "F") == 0);
}

static void test_extract_cap(void) {
    // cap 截断：不溢（ASan 兜底）、结果以 NUL 收尾、无尾空格。
    // Cap truncation: no overflow (ASan), NUL-terminated, no trailing space.
    char small[8];
    const char *t = "一二三四五六七八九十";
    size_t n = tts_page_extract(t, strlen(t), 0, strlen(t), small, sizeof(small));
    CHECK(n < sizeof(small));
    CHECK(small[n] == 0);
    CHECK(n > 0);
    // 截断点落在多字节序列中间时，按字节进度推进——切片合法性由帧计划器
    // 的字符边界规则在对端侧保证（本模块只保证不越界）。
    // A cut mid-sequence still advances by bytes; slice legality on the peer
    // side is the frame planner's character-boundary rule (this module only
    // guarantees the bound).
}

/* ---- 开关 / Switch ---- */

static void test_switch(void) {
    // 键不存在：缺省关。/ Key absent: default off.
    reset_nvs();
    tts_switch_init();
    CHECK(!tts_switch_on());

    // 读到 0：关。/ Read 0: off.
    reset_nvs();
    tts_test_nvs_enabled = 0;
    tts_switch_init();
    CHECK(!tts_switch_on());

    // 读到 1：开。/ Read 1: on.
    reset_nvs();
    tts_test_nvs_enabled = 1;
    tts_switch_init();
    CHECK(tts_switch_on());

    // 怪值：按关（与 autosave 的怪值按开刻意相反——听书不可默认代用户决定）。
    // Odd value: off (deliberately opposite to autosave's odd-is-on).
    reset_nvs();
    tts_test_nvs_enabled = 2;
    tts_switch_init();
    CHECK(!tts_switch_on());

    // 写开：运行期立即生效 + NVS 落盘 + commit。/ Set on: runtime flips at once,
    // NVS records the write, commit lands.
    reset_nvs();
    tts_switch_init();
    tts_switch_set(true);
    CHECK(tts_switch_on());
    CHECK(tts_test_nvs_written == 1);
    CHECK(tts_test_nvs_commits == 1);

    // 写关：同样落盘。/ Set off: equally persisted.
    tts_switch_set(false);
    CHECK(!tts_switch_on());
    CHECK(tts_test_nvs_written == 0);
    CHECK(tts_test_nvs_commits == 2);

    // NVS open 失败：运行期仍更新（本次生效），不假装持久化。
    // A failed NVS open: the runtime still flips (this session honors it), no
    // faked persistence.
    reset_nvs();
    tts_switch_init();
    tts_test_nvs_open_fails = 1;
    tts_switch_set(true);
    CHECK(tts_switch_on());
    CHECK(tts_test_nvs_written == -1); // 没写成 / nothing was written
    CHECK(tts_test_nvs_commits == 0);
}

int main(void) {
    test_extract_basic();
    test_extract_fffc();
    test_extract_blanks();
    test_extract_ranges();
    test_extract_cap();
    test_switch();
    printf("tts page: extract (fffc/blank/range/cap) + switch (default-off, odd-off, persist) - %d checks, %d red\n",
           checks, reds);
    return reds ? 1 : 0;
}
