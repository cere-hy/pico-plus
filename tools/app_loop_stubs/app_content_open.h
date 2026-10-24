#pragma once
#include <stdbool.h>

static inline bool app_book_request_open(const char *path) { (void)path; return true; }
static inline bool app_image_request_open(const char *path) { (void)path; return true; }
static inline void app_files_request_folder(int folder) { (void)folder; }
static inline void app_book_request_manage(void) {}
static inline bool app_book_reader_body_visible(void) { return false; }

#include <stddef.h>
bool app_book_resume_context(char *path, size_t capacity, bool *fullscreen);

/// gen26.3：补 M1 漏同步的声明（app_loop.c 轮询释义 UI 槽用）——宿主编译曾因隐式
/// 声明 + -Werror 全挂，两个 runner 红到本轮才暴露。
/// gen26.3: backfill the declaration M1 forgot to sync here (the app_loop poll of
/// the definition UI slot) - host builds died on implicit declaration + -Werror and
/// two runners stayed red until this round.
bool app_book_ai_def_ready(const char *word, const char *def);

void app_book_cover_mode_changed(void);
void app_home_cover_mode_changed(void);
