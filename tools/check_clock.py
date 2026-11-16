#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
"""
26.9.14 「对时不再砸堆」的结构牙：把 A1/A2/D1/D2/D3 五条不变量钉死。

为什么需要这份检查：26.9.13 的病灶不是逻辑错，是**生命周期**错——一次注定失败的
对时在开机时拉起射频、15 秒后 teardown，把一个 32 KB 内部堆池切成 5 段。这种缺陷
没有断言能直接捕捉（它不改变任何函数的返回值），只能在结构上禁止「离线自启射频」
这个动作重新长回来，并且保证取证探针一直在案发现场。

C1  app_time_service_poll 在拉起 clock_worker 之前，必须有一道
    `#if !CONFIG_CLOCK_OFFLINE_AUTO_SYNC` + `if (!online) return;` 的闸。
C2  临时对时通路的 ip_deadline 不得超过 6 s（且不得小于 1 s，防手滑写 0）。
C3  boot-pre / boot-post 两个采样点存在，且采样时刻晚于 31.4 s 的旧 teardown（≥35 s）。
C4  逐块清单挂在 boot-post 上，病发判据是 BLE 的 20 K。
C5  框选单击的三种结局（miss / ignored / ok）都被记下来。
C6  Kconfig 里 CLOCK_OFFLINE_AUTO_SYNC 默认关闭。

用法：python3 tools/check_clock.py   （0 = 通过）
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TIME_C = ROOT / "main/time_service.c"
XFER_C = ROOT / "components/read_pico_transfer/read_pico_transfer.c"
BOOK_C = ROOT / "main/apps/app_book.c"
KCONFIG = ROOT / "main/Kconfig.projbuild"

fails = []


def bad(msg):
    fails.append(msg)
    print("FAIL: " + msg)


def read(p):
    if not p.exists():
        bad("missing file %s" % p.relative_to(ROOT))
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


time_src = read(TIME_C)
xfer_src = read(XFER_C)
book_src = read(BOOK_C)
kcfg = read(KCONFIG)

# ---- C1：离线不得自启射频 ----------------------------------------------------
# 取 poll 函数体里「判定 online」到「创建 clock_worker」之间的那段，闸必须在这段里。
start = time_src.find("void app_time_service_poll")
spawn = time_src.find("xTaskCreate(clock_worker", start if start >= 0 else 0)
if start < 0:
    bad("C1 time_service.c: app_time_service_poll not found")
elif spawn < 0:
    bad("C1 time_service.c: clock_worker spawn not found (did the whole path move?)")
else:
    seg = time_src[start:spawn]
    if "#if !CONFIG_CLOCK_OFFLINE_AUTO_SYNC" not in seg:
        bad("C1 offline auto-sync guard missing: `#if !CONFIG_CLOCK_OFFLINE_AUTO_SYNC` "
            "must appear between the online verdict and the clock_worker spawn")
    if not re.search(r"if\s*\(\s*!online\s*\)\s*return\s*;", seg):
        bad("C1 offline auto-sync guard body missing: `if (!online) return;` must appear "
            "between the online verdict and the clock_worker spawn")

# ---- C2：临时对时等待不得超过 6 s -------------------------------------------
m = re.search(r"ip_deadline\s*=\s*esp_timer_get_time\(\)\s*\+\s*(\d+)\s*;", xfer_src)
if not m:
    bad("C2 read_pico_transfer.c: ip_deadline literal not found")
else:
    us = int(m.group(1))
    if us > 6000000:
        bad("C2 ip_deadline is %d us (> 6 s): the 26.9.13 log measured 15,104 ms of nothing"
            % us)
    if us < 1000000:
        bad("C2 ip_deadline is %d us (< 1 s): an AP cannot answer that fast" % us)

# ---- C3：开机两个采样点 ------------------------------------------------------
if 'pico_heap_dump("boot-pre"' not in time_src:
    bad("C3 time_service.c: boot-pre sample missing")
if 'pico_heap_dump("boot-post"' not in time_src:
    bad("C3 time_service.c: boot-post sample missing")
m = re.search(r"#define\s+CLOCK_BOOT_PROBE_US\s+\(?\s*(\d+)\s*LL\s*\*\s*1000000", time_src)
if not m:
    bad("C3 CLOCK_BOOT_PROBE_US literal not found")
else:
    secs = int(m.group(1))
    if secs < 35:
        bad("C3 CLOCK_BOOT_PROBE_US is %d s: must be past the old 31.4 s teardown, "
            "otherwise boot-post samples mid-radio and proves nothing" % secs)

# ---- C4：逐块清单挂在 boot-post、判据必须**高于**闸门 -------------------------
# 26.9.15 E2 修正了 C4 的判据：旧版要求「病发判据 = 闸门值 20,480」，结果 26.9.14 那台机
# 读到 21,504（刚好过线）就把唯一次点名机会省掉了。新判据是「不宽裕就点名」：阈值必须
# 严格高于闸门，否则「刚好过线」这一档永远看不到逐块清单。
if 'pico_heap_dump_blocks("boot-post", CLOCK_BOOT_BLOCKS_LARGEST)' not in time_src:
    bad("C4 time_service.c: boot-post does not walk blocks with CLOCK_BOOT_BLOCKS_LARGEST")
if "#define CLOCK_BLE_GATE_LARGEST (20 * 1024)" not in time_src:
    bad("C4 CLOCK_BLE_GATE_LARGEST must be (20 * 1024) - the same number the BLE gate uses")
m = re.search(r"#define\s+CLOCK_BOOT_BLOCKS_LARGEST\s+\(\s*(\d+)\s*\*\s*1024\s*\)", time_src)
if not m:
    bad("C4 CLOCK_BOOT_BLOCKS_LARGEST literal not found")
elif int(m.group(1)) * 1024 <= 20 * 1024:
    bad("C4 CLOCK_BOOT_BLOCKS_LARGEST is %d K: must be STRICTLY above the 20 K gate, "
        "otherwise a unit reading 21,504 again walks nothing" % int(m.group(1)))
elif int(m.group(1)) * 1024 < 24 * 1024:
    bad("C4 CLOCK_BOOT_BLOCKS_LARGEST is %d K: the 4 KB safety pad (24 K) was the ruling"
        % int(m.group(1)))

# ---- C5：单击三种结局都要留痕 ------------------------------------------------
if 'boxsel: tap (%d,%d) page=%u -> miss' not in book_src:
    bad("C5 app_book.c: tap miss is not logged (this is where 'nothing happened' hides)")
if 'took ? "ok" : "ignored"' not in book_src:
    bad("C5 app_book.c: tap ok/ignored is not logged")
if "const bool took = box_select_tap(" not in book_src:
    bad("C5 app_book.c: box_select_tap result is not captured for the log")

# ---- C6：Kconfig 默认关 ------------------------------------------------------
m = re.search(r"config\s+CLOCK_OFFLINE_AUTO_SYNC\s*\n(.*?)(?=\nconfig\s|\nendmenu)",
              kcfg, re.S)
if not m:
    bad("C6 Kconfig.projbuild: CLOCK_OFFLINE_AUTO_SYNC not declared")
elif not re.search(r"^\s*default\s+n\s*$", m.group(1), re.M):
    bad("C6 CLOCK_OFFLINE_AUTO_SYNC must default to n - it is a stop-loss, not an option")

if fails:
    print("check_clock: FAIL (%d)" % len(fails))
    sys.exit(1)

print("check_clock: PASS (no offline radio at boot / <=6 s deadline / boot-pre+boot-post "
      "pinned / block walk on illness / three tap endings logged / switch defaults off)")
