/* SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：IO 契约用例的配置。与设备 26.9.28/26.9.29 的 sdkconfig 对齐，只放掉宿主办不到的两项：
 *   · FF_FS_REENTRANT=0 —— 宿主没有 FreeRTOS 互斥量（由 run_excerpt_io_contract.sh 改）；
 *   · 其它（FF_USE_LFN=3 / FF_USE_DYN_BUFFER=1 / FF_FS_TINY=0 / FF_STR_VOLUME_ID=0）
 *     一个字不改：这三项正是 26.9.28 事故的现场条件。
 * English: config for the IO contract tests, matched to the device sdkconfig except for the
 * one thing a host cannot provide (FreeRTOS mutexes).
 */
#pragma once
#define CONFIG_FATFS_USE_FASTSEEK 0
#define CONFIG_FATFS_USE_LABEL 0
#define CONFIG_FATFS_CODEPAGE 437
#define CONFIG_FATFS_LFN_HEAP 1
#define CONFIG_FATFS_MAX_LFN 255
#define CONFIG_FATFS_API_ENCODING_ANSI_OEM 1
#define CONFIG_FATFS_VOLUME_COUNT 2
#define CONFIG_WL_SECTOR_SIZE 4096
#define CONFIG_FATFS_PER_FILE_CACHE 1
#define CONFIG_FATFS_FS_LOCK 0
#define CONFIG_FATFS_TIMEOUT_MS 10000
#define CONFIG_FATFS_USE_DYN_BUFFERS 1
#define portTICK_PERIOD_MS 1
