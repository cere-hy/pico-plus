/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 小纸 Pico SDMMC 探测、挂载、格式化。
 *
 * Read Pico SDMMC probe, mount, and format.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool present;
    bool mounted;
    bool needs_format;
    // 26.9.23 S24：「有人主动问空间」的请求位住在这里。**不是顺手**——本对象任何新增
    // 静态字节都会把堆基址顶到下一个 16 边界（真机 26.9.22：+2,064 → largest 塌 6,144），
    // 而这个字段落在本组 bool 的原有填充里，`cached_info` 保持 56 B、dram0 布局与
    // 26.9.21 逐字节相同，生死状（largest 回 18,432）才由构造保证。
    // 26.9.23 S24: the "someone asked for the space scan" bit lives here - not as a
    // convenience. Any new static byte bumps the heap base to the next 16-byte boundary
    // (26.9.22 field data: +2,064 cost 6 KB of largest). This field fits the existing
    // padding of the bool group, keeping cached_info at 56 B and the layout identical.
    bool space_urge;
    char name[8];
    uint64_t capacity_bytes;
    // capacity 是物理卡大小；已用/可用只按挂载的文件系统统计。
    // Physical card size is separate from the mounted filesystem's usable capacity.
    uint64_t filesystem_bytes;
    uint64_t free_bytes;
    bool space_valid;
    esp_err_t space_error;
    esp_err_t error;
} read_pico_sd_info_t;

/// 首次无卡开机后插卡可重新探测；已挂载卡拔出失效仍须显式 remount。
/// / Probe a first insertion after an empty boot; a removed mounted card still needs explicit remount.
esp_err_t read_pico_sd_start_probe(void);
/// 查询有效 CD 并锁存拔卡失效；检测通信失败保留快照。重新插入须显式 remount。
/// / Latch removal only from valid CD; detection I/O failure preserves snapshots. Reinsertion needs explicit remount.
esp_err_t read_pico_sd_get_info(read_pico_sd_info_t* info);
/// 更新空间快照，不重挂载、不写卡；勿在 render 中调用。
/// Refresh allocation statistics without remounting or writing; never call from render.
esp_err_t read_pico_sd_refresh_space(void);
/// 调用前关闭所有字体、阅读及上传句柄；这是拔卡失效后恢复挂载的唯一入口。
/// / Close all font, reader and upload handles first; this is the only recovery entry after removal invalidation.
esp_err_t read_pico_sd_remount(void);
/// 仅显式用户确认后调用；失效挂载必须先 remount，绝不自动建议格式化拔出的卡。
/// / Explicit user confirmation only; an invalidated mount needs remount first, never a removal-triggered format suggestion.
esp_err_t read_pico_sd_format(void);
/// 挂载代号：每次挂载成功 +1，只增不减；0 = 本次开机尚未挂载。
/// **为什么消费方要用它而不是 `mounted` 状态位**：状态位是快照，拔插快于一次轮询时
/// 「刚挂载」与「一直在」在快照上同形，边沿会整个丢失（26.9.31 现场丢了 886 ms）。
/// 代号是事件：挂载一次 +1 一次，漏不掉。消费侧必须按代号变化判边沿，同代号重复到达
/// 应视作同一次挂载（幂等由消费侧守，本函数只报事实）。
/// / Monotonic mount epoch: +1 per successful mount, never decreasing. Consumers must
/// edge-detect on this instead of on the `mounted` bit, which is a snapshot and can hide
/// an edge entirely when a replug is faster than one poll interval.
uint32_t read_pico_sd_mount_epoch(void);

/// 31b P1-1：掉盘的**第二个信源**——主动读一次卡，连读不到就判卡不在。
///
/// 为什么需要它：`read_pico_sd_get_info()` 的「在不在」只由 CD 脚回答，而 CD 只擅长
/// 「快」不擅长「准」。卡还在槽里、但已经读不出来（触点氧化、半拔出、卡内部坏）时，
/// CD 仍然说 present，卷就一直挂着空转——26.9.16 现场 83 条 `sdmmc_read_blocks failed
/// (0x107)` 就是那个形状。本函数补上「准」的那一半：连续 8 次超时，或从第一次超时起
/// 满 3 s（墙钟是判据，次数只是快路径），判定「CD 说在、I/O 说不在」，置失效闩并把
/// 快照落成「卡不在」（`present` 保持 CD 的事实，不替它撒谎）。
///
/// 调用方式：主循环每拍调，内部自带节流（正常 1 s / 失败 100 ms）与幂等，被调方不会
/// 因为被频繁调用而多读卡。**只许 app 侧调用**——它读卡，不能进 render 或中断上下文。
/// `fail_count` / `elapsed_ms` 仅在返回 `ESP_ERR_NOT_FOUND` 那一次有值（给黑匣子记
/// 「几次、多久」）；`ESP_ERR_INVALID_STATE` = 当前没有可监视的挂载卷，不是失败。
/// / P1-1: the second source of truth for removal - read the card on purpose. Eight
/// consecutive timeouts, or 3 s of wall clock since the first one, means "CD says
/// present, I/O says gone". Call once per main-loop tick; it throttles itself.
esp_err_t read_pico_sd_watch_io(uint8_t *fail_count, uint16_t *elapsed_ms);

/// 已挂载则卸载落盘。探测未完成返回 ESP_ERR_NOT_FINISHED。
/// / Unmount if mounted so writes land. ESP_ERR_NOT_FINISHED while probing.
esp_err_t read_pico_sd_sync(void);

#ifdef __cplusplus
}
#endif
