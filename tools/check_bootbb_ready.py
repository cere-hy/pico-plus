#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
"""
26.9.18「黑匣子必须吐得出来、日志必须说人话」的结构牙。

为什么需要这份检查：26.9.17 真机暴露的两类问题，都属于「代码照样跑、结果照样对，
但证据链断了」——黑匣子 13.8 s 才吐，是因为它的出口被 `app_time_service_poll()`
的「10 s 无输入」门控卡住；`sd: probe start` 一行永远打不出来，是因为判据写反了，
成功启动恰恰进入不打印的分支。这两种缺陷都不会让任何断言变红，只能靠结构牙钉。

B6  `pico_bootbb_poll()` 必须被**主循环**调用，且**不得**再出现在
    `time_service.c` 的 `boot_probe()` 里。26.9.15 实测 13.8 s 才吐：出口挂在
    「无触摸且距上次输入 ≥ 10 s」的分支后面，用户一直摸屏就永远轮不到它。
B7  采样周期必须是 50 ms。200 ms 抓不住连续分配——26.9.17 现场那 1 KB 是 1 ms 内
    掉的，200 ms 只能看到「有过这么一回事」，看不到它和谁相邻。
B8  `delta` 必须**逐段**列，不能只报最大一次。26.9.17 的两块 1 KB 是先后两次掉的，
    只报最大那块等于把另一次藏起来，逼得账要手算才知道是 −2,048。
B9  `mount_card()` 必须有 SD_STAGE 三段打点（进入 / 进 VFS 挂载 / 返回）。插回到
    挂载完成固定 4.0 s，而驱动日志只到 `input line delay`，之后那段 3.5 s 是黑盒。
B10 `sd: probe start` 必须**无条件**打印（N1）。26.9.17 写成
    `if (perr != ESP_ERR_NOT_FINISHED)` 才打——而成功启动恰恰返回 NOT_FINISHED，
    于是这条日志只在失败时出现，验收清单却让人去找它。
B11 `release=` 必须映射成 ok/fail/`-`（N2）。26.9.17 直接印 `esp_err_to_name()`，
    插回成功后仍挂着上一轮的 NOT_FOUND，读起来像「卸载一直失败」。
B12 增量打印（26.9.19 S5）。`bb_spill()` 里不得再出现 `if (from == 0)` 把
    header/cut/delta/floor/events 整块包住——26.9.18 正是因为它，那七类内容
    「一生只打一次」，偏偏那一次落在串口没接稳的窗口，证据一次性丢光。必须有三组
    独立游标（点 `s_printed` / 事件 `s_notes_printed` / cut 编号跨段连续），
    必须有 `spill: %u/%u points, notes %u/%u` 诊断行与 `FULL` 标记，
    必须有 20 s 心跳（`hb:` + `PICO_BOOTBB_HB_MS`），且 `pico_bootbb_poll()`
    的「有新东西」判据必须同时看点和事件——只看点，拔插写进去的 sd-stage
    就永远没有出口。
B13 prep 栈在 PSRAM（26.9.19 S8）。Kconfig 默认 y、sdkconfig 与 sdkconfig.defaults
    都置 y、创建路径带 `MALLOC_CAP_SPIRAM`、删除路径走 `vTaskDeleteWithCaps`。
    最后一条不是形式主义：用 `vTaskDelete(NULL)` 会把 6 K 的 PSRAM 栈与 TCB
    悄悄漏掉，恰好把这项开关的全部收益赔回去，而且漏得无声无息。

用法：python3 tools/check_bootbb_ready.py   （0 = 通过）
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
APP_LOOP = ROOT / "main/app/app_loop.c"
TIME_SERVICE = ROOT / "main/time_service.c"
BOOTBB_C = ROOT / "components/pico_heapdiag/pico_bootbb.c"
BOOTBB_H = ROOT / "components/pico_heapdiag/include/pico_bootbb.h"
SD_C = ROOT / "components/read_pico/read_pico_sd.c"
KCFG = ROOT / "main/Kconfig.projbuild"
SDKCONFIG = ROOT / "sdkconfig"
SDKCONFIG_DEFAULTS = ROOT / "sdkconfig.defaults"
APP_BOOK = ROOT / "main/apps/app_book.c"


def code_only(src):
    """去掉注释后再查——正向和反向断言都只认代码。

    26.9.21 的教训：B20/B25/B26 第一版对着注释里那句『这里曾经用 f_getfree』判红，
    门禁自己被自己的说明文字绊倒。**注释不是行为**，两个方向都要剥。
    Strip comments before both positive and negative assertions.
    """
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            j = src.find("\n", i)
            i = n if j < 0 else j
        elif c == "/" and i + 1 < n and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
        elif c == '"':
            out.append(c)
            i += 1
            while i < n and src[i] != '"':
                if src[i] == "\\":
                    out.append(src[i]); i += 1
                if i < n:
                    out.append(src[i]); i += 1
            if i < n:
                out.append(src[i]); i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)



def func_body(src, sig, limit=20000):
    """从签名处取到配对的大括号——门禁只看**这个函数体**，不看全文。

    26.9.20 的教训：三条门禁（B14/B15/B17）第一版都是「全文搜关键词」，
    变异把函数体换成 return false 照样绿。结构检查必须落到函数体上。
    Slice the function body by brace matching; searching the whole file is what let
    three mutations survive in 26.9.20.
    """
    src = code_only(src)
    i = src.find(sig)
    if i < 0:
        return ""
    j = src.find("{", i)
    if j < 0:
        return ""
    depth = 0
    for k in range(j, min(len(src), j + limit)):
        if src[k] == "{":
            depth += 1
        elif src[k] == "}":
            depth -= 1
            if depth == 0:
                return src[j:k + 1]
    return ""


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


app_loop = read(APP_LOOP)
time_service = read(TIME_SERVICE)
bootbb_c = read(BOOTBB_C)
bootbb_h = read(BOOTBB_H)
sd_c = read(SD_C)
kcfg = read(KCFG)
sdkconfig = read(SDKCONFIG)
sdkconfig_defaults = read(SDKCONFIG_DEFAULTS)
app_book = read(APP_BOOK)


# ---- B6：出口必须在主循环，且不许留在 idle 门控后面 ---------------------------
# 位置检查是序级的：`pico_bootbb_poll();` 必须出现在 500 ms 块（
# `ctx.now_ms - last_network_poll_ms >= 500`）内部，且在
# `app_time_service_poll(` 之前——那条调用带着 `!touch.touched && ... >= 10000`
# 的门控，排在它后面就等于又被门控一次。
m = re.search(r"ctx\.now_ms\s*-\s*last_network_poll_ms\s*>=\s*500\)\s*\{", app_loop)
if not m:
    bad("B6 cannot find the 500 ms poll block in app_loop.c")
else:
    tail = app_loop[m.end():]
    # 只留代码行：块里那段解释性注释本身就提到了 `app_time_service_poll()`，
    # 直接 find 会命中注释而不是调用。
    code_tail = "\n".join(ln for ln in tail.splitlines() if not ln.strip().startswith("//"))
    poll_at = code_tail.find("pico_bootbb_poll();")
    tsvc_at = code_tail.find("app_time_service_poll(")
    if poll_at < 0:
        bad("B6 pico_bootbb_poll() is not called from the main loop's 500 ms block")
    elif tsvc_at >= 0 and poll_at > tsvc_at:
        bad("B6 pico_bootbb_poll() sits AFTER app_time_service_poll(), which is gated on "
            "10 s without input - that is exactly the 13.8 s spill of 26.9.15")
    else:
        # 门控检查：poll 那一行本身不许带条件（`if (...) pico_bootbb_poll();`）。
        line_start = code_tail.rfind("\n", 0, poll_at) + 1
        line_end = code_tail.find("\n", poll_at)
        line = code_tail[line_start:line_end if line_end >= 0 else len(code_tail)].strip()
        if line != "pico_bootbb_poll();":
            bad("B6 pico_bootbb_poll() is not an unconditional statement (found: %r): a user "
                "who keeps touching the screen would starve the spill" % line)

if re.search(r"\bpico_bootbb_poll\(\)\s*;", time_service):
    bad("B6 pico_bootbb_poll() is still called from time_service.c; it must live in the "
        "main loop only, otherwise the idle gate can still starve it")


# ---- B7：采样周期 50 ms -------------------------------------------------------
m = re.search(r"#define\s+PICO_BOOTBB_PERIOD_US\s+(\d+)", bootbb_h)
if not m:
    bad("B7 PICO_BOOTBB_PERIOD_US is not defined")
elif int(m.group(1)) != 50000:
    bad("B7 PICO_BOOTBB_PERIOD_US is %s us, must be 50000 (50 ms): 200 ms cannot resolve "
        "consecutive allocations" % m.group(1))


# ---- B8：delta 逐段列 ---------------------------------------------------------
# 两条分支都必须存在（26.9.16 的 H9 教训：只查一个子串，改掉另一条仍然通过）。
if 'cut %u: largest' not in bootbb_c:
    bad("B8 pico_bootbb.c has no per-cut line ('cut %u: largest ...'); the 26.9.17 ledger "
        "needed two cuts and only got the biggest")
if "delta: none" not in bootbb_c:
    bad("B8 pico_bootbb.c lost the 'delta: none' branch")
# 旧的「只报最大一次」实现用一个 best 变量挑最大值，必须已经消失。
if re.search(r"\bbest\s*=\s*d\s*;", bootbb_c):
    bad("B8 the old largest-only delta (best = d) is still present")


# ---- B9：mount_card 三段打点 --------------------------------------------------
if not re.search(r"static\s+esp_err_t\s+mount_card\s*\(", sd_c):
    bad("B9 cannot find mount_card() in read_pico_sd.c")
else:
    body_start = sd_c.find("static esp_err_t mount_card(")
    body = sd_c[body_start:body_start + 6000]
    # stage 3 的写法是 `(uint8_t)(3u | (err == ESP_OK ? 0x80u : 0u))`——阶段号套在
    # 强制转换里，所以不能要求它紧跟逗号，只在该 note 的实参窗口里找这个数字。
    for stage in ("1", "2", "3"):
        pat = r"pico_bootbb_note\(\s*PICO_BOOTBB_NOTE_SD_STAGE\s*,"
        found = False
        for mm in re.finditer(pat, body):
            seg = body[mm.end():mm.end() + 140]
            if re.search(r"(?<![0-9a-fA-Fx])%su?(?![0-9a-fA-F])" % stage, seg):
                found = True
                break
        if not found:
            bad("B9 mount_card() has no SD_STAGE note for stage %s" % stage)
    if body.count("PICO_BOOTBB_NOTE_SD_STAGE") < 3:
        bad("B9 mount_card() carries fewer than 3 SD_STAGE notes")


# ---- B10：probe start 无条件 --------------------------------------------------
if "sd: probe start ok" not in app_loop:
    bad("B10 the 'sd: probe start ok' line is missing: NOT_FINISHED is the SUCCESS return "
        "of read_pico_sd_start_probe(), so a `!= NOT_FINISHED` guard makes the line dead")
else:
    # 行级：`read_pico_sd_start_probe()` 之后 14 行内，不许出现把它反过来的判据。
    # 认准 poll_media 里那一处：字体重试分支也调 start_probe()，只是不接返回值，
    # 从第一处开窗会截在完全无关的代码上（M62 第一版就是这么活下来的）。
    # Anchor on the poll_media call: the font-retry branch calls start_probe() too, and it
    # discards the result. Opening the window at the first hit lands in unrelated code -
    # which is exactly how v1 of M62 survived.
    idx = app_loop.find("esp_err_t perr = read_pico_sd_start_probe();")
    window = app_loop[idx:idx + 1400] if idx >= 0 else ""
    if idx < 0:
        bad("B10 cannot find `esp_err_t perr = read_pico_sd_start_probe();` in app_loop.c")
    # 注释里必须允许出现这个判据——26.9.18 就是靠那条注释记住 26.9.17 怎么错的。
    # 所以先剥掉整行注释，只在**可执行代码**里找。
    # The comment MUST be allowed to spell the wrong predicate out - that is how 26.9.18
    # remembers what 26.9.17 did. Strip comment lines and look at executable code only.
    window_code = "\n".join(ln for ln in window.splitlines()
                            if not ln.strip().startswith("//"))
    if idx >= 0 and re.search(r"perr\s*!=\s*ESP_ERR_NOT_FINISHED", window_code):
        bad("B10 the probe-start log is still gated on `perr != ESP_ERR_NOT_FINISHED`: "
            "that makes it print only on failure, never on success")
    if re.search(r"if\s*\(\s*!changed\s*\)", window_code):
        bad("B10 the probe-start log is inverted against `changed`")


# ---- B11：release= 映射 -------------------------------------------------------
if re.search(r"release=%s\"[^;]*esp_err_to_name\(s_last_release\)", app_loop, re.S):
    bad("B11 `release=` still prints esp_err_to_name(s_last_release): after a successful "
        "re-mount it keeps showing the previous NOT_FOUND, which reads like a failure")
# 三态必须都在格式串的实参窗口里：`-`（无记录）/ ok / fail。写成内联三元，
# 所以按字面量找，而不是找某个变量名。
if "release=%s" not in app_loop:
    bad("B11 the release= field is gone from the sd state line")
else:
    seg = app_loop[app_loop.find("release=%s"):][:1200]
    for lit, why in (('"-"', "no-record"), ('"ok"', "success"), ('"fail"', "failure")):
        if lit not in seg:
            bad("B11 release= does not map the %s state (%s)" % (lit, why))
    if not re.search(r"s_release_known", app_loop):
        bad("B11 s_release_known is missing: without it `release=` never returns to `-` "
            "once the card is back")


# ---- B12：增量打印（26.9.19 S5）------------------------------------------------
# 函数体 = 从 `static void bb_spill(` 到下一个顶层函数。
_sp_start = bootbb_c.find("static void bb_spill(")
if _sp_start < 0:
    bad("B12 cannot find bb_spill() in pico_bootbb.c")
    spill_body = ""
else:
    _sp_end = bootbb_c.find("\nvoid pico_bootbb_start(", _sp_start)
    spill_body = bootbb_c[_sp_start:_sp_end if _sp_end > _sp_start else len(bootbb_c)]

if spill_body:
    # D1 的精确指纹：汇总块被 `if (from == 0)` 整块包住。允许 `if (full)`（那是重放
    # 时把 cut 编号归零），但绝不允许再按「是不是第一次」门控任何打印。
    if re.search(r"\bfrom(?:_p)?\s*==\s*0\b", spill_body):
        bad("B12 bb_spill() is still gated on `from == 0`: that is D1 - header/cut/delta/"
            "floor/events printed once in a lifetime, and 26.9.18 lost that one shot")
    if not re.search(r"from_n\s*=\s*full\s*\?\s*0u\s*:\s*s_notes_printed", spill_body):
        bad("B12 the note cursor is not `s_notes_printed`: without it a reinsert note "
            "written after the first spill has no reader (D2, S3 was wasted)")
    if not re.search(r"spill:\s*%u/%u points", spill_body):
        bad("B12 the `spill: %u/%u points, notes %u/%u` diagnostic line is missing: "
            "26.9.18 could not tell 'no change' from 'timer died' from 'buffer full'")
    if "FULL - later changes are LOST" not in spill_body:
        bad("B12 the spill line lost its FULL marker")
    # 跨段 cut 编号：必须用持久计数器，不能每次从 1 数——否则两段日志的 cut 1 是两件事。
    if not re.search(r"\+\+\s*s_cut_seq|s_cut_seq\s*\+\+", spill_body):
        bad("B12 cut numbering is not carried across spills (no `++s_cut_seq`)")

if not re.search(r"#define\s+PICO_BOOTBB_HB_MS\s+20000", bootbb_h):
    bad("B12 PICO_BOOTBB_HB_MS is missing or not 20000: without a heartbeat, silence has "
        "three indistinguishable causes")
if "[DELAYED-BOOT] hb:" not in bootbb_c:
    bad("B12 the heartbeat line ('hb:') is missing from pico_bootbb_poll()")

# poll 的「有新东西」判据必须同时看点和事件。只看点 = 堆安静时拔插事件永远不吐。
_poll_start = bootbb_c.find("void pico_bootbb_poll(void)")
if _poll_start < 0:
    bad("B12 cannot find pico_bootbb_poll()")
else:
    _poll_end = bootbb_c.find("\nvoid pico_bootbb_replay(", _poll_start)
    poll_body = bootbb_c[_poll_start:_poll_end if _poll_end > _poll_start else len(bootbb_c)]
    if not (re.search(r"s_printed\s*<\s*s_n", poll_body) and
            re.search(r"s_notes_printed\s*<\s*s_notes", poll_body)):
        bad("B12 pico_bootbb_poll() gates on points only: a note written while the heap is "
            "quiet never reaches the port (D2)")

# ---- B13：prep 栈在 PSRAM（26.9.19 S8）-----------------------------------------
# 三处开关必须一致：Kconfig 默认 y、sdkconfig 生效、defaults 可复现。
_m = re.search(r"config\s+BOOK_PREP_STACK_PSRAM(.*?)(?=\nconfig\s|\Z)", kcfg, re.S)
if not _m:
    bad("B13 Kconfig has no BOOK_PREP_STACK_PSRAM entry")
else:
    _blk = _m.group(1)
    if not re.search(r"^\s*default\s+y\s*$", _blk, re.M):
        bad("B13 BOOK_PREP_STACK_PSRAM no longer defaults to y: 26.9.19 took it as a fix, "
            "not an experiment")
    if "26.9.19" not in _blk:
        bad("B13 the Kconfig help no longer records the 26.9.19 ruling")
if "CONFIG_BOOK_PREP_STACK_PSRAM=y" not in sdkconfig:
    bad("B13 sdkconfig does not enable CONFIG_BOOK_PREP_STACK_PSRAM")
if "CONFIG_BOOK_PREP_STACK_PSRAM=y" not in sdkconfig_defaults:
    bad("B13 sdkconfig.defaults does not enable it: a fresh clone would rebuild without it")

if not re.search(r"xTaskCreatePinnedToCoreWithCaps\(\s*prep_task.*?MALLOC_CAP_SPIRAM\s*\)",
                 app_book, re.S):
    bad("B13 the PSRAM-stack create call is gone or no longer asks for MALLOC_CAP_SPIRAM: "
        "the 6,144 B would quietly land back in pool A")
if "vTaskDeleteWithCaps(xTaskGetCurrentTaskHandle())" not in app_book:
    bad("B13 the self-delete no longer uses vTaskDeleteWithCaps: a plain vTaskDelete(NULL) "
        "would leak the 6 K PSRAM stack and its TCB, silently undoing S8")


# ================== 26.9.20：容量、相对时间、两步发布、路标 ==================

# ---- B14：黑匣子容量与「关键事件不丢」（S11）---------------------------------
_m = re.search(r"#define\s+PICO_BOOTBB_POINTS\s+(\d+)", bootbb_h)
if not _m or int(_m.group(1)) < 512:
    bad("B14 PICO_BOOTBB_POINTS is %s, needs >= 512: the 128-point ring filled at t=172.6 s "
        "and every later event (2nd book open, BLE, 3rd reinsert) was dropped"
        % (_m.group(1) if _m else "missing"))
_m = re.search(r"#define\s+PICO_BOOTBB_NOTES\s+(\d+)", bootbb_h)
if not _m or int(_m.group(1)) < 64:
    bad("B14 PICO_BOOTBB_NOTES is %s, needs >= 64: the 16-note ring filled at 160 s, so every "
        "later reinsert lost its three sd-stage notes" % (_m.group(1) if _m else "missing"))
if not re.search(r"#define\s+PICO_BOOTBB_SPILL_LINES\s+(\d+)", bootbb_h):
    bad("B14 PICO_BOOTBB_SPILL_LINES is missing: 512 points dumped at once block the main "
        "loop for seconds")
elif int(re.search(r"#define\s+PICO_BOOTBB_SPILL_LINES\s+(\d+)", bootbb_h).group(1)) > 128:
    bad("B14 PICO_BOOTBB_SPILL_LINES is over 128: one spill would hold the main loop again")
if "note_is_critical" not in bootbb_c:
    bad("B14 pico_bootbb.c has no note_is_critical(): with a 64-note ring a long session still "
        "loses the only structured evidence a hotplug leaves")
else:
    # 函数体必须**真的**把 sd-stage / sd-space 判成关键——只查函数名存在，
    # 把函数体换成 `return false;` 也照样绿（M74 第一版就是这么活下来的）。
    # The body must really name both codes: checking the function name alone lets a
    # `return false;` body pass, which is how v1 of M74 survived.
    _ci = bootbb_c.find("static bool note_is_critical(")
    _ci_body = bootbb_c[_ci:_ci + 400] if _ci >= 0 else ""
    for _code in ("PICO_BOOTBB_NOTE_SD_STAGE", "PICO_BOOTBB_NOTE_SD_SPACE"):
        if _code not in _ci_body:
            bad("B14 note_is_critical() does not treat %s as critical: once the ring fills, "
                "that evidence is dropped again" % _code)
if "s_note_evicted" not in bootbb_c:
    bad("B14 critical notes evict plain ones but the eviction is not counted: a silent "
        "eviction is the same sin as a silent drop")

# ---- B15：`at=` 必须是相对时间（S12 / D4）------------------------------------
# 必须查 `sd_stage_ms()` **自己**的函数体：26.9.20 的日志行里还有一处
# `esp_timer_get_time() - s_probe_t0`（打印总耗时用），全文搜索会被它蒙过去。
# Check the body of sd_stage_ms() itself: the log line elsewhere also subtracts s_probe_t0,
# and a whole-file search lets that one stand in for the stamp.
_sm = sd_c.find("static uint16_t sd_stage_ms(void)")
_sm_body = sd_c[_sm:_sm + 400] if _sm >= 0 else ""
if not _sm_body:
    bad("B15 cannot find sd_stage_ms()")
elif not re.search(r"esp_timer_get_time\(\)\s*-\s*s_probe_t0", _sm_body):
    bad("B15 the SD stage stamp is not relative to s_probe_t0: the absolute low-16-bit "
        "version wrapped at 142.6 s into 11,557 and read like '11.5 s'")
if re.search(r"#define\s+SD_STAGE_MS\(\)\s*\(\(uint16_t\)\(esp_timer_get_time\(\)", sd_c):
    bad("B15 SD_STAGE_MS() still takes the absolute millisecond count")
if not re.search(r"s_probe_t0\s*=\s*esp_timer_get_time\(\)", sd_c):
    bad("B15 s_probe_t0 is never set: a zero origin makes every at= a wrap of the uptime")

# ---- B16：stage 的「进入」与「失败」必须分开（S12 / D5）-----------------------
if "SD_STAGE_ENTER" not in sd_c:
    bad("B16 stage 1/2 carry no entry marker: without it they print FAIL forever (26.9.19)")
else:
    for stage in ("1", "2"):
        if not re.search(r"SD_STAGE_ENTER\(%s\)" % stage, sd_c):
            bad("B16 stage %s is not marked as an entry point" % stage)
if not re.search(r"0x08u", bootbb_c):
    bad("B16 pico_bootbb.c never looks at the 0x08 entry bit, so 'no result yet' still "
        "prints as FAIL")
if not re.search(r"\bentry\b", bootbb_c):
    bad("B16 the stage printer has no entry/success split")

# ---- B17：两步发布（S9b）----------------------------------------------------
if not re.search(r"static\s+void\s+fill_info\([^)]*bool\s+with_space", sd_c):
    bad("B17 fill_info() has no with_space switch: the 3.5 s f_getfree is back on the mount "
        "path and the reinsert answer waits for the FAT scan")
_probe = sd_c.find("static void probe_task(void* arg)")
_probe_body = sd_c[_probe:_probe + 2600] if _probe >= 0 else ""
if not _probe_body:
    bad("B17 cannot find probe_task()")
else:
    # 从 `owner = card;` 之后开窗：probe_task 上面还有一处「没卡」分支也会 publish，
    # 从函数头开窗会命中那一处，把顺序检查变成永远通过（M78 第一版就是这么活下来的）。
    # Window after `owner = card;`: the no-card branch above also publishes, and starting at
    # the function head makes the order check vacuous - how v1 of M78 survived.
    _mt = _probe_body.find("sdmmc_card_t *owner = card;")
    _mounted = _probe_body[_mt:] if _mt >= 0 else ""
    if not _mounted:
        bad("B17 cannot find the mounted branch of probe_task()")
    pub = _mounted.find("publish_info(&info)")
    scan = _mounted.find("probe_fill_space_deferred(")
    if pub < 0 or scan < 0:
        bad("B17 probe_task() no longer publishes first and scans afterwards")
    elif pub > scan:
        bad("B17 probe_task() scans the FAT BEFORE publishing: the mount answer is back "
            "behind the 3.5 s scan")
if "space pending" not in sd_c:
    bad("B17 the mount line no longer says 'space pending': a reader cannot tell a deferred "
        "count from a failed one")
if "PICO_BOOTBB_NOTE_SD_SPACE" not in sd_c:
    bad("B17 the deferred scan leaves no blackbox note: the 3.5 s would be invisible")
_refresh = func_body(sd_c, "esp_err_t read_pico_sd_refresh_space(void)")
if not _refresh:
    bad("B17 cannot find read_pico_sd_refresh_space()")
else:
    # 26.9.22 S23(b)：pending 分支从「一行 return」变成一个块（先置催扫位）。
    # 判据不变且更硬：**这一块里不许出现 fill_space** —— 一旦出现，3.5 s 就被搬到了
    # 调用者（界面）线程上，这正是 S9(b) 当初要消灭的东西。
    # 26.9.22 S23(b): the pending branch grew into a block (it raises a request bit first).
    # The rule is unchanged and stricter: no fill_space() inside that block, or the scan
    # lands on the caller's (UI) thread - exactly what S9(b) removed.
    _pm = re.search(r"space_error\s*==\s*ESP_ERR_NOT_FINISHED\)\s*\{", _refresh)
    if not _pm:
        bad("B17 read_pico_sd_refresh_space() lost its 'space pending' branch: a pending mount "
            "would be rescanned on the caller's thread")
    else:
        _pb = _refresh[_pm.start():]
        _depth, _end = 0, len(_pb)
        for _k, _ch in enumerate(_pb):
            if _ch == "{":
                _depth += 1
            elif _ch == "}":
                _depth -= 1
                if _depth == 0:
                    _end = _k
                    break
        _pend = _pb[:_end]
        if "fill_space" in _pend:
            bad("B17 the pending branch of refresh_space() scans on the caller's thread: that "
                "moves the FAT scan into the UI task")
        if "return ESP_ERR_NOT_FINISHED" not in _pend:
            bad("B17 the pending branch of refresh_space() no longer reports NOT_FINISHED")
        # 26.9.22 S23(b)：pending 时唯一该做的事是「催后台」。没有这一句，进文件页也
        # 要等满 20 s 才有数字，用户会以为卡没插好。
        if "cached_info.space_urge = true" not in _pend:
            bad("B17 the pending branch never raises the scan request: opening the file page "
                "would still wait out the full idle window")
app_files = read(ROOT / "main/apps/app_files.c")
if "正在统计空间" not in app_files:
    bad("B17 the file page still tells the user 'space unavailable, re-enter the page' during "
        "the normal pending window")

# ---- B18：开书五个路标（S10）-------------------------------------------------
_marks = re.findall(r'pico_heap_mark\("([a-z0-9_]+)"\)', app_book)
_want = ["reserve1", "save", "free", "font", "reserve2"]
if _marks[:5] != _want:
    bad("B18 the open-book landmarks are %r, expected %r: without them the permanent -1,024 B "
        "per book open has three indistinguishable suspects" % (_marks[:5], _want))
if "pico_heap_mark" not in read(ROOT / "components/pico_heapdiag/include/pico_heapdiag.h"):
    bad("B18 pico_heap_mark() is not declared")

# ---- B19：8,192 B 块归因（S13）-----------------------------------------------
if "pico_heap_attr" not in time_service:
    bad("B19 nothing calls pico_heap_attr(): the 8,192 B merge killer stays anonymous and "
        "largest can never reach 20,480")
_heap_c = read(ROOT / "components/pico_heapdiag/pico_heapdiag.c")
if "heap_caps_get_allocated_size" not in _heap_c:
    bad("B19 pico_heap_attr() does not reverse-look-up task stacks")
if "CONFIG_FREERTOS_USE_TRACE_FACILITY=y" not in sdkconfig:
    bad("B19 CONFIG_FREERTOS_USE_TRACE_FACILITY is off: uxTaskGetSystemState() would be a "
        "compile-time stub and the attribution impossible")
if "CONFIG_FREERTOS_USE_TRACE_FACILITY=y" not in sdkconfig_defaults:
    bad("B19 sdkconfig.defaults does not enable the trace facility: a fresh clone rebuilds "
        "without the attribution")

# ---- B20：分片扫 FAT，不持卷锁扫完（S14b）-----------------------------------
_scan = func_body(sd_c, "static esp_err_t scan_free_clusters")
if not _scan:
    bad("B20 scan_free_clusters() is gone: the space count is back on f_getfree(), which holds "
        "the volume lock for the whole 8 MB scan")
else:
    for need, why in (
        ("sdmmc_read_sectors", "reads the FAT through the diskio-equivalent call"),
        ("ff_mutex_take", "borrows the volume lock per slice"),
        ("ff_mutex_give", "returns the volume lock per slice"),
        ("taskYIELD", "lets the UI read a directory between slices"),
        ("free_clst =", "writes the count back so later f_getfree() is O(1)"),
    ):
        if need not in _scan:
            bad("B20 scan_free_clusters() does not %s (%s missing): the slice is not the "
                "point, releasing the lock between slices is" % (why, need))
    if "f_getfree" in _scan:
        bad("B20 scan_free_clusters() calls f_getfree(): that is the 2 s lock hold this whole "
            "change exists to remove")

# ---- B21：统计入口先分片、回退留 FatFs（S14b）-------------------------------
_space = func_body(sd_c, "static esp_err_t space_total_free")
if "scan_free_clusters" not in _space:
    bad("B21 space_total_free() does not call scan_free_clusters()")
if "esp_vfs_fat_info" not in _space:
    bad("B21 space_total_free() dropped the FatFs fallback: a non-FAT32 volume would report "
        "no space at all instead of falling back")

# ---- B22：扫表搬出挂载流程，先等空闲再动手（S23b）--------------------------
if "SD_SPACE_DELAY_MS" in sd_c:
    bad("B22 SD_SPACE_DELAY_MS is back: that wait let nobody through - the shelf was queued on "
        "the same volume lock")
_deferred = func_body(sd_c, "static void probe_fill_space_deferred")
if not _deferred:
    bad("B22 cannot find probe_fill_space_deferred()")
else:
    # 26.9.22 S23(b)：这里**允许**出现等待，但只允许一种——挂载后的空闲窗口。
    # 判据有四条，缺一条这刀就退化成「又加了一个白等」：
    #   ① 等待量由 SD_SPACE_IDLE_MS 决定，不是魔法数字；
    #   ② 等待在 fill_space 之前（先等再扫，不是先扫再等）；
    #   ③ 等待循环里不碰卡：没有 fill_space / scan_free_clusters / sdmmc_read_sectors；
    #   ④ 可以被 s_space_urge 提前打断，否则进文件页也得干等 20 s。
    # 26.9.22 S23(b): waiting is allowed here, but only one kind - the post-mount idle
    # window. Four conditions, and dropping any one turns this into another blind wait.
    if "SD_SPACE_IDLE_MS" not in _deferred:
        bad("B22 the deferred scan no longer uses SD_SPACE_IDLE_MS: the idle window is a bare "
            "number nobody can trace back to a decision")
    _wait_at = _deferred.find("vTaskDelay")
    _scan_at = _deferred.find("fill_space(")
    if _wait_at < 0 or _scan_at < 0:
        bad("B22 probe_fill_space_deferred() no longer waits and then scans")
    elif _wait_at > _scan_at:
        bad("B22 probe_fill_space_deferred() scans before it waits: the idle window has to come "
            "first or it protects nothing")
    _loop = _deferred[:_scan_at]
    for _bad_call in ("fill_space", "scan_free_clusters", "sdmmc_read_sectors", "f_opendir"):
        if _bad_call in _loop:
            bad("B22 the idle loop touches the card (%s): the whole point of waiting is that no "
                "lock is taken and no sector is read during it" % _bad_call)
    # 只许扫一次。26.9.22 首轮变异 M86 就钻了这个空子：在等待之后、正式扫描之前又插一次
    # fill_space，顺序检查（等待在前）照样通过，等于「白等 + 扫两遍」。
    # Exactly one scan. M86 v1 in 26.9.22 slipped through by inserting a second fill_space
    # after the wait: the order check still passed, and the wait bought a double scan.
    if _deferred.count("fill_space(") != 1:
        bad("B22 probe_fill_space_deferred() calls fill_space() %d times, expected exactly one: "
            "a second call means the idle window bought nothing" % _deferred.count("fill_space("))
    if "cached_info.space_urge" not in _loop:
        bad("B22 the idle loop cannot be cut short: opening the file page would still sit out "
            "the full 20 s, and the user would read 'counting' as 'card not seated'")
    # 窗口必须由常量**驱动**而不是「写着写着变成 0」：`while (0)` 也满足上面四条，
    # 但它等于把这一刀又收回去，而且门禁看不出来。
    # The window must be driven by the constant, not quietly become zero: `while (0)`
    # satisfies every rule above while silently undoing the change.
    if not re.search(r"while\s*\(\s*idle_ms\s*<\s*SD_SPACE_IDLE_MS\s*\)", _loop):
        bad("B22 the idle loop is no longer bounded by SD_SPACE_IDLE_MS: the mount path may as "
            "well scan immediately again")

# ---- B23：书架只读空间快照（S15）-------------------------------------------
_bs = read(ROOT / "main/book/book_store.c")
_bh = read(ROOT / "main/book/book_store.h")
if "bool book_store_free_snapshot(" not in _bs or "book_store_free_snapshot" not in _bh:
    bad("B23 book_store_free_snapshot() is missing: the shelf falls back to f_getfree() and "
        "holds the volume lock on the UI thread")
_cap = func_body(app_book, "static void capacity_text")
if "book_store_free_snapshot" not in _cap:
    bad("B23 capacity_text() does not use the snapshot")
if "book_store_free_bytes" in _cap:
    bad("B23 capacity_text() still calls book_store_free_bytes(): that is f_getfree() on the "
        "shelf path")
_shelf = func_body(app_book, "static void scan_shelf(app_ctx_t* ctx)")
if "book_store_free_bytes" in _shelf:
    bad("B23 scan_shelf() calls book_store_free_bytes() again: the shelf will queue behind the "
        "probe's FAT scan exactly as in 26.9.20")
if "统计中" not in _cap:
    bad("B23 capacity_text() has no 'counting' state: an unknown count would render as 0.0 MB, "
        "which reads as a full card")

# ---- B24：面板两档，绿线已删（S18）-----------------------------------------
_tr = read(ROOT / "main/apps/app_transfer.c")
if "MEM_LEVEL_GREEN" in _tr:
    bad("B24 MEM_LEVEL_GREEN is back: the 20,480 green line is unreachable on the user path, "
        "so it only manufactures false alarm")
_verdict = func_body(_tr, "static void mem_panel_verdict")
if "blocks_enough" not in _verdict:
    bad("B24 mem_panel_verdict() lost the blocks_enough argument: the two former green "
        "messages have nowhere to go")
if "for (int i = 0; i < 2; ++i)" not in _tr:
    bad("B24 the traffic light no longer draws exactly two dots")

# ---- B25：结论行按块反查（S17）---------------------------------------------
if "biggest task stack" in code_only(_heap_c):
    bad("B25 the verdict line still says 'biggest task stack': it answers a question nobody "
        "asked and sends the reader looking at main instead of the 8,192 B block")
if "block 0x%08x" not in code_only(_heap_c):
    bad("B25 pico_heap_attr() no longer prints the per-block reverse lookup")
if "PICO_HEAP_ATTR_MIN_BLOCK" not in code_only(_heap_c):
    bad("B25 PICO_HEAP_ATTR_MIN_BLOCK is gone: every 1 KB task stack would flood the log")

# ---- B26：events 行的 stored00（S16）---------------------------------------
if "stored%s%u%s%u" in code_only(bootbb_c):
    bad("B26 the events format string is back to the two unconditional %%u: a clean run ends "
        "every line with 'stored00', which reads like a counter")
if "stored, dropped %u, evicted %u" not in code_only(bootbb_c):
    bad("B26 the branch that prints dropped/evicted counts is missing")


# ---- B27：蓝牙闸门收窄到自锁态（裁定③，绝不删）---------------------------
_tr_code = code_only(_tr)
if "!ble_pt_running() && ble_pt_locked()" not in _tr_code:
    bad("B27 the push precheck is not narrowed to ble_pt_locked(): ruling 3 keeps the gate "
        "for the locked (already-failed) case only")
if "push gate released: stack down, not locked" not in _tr_code:
    bad("B27 there is no distinct line for 'stack down, not locked': on the wire it would be "
        "indistinguishable from 'stack already up', which reads as another silent bypass")
_mem_gate = read(ROOT / "components/ble_page_turner/src/ble_pt_mem.c")
if "BLE_PT_MIN_LARGEST_INTERNAL" not in _mem_gate or "free_psram >= BLE_PT_MIN_FREE_PSRAM" not in _mem_gate:
    bad("B27 ble_pt_mem_gate() lost a term: ruling 3 says narrow the trigger, never delete "
        "the gate - it is still the backstop inside start_locked")


# ---- B28：扫描缓冲必须住在栈帧里，.bss 与堆双双零痕迹（S24）--------------------
# 26.9.22 的 2 KB .bss 用真机证明了一件事：.bss 每大 1 B，堆基址挪 1 B，装箱重排——
# largest 18,432 → 12,288，红灯全程常亮，map 对账唯一净增就是那 2,057 B。
# 26.9.22 proved on hardware what one .bss byte costs: the heap base moves, packing
# reshuffles, largest 18,432 -> 12,288, and the map shows those bytes were the only growth.
_mb = re.search(r"#define\s+SD_FAT_SLICE_BYTES\s+\(SD_FAT_SLICE_SECTORS\s*\*\s*512\)", sd_c)
_ms = re.search(r"#define\s+SD_FAT_SLICE_SECTORS\s+(\d+)", sd_c)
if not _mb or not _ms:
    bad("B28 SD_FAT_SLICE_BYTES / SD_FAT_SLICE_SECTORS are gone or redefined: the slice size "
        "is now an untraceable number")
elif int(_ms.group(1)) * 512 != 512:
    bad("B28 the FAT slice is %d B, not 512: a bigger slice needs static or heap storage and "
        "both reshuffle the heap layout that S24 exists to restore" % (int(_ms.group(1)) * 512))
if re.search(r"static\s+uint8_t\s+s_fat_slice_buf\s*\[", sd_c):
    bad("B28 the .bss scan buffer is back: 2,057 static bytes shifted the heap base and cost "
        "6 KB of the largest block on real hardware (26.9.22 field log)")
_scan_body = func_body(sd_c, "static esp_err_t scan_free_clusters")
if "stack_buf" not in _scan_body or "aligned(4)" not in _scan_body:
    bad("B28 scan_free_clusters() does not read into a 4-byte-aligned stack buffer: DMA needs "
        "the alignment and the layout needs the buffer to leave no trace anywhere")
for _alloc in ("heap_caps_malloc", "heap_caps_free"):
    if _alloc in _scan_body:
        bad("B28 scan_free_clusters() still calls %s(): that is the per-insertion split that "
            "cost 4 KB of the largest block and never came back" % _alloc)
if "static, slice" in code_only(sd_c):
    bad("B28 the fat scan line still calls the buffer static: it is a stack buffer now")

# ---- B30：栈余量必须打印（S24 生死状的另一半）--------------------------------
# 512 B 落在 sd_probe（4,096）上，安不安全不靠猜；余量 <256 视为方案自败（审计批复原话）。
# 512 B lands on sd_probe (4,096); safety is printed, not guessed - below 256 the plan
# fails by its own terms (audit's exact words).
if "probe hwm=" not in code_only(sd_c):
    bad("B30 the space scan line does not print 'probe hwm=': the stack margin is the second "
        "half of the S24 life-or-death clause and must be visible on every scan")
if "uxTaskGetStackHighWaterMark" not in sd_c:
    bad("B30 the high-water mark is never sampled: a printed guess is not a measurement")
for _regress in ("s_mount_done_us", "s_space_urge", "s_sd_flags"):
    if _regress in code_only(sd_c):
        bad("B30 %s is back: a new .bss symbol changes the linker packing even when the byte "
            "total is unchanged, and the life-or-death clause requires the heap base to be "
            "byte-identical to 26.9.21" % _regress)
import pathlib as _pl
_h = _pl.Path("components/read_pico/include/read_pico_sd.h").read_text(encoding="utf-8")
if "bool space_urge;" not in _h:
    bad("B30 the urge bit left cached_info: a separate .bss symbol would reflow the linker "
        "packing and void the construction guarantee")

# ---- B29：扫描自证 largest 前后相等（S23a）----------------------------------
# 归因从「唯一变量」升到「打印了数字」：只有 before 与 after 相等，才能说吃大块的不是
# 这块缓冲。不等就是归因错了，必须回来重查——所以两行必须来自**两个**采样点。
# The attribution goes from "only variable changed" to "printed numbers": equal before and
# after is the only proof the buffer stayed out of the heap. Two samples, not one value
# printed twice.
if "fat scan: buf %u B stack" not in code_only(sd_c):
    bad("B29 the 'fat scan: buf ... stack' line is gone: the next person cannot tell whether "
        "the scan still touches the heap, nor that the buffer leaves no static trace")
if "largest before=%u after=%u" not in code_only(sd_c):
    bad("B29 the scan line does not print largest before/after: the whole S23(a) attribution "
        "is back to 'we changed one thing and it got better'")
if code_only(sd_c).count("largest_internal_block()") < 2:
    bad("B29 largest is sampled only once: before and after must be two separate reads, or the "
        "line proves nothing")
_scan_code = code_only(_scan_body)
_b_at, _a_at = _scan_code.find("largest_before"), _scan_code.find("largest_after")
_y_at = _scan_code.find("taskYIELD")
if not (0 <= _b_at < _y_at < _a_at):
    bad("B29 the two largest samples are not one before and one after the scan loop: sampling "
        "both on the same side of the loop would always report 'equal'")


# ---- B44：DMA 子池必须被观测，且观测必须有出口（26.9.31 裁决 1）------------
# 0x101 的教训：bootbb 一直只采 MALLOC_CAP_INTERNAL，而 SD driver 的扇区 DMA 缓冲区
# 来自 MALLOC_CAP_DMA——「总堆还剩 11 KB」说明不了子池还剩几块。26.9.31 真机上
# `allocate_dma_buf: not enough mem (0x101)` 34 次、其中 9 次在 20 s 采样窗外。
# 两条一起检查：只采不打等于 26.9.18 的病重演（采到了，没出口）。
_bb_full = read(BOOTBB_C)
if "MALLOC_CAP_DMA" not in func_body(_bb_full, "static void bb_sample(void)"):
    bad("B44 bb_sample() no longer samples MALLOC_CAP_DMA: the driver takes its sector "
        "buffer from that sub-pool, and the internal-heap total cannot answer for it")
if "dma floor" not in code_only(_bb_full):
    bad("B44 the DMA floor is sampled but never printed: a measurement with no outlet is "
        "exactly the 26.9.18 failure mode")

# ---- B45：sd-dma 只能派生，不许外部直接写（26.9.31）------------------------
# 派生事件只在「创新低 ≥1 KB」时记，健康会话零条。没有阈值就会被每个 SD 事件写一条，
# 64 条环在一天的健康会话里就满了——那等于把「观测」变成「污染」。
if code_only(_bb_full).count("PICO_BOOTBB_NOTE_SD_DMA") < 2:
    bad("B45 the derived sd-dma event is gone")
if "1024" not in func_body(_bb_full, "static void bb_dma_note_from"):
    bad("B45 bb_dma_note_from() lost its 1 KB threshold: every SD event would store a level "
        "note, and a healthy session would eat the 64-note ring")
_bb_rel = BOOTBB_C.resolve()
for _p in sorted(list(ROOT.glob("main/**/*.c")) + list(ROOT.glob("components/**/*.c"))):
    if _p.resolve() == _bb_rel:
        continue
    try:
        _src = _p.read_text(encoding="utf-8", errors="replace")
    except OSError:
        continue
    if "PICO_BOOTBB_NOTE_SD_DMA" in code_only(_src):
        bad("B45 %s writes sd-dma directly: it must stay derived, or the ring fills with "
            "levels nobody asked for" % _p.relative_to(ROOT))


# ---- B48：推送首写失败必须有黑匣子码（26.9.30 commit 5，v36 §四的悬空项）----
# 与 EXCERPT_FAIL 的区别是这条牙存在的理由：那条记「已经丢掉的数据」，这条记
# 「第一次没写进去」——后者多数被重试或欠账救回来了，但它是卡在变坏的早期信号。
# 串口只能证明「这一篇推完了」，救回来 20 次的那张卡要能被下一次开机看见。
_push_part_c = ROOT / "components/push_sink/src/push_part.c"
if not _push_part_c.exists():
    bad("B48 %s is missing: the push write path cannot be judged" % _push_part_c)
else:
    _pp = code_only(read(_push_part_c))
    if "PICO_BOOTBB_NOTE_PUSH_WRITE_FAIL" not in _pp:
        bad("B48 the first-write failure is not noted: a rescued failure leaves no trace in "
            "the black box, so a dying card looks identical to a healthy one at next boot")
    elif "s_write_fails++" not in _pp:
        bad("B48 the note is written without a running count: one line on the serial port is "
            "exactly what a crash between two boots loses")
_bbh = read(BOOTBB_H)
if "PICO_BOOTBB_NOTE_PUSH_WRITE_FAIL" not in _bbh:
    bad("B48 note code 8 is not in the code table: pico_bootbb.h is the single register")
if "PICO_BOOTBB_NOTE_PUSH_WRITE_FAIL" not in func_body(_bb_full, "static bool note_is_critical"):
    bad("B48 push-write-fail is not in the critical list: a full ring would drop the earliest "
        "sign of a dying card first")
if "push-write-fail" not in code_only(_bb_full):
    bad("B48 note_name() has no label for code 8: the black box would print '?'")


if fails:
    print("check_bootbb_ready: %d failure(s)" % len(fails))
    sys.exit(1)

print("check_bootbb_ready: PASS (B6 outlet in main loop, B7 50 ms, B8 per-cut delta, "
      "B9 three stage notes, B10 unconditional probe start, B11 release mapping, "
      "B12 incremental spill, B13 prep stack in PSRAM, B14 512/64 ring + critical notes, "
      "B15 relative at=, B16 entry vs FAIL, B17 two-step mount publish, B18 open marks, "
      "B19 8,192 B attribution, B20 sliced FAT scan, B21 FatFs fallback, "
      "B22 idle window before the scan, "
      "B23 shelf reads the snapshot, B24 two-level panel, B25 per-block verdict, "
      "B26 events line without stored00, B27 push gate narrowed to the locked case, "
      "B28 stack-only 512 B slice, B29 largest before/after proof, B30 probe hwm printed, "
      "B44 DMA sub-pool sampled AND spilled, B45 sd-dma derived only with a 1 KB threshold, "
      "B48 push write failures noted with a running count)")
