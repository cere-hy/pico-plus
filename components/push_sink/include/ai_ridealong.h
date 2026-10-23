/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：搭车协议（ride-along）的离线队列。把「没连手机时点的查词」暂存在调用方
 * 提供的扁平缓冲里（设备侧 = PSRAM，宿主侧 = 堆），下次推送会话由手机批量回流
 * 释义。模块不分配内存、不依赖 ESP-IDF，故可在宿主用 ASan/UBSan 逐字节对表。
 * English: the ride-along protocol's offline queue. Words looked up while the
 * phone is disconnected are stashed in a caller-provided flat buffer (PSRAM on
 * device, heap on host); the next push session flushes them so the phone returns
 * the definitions. No allocation, no ESP-IDF: host-testable under ASan/UBSan.
 *
 * 布局（小端，单设备缓冲，字节序无关）：
 *   off 0  uint16  MAGIC (0x5241 'RA')
 *   off 2  uint16  cap（容量，单词数）
 *   off 4  uint16  count（当前条目数）
 *   off 6  条目[]：每条 = uint8 wlen + word[wlen]，紧凑排列。
 * Layout (little-endian, single-device buffer, endian-agnostic): a 6-byte header
 * then compact entries, each a uint8 length followed by the word bytes.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "push_proto.h" // PUSH_AI_WORD_MAX

#ifdef __cplusplus
extern "C" {
#endif

#define AI_RALOC_MAGIC 0x5241u ///< 'RA'

/// 容纳 cap 个单词所需的缓冲字节数。/ Bytes a store of cap words needs.
size_t ai_raloc_buf_size(uint16_t cap);

/// 在 buf 上初始化容量为 cap 的队列；缓冲太小返 false。已带合法 MAGIC 的缓冲
/// 当作续载（校验 cap 一致），否则清零新建。
/// Initialise a queue of capacity cap on buf; false if too small. A buffer
/// already carrying the magic is a resume (cap must match), else zeroed fresh.
bool ai_raloc_init(uint8_t *buf, size_t buf_len, uint16_t cap);

/// 容量 / count。非法缓冲返回 0。
/// Capacity / count. Zero on an invalid buffer.
uint16_t ai_raloc_cap(const uint8_t *buf);
uint16_t ai_raloc_count(const uint8_t *buf);

/// 入队一个单词（去重：已存在则不重复入队）。满或非法返 false。
/// Enqueue a word (deduped: a duplicate is not queued again). False if full/invalid.
bool ai_raloc_enqueue(uint8_t *buf, size_t buf_len, const char *word);

/// 窥视队首（不出队）。/ Peek the head without removing it.
bool ai_raloc_peek(const uint8_t *buf, char *word_out, size_t word_cap);

/// 出队队首。/ Dequeue the head.
bool ai_raloc_dequeue(uint8_t *buf, char *word_out, size_t word_cap);

/// 清空队列（保留容量）。/ Clear the queue (capacity preserved).
void ai_raloc_clear(uint8_t *buf);

#ifdef __cplusplus
}
#endif
