#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
"""
26.9.15 结构牙：取证工具自身不许再埋雷（S7）。

为什么需要这份检查：26.9.14 的发布件里有一个**静默丢弃**的缺陷——站点表只有 8 格，
而调用点有 9 个不同站点名，第 9 个撞上满表就 `slot=-1`，dump 一声不吭地消失。
同一次回头看还发现「健康时沉默」的规则在 `largest=21,504` 那一刻把唯一次点名机会省掉了。
两个缺陷形状相同：**为了取证而引入的工具，自己在关键那一刻不说话。**

黑匣子（B1）引入了三个新的失效方式，每一条都能让数据变成假数据：
  - 缓冲放 .bss → 从池 A 吃掉 1 KB，把 21,504 测成 20,480（测量工具住在被测对象里）；
  - 拿不到 PSRAM 就降级到内部 RAM → 同上，而且更隐蔽；
  - 未连接也打印 → VFS 直接返回 EIO，日志从未写出，现场表现为「怎么没有」。

H1  站点表容量 ≥ 实际调用点去重数。
H2  总预算 ≥ 站点数 × 每站点份数。
H3  满表分支必须打印 DROPPED（不许静默）。
H4  黑匣子缓冲必须是 PSRAM 分配，且源码里不存在静态数组形态的 s_pts。
H5  黑匣子拿不到 PSRAM 必须自我禁用并打印 DISABLED，且失败路径不得出现 MALLOC_CAP_INTERNAL。
H6  pico_bootbb_start() 必须排在 usb_storage_phy_init() **之后**（26.9.16 R1 反转了
    26.9.15 的结论：放第一条会让 TF 卡完全不挂载）。权威检查在 check_boot_order.py 的
    B1，这里是同向冗余——两处都留着，任何一处被误删还有另一处兜底。
H7  `boot_probe()` **不得**再调用 `pico_bootbb_poll()`，出口必须落在主循环。
    26.9.15 把它放在这里（还要求排在 early return 之前），真机 13.8 s 才吐：
    `boot_probe()` 被 `!touch.touched && 距上次输入 >= 10 s` 门控，用户一直摸屏
    就永远轮不到它。26.9.18 把出口挪到主循环的 500 ms 块（`check_bootbb_ready`
    的 B6 守具体位置），所以这条反过来守：这里出现它，就是又回到了被门控的老路。
H8  打印前必须检查 usb_serial_jtag_is_connected()。
H9  delta 必须**逐段**列（`cut` 行 + `delta: none` 空分支），不能只报最大一次。

用法：python3 tools/check_heapdiag.py   （0 = 通过）
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
HD_C = ROOT / "components/pico_heapdiag/pico_heapdiag.c"
HD_H = ROOT / "components/pico_heapdiag/include/pico_heapdiag.h"
BB_C = ROOT / "components/pico_heapdiag/pico_bootbb.c"
APP_C = ROOT / "main/app_main.c"
TIME_C = ROOT / "main/time_service.c"

fails = []


def bad(msg):
    fails.append(msg)
    print("FAIL: " + msg)


def read(p):
    if not p.exists():
        bad("missing file %s" % p.relative_to(ROOT))
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


hd_c = read(HD_C)
hd_h = read(HD_H)
bb_c = read(BB_C)
app_c = read(APP_C)
time_c = read(TIME_C)

# ---- H1：站点表容量 ≥ 调用点去重数 -------------------------------------------
m = re.search(r"#define\s+SITE_SLOTS\s+(\d+)", hd_c)
slots = int(m.group(1)) if m else 0
if not m:
    bad("H1 pico_heapdiag.c: SITE_SLOTS not found")

# 收集全仓所有 pico_heap_dump("literal", ...) 的站点名。黑匣子自己的站点不在表内
# （它不走这套限额），所以排除 bootbb 前缀。
sites = set()
for f in list(ROOT.glob("main/**/*.c")) + list(ROOT.glob("components/**/*.c")):
    try:
        txt = f.read_text(encoding="utf-8", errors="replace")
    except OSError:
        continue
    for mm in re.finditer(r'pico_heap_dump\(\s*"([^"]+)"', txt):
        sites.add(mm.group(1))
if len(sites) > slots:
    bad("H1 %d distinct sites vs %d slots: the excess is DROPPED silently "
        "(this is exactly the 26.9.14 defect)" % (len(sites), slots))

# ---- H2：总预算 ≥ 站点数 × 每站点份数 ----------------------------------------
m_per = re.search(r"#define\s+PICO_HEAP_DUMP_PER_SITE\s+(\d+)", hd_h)
m_tot = re.search(r"#define\s+PICO_HEAP_DUMP_TOTAL\s+(\d+)", hd_h)
if not m_per or not m_tot:
    bad("H2 PICO_HEAP_DUMP_PER_SITE / PICO_HEAP_DUMP_TOTAL not found in the header")
else:
    per, tot = int(m_per.group(1)), int(m_tot.group(1))
    need = max(len(sites), 1) * per
    if tot < need:
        bad("H2 budget %d < %d sites x %d per site = %d: the last sites never get a dump"
            % (tot, max(len(sites), 1), per, need))

# ---- H3：满表必须打印，不许静默 ----------------------------------------------
if "table_full" not in hd_c:
    bad("H3 pico_heapdiag.c: no table_full branch - a full table would return silently")
elif not re.search(r'if\s*\(\s*table_full\s*\)\s*\{[^}]*ESP_LOGW', hd_c, re.S):
    bad("H3 the table_full branch must ESP_LOGW (DROPPED ...), not fall through silently")
if "DROPPED" not in hd_c:
    bad("H3 the drop path never says DROPPED: field logs cannot tell 'not fired' from 'dropped'")

# ---- H4：缓冲必须是 PSRAM，且不存在静态数组形态 ------------------------------
if not re.search(r"heap_caps_malloc\([^;]*MALLOC_CAP_SPIRAM", bb_c, re.S):
    bad("H4 pico_bootbb.c: buffer is not allocated with MALLOC_CAP_SPIRAM")
if re.search(r"static\s+bb_(blob|point)_t\s+s_(blob|pts)\s*\[", bb_c):
    bad("H4 pico_bootbb.c: the buffer is a static array - it would live in .bss, i.e. INSIDE "
        "pool A, and measure 21,504 as 20,480")
if "MALLOC_CAP_SPIRAM" not in bb_c:
    bad("H4 pico_bootbb.c never asks for MALLOC_CAP_SPIRAM")

# ---- H5：拿不到 PSRAM 必须自我禁用，且失败路径不许碰内部 RAM ----------------
if "DISABLED" not in bb_c:
    bad("H5 no DISABLED line: a failed blackbox would be invisible on the wire")
# 任何形态的「用内部 RAM 装黑匣子缓冲」都要抓——不管是改 caps（M40）还是加一条降级重试（M41）。
# 注意不能用 `find("if (s_blob == NULL)")` 定位：`bb_sample()` 与 `pico_bootbb_note()` 开头
# 也有同样的一行，第一版就是这么漏掉 M41 的（检查抓的是错的那一处）。
# Catch ANY form of "buffer allocated from internal RAM" - whether by swapping the caps
# (M40) or by adding a fallback retry (M41). Do NOT locate it via `find("if (s_blob == NULL)")`:
# `bb_sample()` and `pico_bootbb_note()` open with the same line, and v1 of this check
# latched onto the wrong one.
if re.search(r"s_blob\s*=\s*\(bb_blob_t\s*\*\)\s*heap_caps_malloc\s*\([^;]*MALLOC_CAP_INTERNAL",
             bb_c):
    bad("H5 the blackbox buffer is allocated from internal RAM somewhere: that is the "
        "poisoned-data scenario the ruling forbade (pool A would read 20,480 instead of 21,504)")
if "if (s_blob == NULL)" not in bb_c:
    bad("H5 the s_blob == NULL failure branch is missing entirely")

# ---- H6：start 必须排在 PHY 交还之后（与 check_boot_order.py 的 B1 同向）--------
# 26.9.15 的结论是「越早越好」，26.9.16 的真机事实把它推翻了：放第一条会让 TF 卡
# 完全不挂载。这条检查现在是「不许回到第一条」，而不是「必须在第一条」。
# 26.9.15 said "the earlier the better"; the 26.9.16 field result reversed it - calling it
# first stops the TF card from mounting at all. The check now forbids the old order.
i = app_c.find("void app_main(void)")
if i < 0:
    bad("H6 app_main not found")
else:
    body = app_c[i:]
    start_at = body.find("pico_bootbb_start()")
    phy_at = body.find("usb_storage_phy_init()")
    if start_at >= 0 and phy_at >= 0 and start_at < phy_at:
        bad("H6 pico_bootbb_start() runs BEFORE usb_storage_phy_init(): that is 26.9.15, and "
            "it stopped the TF card from mounting (books 7 -> 0, opendir ENOENT)")

# ---- H7：出口不许再留在 boot_probe（26.9.18 反转）----------------------------
# 26.9.15/26.9.16 这条守的是「poll 必须在 boot_probe 的每条 early return 之前」。
# 26.9.17 真机证明那个位置本身就是病根：boot_probe 只在「10 s 无输入」时才跑，
# 用户一直摸屏，黑匣子就 13.8 s 都不吐。出口已挪到主循环，这条现在守反面：
# 这里一旦再出现 poll，就等于又把它塞回 idle 门控后面。
# 26.9.15/26.9.16 guarded "poll before every early return in boot_probe". The 26.9.17
# field log proved the position itself was the disease: boot_probe only runs after 10 s
# without input. The outlet moved to the main loop; this now guards the opposite.
i = time_c.find("static void boot_probe(")
if i < 0:
    bad("H7 boot_probe not found")
else:
    body = time_c[i:time_c.find("\n}", i)]
    if "pico_bootbb_poll()" in body:
        bad("H7 pico_bootbb_poll() is back inside boot_probe(): that function is gated on "
            "'10 s without input', so a user who keeps touching the screen starves the "
            "spill - this is the 13.8 s of 26.9.15")

# ---- H8：打印前必须检查连接 --------------------------------------------------
if not re.search(r"if\s*\(\s*!\s*usb_serial_jtag_is_connected\(\)\s*\)\s*\{", bb_c):
    bad("H8 pico_bootbb.c does not gate the spill on `if (!usb_serial_jtag_is_connected())`: "
        "the VFS returns EIO when disconnected and the spill is lost without a trace")

# ---- H9：delta 必须存在 ------------------------------------------------------
# 两条分支都要有（无下降 / 有下降），而且都必须叫 delta:——只查一次的话，改掉其中一条
# 仍能通过（M45 第一版就是这么活下来的）。
# Both branches must exist and both must say `delta:`; checking only once lets a single
# branch be renamed and still pass (that is how v1 of M45 survived).
if '"[DELAYED-BOOT] delta: none' not in bb_c:
    bad("H9 the 'delta: none' branch is missing or renamed: a flat curve must still be stated")
# 26.9.18 S2-b：从「只报最大一次」改成逐段列。26.9.17 的两块 1 KB 是先后掉的，
# 只报最大那块把另一次藏了，逼得账要手算才知道是 −2,048。
if '"[DELAYED-BOOT] cut %u: largest' not in bb_c:
    bad("H9 the per-cut line ('[DELAYED-BOOT] cut %u: largest') is missing: only the "
        "biggest drop would be reported, and the 26.9.17 ledger had two")

if fails:
    print("check_heapdiag: FAIL (%d)" % len(fails))
    sys.exit(1)

print("check_heapdiag: PASS (%d slots >= %d sites / budget %s / DROPPED loud / blackbox in "
      "PSRAM / fail-closed / start after PHY hand-back / outlet in main loop / "
      "host-gated / per-cut delta)"
      % (slots, len(sites), m_tot.group(1) if m_tot else "?"))
