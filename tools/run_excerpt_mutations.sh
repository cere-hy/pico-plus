#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
# 中文：26.9.26 S26-1 摘抄落盘的变异验证。逐个把纪律拆回「看起来也能跑」的形态，
# 确认每一刀都被宿主用例 + 结构牙擒住（exit 1 = 擒住）。存活变异 = 牙有洞。
# English: mutation sweep for S26-1 excerpt storage. Each mutation breaks one discipline
# into a shape that still looks plausible; the host tests and the structure teeth must
# catch every one. A survivor is a hole in a tooth.
#
# 靶点漂移保护：锚点必须**存在且唯一**，否则直接判失败——重构后「变异没植进去、
# 检查照过」会伪装成 SURVIVED，那种假绿灯比红灯危险（26.9.23 已栽过一次）。
# Target-drift guard: anchors must exist AND be unique, else the sweep fails outright.
#
# 串行纪律：本脚本与其他变异脚本共用工作区，并发会互相覆盖备份（26.9.24 事故）。
# Serial only: it shares the worktree with the other mutation scripts; running them
# concurrently clobbers each other's backups (the 26.9.24 incident).
set -uo pipefail
cd "$(dirname "$0")/.."

EX_C=main/book/excerpt_store.c
EX_H=main/book/excerpt_store.h
FILES=("$EX_C" "$EX_H")

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

# 判定：宿主用例和结构牙都必须绿才算存活。/ Killed if either the host tests or a tooth fails.
run_mutation() {
    local name="$1" desc="$2"
    if bash tools/run_excerpt_host_tests.sh >/dev/null 2>&1 && \
       python3 tools/check_excerpt.py >/dev/null 2>&1; then
        echo "SURVIVED: $name $desc"; FAILED=1
    else
        echo "caught: $name $desc"; CAUGHT=$((CAUGHT + 1))
    fi
    restore; snapshot
}

if ! bash tools/run_excerpt_host_tests.sh >/dev/null 2>&1; then
    echo "BASELINE RED: the excerpt host tests fail before any mutation"
    exit 1
fi

# ---- M101：队列上限漂移（11 连摘不再拒新）----------------------------------
mutate "M101" "$EX_H" '#define EXCERPT_QUEUE_MAX 10u' '#define EXCERPT_QUEUE_MAX 11u' || true
[ "$FAILED" -eq 0 ] && run_mutation "M101" "queue cap drifts to 11: the 11th excerpt is no longer refused"

# ---- M102：PSRAM 申请失败不拒新（拿 NULL 继续跑）----------------------------
mutate "M102" "$EX_C" '    if (!line) {' '    if (false) {' || true
[ "$FAILED" -eq 0 ] && run_mutation "M102" "PSRAM failure no longer refuses the entry"

# ---- M103：冲账不再先 mkdir（目录被删即永久写不进）--------------------------
mutate "M103" "$EX_C" \
    '    if (ex_mkdir(EXCERPT_DIR) != 0 && errno != EEXIST) {' \
    '    if (0) {' || true
[ "$FAILED" -eq 0 ] && run_mutation "M103" "flush stops creating the directory: a deleted one never heals"

# ---- M104：只看 fwrite，不看 fsync（sync 失败假装成功）----------------------
mutate "M104" "$EX_C" '    const int sy = ex_sync(f);' '    const int sy = 0;' || true
[ "$FAILED" -eq 0 ] && run_mutation "M104" "fsync result ignored: a failed sync looks like a save"

# ---- M105：半行也当成功（弃档被整段跳过）-----------------------------------
mutate "M105" "$EX_C" '    if (wr != it->len || ex_flush(f) != 0) {' '    if (false) {' || true
[ "$FAILED" -eq 0 ] && run_mutation "M105" "a half line is treated as a save (the fragment is never terminated)"

# ---- M106：一个家务片抽干整队（书架扫描插不进来）----------------------------
mutate "M106" "$EX_C" \
    '    if (s_count == 0) return EXCERPT_FLUSH_IDLE;' \
    '    if (s_count == 0) return EXCERPT_FLUSH_IDLE;
    if (s_count > 1u) { (void)excerpt_store_tick(now_ms); }' || true
[ "$FAILED" -eq 0 ] && run_mutation "M106" "one slot drains the whole queue: no room for the shelf scan"

# ---- M107：拆掉唯一的时间门（让行 + 退避共用同一个判据）--------------------
# E-1 把两个时间线并成一个字段后，这道门一旦拿掉，**两件事同时失效**：挂载后立刻冲账
# （撞书架扫描），失败后每片都重试（389 次风暴复活）。所以描述里两个后果都写着。
# E-1 merged the two timelines into one gate; removing it breaks BOTH: the mount yield
# races the shelf scan AND every failure retries on the next slot (the 389-retry storm).
mutate "M107" "$EX_C" \
    '    if (s_next_try_ms && now_ms < s_next_try_ms) return EXCERPT_FLUSH_YIELD;' \
    '    (void)now_ms;' || true
[ "$FAILED" -eq 0 ] && run_mutation "M107" "the single time gate removed: the mount yield races the shelf scan and every failure retries on the next slot"

# ---- M107b：失败不记下次时刻（E-1 的原始病灶，逐字复刻）--------------------
# §19.3 的现场不是「重试太多次」，是**失败路径根本不写时间**：于是每个家务片都再试一次，
# 4.7 s 里 389 次。这一刀专门切那个赋值。
# The 19.3 defect was never "too many retries" but "the failure path wrote no deadline",
# so every slot tried again: 389 attempts in 4.7 s. This knife cuts exactly that line.
mutate "M107b" "$EX_C" \
    '    s_next_try_ms = now_ms + k_backoff_ms[step];' \
    '    s_next_try_ms = 0;' || true
[ "$FAILED" -eq 0 ] && run_mutation "M107b" "a failure records no deadline: every housekeeping slot retries again (the 389-retry storm, verbatim)"

# ---- M108：UTF-8 截断不回退（切半个汉字）-----------------------------------
mutate "M108" "$EX_C" \
    '    while (i > 0 && ((unsigned char)s[i] & 0xC0u) == 0x80u) --i;' \
    '    while (false) --i;' || true
[ "$FAILED" -eq 0 ] && run_mutation "M108" "UTF-8 cut stops backing off: a character gets sliced in half"

# ---- M109：不拒空文本--------------------------------------------------------
mutate "M109" "$EX_C" '    if (!text || !*text) return EXCERPT_EMPTY;' '    if (false) return EXCERPT_EMPTY;' || true
[ "$FAILED" -eq 0 ] && run_mutation "M109" "empty text is no longer refused"

# ---- M110：书名不按 160B 硬截（前缀吃穿单行上限）----------------------------
# 26.11.2 P0-4 重定向：copy_capped 已被 fold_ws 取代，靶点跟着搬——
# 书名不按 160B 硬截 → fold_ws 的标题 cap 改喂 EXCERPT_LINE_MAX（ASAN 会咬越界）。
mutate "M110" "$EX_C" \
    '    at += fold_ws(line + at, EXCERPT_TITLE_MAX, book ? book : "未知", NULL);' \
    '    at += fold_ws(line + at, EXCERPT_LINE_MAX, book ? book : "未知", NULL);' || true
[ "$FAILED" -eq 0 ] && run_mutation "M110" "title no longer capped at 160 B: the prefix eats the line budget"

# ---- M111：开机序号不递增----------------------------------------------------
mutate "M111" "$EX_C" \
    '(unsigned)(++s_boot_seq));' \
    '(unsigned)(s_boot_seq));' || true
[ "$FAILED" -eq 0 ] && run_mutation "M111" "per-boot sequence stops advancing"

# ---- M112：不打 `[截]` 标记（截断还说没截）---------------------------------
# 26.11.2 P0-4 重定向：标记写点从 copy_capped 搬到 fold_ws 截断路径。
mutate "M112" "$EX_C" '    memcpy(dst + at, "[截]", 5);' '    (void)at;' || true
[ "$FAILED" -eq 0 ] && run_mutation "M112" "truncation marker dropped: a cut excerpt looks complete"

# ---- M113：fopen 不带 APPEND（覆盖历史摘抄）--------------------------------
mutate "M113" "$EX_C" '#define ex_open_append(p) fopen((p), "ab")' \
                     '#define ex_open_append(p) fopen((p), "wb")' || true
[ "$FAILED" -eq 0 ] && run_mutation "M113" "opened without append: every save would overwrite the history"

# ---- M114：落盘成功但不置「已存」闩（先写后亮断链）-------------------------
mutate "M114" "$EX_C" '    s_saved_latch = true;' '    s_saved_latch = false;' || true
[ "$FAILED" -eq 0 ] && run_mutation "M114" "the saved latch never sets: write-first-then-tick loses its tick"

# ---- M115：尾巴不再 fsync（先写后亮的物理心跳被拿掉）-----------------------
mutate "M115" "$EX_C" '#define ex_sync(f) fsync(fileno(f))' '#define ex_sync(f) 0' || true
[ "$FAILED" -eq 0 ] && run_mutation "M115" "fsync removed from the tail: 'saved' becomes a guess (B38 + branch 3)"

# ---- M116：路径改回盘符（同一个错第二次犯）---------------------------------
mutate "M116" "$EX_H" '#define EXCERPT_PATH "/sdcard/.readpico/excerpts.txt"' \
                      '#define EXCERPT_PATH "0:/.readpico/excerpts.txt"' || true
[ "$FAILED" -eq 0 ] && run_mutation "M116" "the path goes back to a drive letter: the 26.9.28 bug, verbatim"

# ---- M117：fclose 失败也留队（数据已持久却重试 = 写重复行）-----------------
mutate "M117" "$EX_C" '    if (sy != 0 && cl != 0) {' '    if (sy != 0 || cl != 0) {' || true
[ "$FAILED" -eq 0 ] && run_mutation "M117" "a close failure keeps the entry: a durable line gets written twice"

# ---- M118：P0-4 净化被拆（正文里的 \n\t 重新上卡，B43 当场咬）--------------
mutate "M118" "$EX_C" '    const size_t body = fold_ws(line + at, cap, text, &body_truncated);' \
                     '    const size_t body = (size_t)snprintf(line + at, cap, "%s", text); (void)body_truncated;' || true
[ "$FAILED" -eq 0 ] && run_mutation "M118" "the fold is gone: newlines and tabs ride onto the card again (B43)"

# 总数不再硬编码：加了靶点却忘了改计数，会让「18/17 killed」这种自相矛盾的绿留在日志里。
# 数 `^mutate "` 的出现次数，靶点表是唯一真值。
# The total is no longer hard-coded: adding a target and forgetting the count used to leave
# "18/17 killed" in the log, which reads as green while documenting a drift.
EXPECT=$(grep -cE '^mutate "' "$0")
if [ "$FAILED" -ne 0 ] || [ "$CAUGHT" -ne "$EXPECT" ]; then
    echo "excerpt mutations: FAILED ($CAUGHT/$EXPECT killed - a target drifted or a mutation survived)"
    exit 1
fi
echo "excerpt mutations: $CAUGHT/$EXPECT killed"
