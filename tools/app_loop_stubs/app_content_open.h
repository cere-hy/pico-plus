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

/// M2 Step9：双 GC16 档位查询与详解活跃谓词（app_loop.c 接线用）。定义在
/// tools/app_loop_host_test.c 的可编程桩里——本仓库第三次栽在「实现契约变更
/// 忘同步宿主桩」（gen19/gen21/gen26.3 同型），这次接线当天就连头文件带桩
/// 一起补，守门用例四（run_app_loop_host_tests.sh）盯死它们被真的调用。
/// M2 Step9: the double-GC16 gear query and the detail-active predicate (used
/// by the app_loop.c wiring). Definitions live in the programmable stubs of
/// tools/app_loop_host_test.c - this is the third time an implementation
/// contract change missed the host stubs (same shape as gen19/gen21/gen26.3),
/// so this time the header and the stubs land together and gatekeeper 4
/// (run_app_loop_host_tests.sh) watches that they are really called.
bool app_book_ai_def_full(void);
bool app_book_ai_detail_active(void);

void app_book_cover_mode_changed(void);
void app_home_cover_mode_changed(void);
