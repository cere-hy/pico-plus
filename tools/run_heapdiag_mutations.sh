#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# 中文：26.9.15 结构牙（check_heapdiag.py）的变异验证。逐个把护栏拆回 26.9.14 的形态或
# 更糟的形态，确认每一刀都被擒住（exit 1 = 擒住）。任何存活变异说明牙有洞。
# English: mutation sweep for the 26.9.15 structural teeth. Each mutation reverts one guard
# to its 26.9.14 shape (or worse) and check_heapdiag.py must catch every one.
#
# 靶点漂移保护：锚点必须**存在且唯一**，否则直接判失败——重构后「变异根本没植入、
# 检查照过」会伪装成 SURVIVED，那种假绿灯比红灯危险。
# Target-drift guard: every anchor must exist AND be unique; otherwise the sweep fails.
set -uo pipefail
cd "$(dirname "$0")/.."

HD_C=components/pico_heapdiag/pico_heapdiag.c
HD_H=components/pico_heapdiag/include/pico_heapdiag.h
BB_C=components/pico_heapdiag/pico_bootbb.c
APP_C=main/app_main.c
TIME_C=main/time_service.c

FILES=("$HD_C" "$HD_H" "$BB_C" "$APP_C" "$TIME_C")

BACKUPS=()
snapshot() {
    BACKUPS=()
    for f in "${FILES[@]}"; do
        b=$(mktemp)
        cp "$f" "$b"
        BACKUPS+=("$b")
    done
}
restore() {
    local i=0
    for f in "${FILES[@]}"; do
        cp "${BACKUPS[$i]}" "$f"
        rm -f "${BACKUPS[$i]}"
        i=$((i + 1))
    done
}
trap restore EXIT
snapshot

FAILED=0
CAUGHT=0

mutate() {
    local name="$1" file="$2" old="$3" new="$4" rc=0
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
    return 0
}

run_mutation() {
    local name="$1" desc="$2"
    # 两颗牙一起看：某些变异（例如删掉 start 调用）只有 check_boot_order 的 B1 管得住。
    # 只看一颗会把「被另一颗擒住」误报成存活。
    # Both teeth: some mutations (e.g. deleting the start call) are only covered by
    # check_boot_order's B1. Checking one would report a kill by the other as a survivor.
    if python3 tools/check_heapdiag.py >/dev/null 2>&1 && \
       python3 tools/check_boot_order.py >/dev/null 2>&1; then
        echo "SURVIVED: $name $desc"
        FAILED=1
    else
        echo "caught: $name $desc"
        CAUGHT=$((CAUGHT + 1))
    fi
    restore
    snapshot
}

if ! python3 tools/check_heapdiag.py >/dev/null 2>&1; then
    echo "BASELINE RED: check_heapdiag.py fails before any mutation"
    exit 1
fi

# ---- M37：槽位退回 8 格（26.9.14 的原样，9 站点必丢一个）----------------------
mutate "M37" "$HD_C" \
    '#define SITE_SLOTS 16' \
    '#define SITE_SLOTS 8' || true
if [ "$FAILED" -eq 0 ]; then
    run_mutation "M37" "site table back to 8 slots while 9 sites exist (26.9.14 verbatim)"
fi

# ---- M38：总预算退回 8 份 ------------------------------------------------------
mutate "M38" "$HD_H" \
    '#define PICO_HEAP_DUMP_TOTAL 24' \
    '#define PICO_HEAP_DUMP_TOTAL 8' || true
if [ "$FAILED" -eq 0 ]; then
    run_mutation "M38" "budget back to 8: two radio toggles exhaust it before push-bypass"
fi

# ---- M39：满表静默（把 DROPPED 那行拿掉）-------------------------------------
mutate "M39" "$HD_C" \
    '    if (table_full) {
        // 满表必须吼出来：这是配置错，不是策略。26.9.14 就是在这里一声不吭地丢了一个站点。
        // A full table must shout: it is a configuration bug, not a policy. 26.9.14 dropped
        // one site here without a word.
        ESP_LOGW(TAG, "DROPPED site=%s: site table full (%u/%u used) - raise SITE_SLOTS",
                 site, used, (unsigned)SITE_SLOTS);
        return;
    }' \
    '    if (table_full) {
        return; /* MUTATION M39: silent drop restored */
    }' || true
if [ "$FAILED" -eq 0 ]; then
    run_mutation "M39" "table-full drop goes silent again (the 26.9.14 defect)"
fi

# ---- M40：黑匣子缓冲改吃内部 RAM ---------------------------------------------
mutate "M40" "$BB_C" \
    's_blob = (bb_blob_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);' \
    's_blob = (bb_blob_t *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);' || true
if [ "$FAILED" -eq 0 ]; then
    run_mutation "M40" "blackbox buffer in internal RAM: measures 21,504 as 20,480"
fi

# ---- M41：拿不到 PSRAM 就降级到内部 RAM（污染数据）---------------------------
mutate "M41" "$BB_C" \
    '    if (s_blob == NULL) {' \
    '    if (s_blob == NULL) {
        s_blob = (bb_blob_t *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL); /* M41 */
    }
    if (s_blob == NULL) {' || true
if [ "$FAILED" -eq 0 ]; then
    run_mutation "M41" "falls back to internal RAM on PSRAM failure (poisoned, not disabled)"
fi

# ---- M42：直接删掉 start 调用（26.9.16 反转了方向）----------------------------
# 26.9.15 的 M42 是「挪到 PHY 之后」，那时候「之后」是错的；26.9.16 的真机结果证明
# 「之后」才对。所以这条变异改成「把能力整个删掉」——`#if` 是预处理，源文件里的调用
# 必须一直在，删掉它就等于把开关改成永远关不回来。
# 26.9.15's M42 moved the call after the PHY swap, which was wrong then and is right now.
# This mutation deletes the call instead: the switch acts at preprocess time, so removing
# the call in source deletes the capability rather than switching it off.
mutate "M42" "$APP_C" \
    '#if CONFIG_PICO_BOOTBB
    pico_bootbb_start();
#endif' \
    '/* MUTATION M42: start call deleted */' || true
if [ "$FAILED" -eq 0 ]; then
    run_mutation "M42" "start call deleted outright: the switch can never turn it back on"
fi

# ---- M43：把 poll 塞回 boot_probe（26.9.18 反转）------------------------------
# 26.9.15/26.9.16 的 M43 是「挪到 early return 之后」。26.9.17 真机证明整个位置都是
# 错的：boot_probe 被「10 s 无输入」门控，用户一直摸屏就 13.8 s 都不吐。出口已挪到
# 主循环，所以现在的变异方向反过来——把它塞回去，H7 必须当场红。
# 26.9.15/26.9.16 moved the poll after an early return. The 26.9.17 field log proved the
# whole position was wrong (boot_probe is gated on 10 s without input). The outlet moved to
# the main loop, so the mutation is now the reverse: put it back and H7 must go red.
mutate "M43" "$TIME_C" \
    'static void boot_probe(int64_t monotonic_us) {' \
    'static void boot_probe(int64_t monotonic_us) {
    pico_bootbb_poll(); /* MUTATION M43: back behind the idle gate */' || true
if [ "$FAILED" -eq 0 ]; then
    run_mutation "M43" "poll back inside boot_probe: gated on 10 s without input (the 13.8 s of 26.9.15)"
fi

# ---- M44：不判连接就打印 ------------------------------------------------------
mutate "M44" "$BB_C" \
    '    if (!usb_serial_jtag_is_connected()) {' \
    '    if (false && !usb_serial_jtag_is_connected()) { /* MUTATION M44 */' || true
if [ "$FAILED" -eq 0 ]; then
    run_mutation "M44" "spills without checking the host: VFS returns EIO, lost silently"
fi

# ---- M45：删掉 delta 行 -------------------------------------------------------
# 26.9.19 S5 把这一行改成增量语义（"through t=%u ms"），锚点跟着走。
mutate "M45" "$BB_C" \
    '"[DELAYED-BOOT] delta: none - largest held at %u through t=%u ms"' \
    '"[DELAYED-BOOT] flat: %u %u"' || true
if [ "$FAILED" -eq 0 ]; then
    run_mutation "M45" "delta wording removed: the reader must diff the curve by eye again"
fi

echo "---- mutation sweep done ----"
echo "-- caught: $CAUGHT / 9 --"
if [ "$FAILED" -ne 0 ]; then
    echo "heapdiag mutations: FAILED"
    exit 1
fi
echo "heapdiag mutations: all killed"
