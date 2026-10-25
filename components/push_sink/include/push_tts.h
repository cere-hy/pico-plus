/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：TTS 页文本的帧切分计划（gen26.5 STATUS 0x19）。纯 C11，不依赖 ESP-IDF——
 * 只回答「一页文本切成几帧、每帧切哪段」，字节序与帧编码在 push_proto.c，
 * 空口投递在 push_sink.c。切分规则只有一条硬约束：**不截断 UTF-8 多字节序列**
 * （编码方保证字符边界，0x19 登记块的原话）；图片占位等域内过滤属调用方
 * （设备侧 main/tts/tts_page.c），本模块眼里只有字节。
 * English: the frame plan for TTS page text (gen26.5 STATUS 0x19). Pure C11 with
 * no ESP-IDF dependency: it only answers "how many frames, which slice each",
 * while byte order and frame encoding live in push_proto.c and air delivery in
 * push_sink.c. The one hard rule: never cut a multi-byte UTF-8 sequence (the
 * encoder owns character boundaries, per the 0x19 registry); domain filtering
 * (image placeholders etc.) belongs to the caller (main/tts/tts_page.c on the
 * device) - this module sees bytes only.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "push_proto.h" // PUSH_TTS_TEXT_MAX（0x19 帧槽）/ the 0x19 frame slot

#ifdef __cplusplus
extern "C" {
#endif

/// 每帧有效字节预算（页文本槽 234 保 NUL，故 ≤233）。/ Valid-byte budget per frame.
#define PUSH_TTS_FRAME_BUDGET (PUSH_TTS_TEXT_MAX - 1u)

/// 一页文本按预算切成几帧（total ≥ 1；空文本 = 1 帧 0 字节，由调用方决定发不发）。
/// How many frames a page splits into (total >= 1; empty text = one 0-byte frame,
/// and whether to send it is the caller's call).
size_t push_tts_frame_count(const char *text, size_t len);

/// 取第 i 帧的切片（指针 + 长度；i 越界返回 false）。切片恒不截断 UTF-8 序列，
/// 且恒 ≤ PUSH_TTS_FRAME_BUDGET。空文本的第 0 帧是 (ptr, 0)。
/// Take frame i's slice (pointer + length; false when i is out of range). The
/// slice never cuts a UTF-8 sequence and never exceeds the budget.
bool push_tts_frame_at(const char *text, size_t len, size_t i,
                       const char **slice, size_t *slice_len);

/// UTF-8 安全前进：从 s[0] 起前进不超过 budget 字节，停在完整字符边界
/// （续字节 10xxxxxx 不计入新字符起点）。返回前进字节数（0 = 空或首字节即续字节）。
/// UTF-8 safe advance: walk at most budget bytes from s[0], stopping on a
/// character boundary (a continuation byte 10xxxxxx never starts a character).
size_t push_tts_utf8_advance(const char *s, size_t len, size_t budget);

#ifdef __cplusplus
}
#endif
