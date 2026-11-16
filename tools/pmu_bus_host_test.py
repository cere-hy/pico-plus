#!/usr/bin/env python3
"""26.11.19 解耦批：事件取载的宿主测试（K2a / K4 / K9.2）。
26.11.19 decoupling: host tests for the event-take path.

测什么、不测什么：
  - 测 **取键路径碰了哪些寄存器**。这是 K2a 的实体——「取键路径 refresh_core 计数 == 0」
    的机器可判形态就是「一次取键里没有一笔值链寄存器读」。数函数调用得靠 grep，
    数寄存器得靠桩，后者更难糊弄。
  - 测 FIFO 语义：一次取空、LONG 覆盖 SHORT、载荷取批内第一个 SHORT 的 arg0（K4）。
  - 不测像素、不测时序（K3 的三段式里，宿主合成 S 只做结构上界，另起）。
"""

from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "components/read_pico_pmu/read_pico_pmu.c").read_text()

# 值链寄存器：取键路径碰一个就是便车回来了。事件三兄弟（PEEK / COUNT / ACK）不在表里。
VALUE_REGS = {
    "PMU_REG_IDENTITY": "身份块",
    "PMU_REG_STATUS": "状态块",
    "PMU_REG_QUICK_BATTERY": "快充电量",
    "PMU_REG_DIAGNOSTICS": "诊断块",
}


def extract(name):
    m = re.search(r"^(?:static )?[^\n]+\b" + name + r"\([^;{}]*?\)\s*\{", source, re.M)
    assert m, name
    n = m.end()
    level = 1
    while level:
        if source[n] == "{":
            level += 1
        elif source[n] == "}":
            level -= 1
        n += 1
    return source[m.start():n]


unit = r"""
#include "read_pico_pmu.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static pmu_snapshot_t s_snap;

/* ---- 寄存器级 FIFO 桩 ---- */
typedef struct { uint16_t id; uint8_t type; uint32_t arg0; uint16_t arg1; } ev_t;
static ev_t q[16];
static int qn;
static int acks;
static uint16_t ack_ids[16];
static int value_reg_reads;     /* 值链寄存器读次数（K2a 的实体） */
static int count_reads, peek_reads;

static void queue(uint8_t type, uint16_t id, uint32_t arg0, uint16_t arg1) {
    assert(qn < 16);
    q[qn].type = type; q[qn].id = id; q[qn].arg0 = arg0; q[qn].arg1 = arg1; qn++;
}

static esp_err_t pmu_read_reg(uint8_t reg, uint8_t* data, size_t len) {
    if (reg == PMU_REG_EVENT_COUNT) {
        assert(len >= 1);
        data[0] = (uint8_t)qn;
        ++count_reads;
        return ESP_OK;
    }
    if (reg == PMU_REG_EVENT_PEEK) {
        assert(len >= PMU_EVENT_SIZE);
        assert(qn > 0);
        memset(data, 0, len);
        data[0] = (uint8_t)(q[0].id & 0xff);
        data[1] = (uint8_t)(q[0].id >> 8);
        data[2] = q[0].type;
        data[3] = 0;                       /* severity */
        data[8] = (uint8_t)(q[0].arg0 & 0xff);
        data[9] = (uint8_t)((q[0].arg0 >> 8) & 0xff);
        data[10] = (uint8_t)((q[0].arg0 >> 16) & 0xff);
        data[11] = (uint8_t)((q[0].arg0 >> 24) & 0xff);
        data[12] = (uint8_t)(q[0].arg1 & 0xff);
        data[13] = (uint8_t)(q[0].arg1 >> 8);
        uint16_t c = pmu_crc16_ccitt_false(data, 14);
        data[14] = (uint8_t)(c & 0xff);
        data[15] = (uint8_t)(c >> 8);
        ++peek_reads;
        return ESP_OK;
    }
    ++value_reg_reads;                     /* 任何别的寄存器都是便车 */
    memset(data, 0, len);
    return ESP_OK;
}

static esp_err_t pmu_write_reg(uint8_t reg, const uint8_t* data, size_t len) {
    if (reg != PMU_REG_EVENT_ACK) return ESP_OK;
    assert(len == 4);
    uint16_t id = (uint16_t)(data[0] | (data[1] << 8));
    assert(qn > 0);
    assert(q[0].id == id);                 /* 只许 ACK 队头：不许跳着吞 */
    ack_ids[acks++] = id;
    for (int i = 1; i < qn; i++) q[i - 1] = q[i];
    --qn;
    return ESP_OK;
}

static void vTaskDelay(int t) { (void)t; }
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGI(...) ((void)0)
const char* pmu_event_name(uint8_t t) { (void)t; return "ev"; }
"""

body = r"""
int main(void) {
    /* K2a-1：三个短按，一次取空；全程零值链寄存器读。 */
    queue(PMU_EVT_KEY_SHORT, 11, 120, 0);
    queue(PMU_EVT_KEY_SHORT, 12, 130, 0);
    queue(PMU_EVT_KEY_SHORT, 13, 140, 0);
    value_reg_reads = 0;
    uint16_t held = 0;
    assert(read_pico_pmu_take_key_action(&held) == READ_PICO_PMU_KEY_SHORT);
    assert(held == 120);                    /* K4④：批内第一个 SHORT 的 arg0 */
    assert(qn == 0);                        /* K4② 一次取空 */
    assert(acks == 3 && ack_ids[0] == 11 && ack_ids[2] == 13);
    assert(value_reg_reads == 0);           /* K2a：取键路径零值链读 */

    /* K2a-2：LONG 覆盖 SHORT，但整批照样取空。 */
    acks = 0;
    queue(PMU_EVT_KEY_SHORT, 21, 111, 0);
    queue(PMU_EVT_KEY_SHORT, 22, 222, 0);
    queue(PMU_EVT_KEY_LONG,  23, 999, 0);
    held = 0;
    assert(read_pico_pmu_take_key_action(&held) == READ_PICO_PMU_KEY_LONG);
    assert(qn == 0 && acks == 3);
    assert(held == 111);                    /* 分发被 LONG 覆盖，载荷不被覆盖 */

    /* K2a-3：空队列是一次 COUNT 读，不 ACK 任何东西，不碰值链。 */
    acks = 0; value_reg_reads = 0; held = 777;
    assert(read_pico_pmu_take_key_action(&held) == READ_PICO_PMU_KEY_NONE);
    assert(acks == 0 && value_reg_reads == 0);
    assert(held == 0);                      /* 无 SHORT 时载荷归零，不留陈值 */

    /* K9.2 / drain：drain 处理到哪 ACK 到哪，不吞它没处理的事件。
       （FIFO 深度上限 8，超出部分留给下一轮——语义是「不丢」，不是「一次全清」。） */
    acks = 0; value_reg_reads = 0;
    for (int i = 0; i < 3; i++) queue(PMU_EVT_BATTERY_SOC_LOW, (uint16_t)(31 + i), 0, 0);
    read_pico_pmu_drain_events();
    assert(qn == 0 && acks == 3);
    assert(value_reg_reads == 0);           /* drain 也不许拉整份快照 */

    /* ACK 后计数对齐：留下一条，下一次 COUNT 必须是 1 而不是旧值。 */
    queue(PMU_EVT_KEY_SHORT, 41, 150, 0);
    read_pico_pmu_drain_events();
    assert(qn == 0 && s_snap.pending_events == 0);

    /* 载荷：一次取键里，批内**第一个** SHORT 的 arg0 必须可读（K4④ 的原料）。
       LONG 覆盖的是分发结果，不是载荷来源。 */
    acks = 0;
    queue(PMU_EVT_KEY_SHORT, 51, 111, 0);
    queue(PMU_EVT_KEY_SHORT, 52, 222, 0);
    queue(PMU_EVT_KEY_LONG,  53, 999, 0);
    held = 0;
    assert(read_pico_pmu_take_key_action(&held) == READ_PICO_PMU_KEY_LONG);
    assert(qn == 0 && held == 111);

    printf("PASS: pmu bus take path (K2a zero value-chain reads, K4 take-all/override, K9.2 no swallow)\n");
    return 0;
}
"""


def main():
    code = unit + "\n".join(
        extract(f) for f in [
            "pmu_crc16_ccitt_false", "wr16", "crc_ok", "parse_event",
            "peek_next_event", "read_pico_pmu_event_ack",
            "read_pico_pmu_drain_events", "read_pico_pmu_take_key_action",
        ]
    ) + body

    with tempfile.TemporaryDirectory() as tmp:
        p = Path(tmp)
        (p / "driver").mkdir()
        (p / "driver/i2c_master.h").write_text(
            "#pragma once\ntypedef void *i2c_master_bus_handle_t;\n")
        (p / "esp_err.h").write_text(
            "#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n"
            "#define ESP_ERR_INVALID_RESPONSE 0x108\n"
            "#define esp_err_to_name(e) \"err\"\n")
        src = p / "t.c"
        src.write_text(code)
        exe = p / "t"
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            f"-I{root/'components/read_pico_pmu/include'}", f"-I{tmp}",
            str(src), "-o", str(exe),
        ], check=True)
        subprocess.run([str(exe)], check=True)


main()
