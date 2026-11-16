#!/usr/bin/env python3
"""B64 / B62 / B33 负向自检：逐个靶点改坏源码，牙必须红；全红才算这份牙活着。

每个靶点在**临时工作副本**上做（备份 → 改 → 跑 → 还原），失败也会还原。
A tooth that has never been seen to go red is decoration - every target here is a real
mutation that would ship a real defect.

用法：python3 tools/selftest_b64.py
"""
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
BOOK = ROOT / "main/apps/app_book.c"
LOOP = ROOT / "main/app/app_loop.c"
PROTO = ROOT / "docs/PROTOCOL.md"

# (名字, 文件, 原文, 替换成)  —— 原文必须**恰好出现一次**，否则自检本身失败（防靶点漂移）
TARGETS = [
    ("跨上限拒发门被删（改成 if (0)）", BOOK,
     "if (len > PUSH_SCENE_CTX_MAX) {", "if (0) {"),
    ("拒发门挪到发送之后", BOOK,
     "    if (len > PUSH_SCENE_CTX_MAX) {",
     "    if (0) push_sink_ai_scene_send(0, 0, 0, 0, 0);\n    if (len > PUSH_SCENE_CTX_MAX) {"),
    ("拒发不说话了（文案被删）", BOOK,
     '        reader_notice_set("选段过长 · 请缩小范围", 2000);\n'
     '        excerpt_hint_set("选段过长 · 请缩小范围", EXCERPT_HINT_ALERT);',
     '        reader_notice_set("", 2000);'),
    ("回执只写 hint 槽、不上气泡", BOOK,
     "reader_notice_set(receipt, 2000);", "(void)receipt;"),
    ("气泡 show 日志被删（B63）", BOOK,
     'ESP_LOGI(TAG, "notice: show \\"%s\\" %u ms", text, (unsigned)ms);',
     "(void)text;"),
    ("气泡 hide 日志被删（B63）", BOOK,
     'ESP_LOGI(TAG, "notice: hide \\"%s\\"", s_reader_notice); // B63：show/hide 两头留痕',
     "/* hide log removed */"),
    ("启动横幅丢了 ELF 指纹（B62）", LOOP,
     "const uint8_t *elf = esp_app_get_description()->app_elf_sha256;",
     "const uint8_t *elf = (const uint8_t *)\"000000000000\";"),
    ("bit2 凭证被撤（B33）", PROTO,
     "| 2026-11-02（三点一致，源码级） | v1.6.3 |", "| 待联调 | 待定 |"),
]


def run_gate():
    return subprocess.run([sys.executable, "tools/check_excerpt.py"], cwd=ROOT,
                          capture_output=True, text=True)


def main():
    saved = {p: p.read_text() for p in (BOOK, LOOP, PROTO)}
    survived = 0
    try:
        for name, path, old, new in TARGETS:
            src = saved[path]
            count = src.count(old)
            if count != 1:
                print("SELFTEST-BROKEN: %s (anchor matched %d times)" % (name, count))
                return 2
            path.write_text(src.replace(old, new))
            r = run_gate()
            path.write_text(src)
            if r.returncode == 0:
                print("SURVIVED: %s  <- 牙没咬住" % name)
                survived += 1
            else:
                first = next((ln for ln in r.stdout.splitlines() if ln.startswith("FAIL")), "")
                print("killed  : %s  | %s" % (name, first[:110]))
    finally:
        for p, text in saved.items():
            p.write_text(text)
    total = len(TARGETS)
    print("\nSELF-TEST %s: %d/%d targets killed" %
          ("PASS" if survived == 0 else "FAIL", total - survived, total))
    return 0 if survived == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
