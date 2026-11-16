/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：box_select 宿主测试——纯区间逻辑，无桩、无屏幕、无排版。
 * English: host test for box_select — pure interval logic, no stubs, no screen, no layout.
 */
#include <stdio.h>
#include <string.h>

#include "book_select.h"

static int s_fail;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            printf("FAIL: ");                             \
            printf(__VA_ARGS__);                          \
            printf("  (%s:%d)\n", __FILE__, __LINE__);    \
            ++s_fail;                                     \
        }                                                 \
    } while (0)

static void test_enter_and_wait(void) {
    box_selection_t sel;
    box_select_begin(&sel, 7, 1000);
    CHECK(box_select_active(&sel), "begin makes the mode active");
    CHECK(!box_select_ready(&sel), "begin is not a finished range");
    CHECK(sel.page == 7, "page is remembered");
    CHECK(box_select_length(&sel) == 0, "no range before the tail tap");

    // 首点只定锚，不产生完成态。
    CHECK(box_select_tap(&sel, 100, 103, 1500), "head tap accepted");
    CHECK(box_select_active(&sel), "still active after the head tap");
    CHECK(!box_select_ready(&sel), "E-R2: head only is not ready");
    CHECK(sel.off_start == 100 && sel.off_end == 103, "the head word is held as the anchor");
    char pre[8];
    CHECK(box_select_copy_text(&sel, "aaaaaaaaaa", 10, pre, sizeof(pre)) == 0,
          "a provisional range (head only) refuses to copy");
}

static void test_reverse_selection(void) {
    box_selection_t sel;
    box_select_begin(&sel, 3, 0);
    box_select_tap(&sel, 300, 303, 10);   // 先点第 3 行
    CHECK(box_select_tap(&sel, 100, 103, 20), "tail before the anchor is accepted");
    CHECK(box_select_ready(&sel), "E-R1: reverse selection completes");
    CHECK(sel.off_start == 100 && sel.off_end == 303,
          "E-R1: range becomes [100,303), got [%u,%u)", (unsigned)sel.off_start,
          (unsigned)sel.off_end);
}

static void test_same_word_twice(void) {
    box_selection_t sel;
    box_select_begin(&sel, 1, 0);
    box_select_tap(&sel, 500, 505, 0);
    CHECK(box_select_tap(&sel, 500, 505, 5), "E-R4: tapping the same word again is accepted");
    CHECK(box_select_ready(&sel), "E-R4: same word completes a single-word range");
    CHECK(sel.off_start == 500 && sel.off_end == 505, "E-R4: range stays the word itself");
}

static void test_forward_union(void) {
    box_selection_t sel;
    box_select_begin(&sel, 1, 0);
    box_select_tap(&sel, 100, 104, 0);
    CHECK(box_select_tap(&sel, 200, 206, 5), "tail after the anchor is accepted");
    CHECK(sel.off_start == 100 && sel.off_end == 206, "forward range unions to [100,206)");
}

static void test_timeout(void) {
    box_selection_t sel;
    box_select_begin(&sel, 1, 1000);
    CHECK(!box_select_expired(&sel, 1000 + BOX_SELECT_TIMEOUT_MS - 1), "not expired before the deadline");
    CHECK(box_select_expired(&sel, 1000 + BOX_SELECT_TIMEOUT_MS), "E-R3: expires exactly at the deadline");
    CHECK(!box_select_active(&sel), "E-R3: expiry drops the mode");
    CHECK(sel.off_start == 0 && sel.off_end == 0, "E-R3: expiry clears the range");

    // 每次有效点按重置计时：首点后再等 29.9 s 仍不超时。
    box_select_begin(&sel, 1, 0);
    box_select_tap(&sel, 10, 12, 0);
    CHECK(!box_select_expired(&sel, BOX_SELECT_TIMEOUT_MS - 1), "head tap restarts the deadline");
}

static void test_copy_text(void) {
    box_selection_t sel;
    const char *text = "Hello 世界, box selection.";
    box_select_begin(&sel, 1, 0);
    box_select_tap(&sel, 6, 12, 0);    // "世界" (6..12)
    box_select_tap(&sel, 6, 12, 1);
    char buf[32];
    memset(buf, 0x7f, sizeof(buf));
    size_t n = box_select_copy_text(&sel, text, strlen(text), buf, sizeof(buf));
    CHECK(n == 6, "copies the exact UTF-8 byte count, got %u", (unsigned)n);
    CHECK(strcmp(buf, "世界") == 0, "copies the selected text verbatim, got %s", buf);

    // 容量不足：一个字都不写，绝不给半截。
    char small[4];
    memset(small, 0x7f, sizeof(small));
    CHECK(box_select_copy_text(&sel, text, strlen(text), small, sizeof(small)) == 0,
          "short cap refuses instead of clipping");
    CHECK((unsigned char)small[0] == 0x7f, "short cap leaves the buffer untouched");

    // 区间越界：拒绝。
    CHECK(box_select_copy_text(&sel, text, 3, buf, sizeof(buf)) == 0, "out-of-range selection refuses");
}

static void test_cancel(void) {
    box_selection_t sel;
    box_select_begin(&sel, 1, 0);
    box_select_tap(&sel, 10, 14, 0);
    box_select_cancel(&sel);
    CHECK(!box_select_active(&sel), "cancel leaves the mode");
    box_select_cancel(&sel);
    CHECK(!box_select_active(&sel), "cancel is idempotent");

    // 未进入时点按无效（不凭空造出选区）。
    box_selection_t idle;
    memset(&idle, 0, sizeof(idle));
    CHECK(!box_select_tap(&idle, 1, 2, 0), "tap while OFF is ignored");
    CHECK(!box_select_active(&idle), "tap while OFF keeps it OFF");
}

static void test_zero_span(void) {
    box_selection_t sel;
    box_select_begin(&sel, 1, 0);
    CHECK(!box_select_tap(&sel, 50, 50, 0), "empty span (miss) is rejected");
    CHECK(sel.state == BOX_STATE_ARM, "a miss keeps waiting for the head tap");
}

/* ================= 26.9.12 · 按句吸附 / sentence snapping ================= */
// 所有偏移都是**手算的字节偏移**，不是 strlen 猜的：中文字符 3 字节，全角空格 3 字节。
// Every offset below is hand-computed in bytes (a CJK glyph is 3 bytes, the ideographic
// space is 3 bytes) - never guessed with strlen.
static void test_snap_sentence(void) {
    size_t s = 0, e = 0;

    /* ---- 中文：首点向前到句号之后，尾点向后到句号（含）----
     * 第一句。第二句！第三句   →  第0 一3 句6 。9 第12 二15 句18 ！21 第24 三27 句30 ｜33 */
    static const char t1[] = "第一句。第二句！第三句";
    const size_t n1 = sizeof(t1) - 1; /* 33 */
    CHECK(box_select_snap_sentence(t1, n1, 0, n1, 15, 18, -1, &s, &e) && s == 12 && e == 18,
          "S1: CJK head snaps to just after 。 → [12,18), got [%u,%u)", (unsigned)s, (unsigned)e);
    CHECK(box_select_snap_sentence(t1, n1, 0, n1, 15, 18, +1, &s, &e) && s == 15 && e == 24,
          "S2: CJK tail snaps past ！ → [15,24), got [%u,%u)", (unsigned)s, (unsigned)e);
    // 首点正落在句首：不再往前跑，停在原地（幂等）。
    CHECK(box_select_snap_sentence(t1, n1, 0, n1, 12, 15, -1, &s, &e) && s == 12,
          "S3: a head already at the sentence start stays put, got %u", (unsigned)s);
    // 尾点正落在句末标点上：标点计入，不多吃下一句。
    CHECK(box_select_snap_sentence(t1, n1, 0, n1, 21, 24, +1, &s, &e) && e == 24,
          "S4: a tail already at the terminator does not eat the next sentence, got %u",
          (unsigned)e);

    /* ---- 连续标点必须并成一个句末 ---- 
     * 真的吗？！好 → 真0 的3 吗6 ？9 ！12 好15 ｜18 */
    static const char t2[] = "真的吗？！好";
    const size_t n2 = sizeof(t2) - 1;
    CHECK(box_select_snap_sentence(t2, n2, 0, n2, 6, 9, +1, &s, &e) && e == 15,
          "S5: ？！ merges into one sentence end, got end=%u", (unsigned)e);

    /* ---- 换行与全角缩进 ----
     * 甲0 。3 \n6 　　7..13 乙13 丙16 丁19 \n22 戊23 ｜26 */
    static const char t3[] = "甲。\n　　乙丙丁\n戊";
    const size_t n3 = sizeof(t3) - 1;
    CHECK(box_select_snap_sentence(t3, n3, 0, n3, 16, 19, -1, &s, &e) && s == 13,
          "S6: a newline ends the previous sentence and the full-width indent is skipped, got %u",
          (unsigned)s);
    CHECK(box_select_snap_sentence(t3, n3, 0, n3, 16, 19, +1, &s, &e) && e == 22,
          "S7: a newline closes the sentence but is not itself selected, got %u", (unsigned)e);
    // 页起点落在句中间：向前只能到页首（R-S4：句子跨页就按页截断）。
    CHECK(box_select_snap_sentence(t3, n3, 13, n3, 16, 19, -1, &s, &e) && s == 13,
          "S8: a sentence starting on the previous page clips to the page start, got %u",
          (unsigned)s);

    /* ---- 向后跨页：本页扫不到句末 → 停在页尾 ---- 
     * 甲乙。丙丁戊 → 甲0 乙3 。6 丙9 丁12 戊15 ｜18，本页只到 15 */
    static const char t4[] = "甲乙。丙丁戊";
    const size_t n4 = sizeof(t4) - 1;
    CHECK(box_select_snap_sentence(t4, n4, 0, 15, 9, 12, +1, &s, &e) && e == 15,
          "S9: no terminator on this page → stop at the page end, got %u", (unsigned)e);

    /* ---- 英文：句号后必须有空白，否则是小数点/缩写 ---- */
    static const char t5[] = "Hello world. Next one."; /* '.'11 ' '12 N13 */
    const size_t n5 = sizeof(t5) - 1;
    CHECK(box_select_snap_sentence(t5, n5, 0, n5, 13, 16, -1, &s, &e) && s == 13,
          "S10: ASCII '. ' ends a sentence and the space is trimmed, got %u", (unsigned)s);
    CHECK(box_select_snap_sentence(t5, n5, 0, n5, 13, 16, +1, &s, &e) && e == n5,
          "S11: ASCII '.' at the page end counts, got %u", (unsigned)e);

    static const char t6[] = "One. Pi is 3.14 here."; /* '.'3 ' '4 P5 3(11) .12 1(13) */
    const size_t n6 = sizeof(t6) - 1;
    CHECK(box_select_snap_sentence(t6, n6, 0, n6, 11, 12, -1, &s, &e) && s == 5,
          "S12: 3.14 is not a sentence end, head still goes to after 'One. ', got %u",
          (unsigned)s);
    CHECK(box_select_snap_sentence(t6, n6, 0, n6, 11, 12, +1, &s, &e) && e == n6,
          "S13: the decimal point is skipped and the real '.' closes it, got %u", (unsigned)e);

    static const char t7[] = "Why? Because."; /* '?'3 ' '4 B5 */
    const size_t n7 = sizeof(t7) - 1;
    CHECK(box_select_snap_sentence(t7, n7, 0, n7, 5, 12, -1, &s, &e) && s == 5,
          "S14: ASCII '? ' ends a sentence, got %u", (unsigned)s);

    /* ---- 参数与边界 ---- */
    CHECK(!box_select_snap_sentence(NULL, 0, 0, 0, 0, 1, -1, &s, &e),
          "S15: a NULL text is refused");
    CHECK(!box_select_snap_sentence(t1, n1, 0, n1, 10, 10, -1, &s, &e),
          "S16: an empty span is refused");
    CHECK(!box_select_snap_sentence(t1, n1, 12, n1, 0, 3, -1, &s, &e),
          "S17: a span before the page start is refused");
    CHECK(!box_select_snap_sentence(t1, n1, 0, n1, 30, 34, -1, &s, &e),
          "S18: a span past the page end is refused");
    // page_end 超出 text_len 时夹紧，而不是越界读。
    CHECK(box_select_snap_sentence(t4, n4, 0, 999, 9, 12, +1, &s, &e) && e == n4,
          "S19: an over-long page_end is clamped to text_len, got %u", (unsigned)e);
    // 无任何标点的整段：向前到页首、向后到页尾。
    static const char t8[] = "甲乙丙丁";
    const size_t n8 = sizeof(t8) - 1;
    CHECK(box_select_snap_sentence(t8, n8, 0, n8, 6, 9, -1, &s, &e) && s == 0,
          "S20: no terminator at all → head falls back to the page start, got %u", (unsigned)s);
    CHECK(box_select_snap_sentence(t8, n8, 0, n8, 6, 9, +1, &s, &e) && e == n8,
          "S21: no terminator at all → tail falls back to the page end, got %u", (unsigned)e);

    /* ---- 性质：任何被接受的结果都必须**包含**原点按区间 ---- */
    for (size_t ws = 0; ws + 3 <= n1; ws += 3) {
        for (int dir = -1; dir <= 1; dir += 2) {
            size_t a = 0, b = 0;
            if (!box_select_snap_sentence(t1, n1, 0, n1, ws, ws + 3, dir, &a, &b)) continue;
            CHECK(a <= ws && b >= ws + 3, "S22: snap must contain the tapped span (dir=%d ws=%u → "
                                          "[%u,%u))", dir, (unsigned)ws, (unsigned)a, (unsigned)b);
            CHECK(b <= n1, "S23: snap must not run past the page (got %u)", (unsigned)b);
        }
    }
    /* ---- 与状态机合体：吸附后的整句区间直接进 box_select_tap ---- */
    box_selection_t sel;
    box_select_begin(&sel, 0, 0);
    size_t hs = 0, he = 0, ts = 0, te = 0;
    CHECK(box_select_snap_sentence(t1, n1, 0, n1, 15, 18, -1, &hs, &he), "S24: head snap");
    CHECK(box_select_tap(&sel, hs, he, 10), "S24: the snapped head is accepted");
    CHECK(box_select_snap_sentence(t1, n1, 0, n1, 27, 30, +1, &ts, &te), "S24: tail snap");
    CHECK(box_select_tap(&sel, ts, te, 20), "S24: the snapped tail is accepted");
    CHECK(box_select_ready(&sel) && sel.off_start == 12 && sel.off_end == 33,
          "S24: 首句到末句整段 [12,33)，got [%u,%u)", (unsigned)sel.off_start,
          (unsigned)sel.off_end);
}

int main(void) {
    test_enter_and_wait();
    test_reverse_selection();
    test_same_word_twice();
    test_forward_union();
    test_timeout();
    test_copy_text();
    test_cancel();
    test_zero_span();
    test_snap_sentence();
    if (s_fail) {
        printf("BOXSEL_FAIL: %d check(s) failed\n", s_fail);
        return 1;
    }
    printf("BOXSEL_PASS\n");
    return 0;
}
