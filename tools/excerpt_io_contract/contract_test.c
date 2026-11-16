/* SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：IO 契约用例（26.9.29 新增，常驻门禁）。桩层测逻辑，这一层测**桩与真系统之间的
 * 契约**——那正是 26.9.28 事故唯一活下来的地方：
 *   A. 裸 FatFs 只认盘符词汇：`"/sdcard/..."` 恒 `FR_NO_PATH`；
 *   B. `f_open` 的失败清理**真的**会去释放 `fp->buf` —— 栈上 `FIL` 不清零就是野指针释放；
 *   C. 清零 `FIL` + `"0:/..."` 才能写通（VFS 尾巴依赖的形态）；
 *   D. 错误码数值冻结，IDF 改号会当场变红。
 * English: the IO contract tests. These pin the contract between our code and the real
 * filesystem - the only place the 26.9.28 defect survived the stub layer.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ff.h"

/* 错误码数值冻结：IDF 升级若改号，这里先红，而不是等到现场日志里对不上。*/
_Static_assert(FR_OK == 0, "contract: FR_OK");
_Static_assert(FR_NO_PATH == 5, "contract: FR_NO_PATH");
_Static_assert(FR_NO_FILESYSTEM == 13, "contract: FR_NO_FILESYSTEM");
_Static_assert(FR_MKFS_ABORTED == 14, "contract: FR_MKFS_ABORTED");
_Static_assert(FF_USE_DYN_BUFFER == 1, "contract: the device runs with dynamic buffers");

extern void *g_wild_ptr;
extern int g_wild_count;

static FATFS g_fs;

static void mkfs_mount(void) {
    static BYTE work[4096];
    MKFS_PARM opt = {FM_FAT | FM_SFD, 0, 0, 0, 0};
    const FRESULT r = f_mkfs("0:", &opt, work, sizeof work);
    assert(r == FR_OK);
    assert(f_mount(&g_fs, "0:", 1) == FR_OK);
}

/* A：VFS 挂载点路径给裸 FatFs = 恒 FR_NO_PATH（26.9.28 的现场原因）。
 *    注意它**不是**「非法路径」，而是「卡根下没有叫 sdcard 的目录」——所以 f_mkdir
 *    甚至愿意替你创建一个：真有人这么写过一次，用户卡根上就会多出一个空的 sdcard/。
 *    A: a VFS mount-point path is not "invalid" to bare FatFs, it is merely a directory
 *    name that does not exist yet - and mkdir would happily create it. */
static void case_vfs_path_is_foreign(void) {
    /* 26.9.28 现场的第一句就是这一行：父目录 "/sdcard" 不存在 → FR_NO_PATH，
     * 而返回值没人看，于是目录等于没建、后面每一步都在错的前提上继续跑。*/
    assert(f_mkdir("/sdcard/.readpico") == FR_NO_PATH);

    FIL f;
    memset(&f, 0, sizeof f);
    assert(f_open(&f, "/sdcard/.readpico/excerpts.txt", FA_WRITE | FA_OPEN_APPEND) == FR_NO_PATH);

    /* 更坏的一面：裸层把 "/sdcard" 当成卡根下一个可创建的目录。*/
    assert(f_mkdir("/sdcard") == FR_OK);
    printf("  A vfs-path-is-foreign ok (mkdir(parent) = FR_NO_PATH; /sdcard is just a name)\n");
}

/* B：`f_open` 早失败 → 失败清理对 `fp->buf` 调 ff_memfree。未清零的 FIL 就是野指针释放。
 *    26.9.28 的 `assert failed: heap_caps_free ... free() target pointer is outside heap areas`
 *    说的就是这一行——这里用「陌生指针计数」把它钉在宿主上，不必靠一次真崩溃来证明。*/
static void case_failed_open_frees_fp_buf(void) {
    static int sentinel;
    FIL f;
    memset(&f, 0, sizeof f);
    f.buf = (void *)&sentinel; /* 模拟栈上的残留 / pretend the stack left something here */
    g_wild_count = 0;
    g_wild_ptr = NULL;

    const FRESULT r = f_open(&f, "/sdcard/.readpico/excerpts.txt", FA_WRITE | FA_OPEN_APPEND);
    assert(r == FR_NO_PATH);
    assert(g_wild_count == 1);              /* 真的 free 了 fp->buf / it really frees fp->buf */
    assert(g_wild_ptr == (void *)&sentinel); /* 而且 free 的正是那个残留 / and it is that value */
    assert(f.buf == NULL);                  /* 清理后会置空 / cleared afterwards */
    printf("  B failed-open-frees-fp-buf ok (wild frees=%d)\n", g_wild_count);

    /* 清零的 FIL 走同一条失败路径：绝不产生陌生释放（这就是修法的最小充分条件）。*/
    memset(&f, 0, sizeof f);
    g_wild_count = 0;
    assert(f_open(&f, "/sdcard/.readpico/excerpts.txt", FA_WRITE | FA_OPEN_APPEND) == FR_NO_PATH);
    assert(g_wild_count == 0);
    printf("  B zeroed-fil-no-wild-free ok\n");
}

/* C：清零 FIL + 盘符路径，才走得通（VFS 尾巴依赖的形态）。*/
static void case_drive_prefix_works(void) {
    assert(f_mkdir("0:/.readpico") == FR_OK);
    FIL f;
    memset(&f, 0, sizeof f);
    assert(f_open(&f, "0:/.readpico/excerpts.txt", FA_WRITE | FA_OPEN_APPEND) == FR_OK);
    const char *line = "[契约]\tp=1\t#1\t一行\n";
    UINT wr = 0;
    assert(f_write(&f, line, (UINT)strlen(line), &wr) == FR_OK);
    assert(wr == (UINT)strlen(line));
    assert(f_sync(&f) == FR_OK);
    assert(f_close(&f) == FR_OK);

    memset(&f, 0, sizeof f);
    assert(f_open(&f, "0:/.readpico/excerpts.txt", FA_READ) == FR_OK);
    char buf[128] = {0};
    UINT rd = 0;
    assert(f_read(&f, buf, sizeof buf - 1, &rd) == FR_OK);
    assert(rd == (UINT)strlen(line));
    assert(strcmp(buf, line) == 0);
    assert(f_close(&f) == FR_OK);
    printf("  C drive-prefix-works ok (%u B read back)\n", (unsigned)rd);
}

int main(void) {
    printf("excerpt IO contract tests (device-identical FatFs on a RAM disk)\n");
    mkfs_mount();
    case_vfs_path_is_foreign();
    case_failed_open_frees_fp_buf();
    case_drive_prefix_works();
    printf("excerpt IO contract tests: all green\n");
    return 0;
}
