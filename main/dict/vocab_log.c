/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：生词本落盘。TSV 一行一条：词 / 时间 / 书名 / 原句 / 释义。
 * English: Vocabulary-book storage. One TSV row per entry:
 * word / timestamp / book / sentence / definition.
 *
 * gen26.5 契约修订（M2 Step8，用户裁定）：chip 手动保存之外，**AI 交互词自动落库**——
 * miss 词 0x17 发出即落、词典命中卡上点「AI」chip 即落、纯词典命中不落。自动落库受
 * NVS 开关 `rp_vocab`/`autosave`（缺省开）控制，走攒批写：入内存批、5s 合批单次写
 * TF 卡（防 Flash 磨损）；写失败静默且保留批次重试。
 * gen26.5 contract revision (M2 Step8, user-adjudicated): on top of the manual chip
 * save, AI-interacted words log automatically - a dictionary-miss word the moment
 * its 0x17 goes out, a dict-hit word when the "AI" chip is tapped, a pure dictionary
 * hit never. Automatic logging honors the NVS switch "rp_vocab"/"autosave" (on by
 * default) and is batched: words wait in memory and land in one write 5s later
 * (guards the card against wear); a failed write stays silent and keeps the batch
 * for a retry.
 *
 * 保留的冻结语义：绝不因为生词本而卡住界面（索引分配失败退化为扫文件；批写失败静默）。
 * Kept frozen semantics: the vocabulary book must never stall the UI (index
 * allocation failure degrades to a file scan; a failed batch write stays silent).
 */
#include "vocab_log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "book_title.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"

static const char* TAG = "vocab";

// 宿主测试会用 -D 覆盖这三条，指向临时目录。
// The host test overrides these three with -D to point at a temp directory.
#ifndef VOCAB_DIR
#define VOCAB_DIR "/sdcard/.readpico"
#endif
#ifndef VOCAB_PATH
#define VOCAB_PATH "/sdcard/.readpico/vocab.txt"
#endif
#ifndef VOCAB_TMP
#define VOCAB_TMP "/sdcard/.readpico/.vocab.tmp"
#endif
// 虚拟书：把 TSV 渲染成排版好的 TXT，交给现成阅读器打开。
// The virtual book: the TSV rendered into a laid-out TXT the existing reader opens.
#ifndef VOCAB_VIEW
#define VOCAB_VIEW "/sdcard/.readpico/vocab_view.txt"
#endif
#ifndef VOCAB_VIEW_TMP
#define VOCAB_VIEW_TMP "/sdcard/.readpico/.vocab_view.tmp"
#endif
// 每多少词起一章。标题必须写成「第N章」（中间无空格），book_txt.c 的 heading() 才认。
// Entries per chapter. The title must read "第N章" with no space or book_txt.c's
// heading() will not recognize it.
#ifndef VOCAB_CHUNK
#define VOCAB_CHUNK 50
#endif

// AI 交互词自动落库的 NVS 开关：namespace "rp_vocab"、键 "autosave"、u8、
// **缺省开**（读不到/读失败/怪值都按开；只有明确 0 才是关）。
// NVS switch for automatic AI-word logging: namespace "rp_vocab", key
// "autosave", u8, **on by default** (missing read / failure / odd value all count
// as on; only an explicit 0 is off).
#ifndef VOCAB_NVS_NS
#define VOCAB_NVS_NS "rp_vocab"
#endif
#ifndef VOCAB_NVS_KEY_AUTOSAVE
#define VOCAB_NVS_KEY_AUTOSAVE "autosave"
#endif

// AI 批：5s 合批单次写 TF 卡（防 Flash 磨损）。上限 64 词——5 秒内不可能真点这么多，
// 满了丢新的（日志可见），绝不为了攒批阻塞查词路径。
// AI batch: one write 5s later (guards the card against wear). Cap 64 words - no
// one taps that many in 5s; when full new words are dropped (visible in the log),
// never blocking the lookup path to grow a batch.
#ifndef VOCAB_AI_BATCH_DELAY_US
#define VOCAB_AI_BATCH_DELAY_US (5 * 1000 * 1000)
#endif
#ifndef VOCAB_AI_BATCH_MAX
#define VOCAB_AI_BATCH_MAX 64
#endif
#ifndef VOCAB_AI_DEF_MAX
#define VOCAB_AI_DEF_MAX 768 ///< gen26.6 D3：批内 def 槽与设备 PUSH_AI_DEF_MAX 同源加宽到卡容量
#endif
// 与设备释义槽同源：批内 def 槽不得窄于卡容量，否则同一条释义在落库时被截断
// （生词本内容与卡面不一致）。缩槽在此编译失败。
// Shares the device def slot: the batch slot must not be narrower than the card
// capacity, or the same definition is truncated on the way into the vocabulary
// book (its content would disagree with the card). A shrink fails here.
_Static_assert(VOCAB_AI_DEF_MAX >= 768,
               "gen26.6 D3: VOCAB_AI_DEF_MAX must stay >= 768 (shares the card capacity)");

#define VOCAB_HEADER "# read-pico vocab.txt — 列：word<TAB>时间<TAB>书名<TAB>原句<TAB>释义"
#define VOCAB_INDEX_MAX 4096u
#ifndef VOCAB_FILE_MAX // 宿主测试会把它缩小，好把「写满」也测到 / the host test shrinks it to exercise the cap
#define VOCAB_FILE_MAX (2u * 1024u * 1024u)
#endif
// 原句向前后各扫多少源字节。有界，所以抓取不会退化成整章扫描。
// How far to scan each way for sentence bounds; bounded so capture never becomes
// a whole-chapter walk.
#define VOCAB_SCAN_MAX 240
#define VOCAB_SENTENCE_MAX 256
#define VOCAB_LINE_MAX 1024

/* 一次长按的上下文：书名路径与抓下来的原句。定长，不做动态分配，
 * 于是「抓上下文」这条路根本没有失败分支。
 * One long press of context: the book path and the captured sentence. Fixed size
 * and never allocated, so capturing has no failure path at all. */
static char s_book[288];
static char s_sentence[VOCAB_SENTENCE_MAX];

/* 刚被移除的那一行原文。紧接着的「保存」把它原样写回，于是误删可撤销——
 * 也顺带避免了「在生词本视图里删了再存，会用视图里抓来的句子污染这一行」。
 * The row just removed. An immediate "save" writes it back verbatim, so an
 * accidental removal is undoable — and a removed-then-re-saved entry can never
 * be polluted by a sentence captured inside the vocabulary view itself. */
static char s_undo[VOCAB_LINE_MAX];

/* AI 批：入队时快照词、释义（可后补）、原句。定长、PSRAM，无失败分支之外的抖动。
 * 释义列后补：0x17 发出时释义还没回来，入批只记词；0x07 到达经 set_def 补上。
 * The AI batch: word, definition (may arrive later) and sentence, snapshotted at
 * queue time. Fixed size, PSRAM. The definition is filled in later: when 0x17 goes
 * out the answer has not returned yet, so only the word is queued; 0x07 feeds it
 * through set_def. */
typedef struct {
    char word[VOCAB_WORD_MAX];   // 已小写化（与索引同口径）/ already lower-cased
    char def[VOCAB_AI_DEF_MAX];  // 释义，空 = 未回 / empty until the answer lands
    char sentence[VOCAB_SENTENCE_MAX]; // 入队时的原句快照 / the sentence at queue time
} ai_batch_entry_t;

static ai_batch_entry_t* s_batch;      // PSRAM，惰性分配 / lazily allocated
static size_t s_batch_count;
static int64_t s_batch_deadline_us;    // 首次入队 + 5s；0 = 无批 / first queue + 5s
static bool s_autosave = true;         // 缺省开，init 时读 NVS / on until NVS says no

/* 去重索引：PSRAM 里的有序 char* 数组。分配失败就保持 NULL，
 * 由 save() 退化成逐行扫文件；contains() 只读它，绝不读卡。
 * De-duplication index: a sorted char* array in PSRAM. On allocation failure it
 * stays NULL and save() degrades to a file scan; contains() only reads it and
 * never touches the card. */
static char** s_words;
static size_t s_count;

static bool dir_ready(void) {
    struct stat st;
    if (stat(VOCAB_DIR, &st) == 0 && S_ISDIR(st.st_mode)) return true;
    if (mkdir(VOCAB_DIR, 0777) == 0) return true;
    return errno == EEXIST;
}

/* ---------- 纯字符串工具（宿主端单测覆盖） ---------- */

// 字段转义：\t \r \n 与控制字符变空格，连续空格压缩成一个，两端去空格。
// Field escaping: tabs, CR/LF and control characters become spaces, runs of
// spaces collapse to one, and both ends are trimmed.
static void field_escape(char* out, size_t cap, const char* in) {
    size_t o = 0;
    bool pending_space = false;
    for (const char* p = in; p && *p; ++p) {
        unsigned char c = (unsigned char)*p;
        // <= 0x20：普通空格本身就是 0x20，不能被漏掉。
        // <= 0x20: an ordinary space is 0x20 itself and must not slip through.
        bool space = c <= 0x20;
        if (space) { pending_space = o > 0; continue; }
        if (pending_space) {
            if (o + 1 >= cap) break;
            out[o++] = ' ';
            pending_space = false;
        }
        if (o + 1 >= cap) break;
        out[o++] = (char)c;
    }
    out[o] = 0;
}

// 句末判定：返回该标点的字节长度（1 或 3），0 表示不是句末。
// Sentence terminator test: returns the byte length (1 or 3), 0 when not one.
static size_t term_at(const char* t, size_t i, size_t len) {
    if (i >= len) return 0;
    unsigned char c = (unsigned char)t[i];
    if (c < 0x80) {
        if (c == '.' || c == '!' || c == '?' || c == ';' || c == '\n' || c == '\r') return 1;
        return 0;
    }
    if ((c & 0xc0) == 0x80) return 0; // 续字节不是句首 / a continuation byte never starts one
    static const char* terms[] = {"。", "！", "？", "；"};
    for (size_t k = 0; k < sizeof(terms) / sizeof(terms[0]); ++k) {
        size_t n = strlen(terms[k]);
        if (i + n <= len && !strncmp(t + i, terms[k], n)) return n;
    }
    return 0;
}

static size_t prev_cp(const char* t, size_t i) {
    if (!i) return 0;
    size_t p = i - 1;
    while (p && ((unsigned char)t[p] & 0xc0) == 0x80) --p;
    return p;
}

static size_t next_cp(const char* t, size_t i, size_t len) {
    size_t n = 1;
    while (i + n < len && ((unsigned char)t[i + n] & 0xc0) == 0x80) ++n;
    return i + n < len ? i + n : len;
}

// 从原文里切出包含 [start,end) 的整句，转义后放进 s_sentence。
// Slice the sentence containing [start,end) out of the source text and escape it
// into s_sentence.
static void sentence_capture(const char* text, size_t len, size_t start, size_t end) {
    s_sentence[0] = 0;
    if (!text || start >= len) return;
    if (end > len) end = len;
    if (end < start) end = start;

    size_t back = start > VOCAB_SCAN_MAX ? start - VOCAB_SCAN_MAX : 0;
    size_t lo = back;
    for (size_t p = prev_cp(text, start); p > back; p = prev_cp(text, p)) {
        size_t n = term_at(text, p, len);
        if (n) { lo = p + n; break; }
    }

    size_t fwd = end + VOCAB_SCAN_MAX < len ? end + VOCAB_SCAN_MAX : len;
    size_t hi = fwd;
    for (size_t q = end; q < fwd; q = next_cp(text, q, len)) {
        size_t n = term_at(text, q, len);
        if (n) { hi = q + n; break; }
    }
    if (hi <= lo) return;

    // 截断时退到 UTF-8 边界，避免切出半个汉字。
    // Back off to a UTF-8 boundary when truncating so no multibyte char is split.
    size_t take = hi - lo;
    if (take > VOCAB_SCAN_MAX * 2) take = VOCAB_SCAN_MAX * 2;
    while (take > 1 && ((unsigned char)text[lo + take] & 0xc0) == 0x80) --take;
    char raw[VOCAB_SCAN_MAX * 2 + 1];
    memcpy(raw, text + lo, take);
    raw[take] = 0;
    field_escape(s_sentence, sizeof(s_sentence), raw);
}

static void lower_copy(char* out, size_t cap, const char* in) {
    size_t i = 0;
    for (; in && in[i] && i + 1 < cap; ++i)
        out[i] = (char)(in[i] >= 'A' && in[i] <= 'Z' ? in[i] + 32 : in[i]);
    out[i] = 0;
}

static const char* stamp(char* out, size_t cap) {
    // 没有可信时钟就不编造日期（与 book_ticket.c 的 local_day() 同一判据）。
    // Without a trustworthy clock, invent no date (same test as book_ticket.c).
    time_t now = time(NULL);
    struct tm tm;
    if (now < 1704067200 || !localtime_r(&now, &tm) ||
        !strftime(out, cap, "%Y-%m-%d %H:%M", &tm) || !out[0])
        snprintf(out, cap, "-");
    return out;
}

/* ---------- 去重索引 ---------- */

// 索引条目固定放 PSRAM；分配失败由调用方退化处理。
// Index entries always live in PSRAM; callers degrade when this fails.
static char* psram_strdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) memcpy(p, s, n);
    return p;
}

static int cmp_word(const void* a, const void* b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

static bool index_find(const char* lower, size_t* pos) {
    size_t low = 0, high = s_count;
    while (low < high) {
        size_t mid = (low + high) / 2;
        int c = strcmp(s_words[mid], lower);
        if (c < 0) low = mid + 1;
        else if (c > 0) high = mid;
        else { if (pos) *pos = mid; return true; }
    }
    if (pos) *pos = low;
    return false;
}

static void index_add(const char* lower) {
    if (!s_words || !*lower) return;
    size_t pos = 0;
    if (index_find(lower, &pos)) return;
    if (s_count >= VOCAB_INDEX_MAX) {
        ESP_LOGW(TAG, "index full (%u), dropping de-duplication for new words",
                 (unsigned)VOCAB_INDEX_MAX);
        return;
    }
    char* copy = psram_strdup(lower);
    if (!copy) { ESP_LOGW(TAG, "index entry alloc failed"); return; }
    memmove(&s_words[pos + 1], &s_words[pos], (s_count - pos) * sizeof(s_words[0]));
    s_words[pos] = copy;
    ++s_count;
}

static void index_drop_at(size_t pos) {
    heap_caps_free(s_words[pos]);
    memmove(&s_words[pos], &s_words[pos + 1], (s_count - pos - 1) * sizeof(s_words[0]));
    --s_count;
}

static bool index_load(void) {
    if (s_words) return true;
    // 先分配再读：分配失败时直接退化，不去动文件。
    // Allocate before reading: on failure we degrade immediately without touching
    // the file.
    char** words = heap_caps_calloc(VOCAB_INDEX_MAX, sizeof(char*),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!words) { ESP_LOGW(TAG, "index alloc failed; saving will scan the file"); return false; }
    FILE* f = fopen(VOCAB_PATH, "r");
    if (!f) { // 还没有生词本：留一个空索引，避免每次都去开文件
        s_words = words;
        s_count = 0;
        return true;
    }
    char line[VOCAB_LINE_MAX];
    char lower[VOCAB_WORD_MAX];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        char* tab = strchr(line, '\t');
        if (tab) *tab = 0;              // 只留第一段：词头 / keep the word field only
        lower_copy(lower, sizeof(lower), line);
        if (!lower[0] || s_count >= VOCAB_INDEX_MAX) continue;
        words[s_count] = psram_strdup(lower);
        if (!words[s_count]) break;
        ++s_count;
    }
    fclose(f);
    for (size_t i = 0; i < s_count; ++i)
        if (!words[i]) { // 中途分配失败：退掉半成品，交回给扫文件模式
            for (size_t k = 0; k < s_count; ++k) heap_caps_free(words[k]);
            heap_caps_free(words);
            ESP_LOGW(TAG, "index build failed; saving will scan the file");
            return false;
        }
    qsort(words, s_count, sizeof(words[0]), cmp_word);
    s_words = words;
    return true;
}

// 索引不可用时的兜底：逐行扫一遍。只在保存/移除（用户点击）时走。
// Fallback when the index is unavailable: scan line by line. Only used from
// save/remove, i.e. on an explicit tap.
static bool file_contains(const char* lower) {
    FILE* f = fopen(VOCAB_PATH, "r");
    if (!f) return false;
    char line[VOCAB_LINE_MAX];
    char word[VOCAB_WORD_MAX];
    bool found = false;
    while (!found && fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        char* tab = strchr(line, '\t');
        if (tab) *tab = 0;
        lower_copy(word, sizeof(word), line);
        found = word[0] && !strcmp(word, lower);
    }
    fclose(f);
    return found;
}

// s_undo 的第一列（词头）是否与 lower 相同。/ Does the stashed row's word field match?
static bool undo_matches(const char* lower) {
    if (!s_undo[0]) return false;
    char head[VOCAB_WORD_MAX];
    size_t n = strcspn(s_undo, "\t");
    if (n >= sizeof(head)) n = sizeof(head) - 1;
    memcpy(head, s_undo, n);
    head[n] = 0;
    lower_copy(head, sizeof(head), head);
    return head[0] && !strcmp(head, lower);
}

// 把释义列拆成音标与正文：build_dict.py 只在音标非空时把 "[音标]" 作为第一行输出，
// 而 summary_text() 用 " / " 把前三行接起来，所以列首是 '[' 就一定能拆出音标。
// Split the definition column into phonetic and body: build_dict.py only emits a
// "[phonetic]" first line when one exists, and summary_text() joined the first
// three lines with " / ", so a leading '[' reliably marks it.
static void split_phonetic(const char* def, char* phon, size_t phon_cap, const char** body) {
    phon[0] = 0;
    *body = def ? def : "";
    if (!def || def[0] != '[') return;
    const char* close = strchr(def, ']');
    if (!close) return;
    size_t n = (size_t)(close - def) + 1;
    if (n >= phon_cap) return;
    memcpy(phon, def, n);
    phon[n] = 0;
    const char* rest = close + 1;
    while (*rest == ' ') ++rest;
    if (!strncmp(rest, "/ ", 2)) rest += 2; // summary_text() 的连接符 / summary_text()'s joiner
    *body = rest;
}

/* ---------- 对外接口 ---------- */

// 追加一行 TSV（调用方负责 open/close、header 与去重）。手动保存与 AI 批共用——
// 行格式只有这一份，杜绝「批落盘的行和手点保存的行不一样」。
// Append one TSV row (the caller opens/closes, writes the header and
// de-duplicates). The manual save and the AI batch share this: the row format
// exists exactly once, so a batch write can never drift from a chip save.
static void append_row(FILE* f, const char* lower, const char* def, const char* sentence) {
    char when[32];
    char title[sizeof(s_book)];
    if (!book_title_get(s_book, title, sizeof(title)) &&
        !book_title_from_path(s_book, title, sizeof(title)))
        snprintf(title, sizeof(title), "%s", s_book[0] ? s_book : "-");
    char book[160], def_esc[VOCAB_AI_DEF_MAX];
    field_escape(book, sizeof(book), title);
    field_escape(def_esc, sizeof(def_esc), def ? def : "");
    fprintf(f, "%s\t%s\t%s\t%s\t%s\n", lower, stamp(when, sizeof(when)),
            book[0] ? book : "-",
            (sentence && sentence[0]) ? sentence : (s_sentence[0] ? s_sentence : "-"),
            def_esc[0] ? def_esc : "-");
}

void vocab_log_capture(const char* book_path, const char* text, size_t start, size_t end) {
    // 在生词本视图里长按查词时，抓到的「书名」是视图文件、「原句」是视图里的一行——
    // 那是垃圾上下文。宁可清空也不覆盖：留着上一次真书的上下文同样会写错行。
    // A long press inside the vocabulary view would capture the view file as the
    // book and one of its own rows as the sentence. Clear instead of keeping the
    // previous real book's context: a stale one would write just as wrong a row.
    if (vocab_log_is_view_path(book_path)) {
        s_sentence[0] = 0;
        return;
    }
    s_book[0] = 0;
    if (book_path) snprintf(s_book, sizeof(s_book), "%s", book_path);
    if (text) sentence_capture(text, strlen(text), start, end);
    else s_sentence[0] = 0;
}

bool vocab_log_contains(const char* word) {
    char lower[VOCAB_WORD_MAX];
    if (!word) return false;
    lower_copy(lower, sizeof(lower), word);
    if (!lower[0]) return false;
    if (!s_words) return false; // 不读卡：宁可显示「保存」也不拖慢查词
    size_t pos = 0;
    return index_find(lower, &pos);
}

bool vocab_log_save(const char* word, const char* definition) {
    char lower[VOCAB_WORD_MAX];
    if (!word) return false;
    lower_copy(lower, sizeof(lower), word);
    if (!lower[0]) return false;
    if (!dir_ready()) { ESP_LOGW(TAG, "no vocab dir"); return false; }

    struct stat st;
    bool exists = stat(VOCAB_PATH, &st) == 0;
    if (exists && (uint64_t)st.st_size >= VOCAB_FILE_MAX) {
        ESP_LOGW(TAG, "vocab.txt reached %u bytes, refusing to append", (unsigned)VOCAB_FILE_MAX);
        return false;
    }
    // 刚被删掉的那一行：原样写回，等于撤销。这比重新抓上下文可靠——
    // 在生词本视图里长按查词时抓到的上下文本身就是垃圾。
    // The row that was just removed: write it back verbatim, i.e. undo. More
    // reliable than recapturing, since a long press inside the vocabulary view
    // would only ever capture garbage.
    if (undo_matches(lower)) {
        FILE* undo_file = fopen(VOCAB_PATH, "a");
        bool ok = false;
        if (undo_file) {
            bool head_ok = (exists && st.st_size > 0) ||
                           fprintf(undo_file, "%s\n", VOCAB_HEADER) >= 0;
            ok = head_ok && fprintf(undo_file, "%s\n", s_undo) >= 0;
            if (fclose(undo_file) != 0) ok = false;
        }
        if (!ok) { ESP_LOGW(TAG, "undo write failed"); return false; }
        s_undo[0] = 0;
        index_add(lower);
        return true;
    }
    // 索引没建起来就地扫一遍去重——这是用户点击才走的路，慢点没关系。
    // Without an index, scan for duplicates right here; this only runs on a tap.
    if (!s_words && file_contains(lower)) { index_add(lower); return true; }
    if (s_words) { size_t pos = 0; if (index_find(lower, &pos)) return true; }

    FILE* f = fopen(VOCAB_PATH, "a");
    if (!f) { ESP_LOGW(TAG, "open failed: %s", strerror(errno)); return false; }
    bool ok = true;
    if (!exists || st.st_size == 0) ok = fprintf(f, "%s\n", VOCAB_HEADER) >= 0;
    if (ok) append_row(f, lower, definition, s_sentence);
    if (fclose(f) != 0) ok = false;
    if (!ok) { ESP_LOGW(TAG, "write failed"); return false; }
    index_add(lower);
    return true;
}

bool vocab_log_remove(const char* word) {
    char lower[VOCAB_WORD_MAX];
    if (!word) return false;
    lower_copy(lower, sizeof(lower), word);
    if (!lower[0]) return false;
    if (!dir_ready()) return false;
    struct stat st;
    if (stat(VOCAB_PATH, &st) != 0) return false;
    s_undo[0] = 0;

    FILE* in = fopen(VOCAB_PATH, "r");
    if (!in) return false;
    FILE* out = fopen(VOCAB_TMP, "w");
    if (!out) { fclose(in); return false; }
    char line[VOCAB_LINE_MAX];
    char head[VOCAB_WORD_MAX];
    bool removed = false;
    bool ok = true;
    while (fgets(line, sizeof(line), in)) {
        if (line[0] == '#') { if (fputs(line, out) < 0) { ok = false; break; } continue; }
        char* tab = strchr(line, '\t');
        size_t n = tab ? (size_t)(tab - line) : strlen(line);
        if (n >= sizeof(head)) n = sizeof(head) - 1;
        memcpy(head, line, n);
        head[n] = 0;
        lower_copy(head, sizeof(head), head);
        if (head[0] && !strcmp(head, lower)) {
            removed = true;
            size_t raw = strlen(line);
            while (raw && (line[raw - 1] == '\n' || line[raw - 1] == '\r')) --raw;
            if (raw < sizeof(s_undo)) { memcpy(s_undo, line, raw); s_undo[raw] = 0; }
            continue;
        }
        if (fputs(line, out) < 0) { ok = false; break; }
    }
    fclose(in);
    if (fclose(out) != 0) ok = false;
    // 任何一步失败都清掉临时文件，绝不留下半份生词本。
    // Any failure discards the temp file so no half vocabulary book survives.
    if (!ok) {
        remove(VOCAB_TMP);
        s_undo[0] = 0;
        ESP_LOGW(TAG, "remove failed, original kept");
        return false;
    }
    if (remove(VOCAB_PATH) != 0 || rename(VOCAB_TMP, VOCAB_PATH) != 0) {
        remove(VOCAB_TMP);
        ESP_LOGW(TAG, "replace failed: %s", strerror(errno));
        return false;
    }
    if (s_words) { size_t pos = 0; if (index_find(lower, &pos)) index_drop_at(pos); }
    return removed;
}

// 判据三原则（改这个函数前请先读一遍）：
//  1. 比**内容**不比指针。绝不能写成 `path == VOCAB_VIEW` —— 那比的是地址，
//     只有恰好拿到同一个字符串常量时才成立，换个来源的路径就整个失效，
//     而且编译期不会有任何警告。
//  2. 只比**文件名**不比整条路径：视图被拷到别处（比如拷进 /sdcard/books
//     后再读）照样认得出来，不会被当成普通书去抓上下文。
//  3. 精确匹配，不可以用 strstr 之类的子串包含：`my_vocab_view.txt`、
//     `vocab_view.txt.bak` 都含这个名字但不是视图。误判的代价很实在——
//     app_book.c 的 DICT 分支一旦把真书认成视图，删一个生词就会重开虚拟书，
//     把用户正在读的那本顶掉。
// 大小写不敏感是因为 FAT 本身不区分大小写：`Vocab_View.txt` 打开的就是同一个文件。
// Matching rules:
//  1. Compare contents, never pointers: `path == VOCAB_VIEW` compares addresses and
//     silently breaks for any path that did not come from that one literal.
//  2. Compare the file name, not the whole path: a copy of the view opened from
//     elsewhere (say /sdcard/books) is still recognized.
//  3. Exact match, no substring test like strstr: "my_vocab_view.txt" contains the
//     name but is not the view, and a false positive makes app_book.c's DICT branch
//     reopen the vocabulary book over whatever the user was reading.
// Case-insensitive because FAT is case-insensitive: "Vocab_View.txt" is the same file.
static bool name_is_view(const char* path) {
    if (!path || !path[0]) return false;
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char* view = strrchr(VOCAB_VIEW, '/');
    view = view ? view + 1 : VOCAB_VIEW;
    return !strcasecmp(base, view);
}

bool vocab_log_is_view_path(const char* path) { return name_is_view(path); }
const char* vocab_log_view_path(void) { return VOCAB_VIEW; }

bool vocab_log_render(size_t* out_count) {
    if (out_count) *out_count = 0;
    // 还没有生词本：写一本只带提示的空书，让入口有东西可打开。
    // No book yet: write a hint-only view so the entry still has something to open.
    FILE* in = fopen(VOCAB_PATH, "r");
    if (!in) {
        FILE* empty_out = fopen(VOCAB_VIEW_TMP, "w");
        if (!empty_out) return false;
        bool ok = fprintf(empty_out, "# 我的生词本（共 0 词）\n\n"
                                     "还没有保存任何生词。\n"
                                     "读英文书时长按一个单词，在释义卡右上角点「保存」。\n") >= 0 &&
                  fclose(empty_out) == 0;
        if (!ok) { remove(VOCAB_VIEW_TMP); return false; }
        if (remove(VOCAB_VIEW) != 0 && errno != ENOENT) { remove(VOCAB_VIEW_TMP); return false; }
        return rename(VOCAB_VIEW_TMP, VOCAB_VIEW) == 0;
    }

    // 第一遍只数行，好在标题里写出总数。/ First pass counts rows for the header.
    char line[VOCAB_LINE_MAX];
    size_t total = 0;
    while (fgets(line, sizeof(line), in)) if (line[0] != '#') ++total;
    rewind(in);

    FILE* out = fopen(VOCAB_VIEW_TMP, "w");
    if (!out) { fclose(in); return false; }
    bool ok = fprintf(out, "# 我的生词本（共 %u 词）\n\n", (unsigned)total) >= 0;
    size_t index = 0, chapter = 0;
    while (ok && fgets(line, sizeof(line), in)) {
        if (line[0] == '#') continue;
        char* nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;
        char* field[5] = {0};
        char* p = line;
        int tabs = 0;
        for (int i = 0; i < 5; ++i) {
            field[i] = p;
            char* tab = strchr(p, '\t');
            if (!tab) break;
            *tab = 0;
            p = tab + 1;
            ++tabs;
        }
        // 没有 TAB 就不是词条（坏行/手工编辑残留），跳过而不是渲染成怪条目。
        // A row with no tab is not an entry (corruption or hand editing); skip it
        // instead of rendering something odd.
        if (!tabs || !field[0] || !field[0][0]) continue;
        ++index;
        // 章节标题必须写成「第N章」（中间无空格），book_txt.c 的 heading() 才认。
        // The heading must read "第N章" with no space or book_txt.c's heading() misses it.
        if ((index - 1) % VOCAB_CHUNK == 0 && fprintf(out, "第%u章\n\n", (unsigned)++chapter) < 0) {
            ok = false;
            break;
        }
        char phon[160];
        const char* body = NULL;
        split_phonetic(field[4] ? field[4] : "-", phon, sizeof(phon), &body);
        const char* when = field[1] && field[1][0] ? field[1] : "-";
        const char* book = field[2] && field[2][0] ? field[2] : "-";
        const char* sentence = field[3] && field[3][0] ? field[3] : NULL;
        if (fprintf(out, "%u. %s\n", (unsigned)index, field[0]) < 0) { ok = false; break; }
        if (phon[0]) {
            if (fprintf(out, "   %s %s\n", phon, body) < 0) { ok = false; break; }
        } else if (fprintf(out, "   %s\n", body) < 0) { ok = false; break; }
        if (sentence && strcmp(sentence, "-") &&
            fprintf(out, "   原句：%s\n", sentence) < 0) { ok = false; break; }
        if (fprintf(out, "   [保存时间：%s · %s]\n\n", when, book) < 0) { ok = false; break; }
    }
    fclose(in);
    if (fclose(out) != 0) ok = false;
    if (!ok) { remove(VOCAB_VIEW_TMP); return false; }
    if (remove(VOCAB_VIEW) != 0 && errno != ENOENT) { remove(VOCAB_VIEW_TMP); return false; }
    if (rename(VOCAB_VIEW_TMP, VOCAB_VIEW) != 0) { remove(VOCAB_VIEW_TMP); return false; }
    if (out_count) *out_count = index;
    return true;
}

/* ---------- AI 交互词攒批（gen26.5 M2 Step8） ---------- */

// 批惰性分配：查词路径上零开销，直到真有一个 AI 交互词入队。
// Lazily allocate the batch: zero cost on the lookup path until a word queues.
static bool batch_ensure(void) {
    if (s_batch) return true;
    s_batch = heap_caps_calloc(VOCAB_AI_BATCH_MAX, sizeof(ai_batch_entry_t),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_batch) { ESP_LOGW(TAG, "ai batch alloc failed"); return false; }
    s_batch_count = 0;
    return true;
}

static void batch_clear(void) {
    if (s_batch) { heap_caps_free(s_batch); s_batch = NULL; }
    s_batch_count = 0;
    s_batch_deadline_us = 0;
}

static ai_batch_entry_t* batch_find(const char* lower) {
    for (size_t i = 0; i < s_batch_count; ++i)
        if (!strcmp(s_batch[i].word, lower)) return &s_batch[i];
    return NULL;
}

void vocab_log_ai_queue(const char* word) {
    if (!s_autosave) return; // 开关关着：静默丢弃 / switch off: dropped silently
    if (!word) return;
    char lower[VOCAB_WORD_MAX];
    lower_copy(lower, sizeof(lower), word);
    if (!lower[0]) return;
    if (s_batch && batch_find(lower)) return; // 批内已有，首次为准 / first wins
    if (s_batch_count >= VOCAB_AI_BATCH_MAX) {
        ESP_LOGW(TAG, "ai batch full (%u), dropping \"%s\"", (unsigned)VOCAB_AI_BATCH_MAX, lower);
        return;
    }
    if (!batch_ensure()) return; // 分配失败：静默，不阻塞查词 / silent, never blocks lookup
    ai_batch_entry_t* e = &s_batch[s_batch_count];
    snprintf(e->word, sizeof(e->word), "%s", lower);
    e->def[0] = 0;
    // 原句必须快照：之后每点一个词都会覆盖 s_sentence，落盘时早不是这一句了。
    // Snapshot the sentence: every later tap overwrites s_sentence, so by flush
    // time it would no longer belong to this word.
    snprintf(e->sentence, sizeof(e->sentence), "%s", s_sentence);
    ++s_batch_count;
    // deadline 取**首次**入队时刻：连续点词只会并入同批，不会把落盘一推再推。
    // The deadline is the FIRST queue: later taps join the same batch instead of
    // pushing the write back forever.
    if (!s_batch_deadline_us)
        s_batch_deadline_us = esp_timer_get_time() + VOCAB_AI_BATCH_DELAY_US;
}

void vocab_log_ai_set_def(const char* word, const char* def) {
    if (!s_batch || !s_batch_count || !word || !def) return;
    char lower[VOCAB_WORD_MAX];
    lower_copy(lower, sizeof(lower), word);
    ai_batch_entry_t* e = batch_find(lower);
    // 批里没有 = 已落盘或从未入批。释义走预取缓存，下次手动保存会带上。
    // Not in the batch = already on the card or never queued; the definition
    // stays in the prefetch cache for the next manual save.
    if (!e) return;
    snprintf(e->def, sizeof(e->def), "%s", def);
}

void vocab_log_ai_flush(void) {
    if (!s_batch_count) { s_batch_deadline_us = 0; return; }
    if (!s_autosave) { batch_clear(); return; } // 关开关：积压丢弃，不落 / off: drop, don't log
    if (esp_timer_get_time() < s_batch_deadline_us) return; // 5s 窗口未到 / not yet
    if (!dir_ready()) { ESP_LOGW(TAG, "no vocab dir, ai batch kept"); return; }
    struct stat st;
    bool exists = stat(VOCAB_PATH, &st) == 0;
    if (exists && (uint64_t)st.st_size >= VOCAB_FILE_MAX) {
        ESP_LOGW(TAG, "vocab.txt reached %u bytes, dropping ai batch", (unsigned)VOCAB_FILE_MAX);
        batch_clear();
        return;
    }
    FILE* f = fopen(VOCAB_PATH, "a");
    if (!f) { ESP_LOGW(TAG, "ai batch open failed: %s", strerror(errno)); return; } // 保留重试
    bool ok = true;
    if (!exists || st.st_size == 0) ok = fprintf(f, "%s\n", VOCAB_HEADER) >= 0;
    // 一次 open 循环写全部——这就是「攒批」省下来的写次数。去重与手动保存同口径：
    // 索引优先，没索引扫文件。半途失败时已写出的行靠去重挡住，不会重复。
    // One open, every row in a loop - the writes the batch saves. De-duplication
    // matches the manual save (index first, file scan otherwise). Rows written
    // before a mid-way failure are de-duplicated on retry, never doubled.
    for (size_t i = 0; ok && i < s_batch_count; ++i) {
        const char* lower = s_batch[i].word;
        if (s_words) {
            size_t pos = 0;
            if (index_find(lower, &pos)) continue;
        } else if (file_contains(lower)) {
            index_add(lower);
            continue;
        }
        append_row(f, lower, s_batch[i].def, s_batch[i].sentence);
        index_add(lower);
    }
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        // 保留批次等下次 tick 重试：防磨损的合批语义不变，词也不丢。
        // Keep the batch for the next tick: batching semantics intact, no word lost.
        ESP_LOGW(TAG, "ai batch write failed, kept for retry");
        return;
    }
    batch_clear();
}

void vocab_log_autosave_init(void) {
    s_autosave = true; // 缺省开：读不到、读失败、怪值都按开 / default on
    nvs_handle_t h;
    if (nvs_open(VOCAB_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v = 1;
    // 只有明确读到 0 才是关。/ Only an explicit 0 turns it off.
    if (nvs_get_u8(h, VOCAB_NVS_KEY_AUTOSAVE, &v) == ESP_OK && v == 0) s_autosave = false;
    nvs_close(h);
}

bool vocab_log_autosave_enabled(void) { return s_autosave; }

void vocab_log_warm(void) {
    if (s_words) return;
    (void)index_load();
}

void vocab_log_invalidate(void) {
    if (!s_words) {
        s_count = 0;
        // AI 批照清：书上下文失效语义不因「索引没建起来」而变（≤5s 的批，静默）。
        // Drop the AI batch all the same: "the book context is gone" does not
        // change just because the index failed to build (at most 5s, silently).
        batch_clear();
        return;
    }
    for (size_t i = 0; i < s_count; ++i) heap_caps_free(s_words[i]);
    heap_caps_free(s_words);
    s_words = NULL;
    s_count = 0;
    s_book[0] = 0;
    s_sentence[0] = 0;
    s_undo[0] = 0; // 卡都没了，撤销缓冲指向的行也一起没了 / the card is gone, so is the stashed row
    batch_clear();
}
