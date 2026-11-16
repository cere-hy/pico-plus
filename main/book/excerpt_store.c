/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：摘抄落盘实现（S26-1；26.9.29 尾巴换血）。纪律见头文件；实现层面三件：
 * English: excerpt storage (S26-1; tail rewritten in 26.9.29). Three implementation notes:
 *   · 队列条目存的是**格式化后的整行**（PSRAM，每条 EXCERPT_LINE_MAX 字节）。队列
 *     深度 10，PSRAM 常量占用 ≈10 KB（台账已记）。
 *     Each queued item is the formatted line (PSRAM, EXCERPT_LINE_MAX bytes each); ten
 *     slots cost about 10 KB of PSRAM (logged in the budget ledger).
 *   · **落盘一律走 VFS/POSIX**（mkdir / fopen "ab" / fwrite / fflush / fsync / fclose），
 *     与全仓其余 42 处 fopen 同一套路径词汇。26.9.28 的事故恰恰是这里用了**裸 FatFs**
 *     却喂 VFS 路径：`f_open("/sdcard/...")` 恒 `FR_NO_PATH`，而栈上 `FIL f;` 未清零，
 *     `FF_USE_DYN_BUFFER=1` 的失败清理对野指针调 free → 断言重启。见 B38。
 *     All writes go through the VFS. The 26.9.28 crash was bare FatFs fed a VFS path,
 *     plus an uninitialised stack FIL whose error cleanup freed a wild pointer.
 *   · 落盘失败**不重试到底**：一次 tick 只试一次，失败按头文件那四档处置；这样失败
 *     既不阻塞 UI，也不会在拔卡时把主循环钉死在重试里。
 *     A failed flush retries once per tick only; failures never block the UI or pin the loop.
 */
#include "excerpt_store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "pico_bootbb.h"

static const char *TAG = "excerpt";

// 极薄 IO 尾巴：生产走真 VFS（宏，零间接、零 .bss）；宿主测试把五个口换成可注入的桩。
// 桩必须**看每一个参数**——26.9.28 的宿主桩把 path `(void)` 掉，唯一错的就是它不看的那个。
// Thin IO tail: production uses the real VFS through macros; the host tests swap five
// entries for injectable stubs. The stubs must LOOK at every argument: the 26.9.28 stub
// voided `path`, and the path was the only thing that was wrong.
#ifdef EXCERPT_HOST_TEST
int ex_mkdir(const char *path);
FILE *ex_open_append(const char *path);
size_t ex_write(const void *buf, size_t len, FILE *f);
int ex_putc(int c, FILE *f);
int ex_flush(FILE *f);
int ex_sync(FILE *f);
int ex_close(FILE *f);
#else
#define ex_mkdir(p) mkdir((p), 0777)
#define ex_open_append(p) fopen((p), "ab")
#define ex_write(b, n, f) fwrite((b), 1, (n), (f))
#define ex_putc(c, f) fputc((c), (f))
#define ex_flush(f) fflush(f)
#define ex_sync(f) fsync(fileno(f))
#define ex_close(f) fclose(f)
#endif


// 单行上限的静态账：书名 + 前缀 + 正文 + 截断标记 + 换行，必须恒 <1,024。
// Static budget: title + prefix + text + truncation marker + newline must stay < 1,024.
// 前缀形如 "[书名]\tp=4294967295\tt=4294967295\t" ≈ 34 + 160 + 尾部。
// The prefix looks like "[title]\tp=4294967295\tt=4294967295\t": about 34 + 160 + tail.
_Static_assert(EXCERPT_TITLE_MAX + EXCERPT_TEXT_MAX + 64u <= EXCERPT_LINE_MAX,
               "S26-1: one excerpt line must always fit EXCERPT_LINE_MAX");

typedef struct {
    char *line;    ///< PSRAM 上的整行 / the whole line, in PSRAM
    size_t len;    ///< 行字节数（不含结尾 0）/ bytes, NUL excluded
} excerpt_item_t;

// E-1（19.3）：冲账失败的固定三档退避。**此后维持末档，无递增律。**
// 19.3 收口过两种读法（「4×档」按递进 = 50/200/800，或按封顶 = 固定三档），两读并存不许进码；
// 取固定三档：值可静态断言、可入牙，且与判读报告原口径一致。
// E-1: three fixed backoff steps, then the last one is held. No growth law: section 19.3
// closed two competing readings of "4x step" and only the fixed table may enter code.
_Static_assert(EXCERPT_BACKOFF_STEPS == 3u, "E-1: the table is exactly three steps");
static const uint16_t k_backoff_ms[EXCERPT_BACKOFF_STEPS] = {50u, 200u, 1000u};

static excerpt_item_t s_q[EXCERPT_QUEUE_MAX];
static unsigned s_head;                  ///< 队首 / head slot
static unsigned s_count;                 ///< 积压条数 / queued entries
static bool s_saved_latch;               ///< 「已存 ✓」待取 / "saved" pending pickup
static uint32_t s_boot_seq;              ///< 当次开机序号（unix_now==0 时用）/ per-boot sequence
static unsigned s_dropped;               ///< 累计「弃」次数（诊断；reset 不清）/ cumulative drops

// **单一时间判据**（E-1）：让行窗口与失败退避共用这一个字段，绝不许两个字段各判各的。
// 0 = 立即可落。19.3 的代码表原来只有 s_ready_at_ms，失败档完全不参与——这就是风暴的根。
// The single time gate (E-1): the mount yield and the retry backoff share ONE field. Two
// fields judging separately is exactly how 389 retries went unnoticed in 4.7 s.
static uint32_t s_mount_epoch_seen;      ///< 已让过行的挂载代号（幂等锚）/ epoch already yielded
static uint32_t s_next_try_ms;           ///< 下次允许尝试的时刻；0 = 立即 / next allowed try; 0 = now
static uint32_t s_retry_since_ms;        ///< 本轮首次失败时刻（成功报 T ms 用）/ first failure of this run
static uint16_t s_retry_count;           ///< 本轮重试次数（验收行 N）/ retries in this run
static uint8_t  s_backoff_idx;           ///< 当前退避档，封顶末档 / current step, clamped to last
static uint8_t  s_logged_mask;           ///< 已留痕的档位（每档各一次）/ steps already logged

/// UTF-8 安全截断：回退到字符边界，绝不在 continuation byte 上切断（细化账 7 同源纪律）。
/// UTF-8-safe truncation: back off to a char boundary so we never cut inside a sequence.
///
/// 只需回退续字节：循环停下时 s[i] 必是 ASCII 或首字节（若是续字节，循环还会再退一格），
/// 而 [0,i) 因此全是完整序列。S26-1 变异 M108 曾在这里多写一段「若前一个字节是首字节
/// 则再退一格」的判断——那段条件恒假（s[i-1] 是首字节 ⟹ s[i] 是续字节 ⟹ 循环不会停），
/// 是死代码，已删。变异存活一次，换掉一段死代码，值。
/// Backing off continuation bytes is enough: when the loop stops, s[i] is ASCII or a lead
/// byte (a continuation byte would have made it step back once more), so [0,i) is a whole
/// number of characters. Mutation M108 once added a "step back again if the previous byte
/// is a lead byte" clause here; that condition can never hold (s[i-1] being a lead byte
/// implies s[i] is a continuation, so the loop would not have stopped). Dead code, removed.
static size_t utf8_cut(const char *s, size_t n) {
    if (n == 0) return 0;
    size_t i = n;
    while (i > 0 && ((unsigned char)s[i] & 0xC0u) == 0x80u) --i; // 回退过所有续字节 / skip continuation bytes
    return i;
}

/// P0-4（26.11.2）：净化——`\r` `\n` `\t` 与连续空白折叠成单个空格，行首空白不落。
/// 一行必须是一行：字段分隔符 `\t` 与记录分隔符 `\n` 从此不可能出现在正文或书名里。
/// 长度只减不增（每段空白折一、其余逐字节拷贝），所以折叠长度 ≤ 源长——
/// 先数折叠长度再决定是否走截断路径，绝不虚报 truncated。
/// 26 天的存量多行记录写入端救不了，由读取端容忍（excerpt_store_render_view）。
/// P0-4 sanitize: CR/LF/tab and whitespace runs fold into one space; a line stays a
/// line. Folded length never exceeds the source, so we measure first and only take the
/// truncation path when folding alone cannot fit.
static size_t fold_ws(char *dst, size_t cap, const char *src, bool *truncated) {
    if (truncated) *truncated = false;
    const char *s = src ? src : "";
    // 一遍数折叠长度。/ Pass one: folded length.
    size_t folded = 0;
    bool in_ws = false;
    for (; *s; ++s) {
        if (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
            if (!in_ws && folded) ++folded;
            in_ws = true;
        } else {
            ++folded;
            in_ws = false;
        }
    }
    if (folded <= cap) {
        size_t at = 0;
        in_ws = false;
        for (s = src ? src : ""; *s; ++s) {
            if (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
                if (!in_ws && at) dst[at++] = ' ';
                in_ws = true;
            } else {
                dst[at++] = *s;
                in_ws = false;
            }
        }
        dst[at] = '\0';
        return at;
    }
    // 超长：留出 `[截]` 的 5 字节，写满 room 后回退到 UTF-8 边界再补标记——
    // 绝不在续字节上切断（utf8_cut 同源纪律，M108 的教训）。
    // Overlong: reserve room for the marker, cut at a character boundary, then mark.
    if (truncated) *truncated = true;
    const size_t room = cap > 5u ? cap - 5u : 0u;
    size_t at = 0;
    in_ws = false;
    for (s = src ? src : ""; *s; ++s) {
        const bool ws = (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r');
        if (ws) {
            if (in_ws || !at) continue; // 连续空白折一；行首空白不落
            if (at >= room) break;
            dst[at++] = ' ';
            in_ws = true;
        } else {
            if (at >= room) break;
            dst[at++] = *s;
            in_ws = false;
        }
    }
    at = utf8_cut(dst, at);
    memcpy(dst + at, "[截]", 5);
    return at + 5;
}

void excerpt_store_reset(void) {
    for (unsigned i = 0; i < EXCERPT_QUEUE_MAX; ++i) {
        if (s_q[i].line) { heap_caps_free(s_q[i].line); s_q[i].line = NULL; }
        s_q[i].len = 0;
    }
    s_head = s_count = 0;
    s_saved_latch = false;
    s_mount_epoch_seen = 0; // 组件侧代号也从 0 起，两侧必须一起归零 / both sides restart at 0
    s_next_try_ms = 0;
    s_retry_since_ms = 0;
    s_retry_count = 0;
    s_backoff_idx = 0;
    s_logged_mask = 0;
}

unsigned excerpt_store_pending(void) { return s_count; }

unsigned excerpt_store_dropped(void) { return s_dropped; }

bool excerpt_store_take_saved(void) {
    const bool v = s_saved_latch;
    s_saved_latch = false;
    return v;
}

void excerpt_store_mount_yield(uint32_t mount_epoch, uint32_t now_ms) {
    // E-2（§19.4）：让行的锚点从「状态位边沿」换成「挂载代号」。
    //
    // 旧接法 `if (gained) notify_mounted()` 里 `gained = !*mounted && ready` 是**状态差分**：
    // 拔插快于一次轮询时 app 侧的 `*mounted` 全程为 true，这条边沿一次都没成立。
    // 26.9.31 现场 100.977 挂载、100.994 就写卡（17 ms），而 `mounted=1` 到 101.863 才出现——
    // 那 886 ms 里的每一次家务片都在无让行的状态下冲账。
    // 代号是**事件**：挂载一次 +1 一次，不依赖任何快照的前后值。
    //
    // 幂等是必需的，不是保险：本函数被两个地方各调一次——`poll_media`（挂载发现的最早时机）
    // 和 `excerpt_footer_tick` 首行（写卡前一刻，与 `excerpt_store_tick` 相邻两行）。
    // 后者让「观测」与「写卡」之间物理上没有别的事情可插，窗口归零；前者让让行在最早处落下。
    // 同一代号到达两次只写一次让行，否则第二次会把窗口往后推 5 s（`if (ready)` 那种假接线）。
    // Idempotent by necessity: both poll_media and the footer tick call this. The tick's call
    // sits immediately before the write attempt, so nothing can run between observation and
    // write; the poll call lands the window as early as it is knowable. One epoch, one window.
    if (mount_epoch == s_mount_epoch_seen) return;
    s_mount_epoch_seen = mount_epoch;
    // 让行 5 s 与失败退避共用同一判据；挂载即新一轮，退避档从头起算。
    // The 5 s yield shares the one gate with the retry backoff; a mount starts a new run.
    s_next_try_ms = now_ms + EXCERPT_MOUNT_YIELD_MS;
    s_retry_since_ms = 0;
    s_retry_count = 0;
    s_backoff_idx = 0;
    s_logged_mask = 0;
}

/// 记一次失败：推进退避档（封顶末档）、设下次时刻、按档留痕（每档各一次）。
/// 留痕只降频不静音——「失败而不留痕」是 26.9.28 的原罪，这行注释本身就是给它立的钉子。
/// Record a failure: advance the step (clamped), set the next try, and log once per step.
/// Throttled, never silenced: failing without a trace is the original 26.9.28 defect.
static void backoff_on_fail(uint32_t now_ms, const char *what, int err) {
    if (s_retry_count == 0) s_retry_since_ms = now_ms;
    ++s_retry_count;

    const uint8_t step = s_backoff_idx;
    s_next_try_ms = now_ms + k_backoff_ms[step];
    if (s_backoff_idx + 1u < (uint8_t)EXCERPT_BACKOFF_STEPS) ++s_backoff_idx;

    const uint8_t bit = (uint8_t)(1u << step);
    if (!(s_logged_mask & bit)) {
        s_logged_mask = (uint8_t)(s_logged_mask | bit);
        ESP_LOGW(TAG, "%s: errno=%d, keeping %u queued (retry #%u in %u ms, step %u/%u)",
                 what, err, s_count, (unsigned)s_retry_count,
                 (unsigned)k_backoff_ms[step], (unsigned)(step + 1u), (unsigned)EXCERPT_BACKOFF_STEPS);
    }
}

excerpt_status_t excerpt_store_enqueue(const char *book, size_t page, uint32_t unix_now,
                                       const char *text, bool *truncated) {
    if (truncated) *truncated = false;
    if (!text || !*text) return EXCERPT_EMPTY;
    if (s_count >= EXCERPT_QUEUE_MAX) {
        // 拒新，不丢旧（细化账 1）。/ Refuse the new one; keep every old one.
        ESP_LOGW(TAG, "queue full (%u): refusing the new excerpt", (unsigned)s_count);
        return EXCERPT_FULL;
    }

    char *line = (char *)heap_caps_malloc(EXCERPT_LINE_MAX, MALLOC_CAP_SPIRAM);
    if (!line) {
        ESP_LOGW(TAG, "no PSRAM for the excerpt line");
        return EXCERPT_NO_MEM;
    }

    size_t at = 0;
    line[at++] = '[';
    // P0-4：书名与正文都过净化。书名截断保持原语义（不亮 truncated，正文才亮）。
    // P0-4: both title and body pass the fold; only a cut body lights `truncated`.
    at += fold_ws(line + at, EXCERPT_TITLE_MAX, book ? book : "未知", NULL);
    at += (size_t)snprintf(line + at, EXCERPT_LINE_MAX - at, "]\tp=%u\t", (unsigned)page);
    if (unix_now) {
        at += (size_t)snprintf(line + at, EXCERPT_LINE_MAX - at, "t=%u\t", (unsigned)unix_now);
    } else {
        at += (size_t)snprintf(line + at, EXCERPT_LINE_MAX - at, "#%u\t", (unsigned)(++s_boot_seq));
    }

    const size_t room = EXCERPT_LINE_MAX - at - 2u; // 正文 + 换行 + 结尾 0 / text + newline + NUL
    const size_t cap = room < EXCERPT_TEXT_MAX ? room : EXCERPT_TEXT_MAX;
    bool body_truncated = false;
    const size_t body = fold_ws(line + at, cap, text, &body_truncated);
    if (body_truncated && truncated) *truncated = true;
    at += body;
    line[at++] = '\n';
    line[at] = '\0';

    const unsigned slot = (s_head + s_count) % EXCERPT_QUEUE_MAX;
    s_q[slot].line = line;
    s_q[slot].len = at;
    ++s_count;
    return EXCERPT_OK;
}

/// 出队一条（成功或弃，共用）。调用方负责判成败。/ Pop the head slot (success or drop).
static void pop_head(void) {
    heap_caps_free(s_q[s_head].line);
    s_q[s_head].line = NULL;
    s_q[s_head].len = 0;
    s_head = (s_head + 1u) % EXCERPT_QUEUE_MAX;
    --s_count;
}

excerpt_flush_t excerpt_store_tick(uint32_t now_ms) {
    if (s_count == 0) return EXCERPT_FLUSH_IDLE;
    // 单一判据：让行与退避都落在这里。/ One gate for both the yield and the backoff.
    if (s_next_try_ms && now_ms < s_next_try_ms) return EXCERPT_FLUSH_YIELD;

    const excerpt_item_t *it = &s_q[s_head];

    // 细化账一.5：重试第一步恒为 mkdir，目录被删也能自愈。EEXIST 是常态，其它错误
    // 照样往下走一步——让 open 报出真正的 errno；26.9.28 那次连一行错误日志都没有，
    // 「失败而不留痕」本身就是缺陷。
    // Item one point five: every retry starts with mkdir. EEXIST is normal; other errors
    // still fall through to open(), which reports the real errno. In 26.9.28 not a single
    // error line was printed - failing without a trace is a defect of its own.
    if (ex_mkdir(EXCERPT_DIR) != 0 && errno != EEXIST) {
        const int err = errno;
        backoff_on_fail(now_ms, "mkdir(" EXCERPT_DIR ")", err);
        return EXCERPT_FLUSH_FAILED;
    }
    FILE *f = ex_open_append(EXCERPT_PATH);
    if (!f) {
        // ① 零字节写入 → 留队重试是免费的。/ Nothing written yet: keeping the head is free.
        const int err = errno;
        backoff_on_fail(now_ms, "open(" EXCERPT_PATH ")", err);
        return EXCERPT_FLUSH_FAILED;
    }

    const size_t wr = ex_write(it->line, it->len, f);
    if (wr != it->len || ex_flush(f) != 0) {
        // ② 卡上已经有半行：留队重试 = 追加重复整行。补一个 `\n` 把残行锁死在本行内
        //    （下一行摘抄因此仍是干净单行），然后**弃**并在页脚亮红。
        // A half line is already on the card, so a retry would duplicate it. Terminate the
        // fragment with a newline (the next excerpt therefore still starts on a clean line),
        // then drop the entry and go red on the footer.
        const int err = errno;
        (void)ex_putc('\n', f);
        (void)ex_flush(f);
        (void)ex_close(f);
        ++s_dropped;
        pico_bootbb_note(PICO_BOOTBB_NOTE_EXCERPT_FAIL, (uint8_t)(err & 0xFF), (uint16_t)s_dropped);
        ESP_LOGE(TAG, "write %u/%u / flush failed (errno=%d): fragment terminated, entry dropped (%u so far)",
                 (unsigned)wr, (unsigned)it->len, err, s_dropped);
        pop_head();
        // 本条已了结：退避归零，下一条立即可以试（卡刚写进过字节，说明它是可写的）。
        // This entry is finished: clear the backoff so the next one may try at once.
        s_next_try_ms = 0;
        s_retry_since_ms = 0;
        s_retry_count = 0;
        s_backoff_idx = 0;
        s_logged_mask = 0;
        return EXCERPT_FLUSH_DROPPED;
    }

    const int sy = ex_sync(f);
    const int sy_errno = errno;
    const int cl = ex_close(f);

    if (sy != 0 && cl != 0) {
        // ③ 目录项长度未回写，且 fclose 的 f_sync 也没成 → 可见内容不含本行 → 留队。
        //    拔卡主场景走这一档：卡回来了，同一时间窗口内重试，行不重复。
        // fsync failed and close's own f_sync failed too: the line is not visible, so the
        // entry stays queued. This is the pull-the-card branch, and retrying is safe.
        backoff_on_fail(now_ms, "fsync+close", sy_errno);
        return EXCERPT_FLUSH_FAILED;
    }
    if (sy != 0) {
        // ③' fsync 失败但 fclose 内部的 f_sync 成功：这一行其实已经持久。再留队就会写
        //     重复——按不变式（可见内容已含本行）算成功。
        // fsync failed but fclose's internal f_sync succeeded: the line IS durable. Keeping
        // it would duplicate the line, so by the invariant this counts as a save.
        ESP_LOGW(TAG, "fsync errno=%d but close synced: line is durable", sy_errno);
    } else if (cl != 0) {
        // ④ 数据已由 fsync 确认，close 只是收尾失败 → 算成功，只记日志。
        // The data is already durable; a close failure is only a teardown error.
        ESP_LOGW(TAG, "close failed after fsync (data is durable), errno=%d", errno);
    }

    pop_head();
    s_saved_latch = true; // 「已存 ✓」的唯一来源 / the only source of "saved"

    // 验收行（19.3）：N 与 T 都要能直接从日志读出，不用回推时间戳。
    // Acceptance line: both N and T are printed, never reconstructed from wall clock.
    if (s_retry_count != 0) {
        ESP_LOGI(TAG, "flushed after %u retries (%u ms)", (unsigned)s_retry_count,
                 (unsigned)(now_ms - s_retry_since_ms));
    }
    s_next_try_ms = 0;
    s_retry_since_ms = 0;
    s_retry_count = 0;
    s_backoff_idx = 0;
    s_logged_mask = 0;
    return EXCERPT_FLUSH_OK;
}

const char *excerpt_store_peek_line(unsigned index) {
    // P1-2：零拷贝、零出队——返回队列槽位本身。绝不在这里碰 s_head / s_count。
    // P1-2: zero copy, zero dequeue - the slot itself. Never touch s_head/s_count here.
    if (index >= s_count) return NULL;
    return s_q[(s_head + index) % EXCERPT_QUEUE_MAX].line;
}

/* ---------- P0-5 虚拟摘抄本 / The virtual excerpt book (26.11.2) ---------- */
//
// 解析规则（送审定案 §P0-5，注释即规格，正则靠不住）：
// Parsing rules (design ruling §P0-5; the comment IS the spec, regexes are not):
//   1. 以 `[` 开头**且含 `]\t`** 的行才是新记录；其余行并入上一条。P0-4 只救写入端，
//      救不了已追加 26 天的存量多行记录——归并就是对这些历史的容忍。
//      批文里「行首 `p=` 开新记录」一读按字面在真实格式（`[书名]\tp=…`，自 26.9.26
//      df3a4a4 起从未变过）下永不命中；两读并存不进码（E-1 19.3 同纪律），以
//      存储格式为准、按定案规则落码。
//      A record starts at a line beginning with `[` AND containing `]\t`; every other
//      line merges into the previous record. The writer-side fold cannot repair 26 days
//      of legacy multi-line records; merging is the tolerance. The "p= starts a record"
//      reading never matches the real format and is not coded (two readings never both
//      enter code, same as E-1 §19.3).
//   2. 书名 = 第一个 `]\t` 之前（书名里可以有 `]`）。/ Title = before the FIRST `]\t`.
//   3. 之后依次 p=<十进制>、t=<十进制> 或 #<序号>、`\t`，其余全是正文。
//   4. p 是 0 基视觉页序，显示 +1。/ p is 0-based; display adds one.
//   5. t=0（#序号）显示「未校时」，不编日期。/ No clock: say so, never invent a date.
//   6. 正文尾部 `[截]` 显示为（摘录时已截断）。/ The marker becomes a phrase.
//   7. `[` 开头但无 `]\t` 的行 = 残片；无上一条可归并的续行同计——末尾给
//      「另有 N 条无法识别」，不静默丢。/ Bad lines are counted, never dropped silently.

typedef struct {
    long     off;    ///< 记录首行起始偏移 / offset of the record's first line
    uint32_t len;    ///< 到下一记录起始（或 EOF）的字节数 / bytes to the next record or EOF
    uint32_t t;      ///< unix 时间；0 = 未校时 / unix time; 0 = clock never set
    uint32_t p;      ///< 0 基视觉页序 / 0-based visual page
    uint32_t hash;   ///< 书名 FNV-1a（月度「来自 N 本书」去重用）/ title hash for the count
    uint32_t seq;    ///< 原始顺序（qsort 并列 tiebreak）/ original order for stable ties
} ex_rec_t;

typedef struct {
    ex_rec_t *v;
    size_t n, cap;
} ex_recs_t;

/// 索引硬顶：2 万条 ≈ 480 KB PSRAM。摘抄是人手一条条存的，到不了；到了就停，
/// 不做无界承诺（渲染侧另有 4,000 条软顶）。/ Hard index cap, PSRAM bounded.
#define EX_REC_INDEX_CAP 20000u

static bool ex_recs_push(ex_recs_t *r, const ex_rec_t *e) {
    if (r->n >= EX_REC_INDEX_CAP) return false;
    if (r->n == r->cap) {
        const size_t next_cap = r->cap ? r->cap * 2u : 64u;
        ex_rec_t *nv = (ex_rec_t *)heap_caps_realloc(r->v, next_cap * sizeof(ex_rec_t),
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!nv) return false;
        r->v = nv;
        r->cap = next_cap;
    }
    r->v[r->n++] = *e;
    return true;
}

static uint32_t ex_fnv1a(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    return h;
}

/// 解析记录首行字段（就地切分，调用方先去行尾换行）。返回 false = 无 `]\t`（残片）。
/// Parse a record's first line in place. false = no `]\t` (a fragment).
static bool ex_parse_head(char *line, char **title, uint32_t *p, uint32_t *t,
                          bool *has_t, char **body) {
    if (line[0] != '[') return false;
    char *end = strstr(line, "]\t");
    if (!end) return false;
    *end = '\0';
    *title = line + 1;
    char *cur = end + 2;
    *p = 0;
    *t = 0;
    *has_t = false;
    if (!strncmp(cur, "p=", 2)) *p = (uint32_t)strtoul(cur + 2, NULL, 10);
    cur = strchr(cur, '\t');
    if (!cur) { *body = line + strlen(line); return true; }  /* 行尾 NUL 即可写空串，避免字面量丢 const */
    ++cur;
    if (!strncmp(cur, "t=", 2)) {
        *has_t = true;
        *t = (uint32_t)strtoul(cur + 2, NULL, 10);
    } else if (*cur == '#') {
        (void)strtoul(cur + 1, NULL, 10); // #序号：接受但不编时间 / accepted, never a date
    }
    cur = strchr(cur, '\t');
    *body = cur ? cur + 1 : line + strlen(line);
    return true;
}

static void ex_fmt_when(uint32_t t, char *out, size_t cap) {
    if (!t) {
        snprintf(out, cap, "未校时");
        return;
    }
    const time_t tt = (time_t)t;
    struct tm tmv;
    localtime_r(&tt, &tmv);
    snprintf(out, cap, "%02d-%02d %02d:%02d", tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min);
}

/// 自然月键：t==0 归 0 号组（时间未校准，排最末）。/ Calendar-month key; 0 = untimed.
static uint32_t ex_month_key(uint32_t t) {
    if (!t) return 0;
    const time_t tt = (time_t)t;
    struct tm tmv;
    localtime_r(&tt, &tmv);
    return (uint32_t)((tmv.tm_year + 1900) * 12 + tmv.tm_mon);
}

static void ex_fmt_month(uint32_t key, char *out, size_t cap) {
    if (!key) {
        snprintf(out, cap, "时间未校准");
        return;
    }
    snprintf(out, cap, "%u年%u月", (unsigned)(key / 12), (unsigned)(key % 12 + 1));
}

/// 正文折叠输出：连续空白折一，行首空白不落（P0-4 同款语义）。空格**挂起**到
/// 下一个非空白字符才落；两个状态（at_start / pending_space）由调用方持有，
/// 跨多次调用与跨行归并共享——连接空格因此自然浮现，绝不出现双空格。
/// Folded body output: whitespace runs fold into one space, none at the start. The
/// space is HELD until the next non-blank character; BOTH states belong to the caller
/// so merges across calls and lines share one stream of state - the joining space
/// emerges naturally and doubles never happen.
static void ex_fold_fputs(FILE *out, const char *s, bool *at_start, bool *pending_space) {
    for (const char *p = s; *p; ++p) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            if (!*at_start) *pending_space = true;
        } else {
            if (*pending_space) {
                fputc(' ', out);
                *pending_space = false;
            }
            fputc(*p, out);
            *at_start = false;
        }
    }
}

/// 正文尾部是否带 `[截]`（带则剥掉，由调用方换成可读短语）。/ Tail marker detect+strip.
static bool ex_strip_cut_marker(char *s) {
    const size_t n = strlen(s);
    if (n < 5u || strcmp(s + n - 5u, "[截]")) return false;
    s[n - 5u] = '\0';
    return true;
}

static bool ex_is_view(const char *path) {
    if (!path || !path[0]) return false;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *view = strrchr(EXCERPT_VIEW, '/');
    view = view ? view + 1 : EXCERPT_VIEW;
    return !strcasecmp(base, view);
}

bool excerpt_store_is_view_path(const char *path) { return ex_is_view(path); }
const char *excerpt_store_view_path(void) { return EXCERPT_VIEW; }

static int ex_rec_cmp(const void *a, const void *b) {
    const ex_rec_t *x = (const ex_rec_t *)a;
    const ex_rec_t *y = (const ex_rec_t *)b;
    if (x->t != y->t) return x->t > y->t ? -1 : 1; // 最新在前 / newest first
    return x->seq > y->seq ? -1 : 1;               // 同刻：文件靠后在前 / later file order first
}

bool excerpt_store_render_view(unsigned *out_count) {
    if (out_count) *out_count = 0;
    // 目录可能尚不存在（从未落过摘抄的机器也要能开空态书）。
    // The directory may not exist yet; even an empty book needs somewhere to land.
    if (mkdir(EXCERPT_DIR, 0777) != 0 && errno != EEXIST) return false;

    ex_recs_t recs = {0};
    unsigned bads = 0;
    FILE *in = fopen(EXCERPT_PATH, "r");

    // ---- 第一遍：建 PSRAM 行索引（记录偏移 + 字段），顺手数坏行。----
    // ---- Pass one: build the PSRAM line index, counting bad lines on the way. ----
    if (in) {
        char line[EXCERPT_LINE_MAX];
        long line_off = ftell(in);
        long rec_off = -1;     // 当前打开的记录起始；-1 = 无 / open record start, -1 = none
        uint32_t rec_t = 0, rec_p = 0, rec_hash = 0;
        uint32_t seq = 0;
        while (fgets(line, sizeof(line), in)) {
            const long next = ftell(in);
            char *nl = strpbrk(line, "\r\n");
            if (nl) *nl = '\0';
            char *title = NULL, *body = NULL;
            uint32_t p = 0, t = 0;
            bool has_t = false;
            const bool is_record = line[0] == '[' && ex_parse_head(line, &title, &p, &t, &has_t, &body);
            if (is_record) {
                if (rec_off >= 0) {
                    const ex_rec_t e = {rec_off, (uint32_t)(line_off - rec_off), rec_t,
                                        rec_p, rec_hash, seq++};
                    if (!ex_recs_push(&recs, &e)) break; // 索引顶：停止（见 EX_REC_INDEX_CAP）
                }
                rec_off = line_off;
                rec_t = has_t ? t : 0;
                rec_p = p;
                rec_hash = ex_fnv1a(title, strlen(title));
            } else if (line[0] == '[') {
                // 残片（`[` 开头无 `]\t`）：计数、终结上一条。/ A fragment: count it.
                if (rec_off >= 0) {
                    const ex_rec_t e = {rec_off, (uint32_t)(line_off - rec_off), rec_t,
                                        rec_p, rec_hash, seq++};
                    if (!ex_recs_push(&recs, &e)) break;
                    rec_off = -1;
                }
                ++bads;
            } else if (rec_off < 0) {
                ++bads; // 无上一条可归并的续行 / an orphan continuation
            }
            line_off = next;
        }
        if (rec_off >= 0) {
            const long end = ftell(in);
            const ex_rec_t e = {rec_off, (uint32_t)(end - rec_off), rec_t, rec_p, rec_hash, seq++};
            (void)ex_recs_push(&recs, &e);
        }
        // in 保持打开：第二遍按索引偏移重读正文；关闭统一走函数尾部。
        // `in` stays open: pass two re-reads bodies by offset; closing is centralized
        // at the tail of this function.
    }

    qsort(recs.v, recs.n, sizeof(ex_rec_t), ex_rec_cmp);

    // ---- 分组（自然月），组内「来自 N 本书」按书名哈希去重。----
    // ---- Group by month; distinct books per group via title hashes. ----
    typedef struct { uint32_t key; size_t begin, end; } ex_group_t;
    const size_t total = recs.n;
    const size_t render_n = total > EXCERPT_VIEW_MAX_RECORDS ? EXCERPT_VIEW_MAX_RECORDS : total;
    ex_group_t *groups = (ex_group_t *)heap_caps_calloc(
        render_n ? render_n : 1u, sizeof(ex_group_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint32_t *hashes = (uint32_t *)heap_caps_calloc(
        render_n ? render_n : 1u, sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t group_count = 0;
    bool ok = groups && hashes;
    if (ok) {
        for (size_t i = 0; i < render_n; ++i) {
            const uint32_t key = ex_month_key(recs.v[i].t);
            if (group_count == 0 || groups[group_count - 1].key != key) {
                groups[group_count].key = key;
                groups[group_count].begin = i;
                groups[group_count].end = i;
                ++group_count;
            }
            groups[group_count - 1].end = i + 1;
            hashes[i] = recs.v[i].hash;
        }
    }

    FILE *out = ok ? fopen(EXCERPT_VIEW_TMP, "w") : NULL;
    ok = ok && out;
    unsigned pending_shown = 0;
    if (ok && total == 0 && excerpt_store_pending() == 0) {
        // 空态：与生词本同款，写一本只有提示的书。/ Empty state, same as vocab.
        const int wr = fprintf(out, "还没有摘抄。\n\n读书时框选一段文字，点「存摘抄」，\n"
                                    "摘抄会先排队，插回卡后自动保存到这里。\n");
        const int cl = fclose(out);
        out = NULL;
        ok = wr >= 0 && cl == 0;
    } else if (ok) {
        char line[EXCERPT_LINE_MAX];
        // 26.11.4：**容器节有料才写，且不占章号**。
        //
        // 旧形态无论有没有内容都先写一个「第1章」，接着数据章又从「第1章」起算——
        // 目录里两个第 1 章，而那条空身容器在真机上就是一页空白（基线照片即此）。
        // 编号属于数据，说明属于容器：容器节只在真有说明（超量注明 / 待落盘）时出现，
        // 且不写「第N章」，于是数据章恒定从第 1 章起、永不撞号。
        // The container section is written only when it has something to say, and it
        // takes no chapter number. The old shape always emitted 第1章 and then numbered
        // the data chapters from 1 as well: two of them, plus a blank page on hardware.
        const unsigned pend = excerpt_store_pending();
        const bool has_note = total > EXCERPT_VIEW_MAX_RECORDS;
        if (ok && has_note)
            ok = fprintf(out, "（共 %u 条，仅显示最近 %u 条）\n\n", (unsigned)total,
                         (unsigned)EXCERPT_VIEW_MAX_RECORDS) >= 0;
        // P1-2：待落盘节置顶——只读窥队列，不复制、不出队。
        // P1-2: the pending section on top, peeked read-only.
        if (ok && pend) {
            ok = fprintf(out, "〔待落盘 %u 条 · 插卡后自动保存〕\n\n", pend) >= 0;
            for (unsigned i = 0; ok && i < pend; ++i) {
                const char *raw = excerpt_store_peek_line(i);
                if (!raw) break;
                snprintf(line, sizeof(line), "%s", raw);
                char *nl = strpbrk(line, "\r\n");
                if (nl) *nl = '\0';
                char *title = NULL, *body = NULL;
                uint32_t p = 0, t = 0;
                bool has_t = false;
                if (line[0] == '[' && ex_parse_head(line, &title, &p, &t, &has_t, &body)) {
                    char when[32];
                    ex_fmt_when(has_t ? t : 0, when, sizeof(when));
                    const bool cut = ex_strip_cut_marker(body);
                    ok = fprintf(out, "《%s》· 第 %u 页 · %s\n「", title, (unsigned)(p + 1), when) >= 0;
                    bool at_start = true, pending_space = false;
                    if (ok) ex_fold_fputs(out, body, &at_start, &pending_space);
                    if (ok) ok = fprintf(out, "」%s\n\n", cut ? "（摘录时已截断）" : "") >= 0;
                }
            }
            if (ok) ok = fputc('\n', out) != EOF;
        }
        pending_shown = excerpt_store_pending();
        // ---- 章与条目。/ Chapters and entries. ----
        for (size_t g = 0; ok && g < group_count; ++g) {
            const ex_group_t *grp = &groups[g];
            unsigned books = 0;
            for (size_t i = grp->begin; i < grp->end; ++i) {
                bool seen = false;
                for (size_t j = grp->begin; j < i; ++j)
                    if (hashes[j] == hashes[i]) { seen = true; break; }
                if (!seen) ++books;
            }
            char month[32];
            ex_fmt_month(grp->key, month, sizeof(month));
            ok = fprintf(out, "第%u章\n\n%s · %u 条 · 来自 %u 本书\n\n",
                         (unsigned)(g + 1), month, (unsigned)(grp->end - grp->begin),
                         books) >= 0;
            for (size_t i = grp->begin; ok && i < grp->end; ++i) {
                const ex_rec_t *r = &recs.v[i];
                if (!in || fseek(in, r->off, SEEK_SET) != 0) { ok = false; break; }
                if (!fgets(line, sizeof(line), in)) { ok = false; break; }
                char *nl = strpbrk(line, "\r\n");
                if (nl) *nl = '\0';
                char *title = NULL, *body = NULL;
                uint32_t p = 0, t = 0;
                bool has_t = false;
                if (!ex_parse_head(line, &title, &p, &t, &has_t, &body)) { ++bads; continue; }
                char when[32];
                ex_fmt_when(has_t ? t : 0, when, sizeof(when));
                ok = fprintf(out, "《%s》· 第 %u 页 · %s\n「", title,
                             (unsigned)(p + 1), when) >= 0;
                bool at_start = true, pending_space = false;
                bool cut = ex_strip_cut_marker(body);
                if (ok) ex_fold_fputs(out, body, &at_start, &pending_space);
                // 续行归并：并入正文（规则 1 的读取端容忍）。行界换行被 fgets 吃掉，
                // 归并空格在这里补——挂起语义保证与续行自身的行首空白不叠加。
                // Merge continuation lines (the read-side tolerance). fgets eats the line
                // break, so the merging space is supplied HERE; the held-space rule keeps
                // it from stacking with the continuation's own leading blanks.
                while (ok) {
                    const long pos = ftell(in);
                    if (pos < 0 || (uint32_t)pos >= r->off + r->len) break;
                    if (!fgets(line, sizeof(line), in)) break;
                    char *nl2 = strpbrk(line, "\r\n");
                    if (nl2) *nl2 = '\0';
                    if (!at_start) pending_space = true;
                    ex_fold_fputs(out, line, &at_start, &pending_space);
                    cut = cut || ex_strip_cut_marker(line);
                }
                if (ok) ok = fprintf(out, "」%s\n\n", cut ? "（摘录时已截断）" : "") >= 0;
            }
        }
        if (ok && bads) ok = fprintf(out, "另有 %u 条无法识别。\n", bads) >= 0;
        if (out && fclose(out) != 0) ok = false;
        out = NULL;
    }

    heap_caps_free(hashes);
    heap_caps_free(groups);
    heap_caps_free(recs.v);
    if (in) fclose(in);

    if (!ok) {
        remove(EXCERPT_VIEW_TMP);
        return false;
    }
    // 成品不变式：rename 前旧成品要么在、要么 ENOENT；失败删 tmp，成品原样。
    // The finished-file invariant: rename onto a clean slate; failure leaves it intact.
    if (remove(EXCERPT_VIEW) != 0 && errno != ENOENT) {
        remove(EXCERPT_VIEW_TMP);
        return false;
    }
    if (rename(EXCERPT_VIEW_TMP, EXCERPT_VIEW) != 0) {
        remove(EXCERPT_VIEW_TMP);
        return false;
    }
    if (out_count)
        *out_count = (unsigned)render_n + pending_shown;
    return true;
}
