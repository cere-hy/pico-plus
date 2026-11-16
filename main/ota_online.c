/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：HTTPS 分块下载到 TF 文件、流上完整性校验和可取消升级任务；安装由 ota_update.c 在 UI 确认后完成。/ English: HTTPS download to a TF file behind a streaming hash gate; ota_update.c installs after UI confirmation.
 * 冻结：离页或锁屏先等待任务结束；用户确认安装前不碰任何 app 槽。/ Frozen: join before leaving or locking; no slot is touched before the user confirms.
 * 门与诊断：下载专用门 16K（暂定，D1 自证）；门 FAIL 时四项实测值同帧上屏。/ Gates: 16K download-only gate (provisional, D1 self-check); four live readings shown on gate failure.
 * 修订：刷屏推送由递归锁串行化，进度只追加变化像素。/ Revision: screen pushes are serialized by the recursive lock; progress appends changed pixels only.
 * 本地化 OTA 批 26.11.25：在线链路只下载到 TF 文件，安装交回 ota_update.c；全程零 esp_ota_* 调用（第五件化石清除）。
 * / Localized OTA: the online link downloads to the TF file only; the proven local installer flashes it. No esp_ota_* here.
 */
#include "ota_online.h"
#include "ota_update.h"  /* PICO_OTA_UPDATE_PATH：下载落点即安装器入口，不复制第二个家 */
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "read_pico_transfer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "psa/crypto.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static SemaphoreHandle_t s_mutex, s_done, s_flash_display;
static pico_update_status_t *s_status;
static atomic_bool s_cancel;
static bool s_joinable, s_download;

static bool initialize(void) {
    if (!s_status) s_status = heap_caps_calloc(1, sizeof(*s_status), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    if (!s_done) s_done = xSemaphoreCreateBinary();
    if (!s_flash_display) s_flash_display = xSemaphoreCreateRecursiveMutex();
    return s_status && s_mutex && s_done && s_flash_display;
}
// 递归锁壳保留：display.c 有 24 处刷屏串行化调用依赖 pico_online_display_*。
// 本地化 OTA 后在线链路不再碰 flash，此锁退化为「刷屏自身互斥」，不再是 flash/显示互斥。
// / The display/flash exclusion lock shell stays (display.c depends on it); the online link
// / no longer touches flash, so it now only serializes screen pushes.
bool pico_online_display_begin(void) {
    return s_flash_display && xSemaphoreTakeRecursive(s_flash_display, portMAX_DELAY) == pdTRUE;
}
void pico_online_display_end(bool held) {
    if (held) xSemaphoreGiveRecursive(s_flash_display);
}
static bool memory_available(size_t internal_min, size_t internal_block, size_t external_min) {
    const unsigned internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const unsigned external = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    return heap_caps_get_free_size(internal) >= internal_min &&
        heap_caps_get_largest_free_block(internal) >= internal_block &&
        heap_caps_get_free_size(external) >= external_min &&
        heap_caps_get_largest_free_block(external) >= 32768;
}
static void state(pico_update_state_t value, const char *message) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status->state = value;
    snprintf(s_status->message, sizeof(s_status->message), "%s", message);
    ++s_status->revision;
    xSemaphoreGive(s_mutex);
}
// 诊断批 26.11.24 硬约束保留：门 FAIL 时把四项实测值同帧写进提示串（数值前置抗 fit_value 截尾）。
// 第五维 caps 与 memory_available() 完全一致（SPIRAM|8BIT），不照抄体检面板的裸 SPIRAM。
// 门降到 16K 后 FAIL 概率更低，但 TLS 握手峰值无实测——25 的 D1 必须能自证，故一字不动。
static void state_mem_failed(const char *reason) {
    const unsigned internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const unsigned external = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    char message[96];  // == pico_update_status_t::message
    snprintf(message, sizeof(message), "内存不足 内%uK/块%uK/PS%uM/PS块%uK %s",
        (unsigned)(heap_caps_get_free_size(internal) / 1024),
        (unsigned)(heap_caps_get_largest_free_block(internal) / 1024),
        (unsigned)(heap_caps_get_free_size(external) / (1024u * 1024u)),
        (unsigned)(heap_caps_get_largest_free_block(external) / 1024),
        reason);
    state(PICO_UPDATE_FAILED, message);
}
bool pico_online_busy(void) {
    if (!s_status || !s_mutex) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool busy = s_status->busy;
    xSemaphoreGive(s_mutex); return busy;
}
void pico_online_get_status(pico_update_status_t *status) {
    if (!status) return;
    memset(status, 0, sizeof(*status));
    if (!s_status || !s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(status, s_status, sizeof(*status));
    xSemaphoreGive(s_mutex);
}
static bool layout_valid(void) {
    const esp_partition_t *a = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    const esp_partition_t *b = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    const esp_partition_t *data = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    // 关系校验：两个 OTA 槽都在、等大、otadata 在即通过；地址 / 容量一律取自分区表
    // （partition manager），不再硬编码任何数值——分区表改版不会让这里静默失效。
    // / Relational check: both slots present, equal size, otadata present. All addresses
    // / and sizes come from the partition manager; no hard-coded layout constants.
    return a && b && data && a->size == b->size && a->size > 0;
}
static bool online(bool *owned) {
    read_pico_transfer_status_t status;
    read_pico_transfer_get_status(&status);
    if (!(status.mode == READ_PICO_TRANSFER_MODE_STA && status.network_ready)) {
        char ssid[33]; bool saved = false;
        if (read_pico_transfer_get_saved_wifi(ssid, &saved) != ESP_OK || !saved) {
            state(PICO_UPDATE_FAILED, "请先在 WiFi 设置中连接网络"); return false;
        }
        read_pico_transfer_stop();
        read_pico_transfer_cfg_t cfg = {.mode = READ_PICO_TRANSFER_MODE_STA, .network_only = true};
        *owned = true;
        if (read_pico_transfer_start(&cfg) != ESP_OK) {
            state(PICO_UPDATE_FAILED, read_pico_transfer_rf_blocked()
                                          ? "飞行模式已开启，请先在设置中关闭" : "WiFi 连接失败，请重试");
            return false;
        }
    }
    int64_t deadline = esp_timer_get_time() + 30000000;
    while (!atomic_load(&s_cancel)) {
        read_pico_transfer_service_poll();
        read_pico_transfer_get_status(&status);
        if (status.mode == READ_PICO_TRANSFER_MODE_STA && status.network_ready) {
            if (time(NULL) < 1704067200) {
                uint32_t utc;
                if (read_pico_transfer_sync_time_online(&utc) != ESP_OK) {
                    state(PICO_UPDATE_FAILED, "网络对时失败，请对时后重试"); return false;
                }
            }
            return !atomic_load(&s_cancel);
        }
        if (status.state == READ_PICO_TRANSFER_ERROR || esp_timer_get_time() >= deadline) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!atomic_load(&s_cancel)) state(PICO_UPDATE_FAILED, "网络连接超时，请重试");
    return false;
}
typedef struct {
    uint32_t start, end, size;
    bool valid;
} download_range_t;
static esp_err_t range_header(esp_http_client_event_t *event) {
    download_range_t *range = event->user_data;
    if (!range || event->event_id != HTTP_EVENT_ON_HEADER || !event->header_key || !event->header_value)
        return ESP_OK;
    if (!strcasecmp(event->header_key, "Content-Range")) {
        unsigned long start, end, size; char tail;
        range->valid = strnlen(event->header_value, 96) < 96 &&
            sscanf(event->header_value, "bytes %lu-%lu/%lu%c", &start, &end, &size, &tail) == 3 &&
            start <= end && end < size && size <= UINT32_MAX;
        if (range->valid) { range->start = start; range->end = end; range->size = size; }
    }
    return ESP_OK;
}
static esp_http_client_handle_t open_http_range(const char *url, uint32_t offset,
        download_range_t *range, int *status, int64_t *length) {
    if (range) memset(range, 0, sizeof(*range));
    esp_http_client_config_t cfg = {.url = url, .timeout_ms = 15000, .buffer_size = 2048,
        .buffer_size_tx = 1024, .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true, .keep_alive_enable = true,
        .event_handler = range_header, .user_data = range};
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return NULL;
    esp_http_client_set_header(client, "User-Agent", PICO_OTA_CAPABILITY);
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_http_client_set_header(client, "Cache-Control", "no-cache");
    if (offset) {
        char value[48]; snprintf(value, sizeof(value), "bytes=%lu-", (unsigned long)offset);
        if (esp_http_client_set_header(client, "Range", value) != ESP_OK) goto fail;
    }
    if (esp_http_client_open(client, 0) != ESP_OK || atomic_load(&s_cancel)) goto fail;
    *length = esp_http_client_fetch_headers(client);
    if (*length < 0 || atomic_load(&s_cancel)) goto fail;
    *status = esp_http_client_get_status_code(client);
    esp_http_client_set_timeout_ms(client, 1000);
    return client;
fail:
    esp_http_client_cleanup(client);
    return NULL;
}
static esp_http_client_handle_t open_http(const char *url, int *status, int64_t *length) {
    return open_http_range(url, 0, NULL, status, length);
}
// 复用连接管理器等待短暂断网恢复，允许取消，不重启整个无线服务。
// Wait for the connection manager to recover a short outage, with cancellation and no radio restart.
static bool wait_network(void) {
    int64_t deadline = esp_timer_get_time() + 30000000;
    while (!atomic_load(&s_cancel) && esp_timer_get_time() < deadline) {
        read_pico_transfer_service_poll();
        read_pico_transfer_status_t status;
        read_pico_transfer_get_status(&status);
        if (status.mode == READ_PICO_TRANSFER_MODE_STA && status.network_ready) return true;
        if (status.state == READ_PICO_TRANSFER_ERROR) return false;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}
// 读超时可重试；每次收到数据续期，避免慢速网络被总时长截断。
// Retry temporary read timeouts and renew the idle deadline after each chunk.
static int read_chunk(esp_http_client_handle_t client, uint8_t *buffer, int cap, int64_t *idle) {
    while (!atomic_load(&s_cancel) && esp_timer_get_time() < *idle) {
        read_pico_transfer_service_poll();
        int n = esp_http_client_read(client, (char *)buffer, cap);
        if (n > 0) { *idle = esp_timer_get_time() + 30000000; return n; }
        if (!n && esp_http_client_is_complete_data_received(client)) return 0;
        if (n < 0 && n != -ESP_ERR_HTTP_EAGAIN) return -1;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return -1;
}
static bool check_feed(void) {
    char *json = heap_caps_malloc(8193, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    pico_release_t *release = heap_caps_calloc(1, sizeof(*release), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!json || !release) { state_mem_failed("请退出传书后重试"); goto failed; }
    // 自家 Pages 单源；同一缓冲区顺序重试，无并发 TLS 连接、无原厂回退源。
    // / Single self-hosted Pages feed; sequential retries in one buffer, no stock fallback.
    static const char *const feeds[] = {PICO_OTA_FEED_URL};
    bool valid = false;
    for (size_t source = 0; source < sizeof(feeds) / sizeof(feeds[0]) && !atomic_load(&s_cancel); ++source) {
        char url[192]; snprintf(url, sizeof(url), "%s?t=%lld", feeds[source], (long long)time(NULL));
        int code = -1; int64_t length = -1;
        esp_http_client_handle_t client = NULL;
        for (int i = 0; i < 2 && !atomic_load(&s_cancel); ++i) {
            if (wait_network()) client = open_http(url, &code, &length);
            if (client) break;
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        if (!client) continue;
        if (code != 200 || length > 8192) { esp_http_client_cleanup(client); continue; }
        size_t received = 0;
        int64_t idle = esp_timer_get_time() + 30000000;
        int n = -1;
        while (received < 8192 && (n = read_chunk(client, (uint8_t *)json + received, (int)(8192 - received), &idle)) > 0)
            received += n;
        bool complete = esp_http_client_is_complete_data_received(client);
        esp_http_client_cleanup(client);
        json[received] = 0;
        if (n >= 0 && complete && pico_release_parse(json, received, release) == ESP_OK) { valid = true; break; }
    }
    if (!valid) { state(PICO_UPDATE_FAILED, "更新服务器连接失败或清单无效，请稍后重试"); goto failed; }
    const esp_app_desc_t *running = esp_app_get_description();
    if (strcmp(release->project, running->project_name) || !layout_valid() ||
        pico_version_compare(running->version, release->minimum_base_version) < 0) {
        state(PICO_UPDATE_FAILED, "基础包不兼容，请先通过官网完整刷机"); goto failed;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(&s_status->release, release, sizeof(*release));
    xSemaphoreGive(s_mutex);
    state(pico_version_compare(release->version, running->version) > 0 ? PICO_UPDATE_AVAILABLE : PICO_UPDATE_LATEST,
          pico_version_compare(release->version, running->version) > 0 ? "发现新版本，可下载安装" : "当前已是最新版本");
    heap_caps_free(json); heap_caps_free(release);
    return true;
failed:
    heap_caps_free(json); heap_caps_free(release);
    return false;
}
// 本地化 OTA 批 26.11.25：在线链路只做「下载到 TF 文件」，安装交回 ota_update.c 的成熟
// pico_ota_install()。全程零 esp_ota_* 调用——用户确认安装前 flash 一个字节都没碰；
// 「校验先于擦除」从纪律升级为结构。缓冲放 PSRAM，哈希闸在流上累计，流末比对 sha 通过才 READY。
// 下载落点 = PICO_OTA_UPDATE_PATH，正是 pico_ota_resolve_path() 优先命中路径，安装链原生接得住。
// 失败/取消一律 remove() 清理：残缺文件会挡住用户自己放进 TF 卡的同名/异名包。
// / Localized OTA: the online link only downloads to the TF file; the proven local installer
// / flashes after explicit UI confirmation. No esp_ota_* calls here, so no slot is touched
// / before the user confirms. Hash gate stays on the stream; READY only after sha match.
static bool download(void) {
    pico_release_t *release = heap_caps_malloc(sizeof(*release), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *buffer = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!release || !buffer) { state_mem_failed("无法下载升级包"); goto failed; }
    xSemaphoreTake(s_mutex, portMAX_DELAY); *release = s_status->release; xSemaphoreGive(s_mutex);
    FILE *file = fopen(PICO_OTA_UPDATE_PATH, "wb");
    if (!file) { state(PICO_UPDATE_FAILED, "需要 TF 卡，请插入后重试"); goto failed; }
    int code = -1; int64_t length = -1;
    esp_http_client_handle_t client = NULL;
    download_range_t range;
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    esp_err_t error = ESP_OK;
    if (psa_crypto_init() != PSA_SUCCESS || psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) error = ESP_FAIL;
    size_t total = 0;
    int64_t idle = esp_timer_get_time() + 30000000;
    int n = -1;
    unsigned retries = 0;
    bool complete = false;
    while (error == ESP_OK && !atomic_load(&s_cancel)) {
        if (!client) {
            if (wait_network()) client = open_http_range(release->url, total, &range, &code, &length);
            if (!client) {
                if (atomic_load(&s_cancel) || retries++ >= 3) { error = ESP_FAIL; break; }
                vTaskDelay(pdMS_TO_TICKS(250 * retries)); continue;
            }
            // 续传必须严格匹配起点与全文件长度；文件只 fopen 一次、顺序追加，天然对齐 total。
            // / Resumed responses must match offset==total and full length; the whole image still gets SHA256.
            bool response_ok = total ? code == 206 && range.valid && range.start == total &&
                range.size == release->size && range.end == release->size - 1 : code == 200;
            if (!response_ok || (length > 0 && (uint64_t)length != release->size - total)) {
                error = ESP_ERR_INVALID_RESPONSE; break;
            }
            idle = esp_timer_get_time() + 30000000;
        }
        n = read_chunk(client, buffer, 4096, &idle);
        if (n <= 0) {
            complete = esp_http_client_is_complete_data_received(client);
            esp_http_client_cleanup(client); client = NULL;
            if (n == 0 && complete && total == release->size) break;
            complete = false;
            if (total >= release->size || atomic_load(&s_cancel) || retries++ >= 3) { error = ESP_FAIL; break; }
            vTaskDelay(pdMS_TO_TICKS(250 * retries));
            continue;
        }
        if (total + n > release->size) { error = ESP_ERR_INVALID_SIZE; break; }
        if (fwrite(buffer, 1, (size_t)n, file) != (size_t)n) { error = ESP_FAIL; break; }
        if (psa_hash_update(&hash, buffer, n) != PSA_SUCCESS) { error = ESP_FAIL; break; }
        total += n;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_status->received = total; ++s_status->revision;
        xSemaphoreGive(s_mutex);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (client) esp_http_client_cleanup(client);
    if (fclose(file) != 0) error = ESP_FAIL;
    uint8_t digest[32]; size_t digest_size = 0; char hex[65];
    if (error == ESP_OK && (n < 0 || !complete || total != release->size || atomic_load(&s_cancel))) error = ESP_FAIL;
    if (error == ESP_OK && psa_hash_finish(&hash, digest, sizeof(digest), &digest_size) != PSA_SUCCESS) error = ESP_FAIL;
    psa_hash_abort(&hash);
    if (error == ESP_OK) {
        for (unsigned i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
        if (digest_size != 32 || strcasecmp(hex, release->sha256)) error = ESP_ERR_INVALID_RESPONSE;
    }
    if (error != ESP_OK) {
        remove(PICO_OTA_UPDATE_PATH);
        state(PICO_UPDATE_FAILED, "下载中断或校验失败，当前版本保持不变"); goto failed;
    }
    state(PICO_UPDATE_READY, "下载完成，是否立即安装？");
    heap_caps_free(release); heap_caps_free(buffer);
    return true;
failed:
    heap_caps_free(release); heap_caps_free(buffer);
    return false;
}
static void worker(void *unused) {
    (void)unused;
    bool owned = false;
    wifi_ps_type_t previous_ps = WIFI_PS_MIN_MODEM;
    bool restore_ps = false;
    if (online(&owned) && !atomic_load(&s_cancel)) {
        // 更新期间关闭无线省电；完成、取消和失败均恢复原策略。
        // Disable radio power saving for updates, then restore it on success, cancellation or failure.
        restore_ps = esp_wifi_get_ps(&previous_ps) == ESP_OK && esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK;
        // 下载专用门：缓冲移 PSRAM 后内部需求大降，16K 为暂定候选；TLS 握手峰值无实测，
        // 25 的 D1 是它的验收。诊断打点全保留，不第三次用推理放行。
        if (!memory_available(16384, 8192, 262144))
            state_mem_failed("请退出传书后重试");
        else if (s_download) (void)download(); else (void)check_feed();
    }
    if (restore_ps) (void)esp_wifi_set_ps(previous_ps);
    if (owned) read_pico_transfer_stop();
    if (atomic_load(&s_cancel)) state(PICO_UPDATE_CANCELLED, "已取消，当前版本保持不变");
    xSemaphoreTake(s_mutex, portMAX_DELAY); s_status->busy = false; ++s_status->revision; xSemaphoreGive(s_mutex);
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}
static esp_err_t start(bool downloading) {
    if (!initialize()) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool busy = s_status->busy;
    bool available = s_status->state == PICO_UPDATE_AVAILABLE;
    xSemaphoreGive(s_mutex);
    if (busy || (downloading && !available)) return ESP_ERR_INVALID_STATE;
    if (s_joinable) { xSemaphoreTake(s_done, portMAX_DELAY); s_joinable = false; }
    // 与 worker 门同值重校准（改一个点验全部家）：只动 internal_min 一个参数。
    if (!memory_available(16384, 16384, 524288)) {
        state_mem_failed("请退出传书后重试");
        return ESP_ERR_NO_MEM;
    }
    atomic_store(&s_cancel, false);
    s_download = downloading;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status->busy = true; s_status->received = 0;
    xSemaphoreGive(s_mutex);
    state(downloading ? PICO_UPDATE_DOWNLOADING : PICO_UPDATE_CHECKING,
          downloading ? "正在下载，短暂断网会自动重连" : "正在连接并检查更新");
    if (xTaskCreate(worker, "kiikoread_ota", 12288, NULL, 4, NULL) != pdPASS) {
        xSemaphoreTake(s_mutex, portMAX_DELAY); s_status->busy = false; xSemaphoreGive(s_mutex);
        state_mem_failed("无法开始升级"); return ESP_ERR_NO_MEM;
    }
    s_joinable = true;
    return ESP_OK;
}
bool pico_online_updates_disabled(void) { return PICO_ONLINE_UPDATES_DISABLED != 0; }
// 停用后不建任务、不开无线、不访问清单；start() 仍在语法上被引用，避免留下未使用函数告警。
// When disabled: no worker, no radio, no feed request. start() is still referenced syntactically so
// the rest of the pipeline keeps compiling without unused-function warnings.
esp_err_t pico_online_check(void) {
    if (!PICO_ONLINE_UPDATES_DISABLED) return start(false);
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t pico_online_download(void) {
    if (!PICO_ONLINE_UPDATES_DISABLED) return start(true);
    return ESP_ERR_NOT_SUPPORTED;
}
void pico_online_cancel_join(void) {
    if (s_joinable) {
        atomic_store(&s_cancel, true);
        xSemaphoreTake(s_done, portMAX_DELAY); s_joinable = false;
    }
    if (s_status && s_status->state == PICO_UPDATE_READY) state(PICO_UPDATE_CANCELLED, "已取消，当前版本保持不变");
}
// 启动切换由 ota_update.c 的 pico_ota_install() 在 UI 确认后完成；在线链路不再持有
// s_verified / 不再暴露 pico_online_commit()——流式写槽的第二真相已随第五件化石清除。
