/*
 * SPDX-License-Identifier: Apache-2.0
 * 生词本的宿主端单测：先造一个临时目录，把纯逻辑（原句抓取、TSV 转义）和
 * 落盘逻辑（保存、去重、移除、写满、索引分配失败退化）全部跑一遍。
 * Host-side unit tests for the vocabulary book: point the module at a temp
 * directory and exercise both the pure logic (sentence capture, TSV escaping)
 * and the storage logic (save, de-duplication, remove, cap, index degradation).
 *
 * Host build (needs only a C compiler):
 *   gcc -std=gnu11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -g \
 *       -fsanitize=address,undefined \
 *       -Imain/dict -Imain/book -Itools/vocab_log_stubs -Itools/dict_hit_stubs \
 *       -o build/book-tests/vocab_log tools/vocab_log_host_test.c
 *   ./build/book-tests/vocab_log      # prints VOCAB_PASS on success
 */
#define _GNU_SOURCE

// 被测模块：直接包含 .c，这样里面的静态纯函数也能被覆盖到。
// Include the .c directly so the static pure helpers are reachable.
#define VOCAB_DIR "/tmp/rp-vocab-test"
#define VOCAB_PATH "/tmp/rp-vocab-test/vocab.txt"
#define VOCAB_TMP "/tmp/rp-vocab-test/.vocab.tmp"
#define VOCAB_VIEW "/tmp/rp-vocab-test/vocab_view.txt"
#define VOCAB_VIEW_TMP "/tmp/rp-vocab-test/.vocab_view.tmp"
#define VOCAB_CHUNK 3 // 缩小分章粒度，几条就能测到「第N章」/ shrink the chunk to exercise chapters
#define VOCAB_FILE_MAX 4096 // 缩小上限，好把「写满」也测到 / shrink the cap to test it

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int vocab_test_alloc_fail = -1; /* -1 = 不注入失败 / no injected failures */
int64_t vocab_test_now_us = 0;   /* AI 批时钟，测试推进它 / the AI batch clock */
int vocab_test_nvs_autosave = -1; /* -1 = NVS 无此键 / key absent */

#include "vocab_log.h"
#include "vocab_log.c"

// 书名的替身：直接用去扩展名的文件名，方便断言。
// Title stub: the basename without its extension, easy to assert on.
bool book_title_get(const char* path, char* out, size_t cap) {
    (void)path;
    (void)out;
    (void)cap;
    return false;
}
bool book_title_from_path(const char* path, char* out, size_t cap) {
    const char* slash = strrchr(path ? path : "", '/');
    const char* base = slash ? slash + 1 : (path ? path : "");
    snprintf(out, cap, "%s", base);
    char* dot = strrchr(out, '.');
    if (dot) *dot = 0;
    return true;
}

static int failures;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ++failures;                                                        \
            printf("FAIL %s:%d: ", __func__, __LINE__);                        \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

static void reset_book(void) {
    vocab_log_invalidate();
    remove(VOCAB_PATH);
    remove(VOCAB_TMP);
    remove(VOCAB_VIEW);
    remove(VOCAB_VIEW_TMP);
}

// 虚拟书里有没有这一行。/ Does the rendered view contain this text?
static bool view_has(const char* needle) {
    FILE* f = fopen(VOCAB_VIEW, "r");
    if (!f) return false;
    char line[VOCAB_LINE_MAX];
    bool found = false;
    while (!found && fgets(line, sizeof(line), f))
        if (strstr(line, needle)) found = true;
    fclose(f);
    return found;
}

// 虚拟书里这一串出现了几次（用来数章节标题）。/ How often it appears (chapter count).
static int view_count_of(const char* needle) {
    FILE* f = fopen(VOCAB_VIEW, "r");
    if (!f) return -1;
    char line[VOCAB_LINE_MAX];
    int n = 0;
    while (fgets(line, sizeof(line), f))
        for (const char* p = line; (p = strstr(p, needle)); ++p) ++n;
    fclose(f);
    return n;
}

// 直接手写一份 vocab.txt，让渲染器脱离 save() 单独被测。
// Hand-write a vocab.txt so the renderer is tested independently of save().
static void write_tsv(int rows, const char* word_fmt) {
    FILE* f = fopen(VOCAB_PATH, "w");
    assert(f);
    fprintf(f, "%s\n", VOCAB_HEADER);
    char word[64];
    for (int i = 0; i < rows; ++i) {
        snprintf(word, sizeof(word), word_fmt, i);
        fprintf(f, "%s\t2026-10-09 23:%02d\tBook %d\tA sentence with %s in it.\t[/f/%d/] / n. 释义%d\n",
                word, i % 60, i, word, i, i);
    }
    fclose(f);
}

static int count_lines(void) {
    FILE* f = fopen(VOCAB_PATH, "r");
    if (!f) return 0;
    char line[VOCAB_LINE_MAX];
    int n = 0;
    while (fgets(line, sizeof(line), f)) ++n;
    fclose(f);
    return n;
}

static int fields_of(const char* line) {
    int n = 1;
    for (const char* p = line; *p && *p != '\n'; ++p)
        if (*p == '\t') ++n;
    return n;
}

/* ---------- 1. 纯字符串逻辑 ---------- */

static void test_escape(void) {
    char out[64];
    field_escape(out, sizeof(out), "  a\t\tb\n\n  c  ");
    CHECK(!strcmp(out, "a b c"), "collapse/trim: got '%s'", out);
    field_escape(out, sizeof(out), "");
    CHECK(!out[0], "empty stays empty: '%s'", out);
    field_escape(out, sizeof(out), "   ");
    CHECK(!out[0], "spaces only: '%s'", out);
    // 分开写：C 的十六进制转义会贪吃掉后面的 b（"\x02b" 其实是一个字符 0x2B）
    // Split the literal: a C hex escape would swallow the following 'b'.
    field_escape(out, sizeof(out), "a\x01\x02" "b");
    CHECK(!strcmp(out, "a b"), "control chars become spaces: '%s'", out);
    char big[200];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    field_escape(out, 8, big);
    CHECK(strlen(out) == 7, "truncation respects cap: len=%zu", strlen(out));
}

static void test_sentence(void) {
    const char* text = "The child's immaturity showed in every choice. Then he grew up.";
    vocab_log_capture("/sdcard/books/atomic.epub", text, 12, 22);
    CHECK(!strcmp(s_sentence, "The child's immaturity showed in every choice."),
          "whole sentence: '%s'", s_sentence);

    // 中文句末也要能断开 / CJK terminators must break too
    vocab_log_capture("/sdcard/books/x.epub", "前面一句。这里有个 immaturity 的词。后面", 15, 26);
    CHECK(strstr(s_sentence, "immaturity") != NULL, "CJK sentence keeps the word: '%s'", s_sentence);

    // 超长：从尾截断，但不能切出半个汉字
    // Very long: truncate at the tail but never split a multibyte char
    char long_text[4096];
    memset(long_text, 'a', sizeof(long_text) - 1);
    long_text[sizeof(long_text) - 1] = 0;
    memcpy(long_text + 100, "target", 6);
    vocab_log_capture("/sdcard/books/x.epub", long_text, 100, 106);
    CHECK(s_sentence[0] == 'a' && strstr(s_sentence, "target") != NULL,
          "long capture keeps the word: '%.40s…'", s_sentence);

    // 越界输入不得崩 / Out-of-range input must not crash
    vocab_log_capture("/sdcard/books/x.epub", "short", 999, 1000);
    CHECK(!s_sentence[0], "out-of-range capture stays empty: '%s'", s_sentence);
    vocab_log_capture(NULL, NULL, 0, 0);
    CHECK(!s_sentence[0] && !s_book[0], "null capture is a no-op");
}

/* ---------- 2. 落盘与去重 ---------- */

static void test_save_remove(void) {
    reset_book();
    vocab_log_warm();
    CHECK(!vocab_log_contains("apple"), "a fresh book contains nothing");

    vocab_log_capture("/sdcard/books/atomic.epub", "Hello apple pie.", 6, 11);
    CHECK(vocab_log_save("Apple", "n. 苹果"), "save failed");
    CHECK(count_lines() == 2, "header + one entry, got %d", count_lines());
    CHECK(vocab_log_contains("apple"), "lookup is case-insensitive");
    CHECK(vocab_log_contains("APPLE"), "lookup is case-insensitive (upper)");

    // 行的列数必须是 5，且书名用的是文件名 / rows must have 5 fields
    FILE* f = fopen(VOCAB_PATH, "r");
    char line[VOCAB_LINE_MAX];
    int checked = 0;
    while (f && fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        CHECK(fields_of(line) == 5, "row must have 5 fields, got %d: %s", fields_of(line), line);
        CHECK(strstr(line, "atomic") != NULL, "row carries the book name: %s", line);
        CHECK(strstr(line, "Hello apple pie.") != NULL, "row carries the sentence: %s", line);
        ++checked;
    }
    if (f) fclose(f);
    CHECK(checked == 1, "exactly one data row, got %d", checked);

    // 重复保存必须被去重挡掉 / a repeat save must be de-duplicated
    CHECK(vocab_log_save("apple", "n. 苹果"), "repeat save reports success");
    CHECK(count_lines() == 2, "repeat save added no row, got %d", count_lines());

    CHECK(vocab_log_remove("apple"), "remove failed");
    CHECK(!vocab_log_contains("apple"), "removed word is gone");
    CHECK(count_lines() == 1, "only the header survives, got %d", count_lines());
    CHECK(!vocab_log_remove("apple"), "removing again is a no-op");

    CHECK(!vocab_log_save("", "x"), "empty word is rejected");
    CHECK(!vocab_log_save(NULL, "x"), "null word is rejected");
}

static void test_remove_keeps_others(void) {
    reset_book();
    vocab_log_warm();
    vocab_log_capture("/sdcard/books/b.epub", "a pear and an apple", 2, 6);
    CHECK(vocab_log_save("pear", "n. 梨"), "save pear");
    vocab_log_capture("/sdcard/books/b.epub", "a pear and an apple", 14, 19);
    CHECK(vocab_log_save("apple", "n. 苹果"), "save apple");
    CHECK(count_lines() == 3, "two entries, got %d", count_lines());
    CHECK(vocab_log_remove("pear"), "remove pear");
    CHECK(!vocab_log_contains("pear") && vocab_log_contains("apple"), "only pear went away");
    CHECK(count_lines() == 2, "one entry left, got %d", count_lines());
}

/* ---------- 3. 索引分配失败必须退化，不能卡死也不能崩 ---------- */

static void test_index_degradation(void) {
    reset_book();
    vocab_test_alloc_fail = 1; // 让索引的 calloc 失败一次
    vocab_log_warm();
    CHECK(s_words == NULL, "index stays NULL when the allocation fails");
    CHECK(!vocab_log_contains("pear"), "contains must not fall back to reading the card");

    vocab_log_capture("/sdcard/books/b.epub", "a pear", 2, 6);
    CHECK(vocab_log_save("pear", "n. 梨"), "save still works without an index");
    CHECK(vocab_log_save("pear", "n. 梨"), "second save still succeeds");
    CHECK(count_lines() == 2, "file-scan de-duplication held, got %d", count_lines());
    // 之后索引仍然不可用，contains 依旧返回 false（宁可显示「保存」）
    CHECK(!vocab_log_contains("pear"), "still no card read on the hot path");
    vocab_test_alloc_fail = -1;
}

/* ---------- 4. 写满后拒绝追加 ---------- */

static void test_file_cap(void) {
    reset_book();
    vocab_log_warm();
    char word[32];
    int saved = 0;
    for (int i = 0; i < 200; ++i) {
        snprintf(word, sizeof(word), "word%03d", i);
        vocab_log_capture("/sdcard/books/b.epub", "a word here", 2, 6);
        if (!vocab_log_save(word, "n. 一个词，占位用。")) break;
        ++saved;
    }
    struct stat st;
    CHECK(stat(VOCAB_PATH, &st) == 0 && (uint64_t)st.st_size >= VOCAB_FILE_MAX,
          "the file must stop at the cap, size=%ld", stat(VOCAB_PATH, &st) ? -1 : (long)st.st_size);
    CHECK(saved > 0 && saved < 200, "cap actually triggered after %d entries", saved);
}

/* ---------- 5. 虚拟书渲染 ---------- */

static void test_render_basic(void) {
    reset_book();
    vocab_log_warm();
    vocab_log_capture("/sdcard/books/atomic.epub", "Hello apple pie.", 6, 11);
    CHECK(vocab_log_save("apple", "[/ˈæpl/] / n. 苹果"), "save with a phonetic");
    vocab_log_capture("/sdcard/books/atomic.epub", "A pear today.", 2, 6);
    CHECK(vocab_log_save("pear", "n. 梨"), "save without a phonetic");
    vocab_log_capture("/sdcard/books/atomic.epub", "Cherry on top.", 0, 6);
    CHECK(vocab_log_save("cherry", "[/ˈtʃeri/] / n. 樱桃"), "save a third");

    size_t count = 0;
    CHECK(vocab_log_render(&count), "render failed");
    CHECK(count == 3, "render counted %zu entries", count);
    CHECK(view_has("# 我的生词本（共 3 词）"), "header carries the total");
    CHECK(view_has("第1章"), "the first chapter heading is written");
    CHECK(view_has("1. apple"), "entries are numbered");
    CHECK(view_has("3. cherry"), "the last entry is numbered 3");
    // 音标要被拆出来单独排版 / the phonetic must be split out and laid out
    CHECK(view_has("   [/ˈæpl/] n. 苹果"), "phonetic split: entry 1");
    CHECK(view_has("   n. 梨"), "no phonetic, no brackets: entry 2");
    CHECK(view_has("   原句：Hello apple pie."), "the source sentence is reproduced");
    CHECK(view_has("[保存时间："), "the timestamp line is written");
    CHECK(view_has(" · atomic"), "the book name is written");
    CHECK(!view_has("\t"), "the view must not leak raw TSV tabs");
    // 每 VOCAB_CHUNK(3) 条一章：3 条只有 1 章
    CHECK(view_count_of("第") == 1, "3 entries fit in one chapter, got %d", view_count_of("第"));
}

static void test_render_empty(void) {
    reset_book();
    size_t count = 99;
    CHECK(vocab_log_render(&count), "rendering with no vocab.txt must still succeed");
    CHECK(count == 0, "an empty book reports 0, got %zu", count);
    CHECK(view_has("共 0 词"), "the empty view says so");
    CHECK(view_has("还没有保存任何生词"), "the empty view explains what to do");
}

// 1000 条：手写 TSV（绕开写满上限），渲染不得崩、章数要对。
// 1000 entries: hand-written TSV (bypassing the cap); must not crash and the
// chapter count must line up.
static void test_render_many(void) {
    reset_book();
    write_tsv(1000, "word%04d");
    size_t count = 0;
    CHECK(vocab_log_render(&count), "rendering 1000 entries failed");
    CHECK(count == 1000, "counted %zu of 1000", count);
    int expected = (1000 + VOCAB_CHUNK - 1) / VOCAB_CHUNK;
    CHECK(view_count_of("章\n") == expected, "expected %d chapter headings, got %d",
          expected, view_count_of("章\n"));
    CHECK(view_has("1. word0000"), "first entry present");
    CHECK(view_has("1000. word0999"), "last entry numbered 1000");
    CHECK(view_has("第334章"), "the last chapter heading is 第334章");
}

// 残缺行（没有 TAB）不能被当成词条渲染出来。
// A malformed row with no tab must not become an entry.
static void test_render_malformed(void) {
    reset_book();
    FILE* f = fopen(VOCAB_PATH, "w");
    assert(f);
    fprintf(f, "%s\n", VOCAB_HEADER);
    fprintf(f, "garbage line without tabs\n");
    fprintf(f, "apple\t2026-10-09 23:12\tatomic\tA sentence.\t[/ˈæpl/] / n. 苹果\n");
    fclose(f);
    size_t count = 0;
    CHECK(vocab_log_render(&count), "render failed");
    CHECK(count == 1, "the malformed row was skipped, got %zu", count);
    CHECK(!view_has("garbage"), "the malformed row never reaches the view");
}

/* ---------- 6. 误删撤销 + 虚拟书的上下文守卫 ---------- */

static void test_undo(void) {
    reset_book();
    vocab_log_warm();
    vocab_log_capture("/sdcard/books/atomic.epub", "Hello apple pie.", 6, 11);
    CHECK(vocab_log_save("apple", "[/ˈæpl/] / n. 苹果"), "save apple");

    FILE* f = fopen(VOCAB_PATH, "r");
    assert(f);
    char line[VOCAB_LINE_MAX], original[VOCAB_LINE_MAX];
    while (fgets(line, sizeof(line), f)) if (line[0] != '#') break;
    fclose(f);
    snprintf(original, sizeof(original), "%s", line);
    CHECK(strstr(original, "Hello apple pie.") != NULL, "the original row has its sentence");

    CHECK(vocab_log_remove("apple"), "remove apple");
    CHECK(vocab_log_save("apple", "垃圾释义 must not be used"), "undo save succeeded");
    f = fopen(VOCAB_PATH, "r");
    assert(f);
    while (fgets(line, sizeof(line), f)) if (line[0] != '#') break;
    fclose(f);
    CHECK(!strcmp(line, original), "the undo wrote the row back verbatim:\n  %s  vs\n  %s",
          line, original);
    CHECK(!strstr(line, "垃圾释义"), "the undo must not use the freshly passed definition");
    CHECK(vocab_log_contains("apple"), "the index caught up after the undo");
    // 撤销只能用一次：撤销之后再存同一个词，走的是去重分支，不再追加一行。
    // Single-shot: saving the same word again after an undo hits de-duplication.
    CHECK(vocab_log_save("apple", "n. 另一个释义"), "save after the undo");
    CHECK(count_lines() == 2, "no second row appeared, got %d", count_lines());
}

static void test_view_guard(void) {
    CHECK(vocab_log_is_view_path(VOCAB_VIEW), "the view path is recognized");
    CHECK(vocab_log_is_view_path("/sdcard/books/vocab_view.txt"),
          "a copy opened from elsewhere is still recognized");
    CHECK(!vocab_log_is_view_path("/sdcard/books/atomic.epub"), "a real book is not the view");
    CHECK(!vocab_log_is_view_path(NULL), "NULL is not the view");
    CHECK(!vocab_log_is_view_path(""), "an empty path is not the view");

    // 深比较，不是指针比较：内容一样但来自另一份缓冲的路径，必须照样认出来。
    // Deep compare, not pointer compare: an identical path held in a different
    // buffer must still be recognized.
    char copy[256];
    const char* literal = VOCAB_VIEW;  // 不直接比字面量，免得 -Werror=address 报警
    snprintf(copy, sizeof(copy), "%s", VOCAB_VIEW);
    CHECK((const char*)copy != literal, "the copy lives at another address");
    CHECK(vocab_log_is_view_path(copy), "a path with the same contents is recognized");

    // FAT 不区分大小写：换个大小写打开的是同一个文件，守卫必须跟着认。
    // FAT is case-insensitive: the same file under another casing must be caught.
    CHECK(vocab_log_is_view_path("/sdcard/.readpico/VOCAB_VIEW.TXT"), "upper case is the same file");
    CHECK(vocab_log_is_view_path("/sdcard/books/Vocab_View.txt"), "mixed case is the same file");

    // 反过来，名字里"含"这个词但不是视图的文件绝不能误判：误判会让 app_book.c
    // 的 DICT 分支在真书里删词时把虚拟书顶上来。
    // Names that merely contain the word must not match: a false positive makes
    // app_book.c reopen the view over the book the user is actually reading.
    CHECK(!vocab_log_is_view_path("/sdcard/books/my_vocab_view.txt"), "a prefixed name is not the view");
    CHECK(!vocab_log_is_view_path("/sdcard/books/vocab_view.txt.bak"), "an extension is not the view");
    CHECK(!vocab_log_is_view_path("/sdcard/vocab_view"), "a directory-ish name is not the view");

    // 在虚拟书里长按：原句缓冲必须被清空，绝不能把视图自己的一行当成原句。
    // A long press in the view must clear the sentence buffer, never capture a row
    // of the view itself.
    vocab_log_capture("/sdcard/books/atomic.epub", "Hello apple pie.", 6, 11);
    CHECK(s_sentence[0] != 0, "a real capture fills the buffer");
    vocab_log_capture(VOCAB_VIEW, "1. apple", 3, 8);
    CHECK(!s_sentence[0], "a capture inside the view clears the buffer, got '%s'", s_sentence);
    CHECK(strstr(s_book, "atomic") != NULL, "the book context is untouched, got '%s'", s_book);
    // 换一处存放、换个大小写，守卫一样要生效。
    // The guard must hold for a relocated copy and for a different casing too.
    vocab_log_capture("/sdcard/books/atomic.epub", "Hello apple pie.", 6, 11);
    CHECK(s_sentence[0] != 0, "the buffer is filled again for the next case");
    vocab_log_capture("/sdcard/books/VOCAB_VIEW.TXT", "1. apple", 3, 8);
    CHECK(!s_sentence[0], "an upper-case copy is guarded too, got '%s'", s_sentence);
}

/* ---------- 8. AI 交互词攒批（gen26.5 M2 Step8） ---------- */

// 取出某词那一行（小写比词头，与批内口径一致）。/ Fetch one word's row (lower-cased
// head, same as inside the batch). 返回静态缓冲，仅供即时断言。
static const char* row_of(const char* word) {
    static char out[VOCAB_LINE_MAX];
    char lower[VOCAB_WORD_MAX];
    lower_copy(lower, sizeof(lower), word);
    FILE* f = fopen(VOCAB_PATH, "r");
    if (!f) return NULL;
    char line[VOCAB_LINE_MAX];
    const char* ret = NULL;
    while (fgets(line, sizeof(line), f)) {
        char head[VOCAB_WORD_MAX];
        size_t n = strcspn(line, "\t");
        if (n >= sizeof(head)) n = sizeof(head) - 1;
        memcpy(head, line, n);
        head[n] = 0;
        if (head[0] && !strcmp(head, lower)) {
            snprintf(out, sizeof(out), "%s", line);
            ret = out;
            break;
        }
    }
    fclose(f);
    return ret;
}

static void test_ai_batch(void) {
    reset_book();
    // NVS 三态：键不存在 / 怪值 → 缺省开；明确 0 → 关；明确 1 → 开。
    // Three NVS states: absent / odd -> default on; explicit 0 -> off; 1 -> on.
    vocab_test_nvs_autosave = -1;
    vocab_log_autosave_init();
    CHECK(vocab_log_autosave_enabled(), "a missing NVS key defaults to on");
    vocab_test_nvs_autosave = 2;
    vocab_log_autosave_init();
    CHECK(vocab_log_autosave_enabled(), "an odd value counts as the default on");
    vocab_test_nvs_autosave = 0;
    vocab_log_autosave_init();
    CHECK(!vocab_log_autosave_enabled(), "an explicit 0 turns the switch off");
    // 关着：入队即丢，时钟走多久都不落盘。/ Off: dropped at queue time, never lands.
    vocab_log_capture("/sdcard/books/atomic.epub", "Hello apple pie.", 6, 11);
    vocab_log_ai_queue("apple");
    vocab_test_now_us += 10 * 1000 * 1000;
    vocab_log_ai_flush();
    CHECK(access(VOCAB_PATH, F_OK) != 0, "a disabled switch writes nothing");

    vocab_test_nvs_autosave = 1;
    vocab_log_autosave_init();
    CHECK(vocab_log_autosave_enabled(), "an explicit 1 turns the switch on");

    // 5s 窗口：未到不写，到期一次写全部积压。/ The 5s window: nothing before the
    // deadline, everything in one write after it.
    vocab_log_capture("/sdcard/books/atomic.epub", "Hello apple pie.", 6, 11);
    vocab_log_ai_queue("apple");
    vocab_log_ai_queue("Apple"); // 同词不同大小写：批内只留一条 / one row per word
    vocab_log_capture("/sdcard/books/atomic.epub", "The banana split was nice.", 4, 10);
    vocab_log_ai_queue("banana");
    CHECK(count_lines() == 0, "nothing is written while the batch waits");
    vocab_test_now_us += 4 * 1000 * 1000;
    vocab_log_ai_flush();
    CHECK(count_lines() == 0, "still nothing at 4s (deadline is 5s from the first word)");
    vocab_test_now_us += 2 * 1000 * 1000;
    vocab_log_ai_flush();
    CHECK(count_lines() == 3, "one header + two rows in a single write, got %d", count_lines());
    vocab_log_ai_flush();
    CHECK(count_lines() == 3, "a second flush writes nothing (the batch is gone)");

    // 行内容：词头、时间、书名、原句、释义（此时释义未回，写 "-"）。
    // Row contents: word, timestamp, book, sentence, definition ("-" until it lands).
    const char* apple = row_of("apple");
    CHECK(apple != NULL, "the apple row exists");
    if (apple) {
        CHECK(fields_of(apple) == 5, "the row still has five fields, got %d", fields_of(apple));
        CHECK(strstr(apple, "Hello apple pie.") != NULL, "the sentence is snapshotted");
        CHECK(strstr(apple, "atomic") != NULL, "the book name is carried");
        CHECK(strstr(apple, "\t-\n") != NULL, "an empty definition column reads '-'");
    }

    // 原句快照：入队后抓新句子，各词落盘的还是各自那一句。
    // Sentence snapshot: a later capture must not rewrite an already-queued row.
    const char* banana = row_of("banana");
    CHECK(banana != NULL && strstr(banana, "The banana split was nice.") != NULL,
          "banana keeps its own sentence");

    // 释义后补：0x07 回来时补进批，随行落盘。/ A late definition rides with the row.
    vocab_log_capture("/sdcard/books/atomic.epub", "A cherry on top.", 2, 8);
    vocab_log_ai_queue("cherry");
    vocab_log_ai_set_def("cherry", "n. 樱桃");
    vocab_log_ai_set_def("durian", "n. 榴莲"); // 批外词：静默忽略 / not batched: ignored
    vocab_test_now_us += 10 * 1000 * 1000;
    vocab_log_ai_flush();
    const char* cherry = row_of("cherry");
    CHECK(cherry != NULL && strstr(cherry, "n. 樱桃") != NULL,
          "a definition that landed before the flush is written");
    CHECK(row_of("durian") == NULL, "a word outside the batch is never written");
    CHECK(count_lines() == 4, "only cherry was appended, got %d", count_lines());

    // 与手动保存去重：卡上已有的词，批 flush 不重复写。
    // De-dup against the manual save: a word already on the card is not re-written.
    vocab_log_capture("/sdcard/books/atomic.epub", "An elderberry bush.", 3, 13);
    vocab_log_save("elderberry", "n. 接骨木");
    vocab_log_ai_queue("elderberry"); // 模拟同一词走了 AI 又入批 / same word, AI too
    vocab_test_now_us += 10 * 1000 * 1000;
    vocab_log_ai_flush();
    CHECK(count_lines() == 5, "an already-saved word adds no second row, got %d", count_lines());

    // 批满：第 65 个词被丢（日志可见），前 64 个照常落盘。/ Full batch: the 65th
    // word is dropped (visible in the log), the first 64 land as usual.
    reset_book();
    vocab_test_nvs_autosave = 1;
    vocab_log_autosave_init();
    for (int i = 0; i < VOCAB_AI_BATCH_MAX + 1; ++i) {
        char w[32];
        snprintf(w, sizeof(w), "w%02d", i);
        vocab_log_ai_queue(w);
    }
    vocab_test_now_us += 10 * 1000 * 1000;
    vocab_log_ai_flush();
    CHECK(count_lines() == 1 + VOCAB_AI_BATCH_MAX,
          "a full batch writes 64 rows, got %d", count_lines());

    // 写失败保留批次：VOCAB_DIR 被一个文件占着（dir_ready 的 EEXIST 宽进放行它），
    // open 失败 → 批留着；恢复目录后重试成功。
    // A failed write keeps the batch: with VOCAB_DIR shadowed by a plain file
    // (dir_ready's EEXIST leniency lets it through), open fails and the batch
    // waits; once the directory is back the retry lands it.
    reset_book();
    vocab_test_nvs_autosave = 1;
    vocab_log_autosave_init();
    vocab_log_ai_queue("fig");
    remove(VOCAB_DIR);
    FILE* shadow = fopen(VOCAB_DIR, "w");
    assert(shadow);
    fclose(shadow);
    vocab_test_now_us += 10 * 1000 * 1000;
    vocab_log_ai_flush();
    CHECK(access(VOCAB_PATH, F_OK) != 0, "a failed open writes nothing");
    remove(VOCAB_DIR);
    mkdir(VOCAB_DIR, 0777);
    vocab_log_ai_flush();
    CHECK(count_lines() == 2, "the retry lands the batch once the card is back, got %d",
          count_lines());

    // invalidate 丢批：书上下文没了，≤5s 的积压静默作废。
    // invalidate drops the batch: the book context is gone, at most 5s lost silently.
    reset_book();
    vocab_test_nvs_autosave = 1;
    vocab_log_autosave_init();
    vocab_log_ai_queue("grape");
    vocab_log_invalidate();
    vocab_test_now_us += 10 * 1000 * 1000;
    vocab_log_ai_flush();
    CHECK(access(VOCAB_PATH, F_OK) != 0, "invalidate discards the batch");
}

int main(void) {
    test_escape();
    test_sentence();
    test_save_remove();
    test_remove_keeps_others();
    test_index_degradation();
    test_file_cap();
    test_render_basic();
    test_render_empty();
    test_render_many();
    test_render_malformed();
    test_undo();
    test_view_guard();
    test_ai_batch();
    reset_book();
    if (failures) {
        printf("FAILURES: %d\n", failures);
        return 1;
    }
    printf("VOCAB_PASS\n");
    return 0;
}
