/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：步 4 页号错位 A2「翻页即清」的纯逻辑内核（static inline，零依赖）。
 * 把「翻页成功 → has=false → 占位上屏」这条链从 turn_page 的大函数里抽出来，
 * 使 app_book.c 与宿主机 C 测试（tools/app_companion_state_host_test.c）调用的是
 * 同一份真实被调代码——14:26 裁定 §三守卫面 ④ 要求行为断言必带，不能用复制品。
 *
 * 语义正交（裁定 §二第 3 条要求登记）：既有三处 has 写入点
 *   companion_enter()=false（进伴读从占位起）/ companion_exit()=false（出伴读清）/
 *   companion_summary_poll()=true（收到整段摘要）；本处新增 turn_page() 成功后=false。
 * 四处职责互不覆盖：前两处管会话边界，poll 管收帧，turn_page 管翻页瞬间。
 *
 * English: pure-logic core for the A2 "clear on page turn" fix, shared verbatim
 * between app_book.c and the host test so guard 4 asserts the real called code.
 */
#ifndef APP_COMPANION_STATE_H
#define APP_COMPANION_STATE_H

#include <stdbool.h>
#include <stddef.h>

/* 占位三态文案（与步 4 冻结口径逐字一致，overlay 与宿主测试共用同一判据）。 */
#define COMPANION_TXT_WAIT_LINK "等待连接"
#define COMPANION_TXT_GENERATING "摘要生成中…"
#define COMPANION_TXT_NONE "本次未生成"

/* A2 核心：翻页成功后调用。清占位标记 + 清旧摘要缓冲首字节，使下一帧 overlay
   立刻显示「摘要生成中…」，绝不把上一页旧字留到新页框里。
   has / summary 允许 NULL（防御）；summary 有 cap 才写，绝不越界。 */
static inline void companion_turn_clear(bool *has, char *summary, size_t cap) {
    if (has) *has = false;
    if (summary && cap) summary[0] = 0;
}

/* 右栏三态占位：link 未连 → 等待连接；无 has → 生成中；has 但空串 → 本次未生成；
   否则返回摘要正文。overlay 真实调用本函数，宿主测试据此断言 A2 上屏结果。 */
static inline const char *companion_placeholder(bool link_up, bool has,
                                                 const char *summary) {
    if (!link_up) return COMPANION_TXT_WAIT_LINK;
    if (!has) return COMPANION_TXT_GENERATING;
    if (!summary || !summary[0]) return COMPANION_TXT_NONE;
    return summary;
}

#endif /* APP_COMPANION_STATE_H */
