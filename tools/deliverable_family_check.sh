#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# 交付物家族六件对表（第五道固定检查，2026-10-24 gen26.3 基装轮立；第六件 gen26.4.1 收官轮补）。
# The deliverable-family six-item reconciliation (the fifth fixed check,
# established in the 2026-10-24 gen26.3 base-install round; item 6 added in
# the 2026-10-24 gen26.4.1 closing round).
#
# 存在的理由 / Why this exists:
#   gen26.3 基装轮一次抓出三处仓库侧陈旧（firmware/ 旧 bin、flash.bat 锚停在
#   gen22、RELEASE_NOTES gen22 残留）+ 一处漏同步（manifest.json）——不是巧合，
#   是每代交付物家族有同步义务但没有对表。凡出包或重锚动作，这五件必须过一遍：
#   In the gen26.3 base-install round one sweep caught three stale repo-side
#   artifacts (firmware/ old bins, flash.bat's anchor frozen at gen22,
#   RELEASE_NOTES gen22 residue) plus one missed sync (manifest.json) - not a
#   coincidence: every generation owes the deliverable family a sync, but no
#   checklist existed. Any packaging or re-anchoring action must run all six:
#
#   1. firmware/flash.sh   EXPECTED == sha256(firmware/Read_Pico.bin)
#   2. firmware/flash.bat  EXPECTED == 同上（Windows 路径，曾三代没动）
#   3. web-flasher/        Read_Pico.bin 与 firmware/ 逐字节一致；
#                          index.html 徽章 + manifest.json 版本 == PROJECT_VER
#   4. RELEASE_NOTES.md    头部版本 == PROJECT_VER；bin 尺码/哈希前缀、APK
#                          哈希前缀与实物一致
#   5. apk/README.md       所述 APK 尺码与实物一致（KB 取整口径）
#   6. apk/ReadPicoPush.apk  发布副本与构建产物逐字节一致——gen26.4.1 收官轮
#                          实测分叉：web-flasher 下载链接指向远端 main 的 apk/，
#                          而该副本停在 v1.4.1（e9fb1704…），tools 产物已是
#                          v1.4.2（a6877152…），新用户经网页装到旧 APK；
#                          此前五件不覆盖该副本，故分叉两代无人红。
#                          (release copy must be byte-identical to the build
#                          output - it drifted two generations unnoticed)
#   7. 固件自证版本串（26.9.6 立，审计裁定③每版必修）：main/app_main.c 必须打印
#      `fw: <PROJECT_VER>`，且本代验收 README 的**判据第一条**必须钉住这行串。
#      由来：round-3 复验因固件不自证而无法归因（详见脚本内第 7 项注释）。
#      (the firmware must state its own version, and this generation's
#      acceptance README must pin that string as its FIRST item)
#
# 单一真相源：版本串取 CMakeLists.txt 的 PROJECT_VER，哈希/尺码取实物文件。
# Single source of truth: the version string comes from CMakeLists.txt's
# PROJECT_VER; hashes and sizes come from the real files.
#
# 用法 / Usage: bash tools/deliverable_family_check.sh（仓库根运行）
set -u
cd "$(dirname "$0")/.."

fail=0
note() { printf '%s\n' "$*"; }
bad() { printf 'FAIL: %s\n' "$*" >&2; fail=1; }

# --- 单一真相源 / single source of truth ---
# 只取字面量行（else 分支的 0.x 串）；env 覆盖行 set(PROJECT_VER "$ENV{…}")
# 以 $ 开头不匹配。/ Take only the literal line (the else branch's 0.x
# string); the env-override line starts with $ and does not match.
VER=$(sed -n 's/.*set(PROJECT_VER "\(0\.[^"]*\)").*/\1/p' CMakeLists.txt | tail -1)
[ -n "$VER" ] || { note "FAIL: PROJECT_VER not found in CMakeLists.txt"; exit 1; }
BIN=firmware/Read_Pico.bin
APK=tools/android/apk/ReadPicoPush.apk
[ -f "$BIN" ] || { note "FAIL: missing $BIN"; exit 1; }
[ -f "$APK" ] || { note "FAIL: missing $APK"; exit 1; }
SHA=$(sha256sum "$BIN" | cut -d' ' -f1)
SIZE=$(stat -c '%s' "$BIN")
APKSHA=$(sha256sum "$APK" | cut -d' ' -f1)
APKSIZE=$(stat -c '%s' "$APK")
note "deliverable family check: PROJECT_VER=$VER  bin=$SHA (${SIZE}B)  apk=$APKSHA (${APKSIZE}B)"

# --- 1. flash.sh 锚 / flash.sh anchor ---
got=$(sed -n 's/^EXPECTED="\([0-9a-f]*\)"/\1/p' firmware/flash.sh | head -1)
[ "$got" = "$SHA" ] || bad "flash.sh EXPECTED ($got) != sha256($BIN) ($SHA)"
note "  1/5 flash.sh anchor: $got"

# --- 2. flash.bat 锚 / flash.bat anchor ---
got=$(sed -n 's/^set EXPECTED=\([0-9a-f]*\)/\1/p' firmware/flash.bat | head -1)
[ "$got" = "$SHA" ] || bad "flash.bat EXPECTED ($got) != sha256($BIN) ($SHA)"
note "  2/5 flash.bat anchor: $got"

# --- 3. web-flasher 家族 / web-flasher family ---
cmp -s "$BIN" web-flasher/Read_Pico.bin || bad "web-flasher/Read_Pico.bin differs from firmware/ (repo sync missed)"
grep -q "$VER" web-flasher/index.html || bad "web-flasher/index.html badge does not carry $VER"
grep -q "\"version\": \"$VER\"" web-flasher/manifest.json || bad "web-flasher/manifest.json version != $VER"
note "  3/5 web-flasher: bin identical, badge+manifest carry $VER"

# --- 4. RELEASE_NOTES / RELEASE_NOTES ---
grep -q "$VER" RELEASE_NOTES.md || bad "RELEASE_NOTES.md does not mention $VER"
grep -q "${SHA:0:8}…" RELEASE_NOTES.md || bad "RELEASE_NOTES.md misses the bin hash prefix ${SHA:0:8}…"
SIZE_CSV=$(printf "%'d" "$SIZE")   # 4,070,704（千分位口径）/ thousands-separated form
grep -q "$SIZE B" RELEASE_NOTES.md || grep -q "$SIZE_CSV B" RELEASE_NOTES.md \
  || bad "RELEASE_NOTES.md misses the bin size $SIZE B"
grep -q "${APKSHA:0:8}…" RELEASE_NOTES.md || bad "RELEASE_NOTES.md misses the APK hash prefix ${APKSHA:0:8}…"
note "  4/5 RELEASE_NOTES: $VER, ${SHA:0:8}…, ${SIZE}B, apk ${APKSHA:0:8}…"

# --- 5. apk README 尺码 / apk README size (KB, rounded) ---
KB=$(( (APKSIZE + 1023) / 1024 ))
grep -q "${KB} KB" tools/android/apk/README.md || bad "apk/README.md does not state ${KB} KB (actual ${APKSIZE}B)"
note "  5/5 apk README: ${KB} KB"

# --- 6. apk 发布副本 / apk release copy ---
if [ -f apk/ReadPicoPush.apk ] && cmp -s "$APK" apk/ReadPicoPush.apk; then
  note "  6/6 apk release copy: identical to build output"
else
  bad "apk/ReadPicoPush.apk missing or differs from build output $APK (release copy stale)"
fi

# --- 7. 固件自证版本串 + 验收 README 判据第一条（26.9.6 起，审计裁定③每版必修）---
# 由来：PROJECT_VER 此前只活在 CMakeLists.txt，固件运行期无处可查，round-3 复验因此
# 无法区分「新版在跑但修复无效」与「旧版还在跑」——两者在日志上行为完全一致，结论
# 只能停在「未改善、不可归因」，白花一轮推理。此后每版必须自证，并与 APK 的 0x09
# 装锚对齐：先证明基线，再谈行为。
# Why: PROJECT_VER used to live only in CMakeLists.txt with nothing printing it, so the
# round-3 re-verification could not tell "the new build runs but the repair does not work"
# from "the old build is still running" - identical in the log, so the honest verdict was
# "no improvement, not attributable" and a whole round went to reasoning. Every release
# must now state itself, mirroring the APK's 0x09 anchor: prove the baseline first.
grep -q '"fw: %s"' main/app_main.c \
  || bad "main/app_main.c does not print the fw: version line (firmware must self-identify)"
grep -q 'esp_app_get_description()' main/app_main.c \
  || bad "main/app_main.c does not read the app version (esp_app_get_description)"
SHORT=${VER##*pico}
README=${ACCEPTANCE_README:-"../README-${SHORT}.md"}
# 26.9.7：一次性广播有窗口期（本轮那句正好落在启动突发里丢了），所以每行日志自带
# 短码前缀；门禁同检查「代码里还在打」与「README 里钉住了这个短码」。
# 26.9.7: a one-shot broadcast has a window (this round's line drowned in the boot
# burst), so every line now carries the short code; the gate checks both that the code
# still emits it and that this generation's README pins that code.
TAGBODY=${SHORT#26.}
# 26.9.10：短码 = minor*10+patch（与 fw_tag 同款定点编码；26.9.10→100，26.9.9→99）。
# 旧的 "${TAGBODY//./}" 字符串拼接在 patch 两位时会得 910，与 fw_tag 的数值编码不符。
# 26.9.10: code = minor*10+patch (same fixed-point encoding as fw_tag; 26.9.10->100).
# The old "${TAGBODY//./}" concat yielded 910 for a two-digit patch, diverging from fw_tag.
CODE="[$(( ${TAGBODY%%.*} * 10 + ${TAGBODY##*.} ))]"
# 26.9.7 补洞（审计裁定②-2）：fw_tag() 解析 PROJECT_VER 失败会打印 `[??]`，每行日志
# 失去身份——那是假绿入口。门禁必须证明 PROJECT_VER 能被 fw_tag() 同款 sscanf
# (`%*[^-]-pico%d.%d.%d`) 解析出 minor.patch，且解析出的短码正好等于 CODE。解析不出
# 直接判红：不允许「不明就看起来不明」的构建出货。
# 26.9.7 hole-closer (adjudication 2-2): if fw_tag() cannot parse PROJECT_VER it prints
# `[??]` and every line loses its identity - a fake-green entry. The gate must prove
# PROJECT_VER parses to minor.patch under fw_tag()'s own sscanf, and that the parsed
# code equals CODE; a parse failure fails outright - no "looks-unknown-on-purpose" build
# may ship.
PV=$(sed -n 's/.*set(PROJECT_VER "\([^"]*\)").*/\1/p' CMakeLists.txt | tail -1)
[ -n "$PV" ] || bad "CMakeLists.txt has no literal PROJECT_VER (fw_tag would emit [??] on every line)"
# 复刻 fw_tag 的解析：取 -pico 之后的 minor.patch，按**数值** minor*10+patch 得短码数字。
# Reproduce fw_tag's parse: take minor.patch after -pico and encode it numerically as
# minor*10+patch (the same fixed-point form fw_tag prints).
PM=$(printf '%s' "$PV" | sed -n 's/.*-pico[0-9]*\.\([0-9]*\)\.\([0-9]*\).*/\1/p')
PP=$(printf '%s' "$PV" | sed -n 's/.*-pico[0-9]*\.\([0-9]*\)\.\([0-9]*\).*/\2/p')
[ -n "$PM" ] && [ -n "$PP" ] || bad "PROJECT_VER '$PV' does not parse under fw_tag()'s sscanf (would ship [??])"
PARSED=$((PM * 10 + PP))
CODE_INNER=${CODE#[}; CODE_INNER=${CODE_INNER%]}   # [97] -> 97
[ "$PARSED" = "$CODE_INNER" ] || bad "PROJECT_VER '$PV' parses to [$PARSED], not $CODE (fw_tag mismatch -> [??] would ship)"
note "  fw_tag parses PROJECT_VER '$PV' -> $CODE (no [??] can ship)"
grep -q '"%s dual:' main/app/app_loop.c \
  || bad "main/app/app_loop.c no longer prefixes the dual line with the fw short code"
grep -q 'fw_tag()' main/app/app_loop.c \
  || bad "main/app/app_loop.c lost fw_tag() (the per-line firmware identity)"
# 26.9.7 观测件 schema 锚定（审计裁定②）：验收要靠这些字段读判，任一被悄悄改名/删除，
# 真机日志就读不出结论。门禁钉住格式串里的关键 token——去掉即红，不靠自觉。
# **先剥注释再查**：这些 token 在解释性注释里也会出现（`lift0=1` 就踩过——裸 grep 命中
# 注释、删了代码仍报绿，是「锚点不唯一」的同类假绿）。剥注释后只查真正的代码。
# 26.9.7 observation schema anchor (adjudication 2): acceptance reads these fields, so
# a silent rename/removal would make the device log unreadable. The gate pins the key
# tokens in the format strings - remove one and it goes red.
# **Strip comments first**: these tokens also appear in explanatory comments (`lift0=1`
# bit us - a bare grep matched the comment and stayed green after the code was deleted,
# the same fake-green as a non-unique anchor). Check only real code.
if python3 - <<'EOF'
import re, pathlib, sys
s = pathlib.Path('main/app/app_loop.c').read_text()
code = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
code = '\n'.join(re.sub(r'//.*$', '', l) for l in code.split('\n'))
toks = ['dual abort: reason=', 'saw_zero=', 'freeze_n=', '" lift0=1"', 'end=%s']
miss = [t for t in toks if t not in code]
if miss:
    print('FAIL: main/app/app_loop.c lost observation token(s) ' + ', '.join(repr(m) for m in miss) + ' (acceptance cannot read the log)')
    sys.exit(1)
sys.exit(0)
EOF
then
    note "  observation schema: dual / dual abort + reason/end/saw_zero/freeze_n/lift0 all present (comments stripped)"
else
    fail=1
fi
# 26.9.9 诊断取证 schema：0x07 解码后的 word/def、点词缓存命中与否必须可读——这两行是
# 「错配在 APK 还是设备解码」的唯一现场；改掉任一字段名就等于把证据删了。同样先剥注释。
# 26.9.9 diagnostics schema: the decoded word/def of 0x07 and the tap cache verdict are
# the only scene separating "the APK sent it wrong" from "the device decoded it wrong".
if python3 - <<'EOF'
import re, pathlib, sys
s = pathlib.Path('components/push_sink/src/push_sink.c').read_text()
code = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
code = '\n'.join(re.sub(r'//.*$', '', l) for l in code.split('\n'))
toks = ['ai rx: op=07 len=', 'decode=FAIL head=', 'wlen=', 'dlen=', 'def_head=',
        'ai tap: cache=HIT', 'ai tap: cache=MISS', 'klen=']
miss = [t for t in toks if t not in code]
if miss:
    print('FAIL: components/push_sink/src/push_sink.c lost diagnostic token(s) '
          + ', '.join(repr(m) for m in miss) + ' (0x07 attribution evidence gone)')
    sys.exit(1)
sys.exit(0)
EOF
then
    note "  ai diag schema: rx(len/decode/wlen/word/dlen/def_head) + tap(cache/klen/key) present (comments stripped)"
else
    fail=1
fi
if [ -f "$README" ]; then
  grep -q "fw: $VER" "$README" \
    || bad "$README acceptance item 1 does not pin the boot string \"fw: $VER\""
  grep -qF "$CODE dual:" "$README" \
    || bad "$README does not pin the per-line short code \"$CODE dual:\""
  note "  7/7 firmware self-id: fw: $VER + $CODE prefix, $README pins both"
else
  bad "acceptance README not found: $README (set ACCEPTANCE_README to override)"
fi

if [ "$fail" -eq 0 ]; then
  note "deliverable family check: PASS (7/7 in sync)"
else
  note "deliverable family check: FAIL (see above) - 出包/重锚前必须全绿 / must be all green before packaging or re-anchoring"
fi
exit "$fail"
