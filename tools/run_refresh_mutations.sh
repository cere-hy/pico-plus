#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
#
# 26.11.4~26.11.8 的刷新与插卡牙（B77 / B78 / B79 / B80 / B82）的先红验证。
# 26.11.6 把 B79b 从「翻页必须全刷」反转成「翻页必须局刷」——变的不是牙，是判据
# （时间账 -> 闪账/灰账）。R2 就是那次反转的反向探针。
# First-red sweep for B77/B78/B79/B80. 26.11.6 inverted B79b (a turn must be a local
# refresh, not a full one): the tooth did not change, the criterion did.
#
# 为什么单独一个脚本：靶点横跨三处——阅读页的刷新出口（app_book.c）、三拍判据
# （push_session.c，纯函数，宿主用例也在咬）、插拔边沿的序关系与对称义务
# （app_loop.c）。塞进别的变异脚本会让「哪一侧被改脏」读不出来。
# Why its own script: the targets span three places - the reader's refresh exits, the
# quiesce predicate (a pure function also covered by the host test), and the hotplug
# edge's order + symmetry obligation.
#
# 判定：**每刀都必须被指定的检查单独擒住**。存活 = 牙有洞；锚点漂移（不存在或
# 不唯一）= 直接判失败——「变异没植进去、检查照过」是最危险的假绿。
# Every knife must be caught by its designated check; a drifted anchor is an outright
# failure ("the mutation never landed, the check still passed" is the worst green).
#
# 串行纪律：与其他变异脚本共用工作区，不得并发。
#
# 总闸（2026-11-03 起）：跑完必须证明**工作树与跑前逐字节相同**（`tools/tree_identity.py`）。
# 变异全绿只说明「每刀都被咬住」，不说明「树是干净的」——`[122]` 那颗同尺寸的脏 bin
# 就是全绿之后编出来的。出包四步锁序：**变异 → 树身份 → 门禁 → 构建打包**，
# 由 `tools/release_gate.sh` 串起来跑，任一步红即停。
# The mutation suite ends by proving the worktree is byte-identical to how it started:
# all-green only means every knife was caught, not that the tree is clean.
set -uo pipefail
cd "$(dirname "$0")/.."

FILES=(
    main/apps/app_book.c
    main/display_reader_waveform.c
    components/epdiy/src/render.c
    components/push_sink/src/push_session.c
    main/app/app_loop.c
)

BACKUPS=()
snapshot() {
    BACKUPS=()
    for f in "${FILES[@]}"; do
        b=$(mktemp); cp "$f" "$b"; BACKUPS+=("$b")
    done
}
restore() {
    # 幂等：EXIT trap 与正常收尾都可能调，第二次进来时备份已被删空。
    [ "${#BACKUPS[@]}" -eq 0 ] && return 0
    local i=0
    for f in "${FILES[@]}"; do cp "${BACKUPS[$i]}" "$f"; rm -f "${BACKUPS[$i]}"; i=$((i + 1)); done
    BACKUPS=()
}

# ---- 总闸：跑前跑后树身份必须逐字节相同 ------------------------------------
#
# 2026-11-03 `[122]` 事故：FILES 漏了 render.c，R26 改完没还原，错误版本留在
# 树上并编出一颗**同尺寸、能过尺寸检查**的含 bug bin。FILES 补全 + mutate() 的
# 「目标必须在 FILES 内」是**保险丝**，管的是那一个遗漏点；这里是**总闸**，
# 不依赖任何清单的完备性：清单遗漏、restore 失败、套件半途崩溃，全收进这一个检查。
# 清单完备性是隐性耦合（本批恰好同时改 render.c 和变异 render.c 才暴露），
# 而「跑前 == 跑后」不需要知道谁被改过。
TREE_BEFORE=$(python3 tools/tree_identity.py --quiet) || {
    echo "TREE IDENTITY UNAVAILABLE: 无法取得树身份，拒绝开跑（失败关闭）"; exit 2; }
TREE_CHECKED=0
tree_gate() {
    TREE_CHECKED=1
    if python3 tools/tree_identity.py --expect "$TREE_BEFORE" --quiet; then
        echo "tree gate: worktree byte-identical to pre-run ($TREE_BEFORE)"
        return 0
    fi
    echo "TREE GATE FAILED: 变异套件污染了工作树——此时任何构建产物都不可送验"
    FAILED=1
    return 1
}
on_exit() {
    local rc=$?
    restore
    # 半途崩溃（set -u、trap、手动 Ctrl-C）也要过闸，否则「崩了但树是脏的」会静默。
    if [ "$TREE_CHECKED" -eq 0 ]; then
        python3 tools/tree_identity.py --expect "$TREE_BEFORE" --quiet || rc=1
    fi
    exit "$rc"
}
trap on_exit EXIT
snapshot

FAILED=0
CAUGHT=0
EXPECTED=21   # 加一刀就改这里；少一刀脚本自己会红

mutate() {
    local name="$1" file="$2" old="$3" new="$4" rc=0
    # 被测文件必须在 FILES 里，否则 restore 管不到它——2026-11-03 真踩过：
    # R26 改 render.c 后没被还原，错误版本一直被提交进 [122]。
    local listed=""
    for f in "${FILES[@]}"; do [ "$f" = "$file" ] && listed=1; done
    if [ -z "$listed" ]; then
        echo "TARGET NOT IN FILES: $name ($file) - restore would not cover it"; FAILED=1; return 1
    fi
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

# run_mutation <名字> <说明> <检查命令...>。检查必须变红，否则判存活。
run_mutation() {
    local name="$1" desc="$2"; shift 2
    if "$@" >/dev/null 2>&1; then
        echo "SURVIVED: $name $desc"; FAILED=1
    else
        echo "caught: $name $desc"; CAUGHT=$((CAUGHT + 1))
    fi
    restore; snapshot
}

if ! python3 tools/check_refresh_ghost.py >/dev/null 2>&1; then
    echo "BASELINE RED: check_refresh_ghost.py fails before any mutation"
    exit 1
fi
if ! python3 tools/check_hotplug.py >/dev/null 2>&1; then
    echo "BASELINE RED: check_hotplug.py fails before any mutation"
    exit 1
fi
if ! bash tools/run_push_sink_host_tests.sh >/dev/null 2>&1; then
    echo "BASELINE RED: push_sink host test fails before any mutation"
    exit 1
fi
# 基线必须覆盖**每一个被当作判据的检查器**——2026-11-03 `[122]` 的教训：
# `ui_refresh_feedback_host_test.py` 当时已经是红的（AI 页路由断言没跟着改），
# 而 R15 恰好用它当判据，于是「被咬住」是基线本来就红，跟 R15 那一刀毫无关系。
# 全绿 + 21/21 一起成立，掩盖了一次真回归和一次假绿。
# Every checker used as a verdict must be green on the baseline, or "caught" may just
# mean the baseline was already red.
if ! python3 tools/ui_refresh_feedback_host_test.py >/dev/null 2>&1; then
    echo "BASELINE RED: ui_refresh_feedback host test fails before any mutation"
    exit 1
fi

# ---- R1：阅读整页出口退回整屏差分（残影原路）--------------------------------
mutate "R1" main/apps/app_book.c \
    '            s_turns = 0;                    // T2 同为整屏清底，不变式一致
            err = update_display_full(ctx->hl);' \
    '            err = update_display_mode_diff(ctx->hl, MODE_GL16);' || true
[ "$FAILED" -eq 0 ] && run_mutation "R1" "reader whole-page exit back to a screen-wide differential" \
    python3 tools/check_refresh_ghost.py

# ---- R2：正文翻页退回 GL16 + 差分（26.11.6 的留灰）------------------------
mutate "R2" main/apps/app_book.c \
    '            err = update_display_area_full_with(ctx->hl, display_reader_soft_turn_waveform(),
                                                MODE_GL16, s_area);' \
    '            err = update_display_area_diff_with(ctx->hl, &E0470_WAVEFORM, MODE_GL16, s_area);' || true
[ "$FAILED" -eq 0 ] && run_mutation "R2" "body turn back to GL16 + diff: no white phase, the base darkens page by page (26.11.6's ghost)" \
    python3 tools/check_refresh_ghost.py

# ---- R22：正文翻页退回 26.11.7 那一行（GC16 + 36 相裁剪表）----------------
mutate "R22" main/apps/app_book.c \
    '            err = update_display_area_full_with(ctx->hl, display_reader_soft_turn_waveform(),
                                                MODE_GL16, s_area);' \
    '            err = update_display_area_with(ctx->hl, &E0470_WAVEFORM, MODE_GC16, s_area);' || true
[ "$FAILED" -eq 0 ] && run_mutation "R22" "body turn back to GC16 + the trimmed table: per-page flash, left-edge band and inverted text all return" \
    python3 tools/check_refresh_ghost.py

# ---- R21：柔和波形模块换回 git 里的旧 3,664 B 版 ---------------------------
# 这一刀砍的是哈希牙本身：文件在、符号在、编译也过，只有字节不同。
# 「旧版复活」是本批最容易犯的错——它长得就像「恢复上游模块」。
if git show a5a1f91^:main/display_reader_waveform.c > /tmp/rwf_retired.c 2>/dev/null; then
    cp /tmp/rwf_retired.c main/display_reader_waveform.c
    run_mutation "R21" "soft waveform module reverted to the retired revision: compiles, exported, wrong bytes" \
        python3 tools/check_refresh_ghost.py
else
    echo "TARGET MISSING: R21 (retired revision not in history)"; FAILED=1
fi

# ---- R23：x0 的宏改回 12（十二相白推复活）----------------------------------
# 归一化哈希只抹值、不判值：少了这一刀，把宏改回 12 反而全绿，而 12 相正是
# 「静置灰雾」被判决的那个东西。最隐蔽的回归，必须单独一刀。
mutate "R23" main/display_reader_waveform.c \
    '#define READER_SOFT_TURN_SETTLE_EXTRA 0' \
    '#define READER_SOFT_TURN_SETTLE_EXTRA 12' || true
[ "$FAILED" -eq 0 ] && run_mutation "R23" "settle extra back to 12: the idle grey veil and the surplus white push return" \
    python3 tools/check_refresh_ghost.py

# ---- R24：声明偏离之外改一个字节（顺手优化 / 手工微调）---------------------
# 归一化只剥「值」和「那一段注释」，别的字节一个都不许动。
mutate "R24" main/display_reader_waveform.c \
    '#define READER_SOFT_TURN_BLACK_EXTRA 4' \
    '#define READER_SOFT_TURN_BLACK_EXTRA 5' || true
[ "$FAILED" -eq 0 ] && run_mutation "R24" "one byte outside the declared deviations (black re-drive 4 -> 5): still kiko-plus-two, no longer checked" \
    python3 tools/check_refresh_ghost.py

# ---- R25：往 x0 注释块里夹一行代码（借注释块走私）--------------------------
# 注释块是为了把「为什么是 0」留在代码里；一旦它能夹带代码，归一化就成了后门。
mutate "R25" main/display_reader_waveform.c \
    '// <<< 26.11.8 x0 NOTE end' \
    $'static int smuggle = 1;\n// <<< 26.11.8 x0 NOTE end' || true
[ "$FAILED" -eq 0 ] && run_mutation "R25" "code smuggled inside the x0 note block: the block must stay comment-only" \
    python3 tools/check_refresh_ghost.py

# ---- R26：差分右边界掩码退回 min_x（上游移植 bug 复活）---------------------
# 只在「最左脏列与最右脏列奇偶不同」时丢 1 列，肉眼极难抓，只能靠 B83 咬。
mutate "R26" components/epdiy/src/render.c \
    '        uint8_t mask = max_x % 2 ? 0xF0 : 0x0F;' \
    '        uint8_t mask = min_x % 2 ? 0xF0 : 0x0F;' || true
[ "$FAILED" -eq 0 ] && run_mutation "R26" "right-edge crop masks with min_x again: one column lost on the right when parities differ" \
    python3 tools/check_refresh_ghost.py

# ---- R27：AI 解析页退回 GC16 + 差分（26.11.8 的漏项复活）-------------------
mutate "R27" main/apps/app_book.c \
    '            err = update_display_area_full_with(ctx->hl, display_reader_soft_turn_waveform(),
                                                MODE_GL16, reader_area());' \
    '            err = update_display_area_with(ctx->hl, &E0470_WAVEFORM, MODE_GC16, reader_area());' || true
[ "$FAILED" -eq 0 ] && run_mutation "R27" "AI detail turns back to GC16 + diff: the band-making combination returns on the AI path" \
    python3 tools/check_refresh_ghost.py

# ---- R3：视图边沿不再升档（开书第一屏又带旧墨）------------------------------
mutate "R3" main/apps/app_book.c \
    '    if (s_presented_view != (int)s_view &&
        (s_view == READING || s_presented_view == READING))
        redraw = APP_REDRAW_FULL;' \
    '    if (s_presented_view != (int)s_view &&
        (s_view == READING || s_presented_view == READING))
        redraw = APP_REDRAW_PAGE;' || true
[ "$FAILED" -eq 0 ] && run_mutation "R3" "view edge downgraded to PAGE: the first screen keeps the shelf's ink" \
    python3 tools/check_refresh_ghost.py

# ---- R4：页脚小面积被卷进快刷之外（反向判据）--------------------------------
mutate "R4" main/apps/app_book.c \
    'MODE_DU, progress_rect()));' \
    'MODE_GL16, progress_rect()));' || true
[ "$FAILED" -eq 0 ] && run_mutation "R4" "footer strip lost its DU fast path (a 42 px band on the page cadence)" \
    python3 tools/check_refresh_ghost.py

# ---- R10：AI 页翻页不再标「页码前进」（落回整页全刷）------------------------
mutate "R10" main/apps/app_book.c \
    '    s_reader_body_turn = true;
    s_reader_page_advance = true;' \
    '    s_reader_page_advance = true;' || true
[ "$FAILED" -eq 0 ] && run_mutation "R10" "AI page turns stop marking the frame: they fall into the whole-page GC16" \
    python3 tools/check_refresh_ghost.py

# ---- R11：跨章不再强制清底（用户明文要求的「换章节全刷」）--------------------
mutate "R11" main/apps/app_book.c \
    '    s_reader_cleanup = chapter_changed;     // 整屏分支负责把计数归零 / the full branch resets it' \
    '    s_reader_cleanup = false;' || true
[ "$FAILED" -eq 0 ] && run_mutation "R11" "chapter crossing no longer forces cleanup (the ruled 'new chapter = full refresh')" \
    python3 tools/check_refresh_ghost.py

# ---- R12：计数器不自增（静态牙看不见，靠宿主真值表咬）----------------------
mutate "R12" main/apps/app_book.c \
    '    if (++s_turns < full_pages) return false;' \
    '    if (s_turns < full_pages) return false;' || true
[ "$FAILED" -eq 0 ] && run_mutation "R12" "counter never increments: 'every N pages' silently dies (host truth table must bite)" \
    python3 tools/ui_refresh_feedback_host_test.py

# ---- R5：三拍判据退回「等于 IDLE」（26.11.4 的死锁原形）---------------------
mutate "R5" components/push_sink/src/push_session.c \
    '    const bool mid_transfer = state >= PUSH_STATE_RECEIVING && state <= PUSH_STATE_PROCESSING;
    return !mid_transfer && !pending_arm && live_files == 0u;' \
    '    return state == PUSH_STATE_IDLE && !pending_arm && live_files == 0u;' || true
[ "$FAILED" -eq 0 ] && run_mutation "R5" "quiesce predicate reverted to state == IDLE (the never-unmount deadlock)" \
    python3 tools/check_hotplug.py

# ---- R6：丢掉窗口上界（COOLDOWN 又被当成在传）-------------------------------
mutate "R6" components/push_sink/src/push_session.c \
    'state >= PUSH_STATE_RECEIVING && state <= PUSH_STATE_PROCESSING;' \
    'state >= PUSH_STATE_RECEIVING;' || true
[ "$FAILED" -eq 0 ] && run_mutation "R6" "window upper bound dropped: COOLDOWN (post-push grace) counts as transferring" \
    python3 tools/check_hotplug.py

# ---- R7：丢掉「待武装」合取（静态牙看不见，靠宿主用例咬）-------------------
mutate "R7" components/push_sink/src/push_session.c \
    'return !mid_transfer && !pending_arm && live_files == 0u;' \
    'return !mid_transfer && live_files == 0u;' || true
[ "$FAILED" -eq 0 ] && run_mutation "R7" "pending-arm conjunct dropped (host truth table must bite what the static tooth cannot)" \
    bash tools/run_push_sink_host_tests.sh

# ---- R8：拔卡不再收链路（写向不存在的卷）------------------------------------
mutate "R8" main/app/app_loop.c \
    '    push_sink_detach_media("card removed");
' \
    '' || true
[ "$FAILED" -eq 0 ] && run_mutation "R8" "detach removed: the link outlives the medium" \
    python3 tools/check_hotplug.py

# ---- R9：插回不再还可连性（修成「认卡但连不上」）----------------------------
mutate "R9" main/app/app_loop.c \
    '        (void)push_sink_resume_after_media();
' \
    '' || true
[ "$FAILED" -eq 0 ] && run_mutation "R9" "reattach no longer repays connectivity: card works, phone never reconnects" \
    python3 tools/check_hotplug.py

# ---- R14：水波纹收尾退回整屏（26.11.6 的每次翻页闪屏）----------------------
mutate "R14" main/apps/app_book.c \
    '                err = update_display_area_full_with(ctx->hl, &E0470_WAVEFORM, MODE_GC16, reader_area());' \
    '                err = update_display_full(ctx->hl);' || true
[ "$FAILED" -eq 0 ] && run_mutation "R14" "ripple closing wipe back to the whole screen: the 448 ms tail that flashed every turn" \
    python3 tools/check_refresh_ghost.py

# ---- R15：N=0 档的 `n &&` 被拿掉（「不强制」退化成每拍清底）----------------
mutate "R15" main/apps/app_book.c \
    '    if (!full_pages) { s_turns = 0; return false; }   // 「不强制」= 永不因计数清底' \
    '    if (!full_pages) { s_turns = 0; }' || true
[ "$FAILED" -eq 0 ] && run_mutation "R15" "the n == 0 guard lost its return: 'off' becomes a full refresh on every turn" \
    python3 tools/ui_refresh_feedback_host_test.py

echo
tree_gate
echo
echo "refresh mutations: ${CAUGHT}/${EXPECTED} caught, FAILED=${FAILED}"
[ "$FAILED" -eq 0 ] && [ "$CAUGHT" -eq "$EXPECTED" ] && [ "$TREE_CHECKED" -eq 1 ]
