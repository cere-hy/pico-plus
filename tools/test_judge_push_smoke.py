#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
"""判读器自身的牙：五向先红。

判读器的风险不是"判错绿"，而是"永远判绿"——一个只输出 PASS 的脚本和没有脚本
在台账上长得一样（基线 §13.12）。所以每个判据都要能指名道姓地红一次，
另外"没有观测点"必须落在 UNAVAILABLE 而不是 PASS（§13.17）。

用法：python3 tools/test_judge_push_smoke.py
"""

import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
JUDGE = ROOT / "tools" / "judge_push_smoke.py"

BASE = """\
I (1000) boot: rst:0x1 (POWERON)
I (2000) read_pico: sd mount ok
I (3000) book: turn chapter=3 page=10/120 pct=8
I (4000) push_sink: verify ok: sz=100 crc=0x1
I (4100) transfer_page: commit beat 1/2: hold starts, 1500 ms deadline
I (4110) transfer_page: commit beat 2/2: reader hold cleared after 1 frame(s), running commit
I (4500) push_sink: verify ok: sz=120 crc=0x2
I (4600) transfer_page: commit beat 1/2: hold starts, 1500 ms deadline
I (4610) transfer_page: commit beat 2/2: reader hold cleared after 1 frame(s), running commit
I (9000) book: turn chapter=3 page=11/120 pct=9
"""

CASES = [
    ("绿：正常两组 1→2", BASE, "PASS"),
    ("红1：删掉一个 1/2（幽灵 2/2）",
     BASE.replace("I (4600) transfer_page: commit beat 1/2: hold starts, 1500 ms deadline\n", ""),
     "FAIL"),
    ("红2：连出两个 1/2（缺 2/2）",
     BASE.replace("I (4610) transfer_page: commit beat 2/2: reader hold cleared after 1 frame(s), running commit",
                  "I (4610) transfer_page: commit beat 1/2: hold starts, 1500 ms deadline"),
     "FAIL"),
    ("红3：dropped 一条",
     BASE.replace("I (9000) book:", "W (7000) transfer_page: dropped after 3 frames\nI (9000) book:"),
     "FAIL"),
    ("红4：提交后跳章",
     BASE.replace("I (9000) book: turn chapter=3 page=11/120 pct=9",
                  "I (5000) book: turn chapter=0 page=1/18 pct=5"),
     "FAIL"),
    ("不可判：没有拍号行",
     """\
I (1000) boot: rst:0x1 (POWERON)
I (3000) book: turn chapter=3 page=10/120 pct=8
""",
     "UNAVAILABLE"),
    ("不可判：空文件", "", "UNAVAILABLE"),
]


def main():
    failures = 0
    with tempfile.TemporaryDirectory() as folder:
        for name, body, want in CASES:
            p = Path(folder) / "case.log"
            p.write_text(body, encoding="utf-8")
            r = subprocess.run([sys.executable, str(JUDGE), str(p)],
                               capture_output=True, text=True)
            got = "UNAVAILABLE"
            for line in r.stdout.splitlines():
                if line.startswith("总判定："):
                    got = line.split("：", 1)[1].strip()
            ok = got == want
            if not ok:
                failures += 1
            print("%-32s 期望 %-12s 实得 %-12s %s"
                  % (name, want, got, "✓" if ok else "✗"))
            if not ok:
                print("    " + r.stdout.replace("\n", "\n    ")[:400])

    print()
    print("七向 %d 通过 / %d 失败" % (len(CASES) - failures, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
