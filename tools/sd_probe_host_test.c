/*
 * 首次无卡启动后插卡应可挂载；拔出已挂载卡仍须显式重挂。
 * A first insertion after empty boot mounts; removal of a mounted card still requires explicit remount.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_vfs_fat.h"
#include "freertos/task.h"

// 31b P1-1：时间轴由测试推进（判据是墙钟 3 s，真等不起）。
int64_t sd_test_now_us;
static bool test_present, test_exfat, test_raw_error;
static esp_err_t test_read_error = ESP_ERR_TIMEOUT;   // 读失败的**码**，默认 0x107
static bool detect_failure;
static int mount_calls, unmount_calls, test_fail_above_khz;
static int task_creations;
static esp_err_t test_mount_error = ESP_ERR_TIMEOUT;
// 26.9.31a：S3 的 20 轮 × 4 档降速 = 80 次挂载，原来的 64 格装不下。
// 26.9.31a: S3 runs 20 rounds x 4 clock steps = 80 mounts; 64 slots no longer fit.
static int clock_history[512], delays, idle_delays;
static int idle_urge_at;   // 见 vTaskDelay / see vTaskDelay
static uint64_t test_total=1024*512,test_free=768*512;
static esp_err_t test_info_error=ESP_OK;
static unsigned space_calls, invalidations;
static bool fat_locked;

static sdmmc_card_t test_card = {
    .cid.name = "TEST64G", .csd.capacity = 1024, .csd.sector_size = 512,
};

#define mkdir(path, mode) (0)
#include "../components/read_pico/read_pico_sd.c"
#undef mkdir

static FATFS test_fat={.free_clst=1};
BYTE ff_diskio_get_pdrv_card(const sdmmc_card_t *c){assert(c==&test_card);return 0;}
int f_opendir(FF_DIR *root,const char *path){assert(!strcmp(path,"0:"));root->obj.fs=&test_fat;return FR_OK;}
int f_closedir(FF_DIR *root){assert(root->obj.fs==&test_fat);return FR_OK;}
int ff_mutex_take(int drive){assert(drive==0&&!fat_locked);fat_locked=true;return 1;}
// 26.9.21：分片扫描也借这把锁（每片一次），所以这里不能再断言「交出时一定还是
// UINT32_MAX」——那只对作废 FSInfo 的一次成立。改断言配对，作废次数另计。
// 26.9.21: the sliced scan also borrows this lock (once per slice), so the give path can no
// longer assume free_clst is still UINT32_MAX. Assert pairing; count invalidations separately.
void ff_mutex_give(int drive){
    assert(drive==0&&fat_locked);
    fat_locked=false;
    if(test_fat.free_clst==UINT32_MAX) ++invalidations;
}
esp_err_t test_host_init(void){return ESP_OK;}
esp_err_t test_host_deinit(void){return ESP_OK;}
esp_err_t sdmmc_host_init_slot(int slot,const sdmmc_slot_config_t *config){(void)slot;(void)config;return ESP_OK;}
esp_err_t sdmmc_card_init(const sdmmc_host_t *host,sdmmc_card_t *out){(void)host;*out=test_card;return test_raw_error?ESP_ERR_TIMEOUT:ESP_OK;}
// 26.9.22 S23(a)：分片扫描已改成一次 4 个扇区（2 KB），缓冲落在 .bss。
// 默认路径仍按单扇区返回；置 fake_fat_mode 后按簇表内容供货，专门喂给下面的分片用例。
// 26.9.23 S24: the sliced scan reads ONE sector (512 B) at a time from a stack buffer.
// The default path still serves one sector; fake_fat_mode feeds the slicer from a
// synthetic FAT.
#define FAKE_FAT_SECTORS 128
static uint32_t fake_fat[FAKE_FAT_SECTORS * 128];
static bool fake_fat_mode;
static size_t fake_fat_base = 100;
esp_err_t sdmmc_read_sectors(sdmmc_card_t *c,void *buffer,size_t start,size_t count){
    (void)c;
    if (fake_fat_mode) {
        assert(start >= fake_fat_base);
        const size_t first = (start - fake_fat_base) * 128;
        assert(first + count * 128 <= sizeof(fake_fat)/sizeof(fake_fat[0]));
        memcpy(buffer, &fake_fat[first], count * 512);
        return ESP_OK;
    }
    assert(count==1);
    (void)start;
    if(test_raw_error)return test_read_error;
    memset(buffer,0x5a,512);
    if(test_exfat)memcpy((char*)buffer+3,"EXFAT   ",8);
    return ESP_OK;
}
bool read_pico_sd_present(void) { return test_present; }
// 26.9.31a：CD 弹跳注入。`flap_from` 起的 `flap_len` 次采样返回 absent，
// 其余按 `test_present`。去抖用例靠它精确控制「第几次采样抖」。
// 26.9.31a: CD bounce injection - `flap_len` samples starting at `flap_from` report absent.
static int detect_calls, flap_from = -1, flap_len;
esp_err_t read_pico_sd_detect(bool* present) {
    ++detect_calls;
    if (detect_failure) return ESP_ERR_TIMEOUT;
    if (flap_from > 0 && detect_calls >= flap_from && detect_calls < flap_from + flap_len) {
        *present = false;
        return ESP_OK;
    }
    *present = test_present;
    return ESP_OK;
}
// 26.9.31a：`mount_card()` 每次挂载前显式 host_init（幂等，driver sdmmc_host.c:78-83）。
// 这里只数次数——「每次 mount 前都 init 过」是裁决 4 要求的状态机不变式。
// 26.9.31a: mount_card() calls host_init before every mount (idempotent). Count the calls:
// "initialised before every mount" is the state-machine invariant ruling 4 asked for.
static int host_inits;
esp_err_t sdmmc_host_init(void) { ++host_inits; return ESP_OK; }
const char* esp_err_to_name(esp_err_t err) { (void)err; return "test"; }
BaseType_t xTaskCreate(void (*fn)(void*), const char* name, int stack,
                       void* arg, int priority, void* handle) {
    (void)name; (void)stack; (void)priority; (void)handle;
    ++task_creations;
    fn(arg);
    return pdPASS;
}
// 26.9.21 S14(a)：扫描前那 1,200 ms 的「让首屏先走」已删除——它谁也没让过，
// 因为首屏（书架）自己在等同一把卷锁。
// 26.9.22 S23(b)：现在有两种节拍，必须分开计数，否则「空闲等待」会把原来那条
// 「降速重试只许 250 次」的断言冲成一片模糊。
//   · 250 tick = 时钟降速重试的退避（原样保留，任何新增固定等待都会被它拦下）
//   · 100 tick = 挂载后的空闲步进；一个周期正好 200 步 = 20 s
// 26.9.21 S14(a): the 1,200 ms wait is gone. 26.9.22 S23(b): two cadences now exist and
// must be counted apart, or the idle wait would blur the "only 250-tick backoff" rule.
//   - 250 ticks: clock-retry back-off (unchanged; any new fixed wait trips it)
//   - 100 ticks: the post-mount idle step; 200 of them make exactly 20 s
void vTaskDelay(int ticks) {
    if (ticks == 250) { ++delays; return; }
    assert(ticks == 100);
    ++idle_delays;
    // 26.9.22 S23(b)：模拟「等到第 N 步时有人进文件页、主动要空间」，后台扫描应当
    // 立刻放弃剩余空闲、当场开工。0 = 没人催，必须等满 200 步。
    // 26.9.22 S23(b): simulate "someone opened the file page on step N and asked for the
    // numbers". The background scan must abandon the remaining idle time and start now.
    if (idle_urge_at > 0 && idle_delays == idle_urge_at) cached_info.space_urge = true;
}
void vTaskPrioritySet(void* handle, int priority) { (void)handle; assert(priority == 1); }
void vTaskDelete(void* handle) { (void)handle; }
// 分片扫描让 CPU：计数即可，测试用它断言「扫完一片确实让过」。
// Yield counting: the scan asserts it yields once per slice.
static int yields;
void taskYIELD(void) { ++yields; }
// 26.9.23 S24：余量必须打印（<256 判方案自败）。桩里返回固定值，真实余量只有真机能给。
// 26.9.23 S24: the margin must be printed (below 256 the plan fails by its own terms).
// The stub returns a constant; only real hardware can answer for the real margin.
static unsigned test_probe_hwm = 1024;
UBaseType_t uxTaskGetStackHighWaterMark(void* handle) { (void)handle; return test_probe_hwm; }
esp_err_t esp_vfs_fat_sdmmc_mount(const char* path, const sdmmc_host_t* host,
                                  const sdmmc_slot_config_t* slot,
                                  const esp_vfs_fat_sdmmc_mount_config_t* config,
                                  sdmmc_card_t** out) {
    (void)path; (void)host; (void)slot; (void)config;
    assert(mount_calls < 512);
    clock_history[mount_calls++] = host->max_freq_khz;
    assert(test_present);
    assert(!config->format_if_mount_failed);
    if(test_fail_above_khz && host->max_freq_khz > test_fail_above_khz){*out=NULL;return test_mount_error;}
    *out = &test_card;
    return ESP_OK;
}
esp_err_t esp_vfs_fat_sdcard_unmount(const char* path, sdmmc_card_t* mounted) {
    (void)path;
    assert(mounted == &test_card);
    ++unmount_calls;
    return ESP_OK;
}
esp_err_t esp_vfs_fat_sdcard_format(const char* path, sdmmc_card_t* mounted) {
    (void)path; (void)mounted;
    return ESP_OK;
}
esp_err_t esp_vfs_fat_info(const char* path, uint64_t* total, uint64_t* free_bytes) {
    (void)path;
    ++space_calls;
    assert(!fat_locked);
    // 模拟真实 f_getfree：作废 FSInfo 后扫描，后续直接复用随写入变化的计数。
    // Like f_getfree, unknown FSInfo scans once; later counts follow allocation changes.
    if (test_fat.free_clst==UINT32_MAX) test_fat.free_clst=123;
    *total=test_total;*free_bytes=test_free;
    return test_info_error;
}

int main(void) {
    read_pico_sd_info_t info = {0};
    assert(read_pico_sd_start_probe() == ESP_ERR_NOT_FOUND);
    assert(read_pico_sd_get_info(&info) == ESP_ERR_NOT_FOUND);
    assert(!info.present && !info.mounted && mount_calls == 0);

    test_present = true;
    assert(read_pico_sd_get_info(&info) == ESP_ERR_INVALID_STATE);
    assert(info.present && !info.mounted);
    assert(read_pico_sd_start_probe() == ESP_ERR_NOT_FINISHED);
    assert(read_pico_sd_get_info(&info) == ESP_OK);
    assert(info.present && info.mounted && mount_calls == 1);
    // 26.9.22 S23(b)：挂载发布会后，扫描必须先睡满 20 s（200 × 100 ms）才动手。
    // 20 s = SD_SPACE_IDLE_MS / SD_SPACE_IDLE_STEP_MS，两处常量一起改才算生效。
    assert(idle_delays == 200);

    detect_failure = true;
    assert(read_pico_sd_get_info(&info) == ESP_OK && info.mounted && info.capacity_bytes);
    assert(read_pico_sd_start_probe() == ESP_OK && mount_calls == 1);
    detect_failure = false;

    test_present = false;
    assert(read_pico_sd_get_info(&info) == ESP_ERR_NOT_FOUND);
    test_present = true;
    assert(read_pico_sd_start_probe() == ESP_ERR_INVALID_STATE);
    assert(read_pico_sd_remount() == ESP_ERR_NOT_FINISHED);
    assert(read_pico_sd_get_info(&info) == ESP_OK);
    assert(info.mounted && mount_calls == 2 && unmount_calls == 1);
    test_card.csd.capacity = 536870912u; // 256 GiB / 512 byte sectors, exceeds 32-bit byte range.
    test_fail_above_khz=10000;
    assert(read_pico_sd_remount()==ESP_ERR_NOT_FINISHED);
    assert(read_pico_sd_get_info(&info)==ESP_OK&&info.mounted);
    assert(info.capacity_bytes==256ull*1024*1024*1024);
    assert(mount_calls==6 && delays==3);
    assert(clock_history[2]==40000 && clock_history[3]==40000 && clock_history[4]==20000 && clock_history[5]==10000);

    // CMD6 忙不能终止降速；默认频率跳过高速切换。/ CMD6 busy must fall back; default speed skips HS negotiation.
    test_mount_error=ESP_ERR_INVALID_STATE;
    test_fail_above_khz=20000;
    assert(read_pico_sd_remount()==ESP_ERR_NOT_FINISHED);
    assert(read_pico_sd_get_info(&info)==ESP_OK && info.mounted && mount_calls==9);
    assert(clock_history[6]==40000 && clock_history[7]==40000 && clock_history[8]==20000);

    // 资源错误立即失败，初始化失败不建议格式化。/ Resource errors fail immediately; init failures never suggest format.
    test_mount_error=ESP_ERR_NO_MEM;
    test_fail_above_khz=1;
    int before=mount_calls, previous_delays=delays;
    assert(read_pico_sd_remount()==ESP_ERR_NOT_FINISHED);
    assert(read_pico_sd_get_info(&info)==ESP_ERR_NO_MEM && !info.mounted && !info.needs_format);
    assert(mount_calls==before+1 && delays==previous_delays);
    test_mount_error=ESP_ERR_INVALID_STATE;
    before=mount_calls;
    assert(read_pico_sd_remount()==ESP_ERR_NOT_FINISHED);
    assert(read_pico_sd_get_info(&info)==ESP_ERR_INVALID_STATE && !info.needs_format);
    assert(mount_calls==before+4 && delays==previous_delays+3);
    test_mount_error=ESP_FAIL;
    assert(read_pico_sd_remount()==ESP_ERR_NOT_FINISHED);
    assert(read_pico_sd_get_info(&info)==ESP_FAIL && !info.needs_format);

    test_exfat=true;
    assert(read_pico_sd_remount()==ESP_ERR_NOT_FINISHED);
    assert(read_pico_sd_get_info(&info)==ESP_FAIL && info.needs_format);
    test_raw_error=true;
    assert(read_pico_sd_remount()==ESP_ERR_NOT_FINISHED);
    assert(read_pico_sd_get_info(&info)==ESP_FAIL && !info.needs_format);
    test_raw_error=test_exfat=false;

    // CD 通信未知时仍尝试实际挂载，不能假设拔卡。/ Unknown CD still permits an actual mount, not assumed removal.
    detect_failure=true;
    test_fail_above_khz=0;
    assert(read_pico_sd_remount()==ESP_ERR_NOT_FINISHED);
    assert(read_pico_sd_get_info(&info)==ESP_OK && info.mounted && info.present);
    detect_failure=false;
    assert(read_pico_sd_get_info(&info)==ESP_OK && info.mounted);
    assert(info.space_valid && info.filesystem_bytes==test_total && info.free_bytes==test_free);
    assert(invalidations>0);
    // 多分区卡只统计挂载分区；256GiB卡不能截断，挂载状态与统计错误独立。
    test_total=128ull*1024*1024*1024; test_free=100ull*1024*1024*1024;
    unsigned before_space=space_calls;
    assert(read_pico_sd_refresh_space()==ESP_OK && space_calls==before_space+1);
    assert(read_pico_sd_get_info(&info)==ESP_OK && info.capacity_bytes==256ull*1024*1024*1024 &&
           info.filesystem_bytes==test_total && info.filesystem_bytes-info.free_bytes==28ull*1024*1024*1024);
    test_free-=512*1024;
    assert(read_pico_sd_refresh_space()==ESP_OK);
    assert(read_pico_sd_get_info(&info)==ESP_OK&&info.free_bytes==test_free);
    test_info_error=ESP_FAIL;
    assert(read_pico_sd_refresh_space()==ESP_FAIL);
    assert(read_pico_sd_get_info(&info)==ESP_OK&&info.mounted&&!info.space_valid&&info.space_error==ESP_FAIL&&info.free_bytes==0);
    test_info_error=ESP_OK;test_free=test_total+1;
    assert(read_pico_sd_refresh_space()==ESP_FAIL);
    assert(read_pico_sd_get_info(&info)==ESP_OK&&!info.space_valid);
    test_free=0;
    assert(read_pico_sd_refresh_space()==ESP_OK);
    assert(read_pico_sd_get_info(&info)==ESP_OK&&info.space_valid&&info.free_bytes==0);
    // 26.9.22 S23(b)：空闲等待可被「主动问空间」提前打断。等满时 200 步，被打断时
    // 第 5 步就开扫，两种走法的结果必须一致——否则「催扫」等于换了一条算法。
    // 26.9.22 S23(b): the idle wait can be cut short by an explicit request. Waiting the
    // full 200 steps and being interrupted on step 5 must produce the same numbers.
    {
        const int before_idle = idle_delays;
        idle_urge_at = before_idle + 5;
        assert(read_pico_sd_remount() == ESP_ERR_NOT_FINISHED);
        assert(idle_delays == before_idle + 5);              // 没等满 200 步
        idle_urge_at = 0;
        assert(read_pico_sd_get_info(&info) == ESP_OK && info.mounted && info.space_valid);
        assert(info.filesystem_bytes == test_total && info.free_bytes == test_free);
    }
    // 26.9.21 S14(b)：分片扫 FAT——不持卷锁扫完整张表，但每片仍把锁借一次。
    // 26.9.22 S23(a)：片子从 8 KB 缩到 2 KB、缓冲从堆搬到 .bss，两条新断言：
    //   ① 扫描结束后内部最大块必须**一个字没变**（静态缓冲不参与堆块划分）；
    //   ② 片子变小 → 片数变多，让路次数按 4 扇区一片重算。
    // ① 8 簇里 3 个空闲、每簇 2 KB：一片就读完，free_clst 要写回卷缓存。
    {
        memset(fake_fat, 0xFF, sizeof(fake_fat));            // 先全标占用 / all allocated
        fake_fat[2] = 0; fake_fat[4] = 0; fake_fat[9] = 0;   // 三个空闲 / three free
        test_fat.fs_type = FS_FAT32;
        test_fat.n_fatent = 10;                              // 8 簇 + 2
        test_fat.csize = 4;                                  // 每簇 2 KB
        test_fat.fatbase = (uint32_t)fake_fat_base;
        test_fat.free_clst = UINT32_MAX;
        fake_fat_mode = true;
        test_largest_internal = 18432;
        const int yields_small = yields;
        assert(read_pico_sd_refresh_space() == ESP_OK);
        assert(read_pico_sd_get_info(&info) == ESP_OK && info.space_valid);
        assert(info.filesystem_bytes == 8ull * 4 * 512);
        assert(info.free_bytes == 3ull * 4 * 512);
        // 扫完要把结果写回卷缓存，否则下一个 f_getfree 又得全表扫一遍。
        assert(test_fat.free_clst == 3);
        assert(yields == yields_small + 1);                  // 1 个 FAT 扇区 = 一片
        assert(!fat_locked);                                 // 锁已还 / lock released
        // 扫描不吃堆：最大块原样。
        // The scan never touches the heap: the largest block is untouched.
        assert(test_largest_internal == 18432);
    }
    // ② 4,096 簇：33 个 FAT 扇区，按 1 扇区一片切成 33 片，让 33 次——这就是「界面
    //    最多等 512 B」的全部含义。4,096 簇 × 2 KB = 8 MB 刚好等于真机那张表的量级，
    //    片数是 26.9.22 的 4 倍、26.9.21 的 32 倍：换来的就是堆与 .bss 双零痕迹。
    {
        memset(fake_fat, 0xFF, sizeof(fake_fat));
        for (int i = 2; i < 2 + 1024; ++i) fake_fat[i] = 0;  // 1,024 个空闲
        test_fat.n_fatent = 2 + 4096;
        test_largest_internal = 18432;
        const int yields_big = yields;
        assert(read_pico_sd_refresh_space() == ESP_OK);
        assert(read_pico_sd_get_info(&info) == ESP_OK && info.space_valid);
        assert(info.filesystem_bytes == 4096ull * 4 * 512);
        assert(info.free_bytes == 1024ull * 4 * 512);
        assert(test_fat.free_clst == 1024);
        assert(yields == yields_big + 33);                   // 33 / 1 = 33 片
        assert(!fat_locked);
        assert(test_largest_internal == 18432);              // 33 片扫完，最大块仍不动
        fake_fat_mode = false;
        test_fat.fs_type = 0;
    }
    test_present=false;before_space=space_calls;
    assert(read_pico_sd_refresh_space()==ESP_ERR_NOT_FOUND&&space_calls==before_space);
    assert(read_pico_sd_get_info(&info)==ESP_ERR_NOT_FOUND&&!info.mounted&&!info.space_valid);

    // 26.9.17 T1：卡已拔、卷还挂着时，remount 必须真卸载，且不建探测任务——
    // 槽里没有卡，探也探不出东西，建任务只是白占 4 KB 栈。
    // 26.9.17 T1: with the card gone but the volume still mounted, remount must
    // really unmount and start no probe task - there is nothing to probe and a
    // task would only burn 4 KB of stack.
    {
        const int unmounts_before = unmount_calls, mounts_before = mount_calls;
        const int tasks_before = task_creations;
        assert(card != NULL);
        assert(read_pico_sd_remount() == ESP_ERR_NOT_FOUND);
        assert(card == NULL);
        assert(unmount_calls == unmounts_before + 1);
        assert(mount_calls == mounts_before && task_creations == tasks_before);
        assert(read_pico_sd_get_info(&info) == ESP_ERR_NOT_FOUND && !info.mounted && !info.present);
    }
    // ---- 31b P1-1：掉盘的第二个信源 ------------------------------------------
    // 场景就是判据原文：CD 脚恒 present（`test_present=true`），卡已经读不出来
    // （每次读都 0x107）。下面四条各自打掉一种走法，缺一条就有一种真实故障没人管。
    // P1-1: CD pinned present, every read times out. Four cases, one per way this
    // can go wrong.
    test_present = true; test_raw_error = false;
    assert(read_pico_sd_start_probe() == ESP_ERR_NOT_FINISHED);
    assert(read_pico_sd_get_info(&info) == ESP_OK && info.mounted);

    // (a) 快路径：连续 8 次超时判死。每次推进 300 ms（实测单次 117 ms + 余量），
    //     8 次 ≈ 2.1 s < 3 s——**这一次是次数先到**，墙钟还没满。
    test_raw_error = true;
    uint8_t io_fails = 0; uint16_t io_ms = 0;
    sd_test_now_us = 1000000;
    for (int i = 0; i < 7; ++i) {
        sd_test_now_us += 300000;
        assert(read_pico_sd_watch_io(&io_fails, &io_ms) == ESP_OK);
    }
    assert(!io_fails || io_fails == 7);
    sd_test_now_us += 300000;
    assert(read_pico_sd_watch_io(&io_fails, &io_ms) == ESP_ERR_NOT_FOUND);
    assert(io_fails == 8 && io_ms >= 2000 && io_ms < 3000);
    // 判死之后快照必须是「CD 说在、卷不可用」——present 不改，它说的是另一个事实。
    // 判死的返回码是契约里的「这个卷没了」（NOT_FOUND）。**注意它不决定看门狗入口**：
    // 紧接着的 get_info() 会走 observe_media_locked()，锁存期把快照重写成
    // `{present=1, mounted=0, error=INVALID_STATE}`（实测 259）——app_loop 收到的是
    // INVALID_STATE，兜底分支的 `err != NOT_FOUND` 照样放行。
    // 这一点合流时差点判错：曾据「NOT_FOUND 会关掉看门狗」去改码，实测才发现
    // 真正漂的是 app_loop 的宿主桩（桩喂 NOT_FOUND、真件给 INVALID_STATE，§13.12a）。
    // The death code is the contract's "this volume is gone" (NOT_FOUND). It does NOT
    // decide the watchdog door: the next get_info() rewrites the snapshot to
    // {present=1, mounted=0, error=INVALID_STATE} while latched (measured 259), and
    // app_loop's fallback passes on INVALID_STATE. The real drift was in the host stub.
    assert(read_pico_sd_get_info(&info) != ESP_OK && info.present && !info.mounted);

    // (b) 墙钟才是判据：卡比 8 次慢时，3 s 一到照样判死。这一条若只按次数写，
    //     最坏路径（单次 2 s）要求 16 s，而要求是「3 s 内」——两个参数不同源。
    test_raw_error = false;
    assert(read_pico_sd_remount() == ESP_ERR_NOT_FINISHED);
    assert(read_pico_sd_get_info(&info) == ESP_OK && info.mounted);
    test_raw_error = true;
    sd_test_now_us += 1000000;
    assert(read_pico_sd_watch_io(&io_fails, &io_ms) == ESP_OK);   // 第 1 次失败，t0
    sd_test_now_us += 1600000;
    assert(read_pico_sd_watch_io(&io_fails, &io_ms) == ESP_OK);   // 第 2 次，未满 3 s
    sd_test_now_us += 1500000;                                    // 累计 3.1 s
    assert(read_pico_sd_watch_io(&io_fails, &io_ms) == ESP_ERR_NOT_FOUND);
    assert(io_fails == 3 && io_ms >= 3000);

    // (c) 成功必须清零：5 次失败 → 1 次成功 → 再 5 次失败仍然是 5，不是 10。
    //     不清零的话「插拔抖动 + 一次偶发成功」会把两次无关的坏卡拼成一次判死。
    test_raw_error = false;
    assert(read_pico_sd_remount() == ESP_ERR_NOT_FINISHED);
    test_raw_error = true;
    for (int i = 0; i < 5; ++i) {
        sd_test_now_us += 300000;
        assert(read_pico_sd_watch_io(&io_fails, &io_ms) == ESP_OK);
    }
    test_raw_error = false;
    sd_test_now_us += 300000;
    assert(read_pico_sd_watch_io(&io_fails, &io_ms) == ESP_OK);   // 成功 → 清零
    test_raw_error = true;
    for (int i = 0; i < 5; ++i) {
        sd_test_now_us += 300000;
        assert(read_pico_sd_watch_io(&io_fails, &io_ms) == ESP_OK);
    }
    assert(read_pico_sd_get_info(&info) == ESP_OK && info.mounted);   // 没被误判

    // (d) 只数 0x107：驱动内部状态（INVALID_STATE / NO_MEM）不说明「卡不答」。
    //     一次内存紧张把好卡判死，比漏判一次更坏——卷会被无谓卸载。
    test_read_error = ESP_ERR_INVALID_STATE;
    for (int i = 0; i < 10; ++i) {
        sd_test_now_us += 300000;
        assert(read_pico_sd_watch_io(&io_fails, &io_ms) == ESP_OK);
    }
    assert(read_pico_sd_get_info(&info) == ESP_OK && info.mounted);
    test_read_error = ESP_ERR_TIMEOUT;
    test_raw_error = false;

    // 合流顺序：P1-1 先、31a 的 S1–S4 后。理由是状态自足性——S1–S4 每一段都自己
    // 设定 `media_invalidated` / `probe_state` / `cached_info`，放最后最安全；
    // P1-1 那段要的是「已经挂上、CD 恒 present」的干净起点，正好接着上一条用例的尾态。
    // Merge order: P1-1 first, then 31a's S1-S4. Each S-block sets its own latch/probe
    // state, so it belongs last; P1-1 needs the clean "mounted, CD pinned" start.
    // ===== 26.9.31a S1–S4：物理拔卡死锁的四个不变式 ==========================
    // 现场（91855 ms 那次）：首发挂载失败 + 探测窗口里 CD 抖一次 absent →
    // `media_invalidated` 焊死 → 之后每次兜底都只拿到同一句 INVALID_STATE，
    // 插回也不认卡，只能重启。四条用例分别咬住：去抖、锁存判据、上升沿、发布自洽。
    // 26.9.31a S1-S4: the four invariants behind the field deadlock. First mount fails,
    // CD bounces once inside the probe window, the latch welds shut, every fallback then
    // gets the same INVALID_STATE, and reinsertion changes nothing until a reboot.
    {
        // 合流接线（必须在前）：上面 P1-1 那段收尾时卷**还挂着**（card != NULL）。
        // 而 S2 测的是「上升沿清闩」，31a 的清闩条件里明写 `card == NULL`——
        // 残留句柄在时不清闩是**对的**（先 remount、再清），所以这里要先把卡拔掉、
        // 卸干净，让 S1–S4 从「无卡」这个真实起点开始。
        // 不这么做，红的是「残留句柄」而不是「上升沿」——两条完全不同的话。
        // Merge wiring: P1-1 ends with the volume still mounted. S2 tests the rising edge,
        // whose condition explicitly requires `card == NULL`; a stale handle correctly
        // blocks the clear. Start S1-S4 from the real "no card" state or the red would be
        // about the leftover handle, not about the rising edge.
        test_present = false;
        test_raw_error = false;
        assert(read_pico_sd_remount() == ESP_ERR_NOT_FOUND && card == NULL);

        // --- S1(a)：单次弹跳不是判决（去抖） ---
        test_present = true;
        bool seen = false;
        flap_from = detect_calls + 2; flap_len = 1;
        assert(sd_present_stable(&seen) == ESP_ERR_INVALID_STATE);   // M125：退化成单采样则这里变 OK
        flap_from = -1;
        assert(sd_present_stable(&seen) == ESP_OK && seen);

        // --- S1(b)：只有「已挂载的卡消失」才配叫失效 ---
        // 探测窗口里（probe_state == 1）的一次 absent 只是 CD 在弹，不是卡走了。
        // 26.9.29 的 `|| probe_state == 1` 就是本条的反面（M123）。
        media_invalidated = false;
        probe_state = 1;
        cached_info.mounted = false;
        cached_info.present = true;
        observe_media_locked(false);
        assert(!media_invalidated);
        // 已挂载的卡消失：必须锁存，且快照落成「卡不在」。
        probe_state = 2;
        cached_info.mounted = true;
        cached_info.present = true;
        observe_media_locked(false);
        assert(media_invalidated && !cached_info.mounted && !cached_info.present);

        // --- S2：上升沿清锁存 ---
        // 用户把卡插回去是人的意图，不许被旧闩否决。构造「上一拍无卡 + 闩为真」。
        media_invalidated = true;
        probe_state = 2;
        cached_info.present = false;
        cached_info.mounted = false;
        test_present = true;
        read_pico_sd_info_t s2 = {0};
        (void)read_pico_sd_get_info(&s2);                 // 上升沿在这里被观察
        assert(!media_invalidated);                       // M124：删掉上升沿清闩则这里红
        const int tasks_before = task_creations;
        assert(read_pico_sd_start_probe() == ESP_ERR_NOT_FINISHED);
        assert(task_creations == tasks_before + 1);       // 真的建了探测任务，不是只改了状态

        // --- S3：连续 20 轮「CD 抖 + 挂载失败」不许中毒 ---
        // 判据不是「这一轮能挂上」，而是「永远还有下一次」——S26 现场要的就是这一条。
        test_fail_above_khz = 1;                          // 每一档都失败
        test_mount_error = ESP_ERR_TIMEOUT;
        for (int round = 0; round < 20; ++round) {
            test_present = true;
            flap_from = detect_calls + 1; flap_len = 1;
            read_pico_sd_info_t s3 = {0};
            (void)read_pico_sd_get_info(&s3);
            flap_from = -1;
            assert(read_pico_sd_remount() == ESP_ERR_NOT_FINISHED);
            assert(read_pico_sd_start_probe() != ESP_ERR_INVALID_STATE);  // 不再吐旧闩错误
        }

        // --- S4：锁存期内一次真实挂载的发布不许被抹成未挂载 ---
        // 环③：26.9.29 的 `if (!present || media_invalidated) memset(...)` 让
        // 「挂载成功」和「界面宣布无卡」同时成立。
        media_invalidated = true;
        read_pico_sd_info_t s4 = { .present = true, .mounted = true, .error = ESP_OK };
        publish_info(&s4);
        assert(!media_invalidated);
        read_pico_sd_info_t s4b = {0};
        assert(read_pico_sd_get_info(&s4b) == ESP_OK && s4b.mounted && s4b.present);
        test_fail_above_khz = 0;
    }

    puts("sd_probe: empty boot/insertion, checked CD, bounded settle/fallback, CMD6 busy, OOM, no format suggestion, 256 GiB, release-without-probe, P1-1 I/O-dead (8-fast / 3s-wall / reset-on-success / timeout-only), 31a S1-S4 (debounce / latch scope / rising edge / publish coherence) passed");

    return 0;
}
