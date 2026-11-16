#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
"""
26.9.16 「开机时序不许再压到 TF 卡头上」的结构牙。

为什么需要这份检查：26.9.15 引入的缺陷没有任何断言能直接捕捉——黑匣子照样建、照样
采样、没有任何函数返回错误，可 TF 卡从开机起就没进过初始化流程。能把这种缺陷关在
门外的东西只有两条：**启动顺序**和**开关**。所以这份检查只做这两件事，外加把
26.9.15 那个「一次都不探测、一句日志不打」的探测条件钉死。

B1  `app_main` 里 `pico_bootbb_start()` 必须**排在** `usb_storage_phy_init()` 之后。
    26.9.15 放在第一条，真机 TF 卡不挂载（books 7→0、opendir ENOENT、
    池 A alloc_blocks 14→11）。这条不成立，其它都不用看。
B2  那个调用必须被 `#if CONFIG_PICO_BOOTBB` 包住——诊断代码不许无条件常开。
B3  `poll_media` 里针对 `ESP_ERR_INVALID_STATE` 的探测分支**不得**再要求
    `info.present`。加了它就回到 26.9.15：`s_ioe == NULL` 时两个条件同时为假，
    探测永不启动、日志一行不出。
B4  `poll_media` 必须保留状态变化日志（`sd: err=`）与自愈日志（`sd: remount`）。
    这是 26.9.15 现场「书库空了但什么都不说」的直接补救。
B5  Kconfig 里 `PICO_BOOTBB` 默认 **y**（26.9.18 反转）。26.9.16～26.9.17 这条守的是
    「默认 n」，理由是「一次杀死卡槽的诊断不能默认开」；26.9.18 审计批准重开做归因，
    B1（位置）与 check_heapdiag 的 H4（缓冲必须在 PSRAM）一条都没放松，所以 B5 现在
    守的是反面：默认必须是**显式**的 y，不许靠依赖项顺带。

用法：python3 tools/check_boot_order.py   （0 = 通过）
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
APP_MAIN = ROOT / "main/app_main.c"
APP_LOOP = ROOT / "main/app/app_loop.c"
KCONFIG = ROOT / "main/Kconfig.projbuild"

fails = []


def bad(msg):
    fails.append(msg)
    print("FAIL: " + msg)


def read(p):
    try:
        return p.read_text(encoding="utf-8")
    except OSError as exc:
        bad("cannot read %s: %s" % (p, exc))
        return ""


main_src = read(APP_MAIN)
loop_src = read(APP_LOOP)
kcfg = read(KCONFIG)

# ---- B1：启动顺序 ------------------------------------------------------------
# 只认 `app_main` 函数体内的位置，注释里提到这两个名字不算数。
m = re.search(r"void\s+app_main\s*\([^)]*\)\s*\{", main_src)
if not m:
    bad("B1 app_main() not found in main/app_main.c")
else:
    body = main_src[m.end():]
    # 截到函数体结束：靠行首的 "}" 收尾（app_main 是文件里最后一个顶层函数）
    end = re.search(r"\n\}", body)
    if end:
        body = body[:end.start()]
    phy = body.find("usb_storage_phy_init()")
    bb = body.find("pico_bootbb_start()")
    if bb < 0:
        # 开关关掉是预处理阶段的事，源文件里的调用必须一直在。删掉它就是删掉能力。
        # The switch acts at preprocess time; the call must stay in the source file.
        bad("B1 app_main never calls pico_bootbb_start(): the capability was deleted, not "
            "switched off")
    elif phy < 0:
        bad("B1 usb_storage_phy_init() is gone from app_main: the ordering anchor is missing")
    elif bb < phy:
        bad("B1 pico_bootbb_start() at offset %d runs BEFORE usb_storage_phy_init() at %d. "
            "That is exactly 26.9.15, and it stops the TF card from mounting at all "
            "(books 7 -> 0, opendir ENOENT, pool A alloc_blocks 14 -> 11)." % (bb, phy))

# ---- B2：开关守卫 ------------------------------------------------------------
if "pico_bootbb_start()" in main_src:
    guarded = re.search(
        r"#if\s+CONFIG_PICO_BOOTBB\s*\n[^\n]*\n?\s*pico_bootbb_start\(\)\s*;", main_src)
    if not guarded:
        # 允许中间夹注释行：退一步，只要 #if 出现在调用点之前且 #endif 在之后
        alt = re.search(
            r"#if\s+CONFIG_PICO_BOOTBB((?:(?!#endif).)*?)pico_bootbb_start\(\)", main_src,
            re.S)
        if not alt:
            bad("B2 pico_bootbb_start() is not behind `#if CONFIG_PICO_BOOTBB`: diagnostic "
                "code must never be unconditional")
        elif len(alt.group(1)) > 600:
            bad("B2 the `#if CONFIG_PICO_BOOTBB` guard sits %d chars before the call - too far "
                "to prove it actually wraps it" % len(alt.group(1)))

# ---- B3：探测条件不得再要求 info.present ------------------------------------
i = loop_src.find("static bool poll_media(")
if i < 0:
    bad("B3 poll_media() not found in main/app/app_loop.c")
else:
    j = loop_src.find("\nstatic ", i + 10)
    seg = loop_src[i:j if j > 0 else len(loop_src)]
    # 找到针对 INVALID_STATE 的那条 if，检查它到 '{' 之间有没有 info.present
    for mm in re.finditer(r"if\s*\(([^)]*ESP_ERR_INVALID_STATE[^)]*)\)\s*\{", seg):
        cond = mm.group(1)
        if "info.present" in cond:
            bad("B3 poll_media() gates the probe on `info.present` again (%s). When "
                "s_ioe == NULL the detect call returns ESP_ERR_INVALID_STATE and leaves "
                "present false, so the probe never starts and not one line is logged - "
                "that is 26.9.15.\n      condition: %s" % ("B3", " ".join(cond.split())))
    if "ESP_ERR_INVALID_STATE" not in seg:
        bad("B3 the ESP_ERR_INVALID_STATE probe branch is gone from poll_media()")

# ---- B4：状态变化日志与自愈日志 ---------------------------------------------
# 必须匹配「行首（允许缩进）就是 ESP_LOGx」——只查子串会被 `if (0) ESP_LOGW(...)`
# 这类变异骗过去：字符串还在，语句永远不执行。26.9.15 的 H9 就是这么漏掉一条变异的。
# The match must be a statement whose first token is ESP_LOGx: a plain substring test is
# fooled by `if (0) ESP_LOGW(...)`, where the literal survives but never executes - the
# same hole that let one mutation through in 26.9.15.
if not re.search(r'(?m)^\s*ESP_LOG[WIDE]\([^,\n]+,\s*"sd: err=', loop_src):
    bad("B4 poll_media() has no executable `ESP_LOGx(TAG, \"sd: err=...\")` line: 26.9.15's "
        "field symptom was an empty shelf with zero explanation")
if not re.search(r'(?m)^\s*ESP_LOG[WIDE]\([^,\n]+,\s*"sd: remount', loop_src):
    bad("B4 poll_media() has no executable `ESP_LOGx(TAG, \"sd: remount...\")` line: a failed "
        "mount must not stay failed until the user re-seats the card")

# ---- B5：Kconfig 默认开（26.9.18 反转）----------------------------------------
# 26.9.16/26.9.17 守的是「默认 n」。26.9.18 审计批准重开做归因：热插拔已修完、
# TF 卡回归已由真机三轮验证，位置约束（B1）与缓冲位置（H4）两条硬牙都还在，
# 所以这里反过来要求显式的 `default y`——不能靠依赖项顺带默认，必须是写死的。
# 26.9.16/26.9.17 guarded "default n". 26.9.18 re-arms it for attribution: hot-plug is
# fixed and the card regression was confirmed gone across three field cycles, while B1 and
# H4 both still hold. So this now requires an EXPLICIT `default y`, not an inherited one.
m = re.search(r"config\s+PICO_BOOTBB\b(.*?)(?=\nconfig\s|\nendmenu|\Z)", kcfg, re.S)
if not m:
    bad("B5 Kconfig has no `config PICO_BOOTBB`")
else:
    block = m.group(1)
    if re.search(r"default\s+n", block):
        bad("B5 PICO_BOOTBB defaults to n: 26.9.18 needs it armed for heap attribution")
    elif not re.search(r"default\s+y", block):
        bad("B5 PICO_BOOTBB has no explicit `default y`")

if fails:
    print("\ncheck_boot_order: %d violation(s)" % len(fails))
    sys.exit(1)
print("check_boot_order: PASS (B1 order, B2 switch, B3 probe condition, B4 logs, B5 default y)")
