#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
生词本的接线是跨文件的，靠注释同步太脆弱：app_book.c 里少调一次 vocab_log_capture()、
或者 chip 的点击没落到 dict_popup_tap()，编译照样过、真机上只是"点了没反应"，
要等用户拿卡去查才发现。本测试守住这几处接线：

  1. start_dict_lookup() 必须在 unlock_draw() 之后才抓取上下文（不在持锁期间读 s_text）；
  2. 进入/退出/掉卡的三个生命周期钩子必须调用 warm() / invalidate()，索引不能悬空；
  3. 词典面板的点击必须先交给 dict_popup_tap()，命中 chip 时不关闭卡片；
  4. 弹层必须真的画出 chip，并且头部宽度要为它让位；
  5. 工具条第 6 项必须真的打开视图，且视图内删掉一条后要重渲染 + 重开；
  6. 视图路径必须**按内容**比较（不许指针比较，也不许 strstr 子串包含）；
  7. gen26.5 M2 Step8：AI 交互词落库的三处接线——miss 路径与 AI chip 路径各调一次
     vocab_log_ai_queue()（恰好两次，且都在 push_sink_ai_tap_word_ctx() 之后），
     纯词典命中路径（dict_popup_open 到 found 判定之间）**不得**出现 queue；
  8. gen26.5 M2 Step8：app_loop 里 take_ai_def 成功后先 set_def 再 def_ready
     （落库与弹卡解耦），flush 落在 500ms 网络轮询块内；app_main 在
     app_settings_init() 之后调 vocab_log_autosave_init()；.c/.h 双冻结声明
     同步（rp_vocab / autosave / 5s 攒批关键词对表）。

只做静态检查，不编译。

The vocabulary book is wired across files, and comment-only sync is too fragile: if
app_book.c forgets a vocab_log_capture() call, or the chip tap never reaches
dict_popup_tap(), the build still succeeds and the device merely "does nothing" — the
user only finds out later. This test pins the wiring:
  1. start_dict_lookup() must capture context after unlock_draw(), not while holding
     the draw lock;
  2. the enter/exit/media-lost hooks must call warm() / invalidate() so the index
     never dangles;
  3. a dictionary-panel tap must first go to dict_popup_tap(), and a chip hit must
     keep the card open;
  4. the popup must actually draw the chip and the header width must yield to it;
  5. toolbar item 6 must really open the view, and a removal inside the view must
     re-render and re-open it;
  6. the view path must be matched by contents, never by pointer or substring;
  7. gen26.5 M2 Step8: the three AI-logging wirings - the miss path and the AI-chip
     path each call vocab_log_ai_queue() exactly once (twice in total, each after
     push_sink_ai_tap_word_ctx()), and the pure dictionary-hit path (from
     dict_popup_open to the found decision) must NOT queue;
  8. gen26.5 M2 Step8: in app_loop, set_def runs between take_ai_def and def_ready
     (logging decoupled from the card), flush lands inside the 500ms network poll
     block; app_main calls vocab_log_autosave_init() after app_settings_init(); the
     .c/.h frozen declarations stay in sync (rp_vocab / autosave / 5s batching).

Static only, nothing is compiled.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BOOK = ROOT / 'main' / 'apps' / 'app_book.c'
POPUP = ROOT / 'main' / 'apps' / 'dict_popup.c'
VOCAB = ROOT / 'main' / 'dict' / 'vocab_log.c'
VOCAB_H = ROOT / 'main' / 'dict' / 'vocab_log.h'
LOOP = ROOT / 'main' / 'app' / 'app_loop.c'
MAIN = ROOT / 'main' / 'app_main.c'


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
    for path in (BOOK, POPUP):
        if not path.exists():
            print(f'FAIL: missing {path}')
            return 1
    book = BOOK.read_text(encoding='utf-8')
    popup = POPUP.read_text(encoding='utf-8')
    failures = []

    # 1. 抓取上下文必须在解锁之后。/ Capture must happen after the draw lock is released.
    capture = function_body(book, 'static app_redraw_t start_dict_lookup(')
    if not capture:
        failures.append('start_dict_lookup(): not found')
    else:
        if 'vocab_log_capture(' not in capture:
            failures.append('start_dict_lookup(): does not call vocab_log_capture()')
        lock_at, cap_at = capture.find('unlock_draw()'), capture.find('vocab_log_capture(')
        if lock_at < 0:
            failures.append('start_dict_lookup(): never calls unlock_draw()')
        elif cap_at < lock_at:
            failures.append('start_dict_lookup(): captures before unlock_draw() '
                            '(would read s_text under the draw lock)')

    # 2. 生命周期钩子。/ Lifecycle hooks.
    hooks = {
        'static void on_enter(': 'vocab_log_warm()',
        'static void book_on_exit(': 'vocab_log_invalidate()',
        'static void book_on_media_lost(': 'vocab_log_invalidate()',
    }
    for signature, call in hooks.items():
        body = function_body(book, signature)
        name = signature.split('(')[0].split()[-1]
        if not body:
            failures.append(f'{name}(): not found')
        elif call not in body:
            failures.append(f'{name}(): does not call {call}')

    # 3. 面板点击先过 chip。/ Panel taps go through the chip first.
    action = function_body(book, 'static app_redraw_t reader_panel_action(')
    if not action:
        failures.append('reader_panel_action(): not found')
    else:
        dict_branch = action.find('READER_PANEL_DICT')
        if dict_branch < 0:
            failures.append('reader_panel_action(): no READER_PANEL_DICT branch')
        else:
            # 分支窗口：从 READER_PANEL_DICT 到下一个面板判断（或函数尾）。
            # 曾经这里是 400 字符的魔法数——gen26.3 给 chip 守门加了一段注释，
            # 把 dict_popup_reset() 挤出窗口，find() 返回 -1，被误判成
            # "reset 先于 tap" 而假红。窗口只许随代码变大，检测只会更强。
            # Branch window: from READER_PANEL_DICT to the next panel test (or
            # the end of the function). This used to be a 400-char magic
            # number: a gen26.3 comment pushed dict_popup_reset() out of the
            # window, find() returned -1, and the guard false-flagged
            # reset-before-tap. The window may only grow with the code, which
            # can only strengthen the check.
            tail = re.search(r'\n\s*(?:else )?if \(s_reader_panel',
                             action[dict_branch + 10:])
            end = len(action) if not tail else dict_branch + 10 + tail.start()
            branch = action[dict_branch:end]
            if 'dict_popup_tap(' not in branch:
                failures.append('reader_panel_action(): the dictionary branch never '
                                'calls dict_popup_tap()')
            elif branch.find('dict_popup_tap(') > branch.find('dict_popup_reset()'):
                failures.append('reader_panel_action(): resets the popup before giving '
                                'the chip a chance to handle the tap')

    # 4. 上下文守卫：在虚拟书里长按，capture() 必须直接 return 并清空原句。
    # Context guard: a long press inside the virtual book must bail out and clear
    # the sentence buffer, or a row of the view itself becomes a "source sentence".
    if not VOCAB.exists():
        failures.append('vocab_log.c: missing')
    else:
        text = VOCAB.read_text(encoding='utf-8')
        body = function_body(text, 'void vocab_log_capture(')
        if not body:
            failures.append('vocab_log_capture(): not found')
        else:
            if 'vocab_log_is_view_path(' not in body:
                failures.append('vocab_log_capture(): does not consult vocab_log_is_view_path() '
                                '(a long press in the vocabulary view would capture its own row)')
            if 's_sentence[0] = 0' not in body:
                failures.append('vocab_log_capture(): does not clear s_sentence when guarding')
            guard_at = body.find('vocab_log_is_view_path(')
            if guard_at < 0 or body.find('s_book[0] = 0') < guard_at:
                failures.append('vocab_log_capture(): the guard must run before the book '
                                'context is overwritten')

        # 路径比较必须是**深比较**：`path == VOCAB_VIEW` 比的是地址，换个来源的路径
        # 就整个失效，而且编译期一声不吭。也不许退化成 strstr 之类的子串包含——
        # 误判会让 DICT 分支在真书里删词时把虚拟书顶上来。
        # Path matching must compare contents: `path == VOCAB_VIEW` compares addresses
        # and silently breaks; a substring test like strstr would match
        # "my_vocab_view.txt" and reopen the view over a real book.
        matcher = function_body(text, 'static bool name_is_view(')
        if not matcher:
            failures.append('vocab_log.c: name_is_view() not found')
        else:
            if re.search(r'[=!]=\s*(VOCAB_VIEW|view|path)\b', matcher):
                failures.append('name_is_view(): compares pointers instead of contents '
                                '(path == VOCAB_VIEW breaks for any other buffer)')
            # 注意：strcasecmp 里并没有 "strcmp" 这个连续子串，别写错。
            # Careful: "strcasecmp" does not contain the substring "strcmp".
            if not re.search(r'strcasecmp|strcmp', matcher):
                failures.append('name_is_view(): does not compare with strcmp/strcasecmp')
            if 'strstr' in matcher and not re.search(r'strcasecmp|strcmp', matcher):
                failures.append('name_is_view(): substring matching would match '
                                'my_vocab_view.txt too')

    # 5. 工具条：两张表必须等长，且有 _Static_assert 兜着。
    # Toolbar: both tables must be the same length and asserted.
    for needle, label in (
        ('#define BOOK_TOOL_COUNT 6', 'six tools'),
        ('k_tool_labels[BOOK_TOOL_COUNT]', 'a label table sized by the count'),
        ('k_tool_icons[BOOK_TOOL_COUNT]', 'an icon table sized by the count'),
        ('"生词本"', 'the vocabulary-book label'),
    ):
        if needle not in book:
            failures.append(f'app_book.c: missing {label} ({needle!r})')
    # 两条断言必须分别存在。只数 _Static_assert 的出现次数会被上游自己的断言
    # （以及注释里提到的那几次）蒙混过关：删掉一条后总数依然够。
    # Both assertions must exist individually. Counting _Static_assert occurrences
    # lets upstream's own asserts - and the ones merely mentioned in comments -
    # mask a missing one.
    if '"every reader tool needs a label"' not in book or \
       '"every reader tool needs an icon"' not in book:
        failures.append('app_book.c: the toolbar tables lost their _Static_assert '
                        '(a missing entry would silently shift every tap)')
    # 生词本那一项要真的打开视图，而不是落到别的分支上
    # The vocabulary entry must open the view, not fall through to another branch.
    tools = function_body(book, 'static app_redraw_t reader_panel_action(')
    if 'open_vocab_view(ctx, false)' not in tools:
        failures.append('reader_panel_action(): the toolbar never opens the vocabulary view')
    # 在视图里删掉一条后要重开视图 / a removal inside the view must re-open it
    if 'DICT_POPUP_TAP_REMOVED' not in tools or 'open_vocab_view(ctx, true)' not in tools:
        failures.append('reader_panel_action(): removing an entry in the view does not '
                        're-render and re-open it')

    # 6. 弹层真的画 chip，且头部让位。/ The popup draws the chip and the header yields.
    for needle, label in (
        ('dict_popup_save_rect', 'chip geometry'),
        ('ui_draw_choice_round_rect(', 'filled chip when saved'),
        ('已存', 'saved label'),
    ):
        if needle not in popup:
            failures.append(f'dict_popup.c: missing {label} ({needle!r})')
    if 'inner_w - DICT_POPUP_SAVE_W' not in popup:
        failures.append('dict_popup.c: the header width does not reserve room for the chip')

    # 7. gen26.5 M2 Step8：AI 交互词落库接线。/ AI-word logging wiring.
    #    判据一：queue 恰好两次（miss + AI chip），且每次都在 tap_word_ctx 之后。
    #    Criterion 1: exactly two queue calls (miss + AI chip), each after a
    #    push_sink_ai_tap_word_ctx().
    #    判据二：纯词典命中路径不落——dict_popup_open() 到 found 判定之间没有 queue。
    #    Criterion 2: the pure dictionary-hit path never queues - no queue call
    #    between dict_popup_open() and the found decision.
    lookup_body = function_body(book, 'static app_redraw_t start_dict_lookup(')
    panel_body = function_body(book, 'static app_redraw_t reader_panel_action(')
    if not lookup_body or not panel_body:
        failures.append('start_dict_lookup()/reader_panel_action(): not found '
                        '(cannot check the AI logging wiring)')
    else:
        if book.count('vocab_log_ai_queue(') != 2:
            failures.append('app_book.c: expected exactly 2 vocab_log_ai_queue() calls '
                            '(miss path + AI chip), got %d' % book.count('vocab_log_ai_queue('))
        for name, body in (('start_dict_lookup', lookup_body),
                           ('reader_panel_action', panel_body)):
            tap_at = body.find('push_sink_ai_tap_word_ctx(')
            queue_at = body.find('vocab_log_ai_queue(')
            if tap_at < 0 or queue_at < 0:
                failures.append('%s(): the AI path is missing tap_word_ctx or queue' % name)
            elif queue_at < tap_at:
                failures.append('%s(): queues the word before the query goes out' % name)
        # 纯词典命中不落：found 判定窗口。/ The pure dictionary-hit path never queues.
        # 窗口右界是 miss 分支本身——那里的 queue 是**正确**的（miss 词要落），
        # 所以判据只看 dict_popup_open() 到 miss 分支之间。
        # The window's right edge is the miss branch itself - a queue in there is
        # CORRECT (a miss word must log), so the check only covers the span from
        # dict_popup_open() to the miss branch.
        open_at = lookup_body.find('dict_popup_open(')
        found_at = lookup_body.find('if (hit && english && !found)')
        if open_at < 0 or found_at < 0:
            failures.append('start_dict_lookup(): cannot locate the dict-hit window '
                            '(dict_popup_open / the miss branch)')
        elif 'vocab_log_ai_queue(' in lookup_body[open_at:found_at]:
            failures.append('start_dict_lookup(): queues a word on a pure dictionary hit '
                            '(adjudicated: only AI-interacted words log)')

    # 8. gen26.5 M2 Step8：app_loop 的 flush 驱动与解耦顺序、app_main 的开关读取、
    #    .c/.h 双冻结声明同步。/ The flush driver and decoupling order in app_loop,
    #    the switch read in app_main, and the .c/.h frozen declarations in sync.
    if not LOOP.exists() or not MAIN.exists():
        failures.append('app_loop.c / app_main.c: missing')
    else:
        loop = LOOP.read_text(encoding='utf-8')
        take_at = loop.find('push_sink_take_ai_def(')
        set_at = loop.find('vocab_log_ai_set_def(')
        ready_at = loop.find('app_book_ai_def_ready(')
        flush_at = loop.find('vocab_log_ai_flush(')
        poll_at = loop.find('read_pico_transfer_service_poll()')
        if take_at < 0:
            failures.append('app_loop.c: push_sink_take_ai_def() not found')
        else:
            if set_at < 0:
                failures.append('app_loop.c: does not call vocab_log_ai_set_def() '
                                '(a returned definition never feeds the batch)')
            elif not take_at < set_at:
                failures.append('app_loop.c: set_def before take_ai_def (nothing to feed)')
            elif ready_at >= 0 and set_at > ready_at:
                failures.append('app_loop.c: set_def after def_ready - logging stays '
                                'coupled to the card and gate A would drop it')
            if flush_at < 0:
                failures.append('app_loop.c: does not call vocab_log_ai_flush() '
                                '(the batch would never land)')
            elif poll_at >= 0 and flush_at < poll_at:
                failures.append('app_loop.c: flush sits outside the network poll block '
                                '(it must ride the <=500ms tick)')
        main_text = MAIN.read_text(encoding='utf-8')
        settings_at = main_text.find('app_settings_init();')
        auto_at = main_text.find('vocab_log_autosave_init();')
        if settings_at < 0 or auto_at < 0:
            failures.append('app_main.c: missing app_settings_init() or '
                            'vocab_log_autosave_init()')
        elif auto_at < settings_at:
            failures.append('app_main.c: reads the autosave switch before NVS is up')

    # 冻结声明同步：.c/.h 顶部都登记同一份契约（立项令门禁项）。
    # Frozen declarations in sync: both the .c and .h headers register the same
    # contract (a gate from the charter).
    for path, label in ((VOCAB, 'vocab_log.c'), (VOCAB_H, 'vocab_log.h')):
        if not path.exists():
            failures.append(f'{label}: missing')
            continue
        head = path.read_text(encoding='utf-8')[:2500]
        for needle, what in (('rp_vocab', 'the NVS namespace'),
                             ('autosave', 'the NVS key'),
                             ('5s', 'the batch delay'),
                             ('攒批', 'the batching itself')):
            if needle not in head:
                failures.append(f'{label}: the frozen declaration no longer mentions '
                                f'{what} ({needle!r})')

    if failures:
        for line in failures:
            print('  FAIL ' + line)
        print('vocab_wiring_host_test: FAIL')
        return 1
    print('PASS: vocabulary book wiring intact (capture after unlock, 3 lifecycle hooks, '
          'view guard matched by contents, 6 asserted tools, chip tap + re-open, chip drawn)')
    return 0

if __name__ == '__main__':
    sys.exit(main())
