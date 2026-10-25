/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：TTS 听书的页文本抽取与 NVS 开关（gen26.5 M2 Step10）。纯逻辑 + 一个 u8
 * 开关：把「当前视觉页在章内的字节区间」变成可直接朗读的 UTF-8 文本——剔除图片
 * 占位符 U+FFFC（html_text.c 的 add_image 用它占位，TTS 引擎读了只会出怪音），
 * 修剪尾随空白；帧切分在 push_tts.c，空口投递在 push_sink.c，本模块只负责
 * 「一页文字」这一棒。宿测先行：tools/tts_page_host_test.c。
 * English: TTS read-aloud page-text extraction and its NVS switch (gen26.5 M2
 * Step10). Pure logic plus one u8 switch: turn "the current visual page's byte
 * range inside the chapter" into speakable UTF-8 - drop the image placeholder
 * U+FFFC (what html_text.c's add_image emits; a TTS engine would only make
 * noise of it) and trim trailing blanks. Frame planning is push_tts.c's job and
 * air delivery push_sink.c's; this module owns "one page of words" only.
 * Host-first: tools/tts_page_host_test.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// 抽取输出槽上限（含结尾 0）。一页文本的常见量级在几百字节；竖排/大字号下
/// 一视觉页也就一章的几十分之一。超长章按帧预算自然分段，槽只需容纳「一页」。
/// Extraction output cap (NUL included). A page is a few hundred bytes typically;
/// oversized chapters segment naturally by frame budget, so the slot only needs
/// to hold one page.
#define TTS_PAGE_TEXT_MAX 2048u

/// 从章文本的字节区间 [start, end) 抽取可朗读文本：剔除 U+FFFC（EF BF BC）图片
/// 占位、修剪首尾空白（首空白也修——段首缩进在 TTS 里只是杂音）。返回有效字节数
/// （0 = 该页无可朗读文本，如纯图页）。区间非法（start > end 超界）返回 0。
/// Extract speakable text from the byte range [start, end): drop U+FFFC (EF BF BC)
/// image placeholders and trim leading/trailing blanks (leading too - first-line
/// indent is noise for TTS). Returns valid bytes (0 = nothing to speak, e.g. an
/// image-only page). An invalid range returns 0.
size_t tts_page_extract(const char *text, size_t text_len, size_t start, size_t end,
                        char *out, size_t cap);

/// 开机读一次 NVS 开关（namespace "rp_tts"、键 "enabled"、u8）。**缺省关**：
/// 读不到/读失败/怪值都按关——听书必须用户显式开启（与 rp_vocab/autosave 的
/// 缺省开相反：那边是静默记账，这边是手机开口发声，不可默认代用户决定）。
/// Read the NVS switch once at boot (namespace "rp_tts", key "enabled", u8).
/// **Off by default**: a missing read, a failed read or an odd value all count as
/// off - read-aloud must be explicitly enabled (the opposite of rp_vocab/autosave:
/// that one silently logs, this one makes the phone speak).
void tts_switch_init(void);

/// 当前朗读开关（NVS 持久化；app_book 的跟随轮询读它）。/ The read-aloud switch.
bool tts_switch_on(void);

/// 写开关（NVS 落盘 + 运行期更新）。朗读入口的开关卡调它。/ Set the switch (NVS write).
void tts_switch_set(bool on);

#ifdef __cplusplus
}
#endif
