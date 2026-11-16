/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#define FF_FS_REENTRANT 1
#define FR_OK 0
// 26.9.21 S14(b)：分片扫描要读卷对象的这几个字段，stub 必须和 FatFs 的真结构对得上。
// 26.9.21 S14(b): the sliced scan reads these volume fields, so the stub must match.
#define FS_FAT32 3
typedef uint8_t BYTE;
typedef struct {
    BYTE fs_type;      // FS_FAT32 = 3
    BYTE pdrv;
    BYTE ldrv;
    uint16_t csize;    // 每簇扇区数 / sectors per cluster
    uint32_t n_fatent; // 簇数 + 2 / clusters + 2
    uint32_t fatbase;  // FAT 起始扇区 / FAT base sector
    uint32_t free_clst;
} FATFS;
typedef struct {struct {FATFS *fs;}obj;} FF_DIR;
int f_opendir(FF_DIR *root,const char *path);
int f_closedir(FF_DIR *root);
int ff_mutex_take(int drive);
void ff_mutex_give(int drive);
