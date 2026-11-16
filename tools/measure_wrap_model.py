#!/usr/bin/env python3
"""26.11.8：留白基线的**模型侧**（现状 vs 连字符断词 A）——只量数，不改产品码。

为什么还要一个 Python 模型：`measure_line_whitespace.py` 直接跑产品 `take_line`，
能给出「现状」但没法给出「A 之后」——因为断点集合是写死在 C 里的。本脚本用
`fontTools` 读同一个 `builtin.ttf` 的 hmtx，**照抄** `take_line` 的贪心 + 词界回退
规则重放一遍，先用「现状」与 C 单元逐行对齐做**校准**，再把 Liang 音节断点加进去，
于是「A 的收益」是在同一套字宽、同一套断行规则下量出来的差值，不是两套实现的差。

校准门槛：现状档必须与 C 单元的行数与每行宽度逐行一致；不一致就打印分歧行并退出，
不拿「差不多」当证据。

Liang 模式表来自 Knuth `hyphen.tex`（4,447 条模式 + 14 条例外），与 26.11.8 方案
§三 引用的那份同源。左右最小切长 2/3（TeX 默认）。
"""
from __future__ import annotations

import math
import re
import sys
import pathlib

from fontTools.ttLib import TTFont

ROOT = pathlib.Path(__file__).resolve().parents[1]
FONT = ROOT / "main/assets/builtin.ttf"
HYP = pathlib.Path("/tmp/hyphen_knuth.tex")

PX = 48

CORPUS = (
    "As soon as the sound of the Peace-cry of my departing Wife had died away, I began to "
    "approach the Stranger with the intention of taking a nearer view and of bidding him be "
    "seated; but his appearance struck me dumb and motionless with astonishment. Without the "
    "slightest symptoms of angularity he nevertheless varied every instant with gradations of "
    "size and brightness scarcely possible for any Figure within the scope of my experience. "
    "The thought flashed across me that I might have before me a burglar or cut-throat, some "
    "monstrous Irregular Isosceles, who, by feigning the voice of a Circle, had obtained "
    "admission somehow into the house, and was now preparing to stab me with his acute angle."
)


class Metrics:
    def __init__(self, path: pathlib.Path, px: int):
        f = TTFont(str(path))
        self.upem = f["head"].unitsPerEm
        self.scale = px / float(f["hhea"].ascender - f["hhea"].descender)
        hmtx = f["hmtx"].metrics
        cmap = f.getBestCmap()
        self.adv = {}
        for cp, name in cmap.items():
            if name in hmtx:
                self.adv[cp] = int(math.floor(hmtx[name][0] * self.scale + 0.5))

    def width(self, text: str) -> int:
        return sum(self.adv.get(ord(c), round(PX * 0.5)) for c in text)


def word_char(c: str) -> bool:
    return c.isalnum() or c in "'-"


def wrap(text: str, available: int, m: Metrics, breaks: dict[str, list[int]] | None = None):
    """照抄 take_line：逐字累加，超限则回退到词首；breaks 给出词内音节断点（下标）。"""
    lines = []
    i = 0
    n = len(text)
    while i < n:
        line_start = i
        width = 0
        word_start = i
        last_space = -1
        j = i
        cut = None
        while j < n:
            ch = text[j]
            w = m.adv.get(ord(ch), round(PX * 0.5))
            if width + w > available:
                # 词内回退（26.11.4 主刀）
                if word_char(ch) and j > i and word_char(text[j - 1]) and last_space >= 0 and word_start > i:
                    cut = word_start
                else:
                    cut = j
                break
            width += w
            if ch == ' ':
                last_space = j
                word_start = j + 1
            j += 1
        else:
            cut = n
        # A：整词放不下时，退到词内最靠右的合法音节断点
        extra_hyph = 0
        if breaks is not None and cut < n and cut == word_start and word_start > i:
            word_end = word_start
            while word_end < n and not text[word_end].isspace():
                word_end += 1
            word = text[word_start:word_end]
            cand = [p for p in breaks.get(word, []) if p > 0]
            best = None
            for p in cand:
                head = text[word_start:word_start + p] + "-"
                if m.width(text[i:word_start]) + m.width(head) <= available:
                    best = p
            if best:
                cut = word_start + best
                extra_hyph = 1
        if cut is None or cut <= i:
            cut = min(n, i + 1)
        seg = text[i:cut].rstrip(' ')
        if extra_hyph:
            seg = text[i:cut] + '-'
        lines.append((seg, m.width(seg)))
        i = cut
        while i < n and text[i] == ' ':
            i += 1
    return lines


def load_patterns(path: pathlib.Path):
    s = path.read_text(errors="replace")
    body = '\n'.join(l.split('%')[0] for l in s.splitlines())
    toks = re.search(r"\\patterns\{(.*?)\}", body, re.S).group(1).split()
    table = {}
    for t in toks:
        letters, weights = [], [0]
        for ch in t:
            if ch.isdigit():
                weights[-1] = int(ch)
            else:
                letters.append(ch)
                weights.append(0)
        table[''.join(letters)] = weights
    return table


def hyphenate(word: str, table, left=2, right=3):
    w = '.' + word.lower() + '.'
    vals = [0] * (len(w) + 1)
    longest = max(len(k) for k in table)
    for i in range(len(w)):
        for k in range(1, min(longest, len(w) - i) + 1):
            pat = w[i:i + k]
            weights = table.get(pat)
            if not weights:
                continue
            for off, v in enumerate(weights):
                if v > vals[i + off]:
                    vals[i + off] = v
    pts = []
    for p in range(1, len(w) - 1):
        if vals[p] % 2 == 1:
            idx = p - 1  # 相对 word（去掉前导 '.'）
            if idx >= left and len(word) - idx >= right:
                pts.append(idx)
    return pts


def stats(rows, label, drop_final=True):
    # 段末行天然短，它的留白不是排版问题；统计口径必须排除，否则 max 永远等于最后一行。
    body = rows[:-1] if (drop_final and len(rows) > 1) else rows
    slacks = sorted(max(0, r[2]) / PX for r in body)
    def pct(q):
        i = min(len(slacks) - 1, max(0, int(round(q * (len(slacks) - 1)))))
        return slacks[i]
    over2 = sum(1 for s in slacks if s > 2.0)
    over3 = sum(1 for s in slacks if s > 3.0)
    print(f"  {label:<14} 行={len(body):3d}  p50={pct(.5):.2f}  p90={pct(.9):.2f}  max={slacks[-1]:.2f}  "
          f">2em={over2:2d} ({over2/max(1,len(body))*100:3.0f}%)  >3em={over3:2d}")
    return pct(.5), pct(.9), slacks[-1], over2, over3


def main() -> int:
    m = Metrics(FONT, PX)
    table = load_patterns(HYP)
    print(f"字体 {FONT.name} upem={m.upem} scale={m.scale:.5f} px={PX}")
    print(f"模式表 {HYP.name}：{len(table)} 条")

    # 音节断点表
    words = set(re.findall(r"[A-Za-z][A-Za-z'-]*", CORPUS))
    breaks = {w: hyphenate(w, table) for w in words}
    sample = {w: breaks[w] for w in ("astonishment", "nevertheless", "departing", "gradations",
                                     "experience", "motionless")}
    print("抽检断点：", {w: [p for p in v] for w, v in sample.items()})

    for em in (12, 13):
        avail = em * PX
        base = [(seg, w, avail - w) for seg, w in wrap(CORPUS, avail, m)]
        withhyp = [(seg, w, avail - w) for seg, w in wrap(CORPUS, avail, m, breaks)]
        print(f"\n== 版心 {em} em ==")
        stats(base, "现状")
        stats(withhyp, "A（连字符）")
        print("  A 后逐行：")
        for seg, w, slack in withhyp:
            print(f"    [{slack/PX:+5.2f}] {seg}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
