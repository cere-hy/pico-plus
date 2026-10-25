#!/bin/sh
# 生产版烧写脚本（无 instrumentation，第②层 + 其余六项用这个）/ Flash script for the PRODUCTION build
#
# 用法 / Usage:
#   sh flash.sh /dev/ttyUSB0        # Linux / macOS
#   sh flash.sh COM3                # Windows (Git Bash / MSYS2)
#
# 本板没有 BOOT / RESET 物理键，esptool 的 --before default-reset 会自动进下载模式，
# 不需要按任何键。连不上先换端口、再拔插 USB。
# The board has no BOOT/RESET button; esptool's --before default-reset enters download mode
# automatically. If it fails, try another port or replug USB.
#
# 波特率：默认 460800；若烧录中途报错/校验不过，降到 230400 重试：
#   sh flash.sh /dev/ttyUSB0 230400
# Baud: 460800 by default; on mid-flash errors or checksum failures, drop to 230400:
#   sh flash.sh /dev/ttyUSB0 230400
#
# 警告 / WARNING: Read_Pico.bin 必须落在 0x10000。
#                 写到 0x0 会覆盖 bootloader / 分区表 / NVS / phy_init。
#                 Read_Pico.bin MUST go to 0x10000; 0x0 would overwrite the
#                 bootloader, partition table, NVS and phy_init.
#
# 四个偏移对应 partitions_16M.csv 的 5.5MB app 布局：ota_0 在 0x10000、
# ota_1 在 0x590000，故 otadata 落在 0xb10000。不是上游默认的 4MB 布局。
# These offsets match our partitions_16M.csv 5.5MB app layout: ota_0 at 0x10000,
# ota_1 at 0x590000, hence otadata at 0xb10000 - not upstream's 4MB default.
#
# 烧前请把完整输出存盘（esptool 的哈希/摘要行是烧录证据）：
#   sh flash.sh /dev/ttyUSB0 2>&1 | tee flash-prod-$(date +%Y%m%d-%H%M%S).log
# Save the FULL output (esptool hash lines are the flashing evidence).

set -e
PORT="${1:-/dev/ttyUSB0}"
BAUD="${2:-460800}"
DIR="$(cd "$(dirname "$0")" && pwd)"

# 锚定值 = gen14 production（修复 gen13 阻断级推送失败：BEGIN 帧被拒 + 手机端
#   STATUS 超时；版本串 bump 至 0.3.3-pico14）。
#   根因（gen13 真机串口日志 + NimBLE 源码逐行实证 + 第三方 BLE 调试助手交叉验证）：
#   双 bug 叠加——
#   (1) CONFIG_BT_NIMBLE_ATT_MAX_PREP_ENTRIES=4（sdkconfig.defaults:100）太小。
#       手机与设备 MTU 协商失败时（有效 MTU 23），83 字节 BEGIN 由客户端走
#       Prepare Write 长写，每包 20 字节需 5 个 prep entry；即使协商成功，
#       241 字节 DATA 也需 13 个。队列第 5 个 os_memblock_get 返回 NULL，
#       ble_att_svr_insert_prep_entry 打 rc=6（BLE_HS_ENOMEM）——失败只打
#       LOGD，release 构建不可见，真机只表现「写了但没反应」。gen13 真机日志
#       485E 第一周期 149 秒后 E (402329) ble_att_svr_insert_prep_entry rc=6
#       实锤（第三方助手连接、未协商 MTU、手动写 BEGIN 触发的长写）。
#   (2) notify 空口失败路径全部静默：ble_gatts_notify_custom 返回 0 只代表
#       HCI 命令入队，真正的空口结果在 BLE_GAP_EVENT_NOTIFY_TX 的 status；
#       notify_status 的 rc!=0 与 NOTIFY_TX status!=0 过去都只打 LOGD。
#       gen13 真机：485E 第二周期（APK，MTU 247、BEGIN 一次写完、552208
#       一次 notify initiated）手机端 20 秒等不到 STATUS；同一台设备换
#       第三方 BLE 调试助手手动发合法 83B BEGIN（token=0x485E、title「Test」），
#       助手「收: 0」——两个独立客户端表现一致，指向设备侧 notify 通路。
#   gen14 修法：
#   - sdkconfig.defaults / sdkconfig / sdkconfig.ci：MAX_PREP_ENTRIES 4→16
#     （覆盖 241B DATA 长写的 13 个 entry + 3 余量），MSYS_1_BLOCK_COUNT
#     12→20（每个 prep entry 从 MSYS_1 池领一个 mbuf，16 个 entry 需 16 块）。
#   - push_sink.c：notify 失败全链路可见 + 重投——notify_status 的 mbuf 领取
#     失败与 rc!=0 升级 LOGW 并排 esp_timer 100ms 重发 s_last_status（每连接
#     周期 3 次配额）；NOTIFY_TX status!=0 升级 LOGW 并排重投；DISCONNECT
#     撤配额防 timer 野发（cancel 只翻旗，避免与 timer 回调的 s_lock 死锁）。
#   - push_sink.c 诊断日志：BEGIN/DATA/CTRL 每个拒绝点打印 len、头 4 字节、
#     token/type/total——真机一测便知 decode 败在哪个检查（gen13 静态对表
#     全绿却无法归因，补此日志）。
#   - MainActivity.java：MTU 协商失败（<32）明确失败重试，不闷头走长写；
#     waitStatus 接受 RESUME 帧（offset 0 按 READY 继续，非 0 提示重新完整
#     发送——gen13 只等 READY/ERR，RESUME 被丢弃白等 20 秒）。
#   协议两侧逐字节对表复核（用户假设②排除）：PushProto.java encodeBegin 与
#   push_proto.c push_begin_decode 在 magic/op/ver/type/token/total/crc/sid/
#   title[64] 的偏移与小端序上完全一致，25/25 宿主对表 + 168 项 host test 全绿；
#   用户假设①的「max_len 不够」修正：ESP-IDF NimBLE 的 ble_gatt_chr_def 没有
#   max_len 字段，实质限制是 prep 队列深度（修法见上）。
#   gen13 = 03e59a07…（广播回归修复轮，真机验证广播/扫描/连接/MTU 247 全通，
#   但推送全链止于 BEGIN 被拒）改名留档为 Read_Pico.bin.gen13-03e59a07，勿烧。
#   gen12 = 6e1d15c1…（GATT 注册时序修复轮；注册修复有效但广播回归不可扫描）
#   改名留档为 Read_Pico.bin.gen12-6e1d15c1，勿烧。
#   gen11 = 6490999f…（二期 M1 完整轮，真机连接 OK 但 Services 空白）改名留档为
#   Read_Pico.bin.gen11-6490999f，勿烧。
#   gen10 = 9e573c84…（r95 rebase 落定轮，真机轮已 PASS）改名留档为 Read_Pico.bin.gen10-
#   9e573c84，勿烧（不含 M1）。
#   gen9 = a0bbc7a3…（#121 字体弹回修复轮，真机轮已 PASS）留档，勿烧（底座 r93，缺 r95 全部改动）。
#   gen8 = 2610ce20…（缺陷发现轮，已烧设备）留档，勿烧。
#   gen7 = ace5373b…（修复前，G7-A1 前）留档，勿烧。
# Anchor = gen14 production (fixes the gen13 blocking push failure: the BEGIN
# frame refused and the phone's STATUS wait timing out; version bumped to
# 0.3.3-pico14). Root cause (gen13 device serial log + NimBLE source audit +
# cross-validation with a third-party BLE debugger): two stacked bugs -
# (1) CONFIG_BT_NIMBLE_ATT_MAX_PREP_ENTRIES=4 was too small. When the MTU
# exchange fails (effective MTU 23), the 83-byte BEGIN goes out as a Prepare
# Write long write needing 5 prep entries at 20 bytes each; even with a
# successful exchange a 241-byte DATA needs 13. The 5th os_memblock_get
# returned NULL and ble_att_svr_insert_prep_entry logged rc=6
# (BLE_HS_ENOMEM) - the failure only logged at LOGD, invisible in release, so
# hardware just showed "written but no response". The gen13 log line
# E (402329) ble_att_svr_insert_prep_entry rc=6 (149 s into the first 485E
# cycle: third-party debugger, no MTU exchange, manual BEGIN) proves it.
# (2) Every notify on-air failure path was silent: a zero return from
# ble_gatts_notify_custom only means the HCI command was queued, the real air
# result being NOTIFY_TX's status; both notify_status's rc!=0 and NOTIFY_TX
# status!=0 used to log at LOGD only. On gen13 hardware: the second 485E cycle
# (APK, MTU 247, BEGIN written in one shot, one notify at 552208) left the
# phone waiting 20 s for STATUS; the same device driven by a third-party BLE
# debugger with a legal 83 B BEGIN (token=0x485E, title "Test") showed
# "received: 0" - two independent clients, one symptom: the device-side
# notify path. gen14 fixes:
# - sdkconfig.defaults / sdkconfig / sdkconfig.ci: MAX_PREP_ENTRIES 4->16
#   (covers the 13 entries of a 241 B DATA long write plus slack) and
#   MSYS_1_BLOCK_COUNT 12->20 (each prep entry takes one MSYS_1 mbuf; 16
#   entries need 16 blocks).
# - push_sink.c: visible notify failures plus retries - mbuf-allocation
#   failure and rc!=0 in notify_status escalate to LOGW and schedule an
#   esp_timer 100 ms re-send of s_last_status (3 attempts per connection
#   cycle); NOTIFY_TX status!=0 escalates to LOGW and schedules a retry;
#   DISCONNECT drops the quota so the timer cannot fire into the next cycle
#   (cancel flips a flag only, avoiding a deadlock with the timer callback's
#   s_lock).
# - push_sink.c diagnostics: every BEGIN/DATA/CTRL refusal prints len, the
#   first 4 bytes and token/type/total - one real-hardware run then tells
#   which check rejected the frame (gen13's static tables all agreed yet the
#   refusal could not be attributed; this log fills the gap).
# - MainActivity.java: a failed MTU exchange (<32) fails explicitly for a
#   retry instead of silently entering a long write; waitStatus accepts the
#   RESUME frame (offset 0 continues as READY, non-zero prompts a full
#   resend - gen13 waited for READY/ERR only, dropped RESUME and idled out
#   the full 20 s).
# Byte-for-byte protocol cross-check (user hypothesis 2 dismissed):
# PushProto.java's encodeBegin and push_proto.c's push_begin_decode agree on
# every field offset and little-endian order (magic/op/ver/type/token/total/
# crc/sid/title[64]); 25/25 host table tests and 168 host-test checks pass.
# User hypothesis 1 corrected: ESP-IDF NimBLE's ble_gatt_chr_def has no
# max_len field - the real limit was the prep-queue depth (fixed above).
# gen13 = 03e59a07… (adv-regression fix round; hardware verified adv/scan/
# connect/MTU 247 all working, but the push chain stopped at the refused
# BEGIN) archived as Read_Pico.bin.gen13-03e59a07 - do NOT flash.
# gen12 = 6e1d15c1… (GATT registration-timing round; the fix worked but the
# adv regression made the device unscannable) archived as
# Read_Pico.bin.gen12-6e1d15c1 - do NOT flash. gen11 = 6490999f… (full phase-2
# M1 round; hardware connected but Services blank) archived as
# Read_Pico.bin.gen11-6490999f - do NOT flash. gen10 = 9e573c84… (r95 rebase
# round, hardware PASS) archived as Read_Pico.bin.gen10-9e573c84 - do NOT
# flash (no M1). gen9 = a0bbc7a3… (#121 font-snap-back round, hardware PASS)
# archived, do NOT flash (r93 base, missing all r95 rebase changes). gen8 =
# 2610ce20… (defect-discovery round, burned) archived, do NOT flash. gen7 =
# ace5373b… (pre-fix) archived.
#
# 版本串 / Version string: 0.3.3-pico26.5（gen26.5 M2 Step10 TTS 听书：设备侧页文本 +
# 0x19 STATUS 帧 + 面板朗读进度 + NVS 落盘 / APK 重组朗读流 + FGS 跟随 + AI 联动；
# 每代 fork 专属可区分串，沿用旧串会让 TF 卡升级判「版本相同」而拒绝）。
# Version string: 0.3.3-pico26.5 (gen26.5 M2 Step10 TTS audiobook: device-side page
# text + 0x19 STATUS frame + panel read-along progress + NVS persist / APK reassembled
# read stream + FGS follow + AI linkage; fork-specific distinguishable tag - reusing an
# older tag makes the TF-card upgrade reject the package as "same version").
# 体积：4,085,040 B（相对 gen26.4.1 的 4,071,680 B **+13,360 B**，归因 = TTS 页文本
# 缓冲 + 0x19 STATUS 通道 + 面板朗读进度/NVS 落盘；段级闭合待真机 boot 日志 seg1 双锚
# 对表确认）；sha256 3c7892f5…；重锚 2026-10-25。
#
# 版本串 / Version string: 0.3.3-pico26.4.1（gen26.4 真机 defect 热修：词典卡「AI」
# 入口的自引用别名——调用方传 dict_popup_word()（= &s_word 自身），show_pending 入口
# 先 reset 再查空把别名参数一起清空，查询态建不起来、词典卡被清空、0x07 回来被门 A
# 丢弃 = 词典命中词的 AI 释义全丢。修 show_pending/open 入口先拷贝再 reset +
# app_book 取词落局部缓冲；APK v1.4.2 同轮补导入重名去重）。沿用旧串会让 TF 卡升级
# 判「版本相同」而拒绝，故每代 fork 专属可区分串。
# Version string: 0.3.3-pico26.4.1 (gen26.4 device-defect hotfix: a self-referential
# alias in the dict card's "AI" entry - a caller passing dict_popup_word() (= &s_word
# itself) had that argument cleared by show_pending's reset-first-then-check entry,
# so the query state never formed, the dict card was wiped, and 0x07 was dropped by
# gate A = every dict-hit word's AI definition lost. Fixed by copying before the
# reset in show_pending/open plus a local-buffer copy in app_book; APK v1.4.2 adds
# import dedup in the same round). Reusing an older tag makes the TF-card upgrade
# path reject the package as "same version", so each generation gets its own
# fork-specific distinguishable tag).
#
# 段表锚（esptool image-info，与 gen26.4 对照）：
#   seg1 DRAM  Load addr(vaddr)=0x3fc9c700 世代不变锚（逐代相同）；
#   seg1 len=0x08a8c（与 gen26.4 持平）；File offs=0x00234418（与 gen26.4 持平，
#   seg0 无增长）。真机 boot 日志预测锚：seg1 paddr = 0x00234418 + 8(段头) +
#   0x10000(ota_0 基址) = 0x00244420（与 gen26.4 同值——用户实测日志
#   `segment 1: paddr=00244420` 与预测逐字吻合 ✓，公式再添一代实证）。
#   体积：4,071,680 B，相对 gen26.4（4,071,616 B）**+64 B**（双操作数：
#   4,071,680 − 4,071,616 = 64），段级闭合：seg3(IROM)+64（0x18caa8→0x18cae8，
#   show_pending/open 的 own 拷贝 + app_book wbuf 局部缓冲），seg0/seg1/其余持平，
#   尾注差 0——分项净和 = 总差，逐分闭合。
#   bootloader 21,232 B（哈希 c8069ead…，gen26.4 为 e7ab5130…；bootloader 带构建
#   时间戳，哈希每代必变，零行为变化）；
#   partition-table / ota_data_initial 与 gen26.4 同尺寸同内容（sha256 逐字节一致：
#   9d5d890b… / 7d2c7ac4…）。
# Segment anchor (esptool image-info, vs gen26.4): seg1 DRAM Load addr (vaddr) =
# 0x3fc9c700 generation-invariant; seg1 len = 0x08a8c (flat vs gen26.4);
# File offs = 0x00234418 (flat vs gen26.4, no seg0 growth). Predicted hardware
# boot-log anchor: seg1 paddr = 0x00234418 + 8 (segment header) + 0x10000 (ota_0
# base) = 0x00244420 (same value as gen26.4 - the user's live log
# `segment 1: paddr=00244420` matched the prediction character for character,
# one more generation of empirical confirmation).
# Size: 4,071,680 B, **+64 B** vs gen26.4 (4,071,616 B) - both operands:
# 4,071,680 − 4,071,616 = 64 - closed segment by segment: seg3 (IROM) +64
# (0x18caa8->0x18cae8, the own[] copies in show_pending/open plus app_book's
# wbuf local buffer), seg0/seg1/the rest flat, tail-note delta 0 - the
# per-item net equals the total delta, closed item by item.
# bootloader 21,232 B (hash c8069ead…, gen26.4 was e7ab5130…; the bootloader
# embeds a build timestamp so its hash changes every generation with zero
# behavior change); partition-table / ota_data_initial same size and content as
# gen26.4 (sha256 byte-identical: 9d5d890b… / 7d2c7ac4…).
#
# 重锚记录（2026-10-22，gen24.4：推送/传书界面渲染发灰根治——跨页进入武装页与
#   推送收官开书强制一次整屏 GC16 擦旧底，gen24.3 真机验收全绿后报的观感缺陷，
#   gen24.3 的第四枚热修，仍不并 gen25）：
#   版本串：0.3.3-pico24.4（CMakeLists.txt 一处 + 本记录 + 烧录说明，三处同步）。
#   背景（gen24.3 真机 5 项验收全绿后）：「接收推送」页【切换充电时常开】按钮文字
#     模糊发灰、背景隐约有旧图案残影（时钟轮廓）；推送完成后打开第一页也有残影。
#     用户裁定（现象判断+修改要求）：从首页进入「接收推送」页时没有触发全刷，上一页
#     的时钟轮廓叠在现在的按钮上。修改：跨页面进入此视图时强制触发一次全屏刷新
#     （GC16）擦除旧底，保证文字纯黑、底色纯白，绝不允许用低质量局部快刷；整个
#     传书/推送界面清晰度第一，不追求极致快刷。
#   根因（查实）：跨 app 切页唯一出口 app_loop.c:336
#     `app_present(ctx, next, next->enter_full ? APP_REDRAW_FULL : APP_REDRAW_PAGE)`，
#     app_transfer 与 app_book 的 .enter_full 均为 false → 拿 APP_REDRAW_PAGE；
#     app_transfer 的 present() 改前首行 `if (redraw != APP_REDRAW_AREA) return
#     false;` → 落回 app_loop.c:226-230 的 `render + update_display_fast_page()`
#     （GL16 差分快刷，display.c:501-532，每 12 次切页才附带一次 GC16）。首页月相
#     钟的深墨被 GL16 差分留着，透出叠在按钮上。开书侧：app_book present 对
#     APP_REDRAW_PAGE+READING 的落点是 update_display_mode_diff(MODE_GL16)
#     （app_book.c:3853 改前），武装页灰底卡片墨迹同样留着——app_transfer.c:1137
#     原注释「全刷落在 app_book 的开书路径」对跨 app 进入不成立（全刷分支只认
#     APP_REDRAW_FULL/s_reader_cleanup），本次修正该路径。
#     用户方向 1（清屏不彻底）与方向 3（图层重叠）经查证不成立：draw_push 首行
#     ui_clear_page(fb) 全帧清底，武装页无叠加图层；残影是面板物理残留，根因是
#     刷新波形（方向 2）。
#   修改一（app_transfer.c，武装页落地强制 GC16，循 app_dashboard.c:920
#     s_home_force_full_once 范式）：新增 s_push_entry_full_once；置位点 =
#     push_arm_now()（页内点按武装，方法页同样带旧墨）与 on_enter 的
#     push_sink_armed() 保活分支（18-a 手机回连弹回）；on_enter 入口清零防陈旧
#     标记；present() 消费——非 APP_REDRAW_AREA 且标记在 → render +
#     update_display_full（display.c:572-581：E0470_WAVEFORM+MODE_GC16+full，
#     整屏 GC16 非差分），一次即止；标记不在 → 原样 return false 落 app_loop
#     快刷。只打武装页落地：其余视图/页面呈现路径逐字节不变。倒计时活区更新仍走
#     既定 AREA 通道（墨没动就不刷、动只重绘活区，gen19 防闪烁裁定不动摇）；
#     静态按钮文字像素在差分中不被重驱，全刷后纯黑文字保持纯净。
#   修改二（app_book.c，推送收官开书第一页 GC16）：新增
#     app_book_request_open_clean()（内部走 app_book_request_open + 置位
#     s_open_full_once，头文件 app_content_open.h 同步声明）；present 全刷分支扩
#     条件 `redraw == APP_REDRAW_FULL || s_reader_cleanup || s_open_full_once`
#     → update_display_full，一次即止；app_transfer.c push_tick 开书交接改调
#     clean 变体。只服务推送收官：书架/首页开书（_open/_from_home/_resume）不
#     置位，翻页节拍零变化。
#   测试：transfer_ui_host_test.c 补 app_book_request_open_clean 桩 +
#     push_arm_regression 断言 test_book_open_clean（收官必须走 clean 变体）；
#     ui_refresh_feedback_host_test.py 补 s_open_full_once 声明 + present 路由
#     断言（APP_REDRAW_PAGE 默认 READER 差分、标记置位 → FULL/MODE_GC16 且一次
#     即止）。变异验证两处均有牙：换回普通开书变体 → transfer_ui SIGABRT；删
#     present 分支的 || s_open_full_once → ui_refresh_feedback 断言失败。
#     宿主 14/14 runner 全绿。
#   门禁：fullclean（IDF v6.1.0）exit 0；8 警告同基线（全 -Wunused-function：
#     app_book.c 7 + ui_pinpad.c 1，均不在改动面）；宿主测试 14/14 runner 全绿。
#   终版锚：Read_Pico.bin 4,063,488 B（相对 gen24.3 4,063,296 **+192 B**，双操作数
#     4,063,488−4,063,296=192——两处 present 分支与两个静态标志/API 的代码增量），
#     sha256 `ab2a7a51d7f9f5679edc16a16b7efad138f114168b010075d9b7098e78b78a52`；
#     read-pico-gen24.4.zip 完整 sha256 见交付账本（记忆文件）——不在此自引，
#     避免「zip 内含自身哈希」的循环依赖。
#     seg1 File offs `0x00233da8`（esptool 实测，与 gen24.3 持平——DROM 段未动），
#     paddr = 0x00233da8 + 8(段头) + 0x10000(ota_0 基址) = `0x00243db0`（预测，
#     与 gen24.3 同值）；seg1 Load addr 0x3fc9c700 世代不变锚不动，len=0x08a8c
#     持平。partition-table / ota_data_initial 与 gen24.3 逐字节一致（9d5d890b… /
#     7d2c7ac4…）。bootloader sha 由 `d4b82937…` → `8e7f7866…`（体积同为 21,232 B；
#     差异是编译时间戳，IDF 产物非确定性，行为不变）。APK 包内为 v1.1
#     `7e5eea37…`（与 gen24.3 同，协议未变，无需重刷）。
#
# 重锚记录（2026-10-22，gen24.3：gen24.2 真机验收（a–j 全绿）观感组三项 UI/交互
#   修复——首页推送卡误触 + 蓝牙总开关文案溢出 + 文件管理传输按钮排版，gen24.2 的
#   第三枚热修，仍不并 gen25）：
#   版本串：0.3.3-pico24.3（CMakeLists.txt 一处 + 本记录 + 烧录说明，三处同步）。
#   问题 1（严重，首页推送卡误触）：control() 与 on_gesture 的 TAP 分支均先判继续
#     阅读卡、后判推送卡，而两命中矩形 Y 轴重叠 16px（[558,574)），该带点按被当
#     开书。修复：continue_card_hit_rect 下沿 574→566（{24,172,636,394}）、
#     PUSH_CARD_HIT 上沿 558→566（{24,566,636,74}）——两区在 y=566 干净相邻、
#     零重叠；可见卡与派发顺序均不变。
#   问题 2（蓝牙总开关文案溢出）：setting_toggle 的 detail 单行绘制不换行（起点
#     x=59、字号 19≈570px），原 30 字串冲出屏幕右侧并压住开关轨（x=555）。定稿
#     20 字「开启可使用翻页器与手机推送；关闭将释放蓝牙」≈380px，终点 x≈439。
#   问题 3（文件管理传输按钮排版，用户裁定修订）：蓝牙不能传书，二期 M1（提交
#     ba892c3）加的第 4 张「手机推送/蓝牙发送」卡多余且卡宽收窄后右沿 676 冲出
#     内容框 28px。删除第 4 卡，恢复 gen10 原版 3 卡排版：home_transfer_rect 恢复
#     {36+index*208,346,196,100}（x=36/244/452，[36,648] 与内容框对称居中），
#     methods/details 去第 4 项，渲染与命中 loop i<4→i<3。
#   功能入口可达性（放行硬门槛）：首页推流状态卡走 app_transfer_request_method_picker()
#     独立路径（app_dashboard.c:1129-1141），与文件管理按钮的 app_transfer_request_push()
#     是两条路；删按钮后首页入口原样保留。app_transfer_request_push 失去生产调用方，
#     但 transfer_ui 宿主测试（push_arm_regression 等 206/297/343 行）直调该函数，
#     故保留定义 + 注释（公共 API、非 static，无 -Wunused-function 风险）。
#   测试连带（诚实注记，非「零测试改动」）：
#     (1) reading_origin_host_test.py 自 gen18 起静默腐坏（deep-link 分支进
#         app_dashboard.c 后桩未跟进：缺 app_desc_t/app_transfer/
#         app_transfer_request_method_picker 桩与 request_app 字段、提取列表缺
#         push_card_pressed 与 PUSH_CARD_RECT/HIT 宏），且不在任何 run_*.sh
#         门禁（13/13 从未覆盖它）。本代修复：补桩 + 宏从源码正则提取（单一真源）
#         + 遍历边界 y<574→y<566 + 推流卡带 [566,640) 全网格点按不误开书且
#         deep-link 只跳转断言 + 去 macOS-ism 的 dir='/private/tmp'；新增
#         tools/run_reading_origin_host_test.sh 纳入门禁（13→14 runners）。
#         变异验证：把两矩形改回重叠态，测试 SIGABRT（有牙）。
#     (2) file_sd_format_host_test.py 以传输命中 loop 原文作提取区间终点，4→3 后
#         定位串同步更新（机械连带，该测试不断言按钮数组）。
#   门禁：fullclean（IDF v6.1.0）exit 0；8 警告同基线（全 -Wunused-function：
#     app_book.c 7 + ui_pinpad.c 1，均不在改动面）；宿主测试 14/14 runner 全绿
#     （reading_origin 新纳入；transfer_ui 全套含 push_arm/push_gate/
#     push_pending_ui 三回归；push_sink 168/0；app_loop 55 场景；book_management
#     PASS）。
#   终版锚：Read_Pico.bin 4,063,296 B（相对 gen24.2 4,063,392 **−96 B**，双操作数
#     4,063,296−4,063,392=−96），sha256
#     `2cbac7d9e7639cbf1f03aec2f713971a1e1709cf0ecbde2fc78091cbf3284b07`；
#     read-pico-gen24.3.zip 完整 sha256 见交付账本（记忆文件）——不在此自引，
#     避免「zip 内含自身哈希」的循环依赖。
#     seg1 File offs `0x00233da8`（esptool 实测，gen24.2 为 0x00233de8，−0x40=−64B
#     顺移——文案缩短与按钮字符串删除使 DROM 段收缩），paddr = 0x00233da8 + 8(段头)
#     + 0x10000(ota_0 基址) = `0x00243db0`（预测）；seg1 Load addr 0x3fc9c700 世代
#     不变锚不动，len=0x08a8c 持平。partition-table / ota_data_initial 与 gen24.2
#     逐字节一致（9d5d890b… / 7d2c7ac4…）。bootloader sha 由 `9597e307…` →
#     `d4b82937…`（体积同为 21,232 B；差异是编译时间戳，IDF 产物非确定性，行为不变）。
#     APK 包内为 v1.1 `7e5eea37…`（与 gen24.2 同，协议未变，无需重刷）。
#
# Re-anchor record (2026-10-22, gen24.3: three UI/interaction fixes from gen24.2's
#   hardware acceptance (a-j all green) look-and-feel group - the home push card's
#   mis-tap, the BLE master switch's overflowing detail text, and the file
#   manager's transfer button layout; gen24.2's third hotfix, still not merging
#   gen25):
# version 0.3.3-pico24.3 (three places in step: CMakeLists.txt, this record, the
# flashing notes).
#   Problem 1 (severe, home push card mis-tap): both control() and on_gesture's
#     TAP branch test the continue-reading card before the push card, and the two
#     hit rects overlapped by 16px on the Y axis ([558,574)), so a tap in that
#     band opened the book. Fix: continue_card_hit_rect's lower edge 574 -> 566
#     ({24,172,636,394}), PUSH_CARD_HIT's upper edge 558 -> 566 ({24,566,636,74}) -
#     the two areas now abut exactly at y=566 with zero overlap; the visible cards
#     and dispatch order are unchanged.
#   Problem 2 (BLE master switch text overflow): setting_toggle's detail is drawn
#     on one line without wrapping (start x=59, 19px font, ~570px), and the old
#     30-char string ran off the screen's right edge and over the switch track
#     (x=555). The final 20 chars "开启可使用翻页器与手机推送；关闭将释放蓝牙" are
#     ~380px, ending at x~439.
#   Problem 3 (file manager transfer buttons, user-revised ruling): Bluetooth
#     cannot transfer books, so the 4th "phone push / Bluetooth send" card added
#     by phase-2 M1 (commit ba892c3) was redundant, and after the cards narrowed
#     its right edge hit 676, 28px past the content box. The 4th card is removed
#     and the gen10 3-card layout is restored: home_transfer_rect returns to
#     {36+index*208,346,196,100} (x=36/244/452, [36,648] symmetric with the
#     content box), methods/details lose their 4th entries, render and hit loops
#     go i<4 -> i<3.
#   Entry reachability (the hard gate): the home push status card takes the
#     separate app_transfer_request_method_picker() path (app_dashboard.c:
#     1129-1141); the file manager button's app_transfer_request_push() was a
#     different path. With the button gone the home entry is untouched.
#     app_transfer_request_push loses its production caller, but the transfer_ui
#     host tests (push_arm_regression etc., lines 206/297/343) call the function
#     directly, so the definition is kept with a comment (a public API, not
#     static, so no -Wunused-function risk).
#   Test follow-ons (honest note - not "zero test changes"):
#     (1) reading_origin_host_test.py had silently rotted since gen18 (after the
#         deep-link branch entered app_dashboard.c its stubs were never updated:
#         missing app_desc_t/app_transfer/app_transfer_request_method_picker
#         stubs and the request_app field, and push_card_pressed plus the
#         PUSH_CARD_RECT/HIT macros were absent from the extraction), and it sat
#         outside every run_*.sh gate (the 13/13 never covered it). This
#         generation repairs it: stubs added, macros regex-extracted from the
#         source (single source of truth), the traversal bound y<574 -> y<566,
#         a full-grid push-card band [566,640) tap asserting no book opening and
#         deep-link-navigates-only, and the macOS-ism dir='/private/tmp' dropped;
#         tools/run_reading_origin_host_test.sh brings it under the gate (13 -> 14
#         runners). Mutation check: restoring the overlapping rects makes the
#         test SIGABRT (it has teeth).
#     (2) file_sd_format_host_test.py used the transfer hit loop's text as its
#         extraction span's end marker; the 4 -> 3 change updates that locator in
#         step (a mechanical follow-on; the test asserts no button array).
#   Gate: fullclean (IDF v6.1.0) exit 0; 8 warnings at baseline (all
#     -Wunused-function: app_book.c 7 + ui_pinpad.c 1, neither in the changed
#     surface); host tests 14/14 runners green (reading_origin newly under the
#     gate; transfer_ui's full set including the push_arm/push_gate/
#     push_pending_ui regressions; push_sink 168/0; app_loop 55 scenarios;
#     book_management PASS).
#   Final anchors: Read_Pico.bin 4,063,296 B (vs gen24.2 4,063,392 **-96 B**, both
#     operands 4,063,296-4,063,392=-96), sha256
#     `2cbac7d9e7639cbf1f03aec2f713971a1e1709cf0ecbde2fc78091cbf3284b07`;
#     read-pico-gen24.3.zip's full sha256 lives in the delivery ledger (the
#     memory file) - not self-referenced here, to avoid the "zip contains its own
#     hash" circular dependency.
#     seg1 File offs `0x00233da8` (esptool measured; gen24.2 was 0x00233de8,
#     -0x40=-64B shift - the shortened text and removed button strings shrank the
#     DROM segment), paddr = 0x00233da8 + 8 (segment header) + 0x10000 (ota_0
#     base) = `0x00243db0` (predicted); seg1 Load addr 0x3fc9c700 is the
#     generation-invariant anchor, len=0x08a8c flat. partition-table /
#     ota_data_initial byte-identical to gen24.2 (9d5d890b… / 7d2c7ac4…).
#     bootloader sha `9597e307…` -> `d4b82937…` (same 21,232 B; the difference is
#     the build timestamp - IDF artifacts are non-deterministic, behavior
#     unchanged). APK is v1.1 `7e5eea37…` (same as gen24.2, protocol unchanged,
#     no reflash needed).
#
# 重锚记录（2026-10-22，gen24.2：推送按钮状态机静默死锁根治——前置四门 + B 模型
#   开关权威化 + 行内【开启】按钮 + deep-link 澄清，gen24.1 的第二枚热修，仍不并 gen25）：
#   版本串：0.3.3-pico24.2（CMakeLists.txt 一处 + 本记录 + 烧录说明，三处同步）。
#   背景（gen24.1 真机两轮 + 531 行 boot 日志，日志首行 paddr=00243af0 与 gen24.1
#     预测锚逐字吻合，确认测的就是 gen24.1）：问题 1——设置里关「蓝牙总开关」后进
#     传书页点「接收推送」，首次能收；点「退出接收」再点「手机推送」直接死锁无反应。
#     问题 2——开机不开蓝牙点「接收推送」毫无反应、无文字提示。问题 3——关设置里的
#     蓝牙开关后回首页，「接收推送」仍在工作且能正常收，但点「退出接收」再点手机
#     推送即失效（无提示）。代码级闭环：三题同根——待武装（pending）路径自 gen12
#     起没有 UI 存在感，app_loop 拉栈条件漏算它，10s 超时事件又烂死在无人消费的
#     方法页，全程零提示（日志实证：死锁瞬间 largest=31744 内存健康，卡死在权限与
#     状态路径，不是内存；gen24.1 的卸书自愈未动）。
#   根因三环：R1 app_loop.c:1093 sync 条件 `(ble_turner || push_sink_armed())`
#     漏算 pending（pending 时 state 恰是 IDLE）；R2 push_tick 把 pending 当会话
#     结束，500ms 内闪回方法页；R3 方法页 tick 直接 APP_REDRAW_NONE，start_failed/
#     closed 永无人消费。对用户「某处检查 app_settings_ble() 后默默 return」的假设
#     予以修正：push_button_action 全链根本不读设置开关，开关是经 R1 的 sync 条件
#     silently 掐死二次武装的。
#   权限模型裁定 B（用户拍板）：设置开关 = 蓝牙总开关（权威），同时管翻页器与手机
#     推送；gen18 的「武装入口可能就是翻页器关着时点的」注释随 deep-link 自动武装
#     一并修订。用户另拍板两项：行内快捷开关做（门 2 提示条内嵌【开启】，原地写设置
#     项并继续推送，不跳设置页）；deep-link 只跳转不武装（gen23 裁定兑现）。
#   实现八块：
#     1. B 模型落地：设置项更名「蓝牙总开关」，文案定稿「开启后可连接翻页器并使用
#        手机推送；关闭后两者均停用并释放蓝牙内存」；无线组行标签「蓝牙翻页器」→
#        「蓝牙」；settings.h 注释同步；app_loop.c gen18 注释按 B 模型修订。
#     2. 中途关开关即断：设置页 toggle off handler 与 app_loop 快捷层
#        UI_QUICK_BLUETOOTH off handler 两处追加 push_sink_cancel_why("蓝牙已关闭")
#        （覆盖 armed 与 pending 两态，首页状态卡自然回「未开启」）。
#     3. 前置四门（push_button_action）：内存 gate（既有两段式，gen24.1 D1/D2/D3
#        原样）→ 蓝牙总开关（行内提示 + 【开启】按钮）→ 飞行模式（「飞行模式开启中，
#        请先关闭」）→ USB 存储（「USB 传书已接管，接收推送暂停」）；R1 sync 条件
#        不改——pending 只在开关开启时发生，sync 条件恒真，缺口自然消解。
#     4. R2 pending 语义：push_sink 新增 push_sink_pending() 查询；push_tick 回退
#        条件改为「非 armed 且非 pending」；pending 期间推送页显示「正在启动蓝牙…」
#        （draw_push starting 分支，状态卡/副行/窗口行三处文案）。
#     5. R3 结构性根修：方法页 tick 消费一次性事件（methods_tick：start_failed/
#        closed 落 s_method_message + 重绘）；pending 超时补 ESP_LOGW 仪表行；
#        极端闭环 pending 中关开关 → cancel_why → closed → 方法页显示「蓝牙已关闭」。
#     6. 分支 10 收摊原因：on_enter 取 push_sink_last_close_why() 快照入
#        s_last_cancel_why（内存态，不做 NVS），提示条挂「上次接收已结束：X」，
#        下一次武装清除（push_arm_now 新回合干净开局）。
#     7. 分支 1 禁用提示：CONNECTED/RECEIVING/PROCESSING 点按钮 →
#        「手机正在推送，完成后自动结束」，不再静默 return。
#     8. deep-link 澄清修正：on_enter 的 s_push_requested 从 else-if 链解耦——
#        只清 flag 不武装；armed 保活中落武装页由 push_sink_armed 分支接住
#        （不重新武装、不重置窗口）。
#   违规窗口（诚实注记）：自动武装埋点早于本 fork 代际编号，源自提交 ba892c3
#     「武装页与入口——虚拟书打开、单词卡、四卡横排」（二期 M1，彼时版本串上游基
#     0.3.3-rc95）；该分支当时即注释「手机推送入口：直接武装」（push_sink_arm(0,
#     PUSH_ENTRY_WINDOW_MS)）；gen19（3c77610）重构成 push_arm_now() 沿用至今。
#     即 deep-link 自功能出生即为「跳转 + 自动武装」，历经 fork gen22→gen23→gen24
#     →gen24.1 从未修正；gen23 所立「首页只跳转不武装」裁定从立起那天起代码就
#     从未合规——属静默违反，非某代回归引入。gen20–24.1 各轮验收面的
#     「deep-link 首页跳传书页」项实际验证的一直是「跳转 + 自动武装」，本次修正
#     后该项才首次按裁定真实验收。
#   行内【开启】按钮要点（用户钉死）：单一真源（与设置页读写同一 settings 值）；
#     点按写设置项 → 重走四门（门 2 已过，内存/飞行/USB 复查）→ 过则既有武装路径
#     （pending 拉栈，不引入第二套启动路径）；提示条瞬态（点按后「正在启动蓝牙…」，
#     失败回对应门提示）；仅门 2 出现；竞态由同源 + 复查自然收敛。
#   测试：transfer_ui 新增 push_gate_regression（门 2 提示 + 行内点按写设置重走
#     四门 + 飞行/USB 拦截 + pending 中关开关闭环 + 块 6 底账）与
#     push_pending_ui_regression（pending 不闪回 + 方法页消费 start_failed/closed
#     + 查序）；桩新增 test_settings_ble_turner/app_settings_set_ble_turner/
#     app_settings_flight_mode/push_sink_pending/test_push_pending；cancel_why 桩
#     补 pending 分支与 CANCEL 回 IDLE（对齐真实组件）；push_arm_regression 五处
#     deep-link 依赖同步改显式点按（宿主断言即实现契约，教训同 gen19/gen21）。
#     app_loop 补快捷层关蓝牙 cancel 联动断言（既有 BLE toggle 样本上加断言）。
#   门禁：fullclean（IDF v6.1.0）exit 0；8 警告同基线（全 -Wunused-function：
#     app_book.c 7 + ui_pinpad.c 1，不在改动面）；transfer_ui PASS（含 gen24.2 两
#     新回归）；push_sink 168/0；app_loop 55 场景 PASS；book_management PASS；
#     run_*.sh 全量枚举 13/13 绿。
#   终版锚：Read_Pico.bin 4,063,392 B（相对 gen24.1 4,061,584 **+1,808 B**，
#     双操作数 4,063,392−4,061,584=1808），sha256 `41f5d596fe83a2edb0b5c16beebab03f4f2597fa578ad871b0535c0a1f386b1d`；
#     read-pico-gen24.2.zip 完整 sha256 见交付账本（记忆文件）第 19 节——不在此自引，
#     避免「zip 内含自身哈希」的循环依赖；14 条目：1 根目录 + 3 目录 + 10 文件。
#     seg1 File offs `0x00233de8`（esptool 实测，gen24.1 为 0x00233ae8，+0x300=768B
#     顺移——DROM 段与文案增长所致），paddr = 0x00233de8 + 8(段头) + 0x10000(ota_0
#     基址) = `0x00243df0`（预测）；seg1 Load addr 0x3fc9c700 世代不变锚不动，
#     len=0x08a8c 持平。partition-table / ota_data_initial 与 gen24.1 逐字节一致
#     （9d5d890b… / 7d2c7ac4…）。bootloader sha 由 `0dcba57a…` → `9597e307…`
#     （体积同为 21,232 B；差异是编译时间戳，IDF 产物非确定性，行为不变）。
#     APK 包内为 v1.1 `7e5eea37…`（与 gen24.1 同，协议未变，无需重刷）。
#
# Re-anchor record (2026-10-22, gen24.2: the push-button state machine's silent
#   deadlock rooted out - front four gates + model-B switch authority + the
#   inline [enable] button + deep-link clarification; gen24.1's second hotfix,
#   still not merging gen25):
# version 0.3.3-pico24.2 (three places in step: CMakeLists.txt, this record, the
# flashing notes).
#   Background (gen24.1 hardware, two rounds + 531-line boot log; the log's first
#   line paddr=00243af0 matches gen24.1's predicted anchor character for
#   character, confirming the gen24.1 build under test): problem 1 - with the
#   BLE master switch off in Settings, the first "receive push" tap works, but
#   after "exit receive" the next "phone push" tap deadlocks with zero response.
#   Problem 2 - booting without enabling Bluetooth, the tap does nothing and
#   shows no text. Problem 3 - after switching Bluetooth off in Settings, the
#   home "receive push" keeps working and can still receive, but "exit receive"
#   then a phone push fails silently. Code-level closure: all three share one
#   root - the pending-arm path has had no UI presence since gen12, app_loop's
#   stack-pull condition never counted it, and the 10 s timeout event rotted in
#   a method page nobody consumed: zero hints throughout (log proof: at the
#   deadlock largest=31744, memory healthy - it jammed on the permission and
#   state paths, not memory; gen24.1's leave-the-book self-heal untouched).
#   Three-link root cause: R1 app_loop.c:1093's sync condition
#   `(ble_turner || push_sink_armed())` never counts pending (a pending arm's
#   state is exactly IDLE); R2 push_tick treats pending as a finished session
#   and bounces back within 500 ms; R3 the method page's tick returns
#   APP_REDRAW_NONE outright, so start_failed/closed are never consumed. The
#   user's hypothesis ("some path checks app_settings_ble() and returns
#   silently") is corrected: push_button_action never reads the setting at all;
#   the switch kills the second arm silently THROUGH R1's sync condition.
#   Model ruling B (user-decided): the settings switch is the BLE master switch
#   (authoritative), governing both the page-turner and phone push; gen18's
#   "the arm entry may have been tapped with the page-turner setting off" note
#   is revised along with the deep-link auto-arm it described. Two further user
#   rulings: build the inline quick switch (gate-2 hint embeds [enable], writes
#   the setting in place and continues, no settings-page detour); the deep link
#   navigates only, never arms (gen23's ruling paid off).
#   Eight implementation blocks:
#     1. Model B lands: the setting is renamed "bluetooth master switch" with
#        the final wording "on: connect the turner and use phone push; off:
#        both stop and the memory is returned"; the wireless-group row label
#        becomes "bluetooth"; settings.h's comment follows; app_loop.c's gen18
#        note is revised under model B.
#     2. Mid-flight switch-off cuts: both the settings-page toggle-off handler
#        and app_loop's quick-layer UI_QUICK_BLUETOOTH off handler append
#        push_sink_cancel_why("bluetooth off") (covering armed and pending;
#        the home status card naturally returns to "off").
#     3. Front four gates (push_button_action): memory gate (the existing
#        two-part wording, gen24.1 D1/D2/D3 verbatim) -> BLE master switch
#        (inline hint + [enable] button) -> flight mode ("flight mode is on,
#        turn it off first") -> USB storage ("USB transfer owns the card,
#        receive paused"); R1's sync condition unchanged - a pending arm only
#        exists with the switch on, so the condition holds and the gap closes
#        naturally.
#     4. R2 pending semantics: push_sink gains push_sink_pending(); push_tick's
#        fallback tightens to "neither armed nor pending"; while pending the
#        push page shows "starting bluetooth..." (draw_push's starting branch,
#        three文案 spots: status card, sub-line, window line).
#     5. R3 structural root fix: the method page's tick consumes the one-shot
#        events (methods_tick: start_failed/closed land in s_method_message
#        with a repaint); the pending timeout gains an ESP_LOGW instrumentation
#        line; the extreme loop "switch off during pending -> cancel_why ->
#        closed -> the method page shows bluetooth off" closes.
#     6. Branch 10 close-up reason: on_enter snapshots
#        push_sink_last_close_why() into s_last_cancel_why (in-memory, no NVS),
#        the hint bar shows "last receive ended: X", and the next arm clears it
#        (push_arm_now's fresh-round slate).
#     7. Branch 1 disabled hint: CONNECTED/RECEIVING/PROCESSING taps say "the
#        phone is pushing; it ends automatically" instead of returning silently.
#     8. Deep-link clarification: on_enter's s_push_requested decouples from
#        the else-if chain - it only clears the flag and never arms; a
#        keep-alive arm still lands on the arm page via the push_sink_armed
#        branch (no re-arm, no window reset).
#   Inline [enable] button points (user-pinned): single source of truth (the
#   same settings value as the settings page); tap writes the setting, re-runs
#   the four gates (gate 2 passed, memory/flight/USB rechecked), and a full
#   pass takes the existing arm path (pending pulls the stack; no second start
#   path); the hint is transient ("starting bluetooth..." after the tap, back
#   to the refusing gate on failure); it appears only at gate 2; races converge
#   naturally through the shared source plus the recheck.
#   Tests: transfer_ui gains push_gate_regression (gate-2 hint + inline tap
#   writing the setting and re-running the gates + flight/USB refusals + the
#   switch-off-during-pending loop + the block-6 ledger) and
#   push_pending_ui_regression (pending holds + the method page consumes
#   start_failed/closed + the check order); the stubs gain
#   test_settings_ble_turner/app_settings_set_ble_turner/
#   app_settings_flight_mode/push_sink_pending/test_push_pending; the
#   cancel_why stub gains the pending branch and CANCEL-to-IDLE (mirroring the
#   real component); push_arm_regression's five deep-link dependencies switch
#   to explicit taps (a host assertion is an implementation contract - the
#   same lesson as gen19/gen21); app_loop asserts the quick-layer switch-off
#   cancel linkage (on the existing BLE-toggle sample).
#   Gate: fullclean (IDF v6.1.0) exit 0; 8 warnings at baseline (all
#   -Wunused-function: app_book.c 7 + ui_pinpad.c 1, outside the change set);
#   transfer_ui PASS (with gen24.2's two new regressions); push_sink 168/0;
#   app_loop 55 scenarios PASS; book_management PASS; run_*.sh full enumeration
#   13/13 green.
#   Final anchor: Read_Pico.bin 4,063,392 B (**+1,808 B** vs gen24.1's
#   4,061,584 - both operands: 4,063,392 − 4,061,584 = 1808), sha256
#   `41f5d596…`; seg1 File offs `0x00233de8` (esptool-measured; gen24.1 was
#   0x00233ae8, +0x300 = 768B shift from DROM growth and longer strings),
#   paddr = 0x00233de8 + 8 (segment header) + 0x10000 (ota_0 base) =
#   `0x00243df0` (predicted); seg1 Load addr 0x3fc9c700 generation-invariant,
#   len = 0x08a8c flat. partition-table / ota_data_initial byte-identical to
#   gen24.1 (9d5d890b… / 7d2c7ac4…). bootloader sha `0dcba57a…` → `9597e307…`
#   (same 21,232 B; the difference is the build timestamp - IDF artifact
#   non-determinism, behavior unchanged).
#   The package's APK remains v1.1 `7e5eea37…` (same as gen24.1; protocol
#   unchanged, no reflash needed).
#
# 重锚记录（2026-10-22，gen24.1：gen24 真机两项缺陷热修——D2 冤拒 + D1 文案，独立代）：
#   版本串：0.3.3-pico24.1（CMakeLists.txt 一处 + 本记录 + 烧录说明，三处同步）。
#   背景（gen24 真机两轮，用户实证 + 688 行 boot 日志）：开两本大书后点首页
#     「接收推送」出「内存碎片不足，需重新启动」，但恢复链全落空——① 不重启、
#     设置内关开蓝牙无效；② 重启后仍无法开启，须进设置关蓝牙再开才生效；补测：
#     单独关蓝牙或单独重启电源均无效，必须「关闭蓝牙再重启」。代码级闭环定性
#     gen24 自身两笔缺陷 + 一笔仪表缺口：
#   - D2 冤拒（gen24 引入）：push_button_action() 预检无条件执行。boot1 实证
#     BLE 栈自开机即 up，碎片窗口内点按钮仍被闸门拦下——但闸门 ble_pt_mem_gate
#     的语义是预测「NimBLE 能否**启动**」（控制器+栈任务必须落内部 RAM），栈已
#     up 时武装无需新控制器分配。gen24.1 起恢复 gen23 语义：!ble_pt_running()
#     才预检先行；栈 up 跳过预检直接 reset+arm。
#   - D1 虚假承诺（gen24 引入）：文案「重启设备后可用」被真机证伪——失败锁
#     （tries+why）落 NVS flash（rp_blestate namespace），ble_pt_reset_failure()
#     是唯一清点（设置页/快捷层关蓝牙触发：app_device_settings.c:2135 +
#     app_loop.c:700），重启本身不清锁；且重启后开书几秒内 largest 再破闸
#     （boot2：70.4s 开书 → 76.2s 跌至 19456）。用户定稿两段式：「内存碎片不足
#     （最大块 19K，需 20K）——退出当前书籍后重试；若仍失败，请重启设备」——
#     先卸书（boot1 实证：离开书籍回首页 largest 自行恢复 19456→31744，预检
#     自愈无需重启），再兜底重启；「重启不清锁」不再提（自愈按钮自动清锁）。
#   - D3 仪表缺口（gen24 漏）：按钮预检失败不打日志，真机分不清「按钮冤拒」
#     与「启动被拒」。补 ESP_LOGW 带数字（free/largest/闸门/psram），与 UI 同源。
#   实现（外科式四项，main/apps/app_transfer.c + tools/transfer_ui_*）：
#   1. D2：push_button_action() 预检加 !ble_pt_running() 条件（栈 down 才检）。
#   2. D1：s_method_message 缓冲 96→160（两段式约 114B，96 装不下）；draw_methods
#      横幅按「——」拆两行（cy=608/638）；无分隔符的单行文案仍居中单行（cy=622）
#      行为不变。
#   3. D3：预检失败分支补
#      `transfer_page: push precheck refused: internal %u B free / %u B largest
#       (gate %u K), psram %u B free`。
#   4. 测试：transfer_ui 桩加 test_ble_running/ble_pt_running（默认 false=栈
#      down，保住既有三姿态断言）；push_multistate_regression 增姿态 3a'
#      （未锁+栈 down+19K → 两段式文案四断言：退出当前书籍/请重启设备/19K/20K，
#      无 arm 无 reset）与 3a''（栈 up+19K → 跳过闸门直接武装：arm+1、reset+1、
#      无消息）。
#   门禁：fullclean（IDF v6.1.0）exit 0；8 警告同基线（全 -Wunused-function：
#     app_book.c 7 + ui_pinpad.c 1，均不在本代改动面）；transfer_ui host test
#     PASS（含 gen24.1 新分支）；push_sink 168/0；app_loop 55 场景 PASS；
#     book_management PASS；tools/run_*.sh 全量枚举 13/13 绿。
#     构建确定性实证：首构（修一处 ui_text_vc 实参笔误后增量完成）与净构
#     fullclean+build 两产物仅差 68 字节、分三簇——0x74（app 描述符编译时间串
#     06:26:07 vs 06:22:55）、0xb0-0xcf（app_elf_sha256，ELF 含时间串故变）、
#     0x3df96f-0x3df98f（镜像尾部 SHA-256 摘要，实测=镜像主体 sha256，随内容变）。
#     体积与全部段锚逐字节一致，零行为差异。
#   终版锚：Read_Pico.bin 4,061,584 B（相对 gen24 4,061,360 **+224 B**，
#     双操作数 4,061,584−4,061,360=224），sha256 `eb07a4a7…`；
#     seg1 File offs `0x00233ae8`（esptool 实测，gen24 为 0x00233a48，+0xa0=160B
#     顺移——DROM 段增长所致），paddr = 0x00233ae8 + 8(段头) + 0x10000(ota_0 基址)
#     = `0x00243af0`（预测）；seg1 Load addr 0x3fc9c700 世代不变锚不动，len=0x08a8c。
#     partition-table / ota_data_initial 与 gen24 逐字节一致（9d5d890b… / 7d2c7ac4…）。
#     bootloader sha 由 `ae8c4718…` → `0dcba57a…`（体积同为 21,232 B；差异是
#     编译时间戳，属 IDF 产物非确定性，非源码改动，行为不变）。
#     APK 包内为 v1.1 `7e5eea37…`（与 gen24 同，协议未变，无需重刷）。
#
# Re-anchor record (2026-10-22, gen24.1: gen24 hardware two-defect hotfix -
# D2 false refusal + D1 wording, standalone generation):
# version 0.3.3-pico24.1 (three places in step: CMakeLists.txt, this record, the
# flashing notes).
#   Background (gen24 hardware, two rounds, user-proven + 688-line boot log):
#     after opening two large books, tapping "receive push" on the home page
#     showed "insufficient memory fragmentation, restart required", but every
#     recovery path fell flat - (1) no restart, toggling Bluetooth in Settings
#     did nothing; (2) after a restart it still would not enable, requiring
#     Settings>Bluetooth off/on; supplement: power-cycling or Bluetooth-off
#     alone both fail, only "Bluetooth off THEN restart" works. Code-level
#     closure pins two gen24-introduced defects plus one instrumentation gap:
#   - D2 false refusal (introduced by gen24): push_button_action() ran its
#     precheck unconditionally. boot1 proves the BLE stack was up since boot,
#     yet a tap inside the fragmentation window was still refused by the gate -
#     but the gate ble_pt_mem_gate predicts whether NimBLE can START (controller
#     + stack task must land in internal RAM); with the stack already up, arming
#     needs no new controller allocation. From gen24.1 gen23 semantics return:
#     precheck first only when !ble_pt_running(); stack up skips it and goes
#     straight to reset+arm.
#   - D1 false promise (introduced by gen24): "restart the device to use it"
#     was disproven on hardware - the failure lock (tries+why) persists in NVS
#     flash (rp_blestate namespace) and ble_pt_reset_failure() is the only
#     clearing point (Settings/quick-layer Bluetooth off: app_device_settings.c
#     :2135 + app_loop.c:700); a bare restart clears nothing. Worse, opening a
#     book a few seconds after restart breaks the gate again (boot2: book open
#     at 70.4s -> largest falls to 19456 by 76.2s). The user's final two-part
#     wording: "insufficient memory fragmentation (largest block 19K, needs
#     20K) - leave the current book and retry; if it still fails, restart the
#     device" - first leave the book (boot1 proves largest self-recovers
#     19456->31744 on returning home, no restart needed), then restart as
#     fallback; "restart does not clear the lock" is no longer mentioned (the
#     self-heal button clears the lock automatically).
#   - D3 instrumentation gap (missed by gen24): a button precheck failure
#     logged nothing, so hardware logs could not tell "button false refusal"
#     from "start refused". Added ESP_LOGW with numbers (free/largest/gate/
#     psram), same source as the UI hint.
#   Implementation (four surgical items, main/apps/app_transfer.c + tools/
#   transfer_ui_*):
#   1. D2: push_button_action() wraps the precheck in !ble_pt_running().
#   2. D1: s_method_message buffer 96->160 (two-part text ~114B, 96 too small);
#      draw_methods splits the banner at the em-dash into two lines (cy=608/
#      638); single-line hints without the separator stay centered one-liners
#      (cy=622), unchanged.
#   3. D3: precheck-failure branch logs
#      `transfer_page: push precheck refused: internal %u B free / %u B largest
#       (gate %u K), psram %u B free`.
#   4. Tests: transfer_ui stub gains test_ble_running/ble_pt_running (default
#      false = stack down, preserving the existing three-pose assertions);
#      push_multistate_regression adds pose 3a' (unlocked + stack down + 19K ->
#      two-part prompt four assertions: leave the book / restart the device /
#      19K / 20K; no arm, no reset) and 3a'' (stack up + 19K -> skip the gate
#      and arm directly: arm+1, reset+1, no message).
#   Gate: fullclean (IDF v6.1.0) exit 0; 8 warnings same baseline (all
#     -Wunused-function: app_book.c 7 + ui_pinpad.c 1, none in this change's
#     surface); transfer_ui host test PASS (incl. gen24.1 new branches);
#     push_sink 168/0; app_loop 55 scenarios PASS; book_management PASS;
#     tools/run_*.sh full enumeration 13/13 green.
#     Build-determinism evidence: the first build (incrementally finished after
#     fixing one ui_text_vc argument typo) and the pristine fullclean+build
#     differ in only 68 bytes, three clusters - 0x74 (app descriptor build-time
#     string 06:26:07 vs 06:22:55), 0xb0-0xcf (app_elf_sha256, changed because
#     the ELF embeds the time string), 0x3df96f-0x3df98f (image-tail SHA-256
#     digest, verified = sha256 of the image body, content-dependent). Size and
#     every segment anchor byte-identical; zero behavioral difference.
#   Final anchors: Read_Pico.bin 4,061,584 B (**+224 B** vs gen24 4,061,360;
#     both operands: 4,061,584 - 4,061,360 = 224), sha256 `eb07a4a7…`;
#     seg1 File offs `0x00233ae8` (esptool measured; gen24 was 0x00233a48,
#     +0xa0=160B shift from DROM segment growth), paddr = 0x00233ae8 + 8
#     (segment header) + 0x10000 (ota_0 base) = `0x00243af0` (predicted);
#     seg1 Load addr 0x3fc9c700 generation-invariant, unmoved; len=0x08a8c.
#     partition-table / ota_data_initial byte-identical to gen24
#     (9d5d890b… / 7d2c7ac4…).
#     bootloader sha `ae8c4718…` -> `0dcba57a…` (same 21,232 B; difference is
#     the build timestamp - IDF artifact non-determinism, not a source change;
#     behavior unchanged).
#     APK inside the package remains v1.1 `7e5eea37…` (same as gen24; protocol
#     unchanged, no reflash needed).
#
# 重锚记录（2026-10-22，gen24：真机 BLE 碎片化失败急救 + 多态按钮四态与一键自愈）：
#   版本串：0.3.3-pico24（CMakeLists.txt 一处 + 本记录 + 烧录说明，三处同步）。
#   背景（gen23 真机）：用户 31 分钟后首次点「接收推送」，NimBLE 启动失败并自锁
#     （tries≥3），屏幕「启动失败 3 次：内存不足（内部 75K，最大块 19K）」。
#     根因：start_locked() 预检 free≥56K 过、最大块 19K < 20K 闸门差 1KB——
#     内部堆**碎片化**（非净泄漏：internal_before==internal_after 零流失；
#     长时间使用积累的常驻块把大块咬碎）。gen23 固件只把既存碎片化暴露成
#     首次可见故障，定性为高置信既存问题（非实锤新增）。
#   第 1 件（多态按钮四态 + 一键自愈）：app_transfer.c push_button_action() 按
#     push_sink_state() + ble_pt_locked() 分四态——CONNECTED/RECEIVING/PROCESSING
#     禁用；非 IDLE 取消（cancel_why）；IDLE 先过内存预检**再**武装（预检不过
#     零等待出带数字提示「最大块 %uK，需 %uK，重启设备后可用」，不傻等 3 次退避）；
#     锁死态预检过则 ble_pt_reset_failure() 一键自愈后武装（reset 幂等）。
#     draw_methods 第 3 行按状态动态改词；titles/details 去掉 static（修跨调用
#     持久化 bug）。method_gesture origin==2 改调同一入口（on_enter deep-link
#     分支按裁定不动）。
#   第 2 件（预检单一真源）：ble_pt_mem_gate(free,largest,psram) 纯函数落独立 TU
#     components/ble_page_turner/src/ble_pt_mem.c（零 IDF 依赖，宿主测试直接
#     include）；阈值宏 BLE_PT_MIN_* 搬进 ble_page_turner.h 公开（UI 提示与
#     预检引用同一数字）。新增 ble_pt_locked() RAM 锁态查询（比「锁死」宽：
#     预检失败也置位）。
#   第 3 件（诊断日志）：app_transfer.c on_enter 打
#     `transfer_page: on_enter internal free=%u largest=%u psram free=%u largest=%u`；
#     app_book.c free_book() 出入口各一行 before/after——给 gen25 根治三层路线
#     （数据层/分配卫生层/书模块池化层）攒真机数据。
#   第 4 件（不假死底线）：预检失败再点只提示不挂死；on_tick 与其他按钮不受影响。
#   门禁：fullclean exit 0，8 警告同基线（全 -Wunused-function：app_book.c 7 +
#     ui_pinpad.c 1，均不在本代改动面）；push_sink 168/0；transfer_ui host test
#     含 gen24 新回归（gate 边界 7 项 + 四姿态 + 不假死）；顺手修复两个**既存**
#     红灯——transfer_ui gen19 断言脱节（nav 不收摊语义漏同步，躺了 gen20-23
#     四代）与 app_loop 宿主链接失败（gen21 起 push_sink_cancel_why 桩漏定义，
#     躺三代；已补桩并加 gen21 守门用例：武装期手动息屏静默取消武装一次，
#     自动锁屏被武装阻断且不取消）。
#   终版锚：Read_Pico.bin 4,061,360 B（相对 gen23 4,060,480 **+880 B**，
#     双操作数 4,061,360−4,060,480=880），sha256 `b1a46ff2…`；
#     seg1 File offs `0x00233a48`（esptool 实测，gen23 为 0x00233888，+0x1c0=448B
#     顺移——DROM 段增长所致），paddr = 0x00233a48 + 8(段头) + 0x10000(ota_0 基址)
#     = `0x00243a50`（预测）；seg1 Load addr 0x3fc9c700 世代不变锚不动，len=0x08a8c。
#     partition-table / ota_data_initial 与 gen23 逐字节一致（9d5d890b… / 7d2c7ac4…）。
#     bootloader sha 由 `164ac958…` → `ae8c4718…`（体积同为 21,232 B；差异是
#     编译时间戳，属 IDF 产物非确定性，非源码改动，行为不变）。
#     APK 包内为 v1.1 `7e5eea37…`（与 gen23 同，协议未变，无需重刷）。
#
# Re-anchor record (2026-10-22, gen24: hardware BLE-fragmentation emergency fix
# plus the multistate button with one-key self-healing):
# version 0.3.3-pico24 (three places in step: CMakeLists.txt, this record, the
# flashing notes).
#   Background (gen23 hardware): the user's first "receive push" tap came 31
#     minutes in; NimBLE failed to start and self-locked (tries>=3), screen
#     showing "start failed 3 times: out of memory (internal 75K, largest 19K)".
#     Root cause: start_locked()'s precheck passes free>=56K but the largest
#     block 19K misses the 20K gate by 1KB - internal heap FRAGMENTATION (not a
#     net leak: internal_before==internal_after, zero loss; long-session
#     resident blocks shred the large block). gen23 merely exposed a pre-existing
#     fragmentation as a first-visible failure: high-confidence pre-existing
#     issue, not a proven new regression.
#   Item 1 (multistate button, four states + one-key self-heal): app_transfer.c
#     push_button_action() branches on push_sink_state() + ble_pt_locked() -
#     CONNECTED/RECEIVING/PROCESSING disabled; non-IDLE cancels (cancel_why);
#     IDLE runs the memory precheck BEFORE arming (on failure, zero-wait message
#     with the numbers "largest %uK, need %uK, reboot to use" instead of waiting
#     out three backoffs); locked state that passes the precheck self-heals via
#     ble_pt_reset_failure() then arms (reset is idempotent). draw_methods row 3
#     rewords per state; titles/details drop static (fixing cross-call
#     persistence). method_gesture origin==2 routes through the same entry (the
#     on_enter deep-link branch stays as ruled).
#   Item 2 (single source of the precheck): ble_pt_mem_gate(free,largest,psram)
#     as a pure function in its own TU components/ble_page_turner/src/ble_pt_mem.c
#     (zero IDF dependency, host tests include it directly); the BLE_PT_MIN_*
#     thresholds moved into ble_page_turner.h (UI hints and the precheck quote
#     the same numbers). New ble_pt_locked() RAM lock-state query (wider than
#     "locked": a failed precheck also sets it).
#   Item 3 (diagnostics): app_transfer.c on_enter logs
#     `transfer_page: on_enter internal free=%u largest=%u psram free=%u largest=%u`;
#     app_book.c free_book() logs one line before and after - data for gen25's
#     three-layer root-cause route (data layer / allocation hygiene / book-module
#     pooling).
#   Item 4 (never-hang floor): a failed precheck re-tap only messages; on_tick
#     and other buttons stay live.
#   Gate: fullclean exit 0, 8 warnings same as baseline (all -Wunused-function:
#     app_book.c 7 + ui_pinpad.c 1, none inside this generation's change
#     surface); push_sink 168/0; transfer_ui host test with the new gen24
#     regression (7 gate-boundary cases + four postures + never-hang). Two
#     PRE-EXISTING red lights fixed in passing: transfer_ui's gen19 assertion
#     drift (the nav no-teardown semantics missed the test sync, dormant
#     gen20-23) and the app_loop host link failure (push_sink_cancel_why stub
#     undefined since gen21, dormant three generations; stub added plus a gen21
#     gatekeeper: armed manual sleep disarms silently exactly once, armed auto
#     lock stays blocked and never cancels).
#   Anchors: Read_Pico.bin 4,061,360 B (vs gen23's 4,060,480, i.e. +880 B),
#     sha256 b1a46ff2...; seg1 File offs 0x00233a48 (measured; gen23 was
#     0x00233888, +0x1c0 = 448 B shift from DROM growth), paddr = offs + 8 +
#     0x10000 = 0x00243a50 (predicted); seg1 load addr 0x3fc9c700 remains the
#     generation-invariant anchor, len 0x08a8c. partition-table / ota_data_initial
#     byte-identical to gen23 (9d5d890b / 7d2c7ac4). bootloader sha 164ac958 ->
#     ae8c4718 (same 21,232 B; the delta is the build timestamp - IDF output is
#     not deterministic; no source change, no behavior change). APK is v1.1
#     7e5eea37 (same as gen23; protocol unchanged, no reflash needed).
#
# 重锚记录（2026-10-22，gen23：第 3 项共享伽马 LUT —— 本代唯一源码增量）：
#   版本串：0.3.3-pico23（CMakeLists.txt 一处 + 本记录 + 烧录说明，三处同步）。
#   范围裁定：第 1 项 AA 开关早已在既有代际内完成（本代零新增 diff）；第 2 项用户确认
#     拒绝、不在任何发布路径；故 gen23 = 第 3 项共享 LUT 的唯一代际产物。
#   第 3 项内容：新增 main/ui/ui_gray_lut.{h,c}（256 项幂律表，单一真源
#     UI_GRAY_GAMMA 默认 0.5f，编译期可 -D 覆盖）。字体侧删掉私有的 s_cover 与
#     ttf_cover_lut_init()，mix_ink() 与字形 mask 改调 ui_gray_lut_map()；图片侧
#     ui_image_dither_gray() 在 Bayer 量化前把亮度翻成墨量、过共享伽马再翻回亮度
#     （入参口径相反，必须翻转），中灰 128 由 119–136 变深到 68–85，端点 0/255 守恒。
#     按范围，cover_bw / bw / acrylic_bw 三条二值点阵路径**一行未动**。
#   断言语义（image_animation_host_test.c 同步后，待补句落此）：测的是「伽马组合后的
#     量化行为」——输出仍是 17 的倍数（物理级）不变，但断言由「level*17 原样通过」的
#     恒等契约改为「落在伽马提升后灰度的两个相邻物理级之一」+ 端点守恒 + 单调性；
#     恒等契约因伽马重映射已失效，故删除。
#   终版锚：Read_Pico.bin 4,060,480 B（相对 gen22 终版 4,060,496 **−16 B**，
#     双操作数 4,060,480−4,060,496=−16），sha256 `b7cf63a0…`；
#     seg1 File offs `0x00233888`（esptool 实测，与 gen22 同——本代改动落 IROM
#     代码段 seg3，DROM 段不变，故 DRAM 段 seg1 偏移不动），
#     paddr = 0x00233888 + 8(段头) + 0x10000(ota_0 基址) = `0x00243890`（预测，与 gen22 同）；
#     seg1 Load addr 0x3fc9c700 世代不变锚不动，len=0x08a8c。
#     partition-table / ota_data_initial 与 gen22 逐字节一致（9d5d890b… / 7d2c7ac4…）。
#     bootloader sha 由 `73b9a556…` → `164ac958…`（体积同为 21,232 B；37 B 差异是
#     编译时间戳 Oct 22 2026 07:08:34，属 IDF 产物非确定性，非源码改动，行为不变）。
#     APK 包内为 v1.1 `7e5eea37…`（同时独立就位于 v1.0 Release 附件，可单独取用）。
#   CI：源码经 run 39642877598（head 8b687cb）双 job 全绿验证；**gen23 bin 未过 CI**——
#     CI 验证的是当时的源码树，本 bin 为本地 IDF v6.1.0 全量重建产物。
#
# Re-anchor record (2026-10-22, gen23: item 3 shared gamma LUT - this generation's
# only source increment): version 0.3.3-pico23 (three places in step: CMakeLists.txt,
# this record, the flashing notes). Scope: item 1 (AA switch) was already finished
# inside an earlier generation (zero new diff here); item 2 was declined by the user
# and is on no release path; so gen23 is exactly the item-3 shared-LUT generation.
#   Item 3: new main/ui/ui_gray_lut.{h,c} - a 256-entry power LUT with a single source
#     UI_GRAY_GAMMA defaulting to 0.5f and overridable with -D. The font side drops its
#     private s_cover and ttf_cover_lut_init(); mix_ink() and glyph masks now call
#     ui_gray_lut_map(). The image side flips brightness into ink space, applies the
#     shared gamma and flips back before the Bayer quantiser (the argument polarity is
#     opposite, so the flip is required): mid gray 128 moves from 119-136 down to
#     68-85, with the 0/255 endpoints pinned. Per scope, the three binary dither paths
#     cover_bw / bw / acrylic_bw were left untouched.
#   Assertion semantics (after syncing image_animation_host_test.c): the test now
#     covers post-gamma quantisation - outputs are still multiples of 17, but the
#     "level*17 passes through unchanged" identity contract was replaced by "lands on
#     one of the two adjacent physical levels of the gamma-lifted gray", plus endpoint
#     pinning and monotonicity. The identity contract cannot survive gamma remapping.
#   Anchors: Read_Pico.bin 4,060,480 B (vs gen22's 4,060,496, i.e. -16 B), sha256
#     b7cf63a0...; seg1 File offs 0x00233888 (measured, same as gen22 - this change
#     lands in IROM seg3 while DROM is unchanged, so DRAM seg1 does not shift),
#     paddr = offs + 8 + 0x10000 = 0x00243890 (predicted, same as gen22); seg1 load
#     addr 0x3fc9c700 remains the generation-invariant anchor, len 0x08a8c.
#     partition-table / ota_data_initial byte-identical to gen22 (9d5d890b / 7d2c7ac4).
#     bootloader sha 73b9a556 -> 164ac958 (same 21,232 B; the 37 B delta is the build
#     timestamp - IDF output is not deterministic; no source change, no behavior change).
#     APK is v1.1 7e5eea37 (also published standalone in the v1.0 release assets).
#   CI: the source passed run 39642877598 (head 8b687cb) with both jobs green; the
#     gen23 binary itself did not go through CI - CI verified the source tree, while
#     this bin is a local full rebuild on IDF v6.1.0.
#
# 重锚记录（2026-10-20，gen21 真机验收全绿后的发布前打磨轮 / gen22）：
#   版本串：0.3.3-pico22（CMakeLists.txt 一处 + 本记录 + 烧录说明，三处同步）。
#   用户 gen21 真机八项验收面全绿，发布前提出三项，一次做完：
#   ① push 虚拟书专属封面：push.txt 无内嵌封面，按路径哈希落到深色版式（黑底白字
#     占位观感），与带内嵌封面的 epub 放一起很突兀。新增第 4 版式 push_delivery
#     （纸纹浅底 + 「手机→电波→书」图形 + 底部书名），沿用现有灰阶原语，不加图片
#     资源。缓存版本必须同步递增，否则旧结果一直命中：BOOK_AUTO_COVER_VERSION 4→5、
#     FAST_COVER_REVISION 1→2。
#   ② 时钟跑偏（每天快几分钟、已累计 34 分钟）：根因是硬件事实、配置改不掉——
#      组件文档 llms-full 明载「LSI 约 32 kHz 供给 IWDT、RTC，无外部晶振」；深睡时
#      PMU 切断 ESP 供电（sleep.c 注释：SOFT_SLEEP drops the ESP rail），时间完全由
#      PMU 内部 RC 走。修法三条：
#      · 漂移率学习补偿：每次校时记录 (真实UTC, 校正前本地秒) 存 NVS，估算 ppm；
#        开机从 PMU 取时按 ppm 反向修正（无网络也有效）。EMA 平滑，样本 <1h 或
#        >2% 视为噪声丢弃，倒退/超 24h 跨度不补偿。
#      · 放宽触发：离线短连校时 6h → 2h（需已保存 WiFi）。
#      · 蓝牙校时：新增 CTRL 操作码 PUSH_OP_SET_TIME(0x05) 携带 4 字节 UTC（小端）。
#        BEGIN 帧定长 83B 且 Java 侧标注「冻结」，故不动 BEGIN；SET_TIME 独立于
#        「必须武装」闸门之外，连上即可校时——无 Wi-Fi 场景的唯一高频通道。
#        APK 已重新编译（encodeSetTime + 连接后下发）。
#   ③ 防串台（MAC 绑定）：广播名原为随机数 PicoPush-XXXX，咖啡馆两台设备会重名、
#      手机可能推错人。改为 token 由出厂 MAC 后两字节派生（mac_token()），广播名
#      自动变成固定唯一的 PicoPush-1A2B；墨水屏显示的广播名同源同步。
#      BEGIN 帧的 token 校验链路完整保留（值与广播名同源，校验照旧）。
#      APK 侧不再「扫到第一个就连」：收集全部 PicoPush- 设备，首个命中后多收 2.5 秒，
#      **仅一台才自动连接**，多台则弹列表让用户按屏幕上显示的代号点选。
#   Gate：push_sink 纯核 168 checks 0 red；APK 协议对表 25/25 全过（build-apk.sh）；
#     fullclean 全量重建 exit 0，警告 **8 条与基线同集**（app_book×7 + ui_pinpad×1，增 0）。
#   重锚说明（gen22 第二次落盘）：审计放行 `0da62846…` 后，为核销三追问又补了
#     ① push_proto.h 的「协议变更记录」注释块（gen22 / 0x05 条目）与 ② 参考实现
#     push_pico.py 同步（encode_set_time + --set-time）。注释与主机工具都不改固件
#     行为，但源码树已变，故按「源码与交付产物必须同源」重跑 fullclean 重编，
#     锚更新为 `fdd48866…`；体积 4,060,496 B 未变、seg1 offs/paddr 未变，
#     功能与放行的 `0da62846…` 等价。ESP-IDF 产物含非确定性因素（每代重编必换
#     sha，bootloader 时间戳亦同），故每落盘一次重锚一次是常态。
#   Re-anchor note (gen22, second drop): after the audit cleared `0da62846…`, the three
#     question close-out added (1) the protocol change-log comment block in push_proto.h
#     (gen22 / 0x05 entry) and (2) reference-implementation sync in push_pico.py. Neither
#     changes firmware behaviour, but the source tree moved, so "source and shipped binary
#     must be same-origin" forced a fresh fullclean rebuild; the anchor is now
#     `fdd48866…` with size 4,060,496 B and seg1 offs/paddr unchanged - functionally
#     equivalent to the cleared `0da62846…`. ESP-IDF output is non-deterministic (every
#     rebuild changes the sha, as the bootloader timestamp does), so re-anchoring on each
#     drop is the norm.
#   终版锚：Read_Pico.bin 4,060,496 B（相对 gen21 终版 4,053,984 **+6,512 B**，
#     双操作数 4,060,496−4,053,984=6512），sha256 `fdd48866…`；
#     seg1 File offs `0x00233888`（gen21 终版 0x00233778，DROM 段 +0x110=272B 顺移）、
#     paddr `0x00243890`；seg1 Load addr 0x3fc9c700 世代不变锚不动。
#     partition-table / ota_data_initial 与 gen21 逐字节一致（9d5d890b… / 7d2c7ac4…）。
#     APK ReadPicoPush.apk sha256 `6e26875a…`（本轮重新编译，含设备列表 + 校时下发）。
# Re-anchor record (2026-10-20, pre-release polish after gen21's all-green hardware
# round / gen22): version 0.3.3-pico22. Three items, all from the user's release review:
#   (1) dedicated cover for the push pseudo-book - push.txt has no embedded art and its
#     path hash landed on a dark template (black plate, white type) that clashed with
#     epubs carrying real covers. New 4th template push_delivery (grained paper, a
#     "phone -> waves -> book" motif, title at the bottom) drawn with the existing
#     grayscale primitives - no image asset. Cache versions bumped or stale entries keep
#     winning: BOOK_AUTO_COVER_VERSION 4->5, FAST_COVER_REVISION 1->2.
#   (2) clock drift (minutes per day, 34 minutes accumulated) - the root cause is a
#     hardware fact no config can undo: the PMU docs state "LSI ~32 kHz feeds IWDT and
#     RTC; no crystal", and in sleep the PMU drops the ESP rail, so the PMU's internal
#     RC carries time alone. Three fixes: learned drift compensation (persist true UTC and
#     the pre-correction local second per sync, estimate ppm, correct the PMU value at
#     boot - works offline; EMA smoothing, samples under 1 h or beyond 2% discarded,
#     no compensation on backwards jumps or >24 h spans); offline resync shortened
#     6 h -> 2 h; and time over BLE via the new CTRL opcode PUSH_OP_SET_TIME (0x05)
#     carrying 4 LE bytes of UTC. BEGIN is a frozen 83-byte layout, so it is untouched;
#     SET_TIME is deliberately outside the "must be armed" gate - the only high-frequency
#     channel when there is no WiFi. The APK was rebuilt to send it.
#   (3) anti-crosstalk (MAC binding) - the adv name used to be random (PicoPush-XXXX), so
#     two panels in a cafe could collide and a phone could push to the wrong screen. The
#     token now derives from the factory MAC's last two bytes (mac_token()), making the
#     name stable and unique (PicoPush-1A2B); the on-screen name comes from the same
#     source. BEGIN's token check is fully retained (same origin as the name). The APK no
#     longer "connects to the first hit": it collects every PicoPush- device, listens
#     2.5 s past the first hit, auto-connects **only when there is exactly one**, and
#     otherwise shows a picker keyed to the code on the screen.
#   Gate: push_sink 168 checks / 0 red; APK protocol table 25/25 (build-apk.sh); fullclean
#     rebuild exit 0 with 8 warnings (same baseline set, none added). Final anchor:
#     4,060,496 B (+6,512 vs gen21), sha256 0da62846..., seg1 File offs 0x00233888
#     (+0x110 DROM shift), paddr 0x00243890; seg1 Load addr 0x3fc9c700 unchanged.
#     partition-table / ota_data_initial byte-identical to gen21. APK sha256 6e26875a...
#
# 重锚记录（2026-10-20，gen20 真机状态条打磨修复轮 / gen21）：
#   版本串：0.3.3-pico21（CMakeLists.txt 一处 + 本记录 + 烧录说明，三处同步）。
#   绝对红线：本次仅为蓝牙推流优化黑色状态条，「最近阅读」大卡片（通用组件）零改动。
#   两项修复（用户 gen20 真机截图实证）：
#   ① 痛点2 状态条文字垂直不居中：根因 = draw_push_card 里 label_on 委托
#     ui_text_fixed_context_aa_vc，第三参是「垂直中心」而非顶边；gen20 误按顶边式
#     传 566+(64-22)/2=587 → 文字中心整体偏高 11px。修正为 598（卡体 566..630 的
#     真中心）。全仓 _vc 调用点仅 2 处，另一处（app_dashboard 时钟）语义自洽不动。
#   ② 痛点1 未读提醒（黑白反色）：读到 100% 大卡变空后，新推送到达黑条无变化、
#     用户错过。修法 = push_sink COMMIT 成功（仅文本推送，单词卡不置位）置「未读位」，
#     NVS 持久化（复用 PUSH_NVS_NS "rp_pushsink" 独立 key，不动 session blob，重启不丢）；
#     开书成功即清（open_book_impl 唯一汇合点）；快照差分驱动重绘（push_card_snap_t
#     加 .unread 位，零新增轮询）。
#   用户拍板定稿（终版形态，覆盖初稿两处）：
#     · 文案「有新内容」；已读判定 = 打开即清（方案 A）；
#     · 徽章形态弃 104x32 大胶囊 → **实心反色小圆点**（12x12 圆角块 r=6 ≈ 圆）
#       + 旁注「有新内容」，右缘 624、垂直中心 598——墨水屏更清爽；
#     · **未开启（IDLE）态绝不亮点**：未开启收不到数据，且白条里黑底白字块突兀；
#       IDLE 有历史未读时右侧文字直接显示「未开启·有未读内容」；
#     · 未读只在武装（ARMED/WINDOW）与连接（CONNECTED/COOLDOWN）态显示；
#       在场时右侧状态文转短式（去广播名）。
#   ③ 手动息屏体验修复（推翻 gen18 豁免覆盖表①条目）：武装期按电源键想息屏
#     曾被 enter_lock_and_sleep 拦截（日志 `lock refused: push sink armed`），反人类。
#     改为手动电源键拥有最高优先级——prepare_lock 加 manual 标志，手动路径
#     `push_sink_cancel_why("manual sleep")` 静默取消武装（安全关射频）→ 日志
#     `armed canceled by manual sleep` → 正常息屏；唤醒后状态卡回 IDLE/「未开启」闭环成立。
#     自动锁屏/深睡（manual=false）仍豁免保活（M1.5 关屏保活），由 enter_lock_and_sleep 拒锁，不在此取消。
#     技术风险点「手动 vs 自动」已规避：on_before_lock 既被手动也被自动调用，故 cancel 上移到
#     prepare_lock 并仅对 manual 触发；五个调用点中 502/516（run_power_action→深睡自带 cancel）
#     与 1133（auto_lock_due）传 false，735/915/998（手动电源键/关机页/长按）传 true。
#   Gate：push_sink 纯核 168 checks 0 red；fullclean 全量重建 exit 0，警告 **8 条与
#     基线同集**（app_book×7 + ui_pinpad×1，全部 unused-function，gen21 增 0）。
#   三次修订链（同一 fork 专属串 pico21，仅重编）：
#     · 初稿 `28b1474d…` 4,053,792 B（居中 + 104×32 大胶囊未读徽章）；
#     · 拍板修订 `89bb284d…` 4,053,936 B（+144B：IDLE 不亮块 + 小圆点 + 旁注）；
#     · 终版 `c6e433dd…` 4,053,984 B（+48B：手动息屏 prepare_lock 标志 + cancel 调用）。
#   终版锚：seg1 File offs `0x00233778`（gen20 0x00233708，DROM 段随三轮代码增长
#     +0x70=112B 顺移）、paddr `0x00243780`，EXPECTED 随之重锚。
#   bootloader 21,232 B（哈希每代随构建时间戳必变，零行为变化）；partition-table /
#     ota_data_initial 与 gen20 逐字节一致（sha256 9d5d890b… / 7d2c7ac4…）。
# Re-anchor record (2026-10-20, gen20 status-bar polish fix round / gen21):
#   Version string: 0.3.3-pico21. Absolute red line: this round only touches the
#   black status bar for BLE push; the "recent reading" big card (shared
#   component) is untouched. Two fixes (user screenshots on gen20 hardware):
#   (1) vertical centering - draw_push_card delegated label_on to
#     ui_text_fixed_context_aa_vc whose 3rd arg is a VERTICAL CENTER, not a top
#     edge; gen20 passed 587 (top-edge formula) so the text sat 11px high; now
#     598, the true center of the 566..630 card. (2) unread reminder - COMMIT
#     success (text pushes only) sets an unread bit persisted in NVS (own key in
#     the existing "rp_pushsink" namespace; survives reboot), cleared on
#     successful book open (open_book_impl); snapshot-diff redraw via a .unread
#     bit in push_card_snap_t, zero new polling. Final form per user ruling: a
#     solid inverted 12x12 dot (r=6) plus the caption "有新内容" (replacing the
#     104x32 capsule), right edge 624, vcenter 598; ARMED states only - IDLE
#     never shows the dot and simply says "未开启·有未读内容" in the right text;
#     while up, the right status text drops the advertise name. (3) manual
#     screen-off no longer refused - prepare_lock gained a manual flag; manual
#     power-key paths call push_sink_cancel_why("manual sleep") then sleep
#     (log "armed canceled by manual sleep"); auto lock keeps the M1.5 exemption
#     (still refused by enter_lock_and_sleep).
#   Gate: push_sink 168 checks / 0 red; fullclean rebuild exit 0 with 8 warnings
#     (same baseline set, gen21 adds none). Final build: 4,053,984 B (+192 B vs
#     gen20), sha256 c6e433dd..., seg1 File offs 0x00233778 (+0x70 DROM shift),
#     paddr 0x00243780; EXPECTED re-anchored.
#
# 重锚记录（2026-10-20，gen19 真机验收后状态条打磨轮 / gen20）：
#   版本串：审计③点名 gen19 漏 bump（出厂仍带 0.3.3-pico18，TF 升级判同版本会拒）。
#     本代恢复每代一跳 → 0.3.3-pico20（CMakeLists.txt 一处 + 本记录 + 烧录说明，三处同步）。
#   三处真机打磨（用户 gen19 截图实证）：
#   ① 问题1「长文推送后状态条文字不更新」+ 问题2「信息冗余」合并修：砍掉状态条
#     副行「最近：xxx」。根因 = 该副行与大卡右上推送摘录重复，且长文 BEGIN.title 为空时
#     book_title_set 被 valid_utf8 拒（book_title.c:53 空串/首尾空格/控制字符一律拒），
#     副行冻在上一条有标题的推送（截图「最近: 橡胶」）。删行后症状类整体消失。
#   ② 状态条单行化（已武装/未开启两态一眼可分）：
#     · 未开启＝白底 + 浅灰描边(0xb0) + 暗淡字(0x90)；
#     · 已武装＝纯黑底(0x00)反白字(0xff) + 右侧带广播名（等待连接·剩 X 分钟 · PicoPush:XXXX）。
#   ③ take_committed 机制保留（缓存写回书架/阅读器标题覆盖），但不再强制重绘（行已删，
#     卡片由状态快照差分驱动）。push_sink 纯核 168 checks 0 red 不受影响。
#   Gate：push_sink 纯核 168 checks 0 red；全仓 fullclean 全量 **8 警告**（app_book×7 + ui_pinpad×1，
#     全部 unused-function，与 gen18/19 同集，gen20 增 0）；增量零错误。
#   改后重编：Read_Pico.bin 4,053,792 B（相对 gen19 4,053,856 **-64 B**——状态条副行代码删除致缩，
#     相对 gen18 4,053,392 **+400 B**），sha256 `71f05db1…`、seg1 File offs `0x00233708`、paddr `0x00243710`，
#     EXPECTED 随之重锚。
#
# 重锚记录（2026-10-20，gen18 真机回归 5 Bug 修复轮 / gen19）：
#   根因（gen18 真机测试逐条实证）：底层防假死 / 连续推流完美，但暴露 4 个 UI/状态机 Bug + 1 个底层刷新 Bug，本次一并修出。
#   ① Bug1 状态卡 UI 错位（黑条太扁、第二行文字贴边蒸发）：PUSH_CARD_RECT 高 54→64、文字 y 坐标重排（app_dashboard.c）。
#   ② Bug2 推送入口点击概率性失效（首页卡有时进不去、「手机推送」按钮每次需重进）：app_transfer.c method_gesture origin==2 当场 push_arm_now 武装（修入口失效）；
#     app_dashboard.c on_tick 手势期禁刷状态卡（修点击被吞）。
#   ③ Bug3 退出阅读器后状态卡异常（空数据 / 显示「断开连接 / 未开启」）：app_book.c / app_transfer.c 摘除错误 push_sink_cancel（日志实证 armed canceled by 退出阅读器）。
#   ④ Bug4 底层刷新欠载（line buffer underrun + pclk 回 12MHz，BLE 高速收数并发抢 SPI/CPU）：display.c 加 pclk 策略 + 武装期收口不必要的局部刷新。
#   ⑤ Bug5 首页「最近推送」文字不更新（连续推送卡在首推旧值「but」）：根因是 book_title_set(PUSH_TEXT_PATH,...) 仅在 app_transfer 前台消费 take_open 时调用，
#     首页前台时缓存不更新；且 s_push_snap 不含标题、push_card_due 不重绘。修法：push_sink 新增 take_committed 一次性事件（每次成功 COMMIT、非单词卡触发），
#     首页 on_tick 消费后写回 book_title_set 缓存并强制状态卡局部重绘。
#   Gate：push_sink 纯核 168 checks 0 red、全仓增量编译零错误、fullclean 全量 8 警告（app_book×7 + ui_pinpad×1，与 gen18 同集，增量编译仅重编改动文件故一度被误报为 7）。
#   改后重编：Read_Pico.bin 4,053,856 B（相对 gen18 4,053,392 **+464 B**），sha256 `80901528…`、seg1 File offs `0x00233708`、paddr `0x00243710`，EXPECTED 随之重锚。
#
# 重锚记录（2026-10-20，无缝连续推送体验修复轮 / gen18 — 含 18-a 保活 + 自动切回 + 首页 deep-link + 状态卡 + 假武装排雷）：
#   根因（gen17 真机日志逐条实证 + push_session 状态机全转移审计）：推长文开书后蓝牙断开、连推多段需手动退出阅读器→重进接收页→等重连。
#   断裂点 100% 在 UI 层——push_session 状态机 PUSH_STATE_COOLDOWN 已内置连续推流全部转移（DISCONNECT→WINDOW、CONNECT→CONNECTED、
#   BEGIN→RECEIVING），协议层零改动。gen17 日志铁证：verify ok(COOLDOWN) → transfer leave(+100ms，transfer_on_exit cancel) → 手机
#   notify off+disconnected（APK finally closeQuietly()，比设备晚 2s，两边各自拆台）。反证：单词卡回合手机断开→设备回 WINDOW 继续广播→重连二推成功。
#   假武装排雷（静默失败零容忍，任何武装期 BLE 拆除必须打「armed canceled by X」）：全量 grep 通往 ble_pt_stop / 深睡的入口，逐一补 cancel+原因，共 10 路——
#     ① 电源键息屏（app_device_settings.c:2310 on_before_lock / app_loop.c prepare_lock）：**gen21 推翻原「豁免保活」裁定**——手动电源键（息屏/休眠）
#        拥有最高优先级，不再拦截（`lock refused: push sink armed` 退出历史）。改为在 prepare_lock 加 manual 标志、手动路径
#        `push_sink_cancel_why("manual sleep")` 静默取消武装（安全关射频）+ 日志 `armed canceled by manual sleep` → 正常息屏；唤醒后
#        状态卡回 IDLE/「未开启」闭环成立。自动锁屏/深睡（manual=false）仍豁免保活（M1.5 关屏保活），由 enter_lock_and_sleep 拒锁。
#        on_before_lock 的 `if(!push_sink_armed()) ble_pt_stop` else「screen-off skipped ble stop」仅对自动路径生效（手动路径到达时已拆栈）。
#     ② 飞行模式开（app_device_settings.c:1724 flight_mode_toggle）：先 `push_sink_cancel_why("飞行模式已开启，接收暂停")` 再 stop；
#     ③ 关机/深睡（sleep.c:421 app_enter_host_sleep）：先 `push_sink_cancel_why("关机/深睡，接收结束")` 再 stop；
#     ④ USB 存储模式（usb_storage.c:81 usb_storage_start）：`push_sink_cancel_why("USB 传书已接管，接收暂停")`；
#     ⑤ TF 卡拔出（app_transfer.c:1384 transfer_on_media_lost）：`push_sink_cancel_why("TF 卡已拔出，接收结束")`；
#     ⑥ 退出阅读器（app_book.c:6593 book_on_exit）：`else push_sink_cancel_why("退出阅读器，接收结束")`；
#     ⑦ 用户主动退出接收（push_sink.c:1453 push_sink_cancel）：包装 `push_sink_cancel_why("已退出接收")`；
#     ⑧ 栈停兜底（push_sink.c:1365 push_sink_on_stack_stop）：武装期打「stack stopped while armed」覆盖 WiFi 租约等静默入口；
#     ⑨ 锁屏认证（lock_screen.c:87 lock_screen_authenticate）：武装期已在 enter_lock_and_sleep:489 拒锁（`lock refused: push sink armed`），此路不可达——**豁免（上游拒锁）**；
#     ⑩ 拔电收官（push_sink.c poll 拔电判定）：`close_why_set("充电线已拔出，接收结束")` 覆盖 UNPLUG 与宽限收官两路。
#   新增 API：`push_sink_cancel_why(const char*)` / `push_sink_last_close_why(void)`；`push_sink_arm()` 清原因、`push_sink_poll()` 拔电补原因、`ui_post_closed()` 空原因补默认「接收已结束」。
#   gen18 四类功能：① 18-a 保活——transfer_on_exit 条件化（交接标记 s_push_handed_off 双向）+ book_on_exit 收摊；② 自动切回——CONNECT 事件判 s_path==PUSH_TEXT_PATH → request_app=app_transfer
#   （app_book.c on_tick / app_transfer.c on_enter，不重新武装）；③ 首页 deep-link——首页推流卡只跳传书页，绝不静默武装；④ 首页状态卡案1——读 push_sink_state()：IDLE 暗态/ARMED 亮+倒计时/
#   CONNECTED·RECEIVING 进度/最近一推标题，APK 零改动。
#   门禁：13 个 host runner 全绿（含补桩的 transfer_ui/book_management/test_usb_storage）；fullclean 重编 8 条警告与基线函数名集合逐一对上（app_book.c 7 + ui_pinpad.c 1）；
#   partition-table/ota_data_initial 与 gen17 逐字节相同（sha256 9d5d890b…/7d2c7ac4…）；闪前自检 EXPECTED 重锚至本代 Read_Pico.bin 哈希。
# Rebuild record (2026-10-20, seamless continuous-push UX fix round / gen18 — 18-a keep-alive + auto-switchback + home deep-link + status card + false-arm mine sweep):
#   Root cause (gen17 device log, line by line, plus a full push_session state-machine transition audit): after pushing a long text and opening the book, BLE
#   drops and pushing another segment forces a manual exit from the reader, re-enter the receiver page and wait for reconnect. The break is 100% in the UI layer —
#   push_session's PUSH_STATE_COOLDOWN already holds every continuous-push transition (DISCONNECT->WINDOW, CONNECT->CONNECTED, BEGIN->RECEIVING) with zero protocol change.
#   gen17 log proof: verify ok(COOLDOWN) -> transfer leave (+100ms, transfer_on_exit cancel) -> phone notify off + disconnected (APK finally closeQuietly(), 2s later
#   than the device, both sides tearing down). Counterexample: single-card round, phone disconnects -> device returns to WINDOW, keeps advertising -> reconnect, second push succeeds.
#   False-arm sweep (zero tolerance for silent failure; any BLE teardown while armed must log "armed canceled by X"): grep every entry to ble_pt_stop / deep sleep, add cancel+why to each, 10 paths —
#     ① power-key screen-off (app_device_settings.c:2310 on_before_lock / app_loop.c prepare_lock): **gen21 reverses the old "exempt keepalive"
#        ruling** - a manual power key (screen-off/sleep) now outranks the arm and is never refused ("lock refused: push sink armed" is retired);
#        prepare_lock gained a manual flag and the manual path calls push_sink_cancel_why("manual sleep") (silent disarm + "armed canceled by
#        manual sleep" log) then sleeps; on wake the status card returns to IDLE/"未开启". Auto lock/deep-sleep (manual=false) keeps the
#        exemption (M1.5 screen-off keepalive) and is still refused by enter_lock_and_sleep.
#     ② flight mode on (app_device_settings.c:1724): push_sink_cancel_why("飞行模式已开启，接收暂停") before stop;
#     ③ power-off / deep sleep (sleep.c:421 app_enter_host_sleep): push_sink_cancel_why("关机/深睡，接收结束") before stop;
#     ④ USB storage (usb_storage.c:81 usb_storage_start): push_sink_cancel_why("USB 传书已接管，接收暂停");
#     ⑤ TF card out (app_transfer.c:1384 transfer_on_media_lost): push_sink_cancel_why("TF 卡已拔出，接收结束");
#     ⑥ exit reader (app_book.c:6593 book_on_exit): else push_sink_cancel_why("退出阅读器，接收结束");
#     ⑦ user quit (push_sink.c:1453 push_sink_cancel): wraps push_sink_cancel_why("已退出接收");
#     ⑧ stack-stop fallback (push_sink.c:1365 push_sink_on_stack_stop): "stack stopped while armed" covers WiFi-lease silent entries;
#     ⑨ lock auth (lock_screen.c:87 lock_screen_authenticate): while armed, enter_lock_and_sleep:489 already refuses lock ("lock refused: push sink armed"), path unreachable — EXEMPT (upstream refuse);
#     ⑩ unplug (push_sink.c poll): close_why_set("充电线已拔出，接收结束") covers both UNPLUG and grace-period wind-down.
#   New API: push_sink_cancel_why(const char*)/push_sink_last_close_why(void); push_sink_arm() clears the why, push_sink_poll() sets it on unplug, ui_post_closed() defaults empty to "接收已结束".
#   gen18 four features: (1) 18-a keep-alive — transfer_on_exit conditional (s_push_handed_off both ways) + book_on_exit cleanup; (2) auto-switchback — CONNECT judges
#   s_path==PUSH_TEXT_PATH -> request_app=app_transfer (app_book.c on_tick / app_transfer.c on_enter, no re-arm); (3) home deep-link — the home push card only jumps to the transfer page, never silently arms;
#   (4) home status card plan-1 — reads push_sink_state(): IDLE dim / ARMED bright+countdown / CONNECTED·RECEIVING progress / last push title, zero APK change.
#   Gate: all 13 host runners green (incl. restubbed transfer_ui / book_management / test_usb_storage); fullclean rebuild, 8 warnings match the baseline function-name set (app_book.c 7 + ui_pinpad.c 1);
#   partition-table/ota_data_initial byte-identical to gen17 (sha256 9d5d890b…/7d2c7ac4…); pre-flash self-check EXPECTED re-anchored to this build's Read_Pico.bin hash.
#   审计逐行复查补丁（门禁核销后、烧录前）：全量 grep `ble_pt_stop`/`ble_pt_sync(false)`
#   暴露第 6 处 BLE 拆除入口 `app_loop.c:141 quick_toggle_wifi()` 的 `ble_pt_sync(false)`
#   武装期无 `push_sink_armed()` 守卫——原覆盖表漏列。补与息屏同款豁免守卫
#   （`if(!push_sink_armed()) ble_pt_sync(false); else log "wifi toggle skipped ble stop"`），
#   全仓 BLE 拆除入口现全闭（2 处豁免守卫 + 2 处预期拆除带 cancel + 1 处上游拒锁不可达
#   + on_stack_stop 全局兜底日志）。改后重编：Read_Pico.bin 4,053,392 B（+64B），
#   sha256 `2fafca21…`、seg1 File offs `0x002336e8`、paddr `0x002436f0`，EXPECTED 随之重锚。
#
# 重锚记录（2026-10-20，单词卡 ERR(9) 误报修复轮 / gen17）：
#   用户举证：TF 卡 /sdcard/dict/ 下确实有 dict.bin，阅读时长按查词完全正常，
#   但蓝牙推流的单词卡报 ERR(9)「无词典」。gen16 把原因赖到「卡上没词典」——
#   **判错了**。
#   逐条排除用户提出的三个假设：① 路径拼写 —— DICT_DIR 恒为绝对路径
#   "/sdcard/dict"，前缀不缺，排除；② 文件系统并发锁 —— ERR(9) 出现在 37681ms，
#   早于推流 rename 失败（67612ms），不可能是它的后果；且 CONFIG_FATFS_FS_LOCK=0
#   （FatFs 未启用锁）；且 dict_ready() 只读内存指针 s_dict.file，根本不 fopen，
#   排除；③ VFS 挂载 —— 需实测，已加 errno 日志待证。
#   真根因：探针挂的是 `dict_ready()`，其语义是「词典句柄当前是否已打开」
#   （`s_dict.file != NULL && s_dict.sparse != NULL`），**不是「卡上有没有文件」**。
#   词典是惰加载的：只有查词/预热才真正 fopen。长按查词之所以正常，是
#   dict_popup.c 里写着 `if (!dict_ready()) ready = dict_open();` 会自己打开；
#   而推送的 BEGIN 前置闸门只**读**这个状态、从不尝试打开 → 开机后一次词都没查过
#   时，单词卡必然误报 ERR(9)。
#   修法：探针改为 `dict_warm()`（已打开则 true，否则尝试 dict_open()，且失败
#   **不武装退避**——其注释正是为「卡片可能还没就绪」而写）；并在 dict_open /
#   try_open 打印尝试的完整绝对路径与 errno；push_sink 的 err=9 日志改为诚实措辞
#   「探针此刻判定不可用，不等于卡上没有词典」。
# Rebuild record (2026-10-20, word-card ERR(9) false-positive fix round / gen17):
#   the user showed that /sdcard/dict/ really holds dict.bin and that long-press
#   lookup works, yet pushed word cards answered ERR(9) "no dictionary". gen16
#   blamed "no dictionary on the card" - **that diagnosis was wrong**.
#   The user's three hypotheses, ruled out one by one: (1) path spelling -
#   DICT_DIR is the absolute "/sdcard/dict", the prefix is not missing, ruled out;
#   (2) filesystem lock - ERR(9) fired at 37681 ms, BEFORE the push rename failure
#   (67612 ms), so it cannot be a consequence; CONFIG_FATFS_FS_LOCK=0 (FatFs has
#   no locking); and dict_ready() only reads the in-memory pointer s_dict.file
#   without any fopen, ruled out; (3) VFS mount - needs measurement, errno logging
#   added for that.
#   Real root cause: the probe was `dict_ready()`, whose meaning is "is the
#   dictionary handle currently open" (`s_dict.file != NULL && s_dict.sparse !=
#   NULL`), **not** "does a dictionary exist on the card". The dictionary is loaded
#   lazily - only a lookup or a warm-up actually opens it. Long-press lookup works
#   because dict_popup.c says `if (!dict_ready()) ready = dict_open();`, i.e. it
#   opens the file itself; the push BEGIN pre-check only **reads** that state and
#   never tries to open, so a word card always misfired ERR(9) when no word had
#   been looked up since boot.
#   Fix: probe `dict_warm()` instead (true if already open, otherwise try
#   dict_open(), and a failure does **not** arm the back-off - its comment is
#   written exactly for "the card may not be ready yet"); log the full absolute
#   path attempted plus errno in dict_open/try_open; and reword the push_sink
#   err=9 log honestly - "the probe says unavailable right now, which is not proof
#   that the card lacks a dictionary".
#
# 重锚记录（2026-10-20，gen15 真机「再推全失败」修复轮 / gen16）：
#   根因两条叠加：① FatFs 的 f_rename **不覆盖已存在目标**（FR_EXIST → EEXIST），
#   POSIX 的「原子替换」语义它没有——首轮 push.txt 不存在所以成功，**第二轮起
#   rename 必败** → reply_err(ERR(5))，APK 显示「TF 卡不可用」（文案误导，实为
#   EEXIST）。② do_verify() 递归进 exec_actions 时 COMMIT 与 OPEN_BOOK 同位携带，
#   do_commit() 原返回 void、失败无法短路 → **旧 push.txt 被照样弹出**。
#   真机铁证：`verify ok: sz=30 crc=0x15542824` 紧跟 `commit rename: File exists`，
#   其后 `book: open start` 弹出的仍是首次内容。
#   修法：① rename 前先 remove(旧 push.txt)；② do_commit() 改返回 bool，
#   exec_actions 里提交失败即清除 OPEN_BOOK/WORD_CARD 位；③ RESUME 通道加 sid
#   诊断日志（审计裁定：保留分支、改造为诊断通道）；④ APK 的 ERR(5) 文案改诚实。
# Rebuild record (2026-10-20, gen15 on-hardware "every later push fails" round /
# gen16): two root causes stacked. (1) FatFs's f_rename **never overwrites an
# existing target** (FR_EXIST -> EEXIST) - the POSIX "atomic replace" semantics
# are absent - so the first push works (no push.txt yet) while **every later one
# fails the rename** -> reply_err(ERR(5)), shown by the APK as "TF card
# unavailable" (misleading: it is really EEXIST). (2) do_verify() re-enters
# exec_actions with COMMIT and OPEN_BOOK carried together and do_commit() returned
# void, so a failure could not short-circuit - the OLD push.txt was opened anyway.
# Hardware proof: `verify ok: sz=30 crc=0x15542824` immediately followed by
# `commit rename: File exists`, and then `book: open start` still opened the
# first push's content.
#   Fixes: (1) remove() the old push.txt before rename; (2) do_commit() returns
# bool and exec_actions clears OPEN_BOOK/WORD_CARD on failure; (3) the RESUME path
# logs its sid (audit ruling: keep the branch, turn it into a diagnostic channel);
# (4) the APK's ERR(5) label is now honest.
#
# 重锚记录（2026-10-19，gen14 真机 CRC 失败修复轮 / gen15）：
#   根因：ESP-IDF 的 FATFS VFS **没有注册 .flush_p**（vfs_fat.c 全文零 "flush"），
#   fflush() 只把 newlib 缓冲推进 f_write，不触发 f_sync；sdkconfig 实证
#   CONFIG_FATFS_IMMEDIATE_FSYNC 未开，f_write 后也不补 f_sync —— 于是 .part 的
#   **目录项 size 永不更新**。do_verify() 紧随其后的 stat()（读目录项）与
#   fopen("rb") 回读拿到过期值：长度对账必然不等、CRC 必然不符 → 真机 err=4
#   (PUSH_ERR_CHECKSUM)，UI「内容校验失败，请重新发送」。
#   修法：回读前 part_close()（f_close 内部第一行即 f_sync，见 ff.c:4345），
#   顺带消掉 FATFS FS_LOCK=0 下「同一文件两次打开」的非法打开（ffconf.h 明文禁止）。
#   附带修复：session_id 恒定导致「一次失败→永久续传」卡死（APK 改逐次唯一 sid）；
#   NVS 检查点补 part_sync()，让「.part 可见长度不超前于记账」这条注释真正成立。
# Rebuild record (2026-10-19, gen14 on-hardware CRC failure fix round / gen15):
#   root cause: ESP-IDF's FATFS VFS registers **no .flush_p** (zero "flush" in
#   vfs_fat.c), so fflush() only pushes the newlib buffer into f_write without
#   f_sync, and CONFIG_FATFS_IMMEDIATE_FSYNC is off (sdkconfig) so f_write does
#   not sync either - the .part **directory entry never learns the new size**.
#   The stat() (directory entry) and fopen("rb") readback that follow in
#   do_verify() therefore read stale values: the length reconciliation can never
#   match and the CRC can never agree -> on-hardware err=4 (PUSH_ERR_CHECKSUM),
#   UI "内容校验失败，请重新发送".
#   Fix: part_close() before the readback (f_close's first statement is f_sync,
#   see ff.c:4345), which also removes the duplicate open that ffconf.h forbids
#   under FATFS FS_LOCK=0. Also fixed: a constant session id deadlocked every push
#   into a permanent resume after one failure (APK now issues a distinct sid per
#   push); the NVS checkpoint now calls part_sync() so the "visible length never
#   runs ahead of the counters" comment finally holds.
# gen24.5（武装页常开按钮去除）：gen24.4 跨页进入整屏 GC16 已让底色纯白、
#   时钟残影消失，但【切换充电时常开】按钮本身仍糊成一团、严重重影、字与
#   边框黑成一团，还压住上方提示文字。根因三条叠加：① 按钮被并进
#   push_dynamic_rect() 活区，倒计时每秒的 DU 区域快刷反复重驱它，抗锯齿
#   灰阶像素误差累积；② 样式不是纯黑白——灰框 0x92/0xc0、禁用灰底 0xe8、
#   且 can_hold 被当 inverted 传入 ui_text_vc，可用态竟是反白渲染；
#   ③ control_rect(0) 的 y=608 与第二行提示文字（y=586 起，字底约 610）
#   相叠。用户裁定：按钮整块删除——禁用态文案「常开需充电」有歧义，会让人
#   以为开着蓝牙马上就要没电。删除 draw_push 的按钮段与 push_hold_rect()、
#   push_gesture 的 origin==0 常开切换分支、hold 簿记与 push_charging_now()
#   （随按钮成死代码，留着会多一条 -Wunused-function 破 8 警告基线）；
#   活区维持两卡形态（168~532），每秒 DU 快刷不再压到任何静态控件。
#   底层 push_sink_arm(0, 0) 能力保留，只是没有 UI 入口。
#   门禁：宿主 runner 14/14 全绿（活区断言 496→364，原「插电常开切换」回归
#   改为「入口已去除」断言）；变异验证有牙（把 control_rect(0) 命中加回 →
#   transfer_ui 断言 arm/cancel 计数失败）；fullclean exit 0，8 警告同基线
#   （app_book.c 7 + ui_pinpad.c 1）。锚：Read_Pico.bin 4,062,960 B（相对
#   gen24.4 4,063,488 为 -528 B，双操作数 4,062,960-4,063,488=-528），
#   sha256 `5c3bab05…`，seg1 File offs `0x00233d58`（paddr = offs + 0x10008 =
#   0x00243d60）、
#   Load `0x3fc9c700`、len `0x08a8c`；partition-table `9d5d890b…`、
#   ota_data_initial `7d2c7ac4…` 与 gen24.4 逐字节一致；bootloader
#   `8e7f7866…`→`cbe2b12a…`（同 21,232 B，编译时间戳差异）。
# Gen24.5 (the arm page's hold button removed): gen24.4's screen-wide GC16 on
#   entry already made the base pure white and killed the clock ghosting, but
#   the "switch charger-held" button itself was still a smeared, ghosted blob
#   of run-together text and frame, and it covered the notice line above. Three
#   causes stacked: (1) the button was unioned into push_dynamic_rect(), so the
#   per-second DU area refresh kept re-driving it and the antialiased gray
#   pixels accumulated error; (2) the styling was never pure black and white -
#   gray frames 0x92/0xc0, a gray disabled fill 0xe8, and can_hold passed as
#   inverted to ui_text_vc, which made the enabled state an inverted render;
#   (3) control_rect(0)'s y=608 overlapped the second notice line (y=586,
#   bottom around 610). The user ruled the button out entirely: the disabled
#   label "hold needs charger" reads as if Bluetooth being on were about to
#   drain the battery. Removed draw_push's button block and push_hold_rect(),
#   push_gesture's origin==0 hold-switch branch, the hold bookkeeping and
#   push_charging_now() (dead with the button, and one stray -Wunused-function
#   would break the 8-warning baseline); the live region stays at its two-card
#   shape (168-532) so the per-second DU never lands on a static control again.
#   push_sink_arm(0, 0) keeps the ability in the component - it just has no UI
#   entry now. Gate: 14/14 host runners green (the live-region assertion moves
#   496 to 364; the old "charger-held switch" regression becomes an
#   "entry removed" assertion); the mutation test bites (restoring the
#   control_rect(0) hit fails transfer_ui's arm/cancel counters); fullclean
#   exit 0 with the 8-warning baseline intact (app_book.c 7 + ui_pinpad.c 1).
#   Anchors: Read_Pico.bin 4,062,960 B (-528 vs gen24.4's 4,063,488;
#   4,062,960-4,063,488=-528), sha256 `5c3bab05…`, seg1 File offs
#   `0x00233d58` (paddr = offs + 0x10008 = 0x00243d60), Load `0x3fc9c700`,
#   len `0x08a8c`;
#   partition-table `9d5d890b…` and ota_data_initial `7d2c7ac4…` are
#   byte-identical to gen24.4; bootloader `8e7f7866…` -> `cbe2b12a…` (same
#   21,232 B, build-timestamp difference).
# gen25（内存三层根治 + 推送让路链 + 内存体检面板 + gen26 四预留）：
#   版本串：0.3.3-pico25（CMakeLists.txt 一处 + 本记录 + 顶部版本串注释，三处
#   同步；顺带订正 gen24.4/24.5 漏更的顶部注释漂移）。
#   背景（gen24 真机）：BLE 推送预检拒绝的直接原因是内部堆最大连续块跌出 19K——
#   根因三层：数据层（书内大块分配压内部堆）、分配卫生（≥1-2K 非热路径分配
#   落 PSRAM）、以及 12K 元凶 book_prep 任务栈关书后不归还。
#   修法四处：
#     ① 12K 归还：prep_task 改为安全点自删（s_prep_task=NULL → give(s_prep_done)
#       → vTaskDelete(NULL)），外部路径一律不 vTaskDelete；free_book 有界等待
#       句柄为空；任务自删前打 `prep stack HWM: N words`（min-so-far，降栈决策
#       数据）。s_prep_done 为二元信号量（helper 先 take(0) 清残留再等，无
#       「早完成」竞态）。
#     ② app_reading.c 内置 reading.md（7,893 B）malloc → PSRAM（内部兜底）。
#     ③ 推送让路链（惊喜②）：栈 down + 预检不过 + 确实有书开着 三条件同时成立
#       才让路——request_prep_stop 非阻塞置旗 → on_tick 下一拍确认 prep 已停
#       （1500ms 观察窗判据是句柄 NULL 不是钟）→ 卸书 → 重过预检 → 武装；
#       仍不过 → 「内存未能腾出——重启设备后可直接推送」重启兜底（不原地重试）。
#       收官双路：让路过的推送走 restore_after_push 回原书原位置（失败必须
#       出声），B 留书架并 mark_push_first_open（首次手动打开仍走一次 GC16，
#       非让路推送走 open_clean 直达打底，两路不交叉）。
#     ④ 内存体检面板（惊喜③）：传书方法页长按入口（LONG_PRESS 与 TAP 互斥，
#       零手势冲突），模态面板只盖方法卡区（224..560），不碰提示条/说明行/
#       返回键/底栏；读数同源 ble_pt_mem_snapshot + ble_pt_mem_gate（与按钮
#       预检/start_locked 同一份数字）；红绿灯 largest 主判（红 <15K / 黄
#       15–20K 碎片率辅显 / 绿 ≥20K=BLE 闸门同门槛），碎片率 = 1−largest/free
#       （free=0 记 100%，不除零不误导）；【知道了】/再长按/任意按键关闭，
#       其余手势一律吞；视图被让路收官/收摊切走时面板静默让位。
#   gen26 四预留（零行为变更）：push_proto.h CTRL 0x06..0x0F 整段登记
#     reserved-not-implemented（未知 op 仍走 default→ERR(2)，宿主测试断言
#     解码器原样放行且与 STATUS 0x10 起零重叠）；武装页可扩展卡片渲染骨架
#     （只留纪律注释不出函数）；push_sink.h PSRAM 结果缓冲 helper 预留
#     （API 形态/预算/生命周期登记）；AI 设置位预留（settings.h API 形态 +
#     settings.c NVS_KEY_AI "ai_on" 占号 + 备份 profile.home_full_refresh
#     bit 6(0x40) 留位）。
#   门禁：宿主 runner 14/14 全绿（case 级登记 KNOWN-FAIL 维持：ui_kit §1 r93
#     packed framebuffer + home_recent 上游过期测试，均见 KNOWN_FAILURES.md）；
#     push_sink 189/0（168 + 预留区登记 21 项）；transfer_ui 新增
#     push_yield_regression（8 场景）+ mem_panel_regression（阈值/公式/模态/
#     视图卫兵，变异有牙：长按不开面板 → 930 断言失败）；book_management
#     73 PASS（reader_fullscreen 补 gen25 符号立式替身）；fullclean exit 0，
#     8 警告同基线（app_book.c 7 + ui_pinpad.c 1，逐条对表）。锚：
#     Read_Pico.bin 4,066,576 B（相对 gen24.5 4,062,960 为 +3,616 B，双操作数
#     4,066,576-4,062,960=+3,616），sha256 `de93e5e4…`，seg1 File offs
#     `0x002341d8`（paddr = offs + 0x10008 = 0x002441e0）、Load `0x3fc9c700`、
#     len `0x08a8c`；partition-table `9d5d890b…`、ota_data_initial `7d2c7ac4…`
#     与 gen24.5 逐字节一致；bootloader `cbe2b12a…`→`834ec762…`（同 21,232 B，
#     编译时间戳差异）。
# Gen25 (three-layer memory root fix + the push yield chain + the memory health
#   panel + gen26's four reservations):
#   Version string: 0.3.3-pico25 (CMakeLists.txt + this record + the top
#   version comment, three places in sync; also fixes the top-comment drift
#   gen24.4/24.5 left at 24.3).
#   Background (gen24 hardware): the BLE push precheck refusals traced to the
#   internal heap's largest block falling under 19K - three layers: the data
#   layer (in-book large allocations crowd the internal heap), allocation
#   hygiene (1-2K+ off-hot-path allocations belong in PSRAM), and the 12K
#   culprit: the book_prep task stack never returned when a book closed.
#   Four fixes:
#     (1) The 12K returned: prep_task now self-deletes at a safe point
#       (s_prep_task=NULL -> give(s_prep_done) -> vTaskDelete(NULL)); external
#       paths never call vTaskDelete; free_book waits bounded for a NULL
#       handle; the task logs `prep stack HWM: N words` (min-so-far, the
#       stack-shrink decision data) before self-deleting. s_prep_done is a
#       binary semaphore (the helper takes 0 to clear residue first - no
#       "done early" race).
#     (2) app_reading.c's built-in reading.md (7,893 B) malloc -> PSRAM
#       (internal fallback).
#     (3) The push yield chain (surprise 2): only when "stack down + precheck
#       failed + a book is really open" all hold - request_prep_stop sets the
#       flag non-blocking, on_tick's next tick confirms prep stopped (the
#       1500 ms window judges by a NULL handle, not the clock), unloads,
#       re-runs the precheck and arms; still failing -> the restart fallback
#       "memory could not be freed - push works right after a restart" (no
#       in-place retry). Finale dual-path: a yielded push restores the original
#       book at its position (failure must speak up), B stays on the shelf with
#       mark_push_first_open (its first manual open takes one GC16; a non-yield
#       push goes through open_clean directly - the two paths never cross).
#     (4) The memory health panel (surprise 3): long-press on the method page
#       (LONG_PRESS and TAP are mutually exclusive - zero gesture conflict); a
#       modal panel covering the method-card area only (224-560), touching
#       neither the hint banner, info lines, back key nor tab bar; readings
#       share ble_pt_mem_snapshot + ble_pt_mem_gate with the button precheck /
#       start_locked; the traffic light is largest-driven (red <15K, yellow
#       15-20K with fragmentation as a note, green >=20K = the BLE gate's own
#       threshold), fragmentation = 1-largest/free (free=0 reads 100% - no
#       division by zero, no misleading ratio); closed by [got it], another
#       long press or any key, everything else swallowed; the panel steps aside
#       silently when the yield finale or a close-up switches the view.
#   gen26's four reservations (zero behavior change): push_proto.h registers
#     CTRL 0x06-0x0F as reserved-not-implemented (unknown ops still take
#     default->ERR(2); the host test asserts the decoder passes the op through
#     and that the block never overlaps STATUS's 0x10); the arm page's
#     extensible card-render skeleton (a discipline comment, no function);
#     push_sink.h's PSRAM result-buffer helper reservation (API shape, budget
#     and lifetime registered); the AI settings bit reservation (settings.h API
#     shape + settings.c NVS_KEY_AI "ai_on" slot + backup
#     profile.home_full_refresh bit 6 (0x40) held).
#   Gate: 14/14 host runners green (case-level registered KNOWN-FAILs stand:
#     ui_kit (1) r93 packed framebuffer + home_recent's upstream-stale test,
#     both in KNOWN_FAILURES.md); push_sink 189/0 (168 + the reservation
#     registry's 21 checks); transfer_ui gains push_yield_regression (8
#     scenarios) + mem_panel_regression (thresholds/formula/modality/view
#     guard; mutation bites: long-press not opening fails the assert at 930);
#     book_management 73 PASS (reader_fullscreen gains the gen25 symbol
#     stand-ins); fullclean exit 0 with the 8-warning baseline intact
#     (app_book.c 7 + ui_pinpad.c 1, compared line by line). Anchors:
#     Read_Pico.bin 4,066,576 B (+3,616 vs gen24.5's 4,062,960; both operands:
#     4,066,576-4,062,960=+3,616), sha256 `de93e5e4…`, seg1 File offs
#     `0x002341d8` (paddr = offs + 0x10008 = 0x002441e0), Load `0x3fc9c700`,
#     len `0x08a8c`; partition-table `9d5d890b…` and ota_data_initial
#     `7d2c7ac4…` are byte-identical to gen24.5; bootloader `cbe2b12a…` ->
#     `834ec762…` (same 21,232 B, build-timestamp difference).
# gen25.1（resume 首屏 GC16 打底热修，一行级、独立热修不并入 gen26）：
#   版本串：0.3.3-pico25.1（三处同步：CMakeLists.txt + 本记录 + 顶部版本串注释）。
#   三项核查（落码前）：① resume 首屏当前档位 = GL16 差分
#   （present 末分支 update_display_mode_diff(APP_PAGE_REFRESH_MODE)，balanced
#   档 APP_PAGE_REFRESH_MODE=MODE_GL16），与书架普通开书同档——push.txt 的干净
#   不来自 request_open 本身，而来自 gen24.4 open_clean / gen25 push_first_open
#   置位 s_open_full_once 后 present 的 update_display_full 分支（GC16）；
#   ② gen24.5→gen25 diff 显示 app_book_request_resume 函数体零改动——既存缺口，
#   非 gen25 回归；③ 让路 restore 复用 app_book_request_resume
#   （app_book_restore_after_push → request_resume），修复后 restore 首屏顺带干净。
#   修法：app_book_request_resume 置位 s_open_full_once（与 open_clean 同族同一
#   标志，present 既有 update_display_full 消费点，一次即止），只动 resume 一条
#   路径——书架/push.txt/自动开书不碰，后续翻页节拍零变化（gen24.4 红线不破）。
#   resume 三种来路全覆盖：首页「继续阅读」卡、开机恢复（boot recovery）、
#   让路 restore 恢复原书。
#   门禁：宿主 runner 14/14 全绿；book_management 同口径双操作数——检出 gen25
#   提交 54f2ddf 同 runner 复跑（补 managed_components 未跟踪依赖）：基线 56 PASS
#   + 2 KNOWN-FAIL = gen25.1 56 PASS + 2 KNOWN-FAIL，PASS 集合逐条 identical
#   （gen25 轮「73 PASS」为旧计数口径，本轮以同 runner 同口径复核为准）；
#   reader_fullscreen 新增 resume 置位断言（变异有牙：撤除置位 → 断言精确红，
#   还原复绿）；ui_refresh_feedback 增 resume 序列回归（首屏一次 GC16 后翻页仍走
#   READER 差分 = 节拍不变）；transfer_ui 不变（绿）；fullclean exit 0，8 警告同
#   基线逐条对表（app_book.c 7 + ui_pinpad.c 1，零新增）。
#   锚：Read_Pico.bin 4,066,576 B（与 gen25 等长，双操作数
#   4,066,576-4,066,576=+0——一行 store 与版本串增长被段对齐吸收），sha256
#   `11bcf643…`；seg1 File offs `0x002341d8`（paddr = offs + 0x10008 =
#   0x002441e0）、Load `0x3fc9c700`、len `0x08a8c`，与 gen25 逐字段一致；
#   partition-table `9d5d890b…`、ota_data_initial `7d2c7ac4…` 与 gen25 逐字节
#   一致；bootloader `834ec762…`→`95ae371c…`（同 21,232 B，编译时间戳差异）。
# Gen25.1 (resume first-screen GC16 base hotfix - one line, standalone, NOT
#   folded into gen26):
#   Version string: 0.3.3-pico25.1 (three places in sync).
#   Three checks (before coding): (1) resume's first screen currently takes the
#   GL16 differential (present's final branch update_display_mode_diff(
#   APP_PAGE_REFRESH_MODE), balanced profile MODE_GL16) - the same tier as an
#   ordinary shelf open; push.txt's cleanliness does not come from request_open
#   itself but from gen24.4's open_clean / gen25's push_first_open setting
#   s_open_full_once, which routes present through update_display_full (GC16);
#   (2) the gen24.5->gen25 diff shows app_book_request_resume's body untouched -
#   a pre-existing gap, not a gen25 regression; (3) the yield restore reuses
#   app_book_request_resume (app_book_restore_after_push -> request_resume), so
#   the restored book's first screen is clean too.
#   Fix: app_book_request_resume sets s_open_full_once (same flag family as
#   open_clean; present's existing update_display_full branch consumes it once),
#   touching the resume path only - shelf / push.txt / auto-open untouched, later
#   turns keep their cadence (the gen24.4 red line holds). All three resume
#   sources covered: the home "continue reading" card, boot recovery, and the
#   yield restore of the original book.
#   Gate: 14/14 host runners green; book_management same-caliber double operand -
#   gen25 commit 54f2ddf checked out and re-run on the same runner (with the
#   untracked managed_components restored): baseline 56 PASS + 2 KNOWN-FAIL =
#   gen25.1 56 PASS + 2 KNOWN-FAIL, PASS sets identical line by line (gen25's
#   "73 PASS" was the old counting caliber - this round re-verifies on the same
#   runner); reader_fullscreen gains the resume-set assertion (mutation bites:
#   removing the set fails the assert exactly, restore returns green);
#   ui_refresh_feedback gains the resume sequence (one first-screen GC16, then
#   turns still take the READER differential = cadence unchanged); transfer_ui
#   unchanged (green); fullclean exit 0 with the 8-warning baseline intact
#   (app_book.c 7 + ui_pinpad.c 1, zero new). Anchors: Read_Pico.bin 4,066,576 B
#   (equal length to gen25; both operands 4,066,576-4,066,576=+0 - the one store
#   plus the version-string growth absorbed by section alignment), sha256
#   `11bcf643…`; seg1 File offs `0x002341d8` (paddr = offs + 0x10008 =
#   0x002441e0), Load `0x3fc9c700`, len `0x08a8c` - identical to gen25 field by
#   field; partition-table `9d5d890b…` and ota_data_initial `7d2c7ac4…`
#   byte-identical to gen25; bootloader `834ec762…` -> `95ae371c…` (same
#   21,232 B, build-timestamp difference).
# gen25.2（补 gen25.1 的漏：首页 continue 卡未走 resume，修改落空）：
#   版本串：0.3.3-pico25.2（三处同步；必须换串——同串会被 TF 卡升级判「版本相同」拒绝）。
#   根因（gen25.1 真机验收失败后诊断）：gen25.1 只在 app_book_request_resume 真身
#   置位 s_open_full_once（首屏 GC16 打底），而首页「最近阅读」continue 卡
#   （app_dashboard.c continue_card_hit_rect 分支）走的是
#   app_book_request_open_from_home——该函数不置位。from_home 的唯一固件调用方
#   正是这张卡，故 gen25.1 的修改对首页场景完全落空；开机恢复（app_main.c）与
#   让路 restore（app_book.c）本就走 resume，不受影响。
#   修法：continue 卡改调 app_book_request_resume(b->path, true)——「从首页恢复
#   最近阅读」本就是 resume 语义；fullscreen 显式 true 与原 from_home 路径的
#   s_requested_fullscreen=true 等价（request_open 内置），行为零损失。三条
#   resume 来路（首页 continue 卡 / 开机恢复 / 让路 restore）自此统一拿首屏打底。
#   门禁：宿主 runner 14/14 全绿；reading_origin_host_test 替身改 resume 并断言
#   fullscreen 实参（变异有牙：continue 卡改回 from_home → 断言红，还原复绿；
#   1102 全网格点击点全部覆盖）；reader_fullscreen 的 resume/from_home 断言保持
#   （from_home 真身未动）；book_management 56 PASS + 2 KNOWN-FAIL 同口径维持；
#   fullclean exit 0，8 警告同基线逐条对表（app_book.c 7 + ui_pinpad.c 1）。
#   锚：Read_Pico.bin 4,066,384 B（相对 gen25/gen25.1 的 4,066,576 为 **-192 B**，
#   双操作数 4,066,384-4,066,576=-192——调用点收敛后代码略减），sha256
#   `8818e193…`；seg1 File offs `0x002341d8`（paddr = offs + 0x10008 =
#   0x002441e0）、Load `0x3fc9c700`、len `0x08a8c`，与 gen25/gen25.1 逐字段一致；
#   partition-table `9d5d890b…`、ota_data_initial `7d2c7ac4…` 与 gen25 逐字节一致；
#   bootloader `95ae371c…`→`a399f196…`（同 21,232 B，编译时间戳差异）。
# Gen25.2 (plugging gen25.1's miss: the home continue card never took resume):
#   Version string: 0.3.3-pico25.2 (three places in sync; the tag must change -
#   the same tag would make the TF-card upgrade path reject the package as
#   "same version").
#   Root cause (diagnosed after gen25.1's failed hardware check): gen25.1 set
#   s_open_full_once (first-screen GC16 base) only in the
#   app_book_request_resume body, while the home "recent reading" continue card
#   (app_dashboard.c's continue_card_hit_rect branch) went through
#   app_book_request_open_from_home - which does not set it. That card is the
#   only firmware caller of from_home, so gen25.1's fix never reached the home
#   scenario; boot recovery (app_main.c) and the yield restore (app_book.c)
#   already took resume and were unaffected.
#   Fix: the continue card now calls app_book_request_resume(b->path, true) -
#   "resume recent reading from home" is resume semantics; the explicit
#   fullscreen=true equals the old from_home path's s_requested_fullscreen=true
#   (set inside request_open), zero behavior loss. All three resume sources
#   (home continue card / boot recovery / yield restore) now share the
#   first-screen base.
#   Gate: 14/14 host runners green; reading_origin_host_test's double becomes
#   resume and asserts the fullscreen argument (mutation bites: reverting the
#   card to from_home fails the assert, restore returns green; all 1,102
#   full-card hit grid points covered); reader_fullscreen's resume/from_home
#   assertions stand (the from_home body is untouched); book_management 56 PASS
#   + 2 KNOWN-FAIL same caliber; fullclean exit 0 with the 8-warning baseline
#   intact (app_book.c 7 + ui_pinpad.c 1). Anchors: Read_Pico.bin 4,066,384 B
#   (**-192 B** vs gen25/gen25.1's 4,066,576; both operands
#   4,066,384-4,066,576=-192 - the call-site convergence slightly shrank the
#   code), sha256 `8818e193…`; seg1 File offs `0x002341d8` (paddr = offs +
#   0x10008 = 0x002441e0), Load `0x3fc9c700`, len `0x08a8c` - identical to
#   gen25/gen25.1 field by field; partition-table `9d5d890b…` and
#   ota_data_initial `7d2c7ac4…` byte-identical to gen25; bootloader
#   `95ae371c…` -> `a399f196…` (same 21,232 B, build-timestamp difference).
# gen26（块0 · 降栈 12K→6K + HWM 打印纠错 + 版本串 bump）：
#   版本串：0.3.3-pico26（三处同步；必须换串——同串会被 TF 卡升级判「版本相同」拒绝）。
#   范围（本代只落块0，块1–5 待续）：① prep 任务栈 12*1024→6*1024（实测峰值
#   ~2.8K，留 ~3.3K 余量，省 6K 内部堆；开书 largest 由 ~19456 抬到 ~25600，>20K
#   闸门故让路鲜少触发、降为保险）；② 修复 HWM 打印——本 port 的
#   portSTACK_TYPE=uint8_t 致 HWM 计数单位=1 字节，旧 `words` 标签误导（gen25
#   曾误读为「4 B/word→9.2K 已占用→不降栈」）；改为显式 `B free / peak used /
#   total`；③ 三处版本串 0.3.3-pico25.2→pico26。调用方图谱：栈/日志均为 prep
#   内部，无外部 API 改动，无调用方影响。
#   门禁：fullclean exit 0，8 警告同基线逐条对表（app_book.c 7 + ui_pinpad.c 1，
#   零新增）；宿主 runner 14/14 全绿（book_management 56 PASS + 2 KNOWN-FAIL 同
#   口径；ui_kit 的 r93 KNOWN-FAIL 容忍为非回归）；无变异回归。
#   锚：Read_Pico.bin 4,066,400 B（相对 gen25.2 的 4,066,384 为 **+16 B**，双操作数
#   4,066,400-4,066,384=+16——HWM 打印串变长 + 版本串缩短净增，rodata 增长；
#   代码段 seg1 未动）；sha256 `39309cd1…`；seg1 File offs `0x002341d8`（paddr =
#   offs + 0x10008 = 0x002441e0）、Load `0x3fc9c700`、len `0x08a8c`，与 gen25.2
#   逐字段一致；partition-table `9d5d890b…`、ota_data_initial `7d2c7ac4…` 与
#   gen25.2 逐字节一致；bootloader `c720f1bc…`（同 21,232 B，时间戳差异 vs gen25.2
#   的 `a399f196…`）。
# Gen26 (block 0 - stack 12K->6K + HWM print fix + version bump):
#   Version string: 0.3.3-pico26 (three places in sync; the tag must change - the
#   same tag would make the TF-card upgrade reject the package as "same version").
#   Scope (this gen ships block 0 only; blocks 1-5 pending): ① prep task stack
#   12*1024 -> 6*1024 (measured peak ~2.8K, ~3.3K headroom, saves 6K internal
#   heap; book-open largest rises ~19456 -> ~25600, above the 20K gate so yield
#   rarely fires and becomes insurance); ② HWM print fix - this port's
#   portSTACK_TYPE=uint8_t makes the HWM count unit 1 byte, so the old `words`
#   label misled (gen25 misread it as "4 B/word -> 9.2K used -> keep 12K"); now
#   prints explicit `B free / peak used / total`; ③ 0.3.3-pico25.2 -> pico26.
#   Caller graph: stack + log are prep-internal, no external API change, no
#   caller impact.
#   Gate: fullclean exit 0, 8-warning baseline intact (app_book.c 7 + ui_pinpad.c
#   1, zero new); 14/14 host runners green (book_management 56 PASS + 2 KNOWN-FAIL
#   same caliber; ui_kit's r93 KNOWN-FAIL tolerated as non-regression); no
#   mutation regression.
#   Anchors: Read_Pico.bin 4,066,400 B (**+16 B** vs gen25.2's 4,066,384; both
#   operands 4,066,400-4,066,384=+16 - the longer HWM print string net of the
#   shorter version string, rodata growth; the code segment seg1 is untouched);
#   sha256 `39309cd1…`; seg1 File offs `0x002341d8` (paddr = offs + 0x10008 =
#   0x002441e0), Load `0x3fc9c700`, len `0x08a8c` - identical to gen25.2 field by
#   field; partition-table `9d5d890b…` and ota_data_initial `7d2c7ac4…`
#   byte-identical to gen25.2; bootloader `c720f1bc…` (same 21,232 B, timestamp
#   difference vs gen25.2's `a399f196…`).
# gen26.1（块1–5 落码 + main 侧挂接 · AI 查词全链：0x06/0x07 协议 + 蓝牙代理 +
#   搭车离线队列 + 预取缓存 + UI 释义卡 + AI 开关位）：
#   版本串：0.3.3-pico26.1（三处同步；相对块0 的 pico26 必须再换串——TF 卡升级路径
#   判「版本相同」拒绝，块0 用户靠这一跳升级到 AI 包）。
#   范围：① push_proto.h 登记 PUSH_OP_AI_QUERY=0x06 / PUSH_OP_AI_RESULT=0x07 与
#   STATUS 0x17(设备→手机请算释义)/0x18(能力位图)，「登记即冻结」一代一行 + Python
#   参考同步；② ai_ridealong.c 扁平缓冲离线队列（MAGIC 0x5241、去重入队、出队
#   前移、无分配），设备侧 PSRAM 64 词（4,166 B）；③ push_sink.c 胶水——未武装
#   闸前挂 0x06/0x07、CONNECT 锁区内上报 CAPS + 回流队列、0x07 释义进预取缓存
#   (8 词环形) + UI 一次性槽；④ main 挂接：app_main 挂 PSRAM 缓冲 +
#   set_ai_enabled(true)（NVS 持久化，缺省关）、app_book 点词喂
#   push_sink_ai_tap_word、app_loop 500ms 轮询 take_ai_def → app_book_ai_def_ready
#   出卡（无面板开着才开，不抢焦点；释义留预取缓存下次点词秒出）、
#   dict_popup_show_def 外部释义暂存（复用同一折行/绘制/保存 chip 链路）。
#   调用方图谱（纪律①）：七个新 API 全部「声明/实现/单一调用方」三点闭环，无孤儿
#   入口、无漏接；push_sink_ai_enabled 为预留查询口暂无调用方。APK 无需改动——
#   waitStatus 对非候选 STATUS op（0x17/0x18）丢弃继续等，推送流不受影响；手机端
#   AI 计算属后续应用侧工作（Python 参考 push_pico.py 已同步编解码，可手动对测）。
#   门禁：宿主 runner 248 checks, 0 red（gen26 新基线）；fullclean exit 0，7 警告
#   与紧邻基线逐条一致、零新增；块0 真机已验证（HWM free+used=6144 ×9、开大书
#   推送 verify ok 零拒收、largest 回 31744、boot 日志 seg1 paddr=002441e0 与
#   口径公式逐字吻合）。
#   锚：Read_Pico.bin 4,070,032 B（相对块0 4,066,400 为 **+3,632 B**，双操作数
#   4,070,032-4,066,400=+3,632）；段级净和 seg0(DROM)+304 / seg2(IRAM)-304 /
#   seg3(IROM)+3,336 / seg4 len+304 = **+3,640**，尾注对齐差 **-8**（镜像尾 =
#   零填充 + 1B checksum + 32B validation hash，文件按 16B 对齐收尾；段和增长
#   挪动对齐余数，块0 尾 48B / 本代 40B）——**分项净和 + 尾注差 = 总差，逐分
#   闭合**（gen26 新门禁固定项首战：现场差 8 当场查清为尾注填充，非段级错误）。
#   seg1 File offs `0x00234308`（paddr = offs + 8 + 0x10000 = `0x00244310`）、
#   Load `0x3fc9c700`、len `0x08a8c`——**Load/len 与块0 零偏移，双锚守住**；
#   File offs +0x130=+304 系 seg0(DROM) 增长顺移，vaddr 不动。
#   sha256 `d45c7096…`；partition-table `9d5d890b…`、ota_data_initial `7d2c7ac4…`
#   与块0 逐字节一致；bootloader `15f97791…`（同 21,232 B，时间戳差异）。
# Gen26.1 (blocks 1-5 landed + main-side attach - the full AI lookup chain:
#   0x06/0x07 protocol + BLE proxy + ride-along offline queue + prefetch cache +
#   UI definition card + AI switch bit):
#   Version string: 0.3.3-pico26.1 (three places in sync; the tag must change
#   again from block 0's pico26 - the TF-card upgrade path rejects "same version",
#   and block-0 users need this hop to reach the AI package).
#   Scope: ① push_proto.h registers PUSH_OP_AI_QUERY=0x06 / PUSH_OP_AI_RESULT=0x07
#   plus STATUS 0x17 (device->phone, please compute) / 0x18 (capability bitmap),
#   "registration is freeze" one row per generation + Python reference sync;
#   ② ai_ridealong.c flat-buffer offline queue (MAGIC 0x5241, dedup enqueue,
#   compact dequeue, no allocation), 64 words in device PSRAM (4,166 B);
#   ③ push_sink.c glue - 0x06/0x07 mounted ahead of the unarmed gate, CAPS
#   report + queue flush inside the CONNECT lock, 0x07 definitions into the
#   prefetch cache (8-word ring) + a one-shot UI slot; ④ main attach: app_main
#   attaches the PSRAM buffer + set_ai_enabled(true) (NVS-persisted, default
#   off), app_book feeds push_sink_ai_tap_word on taps, app_loop polls
#   take_ai_def every 500 ms -> app_book_ai_def_ready opens the card (only when
#   no panel is open - never steals focus; the definition stays in the prefetch
#   cache for an instant re-show on the next tap), dict_popup_show_def stages
#   external definitions (reusing the same wrap/draw/save-chip chain).
#   Caller graph (discipline ①): all seven new APIs are closed loops of
#   "declaration / implementation / single caller", no orphan entries, nothing
#   unwired; push_sink_ai_enabled is a reserved query entry with no caller yet.
#   The APK needs no change - waitStatus drops non-candidate STATUS ops
#   (0x17/0x18) and keeps waiting, so the push flow is unaffected; the phone-side
#   AI computation is follow-up app work (the Python reference push_pico.py is
#   already in sync codec-wise and can be used for manual round-trips).
#   Gate: host runner 248 checks, 0 red (the gen26 baseline); fullclean exit 0,
#   7 warnings identical to the adjacent baseline line by line, zero new; block-0
#   hardware verified (HWM free+used=6144 x9, large-book push verify ok with zero
#   rejections, largest back to 31744, boot-log seg1 paddr=002441e0 matching the
#   formula character for character).
#   Anchors: Read_Pico.bin 4,070,032 B (**+3,632 B** vs block 0's 4,066,400; both
#   operands 4,070,032-4,066,400=+3,632); segment-level sum seg0(DROM)+304 /
#   seg2(IRAM)-304 / seg3(IROM)+3,336 / seg4 len+304 = **+3,640**, footer
#   alignment delta **-8** (the image tail = zero padding + 1B checksum + 32B
#   validation hash, file closed at 16B alignment; the segment growth shifts the
#   alignment residue, block 0's tail 48B vs this gen's 40B) - **sum of parts +
#   footer delta = total, closed to the byte** (the gen26 fixed gate item's first
#   live run: the on-the-spot 8-byte gap was traced to footer padding, not a
#   segment-level error).
#   seg1 File offs `0x00234308` (paddr = offs + 8 + 0x10000 = `0x00244310`),
#   Load `0x3fc9c700`, len `0x08a8c` - **Load/len zero-offset vs block 0, the
#   double anchor holds**; File offs +0x130=+304 is a pure seg0(DROM) growth
#   shift, vaddr unmoved.
#   sha256 `d45c7096…`; partition-table `9d5d890b…` and ota_data_initial
#   `7d2c7ac4…` byte-identical to block 0; bootloader `15f97791…` (same 21,232 B,
#   timestamp difference).
# gen26.2（回流时序 defect 热修 · AI 链首验暴露、当日裁定即修的小包）：
#   根因（真机首验判读，gen26ai-log 实证）：ai_on_connect()（CAPS 0x18 +
#   ai_flush_queue() 逐词 0x17）在 BLE CONNECT 事件内调用，而手机 CCCD 订阅
#   迟到 ~470ms；notify_status() 对未订阅 peer 直接丢帧（if (!s_status_
#   subscribed) { ESP_LOGD dropped; return; }——LOGD 在 release 构建不可见，
#   丢帧静默）；ai_flush_queue() 不论投递成败逐个 dequeue——帧丢词也被吃，
#   离线队列每次重连被静默清空；订阅处理器只置旗 + 日志，不重发 s_last_status。
#   三重叠加后果：连接后订阅窗口内到达的点词全部流失。宿主测试不可见（不模拟
#   GAP 回调），仅设备级时序暴露——真机首验「功能全绿但无任何 AI 界面」即此
#   defect 的正确表现（APK 无应答方时无 AI 界面本是半链预期，但回流路径本身
#   已断，半链也验不到回流）。
#   修复（push_sink.c 三处，最小改动）：① ai_on_connect() 更名 ai_on_subscribed()
#   并补双语根因注释；② CONNECT 处理器移除调用、留注释说明改在 SUBSCRIBE 交付；
#   ③ SUBSCRIBE 处理器在 cur_notify != 0（peer 已订阅，可收帧的首个时刻）时
#   持锁调用 ai_on_subscribed()——订阅前 ~470ms 窗口内入队的点词在此一并交付。
#   协议上下文扩展（词+书+位置，微信读书式架构）不现在动，并入 APK 应用侧
#   立项，C/Java/Python 三端同步登记即冻结。
#   门禁：宿主 runner 248 checks, 0 red（gen26 基线不松动）；fullclean exit 0，
#   真 fullclean 编 1,244 个编译单元，8 警告 = §33 原始全量基线（app_book.c 7 +
#   ui_pinpad.c 1）逐条一致、零新增——§38 挂账的「8→7 漂移」至此结案：旧
#   「fullclean」日志只编 13 个文件，实为增量构建覆盖伪影，ui_pinpad.c 未被
#   重编故少一警告；本次真全量复现 8 警告坐实此判。
#   锚：Read_Pico.bin 4,070,048 B（相对 gen26.1 的 4,070,032 为 **+16 B**，双
#   操作数 4,070,048-4,070,032=+16）；段级净和 seg3(IROM)+20（0x18c588→
#   0x18c59c，修复代码在 seg3）+ 尾注差 **-4**（40→36，16B 对齐余数挪动）
#   ——**分项净和 + 尾注差 = 总差，逐分闭合**（新门禁固定项再战）；seg1
#   File offs `0x00234308`（paddr = offs + 8 + 0x10000 = `0x00244310`）、
#   Load `0x3fc9c700`、len `0x08a8c`——与 gen26.1 **零偏移**，双锚再守。
#   sha256 `5a97ef4a…`；partition-table `9d5d890b…`、ota_data_initial
#   `7d2c7ac4…` 与 gen26.1 逐字节一致；bootloader `36c0a5e3…`（同 21,232 B，
#   时间戳差异 vs gen26.1 的 `15f97791…`）。
#   真机复测面（待验）：半链回归全绿（同 gen26.1 面）；有 PC 蓝牙适配器则用
#   android-tools/push_pico.py 做全链回流复测——断开态点词入队→重连→订阅
#   事件后 CAPS+逐词 0x17 交付→PC 回 0x07→释义卡弹出。
# Gen26.2 (reflux-timing defect hotfix - exposed by the first live AI-chain
#   verification, ruled "fix" and shipped same-day as a small package):
#   Root cause (first-live-verdict reading, gen26ai-log evidence):
#   ai_on_connect() (CAPS 0x18 + ai_flush_queue() word-by-word 0x17) ran inside
#   the BLE CONNECT event, while the phone's CCCD subscription lands ~470 ms
#   late; notify_status() drops frames for unsubscribed peers outright
#   (if (!s_status_subscribed) { ESP_LOGD dropped; return; } - LOGD invisible
#   in release builds, so the drop is silent); ai_flush_queue() dequeues every
#   word regardless of delivery outcome - dropped frames eat the words, and the
#   offline queue is silently cleared on every reconnect; the subscribe handler
#   only sets the flag + logs, never resending s_last_status. Triple-stacked
#   consequence: every word tapped inside the post-connect pre-subscribe window
#   is lost. Invisible to host tests (GAP callbacks are not simulated), only
#   device-level timing exposes it - the first live run's "all functions green
#   but no AI UI at all" is exactly this defect's correct behavior (no AI UI
#   without a PC responder is expected for the half-chain, but the reflux path
#   itself was already broken, so even the half-chain could not see reflux).
#   Fix (push_sink.c, three edits, minimal): ① ai_on_connect() renamed
#   ai_on_subscribed() with a bilingual root-cause comment; ② the CONNECT
#   handler drops the call, leaving a comment that delivery moved to SUBSCRIBE;
#   ③ the SUBSCRIBE handler calls ai_on_subscribed() under the lock when
#   cur_notify != 0 (peer subscribed - the first moment frames can be
#   received) - words enqueued during the ~470 ms pre-subscribe window are
#   delivered here too. The protocol context extension (word + book + position,
#   the WeChat-Reading-style architecture) is NOT moved now; it folds into the
#   APK app-side project, registered as frozen across C/Java/Python.
#   Gate: host runner 248 checks, 0 red (the gen26 baseline unshaken); fullclean
#   exit 0, a true fullclean of 1,244 compilation units, 8 warnings = the §33
#   original full baseline (app_book.c 7 + ui_pinpad.c 1) line by line, zero
#   new - the "8->7 drift" booked in §38 closes here: the old "fullclean" log
#   compiled only 13 files, actually an incremental-build coverage artifact -
#   ui_pinpad.c was not recompiled hence one warning fewer; this true full run
#   reproduces the 8 and settles the call.
#   Anchors: Read_Pico.bin 4,070,048 B (**+16 B** vs gen26.1's 4,070,032; both
#   operands 4,070,048-4,070,032=+16); segment-level sum seg3(IROM)+20
#   (0x18c588->0x18c59c, the fix lives in seg3) + footer delta **-4** (40->36,
#   the 16B-alignment residue shifted) - **sum of parts + footer delta = total,
#   closed to the byte** (the new fixed gate item's second live run); seg1 File
#   offs `0x00234308` (paddr = offs + 8 + 0x10000 = `0x00244310`), Load
#   `0x3fc9c700`, len `0x08a8c` - **zero-offset vs gen26.1, the double anchor
#   holds again**.
#   sha256 `5a97ef4a…`; partition-table `9d5d890b…` and ota_data_initial
#   `7d2c7ac4…` byte-identical to gen26.1; bootloader `36c0a5e3…` (same 21,232 B,
#   timestamp difference vs gen26.1's `15f97791…`).
#   Live re-test surface (pending): half-chain regression all green (same
#   surface as gen26.1); with a PC Bluetooth adapter, run the full-chain reflux
#   re-test with android-tools/push_pico.py - words tapped while disconnected
#   enqueue -> reconnect -> CAPS + word-by-word 0x17 delivered at the SUBSCRIBE
#   event -> PC replies 0x07 -> the definition card opens.
#
# 重锚记录（gen26.3，M1 联测三项反馈根治——用户裁定 A① 本地优先 + B② 停监听/恢复，
#   届时不重新决策；M1 主链已于 gen26.2 结案推入，本轮只做三项反馈的修复）：
#   背景（M1 联测回报「功能正常」+ 三条反馈，gen26.2ai-log 双实证）：
#     ① 监听查词与发送墨水屏互斥、push.txt 打不开；
#     ② 词典有释义的词显示词典内容、与 APK 返回不一致；
#     ③ 词典查失败的才返回 AI 结果（12/14 次点词白烧 LLM）。
#   根因（读码 + 日志双实证）：
#     ②③ 同根——搭车协议每词必发 0x17，而 app_book_ai_def_ready 的面板门
#     （s_reader_panel != NONE 即丢帧）把词典卡开着时的 AI 释义全丢：一词两答 +
#     白烧 LLM；① 的两半根因不同——APK 侧 doPush 的 resetState 不动 listenMode
#     （M1.1 修），设备侧 gen18 18-a 在 CONNECTED 态即弹回、撞上 gen25 后台保活
#     的长驻监听会话（本代②修）。
#   改动（gen26.3 三项，最小集）：
#     ① app_book.c start_dict_lookup：push_sink_ai_tap_word 移到 dict_popup_open
#        之后，仅 hit && english && !found（含词典不可用）才发——AI = 词典的补充
#        （未收词才补位），不是替代；M2 场景不经本入口，协议不动；
#     ② app_book.c on_tick 18-a：弹回条件 {CONNECTED,RECEIVING,PROCESSING} 收紧为
#        {RECEIVING,PROCESSING}——数据真正在飞才弹；监听态长驻 CONNECTED 不弹，
#        M1.1 推完恢复监听的流程同样不弹；
#     ③ push_sink.c：notify_status 改返回 bool（交付确认），ai_flush_queue 改
#        dequeue-on-success——notify 确认成功才出队，失败（未订阅/mbuf/rc≠0）词留队，
#        帧已由既有 100ms 重投通道兜底；杜绝 gen26.2 注释挂账的「帧丢词被吃」。
#   M1 遗留缺陷顺手结案：app_loop.c 的 AI 释义轮询分支（M1 引入）漏同步宿主桩
#   （app_content_open.h 桩缺 ai_def_ready 声明 + push_sink_take_ai_def 定义），
#   app_loop/book_management 两 runner 编译红躺一代——与 gen21 cancel_why 桩同型
#   第三次。本轮补桩 + app_loop_host_test 三分支用例（出卡透传/丢弃 press/拒绝不抢/
#   槽空零副作用），变异三连擒（去 ready 门、去 continue、等价变换对照）。
#   门禁：宿主 14/14 runner 全绿（push_sink 248 checks, 0 red；app_loop 55 场景
#   + 新增 AI 三分支）；fullclean（删 build 真全量）exit 0，C 1244 / CXX 28 /
#   ASM 26 = 1,298 编译单元全量在编（gen26.2 基线持平，本轮零增删单元）；8 警告
#   与 §33 原始基线逐条一致（app_book.c 7 + ui_pinpad.c 1，均不在改动面）；变异
#   有牙 3/3。
#   锚：Read_Pico.bin 4,070,144 B（相对 gen26.2 的 4,070,048 为 **+96 B**，双操作数
#   4,070,144-4,070,048=+96）；段级净和 seg0(DROM)+48（0x2342e8→0x234318，
#   app_book.c 双语注释增量）+ seg3(IROM)+48（0x18c59c→0x18c5cc，三处逻辑）
#   + 尾注差 0——**分项净和 + 尾注差 = 总差，逐分闭合**；seg1 File offs
#   `0x00234338`（gen26.2 为 0x00234308，+0x30=48B 顺移——DROM 段增长），
#   paddr = 0x00234338 + 8(段头) + 0x10000(ota_0 基址) = `0x00244340`（预测）；
#   seg1 Load `0x3fc9c700` 世代不变锚不动、len `0x08a8c` 与 gen26.2 持平——双锚再守。
#   sha256 `6728fbaa…`；partition-table `9d5d890b…`、ota_data_initial `7d2c7ac4…`
#   与 gen26.2 逐字节一致；bootloader `40396978…`（同 21,232 B，时间戳差异 vs
#   gen26.2 的 `36c0a5e3…`）。
#   本代干跑 zip 不 push、不打 tag（门禁全绿后的既定纪律）；APK 侧 M1.1（v1.3）
#   随后单独提交，zip 重打（RELEASE_NOTES gen22 残留修正 + 内嵌 v1.3）随真机一轮
#   复测通过后进行。真机复测面（待验）：① 命中词串口无 0x17；② 未收词出 AI 卡；
#   ③ 监听态开 push.txt 不弹回；④ 推送后自动恢复点词全链；⑤ 回归面。
# Gen26.3 (roots out the three M1 live-test findings - user ruling A1 local-first
#   + B2 stop-listen/auto-restore, decided once and for all; M1's main chain
#   already closed and pushed at gen26.2, this generation only fixes the three
#   findings):
#   Background (M1 live report "functions green" + three findings, gen26.2ai-log
#   double evidence): (1) listening and sending are mutually exclusive and
#   push.txt will not open; (2) a word the dictionary holds shows the dictionary
#   text, differing from the APK's answer; (3) only dictionary misses return AI
#   results (12 of 14 taps burned LLM for nothing).
#   Root causes (code reading + log double evidence): (2)(3) share one root - the
#   ride-along sends 0x17 for every tap while app_book_ai_def_ready's panel gate
#   (drops when s_reader_panel != NONE) discards the AI definition whenever the
#   dictionary card is open: two answers per tap plus wasted LLM calls; (1)'s two
#   halves have different roots - APK-side doPush's resetState leaves listenMode
#   alone (fixed in M1.1), and device-side gen18 18-a bounces at CONNECTED, which
#   collides with gen25's background keep-alive parking the listening session
#   (fixed here as item 2).
#   Changes (gen26.3, three items, minimal): (1) app_book.c start_dict_lookup:
#   push_sink_ai_tap_word moves after dict_popup_open and fires only on
#   hit && english && !found (including the dictionary being unavailable) - AI is
#   the dictionary's supplement (fills uncollected words), not a replacement; the
#   M2 scenarios never pass through this entry, so the protocol stays put;
#   (2) app_book.c on_tick 18-a: the bounce condition {CONNECTED,RECEIVING,
#   PROCESSING} tightens to {RECEIVING,PROCESSING} - only data actually in flight
#   bounces; a parked CONNECTED never does, and neither does M1.1's post-push
#   listen-restore path; (3) push_sink.c: notify_status now returns bool
#   (delivery confirmation) and ai_flush_queue moves to dequeue-on-success -
#   dequeue only after notify confirms success; on failure (unsubscribed / mbuf
#   shortage / rc!=0) the word stays queued with its frame already scheduled for
#   the existing 100 ms retry, ending the gen26.2-noted "dropped frame, eaten
#   word".
#   M1 leftover closed in passing: the AI definition poll branch (added by M1)
#   missed its host stubs (app_content_open.h stub lacks the ai_def_ready
#   declaration + push_sink_take_ai_def definition), so the app_loop and
#   book_management runners stayed compile-red for a generation - the third
#   incident of the same shape as the gen21 cancel_why stub. This round backfills
#   the stubs and adds app_loop_host_test's three-branch cases (card present +
#   word/def passthrough + dropped press / refusal never steals / empty slot has
#   zero side effects), with three consecutive mutation kills (drop the ready
#   gate, drop the continue, plus an equivalence-control).
#   Gate: host 14/14 runners green (push_sink 248 checks, 0 red; app_loop 55
#   scenarios plus the new AI three-branch cases); fullclean (build dir removed,
#   a true full run) exit 0, C 1244 / CXX 28 / ASM 26 = 1,298 compilation units
#   all compiled (flat vs the gen26.2 baseline - this round adds and removes no
#   unit); 8 warnings line-identical to the §33 original baseline (app_book.c 7 +
#   ui_pinpad.c 1, neither in the change surface); mutation teeth 3/3.
#   Anchors: Read_Pico.bin 4,070,144 B (**+96 B** vs gen26.2's 4,070,048; both
#   operands 4,070,144-4,070,048=+96); segment-level sum seg0(DROM)+48
#   (0x2342e8->0x234318, the app_book.c bilingual-comment growth) + seg3(IROM)+48
#   (0x18c59c->0x18c5cc, the three logic edits) + footer delta 0 - **sum of parts
#   + footer delta = total, closed to the byte**; seg1 File offs `0x00234338`
#   (gen26.2 was 0x00234308, +0x30=48B shifted - the DROM segment grew), paddr =
#   0x00234338 + 8 (segment header) + 0x10000 (ota_0 base) = `0x00244340`
#   (predicted); seg1 Load `0x3fc9c700` generation-invariant anchor unmoved, len
#   `0x08a8c` flat vs gen26.2 - the double anchor holds again.
#   sha256 `6728fbaa…`; partition-table `9d5d890b…` and ota_data_initial
#   `7d2c7ac4…` byte-identical to gen26.2; bootloader `40396978…` (same 21,232 B,
#   timestamp difference vs gen26.2's `36c0a5e3…`).
#   This generation's dry-run zip is not pushed and no tag is cut (the standing
#   discipline after an all-green gate); APK-side M1.1 (v1.3) commits separately
#   afterwards, and the zip rebuild (RELEASE_NOTES gen22-residue fix + embedding
#   v1.3) follows one green hardware re-test round. Live re-test surface
#   (pending): (1) a dictionary hit sends no 0x17 on the serial log; (2) an
#   uncollected word surfaces the AI card; (3) push.txt opens without bouncing
#   while listening; (4) the full word chain works again after a push's automatic
#   listen restore; (5) the regression surface.
#
# 重锚记录（gen26.3 五项终版——上述三项之外，用户修订令新增 ④ 查询态卡 + 双门、
#   ⑤ 词卡容量确认，M1.1 新增 ③ Prompt 40 字硬约束；版本串仍为 0.3.3-pico26.3
#   不换代，产物重锚）：
#   ④ app_book.c/dict_popup.c：miss 分支在发 0x17 前先弹「AI 查询中…」卡
#     （dict_popup_show_pending 暂存词 + 状态行 + chip 初值）；app_book_ai_def_ready
#     双门重写——门 A（保留）：无面板直接开卡 / 有他人面板不抢焦点、释义丢弃
#     （已在预取缓存，下次点同词秒出）；门 B（新）：开着的正是本词查询卡才
#     dict_popup_show_def 原地更新（差分局部刷新不二次打底），不同词不更新。
#     谓词按截断头前缀匹配（s_word 截 47 字 vs 0x07 槽词可达 63 字）。on_tick
#     15s 超时降级「AI 查询失败」notice（真实状态非假卡）。查询态 chip 点击忽略
#     （DICT_POPUP_TAP_NOT_READY，零变化不 redraw；卡外点击照旧 MISSED 关闭）。
#     顺手修真 bug：show_def 钳位落字符边界（ASan 实证的栈溢出读）。
#   ⑤ dict_popup_ai_capacity_chars 与 draw 收行条件同口径实算：竖屏 10 行 ×
#     20 字 = 200 全角字符（横屏 400）；40 字 ×3 字节 =120 ≤169 帧帽 ≤200 容量，
#     预算自洽无需微调。
#   M1.1③ AiClient.java：systemPrompt 改「释义不超过 40 字，一句核心词义 + 一个
#     短语示例」+ 强制纯文本（旧文 50 字 + 词性格式移除）；AiClientTest 补 7 断言。
#   门禁：宿主 15/15 runner 全绿（push_sink 248 checks, 0 red；dict_popup 第 15
#     runner 含双门谓词/容量 200/400/折行边界/超时/reset，变异 5/5 全擒）；
#     fullclean（删 build 真全量）exit 0，C 1244 / CXX 28 / ASM 26 = 1,298 编译
#     单元与 gen26.2 基线持平；8 警告逐条同基线（app_book.c 7 + ui_pinpad.c 1，
#     均不在改动面）；seg1 双锚再守；段级算术闭合。
#   锚：Read_Pico.bin 4,070,704 B（相对 gen26.2 的 4,070,048 为 **+656 B**，双操作数
#   4,070,704-4,070,048=+656）；段级净和 seg0(DROM)+160（0x2342e8→0x234388，
#   本轮注释与容量函数）+ seg3(IROM)+496（0x18c59c→0x18c78c，双门/超时/
#   NOT_READY 逻辑）+ 尾注差 0——**分项净和 + 尾注差 = 总差，逐分闭合**；
#   seg1 File offs `0x002343a8`（gen26.2 为 0x00234308，+0xa0=160B 顺移——
#   DROM 段增长），paddr = 0x002343a8 + 8(段头) + 0x10000(ota_0 基址)
#   = `0x002443b0`（预测）；seg1 Load `0x3fc9c700` 世代不变锚不动、len `0x08a8c`
#   与 gen26.2 持平——双锚再守。
#   sha256 `d441107b…`；partition-table `9d5d890b…`、ota_data_initial `7d2c7ac4…`
#   与 gen26.2 逐字节一致；bootloader `79b992dd…`（同 21,232 B，时间戳差异 vs
#   gen26.2 的 `36c0a5e3…`）。
#   本代干跑 zip 不 push、不打 tag；zip 重打（RELEASE_NOTES gen22 残留修正 +
#   内嵌 v1.3 APK）随真机六验收通过后进行（既定纪律）。真机复测面（待验）：
#   ① 命中词串口无 0x17；② miss 卡立即出现→2-5s 原地变释义无二次打底；③ 监听态
#   开 push.txt 不弹回；④ 推送后自动恢复点词全链；⑤ 40 字实证无半行截断；
#   ⑥ 回归面。
# Gen26.3 five-item final (beyond the three items above, the user's revised order
#   adds 4 the querying card + double gate and 5 the word-card capacity check,
#   and M1.1 adds 3 the 40-character prompt constraint; the version tag stays
#   0.3.3-pico26.3 - no new fork - and the artifacts are re-anchored):
#   (4) app_book.c/dict_popup.c: the miss branch now pops the "AI 查询中…" card
#   before sending 0x17 (dict_popup_show_pending stashes the word + status line +
#   chip seed); app_book_ai_def_ready is rewritten as a double gate - gate A
#   (keep): no panel opens the card directly / another panel never steals focus
#   and the definition is dropped (already in the prefetch cache, the next tap on
#   the same word is instant); gate B (new): only when the open card is THIS
#   word's querying card does dict_popup_show_def update in place (partial
#   refresh, no second underpaint), a different word never updates. The predicate
#   matches on the truncated head prefix (s_word truncated to 47 chars vs the
#   0x07 slot word's up to 63). on_tick degrades to an "AI 查询失败" notice after
#   15 s (a real state, not a fake card). A querying card's chip tap is ignored
#   (DICT_POPUP_TAP_NOT_READY, zero change so no redraw; taps outside the card
#   still close via MISSED). Real bug fixed in passing: show_def's clamp now
#   lands on a character boundary (an ASan-proven stack overread).
#   (5) dict_popup_ai_capacity_chars computes at the same yardstick as the
#   draw's line cutoff: portrait 10 lines x 20 chars = 200 full-width characters
#   (landscape 400); 40 chars x 3 bytes = 120 <= 169 frame cap <= 200 capacity,
#   the budget is self-consistent, no tuning needed.
#   M1.1(3) AiClient.java: the system prompt becomes "definition at most 40
#   characters, one core sense + one phrase example" and enforces plain text (the
#   old 50-character + part-of-speech format is removed); AiClientTest gains 7
#   assertions.
#   Gate: host 15/15 runners green (push_sink 248 checks, 0 red; dict_popup is
#   runner 15 covering the double-gate predicate / capacity 200/400 / wrap
#   bounds / timeout / reset, mutations 5/5 all caught); fullclean (build dir
#   removed, a true full run) exit 0, C 1244 / CXX 28 / ASM 26 = 1,298 units
#   flat vs the gen26.2 baseline; 8 warnings line-identical to baseline
#   (app_book.c 7 + ui_pinpad.c 1, neither in the change surface); the seg1
#   double anchor holds; section arithmetic closes.
#   Anchors: Read_Pico.bin 4,070,704 B (**+656 B** vs gen26.2's 4,070,048; both
#   operands 4,070,704-4,070,048=+656); segment-level sum seg0(DROM)+160
#   (0x2342e8->0x234388, this round's comments and the capacity function) +
#   seg3(IROM)+496 (0x18c59c->0x18c78c, the double gate / timeout / NOT_READY
#   logic) + footer delta 0 - **sum of parts + footer delta = total, closed to
#   the byte**; seg1 File offs `0x002343a8` (gen26.2 was 0x00234308, +0xa0=160B
#   shifted - the DROM segment grew), paddr = 0x002343a8 + 8 (segment header) +
#   0x10000 (ota_0 base) = `0x002443b0` (predicted); seg1 Load `0x3fc9c700`
#   generation-invariant anchor unmoved, len `0x08a8c` flat vs gen26.2 - the
#   double anchor holds again.
#   sha256 `d441107b…`; partition-table `9d5d890b…` and ota_data_initial
#   `7d2c7ac4…` byte-identical to gen26.2; bootloader `79b992dd…` (same 21,232 B,
#   timestamp difference vs gen26.2's `36c0a5e3…`).
#   This generation's dry-run zip is not pushed and no tag is cut; the zip
#   rebuild (RELEASE_NOTES gen22-residue fix + embedding the v1.3 APK) follows a
#   green six-point hardware acceptance (standing discipline). Live re-test
#   surface (pending): (1) a dictionary hit sends no 0x17 on the serial log;
#   (2) a miss card appears at once, then becomes the definition in place within
#   2-5 s with no second underpaint; (3) push.txt opens without bouncing while
#   listening; (4) the full word chain works again after a push's automatic
#   listen restore; (5) the 40-character prompt shows no half-line truncation;
#   (6) the regression surface.
# 重锚记录（2026-10-24，gen26.4：词典卡「AI」入口——M1.2.1 配套设备侧轮，与 APK
#   侧 M1.2.1（模板库三处状态同步热修 v1.4.1/code6）并行；版本串换代 26.3→26.4）：
#   ① dict_popup.c/h：新增 DICT_POPUP_TAP_AI；dict_popup_ai_rect()（保存 chip
#     左侧同头部行，96x52，x=save.x-12-AI_W）；dict_popup_tap 优先判定 AI chip
#     （仅 s_ai_on && !s_pending 命中，查询态点击忽略）；draw 加 AI chip（深底反白）
#     并让位头部行宽；force_ai()（show_pending 后置 s_ai_over_dict）/
#     ai_over_dict()（回退判据）/ set_ai_enabled() / word() getter。
#   ② app_book.c：点词命中路径 dict_popup_set_ai_enabled(push_sink_ai_enabled())；
#     reader_panel_action 的 DICT_POPUP_TAP_AI → push_sink_ai_tap_word(本词)
#     （命中缓存秒出/连上发 0x17/离线入队）+ dict_popup_force_ai() 切查询态；
#     on_tick 15s 超时分流——ai_over_dict() 为真则 dict_popup_open(本词) 回退
#     重开词典卡（不丢词典内容），否则原「AI 查询失败」notice。协议零变（0x17 照发）。
#   ③ tools/dict_popup_host_test.c：test_ai_entry()——AI chip 显隐（默认无/开后有）、
#     AI 区点击 MISSED（关）/TAP_AI（开）、AI 与 save chip 不重叠、force_ai 后
#     is_pending+ai_over_dict 真、查询态卡不画 AI、miss 路径 show_pending 不带
#     over_dict、show_def 后 over_dict 清零、word() getter。
#   门禁：16/16 runner 全绿（dict_popup 第 15 runner 含 AI entry 断言，变异 6/6
#     全擒——新增 M6「AI 入口门禁忽略 s_ai_on」）；fullclean（删 build 真全量）
#     exit 0；8 警告逐条同基线（app_book.c 7 + ui_pinpad.c 1，均不在改动面）；
#     seg1 双锚再守；段级算术闭合。
#   锚：Read_Pico.bin 4,071,616 B（相对 gen26.3 的 4,070,704 为 **+912 B**，双操作数
#   4,071,616-4,070,704=+912）；段级闭合：seg0(DROM)+112（0x234388→0x2343f8，
#   AI chip 文案/几何常量）+ seg3(IROM)+796（0x18c78c→0x18caa8，AI chip 绘制 /
#   tap 门禁 / force_ai / ai_over_dict / app_book AI 分支与超时回退）+
#   seg3 对齐垫 +4（0x18c790→0x18cab0）—— 分项净和 + 尾注差 = 总差，逐分闭合；
#   seg1 File offs `0x00234418`（gen26.3 为 0x002343a8，+0x70=112B 顺移——
#   DROM 段增长），paddr = 0x00234418 + 8(段头) + 0x10000(ota_0 基址)
#   = `0x00244420`（预测）；seg1 Load `0x3fc9c700` 世代不变锚不动、len `0x08a8c`
#   与 gen26.3 持平——双锚再守。
#   sha256 `bb9c3963…`；partition-table `9d5d890b…`、ota_data_initial `7d2c7ac4…`
#   与 gen26.3 逐字节一致；bootloader `e7ab5130…`（同 21,232 B，时间戳差异 vs
#   gen26.3 的 `79b992dd…`）。
#   本代干跑 zip 不 push、不打 tag（既定纪律：真机一轮收官后统一推 origin）。
#   真机复测面（待验）：① 词典命中词弹词典卡 + 卡上有可点「AI」；② 点「AI」→
#   「AI 查询中」卡→2-5s 释义原地更新；③ 离线/失败点「AI」→入队或 15s 回退
#   词典释义（不丢词典内容）；④ 模板库四操作（切换/删除/恢复默认/编辑保存）后
#   主编辑框与激活模板名与存储一致（M1.2.1 APK 侧）；⑤ 回归面。
# Gen26.4 re-anchor (2026-10-24, the dictionary card's "AI" entry - the
#   device-side M1.2.1 companion round, parallel to the APK-side M1.2.1
#   template-library three-point state-sync hotfix v1.4.1/code6; the version
#   tag steps 26.3->26.4):
#   (1) dict_popup.c/h: new DICT_POPUP_TAP_AI; dict_popup_ai_rect() (left of
#   the save chip on the same header row, 96x52, x=save.x-12-AI_W);
#   dict_popup_tap judges the AI chip first (hit only when s_ai_on &&
#   !s_pending; a querying card's tap is ignored); draw adds the AI chip (dark
#   fill, inverted text) and narrows the header row; force_ai() (after
#   show_pending sets s_ai_over_dict) / ai_over_dict() (the fallback predicate)
#   / set_ai_enabled() / word() getter.
#   (2) app_book.c: the word-hit path calls
#   dict_popup_set_ai_enabled(push_sink_ai_enabled()); reader_panel_action's
#   DICT_POPUP_TAP_AI calls push_sink_ai_tap_word(this word) (cache hit =
#   instant / connected = send 0x17 / offline = enqueue) plus
#   dict_popup_force_ai() to switch to the querying card; on_tick's 15 s
#   timeout splits - when ai_over_dict() is true, dict_popup_open(this word)
#   falls back to reopening the dictionary card (dictionary content preserved),
#   otherwise the old "AI 查询失败" notice. The protocol is unchanged (0x17
#   still goes out).
#   (3) tools/dict_popup_host_test.c: test_ai_entry() - AI chip show/hide
#   (absent by default / present when enabled), tapping the AI area MISSED
#   (off) / TAP_AI (on), AI and save chips never overlap, force_ai makes
#   is_pending + ai_over_dict true, a querying card draws no AI chip, the miss
#   path's show_pending carries no over_dict, show_def clears over_dict, the
#   word() getter.
#   Gating: 16/16 runners green (the dict_popup runner-15 includes the AI entry
#   assertions; mutation sweep 6/6 all caught - new M6 "AI entry gate ignores
#   s_ai_on"); fullclean (build dir removed, a true full rebuild) exit 0; the
#   8 warnings match the baseline item for item (app_book.c 7 + ui_pinpad.c 1,
#   neither in the change surface); the seg1 double anchor holds; section
#   arithmetic closes.
#   Anchors: Read_Pico.bin 4,071,616 B (**+912 B** vs gen26.3's 4,070,704; both
#   operands 4,071,616-4,070,704=+912); closed segment by segment: seg0(DROM)
#   +112 (0x234388->0x2343f8, the AI chip label/geometry constants) +
#   seg3(IROM) +796 (0x18c78c->0x18caa8, the AI chip draw / tap gate /
#   force_ai / ai_over_dict / app_book AI branch and timeout fallback) +
#   seg3 alignment pad +4 (0x18c790->0x18cab0) - **sum of parts + tail-note
#   delta = total, closed to the byte**; seg1 File offs `0x00234418` (gen26.3
#   was 0x002343a8, +0x70=112B shifted - the DROM segment grew), paddr =
#   0x00234418 + 8 (segment header) + 0x10000 (ota_0 base) = `0x00244420`
#   (predicted); seg1 Load `0x3fc9c700` generation-invariant anchor unmoved,
#   len `0x08a8c` flat vs gen26.3 - the double anchor holds again.
#   sha256 `bb9c3963…`; partition-table `9d5d890b…` and ota_data_initial
#   `7d2c7ac4…` byte-identical to gen26.3; bootloader `e7ab5130…` (same 21,232
#   B, timestamp difference vs gen26.3's `79b992dd…`).
#   This generation's dry-run zip is not pushed and no tag is cut (standing
#   discipline: push to origin in one wave after the hardware round closes).
#   Live re-test surface (pending): (1) a dictionary hit pops the dictionary
#   card with a tappable "AI"; (2) tapping "AI" gives the "AI 查询中" card, then
#   the definition in place within 2-5 s; (3) an offline/failed "AI" tap
#   enqueues or falls back to the dictionary definition after 15 s (dictionary
#   content preserved); (4) after the four template-library operations
#   (switch / delete / restore defaults / edit-save) the main editor and the
#   active template name match storage (APK-side M1.2.1); (5) the regression
#   surface.
# 重锚记录（2026-10-24 深夜，gen26.4.1：gen26.4 真机 defect 热修——词典命中词点
#   「AI」释义全丢。日志铁证：`ai force ""`（词空）+ 无 `ai pending`/`ai force over
#   dict` + 0x17 的 notify 在 ai force 日志前 1ms（103109→103111）。
#   根因（自引用别名）：app_book TAP_AI 分支 `w = dict_popup_word()` 拿到的是
#   &s_word 自身；push_sink_ai_tap_word(w) 在 s_word 尚存时正常发出 0x17（APK
#   确实收到词并回了 0x07）；dict_popup_force_ai(w) → show_pending(w) 入口先
#   dict_popup_reset()（清 s_word = 清掉别名参数指向的内容）再 `if (!*word)
#   return false` → 误判空词提前返回——查询态没建、词典卡被清空；0x07 回来时
#   is_pending 已不成立，门 A 丢弃进预取缓存 = 用户看到「词典卡内容消失、AI
#   释义全丢」。miss 路径正常是因为传的是局部缓冲；15s 超时回退也不触发
#   （s_pending=false，pending_expired 恒假）。宿主测试没抓到：force_ai 断言传的
#   是字面量（.rodata 不别名）。
#   修法（三层）：① show_pending 入口先拷贝 word 到局部 own[] 再 reset（API 契约
#   根治：传入词可以是任何东西，包括卡自己的词头）；② dict_popup_open 同型隐患
#   对称修复；③ app_book TAP_AI 分支取词立即拷贝到局部 wbuf（调用侧根治，词的
#   生命周期与卡状态解耦）。宿主回归补别名路径断言（force_ai(dict_popup_word())
#   / open(dict_popup_word())），变异新增 M7「别名防御被拆」被擒（7/7）。
#   APK 同轮 v1.4.2（code 7，a6877152…）：审计尾巴①——导入 JSON 文件内重名
#   去重（与保存路径 upsert 语义一致，同名保留后出现的条目；按 name 删除依赖
#   「模板名唯一」）。
#   门禁：16/16 runner 全绿（dict_popup 含别名回归断言，变异 7/7 全擒）；
#     fullclean（删 build 真全量）exit 0；8 警告逐条同基线（app_book.c 7 +
#     ui_pinpad.c 1）；APK 45/45 + 52/52 + 七步 BUILD_OK；家族五件对表 5/5。
#   锚：Read_Pico.bin 4,071,680 B（相对 gen26.4 的 4,071,616 为 **+64 B**，双操作数
#   4,071,680-4,071,616=+64）；段级闭合：seg3(IROM)+64（0x18caa8→0x18cae8，
#   own 拷贝 + wbuf），seg0/seg1/其余持平，尾注差 0；seg1 File offs `0x00234418`
#   与 gen26.4 持平，paddr 预测 `0x00244420`（与 gen26.4 同值——用户实测日志
#   `segment 1: paddr=00244420` 与预测逐字吻合，公式再添一代实证）；seg1 Load
#   `0x3fc9c700` 世代不变 + len `0x08a8c` 持平——双锚再守。
#   sha256 `a13d0b5e…`；partition-table `9d5d890b…`、ota_data_initial `7d2c7ac4…`
#   与 gen26.4 逐字节一致；bootloader `c8069ead…`（仅时间戳）。
#   本代干跑 zip 不 push、不打 tag（既定纪律）。真机复测面（待验）：① 词典命中词
#   点「AI」→「AI 查询中」→ 2-5s 释义原地更新（gen26.4 的丢释义场景必须转绿）；
#   ② 15s 超时回退词典释义（断网/杀 APK）；③ 命中缓存秒出；④ APK v1.4.2 覆盖
#   装后模板库四操作回归（应与 v1.4.1 全绿表现一致）。
# Gen26.4.1 re-anchor (2026-10-24 late night: gen26.4 device-defect hotfix - the
#   AI definition was lost for every dictionary-hit word tapped via "AI". Log
#   evidence: `ai force ""` (empty word), no `ai pending`/`ai force over dict`,
#   and the 0x17 notify 1 ms before the ai force log (103109->103111).
#   Root cause (self-referential alias): app_book's TAP_AI branch took
#   `w = dict_popup_word()` - the pointer IS &s_word; push_sink_ai_tap_word(w)
#   sent the 0x17 fine (the phone answered - see the notify 1 ms earlier);
#   then dict_popup_force_ai(w) -> show_pending(w) first ran
#   dict_popup_reset() (clearing s_word = the aliased argument's content) and
#   only then checked `if (!*word) return false` - a misread empty word bailed
#   out, the query state never formed, the dict card was wiped; when 0x07
#   landed, is_pending no longer held and gate A dropped it into the prefetch
#   cache = the user saw the dict card lose its content and every AI
#   definition vanish. The miss path worked because it passes a local buffer;
#   the 15 s fallback never fired either (s_pending=false, pending_expired
#   always false). The host test missed it because the force_ai assertion
#   passed a literal (.rodata, never aliased).
#   Fix (three layers): (1) show_pending copies word into a local own[] BEFORE
#   the reset (the API contract now holds for any argument, including the
#   card's own head); (2) dict_popup_open's same-shape hazard fixed
#   symmetrically; (3) app_book's TAP_AI branch copies the word into a local
#   wbuf at once (call-site fix, the word's lifetime decoupled from card
#   state). Host regression adds the aliased-path assertions
#   (force_ai(dict_popup_word()) / open(dict_popup_word())); the mutation
#   sweep gains M7 "aliasing defense torn out", caught (7/7).
#   APK same round v1.4.2 (code 7, a6877152…): audit tail (1) - dedupe
#   duplicate names inside an imported JSON file (same upsert stance as the
#   save path, the later entry wins; name-based delete assumes unique names).
#   Gating: 16/16 runners green (dict_popup includes the aliased-path
#   assertions; mutation 7/7 all caught); fullclean (build dir removed, a true
#   full rebuild) exit 0; the 8 warnings match the baseline item for item
#   (app_book.c 7 + ui_pinpad.c 1); APK 45/45 + 52/52 + seven-step BUILD_OK;
#   deliverable-family check 5/5.
#   Anchors: Read_Pico.bin 4,071,680 B (**+64 B** vs gen26.4's 4,071,616; both
#   operands 4,071,680-4,071,616=+64); closed segment by segment: seg3 (IROM)
#   +64 (0x18caa8->0x18cae8, the own copies plus wbuf), seg0/seg1/the rest
#   flat, tail-note delta 0; seg1 File offs `0x00234418` flat vs gen26.4,
#   paddr predicted `0x00244420` (same as gen26.4 - the user's live log
#   `segment 1: paddr=00244420` matched the prediction character for
#   character, one more empirical generation); seg1 Load `0x3fc9c700`
#   generation-invariant + len `0x08a8c` flat - the double anchor holds again.
#   sha256 `a13d0b5e…`; partition-table `9d5d890b…` and ota_data_initial
#   `7d2c7ac4…` byte-identical to gen26.4; bootloader `c8069ead…` (timestamp
#   only).
#   This generation's dry-run zip is not pushed and no tag is cut (standing
#   discipline). Live re-test surface (pending): (1) a dict-hit word tapped
#   via "AI" gives the "AI 查询中" card then the definition in place within
#   2-5 s (the gen26.4 losing scenario must turn green); (2) a 15 s timeout
#   falls back to the dictionary definition (offline / killed APK); (3) the
#   prefetch cache answers instantly on a second tap; (4) after installing
#   v1.4.2 over the old APK the four template-library operations regress green
#   (same as v1.4.1's all-green showing).
# 烧前自检：Read_Pico.bin 的 sha256 必须与锚定值逐字一致，杜绝烧错 bin。
# Pre-flash self-check: Read_Pico.bin's sha256 must match the anchor exactly.
EXPECTED="3c7892f55b8ebb3d4886d37483bc5d175d6f823627da3c89a07b4ea9d14cb0ed"
if command -v sha256sum >/dev/null 2>&1; then
    ACTUAL=$(sha256sum "$DIR/Read_Pico.bin" | cut -d' ' -f1)
elif command -v shasum >/dev/null 2>&1; then
    ACTUAL=$(shasum -a 256 "$DIR/Read_Pico.bin" | cut -d' ' -f1)
else
    ACTUAL=""
    echo "WARN: no sha256 tool found; skipping pre-flash check" >&2
fi
if [ -n "$ACTUAL" ] && [ "$ACTUAL" != "$EXPECTED" ]; then
    echo "FATAL: Read_Pico.bin sha256 mismatch - refusing to flash." >&2
    echo "  expected: $EXPECTED" >&2
    echo "  actual:   $ACTUAL" >&2
    echo "  Check package integrity; do NOT flash a bin off its anchor." >&2
    exit 1
fi
[ -n "$ACTUAL" ] && echo "sha256 OK: $ACTUAL (production build, anchor verified)"

python -m esptool --chip esp32s3 -b "$BAUD" \
  --before default-reset --after hard-reset write-flash \
  --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0x0      "$DIR/bootloader.bin" \
  0x8000   "$DIR/partition-table.bin" \
  0x10000  "$DIR/Read_Pico.bin" \
  0xb10000 "$DIR/ota_data_initial.bin"

echo "Flash done (production build)."
