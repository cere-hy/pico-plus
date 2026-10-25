@echo off
rem 生产版烧写脚本（Windows）/ Flash script for the PRODUCTION build (Windows)
rem
rem 用法 / Usage:
rem   flash.bat COM3
rem   flash.bat COM3 230400        （烧录报错/校验不过时降波特率重试）
rem
rem 警告 / WARNING: Read_Pico.bin 必须落在 0x10000。写到 0x0 会覆盖 bootloader /
rem                 分区表 / NVS / phy_init。
rem                 Read_Pico.bin MUST go to 0x10000; 0x0 would overwrite the
rem                 bootloader, partition table, NVS and phy_init.
rem
rem 四个偏移对应 partitions_16M.csv 的 5.5MB app 布局：ota_0 在 0x10000、
rem ota_1 在 0x590000，故 otadata 落在 0xb10000。不是上游默认的 4MB 布局。
rem These offsets match our partitions_16M.csv 5.5MB app layout; otadata is at
rem 0xb10000 - not upstream's 4MB default.
rem
rem 烧前请把完整输出存盘（esptool 的哈希/摘要行是烧录证据）。
rem Save the FULL output (esptool hash lines are the flashing evidence).

setlocal enabledelayedexpansion
rem 锚：gen26.5 M2 Step10 TTS 听书 Read_Pico.bin 4,085,040 B /
rem   3c7892f5…（2026-10-25 重锚，与 firmware/flash.sh 同值；拒烧语义同 flash.sh：exit 1）。
rem Anchor: the gen26.4.1 device-defect hotfix (the AI entry's self-referential
rem alias) Read_Pico.bin 4,071,680 B / a13d0b5e... (re-anchored 2026-10-24, same
rem value as firmware/flash.sh; refusal semantics match flash.sh: exit 1).
set EXPECTED=a7bed55b3182e167181c255b14f3018b726a2d328dc81526c8a7b1468359c2fd
set DIR=%~dp0
set PORT=%1
if "%PORT%"=="" set PORT=COM3
set BAUD=%2
if "%BAUD%"=="" set BAUD=460800

rem certutil 输出第二行才是哈希；取到即跳出 / certutil prints the hash on line 2.
set ACTUAL=
for /f "skip=1 tokens=1" %%a in ('certutil -hashfile "%DIR%Read_Pico.bin" SHA256') do (
  if not defined ACTUAL (
    set ACTUAL=%%a
  )
)

if not defined ACTUAL (
  echo WARN: certutil unavailable; skipping pre-flash check
) else (
  if /i not "%ACTUAL%"=="%EXPECTED%" (
    echo FATAL: Read_Pico.bin sha256 mismatch - refusing to flash.
    echo   expected: %EXPECTED%
    echo   actual:   %ACTUAL%
    echo   Check package integrity; do NOT flash a bin off its anchor.
    exit /b 1
  )
  echo sha256 OK: %ACTUAL% ^(production build, anchor verified^)
)

python -m esptool --chip esp32s3 -b %BAUD% --before default-reset --after hard-reset write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m 0x0 "%DIR%bootloader.bin" 0x8000 "%DIR%partition-table.bin" 0x10000 "%DIR%Read_Pico.bin" 0xb10000 "%DIR%ota_data_initial.bin"

echo Flash done (production build).
endlocal
