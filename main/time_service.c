/* SPDX-License-Identifier: Apache-2.0
 * 中文：S3 RTC 离线走时；在线每小时校准，离线每六小时短连已保存 WiFi。
 * English: S3 RTC keeps offline time; calibrate hourly online or briefly every six hours offline.
 */
#include "time_service.h"
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "pico_bootbb.h"
#include "pico_heapdiag.h"
#include "read_pico_transfer.h"
#include "push_sink.h"
#include "sleep.h"

#define CLOCK_VALID_MIN 1704067200ULL
#define CLOCK_VALID_MAX 4102444799ULL
#define CLOCK_ONLINE_INTERVAL 3600ULL
// gen22：PMU 内部 RC 每天快几分钟，离线 6 小时才补一次会让误差累积到肉眼可见；
// 缩到 2 小时（仅在已保存 WiFi 时短连，耗电可控）。
// gen22: the PMU's internal RC gains minutes a day; a six-hour offline cadence lets
// the error grow visible. Two hours (short join only when WiFi is saved, power-wise OK).
#define CLOCK_OFFLINE_INTERVAL (2ULL * 3600)
#define CLOCK_RETRY_US (5LL * 60 * 1000000)
// 26.9.14 D1：开机取证钉两个点。`boot-pre` = 第一次空闲轮询；`boot-post` = 40 s 之后。
// 为什么是 40 s 而不是 20 s：26.9.13 日志里 WiFi init 在 16.3 s、对时 teardown 在
// 31.4 s，40 s 是唯一一个「开关开着和关着都落在 teardown 之后」的采样点，同一个点
// 才能做对照组。/ Why 40 s and not 20 s: in the 26.9.13 log WiFi init landed at 16.3 s
// and the calibration teardown at 31.4 s. 40 s is the one sample point that sits after
// the teardown whether the switch is on or off - the same point can serve as control.
#define CLOCK_BOOT_PROBE_US (40LL * 1000000)
#define CLOCK_BLE_GATE_LARGEST (20 * 1024)
// 26.9.15 E2：逐块点名的触发阈值，从闸门值 20,480 抬到 24,576。
// 26.9.14 的教训：那台机 largest=21,504 **刚好越过** 20,480，于是「健康时沉默」的规则
// 把唯一一次点名机会省掉了——最想看明细的那一次，恰恰是它决定不说话的那一次。
// 新语义不是「病了才点名」，而是「不宽裕就点名」：24,576 = 闸门 + 4 KB 的安全垫宽度。
// 代价是每次开机都会倒一次 431 行；诊断版刻意接受，结案时收回。
// Raise the block-walk trigger from the gate value (20,480) to 24,576. In 26.9.14 the unit
// read 21,504 - just ABOVE the gate - so the "silent when healthy" rule skipped the one
// chance we had to see the inventory. The new predicate is "not roomy" rather than "ill":
// 24,576 = gate + a 4 KB safety pad. Cost: 431 lines on every boot; accepted for a
// diagnostic build, withdrawn at closure.
#define CLOCK_BOOT_BLOCKS_LARGEST (24 * 1024)
static atomic_bool s_running;
static bool s_loaded;
static uint64_t s_last_sync;
static int64_t s_next_attempt_us = 15000000;
// gen22 漂移账本：PMU 的本地秒走得快（内部 RC），用「上次校时的真实 UTC + 上次校时的
// 本地读数 + 学习到的 ppm」在没有网络时把时间拉回来。
// gen22 drift ledger: the PMU's local seconds run fast (internal RC); with no network,
// pull time back using "true UTC at last sync + local reading at last sync + learned ppm".
static uint32_t s_last_local;
static int32_t s_drift_ppm;
static bool s_drift_valid;

#define DRIFT_PPM_MAX 20000   // 2%：超过这个量当成异常样本丢弃 / beyond 2% treat the sample as bogus
#define DRIFT_MIN_ELAPSED 3600u  // 少于 1 小时的样本噪声太大 / under an hour, the sample is mostly noise

static void ensure_state(void) {
    if (s_loaded) return;
    nvs_handle_t handle;
    if (nvs_open("kiiko_clock", NVS_READONLY, &handle) == ESP_OK) {
        if (nvs_get_u64(handle, "last_ntp", &s_last_sync) != ESP_OK) s_last_sync = 0;
        uint32_t local = 0; int32_t ppm = 0;
        if (nvs_get_u32(handle, "last_local", &local) == ESP_OK) s_last_local = local;
        if (nvs_get_i32(handle, "drift_ppm", &ppm) == ESP_OK) s_drift_ppm = ppm;
        nvs_close(handle);
    }
    if (s_last_sync < CLOCK_VALID_MIN || s_last_sync > CLOCK_VALID_MAX) s_last_sync = 0;
    s_drift_valid = s_last_sync != 0 && s_last_local >= CLOCK_VALID_MIN &&
                    s_last_local <= CLOCK_VALID_MAX && s_drift_ppm > -DRIFT_PPM_MAX &&
                    s_drift_ppm < DRIFT_PPM_MAX;
    s_loaded = true;
}

void app_time_service_compensate(uint32_t *utc_sec) {
    if (!utc_sec) return;
    ensure_state();
    if (!s_drift_valid) return;                     // 没有可靠样本就不猜 / no sample, no guess
    uint32_t local = *utc_sec;
    if (local < CLOCK_VALID_MIN || local > CLOCK_VALID_MAX) return;
    if (local <= s_last_local) return;              // 时间倒退（换电池/重置）不补偿 / went backwards: skip
    uint64_t elapsed = (uint64_t)local - s_last_local;
    // 本地每走 1e6，真实只走 (1e6 - ppm)；把本地增量换算回真实增量。
    // For every 1e6 the local clock advanced, real time advanced (1e6 - ppm).
    uint64_t real = elapsed * (uint64_t)(1000000 - s_drift_ppm) / 1000000u;
    uint64_t corrected = (uint64_t)s_last_sync + real;
    if (corrected > CLOCK_VALID_MAX) return;
    int64_t delta = (int64_t)corrected - (int64_t)local;
    if (delta > 24L * 3600 || delta < -24L * 3600) return;  // 跨度过大说明账本已失效 / ledger stale
    ESP_LOGI("clock", "drift compensation: local=%lu -> %lu (ppm=%ld, held %llu s)",
             (unsigned long)local, (unsigned long)corrected, (long)s_drift_ppm,
             (unsigned long long)elapsed);
    *utc_sec = (uint32_t)corrected;
}

void app_time_service_note_sync(uint32_t true_utc, uint32_t local_before) {
    if (true_utc < CLOCK_VALID_MIN || true_utc > CLOCK_VALID_MAX) return;
    ensure_state();
    if (s_drift_valid && local_before > s_last_local) {
        uint64_t elapsed = (uint64_t)local_before - s_last_local;
        if (elapsed >= DRIFT_MIN_ELAPSED) {
            int64_t error = (int64_t)local_before - (int64_t)true_utc;  // 正 = 本地偏快 / positive = running fast
            int64_t instant = error * 1000000LL / (int64_t)elapsed;
            if (instant > -DRIFT_PPM_MAX && instant < DRIFT_PPM_MAX) {
                // 指数平滑：老样本权重 3、新样本 1，避免单次抖动主导。
                // EMA: old samples weight 3, new sample 1, so one outlier cannot dominate.
                int64_t blended = ((int64_t)s_drift_ppm * 3 + instant) / 4;
                s_drift_ppm = (int32_t)blended;
                ESP_LOGI("clock", "drift sample: %ld ppm over %llu s -> %ld ppm",
                         (long)instant, (unsigned long long)elapsed, (long)s_drift_ppm);
            }
        }
    }
    s_last_sync = true_utc;
    s_last_local = true_utc;   // 校正后本地读数等于真实 UTC / after correction the local reading equals true UTC
    s_drift_valid = true;
    nvs_handle_t handle;
    if (nvs_open("kiiko_clock", NVS_READWRITE, &handle) == ESP_OK) {
        (void)nvs_set_u64(handle, "last_ntp", s_last_sync);
        (void)nvs_set_u32(handle, "last_local", s_last_local);
        (void)nvs_set_i32(handle, "drift_ppm", s_drift_ppm);
        (void)nvs_commit(handle);
        nvs_close(handle);
    }
}

static void clock_worker(void *arg) {
    (void)arg;
    // gen22：SNTP 成功会直接 settimeofday 覆盖系统时间，所以「漂移前的本地读数」
    // 必须在发起对时之前抓；事后读到的已经是真值，学不到漂移。
    // gen22: a successful SNTP settimeofday overwrites system time, so the "local
    // reading before drift correction" must be captured BEFORE the sync - reading it
    // afterwards yields the truth and teaches us nothing.
    uint32_t local_before = 0;
    {
        time_t now = time(NULL);
        if (now >= (time_t)CLOCK_VALID_MIN && now <= (time_t)CLOCK_VALID_MAX)
            local_before = (uint32_t)now;
    }
    uint32_t utc = 0;
    esp_err_t err = read_pico_transfer_sync_time_auto(&utc);
    ESP_LOGI("clock", "background calibration: %s", esp_err_to_name(err));
    if (err == ESP_OK && local_before) app_time_service_note_sync(utc, local_before);
    atomic_store(&s_running, false);
    vTaskDelete(NULL);
}

// 26.9.14 D1：开机取证的两次采样。放在对时子系统里是有意的——26.9.13 证明「16 秒
// 之后」这件事本来就是这个子系统的副作用，把探针留在案发地，谁改对时谁就看得见它。
// 26.9.14 D1: the two boot samples. They live in the clock subsystem on purpose: 26.9.13
// proved the "after 16 seconds" boundary was a side effect of this very subsystem, so the
// probe stays at the scene - whoever touches calibration sees it.
static bool s_boot_pre_done;
static bool s_boot_post_done;

static void boot_probe(int64_t monotonic_us) {
    // 26.9.18 S2-a：黑匣子的出口已挪到主循环的 500 ms 块（那里不受「10 s 无输入」门控）。
    // 26.9.15 把出口留在这里，实测 13.8 s 才吐——用户一直摸屏幕时这个函数根本不跑。
    // 26.9.18 S2-a: the blackbox outlet now lives in the main loop's 500 ms block, which is
    // not gated on "10 s without input". Leaving it here is what made 26.9.15 spill at 13.8 s.

    if (!s_boot_pre_done) {
        s_boot_pre_done = true;
        pico_heap_dump("boot-pre", "time_service:boot_probe");
        return;
    }
    if (s_boot_post_done || monotonic_us < CLOCK_BOOT_PROBE_US) return;
    s_boot_post_done = true;
    // 兜底：若 4 s 那次没吐成（主机还没回来），40 s 时再试一次；已经吐过就什么都不做。
    // Backstop: if the 4 s attempt never went out, retry at 40 s; no-op once spilled.
    pico_bootbb_replay();
    pico_heap_dump("boot-post", "time_service:boot_probe");
    // 26.9.20 S13：块表能说「8,192 B、后面紧跟 100 B」，说不出「谁的」。这里把任务栈
    // 的真实块大小列出来反查——任务栈在堆里就是「栈 + 紧邻 TCB」，对上了就点名。
    // 26.9.20 S13: the table says "8,192 B, a 100 B block behind it", never whose. This
    // lists every task's real stack block so the pattern can be matched to a name.
    pico_heap_attr("boot-post");
    // 26.9.14 D2：只有最大块掉到 BLE 闸门以下才逐块打印。健康时沉默，病了才开仓库——
    // 431 个块逐条打印在正常使用里毫无价值，但在「11 颗石子」那种现场是唯一能定责的东西。
    // D2: walk every block ONLY when the largest block is under the BLE gate. Silence when
    // healthy, full inventory when ill: 431 lines are worthless in normal use and are the
    // only way to name the culprit on a "11 pebbles" scene.
    pico_heap_dump_blocks("boot-post", CLOCK_BOOT_BLOCKS_LARGEST);
}

void app_time_service_poll(bool idle) {
    if (!idle) return;
    ensure_state();
    // 26.9.14 D1：开机前后各钉一次内部堆。健康机上这两行应当「什么都没变」——
    // 而「没变」正是本版要证明的东西。/ Two pins around boot. On a healthy unit these
    // two lines should show "nothing moved", and "nothing moved" is the point of this build.
    boot_probe(esp_timer_get_time());
    // gen22 蓝牙校时：手机带来的真值优先登记（无 Wi-Fi 时的主力校时通道）。
    // 系统时钟已在 push_sink 收到时写好，这里补漂移样本与 PMU 同步。
    // gen22 time-over-BLE: prefer the phone's truth (the main path with no WiFi).
    // The system clock was already set on receipt; record the sample and sync the PMU.
    uint32_t phone_utc = 0;
    if (push_sink_take_time_sync(&phone_utc)) {
        time_t local = time(NULL);
        uint32_t local_before = (local >= (time_t)CLOCK_VALID_MIN && local <= (time_t)CLOCK_VALID_MAX)
                                ? (uint32_t)local : phone_utc;
        app_time_service_note_sync(phone_utc, local_before);
        (void)app_sync_time_to_pmu();
    }
    uint32_t received;
    if (read_pico_transfer_take_time_sync(&received)) {
        // 只有真实网络成功才持久化；普通重启/手动设时间不能伪装成网络校准。
        // Persist only a real network success; reboot/manual edits do not count as calibration.
        // 前台传书路径的 SNTP 已经把系统时间改成真值，拿不到漂移前的读数，
        // 因此只刷新基准、不参与漂移学习（漂移由后台 clock_worker 采样）。
        // The foreground transfer path already applied SNTP to system time, so the
        // pre-drift reading is gone: refresh the anchor only, leave drift sampling to
        // the background clock_worker.
        app_time_service_note_sync(received, received);
        (void)app_sync_time_to_pmu();
    }
    const int64_t monotonic = esp_timer_get_time();
    if (atomic_load(&s_running) || monotonic < s_next_attempt_us) return;
    read_pico_transfer_status_t status;
    read_pico_transfer_get_status(&status);
    bool online = status.state == READ_PICO_TRANSFER_READY &&
                  status.mode == READ_PICO_TRANSFER_MODE_STA && status.network_ready;
    if (!online && status.state != READ_PICO_TRANSFER_STOPPED) return;
#if !CONFIG_CLOCK_OFFLINE_AUTO_SYNC
    // 26.9.14 A1：离线时不再自己拉起射频。26.9.13 真机日志（摘录在 Kconfig 里）证明，
    // 这一次「注定失败」的对时——15.1 s 的 esp_wifi_init/deinit——把一个 32,767 B 的
    // 内部堆池切成 5 段，最大块永久停在 18,432，BLE 的 20 K 闸门此后再也过不了。
    // 对时收益为零（连不上），代价是一条 BLE 功能的整条命。这一行就是止损点。
    // 无网校时由蓝牙校时（push_sink 收到手机时间）承担，在线校时由 sync_time_online_impl
    // 承担，用户手动校准仍保留临时射频通路（那是显式请求，不是自选动作）。
    // A1: never self-start the radio while offline. The 26.9.13 field log shows one
    // doomed 15.1 s esp_wifi_init/deinit chopping a 32,767 B internal pool into five
    // fragments, pinning the largest block at 18,432 and killing the BLE 20 K gate for
    // the rest of the boot. Zero calibration value (there is nothing to connect to),
    // whole-function cost. BLE time sync and the online path cover calibration; manual
    // calibration keeps its radio path because that is an explicit request.
    if (!online) return;
#endif
    time_t now = time(NULL);
    uint64_t interval = online ? CLOCK_ONLINE_INTERVAL : CLOCK_OFFLINE_INTERVAL;
    if (now >= (time_t)s_last_sync && s_last_sync && (uint64_t)now - s_last_sync < interval) return;
    s_next_attempt_us = monotonic + CLOCK_RETRY_US;
    if (!online) {
        char ssid[33]; bool configured = false;
        if (read_pico_transfer_get_saved_wifi(ssid, &configured) != ESP_OK || !configured) return;
    }
    atomic_store(&s_running, true);
    // 仅对时期间保留任务栈；不创建常驻无线任务或页面缓存。
    // Keep a task stack only during calibration; no permanent radio task or page cache.
    if (xTaskCreate(clock_worker, "rtc_sync", 6144, NULL, 1, NULL) != pdPASS) {
        atomic_store(&s_running, false);
        ESP_LOGW("clock", "background calibration deferred: no task memory");
    }
}
