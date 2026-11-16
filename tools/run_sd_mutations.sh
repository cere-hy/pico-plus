#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
# 中文：26.9.31a 物理拔卡死锁的变异验证。四个靶点对应四条不变式，逐个拆回
# 「看起来也能跑」的形态，确认每刀都被 S1–S4 或 app_loop 看门狗用例擒住。
# 存活变异 = 牙有洞，必须当场补，不许记账了事。
# English: mutation sweep for the 26.9.31a physical-card-pull deadlock. Four invariants,
# four mutations; each must be caught by S1-S4 or the app_loop watchdog case.
# A survivor is a hole in a tooth and must be fixed on the spot.
#
# 靶点漂移保护：锚点必须存在且唯一，否则判失败——重构后「变异没植进去、检查照过」
# 会伪装成 SURVIVED，那种假绿灯比红灯危险（26.9.23 已栽过一次）。
# Target-drift guard: anchors must exist AND be unique, else the sweep fails outright.
#
# 串行纪律：本脚本与其他变异脚本共用工作区，并发会互相覆盖备份（26.9.24 事故）。
# Serial only: it shares the worktree; concurrent runs clobber each other's backups.
set -uo pipefail
cd "$(dirname "$0")/.."

SD_C=components/read_pico/read_pico_sd.c
LOOP_C=main/app/app_loop.c
FILES=("$SD_C" "$LOOP_C")

BACKUPS=()
snapshot() {
    BACKUPS=()
    for f in "${FILES[@]}"; do
        b=$(mktemp); cp "$f" "$b"; BACKUPS+=("$b")
    done
}
restore() {
    local i=0
    for f in "${FILES[@]}"; do cp "${BACKUPS[$i]}" "$f"; rm -f "${BACKUPS[$i]}"; i=$((i + 1)); done
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
    if [ "$rc" -eq 3 ]; then echo "TARGET MISSING: $name"; FAILED=1; return 1
    elif [ "$rc" -eq 4 ]; then echo "TARGET AMBIGUOUS: $name"; FAILED=1; return 1
    elif [ "$rc" -ne 0 ]; then echo "MUTATE ERROR: $name ($rc)"; FAILED=1; return 1
    fi
    return 0
}

judge() {
    local out rc
    out=$(cc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -fsanitize=address,undefined -g \
          -Itools/sd_probe_stubs -Icomponents/read_pico/include \
          tools/sd_probe_host_test.c -o /tmp/sd_mut 2>&1) || { echo "$out" >&2; return 1; }
    /tmp/sd_mut >/dev/null 2>&1 || return 1
    bash tools/run_app_loop_host_tests.sh >/dev/null 2>&1 || return 1
    python3 tools/check_hotplug.py >/dev/null 2>&1 || return 1
    return 0
}

run_mutation() {
    local name="$1" desc="$2"
    if judge; then
        echo "SURVIVED: $name $desc"; FAILED=1
    else
        echo "caught: $name $desc"; CAUGHT=$((CAUGHT + 1))
    fi
    restore; snapshot
}

if ! judge; then
    echo "BASELINE RED: the SD host tests fail before any mutation"
    exit 1
fi

# ---- M123：锁存判据放回「探测期也算失效」 ---------------------------------
# 26.9.29 的第二项 `|| probe_state == 1`。它让「首发挂载失败 + CD 抖一下」= 永久锁存。
# 26.9.29's extra clause: a bounce inside the probe window counts as a removal.
mutate "M123" "$SD_C" \
  'if (!present && cached_info.mounted) media_invalidated = true;' \
  'if (!present && (cached_info.mounted || probe_state == 1)) media_invalidated = true;' || true
run_mutation "M123" "latch also arms during probing (the 26.9.29 clause)"

# ---- M124：删掉上升沿清锁存 -----------------------------------------------
# 用户插回卡是人的意图；删掉它，插回也会被旧闩否决。
mutate "M124" "$SD_C" \
  'if (inserted && media_invalidated && !cached_info.mounted && card == NULL) {' \
  'if (inserted && media_invalidated && !cached_info.mounted && card == NULL && 0) {' || true
run_mutation "M124" "rising edge no longer clears the latch"

# ---- M125：去抖退化成单采样 -----------------------------------------------
# SD_CD_SAMPLES=1 → 循环一次都不跑，等于 26.9.29 的「读到 absent 就判死」。
mutate "M125" "$SD_C" \
  '#define SD_CD_SAMPLES        3' \
  '#define SD_CD_SAMPLES        1' || true
run_mutation "M125" "CD debounce degraded to a single sample"

# ---- M126：看门狗退回「上限 3 次」 ----------------------------------------
# 三次快速重试之后撒手，之后除了重启再没有人碰这张卡。
mutate "M126" "$LOOP_C" \
  'if (!ready && !lost && info.present && err != ESP_ERR_NOT_FOUND &&
        err != ESP_ERR_NOT_FINISHED) {' \
  'if (!ready && !lost && info.present && err != ESP_ERR_NOT_FOUND &&
        s_retries < SD_PROBE_RETRY_MAX && err != ESP_ERR_NOT_FINISHED) {' || true
run_mutation "M126" "watchdog capped at three tries (the 26.9.29 shape)"

echo "sd mutations: $CAUGHT caught, failed=$FAILED"
[ "$FAILED" -eq 0 ]
