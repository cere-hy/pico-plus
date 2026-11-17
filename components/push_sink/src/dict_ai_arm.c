/*
 * dict_ai_arm.c — 词典卡 AI 键武装挂起态纯逻辑实现（见 dict_ai_arm.h）。
 * 零平台依赖：只用 <string.h>，不引用 ESP-IDF / NimBLE / FreeRTOS 符号。
 */
#include "dict_ai_arm.h"
#include <string.h>

void dict_ai_arm_reset(dict_ai_arm_t *st) {
    if (!st) return;
    memset(st, 0, sizeof(*st));
    st->armed_at_ms = 0;
    st->active = false;
}

bool dict_ai_arm_arm(dict_ai_arm_t *st, const char *word, uint32_t book_id,
                     uint32_t pos, int64_t now_ms) {
    if (!st || !word || !*word) return false;
    size_t n = strlen(word);
    if (n >= DICT_AI_ARM_WORD_MAX) n = DICT_AI_ARM_WORD_MAX - 1; // 保 NUL
    memset(st, 0, sizeof(*st));
    memcpy(st->word, word, n);
    st->word[n] = '\0';
    st->book_id = book_id;
    st->pos = pos;
    st->armed_at_ms = now_ms;
    st->active = true;
    return true;
}

bool dict_ai_arm_active(const dict_ai_arm_t *st) {
    return st && st->active;
}

bool dict_ai_arm_expired(const dict_ai_arm_t *st, int64_t now_ms, uint32_t window_ms) {
    if (!st || !st->active || window_ms == 0u) return false;
    if (now_ms < st->armed_at_ms) return false; // 时钟回拨：不误判超时
    return (now_ms - st->armed_at_ms) >= (int64_t)window_ms;
}

bool dict_ai_arm_take(dict_ai_arm_t *st, char *word, size_t wcap,
                      uint32_t *book_id, uint32_t *pos) {
    if (!st || !st->active) return false;
    if (word && wcap) {
        size_t n = strlen(st->word);
        if (n >= wcap) n = wcap - 1;
        memcpy(word, st->word, n);
        word[n] = '\0';
    }
    if (book_id) *book_id = st->book_id;
    if (pos) *pos = st->pos;
    // 取走即清槽：防重发（连接建成只 flush 一次，二次 take 空）。
    dict_ai_arm_reset(st);
    return true;
}

bool dict_ai_arm_cancel(dict_ai_arm_t *st) {
    if (!st || !st->active) return false;
    dict_ai_arm_reset(st);
    return true;
}

const char *dict_ai_arm_word(const dict_ai_arm_t *st) {
    return (st && st->active) ? st->word : NULL;
}
