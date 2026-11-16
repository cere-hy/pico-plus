/* SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：PSRAM 分配桩，带申请失败注入（细化账 2 的取证口）与泄漏计数（入队/释放必须配平）。
 * English: PSRAM allocator stub with failure injection (evidence for item two) and a leak
 * counter (every enqueue must be matched by a free).
 *
 * 26.11.2 P0-5：渲染路径补 calloc / realloc 两个口——泄漏计数口径不变（申请成功 +1、
 * free -1；realloc 失败旧块仍有效，调用方按 NULL 处置、旧块不记双账）。
 * 26.11.2 P0-5: the render path needs calloc/realloc too; the leak accounting is
 * unchanged (+1 on success, -1 on free; a failed realloc keeps the old block valid and
 * is the caller's problem, never double-counted).
 */
#pragma once
#include <stdlib.h>

#define MALLOC_CAP_SPIRAM 4
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2

extern bool test_psram_fail;   ///< 下一次申请返回 NULL / next allocation returns NULL
extern int test_psram_live;    ///< 在分配块数 / live allocations

static inline void *heap_caps_malloc(size_t n, int caps) {
    (void)caps;
    if (test_psram_fail) return NULL;
    void *p = malloc(n);
    if (p) ++test_psram_live;
    return p;
}
static inline void *heap_caps_calloc(size_t n, size_t size, int caps) {
    (void)caps;
    if (test_psram_fail) return NULL;
    void *p = calloc(n, size);
    if (p) ++test_psram_live;
    return p;
}
static inline void *heap_caps_realloc(void *old, size_t n, int caps) {
    (void)caps;
    if (test_psram_fail) return NULL;
    void *p = realloc(old, n);
    if (p && !old) ++test_psram_live;
    return p;
}
static inline void heap_caps_free(void *p) {
    if (p) { --test_psram_live; free(p); }
}
