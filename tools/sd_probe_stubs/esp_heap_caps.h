#pragma once
#include <stdlib.h>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_DMA 2
// 26.9.22 S23(a)：模拟「8 KB 大块切下去就拼不回来」的真实行为——只要有人向内部堆要
// 8 KB 以上，最大连续块就掉到 14,336，之后不再回到 18,432。静态缓冲不经过这里，
// 所以扫描前后的最大块必须相等；谁把缓冲改回堆，这个数会当场掉下去。
// 26.9.22 S23(a): model the real "an 8 KB split never merges back" behaviour. Any
// internal allocation of 8 KB or more drops the largest block to 14,336 for good.
// Static storage never passes through here, so the value must survive a scan.
static size_t test_largest_internal = 18432;
static inline void *heap_caps_malloc(size_t n, int caps) {
    (void)caps;
    if (n >= 8192 && test_largest_internal > 14336) test_largest_internal = 14336;
    return malloc(n);
}
static inline void heap_caps_free(void *p) {free(p);}
static inline size_t heap_caps_get_largest_free_block(int caps) {
    (void)caps; return test_largest_internal;
}
