# -*- coding: utf-8 -*-
"""三端对表 · Python 端 runner（参考实现 push_pico.py）。

与 C `tri_c.c` / Java `TriVec.java` 同一套 9 个文件名（顺序一致），
用于确认三端 `book_id` 哈希逐字节全等。路径相对本脚本解析，仓库内可重跑。
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ANDROID = os.path.normpath(os.path.join(HERE, "..", "android"))  # tools/android
sys.path.insert(0, REPO_ANDROID)
import push_pico as pp  # 参考实现 / reference implementation

# 与 C / Java 运行器同一套 9 个文件名（顺序一致）。
# Same 9 filenames, same order, as the C and Java runners.
samples = [
    "/books/Hold On to Your Kids.epub",
    "/books/战争与和平.txt",
    "/books/The.Great.Gatsby.epub",
    "/books/note.tar.gz",
    "/books/README",
    "/books/UPPER.EPUB",
    "/books/upper.epub",
    "/books/.hidden",
    "/sd/books/中文 书名.epub",
]

for s in samples:
    print("0x%08x" % pp.book_id_from_path(s))
