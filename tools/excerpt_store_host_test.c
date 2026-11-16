/*
 * 中文：S26-1 摘抄落盘宿主测试。纪律逐一取证，不做「看起来通过」。
 * English: S26-1 host tests for excerpt storage; each discipline gets real evidence.
 *
 * 覆盖 / Coverage:
 *   1. 拒新不丢旧（队列满 + 11 连摘）/ refuse-new with 11 straight excerpts
 *   2. PSRAM 申请失败 = 拒新 + 按钮可正确 EXIT / allocation failure refuses
 *   3. UTF-8 四组边界（中文 3B、emoji 4B、混排、正好卡边界）/ boundary cases
 *   4. 目录缺失 → 首次冲账靠 mkdir 自愈 / missing dir self-heals via mkdir
 *   5. 半行分支：短写 / flush 失败 → 补 `\n` 收口 + 弃 + 计数（26.9.29 四分法第二档）
 *      Half-line branch: a short write or a failed flush terminates the fragment, drops
 *      the entry and counts it (branch two of the 26.9.29 four-way rule).
 *   6. 挂载让行窗口（5 s 内不落）/ mount yield window
 *   6b. S26-4 对抗：插卡 + 9 条积压 + 立即回书架 / mount with a nine-deep backlog
 *   6c. S26-4 两拍时序：存后立翻 / 落盘后翻，回执都不许跑在 fsync 前 / two beats
 *   7. 单家务片至多落一条 / at most one entry per slot
 *   8. 无内存泄漏（入队/落盘配平）/ no leaks
 *   9. 单行恒 ≤1,024B（长书名 + 长正文）/ the line always fits
 *  10. 26.9.29 新增：写入前失败（mkdir / open）→ 零字节 → 留队、不亮勾、**必须留痕**
 *      New in 26.9.29: a failure before the first byte keeps the entry, never lights the
 *      tick, and MUST leave a log line (26.9.28 printed nothing at all).
 *  11. 26.9.29 新增：持久性三档（fsync 失败×close 成功 / fsync+close 都失败 / close 单独失败）
 *      New in 26.9.29: the three durability branches.
 *
 * 桩纪律（26.9.29）：**每个参数要么断言、要么显式声明不关心**。26.9.28 的桩把 path
 * `(void)` 掉，而 path 正是唯一错的东西；那种「不看不报」的桩测出来的绿没有意义。
 * Stub discipline: every argument is either asserted or explicitly ignored. The 26.9.28
 * stub voided `path`, and the path was the only thing that was wrong.
 *
 * 追加模式由结构牙 B38 咬住（`"ab"` 在生产的宏里，宿主看不到），这里不重复造一个假证据。
 * Append mode is held by tooth B38 (the "ab" lives in the production macro, invisible to
 * the host); this file does not fabricate a second-hand proof of it.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "excerpt_store.h"
#include "esp_heap_caps.h"

// 常量冻结（防靶点漂移：用例若用宏写循环，宏一改用例跟着漂，变异就看不见）。
// Frozen constants: writing the loop with the macro makes the test drift with the mutation.
_Static_assert(EXCERPT_QUEUE_MAX == 10u, "S26-1 frozen: the queue holds ten excerpts");
_Static_assert(EXCERPT_TITLE_MAX == 160u, "S26-1 frozen: the title caps at 160 B");
_Static_assert(EXCERPT_TEXT_MAX == 700u, "S26-1 frozen: the text caps at 700 B");
_Static_assert(EXCERPT_LINE_MAX == 1024u, "S26-1 frozen: one line caps at 1,024 B");
// E-1 冻结：档数写死在用例里，改档数必须同时改这三处断言（M101 靶点漂移的反面）。
// E-1 frozen: the step count is spelled out here so adding a step cannot pass silently.
_Static_assert(EXCERPT_BACKOFF_STEPS == 3u, "E-1 frozen: three fixed backoff steps");

// ---- 桩状态 / stub state ----------------------------------------------------
bool test_dir_missing = true; // 起始无目录：逼出 mkdir 先手 / start without the dir
bool test_mkdir_fail;
bool test_open_fail;
bool test_short_write;
bool test_flush_fail;
bool test_sync_fail;
bool test_close_fail;
bool test_psram_fail;
int test_mkdir_calls, test_write_calls, test_flush_calls, test_sync_calls, test_close_calls;
int test_psram_live;
int test_log_w, test_log_e, test_log_i;
int test_bb_notes;   ///< 黑匣子记了几条 / black-box notes recorded
char test_fs_buf[16384];
size_t test_fs_len;

static int test_file_tag; ///< 假的 FILE 句柄 / a fake FILE handle

/// 路径口径：**必须**是 VFS 挂载点。裸 FatFs 的 `"0:"` 在这里就该当场炸。
/// Path vocabulary: it must be the VFS mount point. A bare-FatFs "0:" dies right here.
static void expect_vfs_path(const char *p) {
    assert(p != NULL);
    assert(strncmp(p, "/sdcard/", 8) == 0 &&
           "摘抄只许走 VFS 挂载点 / excerpts may only go through the VFS mount point");
}

int ex_mkdir(const char *path) {
    expect_vfs_path(path);
    ++test_mkdir_calls;
    if (test_mkdir_fail) { errno = EACCES; return -1; }
    if (test_dir_missing) { test_dir_missing = false; return 0; }
    errno = EEXIST; // 目录已存在：每片都先 mkdir 的常态 / a re-mkdir is the normal case
    return -1;
}

FILE *ex_open_append(const char *path) {
    expect_vfs_path(path);
    if (test_dir_missing) { errno = ENOENT; return NULL; }
    if (test_open_fail) { errno = EACCES; return NULL; }
    return (FILE *)&test_file_tag;
}

size_t ex_write(const void *buf, size_t len, FILE *f) {
    assert(f == (FILE *)&test_file_tag);
    ++test_write_calls;
    size_t n = len;
    if (test_short_write) { n = len > 3u ? 3u : len; errno = ENOSPC; } // 半行 / half a line
    if (test_fs_len + n < sizeof(test_fs_buf)) {
        memcpy(test_fs_buf + test_fs_len, buf, n);
        test_fs_len += n;
    }
    return n;
}

int ex_putc(int c, FILE *f) {
    assert(f == (FILE *)&test_file_tag);
    if (test_fs_len + 1u < sizeof(test_fs_buf)) test_fs_buf[test_fs_len++] = (char)c;
    return (unsigned char)c;
}

int ex_flush(FILE *f) {
    assert(f == (FILE *)&test_file_tag);
    ++test_flush_calls;
    if (test_flush_fail) { errno = EIO; return EOF; }
    return 0;
}

int ex_sync(FILE *f) {
    assert(f == (FILE *)&test_file_tag);
    ++test_sync_calls;
    if (test_sync_fail) { errno = EIO; return -1; }
    return 0;
}

int ex_close(FILE *f) {
    assert(f == (FILE *)&test_file_tag);
    ++test_close_calls;
    if (test_close_fail) { errno = EIO; return EOF; }
    return 0;
}

/// 整行 UTF-8 合法性：不许有孤立续字节、不许有被砍断的多字节序列。
/// Whole-line UTF-8 validity: no orphan continuation bytes, no truncated sequences.
static bool valid_utf8(const char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        const unsigned char c = (unsigned char)s[i];
        unsigned need;
        if (c < 0x80u) { ++i; continue; }
        else if ((c & 0xE0u) == 0xC0u) need = 2;
        else if ((c & 0xF0u) == 0xE0u) need = 3;
        else if ((c & 0xF8u) == 0xF0u) need = 4;
        else return false; // 续字节开头或非法首字节 / a stray continuation or a bad lead byte
        if (i + need > n) return false; // 被砍断 / truncated
        for (unsigned k = 1; k < need; ++k) {
            if (((unsigned char)s[i + k] & 0xC0u) != 0x80u) return false;
        }
        i += need;
    }
    return true;
}

// ---- 用例 / cases ------------------------------------------------------------
static void reset_all(void) {
    excerpt_store_reset();
    test_dir_missing = true;
    test_mkdir_fail = test_open_fail = test_short_write = false;
    test_flush_fail = test_sync_fail = test_close_fail = test_psram_fail = false;
    test_mkdir_calls = test_write_calls = test_flush_calls = test_sync_calls = test_close_calls = 0;
    test_log_w = test_log_e = test_log_i = 0;
    test_bb_notes = 0;
    test_fs_len = 0;
    memset(test_fs_buf, 0, sizeof(test_fs_buf));
}

static void case_refuse_new(void) {
    reset_all();
    for (unsigned i = 0; i < 10u; ++i) { // 字面 10，不用宏 / literal ten, not the macro
        assert(excerpt_store_enqueue("书", 1, 1000u + i, "摘抄", NULL) == EXCERPT_OK);
    }
    assert(excerpt_store_pending() == 10u);
    // 第 11 条：拒新 / the eleventh is refused
    assert(excerpt_store_enqueue("书", 2, 2000u, "第十一条", NULL) == EXCERPT_FULL);
    assert(excerpt_store_pending() == 10u); // 旧的十条一条不少 / nothing dropped
    assert(!excerpt_store_take_saved());   // 没落盘就不许亮勾 / no fake tick
    // 冲账后第十一条确实不在卡上 / the refused one never reaches the card
    for (unsigned i = 0; i < 10u; ++i) assert(excerpt_store_tick(1000u) == EXCERPT_FLUSH_OK);
    assert(strstr(test_fs_buf, "第十一条") == NULL);
    assert(strstr(test_fs_buf, "#") == NULL);            // 有时间戳时不该出现序号 / no seq when time is known
    assert(test_psram_live == 0);                        // 无泄漏 / no leak
    printf("  case_refuse_new ok\n");
}

static void case_no_mem(void) {
    reset_all();
    test_psram_fail = true;
    assert(excerpt_store_enqueue("书", 1, 1u, "内容", NULL) == EXCERPT_NO_MEM);
    assert(excerpt_store_pending() == 0);
    assert(!excerpt_store_take_saved());
    test_psram_fail = false;
    assert(excerpt_store_enqueue("书", 1, 1u, "内容", NULL) == EXCERPT_OK);
    printf("  case_no_mem ok\n");
}

static void case_utf8_boundaries(void) {
    reset_all();
    // 3 字节汉字卡在边界 / a 3-byte CJK char straddling the cap
    char big[4096];
    for (size_t i = 0; i + 3 < sizeof(big) - 1; i += 3) memcpy(big + i, "中", 3);
    big[sizeof(big) - 1] = 0;
    bool cut = false;
    assert(excerpt_store_enqueue("书", 1, 0u, big, &cut) == EXCERPT_OK);
    assert(cut);
    assert(excerpt_store_tick(0u) == EXCERPT_FLUSH_OK);
    const char *line = test_fs_buf;
    assert(strstr(line, "[截]"));
    // 正文片段里不得出现被切开的续字节：截断标记前必须是完整字符
    // No orphan continuation byte before the marker.
    const char *mark = strstr(test_fs_buf, "[截]");
    assert(mark && mark > test_fs_buf);
    const char *nl1 = strchr(test_fs_buf, '\n');
    assert(nl1 && valid_utf8(test_fs_buf, (size_t)(nl1 - test_fs_buf))); // 整行合法 / whole line valid

    // 4 字节 emoji / 4-byte emoji
    reset_all();
    memset(big, 0, sizeof(big));
    for (size_t i = 0; i + 4 < sizeof(big) - 1; i += 4) memcpy(big + i, "\xF0\x9F\x98\x80", 4);
    cut = false;
    assert(excerpt_store_enqueue("书", 2, 0u, big, &cut) == EXCERPT_OK);
    assert(cut);
    assert(excerpt_store_tick(0u) == EXCERPT_FLUSH_OK);
    mark = strstr(test_fs_buf, "[截]");
    assert(mark);
    const char *nl2 = strchr(test_fs_buf, '\n');
    assert(nl2 && valid_utf8(test_fs_buf, (size_t)(nl2 - test_fs_buf)));

    // 中英混排 + 正好等于上限 / mixed script, exactly at the cap
    reset_all();
    char exact[EXCERPT_TEXT_MAX + 1];
    memset(exact, 'a', sizeof(exact) - 1);
    exact[sizeof(exact) - 1] = 0;
    cut = false;
    assert(excerpt_store_enqueue("书", 3, 0u, exact, &cut) == EXCERPT_OK);
    assert(!cut); // 正好 699 + 结尾 0：不截 / exactly at the cap: no truncation
    assert(excerpt_store_tick(0u) == EXCERPT_FLUSH_OK);
    assert(strstr(test_fs_buf, "[截]") == NULL);

    // 混排：末尾放一个汉字，确保不把汉字切成两半 / mixed with a CJK char at the tail
    reset_all();
    memset(exact, 'b', sizeof(exact) - 4);
    memcpy(exact + sizeof(exact) - 4, "汉", 3);
    exact[sizeof(exact) - 1] = 0;
    cut = false;
    assert(excerpt_store_enqueue("书", 4, 0u, exact, &cut) == EXCERPT_OK);
    assert(excerpt_store_tick(0u) == EXCERPT_FLUSH_OK);
    const char *nl3 = strchr(test_fs_buf, '\n');
    assert(nl3 && valid_utf8(test_fs_buf, (size_t)(nl3 - test_fs_buf)));
    printf("  case_utf8_boundaries ok\n");
}

static void case_missing_dir(void) {
    reset_all();
    assert(excerpt_store_enqueue("书", 1, 0u, "内容", NULL) == EXCERPT_OK);
    assert(excerpt_store_tick(0u) == EXCERPT_FLUSH_OK); // 首次即自愈 / heals on the first try
    assert(test_mkdir_calls >= 1);                      // mkdir 先手 / mkdir came first
    assert(excerpt_store_pending() == 0);
    assert(excerpt_store_take_saved());

    // 目录被删后重试仍从 mkdir 开始 / after the dir vanishes, the retry mkdirs again
    test_dir_missing = true;
    const int before = test_mkdir_calls;
    assert(excerpt_store_enqueue("书", 2, 0u, "再来", NULL) == EXCERPT_OK);
    assert(excerpt_store_tick(0u) == EXCERPT_FLUSH_OK);
    assert(test_mkdir_calls > before);
    printf("  case_missing_dir ok\n");
}

// 26.9.29 四分法第二档：卡上已有半行 → 补 `\n` 收口 + 弃 + 计数 + 亮红。
// Branch two: half a line is on the card. Terminate it, drop the entry, count it, go red.
static void case_half_line(void) {
    reset_all();
    const unsigned dropped0 = excerpt_store_dropped();

    // 短写 / a short write
    assert(excerpt_store_enqueue("书", 1, 0u, "内容甲", NULL) == EXCERPT_OK);
    test_short_write = true;
    assert(excerpt_store_tick(0u) == EXCERPT_FLUSH_DROPPED);
    assert(excerpt_store_pending() == 0);         // 弃：不留在队首 / dropped, not kept
    assert(!excerpt_store_take_saved());          // 不许亮勾 / never a fake tick
    assert(excerpt_store_dropped() == dropped0 + 1);
    assert(test_fs_len > 0);
    assert(test_fs_buf[test_fs_len - 1] == '\n'); // 残行被换行收口 / the fragment is terminated
    assert(excerpt_store_dropped() == 1u || dropped0 + 1 == excerpt_store_dropped());
    test_short_write = false;

    // flush 失败（字节已交给 libc，但没推下去——同样按半行处置）
    // A failed flush: same branch, because the card may hold a fragment.
    const size_t mark = test_fs_len;
    assert(excerpt_store_enqueue("书", 2, 0u, "内容乙", NULL) == EXCERPT_OK);
    test_flush_fail = true;
    assert(excerpt_store_tick(0u) == EXCERPT_FLUSH_DROPPED);
    assert(excerpt_store_pending() == 0);
    assert(excerpt_store_dropped() == dropped0 + 2);
    assert(test_fs_buf[test_fs_len - 1] == '\n');
    assert(test_fs_len > mark); // 换行确实写下去了 / the terminator really went out
    assert(!excerpt_store_take_saved());

    // 弃完之后队列还能继续干活（不是「死一次就瘫」）
    // After a drop the queue keeps working - one bad flush must not poison the module.
    test_flush_fail = false;
    assert(excerpt_store_enqueue("书", 3, 0u, "内容丙", NULL) == EXCERPT_OK);
    assert(excerpt_store_tick(0u) == EXCERPT_FLUSH_OK);
    assert(excerpt_store_take_saved());
    assert(test_psram_live == 0); // 弃的那两条不许泄漏 / dropped entries are freed too
    printf("  case_half_line ok\n");
}

static void case_mount_yield(void) {
    reset_all();
    assert(excerpt_store_enqueue("书", 1, 0u, "内容", NULL) == EXCERPT_OK);
    excerpt_store_mount_yield(1u, 10000u);
    assert(excerpt_store_tick(10000u) == EXCERPT_FLUSH_YIELD); // 窗口内不落 / inside the window
    assert(excerpt_store_tick(14999u) == EXCERPT_FLUSH_YIELD);
    assert(excerpt_store_pending() == 1);
    assert(excerpt_store_tick(15000u) == EXCERPT_FLUSH_OK);    // 窗口结束 / window over

    // E-2 幂等：同一代号再来一次**不许**重开窗口。调用点在 poll_media 与家务片各一处，
    // 而家务片每片都跑——没有幂等，窗口会被每片往后推 5 s，队列永远等不到能落盘的那拍。
    // 这正是 `if (ready)` 那个坑的等价形态，只是搬进了被调方，所以必须在这里咬住。
    // E-2 idempotence: the same epoch must NOT re-arm. Both call sites fire often; without
    // idempotence the window would be pushed forward every turn - the `if (ready)` trap,
    // relocated into the callee.
    assert(excerpt_store_enqueue("书", 2, 0u, "第二条", NULL) == EXCERPT_OK);
    excerpt_store_mount_yield(1u, 15000u);   // 旧代号：应被忽略 / stale epoch, must be ignored
    assert(excerpt_store_tick(15000u) == EXCERPT_FLUSH_OK);   // 立刻落 = 窗口没被重开
    assert(excerpt_store_enqueue("书", 3, 0u, "第三条", NULL) == EXCERPT_OK);
    excerpt_store_mount_yield(2u, 16000u);   // 新代号：开窗 / new epoch, arms
    assert(excerpt_store_tick(16000u) == EXCERPT_FLUSH_YIELD);
    assert(excerpt_store_tick(20999u) == EXCERPT_FLUSH_YIELD);
    assert(excerpt_store_tick(21000u) == EXCERPT_FLUSH_OK);
    printf("  case_mount_yield ok\n");
}

// S26-4 对抗用例：**插卡 + 9 条积压 + 立即回书架**。
// 剪卡期间攒下 9 条，卡插回来那一拍挂载成功 → 让行 5 s。用户不等，按返回回书架；
// 家务片（on_tick）与视图无关，照跑。窗口内一个字节都不许写（书架扫描正在读卡），
// 窗口一过按「一片一条」冲完，九行按序、无重复、无丢失。
// S26-4 adversarial case: card inserted, nine entries backlogged, user leaves for the shelf
// immediately. Inside the window not one byte is written; afterwards the queue drains one
// entry per slot, in order, without duplicates or losses.
static void case_mount_backlog(void) {
    reset_all();
    for (unsigned i = 1; i <= 9; ++i) { // 字面 9，不用宏 / literal nine, not the macro
        char t[32];
        snprintf(t, sizeof(t), "摘抄%u", i);
        assert(excerpt_store_enqueue("书", i, 0u, t, NULL) == EXCERPT_OK);
    }
    assert(excerpt_store_pending() == 9);

    excerpt_store_mount_yield(1u, 1000u); // 插卡 → 让行到 6000 / mounted at 1000, armed to 6000
    for (uint32_t t = 1000u; t < 6000u; t += 250u)
        assert(excerpt_store_tick(t) == EXCERPT_FLUSH_YIELD);
    assert(test_write_calls == 0); // 窗口内一个字节都没写 / not a byte inside the window
    assert(test_fs_len == 0);
    assert(excerpt_store_pending() == 9);
    assert(!excerpt_store_take_saved()); // 没落盘就不许亮勾 / no receipt yet

    for (unsigned i = 0; i < 9; ++i) {
        assert(excerpt_store_tick(6000u) == EXCERPT_FLUSH_OK); // 一片一条 / one per slot
        assert(excerpt_store_pending() == 9u - i - 1u);
    }
    assert(excerpt_store_tick(6000u) == EXCERPT_FLUSH_IDLE);

    // 九行都在，且严格按入队顺序（用换行收口的锚串，避开「摘抄1」是「摘抄10」前缀）。
    // All nine present, strictly in order (anchors end with the newline so 摘抄1 cannot
    // match 摘抄10).
    const char *p = test_fs_buf;
    for (unsigned i = 1; i <= 9; ++i) {
        char want[32];
        snprintf(want, sizeof(want), "摘抄%u\n", i);
        const char *hit = strstr(p, want);
        assert(hit != NULL);
        p = hit + strlen(want);
    }
    assert(test_psram_live == 0); // 九条全部出队，无泄漏 / all nine left the queue, no leak

    // 拔了又插：窗口重新起算——「第二张卡不算新挂载」是这类窗口最容易写漏的一句。
    // Pull and reinsert: the window restarts, because a second mount is still a mount.
    assert(excerpt_store_enqueue("书", 10, 0u, "再插", NULL) == EXCERPT_OK);
    excerpt_store_mount_yield(2u, 9000u);
    assert(excerpt_store_tick(9000u) == EXCERPT_FLUSH_YIELD);
    assert(excerpt_store_tick(13999u) == EXCERPT_FLUSH_YIELD);
    assert(excerpt_store_tick(14000u) == EXCERPT_FLUSH_OK);

    // 队列空时插卡：让行窗口不制造「空转」的刷新请求。
    // Mounting with an empty queue must not manufacture a redraw request.
    excerpt_store_mount_yield(3u, 20000u);
    assert(excerpt_store_tick(20000u) == EXCERPT_FLUSH_IDLE);
    printf("  case_mount_backlog ok\n");
}

// S26-4：两拍时序。「存后立翻」与「落盘后翻」都不许让回执跑在 fsync 前面。
// 页面那一半（回执只在本页显示）是 UI 静态状态，由结构牙 B36 与真机两步兜住。
// S26-4: the two beats. Neither order may put the receipt ahead of fsync; the page half of
// the contract lives in static UI state and is held by tooth B36 plus the on-device steps.
static void case_two_beats(void) {
    // 第一拍：点击即入队，用户立刻翻页（页已变），第二拍还没到。
    reset_all();
    assert(excerpt_store_enqueue("书", 1, 0u, "甲", NULL) == EXCERPT_OK);
    assert(!excerpt_store_take_saved());  // 卡上还没有 → 不许有「已存 ✓」
    assert(excerpt_store_pending() == 1); // 但「待落盘 1」是真的
    // 第二拍：家务片落盘（fsync 回 0），回执这才有资格出现。
    assert(excerpt_store_tick(0u) == EXCERPT_FLUSH_OK);
    assert(excerpt_store_take_saved());
    assert(!excerpt_store_take_saved()); // 一次消费即清，不许挂到下一拍
    assert(excerpt_store_pending() == 0);

    // 反序：先落盘后翻页——回执已在本页亮过，翻页只按页作废；落盘事实不受影响。
    // 三条积压 + 一片一条：每个「已存 ✓」只对应刚刚写下去的那一条。
    // Reverse order: the receipt was shown on its page, flipping only expires it by page.
    // Three queued, one per slot: each "saved" corresponds to exactly the entry just written.
    reset_all();
    for (unsigned i = 0; i < 3; ++i) assert(excerpt_store_enqueue("书", i, 0u, "乙", NULL) == EXCERPT_OK);
    for (unsigned i = 0; i < 3; ++i) {
        assert(excerpt_store_pending() == 3u - i);
        assert(!excerpt_store_take_saved()); // 落盘之前一次都不许亮
        assert(excerpt_store_tick(0u) == EXCERPT_FLUSH_OK);
        assert(excerpt_store_take_saved());  // 落盘之后每一次都能亮
    }
    assert(excerpt_store_tick(0u) == EXCERPT_FLUSH_IDLE); // 空队列不产生第二拍
    printf("  case_two_beats ok\n");
}

static void case_one_per_slot(void) {
    reset_all();
    for (unsigned i = 0; i < 3; ++i) assert(excerpt_store_enqueue("书", i, 0u, "内容", NULL) == EXCERPT_OK);
    assert(excerpt_store_tick(0u) == EXCERPT_FLUSH_OK);
    assert(excerpt_store_pending() == 2); // 一个片只落一条 / one per slot, no more
    assert(excerpt_store_tick(0u) == EXCERPT_FLUSH_OK);
    assert(excerpt_store_pending() == 1);
    assert(excerpt_store_tick(0u) == EXCERPT_FLUSH_OK);
    assert(excerpt_store_tick(0u) == EXCERPT_FLUSH_IDLE);
    assert(test_psram_live == 0);
    printf("  case_one_per_slot ok\n");
}

static void case_line_budget(void) {
    reset_all();
    char title[1024], body[4096];
    memset(title, 'T', sizeof(title) - 1); title[sizeof(title) - 1] = 0;
    memset(body, 'x', sizeof(body) - 1); body[sizeof(body) - 1] = 0;
    bool cut = false;
    assert(excerpt_store_enqueue(title, 4294967295u, 4294967295u, body, &cut) == EXCERPT_OK);
    assert(cut);
    assert(excerpt_store_tick(0u) == EXCERPT_FLUSH_OK);
    const char *nl = strchr(test_fs_buf, '\n');
    assert(nl);
    const size_t line_len = (size_t)(nl - test_fs_buf);
    assert(line_len <= EXCERPT_LINE_MAX - 1); // 单行恒在上限内 / one line never exceeds the cap
    assert(valid_utf8(test_fs_buf, line_len)); // 截断后仍是合法 UTF-8 / still valid after truncation
    printf("  case_line_budget ok (line=%zu B)\n", line_len);
}

static void case_seq_and_fields(void) {
    reset_all();
    assert(excerpt_store_enqueue("书名", 7, 0u, "甲", NULL) == EXCERPT_OK);
    assert(excerpt_store_enqueue("书名", 8, 0u, "乙", NULL) == EXCERPT_OK);
    assert(excerpt_store_tick(0u) == EXCERPT_FLUSH_OK);
    assert(excerpt_store_tick(0u) == EXCERPT_FLUSH_OK);
    // 开机序号是当次开机单调累加（跨用例也累积），所以断言「连续 +1」而不是绝对值。
    // The per-boot sequence only ever grows, so assert +1 rather than an absolute value.
    const char *p1 = strstr(test_fs_buf, "[书名]\tp=7\t#");
    const char *p2 = strstr(test_fs_buf, "[书名]\tp=8\t#");
    assert(p1 && p2);
    assert(strstr(p1, "\t甲\n") && strstr(p2, "\t乙\n"));
    unsigned a = 0, b = 0;
    assert(sscanf(p1 + strlen("[书名]\tp=7\t#"), "%u", &a) == 1);
    assert(sscanf(p2 + strlen("[书名]\tp=8\t#"), "%u", &b) == 1);
    assert(b == a + 1); // 开机内递增 / per-boot sequence
    printf("  case_seq_and_fields ok\n");
}

// 26.9.29 新增（第 12 组）：写入前失败 = 零字节 → 留队；且**必须留痕**。
// 26.9.28 的现场连「open failed」都没打印出来：失败而不留痕，是这次事故最贵的部分。
// New (group 12): a failure before the first byte keeps the entry - and must leave a trace.
static void case_pre_write_failure(void) {
    reset_all();
    assert(excerpt_store_enqueue("书", 1, 0u, "内容", NULL) == EXCERPT_OK);
    test_open_fail = true;
    assert(excerpt_store_tick(0u) == EXCERPT_FLUSH_FAILED);
    assert(excerpt_store_pending() == 1);   // 留队 / kept
    assert(!excerpt_store_take_saved());    // 不亮勾 / no tick
    assert(test_log_w >= 1);                // 留痕 / a trace
    test_open_fail = false;
    // E-1：同一刻再 tick 必须被退避挡住——「每片都重试」正是 389 次风暴的成因。
    // E-1: an immediate retry must be held off by the backoff - "retry every slot" IS the storm.
    assert(excerpt_store_tick(0u) == EXCERPT_FLUSH_YIELD);
    assert(excerpt_store_tick(49u) == EXCERPT_FLUSH_YIELD);
    assert(excerpt_store_tick(50u) == EXCERPT_FLUSH_OK);
    assert(excerpt_store_pending() == 0);
    assert(excerpt_store_take_saved());

    // mkdir 失败（非 EEXIST）同样零字节 → 留队，且一个字节都不许写
    reset_all();
    test_dir_missing = false;               // 让 mkdir 走到失败分支 / force the failure branch
    assert(excerpt_store_enqueue("书", 2, 0u, "内容", NULL) == EXCERPT_OK);
    test_mkdir_fail = true;
    assert(excerpt_store_tick(0u) == EXCERPT_FLUSH_FAILED);
    assert(excerpt_store_pending() == 1);
    assert(test_write_calls == 0);          // 零字节写入 / not a byte written
    assert(test_log_w >= 1);
    test_mkdir_fail = false;
    assert(excerpt_store_tick(0u) == EXCERPT_FLUSH_YIELD); // E-1 退避 / backoff holds
    assert(excerpt_store_tick(50u) == EXCERPT_FLUSH_OK);
    printf("  case_pre_write_failure ok\n");
}

// 26.9.29 新增（第 13 组）：持久性三档。判据只有一条不变式——
// **留队的唯一条件是卡上可见内容不含本行**。
// New (group 13): the three durability branches, all derived from one invariant - the entry
// is kept only while the card's visible content does not contain this line.
static void case_durability_branches(void) {
    // ① fsync 失败 + close 也失败 → 行不可见 → 留队（拔卡主场景）
    reset_all();
    assert(excerpt_store_enqueue("书", 1, 0u, "甲", NULL) == EXCERPT_OK);
    test_sync_fail = true;
    test_close_fail = true;
    assert(excerpt_store_tick(0u) == EXCERPT_FLUSH_FAILED);
    assert(excerpt_store_pending() == 1);
    assert(!excerpt_store_take_saved());
    test_sync_fail = false;
    test_close_fail = false;
    assert(excerpt_store_tick(0u) == EXCERPT_FLUSH_YIELD); // E-1 退避 / backoff holds
    assert(excerpt_store_tick(50u) == EXCERPT_FLUSH_OK);
    assert(excerpt_store_take_saved());

    // ② fsync 失败但 close 内部的 f_sync 成功 → 行已持久 → 算成功（留队就会写重复）
    reset_all();
    assert(excerpt_store_enqueue("书", 2, 0u, "乙", NULL) == EXCERPT_OK);
    test_sync_fail = true;
    assert(excerpt_store_tick(0u) == EXCERPT_FLUSH_OK);
    assert(excerpt_store_pending() == 0);
    assert(excerpt_store_take_saved());
    assert(test_log_w >= 1);                 // 异常必须留痕 / the anomaly is logged
    test_sync_fail = false;

    // ③ close 单独失败（fsync 已过）→ 数据已持久 → 算成功，只记日志
    reset_all();
    assert(excerpt_store_enqueue("书", 3, 0u, "丙", NULL) == EXCERPT_OK);
    test_close_fail = true;
    assert(excerpt_store_tick(0u) == EXCERPT_FLUSH_OK);
    assert(excerpt_store_pending() == 0);
    assert(excerpt_store_take_saved());
    assert(test_log_w >= 1);
    test_close_fail = false;

    // 四档里只有前两档会碰「弃」，弃计数不许被这三档污染
    assert(test_psram_live == 0);
    printf("  case_durability_branches ok\n");
}

// E-1（19.3）：冲账失败退避。三件事各咬一口——
//   ① 三档实测间隔恒为 50 / 200 / 1000 ms（不是「大约」）；
//   ② 索引封顶末档：第 4、5 次失败仍是 1000 ms，**没有递增律**；
//   ③ 单一时间判据：让行与退避共用一个门，任何时刻只有一条时间线在挡。
// E-1 (19.3): the retry backoff. Three separate bites: the measured gaps are exactly
// 50/200/1000 ms; the index clamps at the last step with no growth law; and the mount yield
// shares the very same gate, so there is never a second timeline.
static void case_backoff_ladder(void) {
    reset_all();
    assert(excerpt_store_enqueue("书", 1, 0u, "甲", NULL) == EXCERPT_OK);
    test_open_fail = true;

    // ① 三档实测（tick 用的是「到点才许过」，所以 now == 到期点必须能过）
    // Measured steps: the tick may pass exactly at the deadline, not one ms earlier.
    assert(excerpt_store_tick(1000u) == EXCERPT_FLUSH_FAILED); // 第 1 次失败 → 50 ms
    assert(excerpt_store_tick(1049u) == EXCERPT_FLUSH_YIELD);
    assert(excerpt_store_tick(1050u) == EXCERPT_FLUSH_FAILED); // 第 2 次失败 → 200 ms
    assert(excerpt_store_tick(1249u) == EXCERPT_FLUSH_YIELD);
    assert(excerpt_store_tick(1250u) == EXCERPT_FLUSH_FAILED); // 第 3 次失败 → 1000 ms
    assert(excerpt_store_tick(2249u) == EXCERPT_FLUSH_YIELD);
    assert(excerpt_store_tick(2250u) == EXCERPT_FLUSH_FAILED); // 第 4 次失败

    // ② 封顶：第 4、5 次仍是 1000 ms；递增律会让它们变成 4000 / 16000
    // Clamped: steps four and five stay at 1000 ms; a growth law would give 4000 / 16000.
    assert(excerpt_store_tick(3249u) == EXCERPT_FLUSH_YIELD);
    assert(excerpt_store_tick(3250u) == EXCERPT_FLUSH_FAILED); // 第 5 次失败
    assert(excerpt_store_tick(4249u) == EXCERPT_FLUSH_YIELD);
    assert(excerpt_store_tick(4250u) == EXCERPT_FLUSH_FAILED); // 第 6 次失败

    // ③ 留痕只降频不静音：六次失败、三档 → 恰好三行（每档各一次）
    // Throttled but never silenced: six failures across three steps log exactly three lines.
    assert(test_log_w == 3);

    // 恢复后必须补一条验收行：N 与 T 都要能被读出来
    // After recovery one acceptance line must appear with both N and T readable.
    test_open_fail = false;
    const int logs_before = test_log_i;
    assert(excerpt_store_tick(5250u) == EXCERPT_FLUSH_OK);
    assert(test_log_i == logs_before + 1);      // 成功补 `flushed after N retries (T ms)`
    assert(excerpt_store_pending() == 0);
    assert(excerpt_store_take_saved());

    // ④ 让行与退避共用同一门：挂载后 5 s 内不落，之后立即落
    // The yield shares the one gate: nothing lands for 5 s after a mount, then it lands.
    reset_all();
    assert(excerpt_store_enqueue("书", 2, 0u, "乙", NULL) == EXCERPT_OK);
    excerpt_store_mount_yield(1u, 1000u);
    assert(excerpt_store_tick(1000u) == EXCERPT_FLUSH_YIELD);
    assert(excerpt_store_tick(5999u) == EXCERPT_FLUSH_YIELD);
    assert(excerpt_store_tick(6000u) == EXCERPT_FLUSH_OK);

    // ⑤ 挂载会重置退避档：新一次冲账从 50 ms 起，不继承上一轮的末档
    // A mount resets the ladder: the new run starts at 50 ms, never at the held last step.
    reset_all();
    assert(excerpt_store_enqueue("书", 3, 0u, "丙", NULL) == EXCERPT_OK);
    test_open_fail = true;
    for (uint32_t t = 0; t < 6; ++t) { (void)excerpt_store_tick(t * 10000u); } // 把档推到末档
    excerpt_store_mount_yield(1u, 100000u);
    assert(excerpt_store_tick(105000u) == EXCERPT_FLUSH_FAILED);  // 让行期满，首败
    test_open_fail = false;
    assert(excerpt_store_tick(105049u) == EXCERPT_FLUSH_YIELD);   // 又是 50 ms，不是 1000
    assert(excerpt_store_tick(105050u) == EXCERPT_FLUSH_OK);

    printf("  case_backoff_ladder ok\n");
}

/// P0-4（26.11.2）牙 B43：正文里的 `\r` `\n` `\t` 折叠成单空格后，落盘行里
/// `\n` 计数为 1（仅结尾）、`\t` 计数恰好 3（三段前缀分隔符）——一行必须是一行，
/// 解析端从此无歧义。顺带验 peek 只读（P1-2）：不消耗队列、冲账如常。
/// P0-4 tooth B43: after the fold, the written line holds exactly one `\n` (the final
/// one) and exactly three `\t` (the prefix separators) - one line is one record. Also
/// verifies peek (P1-2) is read-only: the queue survives intact.
static void copy_first_line(char *dst, size_t cap) {
    const char *nl = strchr(test_fs_buf, '\n');
    assert(nl && "there must be a landed line");
    size_t n = (size_t)(nl - test_fs_buf) + 1u;
    assert(n < cap);
    memcpy(dst, test_fs_buf, n);
    dst[n] = '\0';
}
static void case_sanitize_and_peek(void) {
    reset_all();
    bool cut = false;
    assert(excerpt_store_enqueue("书", 4, 1700000000u, "a\nb\tc  d\r\n\te", &cut) == EXCERPT_OK);
    assert(!cut);
    assert(excerpt_store_pending() == 1);
    // P1-2：peek 零消耗。/ P1-2: peeking consumes nothing.
    const char *peeked = excerpt_store_peek_line(0);
    assert(peeked && strncmp(peeked, "[书]\tp=4\tt=1700000000\t", 22) == 0);
    assert(excerpt_store_peek_line(1) == NULL);
    assert(excerpt_store_pending() == 1);
    // 落盘并检查整行计数。/ Flush and count the characters of the landed line.
    assert(excerpt_store_tick(0u) == EXCERPT_FLUSH_OK);
    char line[EXCERPT_LINE_MAX];
    copy_first_line(line, sizeof(line));
    unsigned nl = 0, tabs = 0;
    for (const char *p = line; *p; ++p) {
        if (*p == '\n') ++nl;
        if (*p == '\t') ++tabs;
    }
    assert(nl == 1 && "B43: exactly one newline, at the end");
    assert(tabs == 3 && "B43: exactly the three prefix separators");
    assert(strstr(line, "\ta b c d e\n") && "P0-4: the fold, leading and doubled blanks gone");
    // 队列仍然干净：不剩第二条、不剩半行。/ The queue is still clean.
    assert(excerpt_store_pending() == 0);
    assert(excerpt_store_take_saved());
    printf("  case_sanitize_and_peek ok\n");
}

int main(void) {
    case_refuse_new();
    case_no_mem();
    case_utf8_boundaries();
    case_missing_dir();
    case_half_line();
    case_mount_yield();
    case_mount_backlog();
    case_two_beats();
    case_one_per_slot();
    case_line_budget();
    case_seq_and_fields();
    case_pre_write_failure();
    case_durability_branches();
    case_backoff_ladder();
    case_sanitize_and_peek();
    // 空文本防御 / defensive refusal on empty text
    reset_all();
    assert(excerpt_store_enqueue("书", 1, 0u, "", NULL) == EXCERPT_EMPTY);
    assert(excerpt_store_enqueue("书", 1, 0u, NULL, NULL) == EXCERPT_EMPTY);
    assert(excerpt_store_pending() == 0);
    printf("excerpt_store host tests: all green\n");
    return 0;
}
