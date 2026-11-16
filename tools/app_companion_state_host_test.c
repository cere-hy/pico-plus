/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：步 4 A2「翻页即清」的宿主机守卫断言（14:26 裁定 §三守卫面 ④）。
 * include 的是 app_book.c 同一份 app_companion_state.h —— 测的是真实被调代码，
 * 不是复制品。零 stub、零平台依赖，独立二进制（不并进带 KNOWN-FAIL 的 ui_kit）。
 *
 * 守卫 ④：翻页成功 → has=false → 「摘要生成中…」占位上屏。
 *
 * English: host guard for A2 "clear on page turn"; includes the exact header
 * app_book.c calls, so the assert exercises the real code, not a copy.
 */
#include <stdio.h>
#include <string.h>
#include "app_companion_state.h"

static int pass = 0, fail = 0;

static void check(int cond, const char *msg) {
    if (cond) { pass++; printf("  [PASS] %s\n", msg); }
    else      { fail++; printf("  [FAIL] %s\n", msg); }
}

int main(void) {
    printf("app_companion_state_host_test（A2 翻页即清，纯逻辑）:\n");

    /* 守卫 ④ 主链：翻页前右栏正显示上一页摘要（has=true，缓冲有旧字）。 */
    bool has = true;
    char summary[64];
    strcpy(summary, "十五岁的艾米丽自残——上一页旧摘要");
    check(strcmp(companion_placeholder(true, has, summary), summary) == 0,
            "翻页前：has=true 显示上一页摘要正文");

    /* 翻页成功 → companion_turn_clear → has=false、旧缓冲清空。 */
    companion_turn_clear(&has, summary, sizeof(summary));
    check(has == false, "A2 翻页成功 → has=false");
    check(summary[0] == 0, "A2 翻页成功 → 旧摘要缓冲首字节清空（旧字不残留）");

    /* → 占位上屏：三态判据返回「摘要生成中…」（与 overlay 同一函数）。 */
    check(strcmp(companion_placeholder(true, has, summary),
                 COMPANION_TXT_GENERATING) == 0,
            "A2 → 「摘要生成中…」占位上屏（守卫 ④ 闭环）");

    /* 三态其余两态 + 优先级，确认清占位没改坏既有冻结口径。 */
    check(strcmp(companion_placeholder(false, true, "任意摘要"),
                 COMPANION_TXT_WAIT_LINK) == 0,
            "断链态优先：等待连接（link_down 盖过 has）");
    has = true; summary[0] = 0;
    check(strcmp(companion_placeholder(true, has, summary),
                 COMPANION_TXT_NONE) == 0,
            "has=true 但空摘要 → 本次未生成");
    strcpy(summary, "新页摘要到了");
    check(strcmp(companion_placeholder(true, has, summary), "新页摘要到了") == 0,
            "has=true 有摘要 → 显示正文（poll 收帧后）");

    /* 边界防御：NULL / cap=0 不崩、不越界。用 ASCII 以免多字节字符常量
       触发 -Werror=multichar / 缓冲区溢出告警。 */
    has = true;
    companion_turn_clear(NULL, NULL, 0);
    companion_turn_clear(&has, NULL, 0);
    check(has == false, "turn_clear NULL summary 仍清 has");
    char tiny[8]; strcpy(tiny, "abc");
    companion_turn_clear(NULL, tiny, 0);
    check(tiny[0] == 'a', "cap=0 不写缓冲（no out-of-bounds）");
    companion_turn_clear(&has, tiny, sizeof(tiny));
    check(tiny[0] == 0, "cap>0 正常清首字节");

    /* 幂等 + 与 enter/exit/poll 语义正交：多次清空结果稳定。 */
    has = true; strcpy(summary, "X");
    companion_turn_clear(&has, summary, sizeof(summary));
    companion_turn_clear(&has, summary, sizeof(summary));
    check(has == false && summary[0] == 0
            && strcmp(companion_placeholder(true, has, summary),
                      COMPANION_TXT_GENERATING) == 0,
            "重复翻页清空幂等，稳定停在生成中…");

    printf("app_companion_state_host_test 结果 / result: %d checks, %d failed\n",
           pass, fail);
    return fail != 0 ? 1 : 0;
}
