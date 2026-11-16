#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
book_layout_hit_test() 必须是 draw_page() 的等价重放——否则点词会静默错位（不报错，只是点到
别的词）。两条循环骨架没法合并（一个要画、一个只算），但**排版算术**已经抽成一组共享内联
函数，两边都调用同一份。本测试守住这件事：

  1. 两个循环体都必须调用全部共享 helper；
  2. 两个循环体里都不许再出现那些被手抄过的原始算术（一旦有人把手抄版写回来就失败）。

只做静态检查，不编译：改动能被 CI 立刻抓住，而不用等真机点错词才发现。

book_layout_hit_test() must replay draw_page() exactly, or tap-to-lookup silently maps clicks to
the wrong word. The two loop skeletons cannot be merged (one paints, one only measures), but the
layout arithmetic now lives in a group of shared inline helpers that both sides call. This test
guards that:
  1. both loop bodies must call every shared helper;
  2. neither loop body may contain the arithmetic that used to be hand-copied.

Static only, nothing is compiled: a regression is caught by CI instead of by a user tapping the
wrong word on real hardware.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / 'main' / 'book' / 'book_layout.c'

# 共享内联函数：两个孪生循环都必须调用。/ Shared inline helpers both twin loops must call.
COMMON_HELPERS = [
    'advance_after_image(',
    'line_leading(',
    'line_fits(',
    'line_origin_x(',
    'line_is_final(',
    'line_is_justified(',
    'advance_after_line(',
]

# 右边界这一段两边走的是**同一个函数的两条入口**，不是同一个名字：
#   · draw_page 取 `line_ink_width()`（它要的是着墨宽度，反色底也用它）；
#   · hit_test  取 `justify_target_px()`（它要的是字距调整的 target）。
# 26.9.12 把绘制侧统一到 line_ink_width 之后，写死「两边都必须出现 justify_target_px(」
# 就不再成立——而这条断言当时没人跑（脚本在门禁外），红了很久没人知道。
# `line_ink_width()` 内部必须仍调用 `justify_target_px()`：只断了这一层，两个循环体看
# 起来一个字都没变，右边界却已经开始各算各的。下面 INDIRECT 就是咬这一层。
DRAW_ONLY = ['line_ink_width(']
HIT_ONLY = ['justify_target_px(']
# (helper 定义名, 它体内必须出现的调用)
# (definition name, the call its body must still contain)
INDIRECT = [('line_ink_width', 'justify_target_px(')]
HELPERS = COMMON_HELPERS + DRAW_ONLY + HIT_ONLY

# 曾经被手抄进两个循环、现在必须只存在于 helper 定义里的原始算术。
# Raw arithmetic that used to be hand-copied into both loops and must now live only in the helpers.
FORBIDDEN = [
    ('+ s_px / 2', 'post-image gap'),
    ('+ px / 2', 'post-image gap'),
    ('used ? margin_before : 0', 'leading'),
    ('used + leading + line_height >', 'line fit check'),
    ('if (align == 1)', 'alignment offset'),
    ('else if (align == 2)', 'alignment offset'),
    ('next >= s_len ||', 'final-line test'),
    ('used += line_height;', 'line advance'),
    ('paragraph_end) used += gap_for', 'paragraph gap'),
    ('line_px / 3', 'justify hang'),
]

FUNCTIONS = ['void book_layout_draw_page(', 'bool book_layout_hit_test(']


def function_body(text: str, signature: str) -> str:
    """Return the braces-balanced body of a function, or '' when not found."""
    start = text.find(signature)
    if start < 0:
        return ''
    depth = 0
    seen_open = False
    for i in range(start, len(text)):
        if text[i] == '{':
            depth += 1
            seen_open = True
        elif text[i] == '}':
            depth -= 1
            if seen_open and depth == 0:
                return text[start:i + 1]
    return ''


def main() -> int:
    if not SOURCE.exists():
        print(f'FAIL: missing {SOURCE}')
        return 1
    text = SOURCE.read_text(encoding='utf-8')
    bodies = {}
    for signature in FUNCTIONS:
        body = function_body(text, signature)
        if not body:
            print(f'FAIL: could not locate {signature!r} in {SOURCE.name}')
            return 1
        bodies[signature] = body

    failures = []
    for signature, body in bodies.items():
        name = signature.split('(')[0].split()[-1]
        required = COMMON_HELPERS + (DRAW_ONLY if name == 'book_layout_draw_page' else HIT_ONLY)
        for helper in required:
            if helper not in body:
                failures.append(f'{name}: does not call {helper[:-1]}()')
        for needle, label in FORBIDDEN:
            if needle in body:
                failures.append(f'{name}: hand-copied {label} ({needle!r}) back into the loop')

    # helper 只能有一份定义，别被复制成两份各改各的。/ Each helper must be defined exactly once.
    for helper in HELPERS:
        name = helper.split('(')[0]
        defined = len(re.findall(r'\bstatic\s+inline\s+[A-Za-z_][A-Za-z0-9_]*\s+' + name + r'\s*\(', text))
        if defined != 1:
            failures.append(f'{name}(): expected exactly one definition, found {defined}')

    # 间接那一层：draw 侧的着墨宽度必须仍然是 justify_target_px 的调用者。
    # The indirect link: drawing's ink width must still be a caller of justify_target_px.
    for name, must_call in INDIRECT:
        body = function_body(text, f'static inline int {name}(')
        if not body:
            failures.append(f'{name}(): missing definition, so the indirect link cannot be judged')
        elif must_call not in body:
            failures.append(f'{name}() no longer delegates to {must_call[:-1]}(): draw and hit '
                            f'would then compute the right edge separately')

    if failures:
        for line in failures:
            print('  FAIL ' + line)
        print('book_layout_twin_host_test: FAIL')
        return 1
    print(f'PASS: {len(COMMON_HELPERS)} shared helpers used by both twins, '
          f'{len(DRAW_ONLY)}+{len(HIT_ONLY)} entry points for the right edge (line_ink_width '
          f'still delegates to justify_target_px), '
          f'{len(FORBIDDEN)} hand-copied expressions absent from both loops')
    return 0


if __name__ == '__main__':
    sys.exit(main())
