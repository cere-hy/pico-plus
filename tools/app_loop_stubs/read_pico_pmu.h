/* 中文：调度测试替身。/ English: Scheduler test shim. */
#pragma once
#include "common.h"

// 26.11.16 电量修复批：主循环只读「百分比 + 充电位」两项，桩按最小面暴露。
// The loop reads only percent + charging; the shim exposes the minimum surface.
typedef struct {
    bool present;
    int soc;
    bool charging;
} pmu_snapshot_t;

const pmu_snapshot_t *read_pico_pmu_get(void);
int pmu_battery_percent(const pmu_snapshot_t *s);
bool pmu_battery_charging(const pmu_snapshot_t *s);

// 26.11.19 解耦批：采样支从主循环直接调它（不再借道取键）。桩计数供 K1 断言。
// 26.11.19 decoupling: the loop's sampling branch calls this directly rather than
// riding on key-taking. The shim counts calls so K1 can assert the cadence.
// `read_pico_pmu_ready` 的原型在 common.h（多处替身共用），此处不重复。
// The ready() prototype lives in common.h (shared by several shims); not repeated here.
esp_err_t read_pico_pmu_poll(void);
extern unsigned pmu_poll_calls;
