#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# 中文：26.9.16 结构牙的变异验证。逐个把「启动顺序 / 开关 / 探测条件 / 日志」四条护栏
# 拆掉，确认 check_boot_order.py 擒得住（exit 1 = 擒住）。任何存活变异都说明结构牙有洞。
# English: mutation sweep for the 26.9.16 structural teeth. Each mutation removes one guard
# (order / switch / probe condition / logs) and check_boot_order.py must catch every one.
#
# 靶点漂移保护：锚点必须**存在且唯一**，否则直接判失败——重构后「变异根本没植入、
# 检查照过」会伪装成 SURVIVED，那种假绿灯比红灯危险。
# Target-drift guard: every anchor must exist AND be unique; otherwise the sweep fails.
# A mutation that never landed but still "passes" is a fake green, worse than a red.
set -uo pipefail
cd "$(dirname "$0")/.."

APP_MAIN=main/app_main.c
APP_LOOP=main/app/app_loop.c
KCFG=main/Kconfig.projbuild
BB_C=components/pico_heapdiag/pico_bootbb.c
BB_H=components/pico_heapdiag/include/pico_bootbb.h
SD_C=components/read_pico/read_pico_sd.c
APP_BOOK=main/apps/app_book.c
TIME_SERVICE=main/time_service.c
SDKCONFIG=sdkconfig
# 26.9.21 起 B23/B24 要动书架容量、面板判级与书目录查询，备份面再扩三处。
# 26.9.21 widens the set for B23/B24: shelf capacity, panel levels, book-store query.
APP_TRANSFER=main/apps/app_transfer.c
BOOK_STORE_C=main/book/book_store.c
BOOK_STORE_H=main/book/book_store.h
HEAP_C=components/pico_heapdiag/pico_heapdiag.c

# 26.9.18 起 B6–B11 要动黑匣子与 SD 组件，备份面随之扩大。
# 26.9.19 起 B13（S8）要动 app_book.c 的任务创建/删除，一并备份。
# 26.9.18 widened the backup set for B6-B11; 26.9.19 adds app_book.c for B13 (S8).
# 26.9.20 起 B19（S13）要动 time_service.c 与 sdkconfig，一并备份。
# 26.9.20 widens the set for B19 (S13): time_service.c and sdkconfig.
FILES=("$APP_MAIN" "$APP_LOOP" "$KCFG" "$BB_C" "$BB_H" "$SD_C" "$APP_BOOK" "$TIME_SERVICE" "$SDKCONFIG"
       "$APP_TRANSFER" "$BOOK_STORE_C" "$BOOK_STORE_H" "$HEAP_C")

BACKUPS=()
for f in "${FILES[@]}"; do
    b=$(mktemp)
    cp "$f" "$b"
    BACKUPS+=("$b")
done
restore() {
    local i=0
    for f in "${FILES[@]}"; do
        cp "${BACKUPS[$i]}" "$f"
        rm -f "${BACKUPS[$i]}"
        i=$((i + 1))
    done
}
trap restore EXIT

FAILED=0
CAUGHT=0

MUTATED=0
mutate() {
    local name="$1" file="$2" old="$3" new="$4" rc=0
    MUTATED=0
    OLD="$old" NEW="$new" FILE="$file" python3 - <<'EOF' || rc=$?
import os, pathlib
p = pathlib.Path(os.environ['FILE'])
s = p.read_text(encoding='utf-8')
old, new = os.environ['OLD'], os.environ['NEW']
n = s.count(old)
if n == 0:
    raise SystemExit(3)
if n > 1:
    raise SystemExit(4)
p.write_text(s.replace(old, new, 1), encoding='utf-8')
EOF
    if [ "$rc" -eq 3 ]; then
        echo "TARGET MISSING: $name"
        FAILED=1
        return 1
    elif [ "$rc" -eq 4 ]; then
        echo "TARGET AMBIGUOUS: $name"
        FAILED=1
        return 1
    elif [ "$rc" -ne 0 ]; then
        echo "MUTATE ERROR: $name ($rc)"
        FAILED=1
        return 1
    fi
    MUTATED=1
    return 0
}

run_mutation() {
    local name="$1" desc="$2" checker="${3:-tools/check_boot_order.py}"
    if python3 "$checker" >/dev/null 2>&1; then
        echo "SURVIVED: $name $desc"
        FAILED=1
    else
        echo "caught: $name $desc"
        CAUGHT=$((CAUGHT + 1))
    fi
    restore
    BACKUPS=()
    for f in "${FILES[@]}"; do
        b=$(mktemp); cp "$f" "$b"; BACKUPS+=("$b")
    done
}

# 基线必须绿。基线红说明门禁本身坏了，先修门禁再谈变异。
if ! python3 tools/check_boot_order.py >/dev/null 2>&1; then
    echo "BASELINE RED: check_boot_order.py fails before any mutation"
    exit 1
fi

# ---- M46：黑匣子挪回 PHY 交还之前（26.9.15 原样）----------------------------
mutate "M46" "$APP_MAIN" \
    '    usb_storage_phy_init();
    REG_CLR_BIT(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);' \
    '    pico_bootbb_start(); /* MUTATION M46: back before the PHY hand-back */
    usb_storage_phy_init();
    REG_CLR_BIT(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M46" "blackbox runs before the PHY hand-back (the 26.9.15 behaviour verbatim)"
fi

# ---- M47：拆掉开关守卫 --------------------------------------------------------
mutate "M47" "$APP_MAIN" \
    '#if CONFIG_PICO_BOOTBB
    pico_bootbb_start();
#endif' \
    '    pico_bootbb_start(); /* MUTATION M47: unguarded */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M47" "call is no longer behind CONFIG_PICO_BOOTBB, so every build arms it"
fi

# ---- M48：探测条件加回 info.present（26.9.15 原样）--------------------------
# 26.9.17 T2 已把 `!*invalidated` 拆掉，锚点跟着改成新条件。
mutate "M48" "$APP_LOOP" \
    'if (err == ESP_ERR_INVALID_STATE && !*mounted) {' \
    'if (err == ESP_ERR_INVALID_STATE && info.present && !*mounted) { /* MUTATION M48 */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M48" "probe gated on info.present again: never starts, never logs (26.9.15)"
fi

# ---- M49：状态变化日志删掉 ----------------------------------------------------
# 26.9.17 T4 把状态行扩成五元组，锚点跟着走。
mutate "M49" "$APP_LOOP" \
    '        ESP_LOGW(TAG, "sd: err=%s present=%d mounted=%d invalidated=%d release=%s retries=%u",' \
    '        if (0) ESP_LOGW(TAG, "sd: err=%s present=%d mounted=%d invalidated=%d release=%s retries=%u", /* MUTATION M49 */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M49" "state changes go silent again: an empty shelf with no explanation"
fi

# ---- M50：自愈日志删掉 --------------------------------------------------------
mutate "M50" "$APP_LOOP" \
    '            ESP_LOGW(TAG, "sd: remount %u/%u after err=%s", (unsigned)s_retries,' \
    '            if (0) ESP_LOGW(TAG, "sd: remount %u/%u after err=%s", (unsigned)s_retries, /* MUTATION M50 */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M50" "the retry goes silent, so a self-heal and a never-tried look identical"
fi

# ---- M51：Kconfig 默认翻成关（26.9.18 反转）-----------------------------------
# 26.9.16/26.9.17 的 M51 是「翻成 y」。26.9.18 起正确状态就是 y（审计批准重开做归因），
# 所以变异方向反过来：翻成 n，B5 必须红——否则说明「默认开」这件事没有任何牙。
# 26.9.16/26.9.17 flipped it to y. Since 26.9.18 the correct state IS y, so the mutation
# now flips it to n; if B5 stays green the "armed by default" rule has no teeth.
mutate "M51" "$KCFG" \
    '    bool "Arm the boot blackbox (26.9.15 B1, ON by default since 26.9.18)"
    default y' \
    '    bool "Arm the boot blackbox (26.9.15 B1, ON by default since 26.9.18)"
    default n' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M51" "switch defaults to n: the heap attribution cannot run"
fi

# ================== 26.9.18：黑匣子与日志（B6–B11）======================
# 这一组守的是「证据链」，不是「功能」。它们对应的六个缺陷都不会让任何断言变红：
# 黑匣子照样采样、别的日志照样打、探测照样工作——只是证据没了，或者印错了字。
# This group guards the evidence chain, not the feature. None of the six defects turns an
# assertion red: sampling continues, other lines still print, probing still works - only
# the evidence is missing or mislabelled.

# ---- M58：主循环不再调 poll（B6）---------------------------------------------
mutate "M58" "$APP_LOOP" \
    '            pico_bootbb_poll();
            read_pico_transfer_service_poll();' \
    '            read_pico_transfer_service_poll(); /* MUTATION M58: poll deleted */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M58" "main loop never polls the blackbox: nothing ever spills" \
        tools/check_bootbb_ready.py
fi

# ---- M59：采样周期退回 200 ms（B7）-------------------------------------------
mutate "M59" "$BB_H" \
    '#define PICO_BOOTBB_PERIOD_US 50000' \
    '#define PICO_BOOTBB_PERIOD_US 200000 /* MUTATION M59 */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M59" "back to 200 ms: the 1 ms book-open drop becomes unresolvable" \
        tools/check_bootbb_ready.py
fi

# ---- M60：逐段 delta 改名（B8）-----------------------------------------------
mutate "M60" "$BB_C" \
    '"[DELAYED-BOOT] cut %u: largest' \
    '"[DELAYED-BOOT] step %u: largest' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M60" "per-cut lines renamed away: the ledger needs hand arithmetic again" \
        tools/check_bootbb_ready.py
fi

# ---- M61：删掉 stage 2 打点（B9）---------------------------------------------
mutate "M61" "$SD_C" \
    '        pico_bootbb_note(PICO_BOOTBB_NOTE_SD_STAGE, SD_STAGE_ENTER(2), SD_STAGE_MS());' \
    '        /* MUTATION M61: stage 2 note deleted */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M61" "stage 2 gone: the 4 s can no longer be split by the blackbox" \
        tools/check_bootbb_ready.py
fi

# ---- M62：probe start 判据再翻回去（B10）-------------------------------------
mutate "M62" "$APP_LOOP" \
    '            if (perr == ESP_ERR_NOT_FINISHED) {' \
    '            if (perr != ESP_ERR_NOT_FINISHED) { /* MUTATION M62 */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M62" "probe start only reports failures again: a dead line, 26.9.17 style" \
        tools/check_bootbb_ready.py
fi

# ---- M63：release= 退回错误码（B11）------------------------------------------
mutate "M63" "$APP_LOOP" \
    '                 !s_release_known
                     ? "-"
                     : ((s_last_release == ESP_OK || s_last_release == ESP_ERR_NOT_FOUND)
                            ? "ok" : "fail"),' \
    '                 esp_err_to_name(s_last_release), /* MUTATION M63 */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M63" "release= prints the raw code again: a success reads like a failure" \
        tools/check_bootbb_ready.py
fi

# ---- M64：poll 重新挂上 idle 门控（B6 第二条分支）----------------------------
# 13.8 s 的真因不是 `is_connected()` 迟钝，而是出口落在「10 s 无输入」后面。
# 这条变异把那道门原样加回来：黑匣子照样工作，只是用户一直摸屏时它就不吐。
# The 13.8 s was never a slow `is_connected()`; the outlet sat behind "10 s without
# input". This mutation puts that gate back: the blackbox still works, it just starves
# whenever the user keeps touching the screen.
mutate "M64" "$APP_LOOP" \
    '            pico_bootbb_poll();' \
    '            if (!touch.touched && ctx.now_ms - last_input_ms >= 10000) pico_bootbb_poll(); /* MUTATION M64 */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M64" "poll gated on touch/input again: a touching user starves the spill" \
        tools/check_bootbb_ready.py
fi

# ================== 26.9.19：增量打印与移栈（B12–B13）======================
# 这一组守的是「下一版别再瞎」：三个缺陷（一生只打一次 / 事件没有出口 / 停采无法
# 定因）都不会让任何断言变红，只会让下一次真机日志再一次没有证据。
# This group keeps the next version from being blind again. None of the three defects
# turns an assertion red; they only make the next field log evidence-free.

# ---- M65：事件游标退回「从 0 开始」（B12 / D2）-------------------------------
mutate "M65" "$BB_C" \
    'const uint32_t from_n = full ? 0u : s_notes_printed;' \
    'const uint32_t from_n = full ? 0u : 0u; /* MUTATION M65 */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M65" "notes are re-scanned from 0 every spill: the cursor is gone" \
        tools/check_bootbb_ready.py
fi

# ---- M66：spill 诊断行删掉（B12 / D3）---------------------------------------
mutate "M66" "$BB_C" \
    '"[DELAYED-BOOT] spill: %u/%u points stored, %u printed, notes %u/%u, "' \
    '"[DELAYED-BOOT] spill: done"' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M66" "the spill line loses its counters: 'full' and 'timer died' merge" \
        tools/check_bootbb_ready.py
fi

# ---- M67：心跳删掉（B12 / D3）-----------------------------------------------
mutate "M67" "$BB_C" \
    '                 "[DELAYED-BOOT] hb: t=%u ms, sampled %u, stored %u/%u%s, notes %u/%u, "' \
    '                 "tick "' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M67" "no heartbeat: 140 s of silence has three indistinguishable causes" \
        tools/check_bootbb_ready.py
fi

# ---- M68：poll 判据退回只看点（B12 / D2）-------------------------------------
mutate "M68" "$BB_C" \
    'const bool fresh = (s_printed < s_n) || (s_notes_printed < s_notes);' \
    'const bool fresh = (s_printed < s_n); /* MUTATION M68 */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M68" "poll watches points only: a note written in a quiet second never ships" \
        tools/check_bootbb_ready.py
fi

# ---- M69：汇总块重新门控到第一次（B12 / D1）---------------------------------
mutate "M69" "$BB_C" \
    '    if (from_p < s_n) {' \
    '    if (from_p < s_n && from_p == 0) { /* MUTATION M69 */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M69" "summary block is back behind 'first spill only' - D1 verbatim" \
        tools/check_bootbb_ready.py
fi

# ---- M70：Kconfig 默认翻回 n（B13 / S8）-------------------------------------
mutate "M70" "$KCFG" \
    '    bool "Put the book_prep task stack in PSRAM (26.9.13 P0-a, shipped in 26.9.19)"
    default y' \
    '    bool "Put the book_prep task stack in PSRAM (26.9.13 P0-a, shipped in 26.9.19)"
    default n' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M70" "switch defaults back to n: the 6,144 B hole returns to pool A" \
        tools/check_bootbb_ready.py
fi

# ---- M71：创建时不再要 PSRAM（B13 / S8）-------------------------------------
mutate "M71" "$APP_BOOK" \
    '                                       MALLOC_CAP_SPIRAM)!=pdPASS)s_prep_task=NULL;' \
    '                                       MALLOC_CAP_INTERNAL)!=pdPASS)s_prep_task=NULL; /* MUTATION M71 */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M71" "the stack is created with internal caps: S8 silently undone" \
        tools/check_bootbb_ready.py
fi

# ---- M72：自删退回 vTaskDelete(NULL)（B13）----------------------------------
mutate "M72" "$APP_BOOK" \
    '    vTaskDeleteWithCaps(xTaskGetCurrentTaskHandle());' \
    '    vTaskDelete(NULL); /* MUTATION M72 */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M72" "self-delete leaks the 6 K PSRAM stack and its TCB, without a word" \
        tools/check_bootbb_ready.py
fi

# ================== 26.9.20：容量、相对时间、两步发布、路标（B14–B19）==========
# 这一组守的是「下一个读日志的人还看得懂」：512 点的环、相对时间戳、进入/成功分开、
# 挂载先发布后算、开书五个路标、8 KB 块点名。六项里没有一项会让功能坏掉，
# 全部只会让证据再一次变成谜语。
# This group keeps the next reader from being lied to. None of the six breaks a feature;
# each one turns the evidence back into a riddle.

# ---- M73：环退回 128 点（B14）------------------------------------------------
mutate "M73" "$BB_H" \
    '#define PICO_BOOTBB_POINTS 512' \
    '#define PICO_BOOTBB_POINTS 128 /* MUTATION M73 */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M73" "ring back to 128 points: everything after t=172 s is lost again" \
        tools/check_bootbb_ready.py
fi

# ---- M74：关键事件不再优先（B14）--------------------------------------------
mutate "M74" "$BB_C" \
    '    return code == PICO_BOOTBB_NOTE_SD_STAGE || code == PICO_BOOTBB_NOTE_SD_SPACE;' \
    '    return false; /* MUTATION M74 */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M74" "sd-stage can be dropped again once the ring fills" \
        tools/check_bootbb_ready.py
fi

# ---- M75：at= 退回绝对毫秒（B15 / D4）---------------------------------------
mutate "M75" "$SD_C" \
    '    const int64_t dt = esp_timer_get_time() - s_probe_t0;' \
    '    const int64_t dt = esp_timer_get_time(); /* MUTATION M75 */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M75" "absolute milliseconds again: at= wraps into 11,557 at 142.6 s" \
        tools/check_bootbb_ready.py
fi

# ---- M76：stage 2 丢掉进入位（B16 / D5）-------------------------------------
mutate "M76" "$SD_C" \
    '        pico_bootbb_note(PICO_BOOTBB_NOTE_SD_STAGE, SD_STAGE_ENTER(2), SD_STAGE_MS());' \
    '        pico_bootbb_note(PICO_BOOTBB_NOTE_SD_STAGE, 2u, SD_STAGE_MS()); /* MUTATION M76 */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M76" "stage 2 back to 'no result bits': it prints FAIL forever again" \
        tools/check_bootbb_ready.py
fi

# ---- M77：打印端不再认进入位（B16 第二条）-----------------------------------
mutate "M77" "$BB_C" \
    '                const bool entry = (n->a & 0x08u) != 0;' \
    '                const bool entry = false; /* MUTATION M77 */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M77" "the printer ignores the entry bit: 'no result' reads as FAIL again" \
        tools/check_bootbb_ready.py
fi

# ---- M78：先算空间再发布（B17 / S9b）---------------------------------------
# 26.9.22 S23(b) 起 publish 与 deferred 中间多了一行时刻戳，锚点跟着走。
mutate "M78" "$SD_C" \
    '        publish_info(&info);
        // 26.9.23 S24：时刻戳已删——「mount+ 多少 ms」由 bootbb 的 at= 承担。
        // 26.9.23 S24: the timestamp is gone; the bootbb at= carries the figure.
        probe_fill_space_deferred(owner, &info);' \
    '        probe_fill_space_deferred(owner, &info); /* MUTATION M78 */
        publish_info(&info);' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M78" "the 3.5 s FAT scan is back in front of the mount answer" \
        tools/check_bootbb_ready.py
fi

# ---- M79：refresh_space 守卫拆掉（B17）-------------------------------------
# 26.9.22 S23(b)：pending 分支现在是一块（先置催扫位再 return）。变异改成整块拆掉，
# 门禁的「块里不许 fill_space」和「必须 return NOT_FINISHED」两条都会红。
mutate "M79" "$SD_C" \
    '    if (info.space_error == ESP_ERR_NOT_FINISHED) {
        portENTER_CRITICAL(&state_lock);
        cached_info.space_urge = true;
        portEXIT_CRITICAL(&state_lock);
        return ESP_ERR_NOT_FINISHED;
    }' \
    '    /* MUTATION M79: the pending guard is gone */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M79" "the UI task may rescan a pending mount: the 3.5 s moves into the UI" \
        tools/check_bootbb_ready.py
fi

# ---- M80：删掉一个开书路标（B18 / S10）-------------------------------------
mutate "M80" "$APP_BOOK" \
    '    pico_heap_mark("font");' \
    '    /* MUTATION M80: the font landmark is gone */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M80" "one landmark deleted: the -1,024 B falls between two candidates again" \
        tools/check_bootbb_ready.py
fi

# ---- M81：不再点名 8 KB 块（B19 / S13）-------------------------------------
mutate "M81" "$TIME_SERVICE" \
    '    pico_heap_attr("boot-post");' \
    '    /* MUTATION M81: the attribution is gone */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M81" "no task-stack attribution: the 8,192 B merge killer stays anonymous" \
        tools/check_bootbb_ready.py
fi

# ---- M82：关掉任务枚举（B19）-----------------------------------------------
mutate "M82" "$SDKCONFIG" \
    'CONFIG_FREERTOS_USE_TRACE_FACILITY=y' \
    '# CONFIG_FREERTOS_USE_TRACE_FACILITY is not set /* MUTATION M82 */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M82" "trace facility off: uxTaskGetSystemState cannot name anything" \
        tools/check_bootbb_ready.py
fi

# ---- M83：分片扫描不再让 CPU（B20 / S14b）----------------------------------
mutate "M83" "$SD_C" \
    '        taskYIELD();' \
    '        /* MUTATION M83: no yield between slices */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M83" "no yield: the scan re-becomes one uninterruptible 2 s bus hog" \
        tools/check_bootbb_ready.py
fi

# ---- M84：分片扫描改回 f_getfree（B20）-------------------------------------
mutate "M84" "$SD_C" \
    '        const esp_err_t dr = sdmmc_read_sectors(owner_card, buf, (size_t)(fs->fatbase + s), n);' \
    '        const esp_err_t dr = (f_getfree(SD_MOUNT_POINT, (DWORD *)buf, &fs)) == FR_OK ? ESP_OK : ESP_FAIL; /* MUTATION M84 */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M84" "back to f_getfree: the volume lock is held across the whole table again" \
        tools/check_bootbb_ready.py
fi

# ---- M85：丢掉 FatFs 回退（B21）--------------------------------------------
mutate "M85" "$SD_C" \
    '    if (err != ESP_ERR_NOT_SUPPORTED) return err;
    return esp_vfs_fat_info(SD_MOUNT_POINT, total_bytes, free_bytes);' \
    '    return err; /* MUTATION M85: no fallback */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M85" "no FatFs fallback: a non-FAT32 volume reports no space at all" \
        tools/check_bootbb_ready.py
fi

# ---- M86：把 1.2 s 白等加回来（B22 / S14a）---------------------------------
# 26.9.22 S23(b)：这条的靶子换了。空闲窗口本身是合法的，要防的是「等的时候偷偷读卡」——
# 在窗口里塞一次 fill_space，等于把 20 s 白等变成「白等 + 扫描」，两条罪状一起犯。
mutate "M86" "$SD_C" \
    '    if (card != owner) return;
    const int64_t t0 = esp_timer_get_time();' \
    '    (void)fill_space(info); /* MUTATION M86 */
    if (card != owner) return;
    const int64_t t0 = esp_timer_get_time();' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M86" "the idle window reads the card: 20 s of quiet becomes 20 s of quiet plus a scan" \
        tools/check_bootbb_ready.py
fi

# ---- M87：书架容量改回现场扫表（B23 / S15）---------------------------------
mutate "M87" "$APP_BOOK" \
    '    if (!book_store_free_snapshot(root, &free_bytes)) {' \
    '    if (!(free_bytes = book_store_free_bytes(root), true)) { /* MUTATION M87 */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M87" "capacity_text scans on the shelf path: the UI thread queues on the volume lock" \
        tools/check_bootbb_ready.py
fi

# ---- M88：把绿线加回面板（B24 / S18）---------------------------------------
mutate "M88" "$APP_TRANSFER" \
    'typedef enum { MEM_LEVEL_RED, MEM_LEVEL_YELLOW } mem_level_t;' \
    'typedef enum { MEM_LEVEL_RED, MEM_LEVEL_YELLOW, MEM_LEVEL_GREEN } mem_level_t; /* MUTATION M88 */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M88" "green tier restored: the unreachable 20,480 line manufactures false alarm" \
        tools/check_bootbb_ready.py
fi

# ---- M89：结论行改回「最大任务栈」（B25 / S17）-----------------------------
mutate "M89" "$HEAP_C" \
    '        ESP_LOGW(TAG, "attr %s: block 0x%08x %5u B -> %s", site,
                 (unsigned)hits[i].base, hits[i].size, hits[i].name);' \
    '        ESP_LOGW(TAG, "attr %s: biggest task stack = %u", site, hits[i].size); /* MUTATION M89 */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M89" "verdict answers the wrong question again: the 8,192 B block stays anonymous" \
        tools/check_bootbb_ready.py
fi

# ---- M90：events 行改回 stored00（B26 / S16）-------------------------------
mutate "M90" "$BB_C" \
    '"[DELAYED-BOOT] events: +%u this window, %u/%u stored, dropped %u, evicted %u"' \
    '"[DELAYED-BOOT] events: +%u this window, %u/%u stored%s%u%s%u" /* MUTATION M90 */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M90" "the two unconditional %u are back: every clean line ends in stored00" \
        tools/check_bootbb_ready.py
fi

# ---- M91：闸门收窄退回「栈 down 就拦」（B27 / 裁定③）----------------------
mutate "M91" "$APP_TRANSFER" \
    'if (!ble_pt_running() && ble_pt_locked()) {' \
    'if (!ble_pt_running()) { /* MUTATION M91 */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M91" "the precheck blocks every first attempt again: on the user path it is unreachable, and when reached it refuses work start_locked would have allowed" \
        tools/check_bootbb_ready.py
fi

# ---- M92：删掉「栈 down 未锁」那一支的独立打点（B27）-----------------------
mutate "M92" "$APP_TRANSFER" \
    '"push gate released: stack down, not locked - start_locked holds the gate "' \
    '"push gate skipped: stack already up " /* MUTATION M92 */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M92" "the two cases share one line again: a released arm reads as a silent bypass" \
        tools/check_bootbb_ready.py
fi

# ---- M93：扫描缓冲退回内部堆 8 KB（B28 / S23a）-----------------------------
# 26.9.21 的原样：一次插卡切一刀，还回去拼不回来。这一条必须被擒住，否则 S23(a)
# 整刀等于没落。
mutate "M93" "$SD_C" \
    '    BYTE *buf = stack_buf;' \
    '    BYTE *buf = heap_caps_malloc(SD_FAT_SLICE_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA); /* MUTATION M93 */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M93" "the scan buffer is back on the internal heap: every insertion splits 2 KB off and the largest block degrades again" \
        tools/check_bootbb_ready.py
fi

# ---- M94：片子改回 8 KB（B28 尺寸 / S23a）----------------------------------
mutate "M94" "$SD_C" \
    '#define SD_FAT_SLICE_SECTORS 1    // 512 B 一片 / 512 B per slice' \
    '#define SD_FAT_SLICE_SECTORS 4    /* MUTATION M94 */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M94" "the slice is 2 KB again: a bigger slice cannot live in the stack frame and crawls back into static storage" \
        tools/check_bootbb_ready.py
fi

# ---- M95：取证行退回「只报一个数」（B29 / S23a）----------------------------
mutate "M95" "$SD_C" \
    '    ESP_LOGI(TAG, "fat scan: buf %u B stack, slice %u, largest before=%u after=%u",' \
    '    ESP_LOGI(TAG, "fat scan: buf %u B stack, slice %u, largest=%u", /* MUTATION M95 */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M95" "only one largest sample is printed: 'equal before and after' becomes an assumption instead of evidence" \
        tools/check_bootbb_ready.py
fi

# ---- M96：空闲窗口写成 while(0)（B22 / S23b）-------------------------------
mutate "M96" "$SD_C" \
    '    while (idle_ms < SD_SPACE_IDLE_MS) {' \
    '    while (0) { /* MUTATION M96 */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M96" "the idle window is written as while(0): the scan is back on the mount path while every other rule still passes" \
        tools/check_bootbb_ready.py
fi

# ---- M97：删掉「有人催就提前扫」（B22 / S23b）------------------------------
mutate "M97" "$SD_C" \
    '        if (cached_info.space_urge) break;' \
    '        /* MUTATION M97: request bit ignored */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M97" "the idle loop ignores the request bit: opening the file page still waits out 20 s" \
        tools/check_bootbb_ready.py
fi

# ---- M98：不肯置催扫位（B17 / S23b）---------------------------------------
mutate "M98" "$SD_C" \
    '        cached_info.space_urge = true;' \
    '        (void)0; /* MUTATION M98 */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M98" "refresh_space() never raises the request: the pending branch looks correct but shortens nothing" \
        tools/check_bootbb_ready.py
fi

# ---- M99：栈缓冲丢掉 4 字节对齐（B28 / S24）--------------------------------
# 26.9.23 起靶子是栈缓冲：旧 static 行已删，还打它就是 TARGET MISSING 静默跳过——
# M84 的教训：变异必须打在真实代码上，否则计数器会虚高。
mutate "M99" "$SD_C" \
    '    uint8_t stack_buf[SD_FAT_SLICE_BYTES] __attribute__((aligned(4)));' \
    '    uint8_t stack_buf[SD_FAT_SLICE_BYTES]; /* MUTATION M99 */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M99" "the stack buffer lost its 4-byte alignment: SDMMC DMA on an unaligned address is a hang, not a slow path" \
        tools/check_bootbb_ready.py
fi

# ---- M100：slice 变回 16（B28 / S24）---------------------------------------
# 512 B 的栈帧装不下 8 KB：更大的片只能走 static 或 heap，两条路都会重排堆布局。
mutate "M100" "$SD_C" \
    '#define SD_FAT_SLICE_SECTORS 1    // 512 B 一片 / 512 B per slice' \
    '#define SD_FAT_SLICE_SECTORS 16   /* MUTATION M100 */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M100" "the slice is 16 sectors again: 8 KB cannot live in a stack frame, so it crawls back into .bss or the heap and reshuffles the layout" \
        tools/check_bootbb_ready.py
fi

# ---- M101：.bss 缓冲复活（B28 / S24）---------------------------------------
mutate "M101" "$SD_C" \
    '    uint8_t stack_buf[SD_FAT_SLICE_BYTES] __attribute__((aligned(4)));
    BYTE *buf = stack_buf;' \
    '    static uint8_t s_fat_slice_buf[SD_FAT_SLICE_BYTES] __attribute__((aligned(4))); /* MUTATION M101 */
    BYTE *buf = s_fat_slice_buf;' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M101" "the .bss buffer is back: 26.9.22 proved on hardware that static bytes shift the heap base and cost 6 KB of the largest block" \
        tools/check_bootbb_ready.py
fi

# ---- M102：余量不打印（B30 / S24）------------------------------------------
mutate "M102" "$SD_C" \
    'ESP_LOGI(TAG, "space scan: %s in %u ms (idle %u ms), free=%llu MB, probe hwm=%u",' \
    'ESP_LOGI(TAG, "space scan: %s in %u ms (idle %u ms), free=%llu MB", /* MUTATION M102 */' || true
if [ "$MUTATED" -eq 1 ]; then
    run_mutation "M102" "the stack margin is not printed: a below-256 HWM would go unnoticed until the probe task corrupts memory" \
        tools/check_bootbb_ready.py
fi

echo "---- mutation sweep done ----"
echo "-- caught: $CAUGHT / 51 --"
if [ "$FAILED" -ne 0 ]; then
    echo "boot-order mutations: FAILED"
    exit 1
fi
echo "boot-order mutations: all killed"
