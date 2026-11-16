#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
#
# 31b 收官批（P1-1 掉盘第二信源 + B42 静态牙）的最小先红证据（2026-11-01）。
#
# 六条，每条打掉一处关键语义，要求「退出非 0 且红在那条断言上」：
#   P1a  删掉墙钟判据 → 只有「8 次」时，最坏路径 16 s 判不出，判据失效
#   P1b  次数 8 → 9   → 快路径晚一拍
#   P1c  成功不清零     → 两次无关的坏卡被拼成一次判死
#   P1d  所有错误码都计数 → 一次内存紧张把好卡判死（卷被无谓卸载）
#   B42a 塞回 quiesce(0) → 假收口回潮
#   B42c 删掉 discard     → 插拔边沿不再作废旧封面
#
# 用法：bash tools/run_p11_first_red.sh   （0 = 六条全部按预期见红）
# 不进日常门禁：它改产品码再回滚（同 run_tier1_first_red.sh 的理由）。
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

pass=0
fail=0
current=""
# 回滚锚点是**注入前的文件内容**，不是 HEAD。用 git checkout 回滚会连带丢掉
# 工作区里未提交的改动——本脚本第一版就是这么写的，在同一个工作区里连丢两次码。
# Tier 1 那批没暴露，只因为当时工作区恰好是干净的；工具的正确性不能依赖那个巧合。
# The rollback anchor is the pre-injection file, not HEAD: `git checkout --` also
# reverts uncommitted work. Two losses in a row proved it.
work=$(mktemp -d)
trap 'rm -rf "$work"; cleanup' EXIT INT TERM

cleanup() {
    if [ -n "$current" ] && [ -f "$work/backup" ]; then
        cp "$work/backup" "$current" 2>/dev/null
    fi
}

inject() {
    python3 - "$1" "$2" "$3" <<'PY'
import pathlib
import sys

path, old, new = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
text = path.read_text(encoding="utf-8")
n = text.count(old)
if n != 1:
    print("INJECT-FAIL: pattern occurs %d time(s) in %s" % (n, path))
    sys.exit(9)
path.write_text(text.replace(old, new, 1), encoding="utf-8")
PY
}

# red <id> <file> <old> <new> <expected-substring> <cmd...>
red() {
    local id="$1" file="$2" old="$3" new="$4" expect="$5"
    shift 5
    local out rc
    current="$file"
    cp "$file" "$work/backup"
    if ! inject "$file" "$old" "$new"; then
        echo "FAIL $id: could not inject the bite (pattern drifted)"
        cp "$work/backup" "$file"; current=""; fail=$((fail + 1)); return
    fi
    out=$("$@" 2>&1)
    rc=$?
    cp "$work/backup" "$file"; current=""
    if [ "$rc" -eq 0 ]; then
        echo "FAIL $id: still green after the bite was removed"
        fail=$((fail + 1)); return
    fi
    if ! grep -qF "$expect" <<<"$out"; then
        echo "FAIL $id: red, but not on the expected assertion ($expect)"
        echo "$out" | tail -6
        fail=$((fail + 1)); return
    fi
    echo "ok   $id  (rc=$rc, red on: $expect)"
    pass=$((pass + 1))
}

# red_soft：编译型宿主用例，红点由 assert 打印；**排除编译错**。
red_soft() {
    local id="$1" file="$2" old="$3" new="$4"
    shift 4
    local out rc line
    current="$file"
    cp "$file" "$work/backup"
    if ! inject "$file" "$old" "$new"; then
        echo "FAIL $id: could not inject the bite (pattern drifted)"
        cp "$work/backup" "$file"; current=""; fail=$((fail + 1)); return
    fi
    out=$("$@" 2>&1)
    rc=$?
    cp "$work/backup" "$file"; current=""
    if [ "$rc" -eq 0 ]; then
        echo "FAIL $id: still green after the bite was removed"
        fail=$((fail + 1)); return
    fi
    if grep -q "error:" <<<"$out"; then
        echo "FAIL $id: red at compile time, not on an assertion"
        echo "$out" | grep "error:" | head -3
        fail=$((fail + 1)); return
    fi
    line=$(grep -m1 -E "Assertion|FAIL|failed" <<<"$out")
    if [ -z "$line" ]; then
        echo "FAIL $id: red, but no assertion line found"
        echo "$out" | tail -6
        fail=$((fail + 1)); return
    fi
    echo "ok   $id  (rc=$rc, red on: ${line:0:96})"
    pass=$((pass + 1))
}

SD=components/read_pico/read_pico_sd.c
LOOP=main/app/app_loop.c

# P1a：墙钟判据是判据，次数只是快路径。删掉它 = 最坏路径 16 s。
red_soft "P1a 墙钟判据删除 → 只剩 8 次" \
    "$SD" \
    '    const bool dead = fails >= SD_IO_FAIL_LIMIT || since >= SD_IO_DEAD_MS;' \
    '    const bool dead = fails >= SD_IO_FAIL_LIMIT;' \
    bash tools/run_app_loop_host_tests.sh

# P1b：8 次是快路径，晚一拍就是慢一拍（本用例的 2.1 s 档正好卡在边界）。
red_soft "P1b 快路径 8 → 9" \
    "$SD" \
    '#define SD_IO_FAIL_LIMIT     8u' \
    '#define SD_IO_FAIL_LIMIT     9u' \
    bash tools/run_app_loop_host_tests.sh

# P1c：成功必须清零——不清零，两次无关的坏卡会被拼成一次判死。
red_soft "P1c 成功后不清零" \
    "$SD" \
    '        io_watch_reset();
        s_io_next_ms = now + SD_IO_IDLE_GAP_MS;' \
    '        s_io_next_ms = now + SD_IO_IDLE_GAP_MS;' \
    bash tools/run_app_loop_host_tests.sh

# P1d：只数 0x107——把白名单拆掉，一次 INVALID_STATE 也能判死。
red_soft "P1d 所有错误码都计入" \
    "$SD" \
    '    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {' \
    '    if (err != ESP_OK && err != ESP_ERR_TIMEOUT && 0) {' \
    bash tools/run_app_loop_host_tests.sh

# B42a：假收口回潮。
red "B42a 塞回 quiesce(0)" \
    "$LOOP" \
    '        book_cover_jobs_discard();' \
    '        book_cover_jobs_discard();
        (void)book_cover_jobs_quiesce(0);' \
    "B42a quiesce(0)" \
    python3 tools/check_quiesce_zero.py

# B42c：边沿不再作废旧封面（牙的修法落点被抹掉）。
red "B42c 删掉 discard" \
    "$LOOP" \
    '        book_cover_jobs_discard();' \
    '        book_cover_jobs_cancel(0);' \
    "B42c poll_media() no longer calls book_cover_jobs_discard()" \
    python3 tools/check_quiesce_zero.py

echo
echo "P11_FIRST_RED_SUMMARY: $pass ok / $fail failed"
[ "$fail" -eq 0 ]
