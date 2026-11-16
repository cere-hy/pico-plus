#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""26.9.10 生词本 G1-G4 静态牙（与宿主测试互补）。

G1 行为由 vocab_log_host_test 证明（AI 自动不写盘），此处钉住「queue 是空实现」；
G2 行为由宿主测试证明（中英手工落库），此处钉住「入口不再有 english 总闸」；
G3 只能静态钉：AI 落地 `s_saved = false`，不得再用 vocab_log_contains；
G4 的顺序修复静态钉：append_row 与 fclose 之间不得出现 index_add，且 fclose 成功后
必须补上延迟入索引的循环。
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
vl = (ROOT / 'main/dict/vocab_log.c').read_text()
ab = (ROOT / 'main/apps/app_book.c').read_text()
dp = (ROOT / 'main/apps/dict_popup.c').read_text()

fails = []


def body(text, sig):
    i = text.find(sig)
    if i < 0:
        return ''
    j = text.find('\n}', i)
    return text[i:j] if j > 0 else text[i:]


# ---- G1: vocab_log_ai_queue is a no-op ----
q = body(vl, 'void vocab_log_ai_queue(const char* word) {')
if '++s_batch_count' in q or 'batch_ensure()' in q:
    fails.append('G1: vocab_log_ai_queue still enqueues (++s_batch_count / batch_ensure())')
if '(void)word;' not in q:
    fails.append('G1: vocab_log_ai_queue is not the no-op form')

# ---- G2: entry english gate removed ----
if 'bool found = hit && dict_popup_open(word);' not in ab:
    fails.append('G2: entry line is not "found = hit && dict_popup_open(word)"')
if re.search(r'found\s*=\s*hit\s*&&\s*english\s*&&', ab):
    fails.append('G2: entry still gated by english')

# ---- G3: AI landing must set s_saved = false ----
if 'if (ok) s_saved = false;' not in dp:
    fails.append('G3: AI landing does not set s_saved = false')
tail = dp.rsplit('bool dict_popup_show_def', 1)[-1][:2500]
if 's_saved = vocab_log_contains(s_word)' in tail:
    fails.append('G3: AI landing still derives s_saved from vocab_log_contains')

# ---- G4: index_add deferred until after the fclose check ----
fl = body(vl, 'void vocab_log_ai_flush(void) {')
needle = 'append_row(f, lower, s_batch[i].def, s_batch[i].sentence);'
if needle not in fl or 'if (fclose(f) != 0)' not in fl:
    fails.append('G4: flush shape changed (append_row / fclose marker missing)')
else:
    after_append = fl.split(needle, 1)[1]
    pre_close = after_append.split('if (fclose(f) != 0)', 1)[0]
    post_close = after_append.split('if (fclose(f) != 0)', 1)[1]
    if 'index_add(' in pre_close:
        fails.append('G4: index_add appears between append_row and the fclose check')
    if 'index_add(added' not in post_close:
        fails.append('G4: deferred index_add(added...) loop missing after the fclose check')

for f in fails:
    print('FAIL:', f)
if not fails:
    print('26910 VOCAB G1-G4 PASS')
sys.exit(1 if fails else 0)
