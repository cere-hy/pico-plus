/* 中文：PSRAM 分配替身：宿主直接走 libc。/ English: PSRAM allocation shim: the host goes straight to libc. */
#pragma once
#include "common.h"

#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0

static inline void* heap_caps_malloc(size_t n, int caps) { (void)caps; return malloc(n); }
static inline void* heap_caps_calloc(size_t n, size_t sz, int caps) { (void)caps; return calloc(n, sz); }
