/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * M3-B 情景请求待命态的实现。全部是纯函数：不读时钟、不碰 IO，因此宿主测试
 * 可以用假时间逐条走完四条路径（命中 / 词不配 / 页不配 / 超时）。
 * The M3-B pending-state implementation. All pure: no clock reads, no IO, so the
 * host test can walk all four paths (hit / word mismatch / page mismatch / expiry)
 * with a fake clock.
 */

#include "scene_req.h"

#include <string.h>

static void copy_word(char* dst, size_t cap, const char* src) {
    if (!dst || !cap) return;
    if (!src) { dst[0] = 0; return; }
    size_t n = 0;
    while (src[n] && n + 1 < cap) {
        dst[n] = src[n];
        ++n;
    }
    dst[n] = 0;
}

void scene_req_arm(scene_req_t* r, const char* word, uint16_t word_off, uint8_t word_len,
                   size_t page, int64_t deadline_ms) {
    if (!r) return;
    memset(r, 0, sizeof(*r));
    if (!word || !*word) return; // 没词无法关联：保持未置位 / nothing to correlate: stay disarmed
    copy_word(r->word, sizeof(r->word), word);
    r->word_off = word_off;
    r->word_len = word_len;
    r->page = page;
    r->deadline_ms = deadline_ms;
    r->armed = true;
}

void scene_req_arm_excerpt(scene_req_t* r, const char* word, uint16_t word_off,
                           uint8_t word_len, size_t page, int64_t deadline_ms) {
    // 先走同一条置位路径（词为空就是不置位），再打标记——两条置位路径只有一个
    // 差别，分开写两份 memset 迟早在字段上漂开。
    // Reuse the one arming path (an empty word stays disarmed) and only then set the
    // mark: two separate memsets would drift on fields sooner or later.
    scene_req_arm(r, word, word_off, word_len, page, deadline_ms);
    if (r && r->armed) r->excerpt = true;
}

void scene_req_disarm(scene_req_t* r) {
    if (!r) return;
    r->armed = false;
}

bool scene_req_expired(const scene_req_t* r, int64_t now_ms) {
    return r && r->armed && now_ms > r->deadline_ms;
}

bool scene_req_expire(scene_req_t* r, int64_t now_ms) {
    if (!scene_req_expired(r, now_ms)) return false;
    r->armed = false;
    return true;
}

bool scene_req_claim(scene_req_t* r, const char* word, size_t page, int64_t now_ms) {
    if (!r || !r->armed) return false;
    // 期限优先于匹配：早就过期的单子哪怕是同一个词也不认——用户已经走了。
    // Deadline before matching: an expired request is not claimed even for the same
    // word - the moment has passed.
    if (now_ms > r->deadline_ms) {
        r->armed = false;
        return false;
    }
    if (!word || strcmp(word, r->word) != 0) return false; // 不是我这单 / not mine
    if (page != r->page) {
        // 页不配＝用户在读别处：销号，别让这条答案再去落卡。
        // Page mismatch means the user is reading elsewhere: disarm, so this answer
        // cannot fall through and land a card either.
        r->armed = false;
        return false;
    }
    r->armed = false; // 成功即销号：一次机会 / one shot, disarmed on success
    return true;
}

/* ---------- 引用条取句（纯函数）/ Quote-sentence extraction (pure) ---------- */

// 解码一个 UTF-8 码位，返回字节数（非法前导返回 1 并给出 U+FFFD）。
// Decode one code point; a stray lead byte yields U+FFFD and a 1-byte step.
static int sr_utf8_step(const char* s, size_t avail, uint32_t* out) {
    const unsigned char* p = (const unsigned char*)s;
    const unsigned char c = p[0];
    int n = 1;
    uint32_t cp = c;
    if (c >= 0xF0 && c <= 0xF4) { n = 4; cp = c & 0x07u; }
    else if (c >= 0xE0 && c <= 0xEF) { n = 3; cp = c & 0x0Fu; }
    else if (c >= 0xC2 && c <= 0xDF) { n = 2; cp = c & 0x1Fu; }
    else if (c >= 0x80) { *out = 0xFFFD; return 1; }
    if ((size_t)n > avail) { *out = 0xFFFD; return 1; }
    for (int i = 1; i < n; ++i) {
        if ((p[i] & 0xC0) != 0x80) { *out = 0xFFFD; return 1; }
        cp = (cp << 6) | (p[i] & 0x3Fu);
    }
    *out = cp;
    return n;
}

// 句末标点（含换行）。按码位判定：UTF-8 的多字节标点按字节扫会落在续字节上，
// 句子会被切成半个字。
// Sentence terminators (newline included), judged per code point: scanning bytes
// lands inside a multi-byte glyph and cuts the sentence mid-character.
static bool sr_is_sentence_end(uint32_t cp) {
    return cp == '\n' || cp == '.' || cp == '!' || cp == '?' || cp == ';' ||
           cp == 0x3002 /* 。 */ || cp == 0xFF01 /* ！ */ || cp == 0xFF1F /* ？ */ ||
           cp == 0xFF1B /* ； */;
}

static size_t sr_count_cp(const char* s, size_t n) {
    size_t i = 0, c = 0;
    while (i < n) {
        uint32_t cp = 0;
        const int step = sr_utf8_step(s + i, n - i, &cp);
        i += (size_t)(step > 0 ? step : 1);
        ++c;
    }
    return c;
}

// 从 pos 往前退一个码位。/ Step back one code point from pos.
static size_t sr_step_back(const char* s, size_t pos) {
    if (pos == 0) return 0;
    --pos;
    while (pos > 0 && ((unsigned char)s[pos] & 0xC0) == 0x80) --pos;
    return pos;
}

// 从 pos 往后进一个码位。/ Step forward one code point.
static size_t sr_step_fwd(const char* s, size_t n, size_t pos) {
    uint32_t cp = 0;
    const int step = sr_utf8_step(s + pos, n - pos, &cp);
    return pos + (size_t)(step > 0 ? step : 1);
}

size_t scene_req_quote_sentence(const char* ctx, size_t ctx_len,
                                size_t word_off, size_t word_len,
                                size_t max_chars, size_t* out_off, size_t* out_len,
                                bool* fallback) {
    if (out_off) *out_off = 0;
    if (out_len) *out_len = 0;
    if (fallback) *fallback = false;
    if (!ctx || ctx_len == 0) return 0;
    if (word_off > ctx_len) word_off = ctx_len;
    if (word_len > ctx_len - word_off) word_len = ctx_len - word_off;

    // 向前找句首：最后一个句末标点之后。/ Sentence head: after the last terminator.
    size_t s = 0;
    for (size_t i = 0; i < word_off;) {
        uint32_t cp = 0;
        const int step = sr_utf8_step(ctx + i, ctx_len - i, &cp);
        const size_t nxt = i + (size_t)(step > 0 ? step : 1);
        if (sr_is_sentence_end(cp)) s = nxt;
        i = nxt;
    }
    // 向后找句尾：第一个句末标点（含标点本身）。/ Tail: the first terminator, included.
    size_t e = ctx_len;
    for (size_t i = word_off + word_len; i < ctx_len;) {
        uint32_t cp = 0;
        const int step = sr_utf8_step(ctx + i, ctx_len - i, &cp);
        const size_t nxt = i + (size_t)(step > 0 ? step : 1);
        if (sr_is_sentence_end(cp)) { e = nxt; break; }
        i = nxt;
    }
    if (s == 0 && e == ctx_len && fallback) {
        // 两端都没撞到句界：整段就是一句（正常），仍置标志供日志区分
        // 「本来只有一句」与「句界识别失败」。
        // Neither end hit a boundary: the whole context is one sentence (normal),
        // but the flag is raised so the log can tell that apart from a detection
        // failure.
        *fallback = true;
    }

    const size_t chars = sr_count_cp(ctx + s, e - s);
    if (max_chars && chars > max_chars) {
        // 保词居中截断：词左留 (预算 - 词长)/2，其余给右侧。
        // Truncate around the word: half the slack goes left, the rest right.
        const size_t wchars = sr_count_cp(ctx + word_off, word_len);
        const size_t slack = max_chars > wchars ? max_chars - wchars : 0;
        size_t ns = word_off;
        for (size_t k = 0; k < slack / 2 && ns > s; ++k) ns = sr_step_back(ctx, ns);
        size_t ne = word_off + word_len;
        size_t used = wchars + sr_count_cp(ctx + ns, word_off - ns);
        while (ne < e && used < max_chars) {
            ne = sr_step_fwd(ctx, ctx_len, ne);
            ++used;
        }
        s = ns;
        e = ne;
    }
    if (out_off) *out_off = s;
    if (out_len) *out_len = e - s;
    return e - s;
}

