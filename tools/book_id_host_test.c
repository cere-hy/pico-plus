/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：书标识（跨端契约）的宿主测试。算法用 FNV-1a32 标准向量自证；basename 规则
 * 用「等价性结构断言」钉住（不手抄哈希值——哈希值由 Python 参考实现产出、Java 用
 * 向量对表，C 侧只保证算法与规则本身正确，跨端一致性由同口径 + 那两端对表守）。
 * English: Host test for the book id (cross-end contract). The algorithm is proven by
 * standard FNV-1a32 vectors; the basename rule is pinned by structural equivalence
 * assertions (no hand-transcribed hashes - hashes come from the Python reference and
 * the Java side table-checks them against vectors; the C side proves the algorithm and
 * the rule itself, cross-end agreement rests on the shared口径 plus those two checks).
 */
#include "book_id.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;
static int checks = 0;

static void CHECK(int cond, const char *msg) {
    checks++;
    if (!cond) {
        fails++;
        printf("  [FAIL] %s\n", msg);
    }
}

int main(void) {
    // 1) 算法：FNV-1a32 标准向量（公认值，非本项目自造）。
    // Algorithm: well-known FNV-1a32 vectors (not invented here).
    CHECK(book_fnv1a32("", 0) == 0x811C9DC5u, "fnv1a32(empty) == 0x811C9DC5");
    CHECK(book_fnv1a32("a", 1) == 0xE40C292Cu, "fnv1a32(\"a\") == 0xE40C292C");
    CHECK(book_fnv1a32("foobar", 6) == 0xBF9CF968u, "fnv1a32(\"foobar\") == 0xBF9CF968");
    CHECK(book_fnv1a32(NULL, 3) == 0x811C9DC5u, "fnv1a32(NULL) == basis（不崩）");

    // 2) 规则：路径 → basename（去扩展名），用等价性断言而非哈希值。
    // Rule: path -> basename (extension stripped), asserted structurally.
    CHECK(book_id_from_path("/books/a/b/name.epub") == book_fnv1a32("name", 4),
          "路径取 basename 并去扩展名（与直接算 \"name\" 等价）");
    CHECK(book_id_from_path("name.epub") == book_id_from_path("/x/y/name.epub"),
          "同名同 basename 与所在目录无关");
    CHECK(book_id_from_path("The.Great.Gatsby.epub") == book_fnv1a32("The.Great.Gatsby", 16),
          "多点文件名只去最后一个扩展名");
    CHECK(book_id_from_path("note.tar.gz") == book_fnv1a32("note.tar", 8),
          "多扩展名（.tar.gz）只去最后一个");
    CHECK(book_id_from_path("README") == book_fnv1a32("README", 6),
          "无扩展名原样哈希");
    CHECK(book_id_from_path("/.hidden") == book_fnv1a32(".hidden", 7),
          "首字符为 '.' 的隐藏文件不切扩展名");
    // 大小写不做折叠：两条仅大小写不同的路径必须得到不同 id。
    // No case folding: paths differing only in case must differ.
    CHECK(book_id_from_path("UPPER.EPUB") != book_id_from_path("upper.epub"),
          "大小写不折叠（UPPER != upper）");
    // UTF-8 原始字节：中文与空格都进哈希。/ Raw UTF-8 bytes: CJK and spaces included.
    CHECK(book_id_from_path("/books/战争与和平.txt") == book_fnv1a32("战争与和平", strlen("战争与和平")),
          "中文书名按 UTF-8 原始字节哈希");
    CHECK(book_id_from_path("/books/Hold On to Your Kids.epub")
              == book_fnv1a32("Hold On to Your Kids", 20),
          "带空格书名按原样哈希");

    // 3) 边界：空路径/裸斜杠 → 0（无书上下文）；0 结果保底为 1。
    // Boundaries: empty path / bare slash -> 0 (no book context); a zero hash becomes 1.
    CHECK(book_id_from_path(NULL) == 0u, "NULL 路径 == 0");
    CHECK(book_id_from_path("") == 0u, "空路径 == 0");

    // 4) 打印典型值，供与 Python/Java 参考实现人工对表（真机前的最后一道肉眼闸）。
    // Print typical values for a manual cross-check against Python/Java.
    static const char *samples[] = {
        "/books/Hold On to Your Kids.epub", "/books/战争与和平.txt",
        "/books/The.Great.Gatsby.epub", "/books/note.tar.gz", "/books/README",
        "/books/UPPER.EPUB", "/books/upper.epub",
    };
    printf("book_id samples (C):\n");
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        printf("  %-36s -> 0x%08lx\n", samples[i], (unsigned long)book_id_from_path(samples[i]));
    }

    printf("book id: %d checks, %d red\n", checks, fails);
    return fails == 0 ? 0 : 1;
}
