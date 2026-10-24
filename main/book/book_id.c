/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：书标识（跨端契约）的实现；纯逻辑、无 ESP-IDF 依赖，宿主可直接编译测试。
 * English: Book id (cross-end contract) implementation; pure logic with no ESP-IDF
 * dependency, so the host can compile and test it directly.
 */
#include "book_id.h"

#include <string.h>

#define BOOK_FNV_BASIS 2166136261u /* 0x811C9DC5 */
#define BOOK_FNV_PRIME 16777619u   /* 0x01000193 */

uint32_t book_fnv1a32(const char *utf8, size_t len) {
    uint32_t h = BOOK_FNV_BASIS;
    if (!utf8) return h;
    for (size_t i = 0; i < len; ++i) {
        h ^= (uint8_t)utf8[i];
        h *= BOOK_FNV_PRIME;
    }
    return h;
}

uint32_t book_id_from_path(const char *path) {
    if (!path || !*path) return 0u; // 0 = 无书上下文 / no book context
    const char *base = path;
    for (const char *p = path; *p; ++p) {
        if (*p == '/') base = p + 1; // 只认 '/' / only '/'
    }
    const char *dot = NULL;
    for (const char *p = base; *p; ++p) {
        if (*p == '.') dot = p;      // 最后一个 '.' / the last dot
    }
    size_t n = strlen(base);
    if (dot && dot > base) n = (size_t)(dot - base); // 首字符为 '.' 的隐藏文件不切
    uint32_t id = book_fnv1a32(base, n);
    return id ? id : 1u;             // 0 保留 / 0 is reserved
}
