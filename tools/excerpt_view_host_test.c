/*
 * 中文：P0-5 虚拟摘抄本渲染宿主测试（26.11.2）。渲染走真 stdio + 临时目录（-D 注入，
 * 头文件 #ifndef 与 vocab_log 同款），与写路径的桩 FS 测试（excerpt_store_host_test.c）
 * 互不混装——桩测逻辑，这一层测「卡上那份文件读回来长什么样」。
 * English: P0-5 render host tests (26.11.2). Real stdio against an injected temp dir;
 * separate from the stub-FS write tests because this layer tests what the file on the
 * card looks like when read back.
 *
 * 覆盖 / Coverage:
 *   1. 20 条含跨行框选：全对、页码 +1、最新在前（送审 #9 的宿主镜像）
 *      20 records with multi-line legacy entries: all rendered, page +1, newest first
 *   2. 历史脏数据容忍：续行并入上一条、残片计数「另有 N 条无法识别」
 *      Legacy tolerance: continuations merge, fragments are counted, never dropped silently
 *   3. `[截]` →（摘录时已截断）；#序号 →「未校时」，不编日期
 *   4. 按月分章（第N章 是 book_txt.c 认的唯一标题形式）
 *   5. 待落盘节置顶（P1-2，只读 peek，不出队）
 *   6. 空态提示书；成品不变式（tmp → rename，tmp 不残留）
 *   7. 4,000 软顶：超限只渲染最近 N 条并在首行注明
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "excerpt_store.h"
#include "esp_heap_caps.h"

// esp_log 桩的计数符号（渲染路径不关心日志计数，给出定义以供链接）。
// The log stub's counters (the render path ignores them; definitions for the link).
int test_log_w, test_log_e, test_log_i;
// 桩堆与黑匣子计数。/ The stub heap counters and the black-box note counter.
bool test_psram_fail;
int test_psram_live;
int test_bb_notes;

static const char *FS_DIR = "build/excerpt-tests/fsview";

static void fs_reset(void) {
    (void)system("rm -rf build/excerpt-tests/fsview");
    assert(mkdir(FS_DIR, 0777) == 0);
    excerpt_store_reset();
}

static void write_file(const char *body) {
    FILE *f = fopen(EXCERPT_PATH, "w");
    assert(f);
    assert(fputs(body, f) >= 0);
    assert(fclose(f) == 0);
}

static char *read_view(void) {
    FILE *f = fopen(EXCERPT_VIEW, "r");
    assert(f && "the view must exist after a successful render");
    static char buf[262144];
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    assert(fclose(f) == 0);
    return buf;
}

static void case_twenty_with_multiline(void) {
    fs_reset();
    // 20 条：3 条是 26.9.29 之前的存量多行记录（正文带换行/制表），1 条 [截]，
    // 1 条 #序号（未校时），时间倒着写两个自然月。
    // 20 records: 3 legacy multi-line, 1 truncated, 1 unclocked, across two months.
    char big[65536];
    size_t at = 0;
    at += (size_t)snprintf(big + at, sizeof(big) - at,
        "[甲书]\tp=0\tt=1730000000\t跨行第一条\n第二行续\n第三行续\n"
        "[乙书]\tp=2\tt=1729990000\t乙的正文\n"
        "[甲书]\tp=4\tt=1725000000\t十月的一条\n"
        "[残片没有闭括号\n"
        "[丙书]\tp=1\t#7\t没校时的一条\n");
    for (int i = 0; i < 15; ++i) {
        at += (size_t)snprintf(big + at, sizeof(big) - at, "[甲书]\tp=%d\tt=%d\t第%d条\n",
                               i, 1730000100u + (unsigned)i * 60u, i);
    }
    at += (size_t)snprintf(big + at, sizeof(big) - at, "[乙书]\tp=9\tt=1729990500\t很长的一条[截]\n");
    write_file(big);

    unsigned count = 0;
    assert(excerpt_store_render_view(&count));
    assert(count == 20u); // 20 磁盘记录 + 0 待落盘；坏行不算（另有 N 条）
    const char *v = read_view();
    assert(strstr(v, "第1章\n"));
    assert(strstr(v, "第2章\n")); // 两个自然月 → 两章 / two months, two chapters
    assert(strstr(v, "另有 1 条无法识别。\n")); // 残片计数，不静默丢 / the fragment is counted
    assert(strstr(v, "《乙书》· 第 10 页 · ")); // 页码 +1：p=9 → 第 10 页 / page +1
    assert(strstr(v, "（摘录时已截断）"));
    assert(strstr(v, "未校时"));
    assert(strstr(v, "跨行第一条 第二行续 第三行续")); // 续行并入、空白折一 / merged, folded
    assert(!strstr(v, "残片没有闭括号"));             // 残片不进正文 / the fragment never renders
    assert(!strstr(v, "[截]"));                        // 标记换短语 / the marker becomes a phrase
    // 最新在前：1730001100（最后写入）出现在 1730000000 之前。
    // Newest first: the newest timestamp renders before the older one.
    const char *newest = strstr(v, "第 15 页");   // p=14 → 第 15 页（最新一条）
    const char *older = strstr(v, "跨行第一条");
    assert(newest && older && newest < older);
    struct stat st;
    assert(stat(EXCERPT_VIEW_TMP, &st) != 0 && "tmp must not survive a successful render");
    printf("  case_twenty_with_multiline ok\n");
}

static void case_pending_on_top(void) {
    fs_reset();
    // 队列里 2 条未落盘：渲染不改队列，待落盘节置顶。/ Two queued entries; peek only.
    assert(excerpt_store_enqueue("甲书", 0, 0u, "还没落盘的甲", NULL) == EXCERPT_OK);
    assert(excerpt_store_enqueue("乙书", 3, 1730000000u, "还没落盘的乙", NULL) == EXCERPT_OK);
    write_file("[甲书]\tp=1\tt=1725000000\t已在卡上的\n");

    unsigned count = 0;
    assert(excerpt_store_render_view(&count));
    assert(count == 1u + 2u);
    assert(excerpt_store_pending() == 2 && "peek must never dequeue");
    const char *v = read_view();
    assert(strstr(v, "〔待落盘 2 条 · 插卡后自动保存〕"));
    assert(strstr(v, "还没落盘的甲"));
    assert(strstr(v, "还没落盘的乙"));
    // 待落盘节在磁盘内容之前。/ The pending section sits above the stored records.
    const char *pend = strstr(v, "〔待落盘");
    const char *disk = strstr(v, "已在卡上的");
    assert(pend && disk && pend < disk);
    // 队列仍能正常冲账（渲染没有偷走任何一条）。/ The queue still flushes.
    assert(excerpt_store_tick(0u) == EXCERPT_FLUSH_OK);
    assert(excerpt_store_pending() == 1);
    printf("  case_pending_on_top ok\n");
}

static void case_empty_and_tmp_invariant(void) {
    fs_reset();
    unsigned count = 999;
    assert(excerpt_store_render_view(&count));
    assert(count == 0);
    const char *v = read_view();
    assert(strstr(v, "还没有摘抄"));
    struct stat st;
    assert(stat(EXCERPT_VIEW_TMP, &st) != 0);
    // 成品不变式：再渲染一次，成品仍在、内容一致。
    // The invariant: a second render leaves the finished file intact and identical.
    assert(excerpt_store_render_view(&count));
    assert(strcmp(read_view(), v) == 0);
    printf("  case_empty_and_tmp_invariant ok\n");
}

static void case_cap_note(void) {
    fs_reset();
    char line[96];
    FILE *f = fopen(EXCERPT_PATH, "w");
    assert(f);
    for (unsigned i = 0; i < 4005u; ++i) {
        const int n = snprintf(line, sizeof(line), "[甲书]\tp=%u\tt=%u\t第%u条\n",
                               i, 1730000000u + i, i);
        assert(n > 0 && fwrite(line, 1, (size_t)n, f) == (size_t)n);
    }
    assert(fclose(f) == 0);
    unsigned count = 0;
    assert(excerpt_store_render_view(&count));
    assert(count == 4000u);
    const char *v = read_view();
    assert(strstr(v, "（共 4005 条，仅显示最近 4000 条）"));
    printf("  case_cap_note ok\n");
}

int main(void) {
    case_twenty_with_multiline();
    case_pending_on_top();
    case_empty_and_tmp_invariant();
    case_cap_note();
    assert(test_psram_live == 0 && "render must not leak PSRAM");
    printf("excerpt_view host tests: all green\n");
    return 0;
}
