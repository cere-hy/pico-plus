/* 中文：调度测试替身。/ English: Scheduler test shim. */
#pragma once
#include "common.h"
#define UI_LONG_PRESS_MS 500
#define UI_SWIPE_MIN_PX 120
#define UI_TOUCH_SLOP_PX 24
#define UI_LOCK_WIDTH 684
#define UI_LOCK_HEIGHT 1216
EpdRect ui_content_refresh_area(void);
static inline void ui_text_set_system_scale(bool enabled) { (void)enabled; }
static inline void ui_text_set_system_font(bool enabled) { (void)enabled; }

#define UI_SETTLE_DU_MAX 6
#define UI_SETTLE_IDLE_MS 2000

// gen26.5 M2 Step9：ai_reader 版面用到的绘制原语声明。签名与真头逐字对齐
// （main/ui/ui_kit.h L128/L138/L148/L161/L164/L238/L304、main/ui/ui_nav.h L16），
// 实现由各宿主测试自备（app_loop 测试链对这几个符号零调用，加声明不产生引用）。
// gen26.5 M2 Step9: declarations for the drawing primitives ai_reader's layout
// uses. Signatures match the real headers verbatim; implementations are
// supplied by each host test (the app_loop test chain calls none of these, so
// adding declarations creates no references).
void ui_text(uint8_t* framebuffer, int x, int y_top, int px, const char* text,
             enum EpdFontFlags align, bool inverted);
int ui_text_fixed_width_px(int px, const char* text);
void ui_text_fixed_vc(uint8_t* framebuffer, int x, int center_y, int px, const char* text,
                      enum EpdFontFlags align, bool inverted);
int ui_text_title_fit(char* text, int preferred_px, int width, const char* font_sample);
void ui_text_title_vc(uint8_t* fb, int x, int center_y, int px, const char* text,
                      const char* font_sample, enum EpdFontFlags align);
void ui_hairline(uint8_t* framebuffer, int y, int x, int w, uint8_t color);
void ui_clear_page(uint8_t* framebuffer);
void ui_nav_status(uint8_t* fb);
