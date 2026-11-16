#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# 中文：26.9.14 结构牙的变异验证。逐个把 A1/A2/D1/D3 的护栏拆掉，确认 check_clock.py
# 擒得住（exit 1 = 擒住）。任何存活变异都说明结构牙有洞。
# English: mutation sweep for the 26.9.14 structural teeth. Each mutation removes one
# guard (A1 / A2 / D1 / D3); check_clock.py must catch every one.
#
# 靶点漂移保护：锚点必须**存在且唯一**，否则直接判失败——重构后「变异根本没植入、
# 检查照过」会伪装成 SURVIVED，那种假绿灯比红灯危险。
# Target-drift guard: every anchor must exist AND be unique; otherwise the sweep fails.
# A mutation that never landed but still "passes" is a fake green, worse than a red.
set -uo pipefail
cd "$(dirname "$0")/.."

TIME_C=main/time_service.c
XFER_C=components/read_pico_transfer/read_pico_transfer.c
BOOK_C=main/apps/app_book.c
KCFG=main/Kconfig.projbuild

BACKUPS=()
for f in "$TIME_C" "$XFER_C" "$BOOK_C" "$KCFG"; do
    b=$(mktemp)
    cp "$f" "$b"
    BACKUPS+=("$b")
done
restore() {
    local i=0
    for f in "$TIME_C" "$XFER_C" "$BOOK_C" "$KCFG"; do
        cp "${BACKUPS[$i]}" "$f"
        rm -f "${BACKUPS[$i]}"
        i=$((i + 1))
    done
}
trap restore EXIT

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
    if python3 tools/check_clock.py >/dev/null 2>&1; then
        echo "SURVIVED: $name $desc"
        FAILED=1
    else
        echo "caught: $name $desc"
        CAUGHT=$((CAUGHT + 1))
    fi
    restore
    BACKUPS=()
    for f in "$TIME_C" "$XFER_C" "$BOOK_C" "$KCFG"; do
        b=$(mktemp); cp "$f" "$b"; BACKUPS+=("$b")
    done
}

# 基线必须绿。基线红说明门禁本身坏了，先修门禁再谈变异。
if ! python3 tools/check_clock.py >/dev/null 2>&1; then
    echo "BASELINE RED: check_clock.py fails before any mutation"
    exit 1
fi

# ---- M31：拆掉 A1 的离线闸（26.9.13 的原样）--------------------------------
mutate "M31" "$TIME_C" \
    '    if (!online) return;
#endif' \
    '#endif' || true
if [ "$FAILED" -eq 0 ]; then
    run_mutation "M31" "offline radio comes back at boot (the 26.9.13 behaviour verbatim)"
fi

# ---- M32：把 6 s 等待改回 15 s ---------------------------------------------
mutate "M32" "$XFER_C" \
    'esp_timer_get_time() + 6000000;' \
    'esp_timer_get_time() + 15000000;' || true
if [ "$FAILED" -eq 0 ]; then
    run_mutation "M32" "temporary radio wait back to 15 s (15,104 ms measured on hardware)"
fi

# ---- M33：Kconfig 默认翻成开 -------------------------------------------------
mutate "M33" "$KCFG" \
    '    default n
    help
        允许「空闲对时」' \
    '    default y
    help
        允许「空闲对时」' || true
if [ "$FAILED" -eq 0 ]; then
    run_mutation "M33" "switch defaults to y, so every build ships the fragmenter"
fi

# ---- M34：删掉 boot-post 采样 -------------------------------------------------
mutate "M34" "$TIME_C" \
    '    pico_heap_dump("boot-post", "time_service:boot_probe");' \
    '    /* MUTATION M34: boot-post removed */' || true
if [ "$FAILED" -eq 0 ]; then
    run_mutation "M34" "boot-post sample removed, so 'nothing moved' can never be proven"
fi

# ---- M35：删掉单击 miss 日志 --------------------------------------------------
mutate "M35" "$BOOK_C" \
    '        ESP_LOGI(TAG, "boxsel: tap (%d,%d) page=%u -> miss", (int)ev->x0, (int)ev->y0,
                 (unsigned)s_page);' \
    '        /* MUTATION M35: miss not logged */' || true
if [ "$FAILED" -eq 0 ]; then
    run_mutation "M35" "tap miss goes silent again, so a swallow looks like 'user left'"
fi

# ---- M36：采样点挪回 teardown 之前 -------------------------------------------
mutate "M36" "$TIME_C" \
    '#define CLOCK_BOOT_PROBE_US (40LL * 1000000)' \
    '#define CLOCK_BOOT_PROBE_US (20LL * 1000000)' || true
if [ "$FAILED" -eq 0 ]; then
    run_mutation "M36" "boot-post at 20 s samples mid-radio instead of after teardown"
fi

echo "---- mutation sweep done ----"
echo "-- caught: $CAUGHT / 6 --"
if [ "$FAILED" -ne 0 ]; then
    echo "clock mutations: FAILED"
    exit 1
fi
echo "clock mutations: all killed"
