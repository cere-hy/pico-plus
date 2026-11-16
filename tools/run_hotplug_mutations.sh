#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# 中文：26.9.17 热插拔修复的变异验证。逐个把「卸载调用 / 探测条件 / 假成功护栏 /
# 日志字段 / 语句顺序 / 测试语义」六道护栏拆掉，确认门禁与宿主场景擒得住。
# English: mutation sweep for the 26.9.17 hot-plug fix. Each mutation removes one guard
# (unmount call, probe condition, stale-card guard, log field, statement order, test
# semantics) and every one must be caught.
#
# 靶点漂移保护：锚点必须**存在且唯一**，否则直接判失败——重构后「变异根本没植入、
# 检查照过」会伪装成 SURVIVED，那种假绿灯比红灯危险。
# Target-drift guard: every anchor must exist AND be unique, otherwise the sweep fails.
set -uo pipefail
cd "$(dirname "$0")/.."

if [ -n "$(git status --porcelain 2>/dev/null)" ]; then
    echo "working tree is dirty; commit first so mutations cannot leak into the baseline"
    exit 1
fi

APP_LOOP=main/app/app_loop.c
SD_SRC=components/read_pico/read_pico_sd.c
HOST_T=tools/app_loop_host_test.c

BACKUPS=()
for f in "$APP_LOOP" "$SD_SRC" "$HOST_T"; do
    b=$(mktemp)
    cp "$f" "$b"
    BACKUPS+=("$b")
done
restore() {
    local i=0
    for f in "$APP_LOOP" "$SD_SRC" "$HOST_T"; do
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

run_gate() {
    local name="$1" desc="$2"
    if python3 tools/check_hotplug.py >/dev/null 2>&1; then
        echo "SURVIVED: $name $desc"
        FAILED=1
    else
        echo "caught: $name $desc"
        CAUGHT=$((CAUGHT + 1))
    fi
    restore
    BACKUPS=()
    for f in "$APP_LOOP" "$SD_SRC" "$HOST_T"; do
        b=$(mktemp); cp "$f" "$b"; BACKUPS+=("$b")
    done
}

run_host() {
    local name="$1" desc="$2"
    if sh tools/run_app_loop_host_tests.sh >/dev/null 2>&1; then
        echo "SURVIVED: $name $desc"
        FAILED=1
    else
        echo "caught: $name $desc"
        CAUGHT=$((CAUGHT + 1))
    fi
    restore
    BACKUPS=()
    for f in "$APP_LOOP" "$SD_SRC" "$HOST_T"; do
        b=$(mktemp); cp "$f" "$b"; BACKUPS+=("$b")
    done
}

# 基线必须绿。基线红说明门禁本身坏了，先修门禁再谈变异。
if ! python3 tools/check_hotplug.py >/dev/null 2>&1; then
    echo "BASELINE RED: check_hotplug.py fails before any mutation"
    exit 1
fi

# ---- M52：拔卡不再卸载（26.9.16 原样）---------------------------------------
# 26.9.18 N2 在 remount 与 if 之间插了一行 `s_release_known = true;`，锚点跟着走。
mutate "M52" "$APP_LOOP" \
    '    s_last_release = read_pico_sd_remount();
    s_release_known = true;' \
    '    if (0) s_last_release = read_pico_sd_remount(); /* MUTATION M52: never unmounts */
    s_release_known = true;' || true
if [ "$FAILED" -eq 0 ]; then
    run_gate "M52" "removal no longer unmounts: the FAT volume keeps spinning on a card that left"
fi

# ---- M53：兜底探测加回 !*invalidated（自锁复现）-----------------------------
mutate "M53" "$APP_LOOP" \
    'if (err == ESP_ERR_INVALID_STATE && !*mounted) {' \
    'if (err == ESP_ERR_INVALID_STATE && !*mounted && !*invalidated) { /* MUTATION M53: self-lock */' || true
if [ "$FAILED" -eq 0 ]; then
    run_gate "M53" "reinsertion never probes again: 0 probe start, 0 remount, forever"
fi

# ---- M54：mount_card 改回假成功 --------------------------------------------
mutate "M54" "$SD_SRC" \
    '    if (card != NULL) {
        ESP_LOGE(TAG, "stale card, close before mount");
        return ESP_ERR_INVALID_STATE;
    }' \
    '    if (card != NULL) { return ESP_OK; } /* MUTATION M54: fake success */' || true
if [ "$FAILED" -eq 0 ]; then
    run_gate "M54" "a stale card reports a successful mount while every read fails with 0x107"
fi

# ---- M55：状态行丢掉 release= ----------------------------------------------
mutate "M55" "$APP_LOOP" \
    '"sd: err=%s present=%d mounted=%d invalidated=%d release=%s retries=%u"' \
    '"sd: err=%s present=%d mounted=%d invalidated=%d retries=%u" /* MUTATION M55 */' || true
if [ "$FAILED" -eq 0 ]; then
    run_gate "M55" "the acceptance field release= disappears from the status line"
fi

# ---- M56：先卸载再关句柄（顺序反了）---------------------------------------
mutate "M56" "$APP_LOOP" \
    '    if (current->on_media_lost) current->on_media_lost(ctx);' \
    '    s_last_release = read_pico_sd_remount(); /* MUTATION M56: before the handles close */
    if (current->on_media_lost) current->on_media_lost(ctx);' || true
if [ "$FAILED" -eq 0 ]; then
    run_gate "M56" "the volume is released before pages close their FILE*: dangling handles"
fi

# ---- M57：stub 退回「永不返回 NOT_FOUND」-----------------------------------
mutate "M57" "$HOST_T" \
    '    if (!i->present) return ESP_ERR_NOT_FOUND;' \
    '    if (0) return ESP_ERR_NOT_FOUND; /* MUTATION M57: removal looks like INVALID_STATE */' || true
if [ "$FAILED" -eq 0 ]; then
    run_host "M57" "the host suite stops distinguishing a removal from a fresh insertion"
fi

echo "---- mutation sweep done ----"
echo "-- caught: $CAUGHT / 6 --"
if [ "$FAILED" -ne 0 ]; then
    echo "hotplug mutations: FAILED"
    exit 1
fi
echo "hotplug mutations: all killed"
