/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：联网更新的版本清单与可取消后台任务（只下载到 TF 文件）。/ English: release metadata and cancellable download-to-TF jobs.
 * 冻结：在线链路不写任何 app 槽；完整校验 + UI 确认后由 ota_update.c 安装并切换启动分区。/ Frozen: the online link writes no app slot; ota_update.c installs and switches boot after verification and UI confirmation.
 * 修订：刷屏推送由 pico_online_display_* 递归锁串行化；进度只追加变化像素。/ Revision: screen pushes are serialized by the pico_online_display_* recursive lock; progress appends changed pixels only.
 * 本地化 OTA 批 26.11.25：启动切换由 ota_update.c 在 UI 确认后完成。/ Localized OTA: boot switch happens in ota_update.c after UI confirmation.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define PICO_OTA_LAYOUT "pico-dual-4m-v1"
#define PICO_OTA_BOARD "RDP-G01-W"
#define PICO_OTA_CAPABILITY "PICO_HTTPS_OTA_V1"
#define PICO_OTA_FEED_URL "https://cere-hy.github.io/pico-plus/update.json"
/// 自家 Pages 源（update.json + firmware/Read_Pico.bin）不覆盖词典 / 生词本，故默认启用联网 OTA。
/// PICO_ONLINE_UPDATES_DISABLED 保留为 kill switch：线上源出问题时置 1 可一行断开，回到 TF 卡升级。
/// / Self-hosted feed keeps the dictionary / vocabulary book, so online OTA is enabled by default.
/// / The macro stays as a kill switch: set it to 1 to cut the line and fall back to TF-card upgrades.
/// TF 卡升级与电脑刷机不受影响。/ TF-card and USB flashing are unaffected.
#ifndef PICO_ONLINE_UPDATES_DISABLED
#define PICO_ONLINE_UPDATES_DISABLED 0
#endif

typedef enum { PICO_UPDATE_IDLE, PICO_UPDATE_CHECKING, PICO_UPDATE_AVAILABLE,
               PICO_UPDATE_LATEST, PICO_UPDATE_DOWNLOADING, PICO_UPDATE_READY,
               PICO_UPDATE_FAILED, PICO_UPDATE_CANCELLED } pico_update_state_t;
typedef struct {
    char version[33], project[33], board[24], layout[32], minimum_base_version[33];
    char url[256], sha256[65], notes[384];
    uint32_t size;
} pico_release_t;
typedef struct {
    pico_update_state_t state;
    pico_release_t release;
    uint32_t revision, received;
    char message[96];
    bool busy;
} pico_update_status_t;

/// 有界解析、型号与版本验证；不接触网络。/ Bounded parsing, board and version validation without networking.
esp_err_t pico_release_parse(const char *json, size_t length, pico_release_t *release);
int pico_version_compare(const char *a, const char *b);
bool pico_release_url_valid(const char *url);
/// 本固件是否停用联网 OTA。/ Whether this build disables online updates.
bool pico_online_updates_disabled(void);
/// 检查或下载唯一后台任务；离页、锁屏先取消并等待。/ Start one worker; cancel and join on exit or lock.
esp_err_t pico_online_check(void);
esp_err_t pico_online_download(void);
bool pico_online_busy(void);
void pico_online_get_status(pico_update_status_t *status);
void pico_online_cancel_join(void);
/// 刷屏串行化递归锁（display.c 有 24 处依赖）；未初始化 OTA 时不分配内存。/ Recursive display serialization lock (24 callers in display.c); no allocation before OTA initialization.
bool pico_online_display_begin(void);
void pico_online_display_end(bool held);
// 本地化 OTA 批 26.11.25：启动切换由 ota_update.c 的 pico_ota_install() 在 UI 确认后完成，
// 在线链路不再暴露 pico_online_commit()（流式写槽第二真相已随第五件化石清除）。
