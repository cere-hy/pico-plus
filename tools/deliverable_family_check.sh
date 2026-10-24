#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# 交付物家族五件对表（第五道固定检查，2026-10-24 gen26.3 基装轮立）。
# The deliverable-family five-item reconciliation (the fifth fixed check,
# established in the 2026-10-24 gen26.3 base-install round).
#
# 存在的理由 / Why this exists:
#   gen26.3 基装轮一次抓出三处仓库侧陈旧（firmware/ 旧 bin、flash.bat 锚停在
#   gen22、RELEASE_NOTES gen22 残留）+ 一处漏同步（manifest.json）——不是巧合，
#   是每代交付物家族有同步义务但没有对表。凡出包或重锚动作，这五件必须过一遍：
#   In the gen26.3 base-install round one sweep caught three stale repo-side
#   artifacts (firmware/ old bins, flash.bat's anchor frozen at gen22,
#   RELEASE_NOTES gen22 residue) plus one missed sync (manifest.json) - not a
#   coincidence: every generation owes the deliverable family a sync, but no
#   checklist existed. Any packaging or re-anchoring action must run all five:
#
#   1. firmware/flash.sh   EXPECTED == sha256(firmware/Read_Pico.bin)
#   2. firmware/flash.bat  EXPECTED == 同上（Windows 路径，曾三代没动）
#   3. web-flasher/        Read_Pico.bin 与 firmware/ 逐字节一致；
#                          index.html 徽章 + manifest.json 版本 == PROJECT_VER
#   4. RELEASE_NOTES.md    头部版本 == PROJECT_VER；bin 尺码/哈希前缀、APK
#                          哈希前缀与实物一致
#   5. apk/README.md       所述 APK 尺码与实物一致（KB 取整口径）
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

if [ "$fail" -eq 0 ]; then
  note "deliverable family check: PASS (5/5 in sync)"
else
  note "deliverable family check: FAIL (see above) - 出包/重锚前必须全绿 / must be all green before packaging or re-anchoring"
fi
exit "$fail"
