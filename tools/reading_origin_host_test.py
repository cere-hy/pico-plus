#!/usr/bin/env python3
"""中文：实际续读点击、反馈区域与返回来源回归。/ English: Actual continue-card hits, bounded feedback and reader return origins.
SPDX-License-Identifier: Apache-2.0
"""
from pathlib import Path
import re,subprocess,tempfile
from home_cover_cache_host_test import function
root=Path(__file__).resolve().parents[1]
home=root/'main/apps/app_dashboard.c';book=root/'main/apps/app_book.c'
unit=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct {int x,y,width,height;} EpdRect;
// gen24.3 修复（reading_origin 纳管）：补 app_desc_t / app_transfer /
// app_transfer_request_method_picker 桩与 request_app 字段——gen18 在
// app_dashboard.c 加入 deep-link 分支（extern const app_desc_t
// app_transfer; app_transfer_request_method_picker(); ctx->request_app
// =&app_transfer;）后本测试桩未跟进，且不在任何 run_*.sh 门禁内，
// 静默腐坏至 gen24.2（13/13 从未覆盖它）。
// gen24.3 fix (bringing reading_origin under the gate): stubs for
// app_desc_t / app_transfer / app_transfer_request_method_picker and the
// request_app field - after gen18 added the deep-link branch to
// app_dashboard.c, this test's stubs were never updated and it sat outside
// every run_*.sh gate, silently rotting through gen24.2 (the 13/13 never
// covered it).
typedef struct app_desc_s app_desc_t;
struct app_desc_s {int pad;};
typedef struct {int now_ms,leaf; const app_desc_t *request_app;} app_ctx_t;
typedef enum {APP_REDRAW_NONE,APP_REDRAW_AREA,APP_REDRAW_PAGE,APP_REDRAW_FULL} app_redraw_t;
enum {HOME_ROOT,HOME_COMPONENT,HOME_GOAL,HOME_TIME,READING,SHELF};
enum {UI_GESTURE_PRESS,UI_GESTURE_MOVE,UI_GESTURE_TAP,UI_GESTURE_LONG_PRESS,UI_GESTURE_SWIPE_L,UI_GESTURE_SWIPE_R,UI_GESTURE_SWIPE_U};
typedef struct {int type,x0,y0,x,y;} ui_gesture_event_t;
typedef int app_home_component_t;
#define APP_HOME_FOLDER 7
#define HOME_COMPONENT_OPTIONS 2
#define ESP_OK 0
static app_home_component_t s_components[]={0,7};
static int s_view=HOME_ROOT,s_component_browse,s_goal_draft,s_time_stamp;
static bool s_press_pending,s_save_error,s_component_pending,s_widget_pending,s_time_pending;
static struct {bool valid;} s_time_trail;
static EpdRect s_pressed,s_area;
typedef struct {char path[32];} home_book_t;
static home_book_t book={"/sdcard/books/Novel/a.epub"};
static home_book_t *featured(void){return &book;}
static int open_calls,nav=-1;static bool fail_open;
static int app_settings_home_component(void){return 0;}
static int app_settings_set_home_component(int i){(void)i;return 0;}
static int app_settings_home_goal_minutes(void){return 30;}
static int app_settings_set_home_goal_minutes(int i){(void)i;return 0;}
static unsigned component_index(int c){(void)c;return 0;}
static void refresh_days(void){}
static int ui_nav_hit(int x,int y){(void)x;return y>=1096?2:-1;}
static void ui_nav_request(app_ctx_t *ctx,int tab){(void)ctx;nav=tab;}
static bool app_book_request_open_from_home(const char *path){assert(!strcmp(path,book.path));++open_calls;return !fail_open;}
static int picker_calls;
const app_desc_t app_transfer={0};
void app_transfer_request_method_picker(void){++picker_calls;}
'''
# gen24.3：命中宏从源码正则提取，置于提取函数之前（control/on_gesture 引用
# 它们）——单一真源，杜绝与 app_dashboard.c 再次腐坏；提取列表补
# push_card_pressed（on_gesture 的 MOVE 分支引用它）。
# gen24.3: the hit macros are regex-extracted from the source ahead of the
# extracted functions (control/on_gesture reference them) - single source of
# truth, so the test cannot rot against app_dashboard.c again; the extraction
# list gains push_card_pressed (on_gesture's MOVE branch calls it).
unit+='\n'.join(re.findall(r'^#define\s+PUSH_CARD_(?:RECT|HIT)\b.*$',home.read_text(),re.M))
unit+='\n'+'\n'.join(function(n,home) for n in ['continue_card_rect','continue_card_hit_rect','continue_card_pressed','push_card_pressed','control_feedback_area','hit','control','on_gesture'])
settings=(root/'main/settings.h').read_text();a=settings.index('typedef enum {\n    APP_READER_KEY_PREV');b=settings.index('} app_reader_key_action_t;',a)+len('} app_reader_key_action_t;');unit+=settings[a:b]
unit+=r'''
enum {READER_PANEL_NONE,READER_PANEL_TOOLS};
static bool s_reader_return_home;static int s_reader_panel,s_reader_slider,s_stats_activity_ms;
static struct {bool pending;} s_reader_tap;
static int saved,freed,restored,shelf_page=4,shelf_category=3;
static void save_progress(void){++saved;}
static void free_book(void){++freed;}
static void return_to_cached_shelf(app_ctx_t *ctx){++restored;ctx->leaf=shelf_page;assert(shelf_category==3);}
static app_redraw_t turn_page(app_ctx_t *ctx,int d){(void)ctx;(void)d;return APP_REDRAW_AREA;}
static app_redraw_t toggle_reader_fullscreen(app_ctx_t *ctx){(void)ctx;return APP_REDRAW_PAGE;}
static app_redraw_t reader_manual_refresh(app_ctx_t *ctx){(void)ctx;return APP_REDRAW_FULL;}
static void invalidate_prep(void){}
'''
unit+='\n'.join(function(n,book) for n in ['reader_return','reader_key_action','reader_key_label'])
unit+=r'''
int main(void){app_ctx_t ctx={0};
 int points=0;
 // gen24.3 问题 1：继续卡命中区下沿 574 → 566（与推流卡命中区零重叠）。
 // gen24.3 problem 1: the continue card's hit area's lower edge moves
 // 574 -> 566 (zero overlap with the push card's hit area).
 for(int y=172;y<566;y+=17)for(int x=24;x<660;x+=17){
  s_view=HOME_ROOT;nav=-1;int old=open_calls;
  ui_gesture_event_t ev={UI_GESTURE_PRESS,x,y,x,y};
  assert(on_gesture(&ctx,&ev)==APP_REDRAW_AREA&&continue_card_pressed());
  assert(s_area.x==56&&s_area.y==270&&s_area.width==184&&s_area.height==252); // no added card frame
  ev.type=UI_GESTURE_TAP;assert(on_gesture(&ctx,&ev)==APP_REDRAW_NONE);
  assert(open_calls==old+1&&nav==1&&!s_press_pending);++points;
 }
 // gen24.3 问题 1 加固：推流卡带 [566,640) 全网格点按——只跳传书页
 // （picker 被调 + request_app 落 app_transfer），绝不开书；修复前
 // [558,574) 重叠带内点按会被当开书（open_calls 会 +1）。
 // gen24.3 problem 1 hardening: tap every grid point of the push card band
 // [566,640) - it only jumps to the transfer page (picker called +
 // request_app set to app_transfer), never opens the book; before the fix a
 // tap inside the [558,574) overlap band opened the book (open_calls +1).
 for(int y=566;y<640;y+=17)for(int x=24;x<660;x+=17){
  s_view=HOME_ROOT;nav=-1;picker_calls=0;ctx.request_app=NULL;int old=open_calls;
  ui_gesture_event_t ev={UI_GESTURE_PRESS,x,y,x,y};
  assert(on_gesture(&ctx,&ev)==APP_REDRAW_AREA&&push_card_pressed());
  ev.type=UI_GESTURE_TAP;assert(on_gesture(&ctx,&ev)==APP_REDRAW_NONE);
  assert(open_calls==old&&picker_calls==1&&ctx.request_app==&app_transfer&&nav==-1&&!s_press_pending);++points;
 }
 ui_gesture_event_t ev={UI_GESTURE_PRESS,400,350,400,350};on_gesture(&ctx,&ev);
 ev.type=UI_GESTURE_MOVE;ev.x=670;assert(on_gesture(&ctx,&ev)==APP_REDRAW_AREA&&!s_press_pending);
 int old=open_calls;ev.type=UI_GESTURE_SWIPE_U;on_gesture(&ctx,&ev);assert(open_calls==old);
 book.path[0]=0;ev=(ui_gesture_event_t){UI_GESTURE_PRESS,400,350,400,350};assert(on_gesture(&ctx,&ev)==APP_REDRAW_NONE);
 ev.type=UI_GESTURE_TAP;nav=-1;on_gesture(&ctx,&ev);assert(nav==-1&&open_calls==old);
 strcpy(book.path,"/sdcard/books/Novel/a.epub");fail_open=true;ev.type=UI_GESTURE_PRESS;on_gesture(&ctx,&ev);
 ev.type=UI_GESTURE_TAP;assert(on_gesture(&ctx,&ev)==APP_REDRAW_AREA&&nav==-1);fail_open=false;
 assert(!strcmp(reader_key_label(APP_READER_KEY_HOME),"返回主页"));
 for(int via_key=0;via_key<2;++via_key)for(int from_home=0;from_home<2;++from_home){
  s_view=READING;s_reader_panel=READER_PANEL_TOOLS;s_reader_return_home=from_home;
  nav=-1;ctx.leaf=99;int before=freed;
  app_redraw_t result=via_key?reader_key_action(&ctx,APP_READER_KEY_HOME):reader_return(&ctx);
  if(from_home)assert(result==APP_REDRAW_NONE&&nav==0&&freed==before);
  else assert(result==APP_REDRAW_PAGE&&nav==-1&&s_view==SHELF&&ctx.leaf==4&&freed==before+1&&s_reader_panel==READER_PANEL_NONE);
 }
 assert(saved==4&&restored==2);
 printf("PASS: %d full-card hit points, push-card band taps jump to the transfer page only (never open the book), local cover-lift bounds, cancel/empty/open-failure guards; screen/key/hold shared return routes retain home or shelf category/page; renamed label\n",points);
}
'''
# gen24.3：去掉 dir='/private/tmp'（macOS-ism，干净 Linux checkout 会
# FileNotFoundError），改用系统默认临时目录。
# gen24.3: drop dir='/private/tmp' (a macOS-ism that raises
# FileNotFoundError on a clean Linux checkout) and use the system default
# temporary directory.
with tempfile.TemporaryDirectory(prefix='kiiko-reading-origin-') as folder:
 c=Path(folder)/'test.c';binary=Path(folder)/'test';c.write_text(unit)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-g','-fsanitize=address,undefined',str(c),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
