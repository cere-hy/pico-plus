/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：书标识 = 文件名 basename（去扩展名）的 FNV-1a32 哈希。
 * English: Book id = FNV-1a32 of the file's basename (extension stripped).
 *
 * 这是一个**跨端契约**：设备把它放进 0x17/0x06 上下文档（book_id），手机侧要按
 * **同一算法同一口径**再算一遍才能把查询对回书本（语境查询/生词归类）。口径不一致
 * 的表现是「设备算 A、手机算 B」→ 语境静默失效，且只有真机才现形，所以口径在这里
 * 钉死，并由三端向量对表守着（Python 参考实现、Java 对表、本模块的宿主测试）。
 * This is a **cross-end contract**: the device puts it in the contextual 0x17/0x06
 * frame and the phone must recompute it with the identical algorithm to match the
 * query back to the book. A rule mismatch means "device computes A, phone computes B"
 * -> context silently fails, and only real hardware reveals it. Hence the rules are
 * pinned here and guarded by vectors on all three ends.
 *
 * 口径 / Rules (frozen):
 *   1. basename = 最后一个 '/' 之后的全部字节（只认 '/'）。/ after the last '/'
 *   2. 扩展名 = basename 内最后一个 '.' 起（含）截断；若 '.' 是首字符（隐藏文件）
 *      则不截断。/ strip from the last '.' unless it is the first char
 *   3. 对 basename 的 **UTF-8 原始字节**做 FNV-1a32：offset basis 0x811C9DC5、
 *      prime 0x01000193。**不做**大小写折叠、**不做** Unicode 归一化。
 *      FNV-1a32 over the **raw UTF-8 bytes**: basis 0x811C9DC5, prime 0x01000193.
 *      **No** case folding, **no** Unicode normalisation.
 *   4. 结果 0 一律返回 1：0 保留为「无书上下文」的语义。/ A zero result becomes 1:
 *      0 means "no book context".
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/// FNV-1a32（原始字节）。/ FNV-1a32 over raw bytes.
uint32_t book_fnv1a32(const char *utf8, size_t len);

/// 路径 → 书标识；空路径返回 0。/ Path to book id; 0 for an empty path.
uint32_t book_id_from_path(const char *path);
