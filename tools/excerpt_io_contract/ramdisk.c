/* SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：契约用例的 RAM 盘与分配器。分配器**记录**每一块自己发出的内存，`ff_memfree`
 * 只释放认识的那几块；陌生指针记一笔 `wild` 而不交给 `free()`——这样契约用例可以
 * 在「不崩」的前提下证明 FatFs 真的会去释放 `fp->buf`，而不必靠一段会死的进程来说明。
 * English: RAM disk and allocator for the contract tests. The allocator remembers every
 * block it handed out; `ff_memfree` only frees what it recognises and records anything else
 * as `wild` instead of calling free(). That lets the test prove FatFs really frees `fp->buf`
 * without having to demonstrate it by dying.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "diskio.h"

#define SECT 512u
#define NSECT (16u * 1024u) /* 8 MiB，够 FAT16 / 8 MiB, enough for FAT16 */
static unsigned char g_disk[NSECT * SECT];

/* 分配台账 / allocation ledger */
#define LIVE_MAX 64
static void *g_live[LIVE_MAX];
void *g_wild_ptr;     /* 最近一个"不认识"的释放指针 / the last unrecognised free */
int g_wild_count;     /* 陌生释放次数 / how many unrecognised frees happened */

DSTATUS disk_status(BYTE pdrv) { (void)pdrv; return 0; }
DSTATUS disk_initialize(BYTE pdrv) { (void)pdrv; return 0; }

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    (void)pdrv;
    memcpy(buff, g_disk + (size_t)sector * SECT, (size_t)count * SECT);
    return RES_OK;
}
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    (void)pdrv;
    memcpy(g_disk + (size_t)sector * SECT, buff, (size_t)count * SECT);
    return RES_OK;
}
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    (void)pdrv;
    switch (cmd) {
        case CTRL_SYNC: return RES_OK;
        case GET_SECTOR_COUNT: *(LBA_t *)buff = NSECT; return RES_OK;
        case GET_SECTOR_SIZE: *(WORD *)buff = SECT; return RES_OK;
        case GET_BLOCK_SIZE: *(DWORD *)buff = 1; return RES_OK;
        default: return RES_PARERR;
    }
}
DWORD get_fattime(void) { return ((DWORD)(2026 - 1980) << 25) | (10u << 21) | (31u << 16); }

/* 卷 → 物理盘：整盘一个卷（FM_SFD），所以 pt=0。 */
PARTITION VolToPart[FF_VOLUMES] = { {0, 0}, {1, 0} };

void *ff_memalloc(size_t msize) {
    void *p = calloc(1, msize);
    if (!p) return NULL;
    for (int i = 0; i < LIVE_MAX; ++i) {
        if (!g_live[i]) { g_live[i] = p; return p; }
    }
    return p; /* 台账满了也照常给：契约用例不靠它判泄漏 / ledger overflow is not fatal here */
}

static int ledger_take(void *p) {
    for (int i = 0; i < LIVE_MAX; ++i) {
        if (g_live[i] == p) { g_live[i] = NULL; return 1; }
    }
    return 0;
}

void ff_memfree(void *mblock) {
    if (!mblock) return;
    if (ledger_take(mblock)) { free(mblock); return; }
    ++g_wild_count;
    g_wild_ptr = mblock; /* 不 free：记下来，由用例断言 / do not free: record it and let the test assert */
}
