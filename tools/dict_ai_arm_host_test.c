/*
 * SPDX-FileCopyrightText 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 词典卡 AI 键武装挂起态 · 纯逻辑宿主测试（零 stub、零平台依赖）。
 * 审计 26.11.31 谕令 §三.3 五路径：武装 / 超时 / 取消 / 连接即发 / 防重发。
 * Plus boundary guards: empty word, 60s window edges, clock rollback, overwrite.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "dict_ai_arm.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (cond) {                                                             \
            ++g_pass;                                                           \
        } else {                                                                \
            ++g_fail;                                                           \
            printf("FAIL: %s  (line %d)\n", (msg), __LINE__);                   \
        }                                                                       \
    } while (0)

int main(void) {
    dict_ai_arm_t st;

    /* ---- 路径 1：武装（arm → active，词/上下文/时刻落位）---- */
    dict_ai_arm_reset(&st);
    CHECK(!dict_ai_arm_active(&st), "reset -> inactive");
    CHECK(dict_ai_arm_arm(&st, "conditions", 0x11223344u, 512u, 1000), "arm ok");
    CHECK(dict_ai_arm_active(&st), "arm -> active");
    CHECK(strcmp(dict_ai_arm_word(&st), "conditions") == 0, "armed word matches");
    CHECK(st.book_id == 0x11223344u, "armed book_id matches");
    CHECK(st.pos == 512u, "armed pos matches");
    CHECK(st.armed_at_ms == 1000, "armed timestamp matches");

    /* 幂等：重复 arm 覆盖，不产生多份 */
    CHECK(dict_ai_arm_arm(&st, "second", 7u, 8u, 2000), "re-arm ok");
    CHECK(strcmp(dict_ai_arm_word(&st), "second") == 0, "re-arm overwrites word");
    CHECK(st.book_id == 7u && st.pos == 8u, "re-arm overwrites context");
    CHECK(st.armed_at_ms == 2000, "re-arm refreshes timestamp");

    /* 空词拒绝 */
    CHECK(!dict_ai_arm_arm(&st, "", 0u, 0u, 0), "empty word rejected");

    /* 超长词截断保 NUL（缓冲边界） */
    {
        char big[DICT_AI_ARM_WORD_MAX + 16];
        memset(big, 'x', sizeof(big));
        big[sizeof(big) - 1] = '\0';
        CHECK(dict_ai_arm_arm(&st, big, 0u, 0u, 0), "overlong word arm ok");
        CHECK(strlen(dict_ai_arm_word(&st)) == DICT_AI_ARM_WORD_MAX - 1,
              "overlong word truncated with NUL");
    }

    /* ---- 路径 2：超时（60s 窗口；边界严格 >=；window=0 恒不到期；时钟回拨不误判）---- */
    dict_ai_arm_reset(&st);
    CHECK(dict_ai_arm_arm(&st, "word", 1u, 2u, 10000), "arm for timeout");
    CHECK(!dict_ai_arm_expired(&st, 69999, DICT_AI_ARM_WINDOW_MS), "at 59999ms not expired");
    CHECK(!dict_ai_arm_expired(&st, 70000 - 1, DICT_AI_ARM_WINDOW_MS), "just before 60s not expired");
    CHECK(dict_ai_arm_expired(&st, 70000, DICT_AI_ARM_WINDOW_MS), "at 60s expired (>=)");
    CHECK(dict_ai_arm_expired(&st, 99999, DICT_AI_ARM_WINDOW_MS), "beyond 60s expired");
    CHECK(!dict_ai_arm_expired(&st, 99999, 0u), "window=0 never expires");
    CHECK(!dict_ai_arm_expired(&st, 5000, DICT_AI_ARM_WINDOW_MS), "clock rollback not expired");
    dict_ai_arm_reset(&st);
    CHECK(!dict_ai_arm_expired(&st, 99999, DICT_AI_ARM_WINDOW_MS), "inactive never expires");

    /* ---- 路径 3：取消（cancel → 非 active；二次 cancel false）---- */
    dict_ai_arm_reset(&st);
    CHECK(dict_ai_arm_arm(&st, "cancelme", 3u, 4u, 0), "arm for cancel");
    CHECK(dict_ai_arm_cancel(&st), "cancel returns true when active");
    CHECK(!dict_ai_arm_active(&st), "cancel -> inactive");
    CHECK(!dict_ai_arm_cancel(&st), "second cancel returns false");
    CHECK(dict_ai_arm_word(&st) == NULL, "cancelled word pointer NULL");

    /* ---- 路径 4：连接即发（take → 词/上下文交付、槽清空）---- */
    dict_ai_arm_reset(&st);
    CHECK(dict_ai_arm_arm(&st, "associated", 0xaabbccddu, 4096u, 500), "arm for take");
    {
        char out[DICT_AI_ARM_WORD_MAX];
        uint32_t bid = 0, pos = 0;
        memset(out, 0, sizeof(out));
        CHECK(dict_ai_arm_take(&st, out, sizeof(out), &bid, &pos), "take returns true");
        CHECK(strcmp(out, "associated") == 0, "taken word matches");
        CHECK(bid == 0xaabbccddu && pos == 4096u, "taken context matches");
        CHECK(!dict_ai_arm_active(&st), "take clears slot");
    }

    /* ---- 路径 5：防重发（二次 take false；take 后 cancel/expire 均 false）---- */
    CHECK(!dict_ai_arm_take(&st, NULL, 0, NULL, NULL), "second take returns false (no resend)");
    CHECK(!dict_ai_arm_cancel(&st), "cancel after take returns false");
    CHECK(!dict_ai_arm_expired(&st, 999999, DICT_AI_ARM_WINDOW_MS), "expire after take false");

    /* take 的空指针/窄缓冲不崩且保语义 */
    dict_ai_arm_reset(&st);
    CHECK(dict_ai_arm_arm(&st, "abcd", 0u, 0u, 0), "arm for narrow take");
    CHECK(dict_ai_arm_take(&st, NULL, 0, NULL, NULL), "take with null out still consumes");
    CHECK(!dict_ai_arm_active(&st), "null-out take clears slot");

    /* take 无输出缓冲但取上下文 */
    dict_ai_arm_reset(&st);
    CHECK(dict_ai_arm_arm(&st, "ctx", 0x1234u, 0x5678u, 0), "arm for ctx-only take");
    {
        uint32_t bid = 0, pos = 0;
        CHECK(dict_ai_arm_take(&st, NULL, 0, &bid, &pos), "ctx-only take true");
        CHECK(bid == 0x1234u && pos == 0x5678u, "ctx-only take context matches");
    }

    /* ---- 路径 6（真机第⑧格修复·方案 A）：双锚点幂等 ----
     * SUBSCRIBE 用 peek 试发：词交付但槽保留 → 0x09 用 take 补发并清槽 →
     * 0x07 回流的 cancel 变 no-op（不重复查词）。 */
    dict_ai_arm_reset(&st);
    CHECK(dict_ai_arm_arm(&st, "dissuade", 0xdeadbeefu, 2048u, 1000), "arm for dual-anchor");
    {
        char p[DICT_AI_ARM_WORD_MAX];
        uint32_t bid = 0, pos = 0;
        /* SUBSCRIBE peek：交付但不清槽 */
        CHECK(dict_ai_arm_peek(&st, p, sizeof(p), &bid, &pos), "subscribe peek true");
        CHECK(strcmp(p, "dissuade") == 0, "peek word matches");
        CHECK(bid == 0xdeadbeefu && pos == 2048u, "peek context matches");
        CHECK(dict_ai_arm_active(&st), "peek KEEPS slot (vs take)");
        CHECK(dict_ai_arm_peek(&st, p, sizeof(p), &bid, &pos), "second peek still true");
        /* 0x09 take：补发并清槽 */
        memset(p, 0, sizeof(p)); bid = pos = 0;
        CHECK(dict_ai_arm_take(&st, p, sizeof(p), &bid, &pos), "caps take true");
        CHECK(strcmp(p, "dissuade") == 0, "caps take word matches");
        CHECK(bid == 0xdeadbeefu && pos == 2048u, "caps take context matches");
        CHECK(!dict_ai_arm_active(&st), "caps take clears slot");
        /* 0x07 回流 cancel：no-op，不重复 */
        CHECK(!dict_ai_arm_cancel(&st), "0x07 cancel after caps-take is no-op");
        CHECK(!dict_ai_arm_peek(&st, NULL, 0, NULL, NULL), "peek after cleared false");
    }
    /* 另一条收敛序：SUBSCRIBE peek 侥幸奏效 → 0x07 cancel 清槽 → 0x09 take no-op */
    dict_ai_arm_reset(&st);
    CHECK(dict_ai_arm_arm(&st, "lucid", 9u, 9u, 0), "arm for ack-first order");
    CHECK(dict_ai_arm_peek(&st, NULL, 0, NULL, NULL), "peek before ack");
    CHECK(dict_ai_arm_cancel(&st), "0x07 ack cancels slot");
    CHECK(!dict_ai_arm_take(&st, NULL, 0, NULL, NULL), "later 0x09 take no-op after ack (no resend)");

    printf("DICT_AI_ARM_PASS checks=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
