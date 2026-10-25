/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：生词本。长按查词后由用户点卡片上的 chip 手动保存，写 TF 卡 TSV，可直接导入 Anki。
 * English: Vocabulary builder. After a long-press lookup the reader taps a chip on the
 * card to save; entries land on the TF card as TSV, ready for Anki.
 *
 * gen26.5 契约修订（M2 Step8，用户裁定）：在 chip 手动保存之外新增
 * **AI 交互词自动落库**——「与 AI 交互过的词」即落：① 词典 miss 词 0x17 发出即落；
 * ② 词典命中后用户主动点「AI」chip 即落；③ **纯词典命中（没要 AI）不落**（避免噪音）。
 * 自动落库受 NVS 开关控制（`rp_vocab` namespace、`autosave` u8、**缺省开**），
 * 且走**攒批写**：词只进内存批，5s 后合批单次写 TF 卡（防 Flash 磨损）。
 * gen26.5 contract revision (M2 Step8, user-adjudicated): on top of the manual chip
 * save, words that interacted with AI are logged automatically - a dictionary-miss
 * word logs the moment 0x17 goes out; a dict-hit word logs when the user taps the
 * "AI" chip; a pure dictionary hit (AI never asked) never logs (noise). Automatic
 * logging honors an NVS switch (namespace "rp_vocab", key "autosave", **on by
 * default**) and is **batched**: words wait in memory and land in one write 5s
 * later (guards the card against wear).
 *
 * 保留的冻结语义：写失败必须静默，绝不得阻塞 UI。
 * Kept frozen semantics: a failed write stays silent and never blocks the UI.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VOCAB_WORD_MAX 64

/// AI 交互词入批（不写卡）。与 AI 交互过 = 词典 miss 时 0x17 发出、或词典命中卡上
/// 主动点「AI」chip——两者都只入内存批，由 vocab_log_ai_flush() 5s 后合批落盘。
/// autosave 开关关着时直接丢弃（静默）。原句取当前抓取上下文（点词时已抓好）。
/// Queue an AI-interacted word (no card write). AI interaction = a dictionary miss
/// whose 0x17 went out, or a deliberate "AI" chip tap on a dict-hit card. Both only
/// enter the memory batch; vocab_log_ai_flush() lands it 5s later in one write.
/// Discarded silently when the autosave switch is off. The sentence comes from the
/// context captured at tap time.
void vocab_log_ai_queue(const char* word);
/// 释义回流补进批（0x07 到达时调）。批里没有这个词就忽略——它可能已经落盘了，
/// 那时释义走预取缓存，下次手动保存会带上。/ Feed a returning definition (0x07)
/// into the batch. A word not in the batch is ignored: it may already be on the
/// card, and the definition stays in the prefetch cache for the next manual save.
void vocab_log_ai_set_def(const char* word, const char* def);
/// 合批落盘。由主循环 tick 驱动（≤500ms 一遍）：首次入队满 5s 才写，一次写全部
/// 积压；写失败保留批次等下次重试（防磨损的合批语义不变，也不丢词）。
/// Flush the batch. Driven by a main-loop tick (every <=500ms): writes once 5s
/// after the first queued word, everything pending in one go; a failure keeps the
/// batch for the next attempt (batching semantics intact, no word lost).
void vocab_log_ai_flush(void);
/// 自动落库开关是否开着（缺省开）。读 NVS `rp_vocab`/`autosave`：读不到或读失败
/// 都按缺省开，只有明确读到 0 才是关。/ Whether automatic logging is on (default
/// on). NVS "rp_vocab"/"autosave": a missing or failed read defaults to on, and
/// only an explicit 0 turns it off.
bool vocab_log_autosave_enabled(void);
/// 读一次 NVS 开关（开机时调）。重复调用会重读。/ Read the NVS switch once (at
/// boot). Calling it again re-reads.
void vocab_log_autosave_init(void);

/// 抓取长按上下文（书名路径、章节原文、词区间）。必须在原文仍有效时调用，
/// 且不要在绘制锁内做——扫描是有界的字符串操作，不该拉长持锁时间。
/// Capture the long-press context while the chapter text is still valid. Call it
/// outside the draw lock: the scan is bounded string work and must not extend
/// the time the lock is held.
void vocab_log_capture(const char* book_path, const char* text, size_t start, size_t end);
/// 词是否已在生词本。索引未就绪时返回 false（chip 显示为未保存），**不读卡**，
/// 所以它永远不会拖慢查词路径。
/// Whether the word is saved. Returns false when the index is unavailable and
/// never touches the card, so it can never slow the lookup path down.
bool vocab_log_contains(const char* word);
/// 追加一条。索引未就绪时改为逐行扫文件去重（只在用户点击时发生，不在热路径上）。
/// Append one entry. Falls back to a file scan when the index is unavailable;
/// that only ever happens on an explicit tap, never on a hot path.
bool vocab_log_save(const char* word, const char* definition);
/// 移除一条（chip 再点一次）。走临时文件重写，中途失败不留半成品。
/// 被删的那一行会被记住：紧接着再点一次「保存」就**原样写回**，所以误删可撤销。
/// Remove one entry through a temp-file rewrite; a failure leaves no half file.
/// The removed row is remembered so an immediate second tap writes it back
/// verbatim — an accidental removal is undoable.
bool vocab_log_remove(const char* word);
/// 把 vocab.txt 渲染成一本排版好的 TXT 虚拟书，供现成阅读器打开。
/// 返回是否成功；*out_count 是词条数（0 表示生词本是空的）。
/// Render vocab.txt into a laid-out TXT virtual book the existing reader can open.
/// Returns success; *out_count is the entry count (0 means the book is empty).
bool vocab_log_render(size_t* out_count);
/// 虚拟书路径，用来打开它。/ The virtual book path, used to open it.
const char* vocab_log_view_path(void);
/// 是不是虚拟书（只比文件名，所以拷到别处再读也认得出）。
/// capture() 用它拒绝抓取上下文，免得把视图自己的一行当成原句写回去。
/// Whether this is the virtual book (matches on the file name, so a copy read from
/// elsewhere is still recognized). capture() uses it to refuse the context, so a
/// row of the view itself can never become a "source sentence".
bool vocab_log_is_view_path(const char* path);
/// 预建去重索引。与 dict_warm() 同处调用，把读卡开销挪出查词路径。
/// Build the index up front; call next to dict_warm() to keep the card read off
/// the lookup path.
void vocab_log_warm(void);
/// 拔卡/退出时释放索引。与 dict_close() 同处调用。**同时丢弃未落盘的 AI 批**——
/// 书上下文已失效，批内词的原句/书名不再可靠；损失 ≤5s 的批，静默（契约保留）。
/// Drop the index when the card goes away; call next to dict_close(). **Also drops
/// the unflushed AI batch**: with the book context gone, a batched word's sentence
/// and book name are no longer trustworthy; at most 5s of batch is lost, silently
/// (the contract is kept).
void vocab_log_invalidate(void);

#ifdef __cplusplus
}
#endif
