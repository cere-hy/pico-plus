/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * SDMMC 探测、挂载、格式化。
 *
 * SDMMC probe, mount, and format.
 */

#include "read_pico_sd.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "read_pico_board.h"
#include "pico_bootbb.h"
#include "sdmmc_cmd.h"
#include "ff.h"
#include "diskio_sdmmc.h"

#define SD_MOUNT_POINT "/sdcard"
#define SD_PIN_CLK GPIO_NUM_38
#define SD_PIN_CMD GPIO_NUM_42
#define SD_PIN_D0 GPIO_NUM_44

static const char* TAG = "sd_card";
static sdmmc_card_t* card;
static int probe_state;
static bool filesystem_unreadable;
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool media_invalidated;
static read_pico_sd_info_t cached_info;
// E-2（§19.4）：**挂载代号**。挂载成功一次 +1，只增不减。
//
// 为什么需要它：状态位（`present`/`mounted`）是**快照**，不是事件。26.9.31 现场
// `Mounted` 100.977 → 队列 100.994 就写卡（17 ms）→ `mounted=1` 101.863 才开始计
// 5 s 让行。中间 886 ms 里 `!*mounted && ready` 这条边沿根本没成立——拔插快于一次
// 轮询时，app 侧看到的 `*mounted` 一直是 true，"从没挂载"与"刚挂载"在快照上同形。
// 代号没有这个歧义：挂载就是 +1，漏不掉，也不会因为状态抖动而重复触发（幂等由消费侧守）。
// Why: state bits are snapshots, not events. In the 26.9.31 trace the card mounted at
// 100.977 ms and the queue wrote at 100.994 ms, while `mounted=1` only showed up at
// 101.863 ms - the edge never existed inside that 886 ms window. A monotonic epoch has
// no such ambiguity: one mount, one increment.
static uint32_t s_mount_epoch;
// P1-1 的状态（定义在此处：`mount_card()` 先于本文件的 P1-1 段落，换卡时要清零）。
// P1-1 state, declared up here because mount_card() clears it on every successful mount.
static uint8_t  s_io_fails;      // 连续 0x107 次数（成功即清零）/ consecutive timeouts
static uint32_t s_io_fail_ms;    // 首次失败时刻（0 = 当前没有连续失败）
static uint32_t s_io_next_ms;    // 下次允许探测的时刻（0 = 立刻）
// 26.9.23 S24：urge 请求位住在 cached_info.space_urge（头文件 bool 组的原有填充里，
// 结构体仍 56 B）。**为什么这么绕**：ld 的装箱对符号集敏感，不止对字节数敏感——
// 实测 uint16 版 s_sd_flags 被 ld 塞进 s_mounted_fs 后的对齐空隙，符号集体积没变，
// 节总量反而 −16、堆基址 −16。符号集与 26.9.21 **逐字节相同**是唯一构造安全的形态。
// 26.9.23 S24: the urge bit lives in cached_info.space_urge (inside the header bool
// group's existing padding; the struct stays 56 B). Why so roundabout: ld packing is
// sensitive to the symbol SET, not just byte totals - measured: a uint16 flags variable
// got squeezed into the gap after s_mounted_fs and the section shrank by 16 bytes,
// moving the heap base. A byte-identical symbol set to 26.9.21 is the only
// construction-safe form.

// ---- 26.9.31a：CD 采样去抖 --------------------------------------------------
// 现场 344796 ms 那次 `present=0` 与同一秒里的 `sdmmc_read_blocks failed (0x107)`
// 是**同一次拔卡**：CD 引脚在一次插拔里会弹跳几十毫秒。26.9.29 拿单次采样当判决，
// 于是 `mount_card()` 的第一档（40 MHz）见到一次 absent 就 `return NOT_FOUND`，
// 后面 20/10 MHz 三档一次都没走（现场只有 `probe 1`，没有 2/3/4），并且在探测窗口里
// 把 `media_invalidated` 焊死。**单次采样不是判决**，这是本病的第一个环。
//
// 现在：连续 3 次一致才算数；中途不一致就交出 `ESP_ERR_INVALID_STATE` ——「还不知道」，
// 由 I/O 去判。CD 快、I/O 准，双信源，谁也不许单独否掉对方。
// 4 ms 的采样窗用 `esp_rom_delay_us()` 忙等而不是 `vTaskDelay()`：100 Hz tick 下
// `pdMS_TO_TICKS(2)` 等于 0，退化成「三连读」——去抖就没了。
// 26.9.31a: CD sampling is debounced. The 344796 ms `present=0` in the field log and the
// 0x107 read failure one second later are the SAME pull: the CD line bounces for tens of
// milliseconds. 26.9.29 treated one sample as a verdict, so mount_card()'s first clock
// step (40 MHz) returned NOT_FOUND on it and the 20/10 MHz steps never ran, while the latch
// was welded shut inside the probe window. One sample is not a verdict.
// Three consecutive agreeing samples are required; a disagreement yields INVALID_STATE
// ("no verdict yet") and the I/O path decides. CD is fast, I/O is authoritative.
#define SD_CD_SAMPLES        3
#define SD_CD_SAMPLE_GAP_US  2000
#define SD_CD_WINDOW_US      12000
static esp_err_t sd_present_stable(bool* present) {
    bool first = false;
    esp_err_t err = read_pico_sd_detect(&first);
    if (err != ESP_OK) return err;
    int agree = 1;
    const int64_t t0 = esp_timer_get_time();
    while (agree < SD_CD_SAMPLES && (esp_timer_get_time() - t0) < SD_CD_WINDOW_US) {
        esp_rom_delay_us(SD_CD_SAMPLE_GAP_US);
        bool again = false;
        err = read_pico_sd_detect(&again);
        if (err != ESP_OK) return err;
        if (again != first) return ESP_ERR_INVALID_STATE;
        ++agree;
    }
    *present = first;
    return ESP_OK;
}

// 失效只影响快照，消费者释放句柄前不得卸载卡。/ Invalidation changes snapshots only; consumers must close handles before unmount.
static void observe_media_locked(bool present) {
    bool inserted = present && !cached_info.present;
    // 26.9.31a：**只有「已挂载的卡消失」才配叫失效**。26.9.29 的条件是
    // `!present && (cached_info.mounted || probe_state == 1)`，第二项把「探测窗口里
    // 的一次 absent」也算成失效 —— 首发挂载失败 + CD 抖一下 = 永久锁存（现场 344796）。
    // 这个闩的含义是「卡正在被拔」，不是「卡永远不可信」。
    // 26.9.31a: only a MOUNTED card disappearing counts as invalidation. The old second
    // clause (`probe_state == 1`) turned "one absent sample during probing" into a permanent
    // latch - first mount fails, CD bounces once, and the device is dead until reboot. The
    // latch means "a card is being pulled", not "cards are never trustworthy again".
    if (!present && cached_info.mounted) media_invalidated = true;
    // 26.9.31a：上升沿清锁存 —— 用户把卡插回去是**人的意图**，不许被旧闩否决。
    // 只在「没有任何已发布挂载」时清闩：卷还挂着的话必须先走 remount（先收句柄、再卸载）。
    // 26.9.31a: a rising edge clears the latch - reinsertion is a human intent and must not
    // be vetoed by an old latch. Only while nothing is published as mounted: a still-mounted
    // volume has to go through remount first (close handles, then unmount).
    if (inserted && media_invalidated && !cached_info.mounted && card == NULL) {
        media_invalidated = false;
        probe_state = 0;
        cached_info.error = ESP_ERR_INVALID_STATE;
    } else if (inserted && probe_state == 2 && !media_invalidated && !cached_info.mounted) {
        // 首次开机无卡时没有文件句柄；后插卡可重新探测。
        // A card inserted after an empty boot has no stale handles and can be probed.
        probe_state = 0;
        cached_info.error = ESP_ERR_INVALID_STATE;
    }
    cached_info.present = present;
    if (!present || media_invalidated) {
        memset(&cached_info, 0, sizeof(cached_info));
        cached_info.present = present;
        cached_info.error = present ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_FOUND;
    }
}

static void publish_info(const read_pico_sd_info_t* info) {
    bool present = false;
    esp_err_t detect_err = sd_present_stable(&present);
    portENTER_CRITICAL(&state_lock);
    cached_info = *info;
    // 26.9.31a：**一次真实成功的挂载就是握手完成的证据**。闩的语义是「卡正在被拔」，
    // 卷都挂上了这句话已经不成立；26.9.29 在这里被 `!present || media_invalidated` 抹成
    // 未挂载 ——「挂载成功」与「界面宣布无卡」同时成立，这就是环③。
    // 26.9.31a: a real successful mount is proof the handshake is over. The latch means
    // "a card is being pulled"; once the volume is mounted that sentence is false. 26.9.29
    // let the latch overwrite the publish, so "mounted" and "no card" were true at once.
    if (info->mounted && info->present && info->error == ESP_OK) media_invalidated = false;
    // 已经确认的拔卡失效不会被后续通信错误或探测成功覆盖。
    // A confirmed removal latch survives later detection faults and probe success.
    if (detect_err == ESP_OK || media_invalidated)
        observe_media_locked(detect_err == ESP_OK ? present : cached_info.present);
    probe_state = 2;
    portEXIT_CRITICAL(&state_lock);
}

// 26.9.21 S14(b)：分片读 FAT，自己数空闲簇——**不把卷锁借出去 2 秒**。
//
// 26.9.19/26.9.20 的账：`f_getfree()` 从 `find_volume()` 拿锁开始，到扫完 8 MB FAT
// 才 `ff_rel_grant`，全程持锁。`scan_shelf()` 里那句「TF余 X MB」也要调它，于是界面
// 线程在锁上排队 2 秒——三轮拔插都是 3.667 s，方差 1 ms，就是这个固定持锁窗口。
//
// 现在改成一次只借总线读 8 KB（约 2 ms），读完立刻还锁、再让一拍。界面任何时刻最多
// 等一片，而不是等一整张表。**仍然持同一把卷锁**，所以不会和 FatFs 的读目录抢总线——
// 只是把一次 2,049 ms 的持有切成 512 次 2 ms。
// 26.9.21 S14(b): read the FAT in slices and count free clusters ourselves, so the volume
// lock is never held for the whole 2 s. f_getfree() holds it from find_volume() to the end
// of an 8 MB scan, and scan_shelf()'s "TF free X MB" hits the same lock - that fixed window
// is why every reinsert landed on 3.667 s with 1 ms of variance. Now each slice borrows the
// bus for ~2 ms and returns the lock before yielding. Same lock, 512 short holds instead of
// one long one, so a directory listing never races the bus.
#define SD_FAT_SLICE_SECTORS 1    // 512 B 一片 / 512 B per slice
#define SD_FAT_SLICE_BYTES   (SD_FAT_SLICE_SECTORS * 512)
#define SD_FAT_MAX_CLUSTERS  0x0FFFFFFFu  // FAT32 表项掩码 / FAT32 entry mask

// 26.9.23 S24：缓冲**搬上栈**。26.9.22 的 2 KB `.bss` 静态缓冲把自己判了死刑：
// 真机 t=3,950 ms（蓝牙栈刚起完，与 26.9.21a 同刻同路径）largest 18,432 → 12,288，
// 面板全程红灯。map 对账：`.bss` 唯一净增就是那 2,057 B。`.bss` 排在堆前面，它每大
// 1 B 堆基址就挪 1 B，所有块边界重排——装箱是非线性的，2 KB 的位移让那个 18,432 的
// 空洞丢了 6,144。**教训：静态内存的对账单位不是「加了多少」，是「把谁挤到了哪里」。**
//
// 栈缓冲用完即逝，堆与 `.bss` 都不留痕：`.bss` 与 26.9.21 逐字节相同 → dram0 布局还原
// → 同一启动分配序列下（分配器是确定性的）largest 应当回到 18,432。**这是可证伪的
// 预测，不是结论**：t≈3,950 的 delayed-boot 回不到 18,432，假说就推翻，方案撤销。
// 代价：片数 ×16（2,048 → 16,384 片），空闲扫描预估 2,300–2,600 ms——不在任何关键
// 路径上，切片让锁的语义（每片借锁一次、即刻归还）原样保留。
// 26.9.23 S24: move the buffer onto the stack. The 2 KB .bss buffer of 26.9.22 convicted
// itself: largest fell 18,432 -> 12,288 at t=3,950 ms (same instant, same path as 26.9.21a)
// and the map shows those 2,057 bytes were the ONLY .bss growth. Every .bss byte shifts the
// heap base, and non-linear bin packing turned a 2 KB shift into a 6 KB lost hole. A stack
// buffer leaves no trace in either the heap or .bss, so the layout - and with it the
// 18,432 hole - must come back. That is a falsifiable prediction, not a conclusion.
// Cost: 16x more slices, idle scan ~2,300-2,600 ms estimated - off every critical path,
// with the lock semantics (borrow once per slice, return immediately) unchanged.

static FATFS *s_mounted_fs;   // 挂载时抓到的卷对象（只读字段 + free_clst）/ the mounted volume

static size_t largest_internal_block(void) {
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
}

static esp_err_t scan_free_clusters(sdmmc_card_t *owner, uint32_t *free_clusters,
                                    uint32_t *total_clusters) {
    FATFS *fs = s_mounted_fs;
    if (fs == NULL || fs->fs_type != FS_FAT32) return ESP_ERR_NOT_SUPPORTED;
    const uint32_t n_fatent = fs->n_fatent;      // 簇数 + 2
    if (n_fatent < 8 || fs->csize == 0) return ESP_ERR_INVALID_SIZE;
    const uint32_t total = n_fatent - 2;
    const uint32_t entries_per_sector = 512 / 4; // FAT32 表项 4 字节 / 4-byte FAT32 entries
    const uint32_t fat_sectors = (n_fatent + entries_per_sector - 1) / entries_per_sector;

    // 26.9.23 S24：512 B 就在这条栈帧里。DMA 要求 4 字节对齐，`aligned(4)` 不能省。
    // 26.9.23 S24: 512 B living in this very stack frame. DMA needs 4-byte alignment;
    // the attribute is not optional.
    const uint32_t slice = SD_FAT_SLICE_SECTORS;
    uint8_t stack_buf[SD_FAT_SLICE_BYTES] __attribute__((aligned(4)));
    BYTE *buf = stack_buf;
    const size_t largest_before = largest_internal_block();

    esp_err_t err = ESP_OK;
    uint32_t free_count = 0;
    for (uint32_t s = 0; s < fat_sectors; s += slice) {
        const uint32_t n = (fat_sectors - s) < slice ? (fat_sectors - s) : slice;
        // 读卡走 `sdmmc_read_sectors()`——和 FatFs 的 diskio 层同一个调用，但不必把
        // `diskio.h` 拖进宿主测试。**卷锁照拿**：这一片（8 KB）读的时候，FatFs 的目录
        // 操作进不来，所以总线上不会有两个事务；锁一还，界面立刻能插队。
        // Read through sdmmc_read_sectors() - the same call FatFs' diskio layer makes - but
        // without dragging diskio.h into the host tests. The volume lock is still taken for
        // this one slice, so the bus never carries two transactions; releasing it lets the
        // UI cut in immediately.
#if FF_FS_REENTRANT
        if (!ff_mutex_take(fs->ldrv)) { err = ESP_ERR_TIMEOUT; break; }
#endif
        // 卡句柄先落到局部量：`card` 是全局，`read_pico_sd_sync()` 可以在 UI 线程把它
        // 置 NULL 再卸载。读之前取一次、读之后比一次，任何一半都不碰野指针。
        // Take the handle into a local: `card` is global and sync() can NULL it from the UI
        // thread. Snapshot before the read, re-check after - never touch a dangling pointer.
        sdmmc_card_t *owner_card = card;
        if (owner_card != owner) { err = ESP_ERR_INVALID_STATE; break; }
        const esp_err_t dr = sdmmc_read_sectors(owner_card, buf, (size_t)(fs->fatbase + s), n);
#if FF_FS_REENTRANT
        ff_mutex_give(fs->ldrv);
#endif
        if (dr != ESP_OK) { err = ESP_FAIL; break; }
        const uint32_t first = s * entries_per_sector;
        const uint32_t count = n * entries_per_sector;
        for (uint32_t k = 0; k < count; ++k) {
            const uint32_t idx = first + k;
            if (idx < 2 || idx >= n_fatent) continue;   // 簇 0/1 是介质描述 / entries 0/1 are reserved
            const BYTE *p = buf + k * 4;
            const uint32_t v = ((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                                ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24)) & SD_FAT_MAX_CLUSTERS;
            if (v == 0) ++free_count;
        }
        // 这 2 秒里用户完全可能把卡拔了或换一张；后面的几十片没有意义。
        // The user can pull or swap the card during these 2 s; the remaining slices are moot.
        if (card != owner) { err = ESP_ERR_INVALID_STATE; break; }
        taskYIELD();
    }
    // 26.9.22 S23(a)：把「这块缓冲到底吃不吃大块」变成一行可核对的数字。
    // 相等 = 静态缓冲确实没进堆，largest 的窟窿另有主；不等 = 归因错了，回来重查。
    // 26.9.22 S23(a): pin the attribution to numbers. Equal largest before and after means
    // the static buffer truly stayed out of the heap; anything else invalidates the analysis.
    const size_t largest_after = largest_internal_block();
    ESP_LOGI(TAG, "fat scan: buf %u B stack, slice %u, largest before=%u after=%u",
             (unsigned)SD_FAT_SLICE_BYTES, (unsigned)slice,
             (unsigned)largest_before, (unsigned)largest_after);
    (void)largest_before;
    (void)largest_after;
    if (err != ESP_OK) return err;

    *free_clusters = free_count;
    *total_clusters = total;
    // 把结果写回 FatFs 自己的缓存：此后任何 `f_getfree()`（本文件以外还有别的调用者）
    // 都直接返回这个数，不必再扫一遍。这一句是整个 S14(b) 的收尾——不作废它，
    // 我们省下的 2 秒会在下一个调用者手里重新花掉。
    // Write the result back into FatFs' own cache so every later f_getfree() is O(1).
#if FF_FS_REENTRANT
    if (!ff_mutex_take(fs->ldrv)) return ESP_ERR_TIMEOUT;
#endif
    fs->free_clst = free_count;
#if FF_FS_REENTRANT
    ff_mutex_give(fs->ldrv);
#endif
    return ESP_OK;
}

// 优先走分片扫描；非 FAT32（exFAT 不会挂到这里）或没抓到卷时退回 FatFs 自己的统计。
// Prefer the sliced scan; fall back to FatFs for non-FAT32 volumes or a missing handle.
static esp_err_t space_total_free(uint64_t *total_bytes, uint64_t *free_bytes) {
    uint32_t free_clusters = 0, total_clusters = 0;
    esp_err_t err = scan_free_clusters(card, &free_clusters, &total_clusters);
    if (err == ESP_OK) {
        const uint64_t cluster_bytes = (uint64_t)s_mounted_fs->csize * 512u;
        *total_bytes = cluster_bytes * total_clusters;
        *free_bytes = cluster_bytes * free_clusters;
        return ESP_OK;
    }
    if (err != ESP_ERR_NOT_SUPPORTED) return err;
    return esp_vfs_fat_info(SD_MOUNT_POINT, total_bytes, free_bytes);
}

static esp_err_t fill_space(read_pico_sd_info_t *info) {
    uint64_t total = 0, available = 0;
    esp_err_t err = space_total_free(&total, &available);
    // 出错或损坏的计数不等于「卡已满」，也不能发生无符号减法下溢。
    // Failed/corrupt counters never mean a full card, and must not underflow subtraction.
    if (err == ESP_OK && (!total || available > total || total > info->capacity_bytes)) err = ESP_FAIL;
    info->space_valid = err == ESP_OK;
    info->space_error = err;
    info->filesystem_bytes = err == ESP_OK ? total : 0;
    info->free_bytes = err == ESP_OK ? available : 0;
    return err;
}

// FAT32 的 FSInfo 可能仍是电脑上旧的计数。每次挂载仅一次作废它，首轮统计只读扫描 FAT。
// Invalidate possibly stale FAT32 FSInfo once per mount; the first read-only FAT scan then counts allocations.
static void invalidate_mount_space(void) {
    BYTE drive = ff_diskio_get_pdrv_card(card);
    if (drive == 0xff) return;
    char path[12]; snprintf(path,sizeof(path),"%u:",(unsigned)drive);
    FF_DIR root;
    if (f_opendir(&root,path) != FR_OK) return;
    FATFS *fs = root.obj.fs;
    (void)f_closedir(&root);
    if (!fs) return;
    // 26.9.21 S14(b)：把这个卷对象留下来。分片扫描要它的 fatbase / n_fatent / csize / drv，
    // 每一样都是挂载时定死的只读字段，不随文件操作变。
    // 26.9.21 S14(b): keep the volume object. The sliced scan needs fatbase / n_fatent /
    // csize / drv - all fixed at mount time.
    s_mounted_fs = fs;
#if FF_FS_REENTRANT
    if (!ff_mutex_take(fs->ldrv)) return;
#endif
    fs->free_clst = UINT32_MAX;
#if FF_FS_REENTRANT
    ff_mutex_give(fs->ldrv);
#endif
}

static void fill_info(read_pico_sd_info_t* info, esp_err_t mount_err, bool with_space) {
    memset(info, 0, sizeof(*info));
    bool present = false;
    esp_err_t detect_err = read_pico_sd_detect(&present);
    // 成功挂载是有卡的证据；检测通信失败不覆盖挂载结果。
    // A successful mount proves presence; failed detection must not override it.
    info->present = detect_err == ESP_OK ? present : mount_err != ESP_ERR_NOT_FOUND;
    info->error = mount_err;
    // 挂载错误本身不证明文件系统问题；仅只读识别到 exFAT 或空白卡才提示。
    // Mount errors alone are inconclusive; only read-only confirmation of exFAT/blank media enables the hint.
    info->needs_format = info->present && filesystem_unreadable;
    if (mount_err != ESP_OK || card == NULL) return;

    info->mounted = true;
    info->needs_format = false;
    strlcpy(info->name, card->cid.name, sizeof(info->name));
    info->capacity_bytes = (uint64_t)card->csd.capacity * card->csd.sector_size;
    // 空间统计失败不覆盖「挂载成功」，仍允许正常读取文件。
    // Allocation-query failure must not turn a mounted filesystem into a mount failure.
    //
    // 26.9.20 S9(b)：**关键路径上不再算空间**。`invalidate_mount_space()` 把 FSInfo 作废
    // 之后，`f_getfree()` 必须逐簇扫完整张 FAT 表——32 GB 卡约 8 MB FAT，1-bit SD 上
    // 约 3.5 s，而这段时间插回早已完成，用户看到的是「卡回来了但界面卡住」。
    // `with_space = false` 时不扫，`space_valid = false` + `space_error = NOT_FINISHED`
    // 明确表示「还没算」，由 probe_task 在**发布之后**补算并回填。
    // 26.9.20 S9(b): no space accounting on the critical path. After
    // invalidate_mount_space() the FSInfo is void, so f_getfree() must scan the whole FAT
    // (about 8 MB on a 32 GB card, ~3.5 s on 1-bit SD) - by which time the reinsert has
    // long finished and the user just sees a frozen screen. With with_space = false the
    // scan is skipped and space_error = NOT_FINISHED says "not counted yet".
    if (!with_space) {
        info->space_valid = false;
        info->space_error = ESP_ERR_NOT_FINISHED;
        return;
    }
    (void)fill_space(info);
}

static void ensure_media_dirs(void) {
    if (mkdir("/sdcard/assets", 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir assets: %d", errno);
    }
    if (mkdir("/sdcard/assets/fonts", 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir assets/fonts: %d", errno);
    }
    if (mkdir("/sdcard/fonts", 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir fonts: %d", errno);
    }
    if (mkdir("/sdcard/books", 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir books: %d", errno);
    }
    if (mkdir("/sdcard/pictures", 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir pictures: %d", errno);
    }
    if (mkdir("/sdcard/dict", 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir dict: %d", errno);
    }
}

// 只识别明确不支持的文件系统，不写卡，也不根据通用 ESP_FAIL 猜测。
// Identify known unsupported media without writing or interpreting generic ESP_FAIL as a filesystem error.
static bool unsupported_boot_sector(const uint8_t *sector) {
    if (!memcmp(sector + 3, "EXFAT   ", 8)) return true;
    for (unsigned i = 0; i < 512; ++i) if (sector[i]) return false;
    return true;
}
static bool probe_unsupported_filesystem(sdmmc_host_t host, const sdmmc_slot_config_t *slot) {
    sdmmc_card_t *probe = calloc(1, sizeof(*probe));
    uint8_t *sector = heap_caps_malloc(512, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    bool owned = false, unsupported = false;
    if (!probe || !sector) goto done;
    host.max_freq_khz = 10000;
    if (host.init() != ESP_OK) goto done; // 不接管其他已初始化会话。/ Never take over another initialized session.
    owned = true;
    if (sdmmc_host_init_slot(host.slot, slot) != ESP_OK || sdmmc_card_init(&host, probe) != ESP_OK ||
        probe->csd.sector_size != 512 || sdmmc_read_sectors(probe, sector, 0, 1) != ESP_OK) goto done;
    unsupported = unsupported_boot_sector(sector);
    if (!unsupported && sector[510] == 0x55 && sector[511] == 0xaa) {
        uint32_t starts[4] = {0};
        for (unsigned i = 0; i < 4; ++i) {
            const uint8_t *entry = sector + 446 + 16 * i;
            if (!entry[4]) continue;
            starts[i] = (uint32_t)entry[8] | (uint32_t)entry[9] << 8 | (uint32_t)entry[10] << 16 | (uint32_t)entry[11] << 24;
        }
        for (unsigned i = 0; i < 4 && !unsupported; ++i) {
            if (!starts[i] || starts[i] >= probe->csd.capacity) continue;
            if (sdmmc_read_sectors(probe, sector, starts[i], 1) != ESP_OK) goto done;
            // 分区必须明确标识 exFAT；普通 FAT 或未知损坏不建议格式化。
            // A partition must explicitly identify exFAT; do not suggest formatting FAT or unknown corruption.
            unsupported = !memcmp(sector + 3, "EXFAT   ", 8);
        }
    }
done:
    if (owned) host.deinit();
    free(probe); heap_caps_free(sector);
    return unsupported;
}

// 26.9.20 S12/D4：阶段时刻改成**相对**本次探测起点的毫秒数。
// 26.9.18/26.9.19 存的是绝对毫秒的低 16 位，142.6 s 处回绕成 11,557——真实值是
// 142,629 ms，读日志的人会把 11,557 当成「11.5 秒」，整条时间线错一个小时。
// 审计 26.9.20 的方法论第 2 条：任何存绝对时间戳的变量都必须在**源头**扣掉起点。
// 26.9.20 S12/D4: stage instants are milliseconds SINCE THIS PROBE STARTED. 26.9.18/19
// stored the low 16 bits of the absolute count, which wrapped at 142.6 s into 11,557 and
// read like "11.5 s". Subtract the origin at the source, never store an absolute stamp.
static int64_t s_probe_t0;   // 本次探测起点（µs）/ origin of this probe

static uint16_t sd_stage_ms(void) {
    const int64_t dt = esp_timer_get_time() - s_probe_t0;
    return (uint16_t)((dt > 0 ? dt : 0) / 1000);
}
#define SD_STAGE_MS() sd_stage_ms()

// `a` 的位域（26.9.20 S12/D5）：低 3 位阶段号，0x08 = 进入点（还没有结果），
// 0x80 = 成功。26.9.19 只有 stage 3 带结果位，于是 stage 1/2 永远印 FAIL——而两次
// 挂载全都成功了。打印端不再用「有没有 0x80」判断「成功还是失败」，而是先看是不是
// 进入点：**没有结果不是失败**，这是同族错误的第三次，必须从语义上断根。
// `a` bits (26.9.20 S12/D5): low 3 = stage, 0x08 = entry point (no result yet), 0x80 = ok.
#define SD_STAGE_ENTER(stage) ((uint8_t)((stage) | 0x08u))

static esp_err_t mount_card(bool format_if_failed) {
    // 阶段 1 = 进入挂载。与前面 `poll_media` 的 sd-state note 相隔的就是「判定→建任务」。
    // Stage 1 = entering the mount. What separates it from poll_media's sd-state note is
    // the judgement and the task creation.
    pico_bootbb_note(PICO_BOOTBB_NOTE_SD_STAGE, SD_STAGE_ENTER(1), SD_STAGE_MS());
    filesystem_unreadable = false;
    // 26.9.17 T3：`card` 非空意味着上一张卡没有被卸载。以前这里直接返回 ESP_OK，
    // 于是「拔卡后忘了 unmount」会被 `fill_info()` 当成挂载成功——UI 宣布有卡，
    // 实际每次读都是 0x107。这是比「不认卡」更难查的假成功，必须让它以错误码暴露。
    // 合法调用方要么在 card == NULL 时进来（probe_task / format），要么根本不该来。
    // 26.9.17 T3: a non-NULL `card` means the previous card was never unmounted.
    // Returning ESP_OK here let `fill_info()` report a successful mount for a card
    // that had already left - the UI says "card ready" while every read fails with
    // 0x107. That false success is harder to debug than a plain failure, so expose
    // it as an error instead. Every legitimate caller arrives with card == NULL.
    if (card != NULL) {
        ESP_LOGE(TAG, "stale card, close before mount");
        return ESP_ERR_INVALID_STATE;
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    // 1-bit 只能靠提时钟换带宽。20MHz 默认对随机小读太慢，40MHz 多数卡能稳住。
    // / 1-bit only buys bandwidth by raising the clock. 20 MHz is too slow for
    // random small reads; 40 MHz holds on most cards.
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = SD_PIN_CLK;
    slot.cmd = SD_PIN_CMD;
    slot.d0 = SD_PIN_D0;
    slot.d1 = GPIO_NUM_NC;
    slot.d2 = GPIO_NUM_NC;
    slot.d3 = GPIO_NUM_NC;
    slot.cd = GPIO_NUM_NC;
    slot.wp = GPIO_NUM_NC;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = format_if_failed,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

    // 冷启动先等待并重试高速，再降速；CMD6 忙也可能返回 INVALID_STATE，不得直接放弃。
    // Settle/retry high speed before lowering clocks; CMD6 busy may return INVALID_STATE and must retry.
    const int clocks[] = {SDMMC_FREQ_HIGHSPEED, SDMMC_FREQ_HIGHSPEED, 20000, 10000};
    esp_err_t err = ESP_FAIL;
    // 26.9.31a（裁决 4 的状态机完整性）：每次 mount 前显式 `sdmmc_host_init()`。
    // 读码事实（driver 源码）：
    //   · `sdmmc_host_init()` **幂等** —— `if (s_ctlr) return ESP_OK;`
    //     `esp_driver_sdmmc/legacy/src/sdmmc_host.c:78-83`
    //   · `sdmmc_host_deinit()` **不幂等** —— 第二次调用时两个 slot 已被置 NULL，
    //     `sd_host_del_controller(NULL)` 经 `SD_HOST_ARG_CHECK` 返回 `ESP_ERR_INVALID_ARG`
    //     （`sdmmc_host.c:173-189` + `esp_driver_sd_intf/sd_host.c:65-71`、`:19-24`）。
    //     它不会二次释放，但会报错，且 `s_ctlr` 已被清空。
    //   · 于是 deinit 之后**必须**重新 init 才能 `init_slot`
    //     （`sdmmc_host.c:146` 的 add_slot 直接用 `s_ctlr`）。
    // 「复位」在这里的语义是「回到已知初始态」，所以每次挂载前都先确保 init 过；
    // 卷还挂着时绝不 deinit —— VFS 仍持有 card 与 pdrv，那是悬空句柄。
    // 26.9.31a (ruling 4): call sdmmc_host_init() before every mount. Source facts:
    // init is idempotent (`if (s_ctlr) return ESP_OK`, sdmmc_host.c:78-83); deinit is NOT
    // (second call hits sd_host_del_controller(NULL) -> ESP_ERR_INVALID_ARG; no double free,
    // but it errors and s_ctlr is already NULL); after deinit init_slot needs a fresh init.
    // "Reset" means "back to a known state", so ensure init before mounting; never deinit
    // while the volume is mounted - VFS still holds card and pdrv.
    {
        const esp_err_t host_err = sdmmc_host_init();
        if (host_err != ESP_OK) ESP_LOGW(TAG, "host init before mount: %s", esp_err_to_name(host_err));
    }
    for (size_t attempt = 0; attempt < sizeof(clocks) / sizeof(clocks[0]); ++attempt) {
        bool present = false;
        // 26.9.31a：去抖后的 CD。**一次 absent 只说明「还不知道」**——26.9.29 在这里
        // 直接 `return ESP_ERR_NOT_FOUND`，把三档降速全跳过（现场只有 `probe 1`）。
        // 26.9.31a: debounced CD. One absent sample only means "no verdict yet";
        // 26.9.29 returned NOT_FOUND from here and skipped all three lower clock steps.
        esp_err_t detect_err = sd_present_stable(&present);
        if (detect_err == ESP_OK && !present) return ESP_ERR_NOT_FOUND;
        if (detect_err != ESP_OK) ESP_LOGW(TAG, "SD detect unavailable: %s; try card I/O", esp_err_to_name(detect_err));
        host.max_freq_khz = clocks[attempt];
        card = NULL;
        // 阶段 2 = 真正进 `esp_vfs_fat_sdmmc_mount`（host init + slot + card init + f_mount
        // 全在它内部），阶段 3 = 它返回。26.9.17 现场这段是 3.535 s，而驱动自己的
        // `input line delay` 只出现一次，说明四档降速一次都没走——4 s 全在一次成功的
        // 挂载内部。2→3 的差值就是它的真身。
        // Stage 2 = entering esp_vfs_fat_sdmmc_mount (host init + slot + card init + f_mount
        // all live inside it), stage 3 = its return. In the 26.9.17 field log this span was
        // 3.535 s with `input line delay` appearing once, i.e. no clock fallback at all.
        pico_bootbb_note(PICO_BOOTBB_NOTE_SD_STAGE, SD_STAGE_ENTER(2), SD_STAGE_MS());
        err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mount_config, &card);
        // `a` 的低 3 位是阶段号，0x80 表示成功；没有 0x08（进入位）= 这个点**带结果**。
        // Low 3 bits are the stage, 0x80 means ok; no 0x08 means this point carries a result.
        pico_bootbb_note(PICO_BOOTBB_NOTE_SD_STAGE,
                         (uint8_t)(3u | (err == ESP_OK ? 0x80u : 0u)), SD_STAGE_MS());
        if (err == ESP_OK) break;
        if (err == ESP_ERR_NO_MEM || err == ESP_ERR_INVALID_ARG) break;
        ESP_LOGW(TAG, "SD probe %u at %d kHz failed: %s", (unsigned)attempt + 1,
                 clocks[attempt], esp_err_to_name(err));
        card = NULL;
        // VFS mount 在失败时只清理自己建立的 host；不额外 deinit 别的存储会话。
        // Failed VFS mount cleans up its own host; never forcibly deinit another storage session.
        if (attempt + 1 < sizeof(clocks) / sizeof(clocks[0])) vTaskDelay(pdMS_TO_TICKS(250));
    }
    if (err != ESP_OK) {
        card = NULL;
        ESP_LOGW(TAG, "Mount failed: %s", esp_err_to_name(err));
        if (!format_if_failed && err == ESP_FAIL) filesystem_unreadable = probe_unsupported_filesystem(host, &slot);
    } else {
        // E-2：**真挂载成功处**才 +1。不放在 `publish_info()`——空间回填会再发布一次
        // （`probe_fill_space_deferred`），那会把同一张卡算成第二次挂载，让行窗口被
        // 重置到 20 s 之后，摘抄看起来像"永远不落盘"。
        // E-2: bumped at the successful mount, NOT in publish_info() - the deferred space
        // scan publishes a second time, and counting that would restart the yield window
        // 20 s later, which reads as "excerpts never flush".
        portENTER_CRITICAL(&state_lock);
        ++s_mount_epoch;
        // P1-1：新一次挂载 = 新一笔账。上一张卡的连续超时不属于这一张——
        // 不清零的话，换卡后第一次心跳就会带着旧的 8 次直接判死（宿主测试当场抓到）。
        // P1-1: a new mount starts a new ledger. Carry-over made the first heartbeat
        // after a swap die instantly with the previous card's count.
        s_io_fails = 0;
        s_io_fail_ms = 0;
        s_io_next_ms = 0;
        portEXIT_CRITICAL(&state_lock);
        invalidate_mount_space();
        ensure_media_dirs();
    }
    return err;
}

/// 挂载代号：每次挂载成功 +1 的单调计数，供 app 侧把「挂载事件」与「状态快照」分开。
/// 0 表示本次开机还没挂载过。
/// / Monotonic mount epoch: one increment per successful mount, so the app can tell an
/// event from a snapshot. Zero means nothing has been mounted this boot.
uint32_t read_pico_sd_mount_epoch(void) {
    portENTER_CRITICAL(&state_lock);
    const uint32_t epoch = s_mount_epoch;
    portEXIT_CRITICAL(&state_lock);
    return epoch;
}

// ---- 31b P1-1：掉盘的第二个信源：I/O 连续超时 --------------------------------
//
// 「卡没了」此前只有一个信源：CD 脚（`read_pico_sd_detect()`，无去抖、单次采样）。
// 26.9.31 现场 present 在 888/892/923/927 行之间来回跳，就是单一信源在抖。CD 脚擅长
// 「快」——卡一离开槽位它立刻知道；它答不了「准」——卡还在槽里、但已经读不出来了。
//
// 第二个信源只能**主动读**。原因不是设计偏好：本工程所有读卡都走 FatFs/VFS，没有
// 自建 diskio 层可挂钩（`diskio_sdmmc` 是 IDF vendor 件，不在本项目改动范围），而判据
// 要求的场景恰恰是「没人读卡的时候拔卡」——首页静止、CD 被飞线短接到 present。没有
// 心跳，这个场景永远等不到第一个 0x107。
//
// **8 次是快路径，墙钟才是判据**：单次 `read_blocks` 最坏 2×1,000 ms（失败后还要发
// CMD13 取状态），8 次 = 16 s，装不进「3 s 内判 lost」；实测单次 117 ms，8 次 ≈ 0.94 s
// 才装得进。所以判据写成 `now - t0 >= 3 s` **无条件**成立，次数只在卡响应快时提前收工。
// 两处参数不同源——把它们写成同一个数，下一次调采样率就会互相打架（§13.5 第一课的
// 「段值骗人」同族：一个数字承担两个语义，读的人无法判断该改哪一个）。
//
// 采样节拍：正常态 1 s 一次（512 B 读 ≈ 1 ms，占空比 0.1%），失败态 100 ms 一次
// （让连续失败在 3 s 内攒够 8 次）。**失败态不重试到 8 次再返回**：那是把 app_loop
// 的 5 ms tick 堵住最多 3 s，UI 会肉眼可见地卡；跨拍累计才是唯一不伤主循环的形态。
//
// 为什么只数 0x107：其它错误码是驱动内部状态（INVALID_STATE / NO_MEM / 参数错），
// 它们说明「这次没读成」，不说明「卡不答」。把它们一并计入，一次内存紧张就能把
// 一张好卡判死——「准」是这条信源存在的全部理由。
#define SD_IO_FAIL_LIMIT     8u
#define SD_IO_DEAD_MS        3000u
#define SD_IO_IDLE_GAP_MS    1000u
#define SD_IO_FAIL_GAP_MS    100u

static uint32_t io_now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void io_watch_reset(void) {
    s_io_fails = 0;
    s_io_fail_ms = 0;
}

esp_err_t read_pico_sd_watch_io(uint8_t *fail_count, uint16_t *elapsed_ms) {
    if (fail_count != NULL) *fail_count = 0;
    if (elapsed_ms != NULL) *elapsed_ms = 0;

    portENTER_CRITICAL(&state_lock);
    const bool armed = cached_info.mounted && !media_invalidated && probe_state == 2;
    const uint32_t now = io_now_ms();
    const bool due = s_io_next_ms == 0 || (int32_t)(now - s_io_next_ms) >= 0;
    portEXIT_CRITICAL(&state_lock);

    sdmmc_card_t *owner = card;
    if (!armed || owner == NULL) {
        io_watch_reset();
        s_io_next_ms = 0;
        return ESP_ERR_INVALID_STATE;
    }
    if (!due) return ESP_OK;

    // 512 B 读一个固定扇区（MBR/引导扇区）。**不读 FatFs 的 FAT 或数据区**：心跳要的是
    // 「卡还答不答」，不是「文件系统还对不对」——后者是挂载与校验的职责，混进来会让
    // 一次目录损坏被记成掉卡。
    uint8_t buf[512] __attribute__((aligned(4)));
    FATFS *fs = s_mounted_fs;
    esp_err_t err;
#if FF_FS_REENTRANT
    if (fs != NULL && !ff_mutex_take(fs->ldrv)) {
        // 卷锁被别人拿着：这一拍没读到，不是卡坏了。按失败间隔退一步，不计数。
        portENTER_CRITICAL(&state_lock);
        s_io_next_ms = now + SD_IO_FAIL_GAP_MS;
        portEXIT_CRITICAL(&state_lock);
        return ESP_OK;
    }
#endif
    err = sdmmc_read_sectors(owner, buf, 0, 1);
#if FF_FS_REENTRANT
    if (fs != NULL) ff_mutex_give(fs->ldrv);
#endif

    portENTER_CRITICAL(&state_lock);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        // 驱动内部状态，不算「卡不答」（见上）。
        s_io_next_ms = now + SD_IO_FAIL_GAP_MS;
        portEXIT_CRITICAL(&state_lock);
        return ESP_OK;
    }
    if (err == ESP_OK) {
        io_watch_reset();
        s_io_next_ms = now + SD_IO_IDLE_GAP_MS;
        portEXIT_CRITICAL(&state_lock);
        return ESP_OK;
    }
    if (s_io_fails == 0) s_io_fail_ms = now ? now : 1u;  // 0 保留给「无失败」
    if (s_io_fails < 0xFFu) ++s_io_fails;
    const uint8_t fails = s_io_fails;
    const uint32_t since = (uint32_t)(now - s_io_fail_ms);
    const bool dead = fails >= SD_IO_FAIL_LIMIT || since >= SD_IO_DEAD_MS;
    s_io_next_ms = now + SD_IO_FAIL_GAP_MS;
    if (!dead) {
        portEXIT_CRITICAL(&state_lock);
        return ESP_OK;
    }
    // 判死：置失效闩，快照落成「卡不在」。
    // 为什么**不改 present**：CD 说在，那是它的事实；I/O 说的是「卷不可用」。两个事实
    // 各自保留，拼一起才是完整现场——把 present 改成 false 等于替 CD 撒谎，
    // 下一个看日志的人会以为是槽位开关坏了。
    //
    // 码用 NOT_FOUND 是 31b 的契约（「这个卷没了」），**但它不决定看门狗入口**：
    // 下一次 `get_info()` 会走 `observe_media_locked()`，那里在锁存期把快照整体重写成
    // `{present=1, mounted=0, error=INVALID_STATE}`（31a 环③的修法，实测 259）。
    // app_loop 拿到的是 INVALID_STATE，兜底分支的 `err != NOT_FOUND` 照样放行。
    //
    // 这一点合流时差点判错：曾据「NOT_FOUND 会关掉看门狗」改了码，实测才发现
    // 真正的偏差在**宿主桩**——桩返回 NOT_FOUND，真件返回 INVALID_STATE，
    // 于是「判死之后还有人碰这张卡」这条路径在宿主侧从来没被测到（§13.12a 靶子为假）。
    // 桩已按真件改正，码保持原样。
    //
    // NOT_FOUND is the 31b contract for "the volume is gone", and it does NOT decide the
    // watchdog door: the next get_info() runs observe_media_locked(), which rewrites the
    // snapshot to {present=1, mounted=0, error=INVALID_STATE} while latched (measured 259).
    // The merge nearly "fixed" the wrong thing: the real drift was in the host stub, which
    // returned NOT_FOUND where the component returns INVALID_STATE - so "someone still
    // touches this card after death" was never exercised on the host side.
    media_invalidated = true;
    cached_info.mounted = false;
    cached_info.error = ESP_ERR_NOT_FOUND;
    portEXIT_CRITICAL(&state_lock);

    if (fail_count != NULL) *fail_count = fails;
    if (elapsed_ms != NULL) *elapsed_ms = (uint16_t)(since > 0xFFFFu ? 0xFFFFu : since);
    ESP_LOGE(TAG, "sd: io dead (cd still present): %u timeouts over %u ms",
             (unsigned)fails, (unsigned)since);
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t close_card(void) {
    if (card != NULL) {
        esp_err_t err = esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, card);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "unmount %s", esp_err_to_name(err));
            // 26.9.31a：卸载失败留痕（stage 4 = 卸载带结果，bit7=0 即失败）。**不在这里
            // 硬 deinit**：`esp_vfs_fat_sdcard_unmount()` 失败时 VFS 仍注册着 pdrv、`card`
            // 仍有效，deinit 会把它们变成悬空句柄——比不卸载更糟。复位手段是
            // 「补卸载闩锁（app_loop）+ 每次 mount 前的 host_init」这两条，不是这一锤子。
            // 26.9.31a: a failed unmount is recorded (stage 4, bit7=0 = failure). Never hard
            // deinit here: VFS still has the pdrv registered and card is still valid, so
            // deinit would turn both into dangling handles. The cure is the release latch in
            // app_loop plus host_init before every mount.
            pico_bootbb_note(PICO_BOOTBB_NOTE_SD_STAGE, (uint8_t)4u, SD_STAGE_MS());
            return err;
        }
        card = NULL;
        // 卷对象随卸载失效：留着它只会让下一次扫描读到一个已经不存在的卷。
        // The volume object dies with the unmount; keeping it would scan a dead volume.
        s_mounted_fs = NULL;
    }
    return ESP_OK;
}

// 26.9.22 S23(b)：**扫表搬出挂载流程**。
//
// 26.9.21 把扫描切成了 512 片，界面最多等 8 KB（约 2 ms），这一刀确实把「卡住」变成了
// 「有点慢」。但 26.9.21 真机仍然给出两个数：拔插到书架 1,813–1,839 ms，且第 4 次拔插
// 时目录读被挤到 2,283 ms——因为扫描虽然不长时间持锁，却一直在**抢总线**，目录读要跟它
// 排队。**切锁解决的是「谁持锁」，解决不了「谁在占用总线」。**
//
// 所以这版干脆不问「怎么扫得快」，改问「什么时候扫才不碍事」：
//   插卡 → 挂载 → 发布 → **等 20 s 空闲** → 再扫（或进文件页时提前扫）。
// 插卡到书架这条路上，一把锁都不用拿，一次总线都不占。
//
// `s_space_urge` 是「有人主动问」的那条捷径：`read_pico_sd_refresh_space()` 在
// NOT_FINISHED 时置位，等待中的扫描立刻开始（进文件页就是这个时机）。
// The mount-to-shelf path holds no lock and touches no bus at all. `cached_info.space_urge`
// is the "someone asked" shortcut, set by refresh_space() when the user opens the file page.
// 26.9.23 S24：`s_mount_done_us` 删除——「mount+ 多少 ms」与 bootbb 事件行的
// `sd-space ok at=… ms` 是同一事实的两份取证，删掉重复那份，换回 8 字节的零痕迹。
// 26.9.23 S24: s_mount_done_us deleted. "mount+ N ms" and the bootbb event's at= are two
// copies of the same fact; the duplicate goes, and 8 zero-trace bytes come back.
#define SD_SPACE_IDLE_MS      20000u
#define SD_SPACE_IDLE_STEP_MS 100u

// 26.9.20 S9(b)：挂载后补算空间。必须**在 publish 之后**调用，且要被卡的身份检查
// 守住——等待与扫描加起来 20 秒以上，用户完全可能把卡拔了或又插了一张。
// 26.9.20 S9(b): the deferred free-space scan. It runs AFTER publish and is guarded by the
// card's identity, because the user can pull or swap the card during the wait and the scan.
static void probe_fill_space_deferred(sdmmc_card_t *owner, read_pico_sd_info_t *info) {
    // 26.9.21 S14(a)：26.9.20 那 1,200 ms 的「让首屏先走」已删除——首屏（书架）当时恰恰
    // 在等同一把卷锁，那 1.2 s 谁也没让过。本版不是又把它加回来：这 20 s 期间**一次锁都
    // 不拿、一个扇区都不读**，与「等一等再抢锁」是两件事。
    // 26.9.21 S14(a): the old 1,200 ms wait is still gone. This 20 s is not the same thing:
    // no lock is taken and no sector is read during it.
    //
    // 降到最低优先级：这是纯后台工作，任何时候都该让位给界面。
    // Drop to the lowest priority: background work must always yield to the UI.
    vTaskPrioritySet(NULL, 1);
    uint32_t idle_ms = 0;
    while (idle_ms < SD_SPACE_IDLE_MS) {
        // 卡已经不是当初那张（拔了 / 换了 / 卸载了）→ 这条扫描线作废。
        // A different card is in the slot now; this scan line is moot.
        if (card != owner) return;
        if (cached_info.space_urge) break;
        vTaskDelay(pdMS_TO_TICKS(SD_SPACE_IDLE_STEP_MS));
        idle_ms += SD_SPACE_IDLE_STEP_MS;
    }
    if (card != owner) return;
    const int64_t t0 = esp_timer_get_time();
    const esp_err_t err = fill_space(info);
    // 三个数各自先落成局部量：宿主测试的日志桩把变参整个丢掉，直接写进 ESP_LOGI
    // 会让 `-Werror=unused-variable` 把这里判成死代码。
    // Bind the numbers to locals first: the host log stub drops varargs entirely, so
    // writing them straight into ESP_LOGI trips -Werror=unused-variable.
    const uint32_t scan_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    const unsigned long long free_mb = (unsigned long long)(info->free_bytes / (1024 * 1024));
    // 26.9.23 S24：512 B 缓冲住在 `sd_probe`（4,096）的栈帧里。安不安全不靠猜——
    // 高水位直接印出来；**余量 <256 B 视为方案自败**（审计批复原话）。
    // 26.9.23 S24: the 512 B buffer lives in sd_probe's (4,096) stack frame. Whether that is
    // safe is printed, not guessed; a margin below 256 B fails the plan by its own terms.
    const UBaseType_t probe_hwm = uxTaskGetStackHighWaterMark(NULL);
    pico_bootbb_note(PICO_BOOTBB_NOTE_SD_SPACE, err == ESP_OK ? 1u : 0u, SD_STAGE_MS());
    // 「mount+ 多少 ms」不再打印：它与 bootbb 事件行的 `sd-space ok at=… ms` 是同一
    // 事实，重复取证值 8 字节 .bss，不值（26.9.23 S24）。
    // The "mount+ N ms" figure is dropped: the bootbb event's at= already carries the same
    // fact, and the duplicate is not worth 8 zero-trace bytes (26.9.23 S24).
    ESP_LOGI(TAG, "space scan: %s in %u ms (idle %u ms), free=%llu MB, probe hwm=%u",
             esp_err_to_name(err), (unsigned)scan_ms, (unsigned)idle_ms,
             free_mb, (unsigned)probe_hwm);
    (void)scan_ms;
    (void)idle_ms;
    (void)free_mb;
    (void)probe_hwm;
    if (card == owner && err == ESP_OK) publish_info(info);
}

static void probe_task(void* arg) {
    (void)arg;
    read_pico_sd_info_t info = { 0 };
    esp_err_t detect_err = read_pico_sd_detect(&info.present);
    if (detect_err == ESP_OK && !info.present) {
        info.error = ESP_ERR_NOT_FOUND;
        publish_info(&info);
        ESP_LOGI(TAG, "SD_CD absent, skip mount");
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = mount_card(false);
    // 26.9.20 S9(b)：`with_space = false` —— 先发布「挂载成功」，空间随后补。
    // 26.9.20 S9(b): publish the mount first, account for the space afterwards.
    fill_info(&info, err, false);
    if (err == ESP_OK) {
        sdmmc_card_t *owner = card;
        ESP_LOGI(
            TAG,
            "Mounted %s, capacity=%llu MB (space pending) at %u ms",
            info.name,
            (unsigned long long)(info.capacity_bytes / (1024 * 1024)),
            (unsigned)SD_STAGE_MS()
        );
        publish_info(&info);
        // 26.9.23 S24：时刻戳已删——「mount+ 多少 ms」由 bootbb 的 at= 承担。
        // 26.9.23 S24: the timestamp is gone; the bootbb at= carries the figure.
        probe_fill_space_deferred(owner, &info);
    } else {
        publish_info(&info);
    }
    vTaskDelete(NULL);
}

esp_err_t read_pico_sd_start_probe(void) {
    bool present = true;
    esp_err_t detect_err = sd_present_stable(&present);
    portENTER_CRITICAL(&state_lock);
    if (detect_err == ESP_OK) observe_media_locked(present);
    esp_err_t err = ESP_OK;
    bool recovered = false;
    if (probe_state == 1) err = ESP_ERR_NOT_FINISHED;
    else if (media_invalidated) {
        // 26.9.31a：锁存但卡在位 → **不再吐旧错误**。26.9.29 这里是
        // `err = cached_info.error`（恒 INVALID_STATE），而 app_loop 的兜底分支恰恰靠
        // `err == INVALID_STATE` 触发 —— 每次兜底都只换来同一句 `probe start failed`，
        // 探测任务一次都没建（现场 382146 那条无限重复，就这样到关机）。
        // 卡在位、且没有残留句柄（`card == NULL`）= 可以真探一次；有残留时仍交给
        // remount：先卸卷、再清闩，顺序不能倒。
        // 26.9.31a: latched but the card is present -> stop returning the stale error.
        // 26.9.29 returned cached_info.error (always INVALID_STATE), and app_loop's fallback
        // fires exactly on INVALID_STATE - so every fallback produced the same
        // `probe start failed` and no probe was ever created. Present + no stale handle
        // (card == NULL) means a real probe; with a stale handle remount must go first.
        if (!present || card != NULL) {
            err = cached_info.error;
        } else {
            media_invalidated = false;
            memset(&cached_info, 0, sizeof(cached_info));
            cached_info.present = true;
            cached_info.error = ESP_ERR_NOT_FINISHED;
            probe_state = 1;
            recovered = true;
        }
    } else if (probe_state == 2) err = cached_info.mounted ? ESP_OK :
        cached_info.error != ESP_OK ? cached_info.error : ESP_FAIL;
    else if (!present) {
        probe_state = 2;
        err = ESP_ERR_NOT_FOUND;
    } else {
        memset(&cached_info, 0, sizeof(cached_info));
        cached_info.present = true;
        cached_info.error = ESP_ERR_NOT_FINISHED;
        probe_state = 1;
    }
    bool start = err == ESP_OK && probe_state == 1;
    portEXIT_CRITICAL(&state_lock);
    if (!start) return err;
    if (recovered) ESP_LOGW(TAG, "sd: latch cleared by probe (card present, no stale handle)");
    // 26.9.20 S12/D4：时间原点在**发起探测的那一刻**取，之后所有 sd-stage 的 `at=`
    // 都是「距此刻多少毫秒」。这样 `at=343` 就是 343 ms，不再是回绕后的 11,557。
    // Take the origin the instant a probe is launched; every sd-stage `at=` is then
    // milliseconds since here, never a wrapped absolute stamp.
    s_probe_t0 = esp_timer_get_time();
    // 新一次探测：上一次留下的「催扫」请求作废，否则新卡的空闲等待会立刻被跳过。
    // A new probe invalidates the previous request, or the new card's idle wait would be
    // skipped the moment it starts.
    portENTER_CRITICAL(&state_lock);
    cached_info.space_urge = false;   // 一次性请求，开扫即清 / one-shot request
    portEXIT_CRITICAL(&state_lock);
    BaseType_t created = xTaskCreate(
        probe_task, "sd_probe", 4096, NULL, 5, NULL
    );
    if (created != pdPASS) {
        portENTER_CRITICAL(&state_lock);
        probe_state = 0;
        cached_info.error = ESP_ERR_NO_MEM;
        portEXIT_CRITICAL(&state_lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_ERR_NOT_FINISHED;
}

esp_err_t read_pico_sd_get_info(read_pico_sd_info_t* info) {
    if (info == NULL) return ESP_ERR_INVALID_ARG;
    bool present = false;
    esp_err_t detect_err = sd_present_stable(&present);
    portENTER_CRITICAL(&state_lock);
    if (detect_err == ESP_OK) observe_media_locked(present);
    *info = cached_info;
    esp_err_t err = probe_state == 0 && detect_err != ESP_OK ? detect_err :
        !info->present || media_invalidated ? info->error :
        probe_state == 0 ? ESP_ERR_INVALID_STATE :
        probe_state == 1 ? ESP_ERR_NOT_FINISHED : info->error;
    portEXIT_CRITICAL(&state_lock);
    return err;
}

// 驱动操作期间占用忙状态，不在临界区执行 I/O。/ Reserve busy state across driver I/O outside the critical section.
static bool begin_operation(void) {
    portENTER_CRITICAL(&state_lock);
    bool ready = probe_state != 1;
    if (ready) probe_state = 1;
    portEXIT_CRITICAL(&state_lock);
    return ready;
}

esp_err_t read_pico_sd_refresh_space(void) {
    read_pico_sd_info_t info;
    esp_err_t err = read_pico_sd_get_info(&info);
    if (err != ESP_OK || !info.mounted) return err != ESP_OK ? err : ESP_ERR_INVALID_STATE;
    // 26.9.20 S9(b)：本次挂载的空间还是「待统计」（后台正在排队/正在扫）时，**不要**
    // 在调用者（界面任务）里再扫一遍——那等于把刚从挂载路径上摘下来的 3.5 s 搬到
    // 界面线程里。等后台回填即可，`publish_info()` 会让界面自己重画。
    // 26.9.20 S9(b): while this mount's space is still pending, do NOT rescan on the
    // caller's thread - that just moves the 3.5 s from the mount path into the UI task.
    //
    // 26.9.22 S23(b)：但「有人主动问」是一条正当的加速理由——进文件页就是这个时机。
    // 置一个请求位，让后台那个还在 20 s 空闲里睡着的扫描立刻开工，扫描仍在后台任务、
    // 仍在分片、仍然每片还锁，界面线程一个字都不干。
    // 26.9.22 S23(b): an explicit request is a fair reason to skip the idle wait (opening
    // the file page is that moment). Set the request bit so the sleeping background scan
    // starts now; the scan stays on the background task, still sliced, still releasing the
    // lock per slice. The caller's thread does nothing.
    if (info.space_error == ESP_ERR_NOT_FINISHED) {
        portENTER_CRITICAL(&state_lock);
        cached_info.space_urge = true;
        portEXIT_CRITICAL(&state_lock);
        return ESP_ERR_NOT_FINISHED;
    }
    if (!begin_operation()) return ESP_ERR_NOT_FINISHED;
    err = fill_space(&info);
    publish_info(&info);
    return err;
}

esp_err_t read_pico_sd_remount(void) {
    if (!begin_operation()) return ESP_ERR_NOT_FINISHED;
    esp_err_t err = close_card();
    if (err != ESP_OK) {
        read_pico_sd_info_t info = { .present = true, .error = err };
        (void)read_pico_sd_detect(&info.present);
        publish_info(&info);
        return err;
    }
    portENTER_CRITICAL(&state_lock);
    memset(&cached_info, 0, sizeof(cached_info));
    media_invalidated = false;
    probe_state = 0;
    portEXIT_CRITICAL(&state_lock);
    return read_pico_sd_start_probe();
}

esp_err_t read_pico_sd_sync(void) {
    if (!begin_operation()) return ESP_ERR_NOT_FINISHED;
    esp_err_t err = close_card();
    if (err != ESP_OK) {
        read_pico_sd_info_t info = { .present = true, .mounted = true, .error = err };
        (void)read_pico_sd_detect(&info.present);
        publish_info(&info);
        return err;
    }
    read_pico_sd_info_t info = { .error = ESP_ERR_INVALID_STATE };
    publish_info(&info);
    return ESP_OK;
}

esp_err_t read_pico_sd_format(void) {
    read_pico_sd_info_t current;
    (void)read_pico_sd_get_info(&current);
    portENTER_CRITICAL(&state_lock);
    bool invalid = media_invalidated;
    bool busy = probe_state == 1;
    if (!invalid && !busy) probe_state = 1;
    portEXIT_CRITICAL(&state_lock);
    if (invalid) return current.present ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_FOUND;
    if (busy) return ESP_ERR_NOT_FINISHED;
    bool present = false;
    if (read_pico_sd_detect(&present) == ESP_OK && !present) {
        read_pico_sd_info_t info = { .error = ESP_ERR_NOT_FOUND };
        publish_info(&info);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = ESP_OK;
    if (card == NULL) {
        err = mount_card(true);
    } else {
        err = esp_vfs_fat_sdcard_format(SD_MOUNT_POINT, card);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "format %s", esp_err_to_name(err));
        }
    }

    read_pico_sd_info_t info = { 0 };
    // 格式化是用户明确发起的动作，等它算完空间是对的（而且刚格式化完 FSInfo 是新的，
    // `f_getfree` 走的是缓存值，不是全表扫描）。
    // Formatting is an explicit user action: blocking for the counts is correct (and right
    // after a format the FSInfo is fresh, so f_getfree reads the cached value).
    fill_info(&info, err, true);
    if (err == ESP_OK) {
        ensure_media_dirs();
        fill_info(&info, ESP_OK, true);
        ESP_LOGI(TAG, "formatted %s", info.name);
    }
    publish_info(&info);
    return err;
}
