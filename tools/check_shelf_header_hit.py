#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""26.11.3 结构牙：书架头部三按钮的字号一致性与命中收口。

两条真机缺陷各咬一组：
  Bug 1（字号）——三处 draw_shelf_header_button 的 font 实参必须恒等；文案必须
                 放得下（无「待」、无全角间隔符），宽度实测 ≤ 预算。
  Bug 2（串台）——分类感应区必须命名、右界零重叠、路由有按钮优先守卫，
                 并把边界两侧（332/333）的归属钉死。

判据一律咬行为/几何关系，不咬文本巧合：注释先剥、字面量按语义解析、
边界采样用源码里的真实矩形值而不是硬编码坐标。

运行：python3 tools/check_shelf_header_hit.py
"""

import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
APP = ROOT / "main/apps/app_book.c"
TTF = ROOT / "main/assets/builtin.ttf"

# 按钮内可用宽：97 - 2*12 padding。文案超了就会顶到圆角。
LABEL_BUDGET_PX = 73
LABEL_PX = 20


def strip_comments(text: str) -> str:
    """去注释，避免注释字面量骗过判据（26.11.1 被咬过三次）。"""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def function_body(text: str, name: str) -> str:
    """按花括号配平取函数体（第一个同名定义）。"""
    m = re.search(r"\b" + re.escape(name) + r"\s*\([^;{]*\)\s*\{", text)
    if not m:
        return ""
    i = m.end() - 1
    depth = 0
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[m.start() : i + 1]
        i += 1
    return ""


def rect_of(body: str, fn: str):
    """从 `static EpdRect fn(void) { return (EpdRect){x, y, w, h}; }` 取四元组。"""
    m = re.search(r"\(EpdRect\)\s*\{\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\}", body)
    if not m:
        return None
    return tuple(int(g) for g in m.groups())


def hit(rect, x, y) -> bool:
    return rect is not None and rect[0] <= x < rect[0] + rect[2] and rect[1] <= y < rect[1] + rect[3]


# ---------------------------------------------------------------- 字宽实测
def ttf_advance_width(s: str, px: int) -> int:
    """复刻 ui_font.c 的取宽：stbtt_ScaleForPixelHeight + lroundf(advance*scale)。

    直接读 ttf 的 head/hhea/hmtx/cmap(format 4)——宿主链接整条字体链要拖进
    miniz/tinfl，太重；字节级解析是同一份数据的另一条读法，可复现。
    """
    d = TTF.read_bytes()
    tables = {}
    for i in range(struct.unpack_from(">H", d, 4)[0]):
        tag, _cs, off, _ln = struct.unpack_from(">4sIII", d, 12 + i * 16)
        tables[tag.decode()] = off
    head, hhea, hmtx = tables["head"], tables["hhea"], tables["hmtx"]
    asc = struct.unpack_from(">h", d, hhea + 4)[0]
    desc = struct.unpack_from(">h", d, hhea + 6)[0]
    nhm = struct.unpack_from(">H", d, hhea + 34)[0]
    cmap = tables["cmap"]
    sub = None
    for i in range(struct.unpack_from(">H", d, cmap + 2)[0]):
        pid, eid, off = struct.unpack_from(">HHI", d, cmap + 4 + i * 8)
        if (pid, eid) == (3, 1) and struct.unpack_from(">H", d, cmap + off)[0] == 4:
            sub = cmap + off
            break
    if sub is None:
        raise RuntimeError("no (3,1) format-4 cmap")
    segx2 = struct.unpack_from(">H", d, sub + 6)[0]
    seg = segx2 // 2
    ends = [struct.unpack_from(">H", d, sub + 14 + i * 2)[0] for i in range(seg)]
    starts = [struct.unpack_from(">H", d, sub + 16 + segx2 + i * 2)[0] for i in range(seg)]
    deltas = [struct.unpack_from(">h", d, sub + 16 + 2 * segx2 + i * 2)[0] for i in range(seg)]
    ro_base = sub + 16 + 3 * segx2
    ros = [struct.unpack_from(">H", d, ro_base + i * 2)[0] for i in range(seg)]

    def gid(cp):
        for i in range(seg):
            if starts[i] <= cp <= ends[i]:
                if ros[i] == 0:
                    return (cp + deltas[i]) & 0xFFFF
                g = struct.unpack_from(">H", d, ro_base + i * 2 + ros[i] + (cp - starts[i]) * 2)[0]
                return (g + deltas[i]) & 0xFFFF if g else 0
        return 0

    def adv(g):
        return struct.unpack_from(">H", d, hmtx + g * 4)[0] if g < nhm else struct.unpack_from(">H", d, hmtx + (nhm - 1) * 4)[0]

    scale = px / float(asc - desc)
    return sum(int(round(adv(gid(ord(c))) * scale)) for c in s)


def run_checks(src: str):
    """对（已去注释的）源码跑全部判据，返回错误列表——自检模式靠它吃变异体。"""
    errs = []

    # ---- T1：所有按钮绘制的 font 实参恒等（不是"等于 20"——将来改字号一起改）
    # 调用会跨行（局部重绘那段换行写），按括号配平取实参，正则抓不全。
    fonts = []
    for m in re.finditer(r"draw_shelf_header_button\s*\(", src):
        i, depth, start = m.end() - 1, 0, m.end()
        args = []
        while i < len(src):
            ch = src[i]
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0:
                    args.append(src[start:i])
                    break
            elif ch == "," and depth == 1:
                args.append(src[start:i])
                start = i + 1
            i += 1
        if len(args) == 5:  # (fb, rect, id, label, font)
            tail = args[-1].strip()
            if tail.isdigit():
                fonts.append(int(tail))
    if len(fonts) < 6:  # 3 处整屏绘制 + 3 处局部重绘
        errs.append(f"T1: 只解析到 {len(fonts)} 处带字号实参的按钮绘制，应有 6 处 {sorted(fonts)}")
    if fonts and len(set(fonts)) != 1:
        errs.append(f"T1: 三按钮字号不统一 {sorted(set(fonts))}——必须恒等")

    # ---- T2：文案放得下（无「待」、无全角间隔符），且按真值拼
    label = function_body(src, "shelf_excerpt_label")
    if not label:
        errs.append("T2: shelf_excerpt_label 失踪")
    else:
        if "待" in label:
            errs.append("T2: 文案又出现「待」——20 号放不下，会把字号拖回去")
        if "·" in label:
            errs.append("T2: 文案出现全角间隔符「·」——「摘抄·10」≈58px+，超出预算")
        if not re.search(r'"摘抄 %u"', label):
            errs.append('T2: 文案必须用 "摘抄 %u" 按真值拼')

    # ---- 字宽实测：最长态（队列封顶 10）必须 ≤ 预算
    longest = "摘抄 10"
    w = ttf_advance_width(longest, LABEL_PX)
    if w > LABEL_BUDGET_PX:
        errs.append(f"T2b: {longest!r}@{LABEL_PX} = {w}px 超出 {LABEL_BUDGET_PX}px 预算")

    # ---- 几何：把命名矩形解析成真实坐标，后面的判据全用它们
    tap = rect_of(function_body(src, "shelf_category_tap_rect"), "shelf_category_tap_rect")
    ex = rect_of(function_body(src, "shelf_excerpt_rect"), "shelf_excerpt_rect")
    mg = rect_of(function_body(src, "shelf_manage_rect"), "shelf_manage_rect")
    im = rect_of(function_body(src, "shelf_import_rect"), "shelf_import_rect")
    for name, r in (("tap", tap), ("excerpt", ex), ("manage", mg), ("import", im)):
        if r is None:
            errs.append(f"G: 取不到 {name} 矩形（解析失败）")

    # ---- T3：匿名分类框字面量绝迹（这是缺陷本体，注释里出现不算）
    for m in re.finditer(r"\(EpdRect\)\s*\{\s*24\s*,\s*79\s*,\s*388\s*,\s*74\s*\}", src):
        errs.append("T3: 匿名分类命中框 {24,79,388,74} 又回来了")
        break

    if tap and ex:
        # 零重叠：感应区右界不得碰到按钮左界
        if tap[0] + tap[2] > ex[0]:
            errs.append(f"T3b: 感应区右界 {tap[0]+tap[2]} 越过按钮左界 {ex[0]}——重叠 {tap[0]+tap[2]-ex[0]}px")
        # 且必须够宽到覆盖箭头满预算（36+230+27+26）
        if tap[0] + tap[2] < 36 + 230 + 27 + 26:
            errs.append(f"T3c: 感应区右界 {tap[0]+tap[2]} 盖不住箭头满预算右缘 {36+230+27+26}")

    # ---- 边界采样：(332,120) 归分类，(333,120) 归按钮。边界两侧归属互换就是收口正确性
    if tap and ex:
        def owner(x, y):
            if hit(ex, x, y) or hit(mg, x, y) or hit(im, x, y):
                return "button"
            if hit(tap, x, y):
                return "category"
            return "none"

        if owner(332, 120) != "category":
            errs.append(f"B1: (332,120) 应归分类，实际 {owner(332,120)}")
        if owner(333, 120) != "button":
            errs.append(f"B2: (333,120) 应归按钮，实际 {owner(333,120)}")
        # 按钮全域采样：97×54 里任何一点都不能落进分类
        bad = [(x, y) for x in range(ex[0], ex[0] + ex[2], 4) for y in range(ex[1], ex[1] + ex[3], 4)
               if hit(tap, x, y)]
        if bad:
            errs.append(f"B3: 按钮内有 {len(bad)} 个采样点仍被感应区吞掉，首例 {bad[0]}")

    # ---- G2：三按钮的关系不变式——咬 getter 自身腐烂（字面量回潮测不到这一档）
    if ex and mg and im:
        if not (ex[1] == mg[1] == im[1]):
            errs.append(f"G2: 三按钮不同基线 y={ex[1]}/{mg[1]}/{im[1]}——有人单独改了某个 getter 的 y")
        if not (ex[3] == mg[3] == im[3]):
            errs.append(f"G2: 三按钮不同高 h={ex[3]}/{mg[3]}/{im[3]}——有人单独改了某个 getter 的 h")
        if not (ex[2] == mg[2] == im[2]):
            errs.append(f"G2: 三按钮不同宽 w={ex[2]}/{mg[2]}/{im[2]}")
        if mg[0] - (ex[0] + ex[2]) != 12 or im[0] - (mg[0] + mg[2]) != 12:
            errs.append(f"G2: 按钮间距不再是 12（{mg[0]-(ex[0]+ex[2])}/{im[0]-(mg[0]+mg[2])}）")

    # ---- T4：感应区必须通过命名 getter 使用（两处：手势路由 + 按下判定）
    uses = len(re.findall(r"shelf_category_tap_rect\s*\(\s*\)", src))
    if uses < 2:
        errs.append(f"T4: shelf_category_tap_rect() 只被用了 {uses} 次，应 ≥2（路由 + 按下）")

    # ---- T5：按钮优先守卫必须在两处都出现
    guards = len(re.findall(r"!\s*shelf_header_button_hit\s*\(", src))
    if guards < 2:
        errs.append(f"T5: 按钮优先守卫只出现 {guards} 次，应 ≥2")

    # ---- T6：跨矩形关系断言
    if not re.search(r"_Static_assert\s*\(\s*24\s*\+\s*309\s*<=\s*333", src):
        errs.append("T6: 缺跨矩形断言 24+309<=333（这正是当初漏掉的那条）")

    # ---- T7：按钮坐标只许有一个来源。getter 之外任何地方出现 (EpdRect){333,...}
    # 都是第四份副本——包括"先赋给局部变量再比较"这种绕法（自检抓过）。
    # getter 自身的定义是唯一合法来源，先摘掉它再查其余部分。
    others = src
    for name in ("shelf_excerpt_rect", "shelf_manage_rect", "shelf_import_rect"):
        others = others.replace(function_body(src, name), "")
    if re.search(r"\(EpdRect\)\s*\{\s*333\s*,", others):
        errs.append("T7: 又出现了一份 (EpdRect){333,...} 字面量——按钮坐标必须只由 getter 给出")
    if re.search(r"\(EpdRect\)\s*\{\s*442\s*,", others):
        errs.append("T7: 出现 (EpdRect){442,...} 字面量——管理按钮坐标必须只由 getter 给出")

    return errs


def main() -> int:
    errs = run_checks(strip_comments(APP.read_text()))
    if errs:
        print("RED — 26.11.3 结构牙咬到 %d 处：" % len(errs))
        for e in errs:
            print("  ✗", e)
        return 1
    w = ttf_advance_width("摘抄 10", LABEL_PX)
    print(f"GREEN — 26.11.3 七牙全过；'摘抄 10'@{LABEL_PX} 实测 {w}px ≤ {LABEL_BUDGET_PX}px；"
          f"感应区 (24,79,309,74) / 按钮 (333,94,97,54) 零重叠；边界 332=category 333=button")
    return 0

# ---------------------------------------------------------------- 负向自检
# 七个靶点，每个都必须能让牙变红；不红的靶点说明判据只是"看着像"。
# 审计 26.11.3 补的三档：T1 双靶（绘制侧/局部重绘侧各一）、T4 与 T7 的
# getter 漂移档（字面量回潮测到了，getter 自身腐烂没测到）、边界两侧归属。
MUTATIONS = [
    ("T1-a 绘制侧字号退回 16", lambda s: s.replace(
        'draw_shelf_header_button(fb, shelf_excerpt_rect(), 118, shelf_excerpt_label(excerpt_label, sizeof(excerpt_label)), 20);',
        'draw_shelf_header_button(fb, shelf_excerpt_rect(), 118, shelf_excerpt_label(excerpt_label, sizeof(excerpt_label)), 16);')),
    ("T1-b 局部重绘侧字号退回 16", lambda s: s.replace(
        'shelf_excerpt_label(excerpt_label, sizeof(excerpt_label)), 20);',
        'shelf_excerpt_label(excerpt_label, sizeof(excerpt_label)), 16);')),
    ("T2 文案退回「·待」", lambda s: s.replace(
        'snprintf(buf, cap, "摘抄 %u", n);', 'snprintf(buf, cap, "摘抄·待%u", n);')),
    ("T3 感应区退回匿名 388 框", lambda s: s.replace(
        'static EpdRect shelf_category_tap_rect(void) { return (EpdRect){24, 79, 309, 74}; }',
        'static EpdRect shelf_category_tap_rect(void) { return (EpdRect){24, 79, 388, 74}; }')),
    ("T4-a 路由退回裸字面量", lambda s: s.replace(
        'ui_rect_hit(shelf_category_tap_rect(),ev->x0,ev->y0)', 'ui_rect_hit((EpdRect){24,79,309,74},ev->x0,ev->y0)')),
    ("T4-b 感应区 getter 漂移（309→310，吃进按钮 1px）", lambda s: s.replace(
        '{ return (EpdRect){24, 79, 309, 74}; }', '{ return (EpdRect){24, 79, 310, 74}; }')),
    ("T5 删按钮优先守卫", lambda s: s.replace(
        ' && !shelf_header_button_hit(ev->x0,ev->y0)', '')),
    ("T6 删跨矩形断言", lambda s: s.replace(
        '_Static_assert(24 + 309 <= 333, "26.11.3: category sensing rect must not reach the excerpt button");', '')),
    ("T7 局部重绘抄回字面量", lambda s: s.replace(
        'const EpdRect ex_btn = shelf_excerpt_rect();', 'const EpdRect ex_btn = (EpdRect){333,94,97,54};')),
    ("G2 按钮 getter 漂移（y 94→96，三钮不同基线）", lambda s: s.replace(
        '{ return (EpdRect){333, 94, 97, 54}; }', '{ return (EpdRect){333, 96, 97, 54}; }')),
]


def self_test() -> int:
    src = strip_comments(APP.read_text())
    if run_checks(src):
        print("SELF-TEST 前提不成立：干净源码已经是红的")
        return 1
    bad = 0
    for name, mutate in MUTATIONS:
        mutated = mutate(src)
        if mutated == src:
            print(f"  ?? {name}：变异没落地（靶点漂移），这条自检无效")
            bad += 1
            continue
        errs = run_checks(mutated)
        if errs:
            print(f"  OK {name} → 红（{errs[0][:56]}）")
        else:
            print(f"  XX {name} → 仍然绿，判据没咬住")
            bad += 1
    print(f"SELF-TEST {'PASS' if bad == 0 else 'FAIL'}：{len(MUTATIONS) - bad}/{len(MUTATIONS)} 靶点全红")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(self_test() if "--self-test" in sys.argv else main())
