/* SPDX-License-Identifier: Apache-2.0
 * SD 探测主机测试总线配置。/ Host SD-probe bus configuration stub.
 */
#pragma once

#include "esp_err.h"
#include <stddef.h>
esp_err_t test_host_init(void);
esp_err_t test_host_deinit(void);
// 26.9.31a：`mount_card()` 每次挂载前显式调它（幂等，见 driver 源码 sdmmc_host.c:78-83）。
// 26.9.31a: mount_card() calls this before every mount; it is idempotent.
esp_err_t sdmmc_host_init(void);
typedef struct { int max_freq_khz, slot; esp_err_t (*init)(void); esp_err_t (*deinit)(void); } sdmmc_host_t;
typedef struct {
    int width, clk, cmd, d0, d1, d2, d3, cd, wp, flags;
} sdmmc_slot_config_t;

#define SDMMC_HOST_DEFAULT() ((sdmmc_host_t){.init=test_host_init,.deinit=test_host_deinit})
#define SDMMC_SLOT_CONFIG_DEFAULT() ((sdmmc_slot_config_t){0})
#define SDMMC_FREQ_HIGHSPEED 40000
#define SDMMC_SLOT_FLAG_INTERNAL_PULLUP 1
#define GPIO_NUM_38 38
#define GPIO_NUM_42 42
#define GPIO_NUM_44 44
#define GPIO_NUM_NC -1

esp_err_t sdmmc_host_init_slot(int slot, const sdmmc_slot_config_t *config);
