/* SPDX-License-Identifier: Apache-2.0
 * 堆桩：宿主上 PSRAM/内部 RAM 都落到 malloc，容量不设限——门 1 量的是宽度，不是内存。
 * Heap shim: both regions land on malloc; the gate measures advances, not memory.
 */
#pragma once
#include <stddef.h>

#define MALLOC_CAP_SPIRAM (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_DMA (1 << 3)

void *heap_caps_malloc(size_t size, unsigned caps);
void *heap_caps_calloc(size_t n, size_t size, unsigned caps);
void *heap_caps_realloc(void *p, size_t size, unsigned caps);
void heap_caps_free(void *p);
size_t heap_caps_get_largest_free_block(unsigned caps);
