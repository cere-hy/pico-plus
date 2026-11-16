"""中文：联网 OTA 默认启用自家 Pages 源；kill switch 保留，且不得再冒出原厂回退源。
English: online OTA defaults to the self-hosted Pages feed; the kill switch stays, and no
stock fallback source may reappear.
SPDX-License-Identifier: Apache-2.0
R5：宏默认改 0 后，本守卫从「停用状态守卫」转型「自家源守卫」——编码当前真不变量
（宏==0 && feed/白名单==自家前缀），并继续守住 kill switch 断开路径。
"""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
header = (root / "main/ota_online.h").read_text()
source = (root / "main/ota_online.c").read_text()
release = (root / "main/ota_release.c").read_text()
failures = []

# 1. 默认值必须是 0（启用自家源）；仍用 #ifndef 包住，runner 才能 -D=1 单编验证断开路径。
if not re.search(r"#ifndef PICO_ONLINE_UPDATES_DISABLED\s*\n"
                 r"#define PICO_ONLINE_UPDATES_DISABLED 0\s*\n#endif", header):
    failures.append("ota_online.h: PICO_ONLINE_UPDATES_DISABLED must default to 0 behind an #ifndef guard")
# 2. feed 常量指向自家 update.json，且旧的 fallback 常量不得再出现。
if '#define PICO_OTA_FEED_URL "https://cere-hy.github.io/pico-plus/update.json"' not in header:
    failures.append("ota_online.h: PICO_OTA_FEED_URL must be the self-hosted update.json")
if "PICO_OTA_FEED_FALLBACK_URL" in header or "PICO_OTA_FEED_FALLBACK_URL" in source:
    failures.append("ota_online.h/.c: the stock fallback feed constant must stay removed")
# 3. check_feed() 的 feeds[] 必须是单元素自家源，不允许第二个家。
if not re.search(r"static const char \*const feeds\[\]\s*=\s*\{PICO_OTA_FEED_URL\};", source):
    failures.append("ota_online.c: feeds[] must be a single self-hosted source (no fallback)")
# 4. bin URL 白名单收缩为自家单前缀；原厂两域名不得残留。
prefix = '"https://cere-hy.github.io/pico-plus/firmware/"'
if prefix not in release:
    failures.append("ota_release.c: the single self-hosted firmware prefix is missing")
if "kiikoread.com" in release or "wegooo-cell.github.io" in release:
    failures.append("ota_release.c: a stock origin leaked back into the URL whitelist")
m = re.search(r"static const char \*const prefixes\[\]\s*=\s*\{(.*?)\};", release, re.S)
if not m or len(re.findall(r'"[^"]+"', m.group(1))) != 1:
    failures.append("ota_release.c: prefixes[] must contain exactly one prefix")
# 5. kill switch 断开路径仍在：两个入口在禁用时直接拒绝，而不是去 start() 建任务。
def entry_body(name):
    match = re.search(r"^esp_err_t " + name + r"\(void\)\s*\{", source, re.M)
    if not match:
        return None
    at, depth = match.end(), 1
    while depth and at < len(source):
        if source[at] == "{":
            depth += 1
        elif source[at] == "}":
            depth -= 1
        at += 1
    return source[match.end():at] if not depth else None


for name in ("pico_online_check", "pico_online_download"):
    body = entry_body(name)
    if body is None:
        failures.append("ota_online.c: %s() disappeared or lost its braces" % name)
        continue
    if "PICO_ONLINE_UPDATES_DISABLED" not in body or "ESP_ERR_NOT_SUPPORTED" not in body:
        failures.append("ota_online.c: %s() must refuse with ESP_ERR_NOT_SUPPORTED while disabled" % name)
# 6. 除升级页外不得再冒出新的联网任务发起者。
for path in sorted((root / "main").rglob("*.c")):
    text = path.read_text()
    if "pico_online_check(" not in text and "pico_online_download(" not in text:
        continue
    if path.name in ("ota_online.c", "app_device_settings.c"):
        continue
    failures.append("%s: a new caller of pico_online_check/download appeared" % path.relative_to(root))

if failures:
    raise SystemExit("\n".join("FAIL: " + item for item in failures))
print("PASS: online OTA defaults enabled on the self-hosted single feed; whitelist is our prefix only; fallback stays removed; kill switch still cuts both entries; no new callers")
