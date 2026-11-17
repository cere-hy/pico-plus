/*
 * dict_ai_arm.h — 词典卡「AI 键一键蓝牙武装」挂起态纯逻辑（零平台依赖）。
 *
 * 定版（公主 2026-11-17 谕令 26.11.31）：词典卡点 AI 键、尚未连接时，词条＋
 * 查询上下文入挂起缓存并启动蓝牙广播武装；连接建成（订阅事件）时把挂起词以
 * 既有 0x17 帧发出、随即清缓存防重发；广播超时 / 用户取消 / 翻页换书关卡通
 * 均取消挂起。此模块只管状态与缓存，不碰 BLE / NVS / 渲染，故可宿主单测。
 *
 * DICT_AI_ARM is the pure logic behind the "one-tap AI arming" flow: on an AI
 * tap while offline, stash the word + query context and arm BLE advertising;
 * on link-up (subscribe) the stashed word is handed out for the existing 0x17
 * frame and the slot is cleared to prevent a resend. Timeout / cancel / page
 * turn all drop the slot. Platform-free so the five paths are host-testable.
 */
#ifndef DICT_AI_ARM_H
#define DICT_AI_ARM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DICT_AI_ARM_WORD_MAX 64u
#define DICT_AI_ARM_WINDOW_MS 60000u  ///< 广播武装窗口 60 s / arming window

typedef struct {
    char     word[DICT_AI_ARM_WORD_MAX]; ///< 挂起词（NUL 结尾）/ stashed word
    uint32_t book_id;                    ///< 查询上下文 / book id
    uint32_t pos;                        ///< 查询上下文 / page offset
    int64_t  armed_at_ms;                ///< 武装时刻 / arming timestamp (ms)
    bool     active;                     ///< 是否有挂起词 / slot in use
} dict_ai_arm_t;

/// 清空挂起槽。/ Clear the slot.
void dict_ai_arm_reset(dict_ai_arm_t *st);

/// 武装并入缓存；空词拒绝；重复调用覆盖（幂等）。返回是否成功入缓存。
/// Arm and stash; empty word rejected; a repeat call overwrites (idempotent).
bool dict_ai_arm_arm(dict_ai_arm_t *st, const char *word, uint32_t book_id,
                     uint32_t pos, int64_t now_ms);

/// 是否有挂起词。/ Whether a word is stashed.
bool dict_ai_arm_active(const dict_ai_arm_t *st);

/// 窗口是否到期（window_ms==0 恒不到期）。/ Whether the window expired.
bool dict_ai_arm_expired(const dict_ai_arm_t *st, int64_t now_ms, uint32_t window_ms);

/// 连接建成时取走挂起词并清槽（防重发）；二次调用返回 false。
/// Hand out the stashed word on link-up and clear the slot (no resend).
bool dict_ai_arm_take(dict_ai_arm_t *st, char *word, size_t wcap,
                      uint32_t *book_id, uint32_t *pos);

/// 取消挂起（用户取消 / 翻页 / 换书 / 关卡通）；返回此前是否有效。
/// Cancel the stash; returns whether it was active.
bool dict_ai_arm_cancel(dict_ai_arm_t *st);

/// 挂起词只读指针（仅 active 时有效）。/ Read-only word pointer (valid if active).
const char *dict_ai_arm_word(const dict_ai_arm_t *st);

#endif /* DICT_AI_ARM_H */
