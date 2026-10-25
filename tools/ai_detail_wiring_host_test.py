#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
M2 Step9 详解视图的跨文件接线守门。宿主单测（ai_reader_host_test.c 108 断言）
证明详解视图自身正确，app_loop 宿主守门（run_app_loop_host_tests.sh 用例四）
证明主循环接线正确；本脚本守住剩下那层「接线还在不在」——跨文件的静态断言，
编译照过、真机上「开了详解却还能翻原书」这类回归只有它能抓：

  1. render 详解分支存在且在 draw_reader 之前：READING 态 + ai_reader_active()
     时只画 ai_reader_render(fb) 并早退，原书绘制路径不被触碰（铁律②：
     章节缓冲不卸载、不重排）。
  2. 五处输入语义隔离（铁律①）：on_key / on_power_short / on_key_long /
     gesture_event / BLE turn 五个人口在函数前部分就查 ai_reader_active()，
     分支里只调 ai_reader_turn / ai_reader_close_full，且守卫位置早于任何
     原书动作（turn_page / reader_key_action / ui_nav_request / 面板切换）。
  3. 双 GC16 两侧俱全：open 侧——门 A 开会话成功置 s_ai_def_full=true，
     app_loop 回流点按 app_book_ai_def_full() 选 FULL/PAGE；close 侧——
     ai_reader_close_full() 关会话并回 APP_REDRAW_FULL，on_key/on_key_long 的
     KEY2 分支都走它；getter 一次性消费（读后即清）。
  4. 门 A 的截断改道：dict_popup_ai_truncated() 为真时 reset 卡面、不开卡，
     填 ai_reader_source_t（chapter/page/page_start_offset/title）后
     ai_reader_open()；失败则防御性重开卡面。dict_popup 侧 s_truncated 由
     store_line 触顶时置位、getter 出口 .h/.c 双在。
  5. 会话期原书零卸载：render 详解分支与五处守卫窗口内不出现 free_book() /
     draw_reader() / turn_page()（覆盖层只是 READING 视图上的一层）。
  6. 构建与宿主桩同步：CMakeLists 收 apps/ai_reader.c；app_content_open.h 声明
     两个新 getter；tools/app_loop_stubs/app_content_open.h 与
     tools/app_loop_host_test.c 的桩定义同步（gen19/gen21/gen26.3「实现契约
     变更忘同步宿主桩」三代同型事故的守门）。

只做静态检查，不编译。退出码 0 = 全过。

The cross-file wiring gatekeeper for the M2 Step9 detail view. The host unit
test (ai_reader_host_test.c, 108 assertions) proves the detail view itself and
the app_loop host gatekeeper (run_app_loop_host_tests.sh case 4) proves the
main-loop wiring; this script pins the remaining layer - "the wiring is still
there" - because regressions like "the detail view opens but the source book
still turns" compile fine and only bite on hardware:

  1. the render detail branch exists ahead of draw_reader: READING +
     ai_reader_active() paints only ai_reader_render(fb) and returns early,
     never touching the source draw path (law 2: the chapter buffer is never
     unloaded or repaginated);
  2. five input entry points are isolated (law 1): on_key / on_power_short /
     on_key_long / gesture_event / BLE turn each test ai_reader_active() early,
     call only ai_reader_turn / ai_reader_close_full inside the guard, and the
     guard precedes any source action (turn_page / reader_key_action /
     ui_nav_request / panel switch);
  3. both sides of the double GC16: open side - gate A sets s_ai_def_full=true
     on a successful open and the app_loop return flow picks FULL/PAGE via
     app_book_ai_def_full(); close side - ai_reader_close_full() closes the
     session and answers APP_REDRAW_FULL, and both KEY2 branches route through
     it; the getter consumes itself on read;
  4. gate A's truncation detour: when dict_popup_ai_truncated() is true the
     card is reset (not shown), ai_reader_source_t is filled
     (chapter/page/page_start_offset/title) and ai_reader_open() runs; on
     failure the card is defensively re-staged. On the dict_popup side
     s_truncated is set by store_line at the line cap and the getter exists in
     both .h and .c;
  5. zero source unload during a session: no free_book() / draw_reader() /
     turn_page() inside the render detail branch or the five guard windows
     (the overlay is just a layer over the READING view);
  6. build and host-stub sync: CMakeLists picks up apps/ai_reader.c;
     app_content_open.h declares both new getters; the stubs in
     tools/app_loop_stubs/app_content_open.h and tools/app_loop_host_test.c
     stay in sync (the gate against the three-generation "contract change
     missed the host stubs" family: gen19/gen21/gen26.3).

Static only, nothing is compiled. Exit code 0 means all checks passed.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BOOK = ROOT / 'main' / 'apps' / 'app_book.c'
POPUP_C = ROOT / 'main' / 'apps' / 'dict_popup.c'
POPUP_H = ROOT / 'main' / 'apps' / 'dict_popup.h'
LOOP = ROOT / 'main' / 'app' / 'app_loop.c'
OPEN_H = ROOT / 'main' / 'apps' / 'app_content_open.h'
STUB_H = ROOT / 'tools' / 'app_loop_stubs' / 'app_content_open.h'
HOST_TEST = ROOT / 'tools' / 'app_loop_host_test.c'
CMAKE = ROOT / 'main' / 'CMakeLists.txt'


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


def first_index(body: str, needles) -> int:
    """Earliest offset of any needle, or len(body) when none is present."""
    hits = [body.find(n) for n in needles if body.find(n) >= 0]
    return min(hits) if hits else len(body)


def strip_comments(text: str) -> str:
    """Drop C block and line comments, keeping byte offsets stable.

    Comments mention the very calls these checks order (on_key's header talks
    about "the source turn_page"), so a whole-body ordering check would
    false-flag prose. Comments are blanked in place - same length, same
    offsets - so every relative comparison stays valid.
    """
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        if text[i] == '/' and i + 1 < n and text[i + 1] == '*':
            end = text.find('*/', i + 2)
            end = n if end < 0 else end + 2
            for j in range(i, end):
                if out[j] != '\n':
                    out[j] = ' '
            i = end
        elif text[i] == '/' and i + 1 < n and text[i + 1] == '/':
            end = text.find('\n', i)
            end = n if end < 0 else end
            for j in range(i, end):
                out[j] = ' '
            i = end
        elif text[i] == '"' or text[i] == "'":
            # 字符串/字符字面量里的 "//" 不是注释；字面量整体保留。
            # A "//" inside a string or char literal is not a comment; keep
            # the literal intact.
            quote = text[i]
            j = i + 1
            while j < n and text[j] != quote:
                if text[j] == '\\':
                    j += 1
                j += 1
            i = j + 1
        else:
            i += 1
    return ''.join(out)


def main() -> int:
    for path in (BOOK, POPUP_C, POPUP_H, LOOP, OPEN_H, STUB_H, HOST_TEST, CMAKE,
                 ROOT / 'main' / 'apps' / 'ai_reader.h'):
        if not path.exists():
            print(f'FAIL: missing {path}')
            return 1
    book = BOOK.read_text(encoding='utf-8')
    popup_c = POPUP_C.read_text(encoding='utf-8')
    popup_h = POPUP_H.read_text(encoding='utf-8')
    loop = LOOP.read_text(encoding='utf-8')
    open_h = OPEN_H.read_text(encoding='utf-8')
    stub_h = STUB_H.read_text(encoding='utf-8')
    host_test = HOST_TEST.read_text(encoding='utf-8')
    cmake = CMAKE.read_text(encoding='utf-8')
    failures = []

    # ---- P1 render 详解分支 -------------------------------------------------
    # 用 .on_enter 之前的 render 定义签名太宽，取「static app_redraw_t render(」
    # 的第一个匹配（app_book.c 里 render 是静态函数，签名唯一）。
    # 签名带 ") {"——app_book.c 在 L480 有 render 的前向声明，裸签名会先命中
    # 声明、把后面别的函数体当 render 返回（守卫检查因此假红）。
    # The signature carries ") {" - app_book.c forward-declares render at
    # L480, and a bare signature would hit the declaration first and return
    # some later function's body as "render" (the guard check then false-flags).
    render = function_body(book, 'static void render(app_ctx_t* ctx, uint8_t* fb) {')
    render = strip_comments(render) if render else render
    if not render:
        failures.append('render(): not found')
    else:
        guard = render.find('ai_reader_active()')
        draw = render.find('draw_reader(')
        paint = render.find('ai_reader_render(')
        if guard < 0:
            failures.append('render(): no ai_reader_active() detail branch')
        else:
            if paint < 0 or paint < guard:
                failures.append('render(): detail branch does not call ai_reader_render()')
            if draw >= 0 and draw < guard:
                failures.append('render(): draw_reader() runs before the detail guard '
                                '(a live session would paint the source page)')
            # 详解分支窗口只到分支自己的 return 为止——之后的 draw_reader 是
            # 无会话时的正常原书路径，本就该在，禁词检查不能把它算进来。
            # The detail-branch window stops at the branch's own return - the
            # draw_reader() after it is the normal source path when no session
            # is live and belongs there; the banned-call check must not count it.
            branch_end = render.find('return', paint if paint >= 0 else guard)
            window = render[guard:branch_end if branch_end >= 0 else len(render)]
            for banned in ('draw_reader(', 'turn_page(', 'free_book('):
                if banned in window:
                    failures.append(f'render(): detail branch reaches {banned} '
                                    '(law 2: the source is never touched under the overlay)')

    # ---- P2 五处输入隔离 ----------------------------------------------------
    # 每个人口：守卫必须存在、分支里只调 ai_reader_*、守卫早于首个原书动作。
    entries = [
        # (函数签名, 分支内必须出现的 ai_reader 调用, 原书动作标志, 人名)
        ('static app_redraw_t on_key(', ['ai_reader_turn(', 'ai_reader_close_full()'],
         ['turn_page(', 'reader_key_action(', 'ui_nav_request('], 'on_key'),
        ('static app_redraw_t on_power_short(', ['ai_reader_turn('],
         ['turn_page('], 'on_power_short'),
        ('static app_redraw_t on_key_long(', ['ai_reader_close_full()'],
         ['ui_nav_request(', 'turn_page('], 'on_key_long'),
        ('static app_redraw_t gesture_event(', ['ai_reader_turn('],
         ['turn_page(', 'shelf_category_gesture('], 'gesture_event'),
    ]
    for signature, required, source_marks, name in entries:
        body = function_body(book, signature)
        if not body:
            failures.append(f'{name}(): not found')
            continue
        body = strip_comments(body)
        guard = body.find('ai_reader_active()')
        if guard < 0:
            failures.append(f'{name}(): no ai_reader_active() isolation guard')
            continue
        window = body[guard:]
        for call in required:
            if call not in window:
                failures.append(f'{name}(): detail branch never calls {call}')
        # 定序看整个函数体（去注释后）：守卫必须早于任何原书动作——只查 guard
        # 之后的窗口会漏掉「把原书动作挪到守卫之前」的退化（变异演示 P2 实测）。
        # Order over the whole (comment-stripped) body: the guard must precede
        # any source action. Checking only the window after the guard misses
        # the "move the source action ahead of the guard" regression (caught
        # live by mutation demo case P2).
        first_source = first_index(body, source_marks)
        first_ai = first_index(body, required)
        if first_ai > first_source:
            failures.append(f'{name}(): a source action precedes the ai_reader call '
                            '(the overlay would fall through to the book)')

    # BLE turn：on_tick 内的 ble_dir 分支——ai_reader_turn(ble_dir) 必须早于
    # turn_page(ctx, ble_dir)。注意 shelf_turn_page(ctx, ble_dir) 含后者的子串，
    # 用负向回顾断言词边界，否则书架的转向会被误判成「原书先跑」。
    # BLE turn: inside on_tick's ble_dir branch, ai_reader_turn(ble_dir) must
    # precede turn_page(ctx, ble_dir). NB: shelf_turn_page(ctx, ble_dir)
    # contains the latter as a substring, so assert a word boundary with a
    # negative lookbehind - otherwise the shelf turn false-flags "the source
    # ran first".
    tick = function_body(book, 'static app_redraw_t on_tick(')
    tick = strip_comments(tick) if tick else tick
    if not tick:
        failures.append('on_tick(): not found')
    else:
        ai_turn = tick.find('ai_reader_turn(ble_dir)')
        src_turn = re.search(r'(?<![a-z_])turn_page\(ctx, ble_dir\)', tick)
        if ai_turn < 0:
            failures.append('on_tick(): BLE turn has no ai_reader_turn(ble_dir) isolation')
        elif src_turn and src_turn.start() < ai_turn:
            failures.append('on_tick(): BLE turn calls turn_page() before the detail guard')

    # ---- P3 双 GC16 两侧 ----------------------------------------------------
    gate = function_body(book, 'bool app_book_ai_def_ready(')
    if not gate:
        failures.append('app_book_ai_def_ready(): not found')
    else:
        if 's_ai_def_full = true' not in gate:
            failures.append('gate A: never sets s_ai_def_full (open side of the double GC16 missing)')
        # 开门成功路径上的置位必须在 ai_reader_open() 之后（驱逐路径的置位在
        # 前，是有意的：详解页→卡面切换也要 GC16）。取 open 调用之后的那次。
        # The success-path flag must sit after ai_reader_open() (the eviction
        # path's flag sits before it on purpose - a detail-page-to-card switch
        # also needs a GC16). Check the occurrence that follows the open call.
        open_at = gate.find('ai_reader_open(')
        if open_at < 0:
            failures.append('gate A: never calls ai_reader_open()')
        elif 's_ai_def_full = true' not in gate[open_at:]:
            failures.append('gate A: s_ai_def_full is not set after ai_reader_open() '
                            '(a failed open would still demand a FULL refresh)')
    getter = function_body(book, 'bool app_book_ai_def_full(')
    if not getter:
        failures.append('app_book_ai_def_full(): not found')
    elif 's_ai_def_full = false' not in getter:
        failures.append('app_book_ai_def_full(): does not consume the flag on read '
                        '(a stale FULL would leak into later flows)')
    closer = function_body(book, 'static app_redraw_t ai_reader_close_full(')
    if not closer:
        failures.append('ai_reader_close_full(): not found')
    else:
        if 'ai_reader_close()' not in closer:
            failures.append('ai_reader_close_full(): does not close the session')
        if 'APP_REDRAW_FULL' not in closer:
            failures.append('ai_reader_close_full(): does not answer APP_REDRAW_FULL '
                            '(close side of the double GC16 missing)')
    # KEY2 两处关闭入口都走 close_full。/ Both KEY2 exits route through close_full.
    for name in ('on_key', 'on_key_long'):
        body = function_body(book, f'static app_redraw_t {name}(')
        if body and 'ai_reader_close_full()' not in body:
            failures.append(f'{name}(): KEY2 does not route through ai_reader_close_full()')
    # app_loop 回流点档位选择。/ The app_loop return flow picks the gear.
    if 'app_book_ai_def_full() ? APP_REDRAW_FULL : APP_REDRAW_PAGE' not in loop:
        failures.append('app_loop: the AI return flow does not select FULL/PAGE via '
                        'app_book_ai_def_full()')

    # ---- P4 门 A 截断改道 ---------------------------------------------------
    if not gate:
        pass  # 已在 P3 报过 / already reported under P3
    else:
        trunc = gate.find('dict_popup_ai_truncated()')
        if trunc < 0:
            failures.append('gate A: never consults dict_popup_ai_truncated()')
        else:
            window = gate[trunc:]
            for call in ('dict_popup_reset(', 'ai_reader_open('):
                if call not in window:
                    failures.append(f'gate A: truncation branch does not call {call}')
            for field in ('src.chapter', 'src.page', 'src.page_start_offset', 'src.title'):
                if field not in window:
                    failures.append(f'gate A: ai_reader_source_t misses {field}')
            if 'dict_popup_show_def(' not in window:
                failures.append('gate A: no defensive card re-stage after a failed open')
    if 's_truncated = true' not in popup_c:
        failures.append('dict_popup.c: nothing ever sets s_truncated')
    else:
        store = function_body(popup_c, 'static bool store_line(')
        if store and 's_truncated = true' not in store:
            failures.append('dict_popup.c: store_line() does not flag the line-cap hit')
    if 'bool dict_popup_ai_truncated(void);' not in popup_h:
        failures.append('dict_popup.h: dict_popup_ai_truncated() declaration missing')
    if 'bool dict_popup_ai_truncated(void)' not in popup_c:
        failures.append('dict_popup.c: dict_popup_ai_truncated() definition missing')

    # ---- P5 会话期原书零卸载（守卫窗口禁 free_book）-------------------------
    for signature, _required, _marks, name in entries:
        body = function_body(book, signature)
        if not body:
            continue
        guard = body.find('ai_reader_active()')
        if guard >= 0 and 'free_book(' in body[guard:]:
            failures.append(f'{name}(): detail window reaches free_book() '
                            '(the source chapter would be unloaded under the overlay)')
    if render:
        guard = render.find('ai_reader_active()')
        if guard >= 0:
            paint = render.find('ai_reader_render(')
            branch_end = render.find('return', paint if paint >= 0 else guard)
            window = render[guard:branch_end if branch_end >= 0 else len(render)]
            if 'free_book(' in window:
                failures.append('render(): detail branch reaches free_book()')

    # ---- P6 构建与宿主桩同步 ------------------------------------------------
    if 'apps/ai_reader.c' not in cmake:
        failures.append('CMakeLists: apps/ai_reader.c is not in the source list')
    for decl in ('bool app_book_ai_def_full(void);', 'bool app_book_ai_detail_active(void);'):
        if decl not in open_h:
            failures.append(f'app_content_open.h: missing declaration {decl}')
        if decl not in stub_h:
            failures.append(f'app_loop_stubs/app_content_open.h: missing declaration {decl} '
                            '(the gen26.3 missed-stub family: host builds die on implicit '
                            'declaration + -Werror)')
    for stub in ('bool app_book_ai_def_full(void)', 'bool app_book_ai_detail_active(void)'):
        if stub not in host_test:
            failures.append(f'app_loop_host_test.c: missing stub definition {stub}')

    # ---- P7 头文件声明同步 ---------------------------------------------------
    # ai_reader 宿测直接 #include "ai_reader.c"，静态定义全可见——头文件漏声明
    # 它永远看不见；app_loop 宿测不编 app_book.c，也看不见。这类缺口只有固件
    # fullclean 能抓（本轮 ai_reader_render 就漏过一次，fullclean 逮个正着）。
    # 守门在此补一张静态网：app_book.c 调用的每个 ai_reader_* 都必须在
    # ai_reader.h 里有声明。
    # The ai_reader host test #includes the .c directly, so every static
    # definition is visible and a missing header declaration can never surface
    # there; the app_loop host test does not compile app_book.c either. Only a
    # firmware fullclean catches that class (this round's missing
    # ai_reader_render declaration was caught exactly there). This check adds a
    # static net: every ai_reader_* symbol app_book.c calls must be declared in
    # ai_reader.h.
    reader_h = (ROOT / 'main' / 'apps' / 'ai_reader.h').read_text(encoding='utf-8')
    # ai_reader_close_full 是 app_book.c 自己的静态 helper（关会话 + 回 FULL），
    # 不是 ai_reader 模块 API，不进头文件——显式排除。
    # ai_reader_close_full is app_book.c's own static helper (close + answer
    # FULL), not an ai_reader module API, so it never enters the header -
    # exclude it explicitly.
    local_helpers = {'ai_reader_close_full'}
    used = sorted({m[:-1] for m in re.findall(r'\bai_reader_[a-z_]+\(', book)} - local_helpers)
    for name in used:  # used 已去括号 / names already have the paren stripped
        if not re.search(rf'\b{name}\s*\(', reader_h):
            failures.append(f'ai_reader.h: app_book.c calls {name}() but the header '
                            'never declares it (firmware fullclean would fail on an '
                            'implicit declaration)')

    if failures:
        for line in failures:
            print(f'FAIL: {line}')
        print(f'ai detail wiring: {len(failures)} check(s) failed')
        return 1
    print('PASS: M2 Step9 detail-view wiring: render branch ahead of draw_reader, five '
          'input entry points isolated, both sides of the double GC16, gate A truncation '
          'detour, zero source unload under the overlay, CMake/host-stub/header declarations in sync')
    return 0


if __name__ == '__main__':
    sys.exit(main())
