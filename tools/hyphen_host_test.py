#!/usr/bin/env python3
"""门 2 的行为面：断点算得对不对（表是抄的，算法是写的）。

三层证据：
  1. **金字招牌**：`hy-phen-ation` / `ap-pre-ci-ate` / `oblig-a-tory` 等 TeX 文档里
     出现过的断法，逐条写死在这里；`present` / `project` 是例外表里**禁止断词**的词，
     必须一个断点都不给（这一条咬「例外缺席判据」——把 mask==0 当缺席会立刻红）。
  2. **独立实现差分**：把同一份 TeX 模式表转成 libhyphen 的 dic，交给 pyphen 的
     Liang 实现跑一遍，与我们的 C 实现逐词比对。表一样、算法一样，断点就必须一样；
     不一致要么是我们算错，要么是表被换。pyphen 只是开发依赖，缺席时打 SKIP 不判红。
  3. 表本身可重生成由 `tools/check_hyphen_table.py` 管，这里不重复。

跑法：python3 tools/hyphen_host_test.py
"""
from __future__ import annotations

import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "tools/vendor/hyph-en-us.tex"

# word: 期望断点（词内字节位置，升序）。来自 TeX 文档与 hyphen.tex 的例外表。
GOLDEN = {
    "hyphenation": [2, 6],
    "appreciate": [2, 5, 7],
    "impression": [2, 6],
    "towards": [2],
    "particular": [3, 6, 7],
    "revolution": [3, 4, 6],
    "threatened": [6],
    "extraordinary": [2, 7, 9],
    "associate": [2, 4],       # 例外 as-so-ci-ate
    "declaration": [3, 5, 7],  # 例外 dec-li-na-tion
    "obligatory": [5, 6],      # 例外 oblig-a-tory（只有这两处）
    "philanthropic": [4, 6],   # 例外 phil-an-thropic
    "recognizance": [2, 5, 7],
    "reformation": [3, 5, 7],
    "retribution": [3, 5, 7],
    "table": [2],              # 例外 ta-ble
    "present": [],             # 例外：禁止 pre-sent
    "presents": [],
    "project": [],
    "projects": [],
    "cat": [],                 # 太短 / 无断点
}

MAIN_C = r'''
#include <stdio.h>
#include <string.h>
#include "hyphen.h"

int main(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        hyph_points_t p;
        memset(&p, 0, sizeof p);
        (void)hyph_points(argv[i], strlen(argv[i]), &p);
        printf("%s", argv[i]);
        for (int k = 0; k < p.count; ++k) printf(" %u", (unsigned)p.pos[k]);
        printf("\n");
    }
    return 0;
}
'''


def build(tmp: Path) -> Path:
    (tmp / "main.c").write_text(MAIN_C, encoding="utf-8")
    exe = tmp / "hyphen_test"
    subprocess.run([
        "gcc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-o", str(exe),
        str(tmp / "main.c"),
        str(ROOT / "main/book/hyphen.c"),
        str(ROOT / "main/book/hyph_en_us_data.c"),
        "-I", str(ROOT / "main/book"),
    ], check=True)
    return exe


def run(exe: Path, words: list[str]) -> dict[str, list[int]]:
    r = subprocess.run([str(exe)] + words, capture_output=True, text=True, check=True)
    out = {}
    for line in r.stdout.splitlines():
        parts = line.split()
        out[parts[0]] = [int(x) for x in parts[1:]]
    return out


def write_dic(path: Path) -> None:
    text = SRC.read_text(encoding="utf-8")
    lines, cut_line = [], []
    for line in text.splitlines():
        i, cut = 0, len(line)
        while i < len(line):
            if line[i] == "\\":
                i += 2
                continue
            if line[i] == "%":
                cut = i
                break
            i += 1
        cut_line.append(line[:cut])
    body = "\n".join(cut_line)
    pats = re.search(r"\\patterns\{(.*?)\}", body, re.S).group(1).split()
    path.write_text("UTF-8\nLEFTHYPHENMIN 2\nRIGHTHYPHENMIN 3\n" + "\n".join(pats) + "\n",
                    encoding="utf-8")


def corpus_words() -> list[str]:
    text = (ROOT / "tools/measure_wrap_model.py").read_text(encoding="utf-8")
    m = re.search(r"CORPUS = \((.*?)\)\n", text, re.S)
    corpus = "".join(re.findall(r'"([^"]*)"', m.group(1))) if m else ""
    corpus += " " + (ROOT / "README.md").read_text(encoding="utf-8")[:6000]
    return sorted({w.lower() for w in re.findall(r"[A-Za-z]{5,}", corpus)})


def main() -> int:
    fails = 0
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        exe = build(tmp)
        got = run(exe, list(GOLDEN))
        for word, want in GOLDEN.items():
            if got.get(word, []) != want:
                print(f"FAIL  {word}: want={want} got={got.get(word, [])}")
                fails += 1
        if fails:
            print(f"hyphen: FAIL - {fails} golden word(s)")
            return 1
        print(f"hyphen: golden {len(GOLDEN)}/{len(GOLDEN)} ok (exceptions included)")

        # 独立实现差分（可选依赖）
        try:
            import pyphen  # type: ignore
        except ImportError:
            print("hyphen: SKIP differential - pyphen not installed (dev-only dependency)")
            return 0
        dic = tmp / "tex.dic"
        write_dic(dic)
        ref = pyphen.Pyphen(filename=str(dic), left=2, right=3)
        # 例外词不参与差分：libhyphen 的 dic 只有模式表，\hyphenation 那 14 条例外
        # 不在里面。例外本身由上面的金字招牌逐条咬（present/project/table/…）。
        # Exception words are excluded: the dic carries patterns only, not \hyphenation.
        exceptions = set()
        body = re.sub(r"(?m)%.*$", "", SRC.read_text(encoding="utf-8"))
        m = re.search(r"\\hyphenation\{(.*?)\}", body, re.S)
        if m:
            for w in m.group(1).split():
                exceptions.add(w.replace("-", "").lower())
        words = [w for w in corpus_words() if w not in exceptions]
        got = run(exe, words)
        diff = 0
        for w in words:
            ins = ref.inserted(w)
            theirs, pos = [], 0
            for part in ins.split("-"):
                pos += len(part)
                if pos < len(w):
                    theirs.append(pos)
            if got.get(w, []) != theirs:
                diff += 1
                if diff <= 10:
                    print(f"DIFF  {w}: ours={got.get(w, [])} reference={theirs} ({ins})")
        if diff:
            print(f"hyphen: FAIL - {diff}/{len(words)} words differ from the reference "
                  f"implementation over the *same* table")
            return 1
        print(f"hyphen: differential {len(words)}/{len(words)} words identical to the "
              f"reference Liang implementation (same table)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
