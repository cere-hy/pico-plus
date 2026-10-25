#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
ai_detail_wiring_host_test.py 的变异有牙演示：对六个设计点各做一个针对性变异，
守门必须逐一擒红（退出码非 0），否则说明守门是口号。变异在副本上进行，
原文件逐字节恢复——不动工作区。

A mutation demo for ai_detail_wiring_host_test.py: one targeted mutation per
design point, and the gatekeeper must catch each one (non-zero exit), or the
gate is a slogan. Mutations run on copies and every file is restored byte for
byte afterwards - the working tree is never touched.
"""
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GATE = ROOT / 'tools' / 'ai_detail_wiring_host_test.py'

# (设计点, 文件, 原串, 变异串, 预期守门台词关键词)
MUTATIONS = [
    ('P1 render 详解分支退化：分支改走原书绘制',
     'main/apps/app_book.c',
     '        ai_reader_render(fb);',
     '        draw_reader(fb, s_page);',
     'draw_reader'),
    ('P2 输入隔离退化：on_key 守卫落到原书动作之后',
     'main/apps/app_book.c',
     '    if (s_view == READING && ai_reader_active()) {\n        if (key == UI_KEY_1 || key == UI_KEY_3) {\n            return ai_reader_turn(key == UI_KEY_1 ? -1 : 1);\n        }',
     '    if (key == UI_KEY_2) return reader_key_action(ctx, app_settings_reader_key_action(1));\n    if (s_view == READING && ai_reader_active()) {\n        if (key == UI_KEY_1 || key == UI_KEY_3) {\n            return ai_reader_turn(key == UI_KEY_1 ? -1 : 1);\n        }',
     'source action precedes'),
    ('P3 双 GC16 退化：回流点档位改死 PAGE',
     'main/app/app_loop.c',
     'app_book_ai_def_full() ? APP_REDRAW_FULL : APP_REDRAW_PAGE',
     'APP_REDRAW_PAGE',
     'does not select FULL/PAGE'),
    ('P4 门 A 退化：不再问截断谓词',
     'main/apps/app_book.c',
     'if (dict_popup_ai_truncated()) {',
     'if (0) {',
     'dict_popup_ai_truncated'),
    ('P5 会话期卸载退化：详解分支里卸原书',
     'main/apps/app_book.c',
     '        ai_reader_render(fb);',
     '        free_book();\n        ai_reader_render(fb);',
     'free_book'),
    ('P6 宿主桩忘同步：删桩头声明',
     'tools/app_loop_stubs/app_content_open.h',
     'bool app_book_ai_def_full(void);',
     '// mutated away',
     'missing declaration'),
    ('P7 头文件漏声明：删 ai_reader_render 声明（fullclean 实测缺口）',
     'main/apps/ai_reader.h',
     'void ai_reader_render(uint8_t* fb);',
     '// mutated away',
     'ai_reader_render'),
]


def main() -> int:
    failures = []
    for name, rel, old, new, expect in MUTATIONS:
        path = ROOT / rel
        text = path.read_text(encoding='utf-8')
        if text.count(old) != 1:
            print(f'FAIL: mutation anchor not unique in {rel}: {old!r}')
            failures.append(name)
            continue
        backup = path.read_bytes()
        try:
            path.write_text(text.replace(old, new, 1), encoding='utf-8')
            proc = subprocess.run([sys.executable, str(GATE)],
                                  capture_output=True, text=True)
            caught = proc.returncode != 0 and expect in (proc.stdout + proc.stderr)
            status = 'CAUGHT' if caught else 'MISSED'
            print(f'{status}: {name}')
            if not caught:
                print(f'  gate said: {proc.stdout.strip()}')
                failures.append(name)
        finally:
            path.write_bytes(backup)
    if failures:
        print(f'mutation demo: {len(failures)}/{len(MUTATIONS)} mutations survived '
              '(the gate has no teeth for these)')
        return 1
    print(f'mutation demo: all {len(MUTATIONS)} mutations caught - the wiring '
          'gatekeeper has teeth')
    return 0


if __name__ == '__main__':
    sys.exit(main())
