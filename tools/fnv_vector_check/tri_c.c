#include "book_id.h"
#include <stdio.h>

/* 与 Java 测试 / Python 向量同一套 9 个文件名（顺序一致），用于三端全等对表。
 * Same 9 filenames, same order, as the Java test / Python vectors. */
static const char *samples[] = {
    "/books/Hold On to Your Kids.epub",
    "/books/\xe6\x88\x98\xe4\xba\x89\xe4\xb8\x8e\xe5\x92\x8c\xe5\xb9\xb3.txt",
    "/books/The.Great.Gatsby.epub",
    "/books/note.tar.gz",
    "/books/README",
    "/books/UPPER.EPUB",
    "/books/upper.epub",
    "/books/.hidden",
    "/sd/books/\xe4\xb8\xad\xe6\x96\x87 \xe4\xb9\xa6\xe5\x90\x8d.epub",
};

int main(void) {
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        printf("0x%08lx\n", (unsigned long)book_id_from_path(samples[i]));
    }
    return 0;
}
