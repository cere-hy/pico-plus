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
     同步（rp_vocab / autosave / 5s 攒批关键词对表）；
  9. gen26.5 M2 Step10：TTS 听书三个新声明点的头同步 + CMake 登记——
     app_book.c 点到的 push_sink_*/tts_(switch|page)_* 必须落在各自头文件里，
     push_sink.c 点到的 push_tts_* 必须落在 push_tts.h 里，两个新 .c 必须进
     各自 CMake 清单（ai_detail P7 同型网：宿测看不见的漏声明只有固件
     fullclean 抓，那是最后一道也是最贵的一道网）。

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
     .c/.h frozen declarations stay in sync (rp_vocab / autosave / 5s batching);
  9. gen26.5 M2 Step10: header sync plus CMake registration for the three new TTS
     declaration points - every push_sink_*/tts_(switch|page)_* symbol app_book.c
     uses must live in its module header, every push_tts_* symbol push_sink.c uses
     must live in push_tts.h, and both new .c files must sit in their CMake source
     lists (the same net as ai_detail P7: a missing declaration the host builds
     cannot see only surfaces in a firmware fullclean - the last and priciest net).

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
SINK_C = ROOT / 'components' / 'push_sink' / 'src' / 'push_sink.c'
SINK_H = ROOT / 'components' / 'push_sink' / 'include' / 'push_sink.h'
PLAN_H = ROOT / 'components' / 'push_sink' / 'include' / 'push_tts.h'
TTS_PAGE_H = ROOT / 'main' / 'tts' / 'tts_page.h'
MAIN_CMAKE = ROOT / 'main' / 'CMakeLists.txt'
SINK_CMAKE = ROOT / 'components' / 'push_sink' / 'CMakeLists.txt'


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
        ('#define BOOK_TOOL_COUNT 7', 'seven tools'),
        ('k_tool_labels[BOOK_TOOL_COUNT]', 'a label table sized by the count'),
        ('k_tool_icons[BOOK_TOOL_COUNT]', 'an icon table sized by the count'),
        ('"生词本"', 'the vocabulary-book label'),
        ('"听书"', 'the TTS read-aloud label'),
        ('UI_ICON_VOLUME_2', 'the TTS read-aloud icon'),
        ('READER_PANEL_TTS', 'the TTS reader panel'),
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
    # gen26.5 Step10：听书那一项要真的打开 TTS 面板，且面板里有开关卡接线
    # （NVS rp_tts）。工具计数从 6 到 7 是本次唯一一次扩容，两条表同长由
    # _Static_assert 兜着；这里盯「第 7 项点下去有去处」。
    # gen26.5 Step10: the TTS entry must open the TTS panel, and the panel must
    # wire the switch card (NVS rp_tts). 6 -> 7 is the only toolbar expansion;
    # the _Static_asserts guard equal table lengths, this guards "the seventh
    # tap goes somewhere".
    if 's_reader_panel = READER_PANEL_TTS' not in tools:
        failures.append('reader_panel_action(): the toolbar never opens the TTS panel')
    if 'tts_switch_set(' not in tools:
        failures.append('reader_panel_action(): the TTS panel never sets the switch')
    if 'tts_switch_on()' not in book:
        failures.append('app_book.c: the TTS follow path never reads the switch')
    if 'push_sink_tts_send_page(' not in book:
        failures.append('app_book.c: the TTS follow path never pushes page text')
    # gen26.5 Step10-5：跟随轮询的两条安全线也要有牙——① 页变哨兵去重
    # （删了它每 tick 重发同一页，手机上把一页读成复读）；② link_up 门
    # （删了它没连接也调 notify，白耗重投配额）。变异脚本 run_tts_mutations.sh
    # 逐条植入验证这两行使它 FAIL。**必须在 tts_follow_poll 函数体内查**：
    # push_sink_link_up() 在 draw_tts 状态卡里也出现，整文件查会被别的调用点
    # 蒙混过关（M8 变异第一版就是这么存活下来的）。
    # gen26.5 Step10-5: the follow poll's two safety lines need teeth too - the
    # page-change sentinel (without it every tick re-sends the same page and the
    # phone reads it on repeat) and the link_up gate (without it a dead link
    # still calls notify, burning retry quota for nothing). The mutation script
    # run_tts_mutations.sh plants each and proves this check FAILs. The lookup
    # MUST be scoped to tts_follow_poll's body: push_sink_link_up() also appears
    # in draw_tts's status card, so a whole-file search lets the other call site
    # mask the deletion (that is exactly how mutation M8 survived revision 1).
    poll = function_body(book, 'static void tts_follow_poll(void)')
    if not poll:
        failures.append('app_book.c: tts_follow_poll() not found')
    else:
        if 's_chapter == s_tts_last_chapter && s_page == s_tts_last_page' not in poll:
            failures.append('tts_follow_poll(): lost its page-change sentinel '
                            '(every tick would re-send the current page)')
        if 'push_sink_link_up()' not in poll:
            failures.append('tts_follow_poll(): lost its link_up read '
                            '(a dead link would still call notify)')
        if 'if (!up) return;' not in poll:
            failures.append('tts_follow_poll(): lost its link_up gate '
                            '(a dead link would still call notify)')
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
    if not re.search(r'reserved\s*=.*DICT_POPUP_SAVE_W', popup):
        failures.append('dict_popup.c: the header width does not reserve room for the chip')

    # 7. 26.9.5：AI 交互词落库接线（**语义已随入口卡改写**）。
    #    26.9.5 起点：点词不再自动发 0x17 —— 本地无释义时先给入口卡，用户点
    #    「AI 释义 ›」才转 pending 并发送。故 tap_word_ctx 与 queue 全部搬到
    #    TAP_AI 分支（恰好各一处），点词路径两者皆无。
    #    26.9.5 之前的判据是「两处 queue（miss + AI chip）」，本次起作废：那不是
    #    数量微调，而是「谁该发起查询」的语义变化——用户在通知里会看到卡片先出现、
    #    再点才查，所以落库时机必须同步后移，否则记的是「点过但没查」的词。
    #    gen26.5 M2 Step8 的原意（「与 AI 交互过的词才落」）在新链路上更强了：
    #    交互的唯一入口就是那颗 chip。
    # 7. 26.9.5: AI-word logging wiring (semantics rewritten with the entry card).
    # From 26.9.5 a tap no longer fires 0x17 by itself - with no local definition an
    # entry card opens first and the user taps "AI 释义 ›" to switch to pending and
    # send. So tap_word_ctx and queue both live in the TAP_AI branch (exactly one
    # each) and the tap path has neither. The pre-26.9.5 criterion ("two queue calls:
    # miss + AI chip") is void - that was not a count tweak but a change in WHO
    # starts the query, and the logging moment must move with it or it records words
    # that were tapped but never looked up. gen26.5's intent ("only AI-interacted
    # words log") is in fact stronger here: the chip is the only interaction.
    lookup_body = function_body(book, 'static app_redraw_t start_dict_lookup(')
    panel_body = function_body(book, 'static app_redraw_t reader_panel_action(')
    if not lookup_body or not panel_body:
        failures.append('start_dict_lookup()/reader_panel_action(): not found '
                        '(cannot check the AI logging wiring)')
    else:
        if book.count('vocab_log_ai_queue(') != 1:
            failures.append('app_book.c: expected exactly 1 vocab_log_ai_queue() call '
                            '(the AI-chip path), got %d' % book.count('vocab_log_ai_queue('))
        tap_at = panel_body.find('push_sink_ai_tap_word_ctx(')
        queue_at = panel_body.find('vocab_log_ai_queue(')
        if tap_at < 0 or queue_at < 0:
            failures.append('reader_panel_action(): the AI-chip path is missing '
                            'tap_word_ctx or queue')
        elif queue_at < tap_at:
            failures.append('reader_panel_action(): queues the word before the query goes out')
        # 点词路径必须两者皆无：有任何一个 = 26.9.5 的「用户确认才查」被改回去了。
        # The tap path must have neither: either one means the 26.9.5 "the user
        # confirms before we query" rule was reverted.
        for needle, what in (('push_sink_ai_tap_word_ctx(', 'fires 0x17 on tap'),
                             ('vocab_log_ai_queue(', 'queues the word on tap')):
            if needle in lookup_body:
                failures.append('start_dict_lookup(): still %s (26.9.5 moved both '
                                'to the AI-chip path)' % what)

    # 7c. 26.9.5 入口卡接线：拆掉 `english` 总闸后，三道接线缺一即退回「中文静默
    #     不响应」——这正是本版要根治的缺陷，所以每条都要有牙。
    #     ① app_book 真的开入口卡；② show_entry 置 s_entry 且**不**置 s_pending
    #     （否则没发请求却被 0x07 认领）；③ reset 清 s_entry（生命周期唯一出口）；
    #     ④ 入口卡不画保存 chip 也不响应（有释义才可存）；⑤ 顺序：先 reset 再置位
    #     （反了会留上一张卡的残影，与 gen26.4 别名 bug 同族）。
    # 7c. 26.9.5 entry-card wiring: with the `english` master gate gone, a missing
    # link puts Chinese back into silent non-response - the very defect this version
    # exists to kill - so every link needs teeth: (1) app_book really opens the
    # entry card; (2) show_entry sets s_entry and NOT s_pending (or an unsent card
    # gets claimed by 0x07); (3) reset clears s_entry (the single lifecycle exit);
    # (4) no save chip drawn or hit (only a definition may be saved); (5) order -
    # reset first, then set the flags (reversed leaves the previous card's ghost,
    # the same family as the gen26.4 aliasing bug).
    if 'dict_popup_show_entry(' not in lookup_body:
        failures.append('start_dict_lookup(): never calls dict_popup_show_entry() - '
                        'Chinese taps would go silent again (the 26.9.5 defect returns)')
    if 'if (hit && !found' not in lookup_body:
        failures.append('start_dict_lookup(): the entry branch (`hit && !found`) is gone')
    entry_fn = function_body(popup, 'bool dict_popup_show_entry(')
    if not entry_fn:
        failures.append('dict_popup.c: dict_popup_show_entry() not found')
    else:
        if 's_entry = true' not in entry_fn:
            failures.append('dict_popup_show_entry(): never sets s_entry')
        if 's_pending = false' not in entry_fn:
            failures.append('dict_popup_show_entry(): does not pin s_pending = false '
                            '(an unsent card would be claimed by 0x07)')
        if entry_fn.find('dict_popup_reset()') > entry_fn.find('s_entry = true'):
            failures.append('dict_popup_show_entry(): sets s_entry before reset '
                            '(ghost flags from the previous card survive)')
    reset_fn = function_body(popup, 'void dict_popup_reset(')
    if not reset_fn or 's_entry = false' not in reset_fn:
        failures.append('dict_popup_reset(): does not clear s_entry (the entry state '
                        'would leak into the next card)')
    if 'if (s_entry) return DICT_POPUP_TAP_MISSED;' not in popup:
        failures.append('dict_popup.c: an entry card still answers the save chip '
                        '(a definition-less card must not save an empty shell)')
    if 'const bool save_chip = !s_entry;' not in popup:
        failures.append('dict_popup.c: an entry card still draws the save chip')
    # 7d. 开关顺序：reset 会清 s_ai_on / s_scene_on（开卡第一步就是 reset），所以
    #     set_*_enabled 必须在**开卡之后**。写反了 AI 与情景 chip 一起消失，而宿主
    #     用例照产品顺序写，抓不到——这正是「桩与产品调用一一对表」要补的一格。
    # 7d. Switch order: reset clears s_ai_on / s_scene_on (opening resets first), so
    # set_*_enabled must come AFTER the card opens. Reversed, both the AI and the
    # scene chip vanish, and the host tests cannot see it (they follow the product
    # order) - exactly the gap the "stub vs call-site table" is meant to cover.
    set_at = lookup_body.find('dict_popup_set_ai_enabled(')
    show_at = lookup_body.find('dict_popup_show_entry(')
    if set_at < 0 or show_at < 0:
        failures.append('start_dict_lookup(): cannot locate the entry card or the AI '
                        'switch call')
    elif set_at < show_at:
        failures.append('start_dict_lookup(): sets the AI switch before opening the card '
                        '(reset clears s_ai_on, so the AI chip would disappear)')

    # 7b. gen26.6 裁定：补查机制整体移除——设备端断连 miss 词不得再入搭车队列。
    #     push_sink.c 若重新出现 ai_raloc_enqueue 调用，说明补查回潮（「错过的词
    #     让它过去」是裁定语义，不是实现细节）。注意 ai_ridealong.c 模块本身仍
    #     保留（纯逻辑 + 宿主测试），故只查 push_sink.c 这个调用点。
    #     gen26.6 ruling: the re-query mechanism is gone - an offline miss must
    #     never re-enter the ride-along queue. If push_sink.c calls
    #     ai_raloc_enqueue again, the retired mechanism has crept back ("a missed
    #     word is allowed to pass" is adjudicated semantics, not an impl detail).
    #     The ai_ridealong.c module itself stays (pure logic + host tests), so
    #     only push_sink.c's call site is checked.
    sink_text = SINK_C.read_text(encoding='utf-8') if SINK_C.exists() else ''
    if not sink_text:
        failures.append('push_sink.c: missing')
    else:
        if 'ai_raloc_enqueue(' in sink_text:
            failures.append('push_sink.c: an offline miss is queued again '
                            '(ai_raloc_enqueue found - gen26.6 retired the re-query)')
        # gen26.6.2：离线缓存命中——预取缓存是设备自己的记忆，命中即本地出卡，
        # **与连接态无关**。故查缓存必须排在 live 门之前，顺序颠倒即缺陷。
        # gen26.6.2: the offline cache hit - the prefetch cache is the device's own
        # memory, surfacing locally regardless of link state. So the cache lookup must
        # precede the live gate; flipping the order is the defect.
        tap = function_body(sink_text, 'void push_sink_ai_tap_word_ctx(')
        if not tap:
            failures.append('push_sink.c: push_sink_ai_tap_word_ctx() not found')
        else:
            cache_at = tap.find('ai_cache_find(')
            live_at = tap.find('s_status_subscribed')
            if cache_at < 0 or live_at < 0:
                failures.append('push_sink_ai_tap_word_ctx(): missing the cache lookup '
                                'or the live gate')
            elif cache_at > live_at:
                failures.append('push_sink_ai_tap_word_ctx(): the prefetch-cache lookup '
                                'sits after the live gate (offline cache hits would die)')

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

    # 9. gen26.5 M2 Step10：TTS 三个新声明点的头同步 + CMake 登记（P7 同型网）。
    #    ai_detail_wiring_host_test.py 的 P7 段立过规矩：宿测直接 #include .c
    #    （tts_page 独册就是这么编的，静态定义全可见）或压根不编调用方（本守门
    #    只读文本）时，头文件漏声明只有固件 fullclean 抓得到——那是最后一道、
    #    也是最贵的一道网。这里照同式结一张静态网：调用方点到的每个模块符号都
    #    必须落在模块头的声明里；两个新 .c 必须进各自 CMake 清单（漏登记 =
    #    固件链接期 undefined，宿主测试一声不吭）。
    #    gen26.5 M2 Step10: header sync plus CMake registration for the three new
    #    TTS declaration points (the same net as P7). The tts_page booklet
    #    #includes the .c directly and this guard only reads text, so a missing
    #    header declaration would only surface in a firmware fullclean - the last
    #    and priciest net. Same pattern here: every module symbol the callers use
    #    must be declared in the module header, and both new .c files must sit in
    #    their CMake source lists (a missing entry = firmware link-time undefined,
    #    which the host tests never notice).
    tts_h = TTS_PAGE_H.read_text(encoding='utf-8') if TTS_PAGE_H.exists() else ''
    sink_h_text = SINK_H.read_text(encoding='utf-8') if SINK_H.exists() else ''
    plan_h_text = PLAN_H.read_text(encoding='utf-8') if PLAN_H.exists() else ''
    sink_c = SINK_C.read_text(encoding='utf-8') if SINK_C.exists() else ''
    main_c = MAIN.read_text(encoding='utf-8') if MAIN.exists() else ''
    for path, label in ((TTS_PAGE_H, 'tts_page.h'), (SINK_H, 'push_sink.h'),
                        (PLAN_H, 'push_tts.h'), (SINK_C, 'push_sink.c')):
        if not path.exists():
            failures.append(f'{label}: missing')
    # tts_page_range 是 app_book.c 自己的静态 helper（算当前视觉页的字节区间），
    # 不是 tts_page 模块 API，不进头文件——同 ai_detail P7 的
    # ai_reader_close_full，显式排除。
    # tts_page_range is app_book.c's own static helper (the current visual
    # page's byte range), not a tts_page module API - exclude it explicitly,
    # exactly like ai_detail P7's ai_reader_close_full.
    local_helpers = {'tts_page_range'}
    step10_pairs = (
        # 调用方文本, 模块头文本, 符号模式, 头文件名
        # caller text, module header text, symbol pattern, header name
        (book, sink_h_text, r'\bpush_sink_[a-z_0-9]+\(', 'push_sink.h'),
        (book + '\n' + main_c, tts_h, r'\btts_(?:switch|page)_[a-z_]+\(',
         'tts_page.h'),
        (sink_c, plan_h_text, r'\bpush_tts_[a-z_]+\(', 'push_tts.h'),
    )
    for caller_text, header_text, pattern, header_name in step10_pairs:
        if not caller_text or not header_text:
            continue  # 缺文件已在上面记过 / missing files already reported
        used = sorted({m[:-1] for m in re.findall(pattern, caller_text)}
                      - local_helpers)
        for name in used:  # used 已去括号 / names already have the paren stripped
            if not re.search(rf'\b{re.escape(name)}\s*\(', header_text):
                failures.append(f'{header_name}: the caller uses {name}() but the '
                                'header never declares it (a firmware fullclean '
                                'would fail on an implicit declaration)')
    for cmake, needle, label in ((MAIN_CMAKE, '"tts/tts_page.c"', 'main/CMakeLists.txt'),
                                 (SINK_CMAKE, '"src/push_tts.c"',
                                  'components/push_sink/CMakeLists.txt')):
        if not cmake.exists():
            failures.append(f'{label}: missing')
        elif needle not in cmake.read_text(encoding='utf-8'):
            failures.append(f'{label}: {needle} is not in the source list '
                            '(the firmware link would fail on undefined symbols)')

    if failures:
        for line in failures:
            print('  FAIL ' + line)
        print('vocab_wiring_host_test: FAIL')
        return 1
    print('PASS: vocabulary book wiring intact (capture after unlock, 3 lifecycle hooks, '
          'view guard matched by contents, 7 asserted tools, chip tap + re-open, chip '
          'drawn, TTS panel wiring, Step10 header sync + CMake registration)')
    return 0

if __name__ == '__main__':
    sys.exit(main())
