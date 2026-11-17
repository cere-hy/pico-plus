/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 图书书架、阅读、目录与页内进度；文件解析和排版由book模块负责。
 * Book shelf, reader, TOC and progress; book modules own parsing and pagination.
 *
 * 用户修订：阅读设置采用已确认的半屏双列排版；自动全刷与翻页效果点按轮换，保留按键控制二级页。
 * User revision: use the approved compact two-column reading sheet; tap cycles refresh/effect and key controls retain their secondary page.
 * 冻结：Phase4b统一手势入口并接管三键为上页/工具条/下页；工具条保留强刷。屏幕翻页在抬起提交，不画按下态。
 * 用户修订：图文混排原样采用 PR9 的 book_layout；调用侧只适配图片缓存与上下文生命周期。
 * 晃动默认关；横向左晃上一页、右晃下一页，触摸与回弹不触发；离页恢复加速度配置并休眠。render只绘图。
 * 预渲染与同步推屏重叠、按行可停；推屏返回前交还字体与章节资源。
 * 普通翻页只刷新正文与页脚；手动或周期清残影整屏全刷。
 * 用户修订：阅读手动与周期全刷恢复 rc90 的原表 GC16 目标页输出，不先擦白再补文字。
 * User revision: restore rc90's original-table GC16 target-page output for manual and periodic refresh, without a white pre-clear and text pass.
 * 用户修订：正文中间双击固定切换全屏，单击左右各半或上1/3下2/3翻页，不受按键配置影响。
 * 三键单击及中键长按可自定义，提供翻页/工具栏及首页/全屏/工具栏两套推荐；中键长按默认返回主页，阅读页不画右下角菜单图标。
 * 用户授权基础管理：长按书架先看完整详情，清进度与删文件分别确认；失败保留待重试记录，不自动回收其他书进度。
 * 用户修订：单本管理为书架弹窗；管理页用于批量操作。分页和排序保留勾选，筛选/应用搜索及重扫清除勾选。
 * 失败进度仅按变更路径失效；删除后的清理重试保留到本次开机结束，不随切页释放。
 * 卡失效时先保存进度并关闭阅读资源，再由主循环回退字体；禁止自动续读失效挂载。
 * 用户最新修订：书架只保留深色书轨与亚克力，每页九本；收录导入及读过的书，移出仅隐藏，再读重新上架。
 * 底栏保留首页/书架/文件/设置，设置直接进入设置页。
 * 用户修订：长按图书可用本机拼音输入编辑书名；阅读时长与翻页真实记录，供票根锁屏使用。
 * 用户最新修订：书名编辑可点选插入位置并用左右键微调，支持在文字中间插入和删除。
 * 用户授权新输入法：书名和搜索共用九宫格/全键盘、中英文及离线词语候选，不改正文排版。
 * 用户修订：首页、书架、文件、设置四栏导航；切换界面采用 GL16，章节首页单独排标题。
 * 用户修订：EPUB 章节首页以书内目录标题为准，正文题头仅在相同时折叠，避免引言误判。
 * 用户修订：阅读进度条无外伸刻度并使用圆角；继续阅读区显示最近书籍封面。
 * 用户修订：书架整页读取成品缓存后一次显示；首次生成有界分批处理，但整页封面一起显现，不逐本推屏。
 * User revision: read the page's finished caches before its first frame; cold generation is bounded, but cover artwork is revealed together.
 * 用户修订：字号和间距重排只保存最终进度，字号使用现有闲置灰阶整理。
 * 用户修订：阅读设置滑杆可拖动并在松手后重排；字体选择可纵向翻页；统计入口显示真实明细与近30天数据。
 * 用户修订：前后均为纯文字的普通翻页采用柔和 GL16 差分，仅白底灰边提前少量回浅，不改字形或灰度目标；插图、混排、周期全刷及水波纹保留原规则。
 * User revision: soften ordinary pure-text GL16 turns by advancing a few white-to-gray settling pulses; preserve glyph targets, images, mixed pages, cleanup and ripple.
 * 用户修订：目录由独立模块整页绘制与命中；目录标题清理换行并限制为单行，翻页不再沿用书架的局部刷新。
 * 用户修订：书架封面抽出与取消仅驱动变化像素，保持灰阶，不在点按时强制清屏。
 * 用户修订：列表封面上弹8px，松手先回位再开书；只重画两位置的并集，复用缓存，不触碰文字/边框/分隔线。快刷用跟随DU，其他模式保留灰阶差分。
 * User revision: list covers bounce up 8px and settle before opening; repaint only the position union using cached artwork, preserving text, frames and separators. Fast uses FOLLOW DU; other modes retain gray differentials.
 * 用户确认：列表封面缩至120×156，保留原中心点与右侧文字位置，等比填充并居中裁切。
 * User approval: list covers use 120×156 boxes, retaining their center and right-side text positions with aspect fill and centered cropping.
 * 用户修订：快刷亚克力采用规则一像素棋盘格，与已缓存封面合并输出，不注册灰区或等待框架先显示；未缓存封面继续有界延后解码。普通及水波纹保留灰阶，切页/锁屏取消旧目标。
 * 用户修订：书架读取最多当前整页缓存；未缓存封面限时准备，整组显现后才确认待刷标记，文件间检查导航，不新增整页缓存。
 * 用户修订：阅读背景先列pictures图片、忽略隐藏文件，点开预览上下切换，返回目录；背景按整屏坐标固定，不随文字边距或全屏状态缩放。
 * User revision: fast acrylic uses a regular one-pixel checkerboard in the same output as cached covers, without gray registration or a furniture-first wait; uncached covers retain bounded lazy decoding. Ordinary/water retain grays; page/lock changes cancel stale targets.
 * 用户要求采用PR17的快速水波纹，并最新指定8条带、每拍11090us；每次显式设置，结束恢复先前节拍，避免影响主页动画。全刷、插图及失败恢复保留原优先级。
 * User requests PR17 fast ripple with the latest 8 bands at 11090us; explicitly select pacing per turn and restore it afterward so main animations remain independent. Retain cleanup, image and recovery priority.
 * 用户增加管理/导入按压态：复用现有抬起确认流程，按下灰底加深并下沉2px，只推按钮区域。
 * User adds management/import feedback: retain release-confirmed actions, darken and depress controls 2px on press, presenting only their region.
 * 用户修订：快刷封面保留轻量减黑并按最新要求略增黑点密度，在PSRAM缓存最多九张1位缩略图（约40KiB）；进度未改变时不重复丢弃封面。缓存只画封面，收藏与界面文字实时绘制，开书或卡失效时释放。
 * Frozen: Phase4b uses the shared gesture entry and owns previous/tools/next keys; the toolbar keeps full refresh. Screen turns commit on release without pressed decoration.
 * User revision: adopt PR9's book_layout unchanged; callers only adapt bitmap caching and context lifetimes.
 * Authorized keyboard revision: titles and search share T9/QWERTY, bilingual offline phrase candidates; body layout stays intact.
 * 用户修订：输入不重画题头、封面或正文，只刷新输入变化区域，退出后释放查字缓存。
 * 用户修订：新开书默认全屏；双击中间切换，翻页关闭工具栏并回全屏；重排以已选目标页的文字位置为锚，不跳过未读文字。
 * User revision: new books open full screen; center double taps toggle it, turns dismiss tools and return to full screen; reflow anchors the selected destination before changing geometry so unread text is not skipped.
 * 用户修订：字重用横向减/加胶囊，每次微调1/16像素覆盖率；保持字形比例/字宽，不加载额外整份字库。
 * User revision: a horizontal minus/plus pill adjusts coverage by 1/16 pixel per tap, preserving advances/proportions without another font.
 * 用户确认：字体/字重与书内字体/排版分别并排，移除阅读背景入口；字号和阅读线继续可调。
 * User approval: pair face/weight and embedded-face/typography, removing the background entry while keeping size and reading rules adjustable.
 * 用户增加阅读抗锯齿开关，默认开；书架整页封面合并显示；正文底部上滑固定呼出工具栏，不受翻页/按键设置影响。
 * User adds default-on reading AA and grouped shelf artwork; upward swipes from the body bottom always open tools independently of tap/key modes.
 * 用户修订：底部上滑固定呼出工具栏，不随按键及翻页模式变化；取消短按或长按必须保留工具栏的限制。
 * User revision: bottom-edge swipes always open tools independently of key/tap modes; short and hold actions no longer require a toolbar mapping.
 * User revision: input refreshes changed regions without repainting headers, covers or body; release lookup caches on exit.
 * Shake is off by default: left/right impulses turn back/forward, suppressing touch and rebound; exit restores the sensor and sleeps it. Render only paints.
 * Prefetch overlaps synchronous output and stops between rows; no font/chapter resources survive presentation.
 * Ordinary turns refresh only body and footer; manual and periodic ghost cleanup refresh the entire screen.
 * User revision: center body double taps always toggle full screen; single taps use halves or upper 1/3/lower 2/3 independently of key mappings.
 * Short key actions are configurable with two presets; middle holds are configurable and default to home. The reader has no bottom-right menu icon.
 * User-authorized management shows full details before separate clear/delete confirmations; retain failed saves for retry without pruning other books.
 * User revision: single-book actions use a shelf dialog; full management is for batches. Paging/sorting preserve selection; filtering/applied search and rescanning clear it.
 * Invalidate failed progress only for changed paths; retain deletion cleanup retries across page exits for this boot.
 * Lost media saves progress and closes reader resources before global font fallback; never auto-resume an invalid mount.
 * Latest user revision: the two retained shelf styles hold nine imported or read books; retired styles restore as acrylic, and removal hides until reread.
 * The fourth tab opens Settings directly.
 * User revision: book details lead to an on-device Pinyin title editor; measured reading time and turns feed the ticket lock face.
 * Latest user revision: the title editor can place and move an insertion caret for edits in the middle of text.
 * 用户统一修订：书名与搜索采用共用常亮光标；点文字或箭头定位，按系统字体测量。
 * User-wide revision: title/search share a steady caret, text/arrow positioning and system-font measurement.
 * User revision: home, shelf, files and settings have four-tab navigation; view changes use GL16 and chapter starts have a title lead.
 * User revision: EPUB chapter leads use navigation titles; body headings are folded only when matching, preventing front matter from being mislabeled.
 * User revision: the reader bar is rounded without protruding ticks; continue reading displays the latest cover.
 * User revision: the shelf retains its title layout, reads a complete cached page before painting, and reveals cold artwork only after the page is prepared.
 * User revision: size and spacing reflow saves final progress only; size uses the existing idle grayscale settle.
 * User revision: reader sliders drag and reflow on release; font selection pages vertically; statistics entries show real details and recent-30-day data.
 * User revision: pure-text turns use softened gray-edge differential GL16; images/mixed pages, periodic cleanup and water turns retain their policies.
 * User revision: a standalone module owns full-page TOC rendering and hit testing; normalized single-line titles cannot leak into another row.
 * User revision: cover lift and cancellation drive changed pixels in grayscale, without forced cleanup during a tap.
 * 用户修订：快刷仅常驻黑白缩略图；灰阶解码缓冲转好即释放，灰阶模式按需重建，收藏白边单独记录。
 * User revision: fast mode retains only packed monochrome thumbnails, frees gray decoding buffers immediately after conversion, rebuilds gray on demand, and stores favorite-edge contrast separately.
 * User revision: fast covers retain a light white lift with slightly denser blacks per the latest request and cache at most nine 1-bit thumbnails in PSRAM (about 40 KiB); unchanged progress no longer discards covers. Cache artwork only, draw favorites and UI text live, and release on book opening or media loss.
 * 用户最新修订：阅读书名与章节标题复用当前阅读字体，以原32/48px直接光栅化；首页另用其活动系统字体，不同时加载两份字库。
 * Latest user revision: book/chapter titles reuse the current reader face at the original 32/48px; Home uses its active system face, never retaining both fonts.
 * 用户确认：题头下拉选择 books 子目录，各类别/布局记住页码；去掉题头横线，主体在确认位置基础上下移8px。
 * User approval: choose books subfolders in the title menu, retain per-category/layout pages, remove the header rule and lower the approved body by 8px.
 */
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <stdatomic.h>
#include <unistd.h>

#include "app.h"
#include "app_companion_state.h"
#include "app_content_open.h"
#include "boot_state.h"
#include "app_registry.h"
#include "ble_page_turner.h"
#include "book_layout.h"
#include "book_select.h"
#include "book_id.h"
#include "book_cover.h"
#include "book_cover_jobs.h"
#include "ai_reader.h"
#include "scene_req.h"
#include "book_lock_collage.h"
#include "book_epub.h"
#include "book_policy.h"
#include "book_progress.h"
#include "book_title.h"
#include "book_ticket.h"
#include "book_excerpt.h"
#include "pico_heapdiag.h"
#include "book_toc.h"
#include "app_font_context.h"

// M5-A 框选：实现块在文件后半段，这些前置声明让早于它的消费者（双指、按键、
// 绘制、任务边界）能引用；全部 static，不扩大任何外部接口。
// M5-A selection: the implementation sits later in this file, so these forward
// declarations let earlier consumers (two-finger, keys, drawing, task boundaries)
// reach it. All static; no external surface grows.
static bool boxsel_active(void);
static void boxsel_reset(void);
static void boxsel_draw(uint8_t* fb);
static void boxsel_boundary_reset(void);
static bool boxsel_single_finger_priority(app_ctx_t* ctx);
// S26-2 出口声明制：任何离开框选态的路径都必须报名字，所以早期调用点（双指、按键）
// 调的是这一个带标签的出口，而不是裸 boxsel_reset()。
// S26-2 exit declaration: anything leaving box-select must name itself, so early call sites
// (two-finger, keys) call this labelled exit rather than a bare boxsel_reset().
typedef enum {
    BOXSEL_EXIT_SAVE = 0, BOXSEL_EXIT_AI, BOXSEL_EXIT_CANCEL,
    BOXSEL_EXIT_TAP_OUT, BOXSEL_EXIT_DUAL, BOXSEL_EXIT_TIMEOUT,
    BOXSEL_EXIT_KEY,          ///< 短按：先退出框选，再把键交给常规处理 / short key: leave first, then handle the key
    BOXSEL_EXIT_KEY_LONG,     ///< 长按 / long key
    BOXSEL_EXIT_BOUNDARY,     ///< 行列边界失效后的清场（换书/熄屏等，非用户出口）/ cleared after a reflow or book switch
} boxsel_exit_t;
static app_redraw_t boxsel_exit(boxsel_exit_t why);
#include "book_source.h"
#include "book_store.h"
#include "book_shelf_categories.h"
#include "dict_lookup.h"
#include "dict_popup.h"
#include "vocab_log.h"
#include "tts_page.h"
#include "display.h"
#include "display_reader_waveform.h"
#include "e0470_epaper_waveform.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "push_sink.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#if CONFIG_BOOK_PREP_STACK_PSRAM
// 26.9.13 P0-a（实验开关）：带内存属性的任务创建/删除。只在开关打开时引入。
// 26.9.13 P0-a (experiment switch): memory-caps task create/delete, only when enabled.
#include "freertos/idf_additions.h"
#endif
#include "read_pico_init.h"
#include "read_pico_sd.h"
#include "read_pico_transfer.h"
#include "read_pico_search.h"
#include "settings.h"
#include "book/book_reader_input.h"
#include "book/excerpt_store.h"   // 26.9.26 S26-2：摘抄落盘（只剩入队 API 可用，见 B32'）/ excerpt storage (enqueue API only, see B32-prime)
#include "book/excerpt_section.h" // 26.11.4：段落解析切节（只取【原文对照】）/ section cut for passage answers
#include "esp_attr.h"              // 26.11.4：EXT_RAM_BSS_ATTR（切节缓冲落 PSRAM）
#include "ttf_font.h"
#include "ui_kit.h"
#include "ui_text_input.h"
#include "ui_keyboard.h"
#include "ui_image_dither.h"
#include "ui_reader_background.h"
#include "ui_gesture.h"
#include "ui_dualgesture.h"
#include "ui_menu.h"
#include "ui_nav.h"
#include "ui_click_feedback.h"
#include "app_transfer_mode.h"

// 阅读工具栏图标盒边长；行内图标在 1096..1216 的工具条里以 1138 为中心。
// Reader toolbar icon box; the row centres on y=1138 inside the 1096..1216 bar.
#define READER_TOOL_ICON_PX 40
// 二级面板返回键里的箭头边长；圆底半径 24，原来的箭头是 20 像素高。
// Chevron box inside a secondary panel's back control; the old arrow was 20 px tall.
#define READER_SHEET_BACK_PX 34

#define BOOK_ROWS 13
#define BOOK_GRID_ROWS 9
#define BOOK_BULK_ROWS 6
#define BOOK_PX_MIN 36
#define BOOK_PX_MAX 72
#define BOOK_PX_STEP 4
// 边距取上游 r91 的九档滑杆（0..48）；工具条第 6 项（生词本）与第 7 项（听书）是我方新增，故计数为 7。
// Margins follow upstream r91's nine-step slider (0..48); the sixth tool (vocabulary book) and the
// seventh (TTS read-aloud) are ours, hence the count of seven.
#define BOOK_MARGIN_MIN 0
#define BOOK_MARGIN_MAX 48
#define BOOK_MARGIN_CHOICES 9
#define BOOK_TOOL_COUNT 7
#define BOOK_FONT_PAGE 9
#define BOOKMARK_MAX 24
#define BOOKMARK_ROWS 6
// gen26.3 ④ 「AI 查询中…」卡超时：miss 点词后手机 15s 不回帧即降级为失败提示。
// 查询卡显示的是真实状态，不能无限期假活。/ gen26.3 "AI querying..." card
// timeout: 15s without a phone answer degrades the card to a failure notice.
// The card shows a real state and must never fake forever.
#define DICT_AI_PENDING_TIMEOUT_US 15000000
// 26.11.31：AI 键离线武装的等待卡用 60s 广播窗口判超时（与 device 侧
// DICT_AI_ARM_WINDOW_MS 对齐），避免 15s 卡片先回退、广播还在、0x07 回来被丢。
#define DICT_AI_ARMED_TIMEOUT_US 60000000

typedef enum { SHELF, READING, TOC, MANAGE, BULK, IMPORT, SEARCH, EDIT } book_view_t;
typedef enum {
    READER_PANEL_NONE,
    READER_PANEL_TOOLS,
    READER_PANEL_FONT_SETTINGS,
    READER_PANEL_LAYOUT_SETTINGS,
    READER_PANEL_WEIGHT_SETTINGS,
    READER_PANEL_RULE_SETTINGS,
    READER_PANEL_FONT_PICKER,
    READER_PANEL_BACKGROUND,
    READER_PANEL_BACKGROUND_PREVIEW,
    READER_PANEL_STATS,
    READER_PANEL_BOOKMARKS,
    READER_PANEL_STATS_RECENT,
    READER_PANEL_REFRESH_SETTINGS,
    READER_PANEL_TURN_SETTINGS,
    READER_PANEL_KEY_SETTINGS,
    READER_PANEL_KEY_PICKER,
    READER_PANEL_DICT, ///< 点词查译卡片 / Tap-to-lookup definition card
    READER_PANEL_TTS,  ///< 听书面板（开关 + 链路状态）/ TTS panel (switch + link status)
} reader_panel_t;
typedef struct {
    char name[256];
    char author[128];
    char path[BOOK_STORE_PATH_MAX];
    uint32_t size;
    uint16_t chapter;
    bool is_flash;
    bool has_progress;
    uint8_t pct;
    uint32_t recent;
    bool selected, removed, search_match, favorite;
} shelf_entry_t;

static const char* TAG = "book";
// M2 Step9：AI 释义超容量时改用 ai_reader 详解会话承载（覆盖层，不开书）。
// 本标志是「本次卡面改动需要 FULL 重刷」的信号——开/关详解各一次 GC16
// （双 GC16 设计点），词卡原地更新则为 PAGE。app_loop 回流点据此选档位。
// M2 Step9: when an AI definition overflows the card, the ai_reader detail
// session (an overlay, no book opened) carries it. This flag signals "this
// card change needs a FULL repaint" - one GC16 each for open and close (the
// double-GC16 design point); an in-place card update stays PAGE. The app_loop
// return path picks the tier from it.
static bool s_ai_def_full;
static book_view_t s_view;
static int s_presented_view = -1;
static bool s_reader_image_refresh_pending;
static bool s_reader_turn_pending;
// 26.11.6：这一拍是不是「页码前进」（正文翻页 / AI 解析页翻页）。
// 由调用侧显式置位，present 不靠几何猜——翻页的面积永远等于正文区，没有几何签名。
// 忘置位时兜底是整屏 full：灰账安全、闪账爆炸，所以宿主真值表逐条枚举 READING 的
// 每一条 PAGE 路径，断言各自落的分支，不许有路径靠兜底活着。
// 26.11.6: is this frame a page advance (body turn or AI detail turn)? The caller sets
// it explicitly; present never guesses from geometry - a turn's area always equals the
// body, so there is no geometric signature. Forgetting it falls back to a full refresh
// (ghost-safe, flash-hostile), which is why the host truth table enumerates every
// READING PAGE path and asserts the branch each one lands in.
static bool s_reader_body_turn;
// 26.11.7 S2：这一拍是不是**页码前进**（正文翻页 / AI 解析页 / 全屏翻页）。
// 26.11.6 的计数散在两处（turn_page 与 reader_ai_turn 各调一次 reader_note_turn），
// 于是「记了多少页」与「实际播了几屏」是两条线：中途被 request_app 截走的拍、
// 走了整屏清底的拍，都可能数错。这里只留一个标记，账统一在 present 里结。
// 26.11.7 S2: is this frame a page advance? 26.11.6 counted in two places, so
// "pages counted" and "screens actually presented" were two separate lines. Only a
// flag here; the ledger settles in present(), once.
static bool s_reader_page_advance;
// 26.11.6 T3：上一页有灰阶内容、这一页没有 —— 正文区整区清底一次。
// Last frame carried grayscale, this one does not: clean the body area once.
static bool s_reader_clear_from_image;
// T3 的「上一页」不能在翻页后从 s_page 反推（它已经变了），所以翻页开始时先记下来。
// The "previous page" for T3 cannot be recovered from s_page after the turn, so the
// turn records it before advancing.
static bool s_reader_frame_had_image;
// 只记录成功推屏的纯正文基准，避免跨章或退出插图时误用文字波形。
// Track only successfully presented text-only bodies, protecting chapter and image transitions.
static bool s_reader_text_frame;
static char s_requested_open[BOOK_STORE_PATH_MAX];
static bool s_requested_open_home;
static bool s_requested_fullscreen = true;
static bool s_reader_return_home;
static bool s_requested_manage;
static shelf_entry_t* s_shelf;
static struct {
    int index;
    uint8_t *gray, *fast_bits;
    unsigned width, height;
    uint8_t fast_contrast, fast_phase;
    bool fast_white_edge;
} s_covers[BOOK_ROWS];
static unsigned s_cover_pending_mask, s_cover_decode_mask, s_cover_warm_mask;
static unsigned s_cover_dirty_mask, s_cover_present_mask;
static int64_t s_cover_warm_after_ms;
static size_t s_shelf_capacity;
static int s_count, s_visible_count, s_filter;
static bool s_recent_sort;
// 目录记录有界增长；同一开机周期按完整路径保存两种布局的页码。
// Bounded folder records retain pages by full path for the two layouts during this boot.
#define SHELF_CATEGORY_LIMIT 128
#define SHELF_CATEGORY_ROWS 7
typedef struct { char path[BOOK_STORE_PATH_MAX]; int pages[2], count; bool available; } shelf_category_t;
static shelf_category_t *s_categories;
static int s_category_count, s_category_capacity, s_category_selected, s_category_menu_page;
static bool s_category_menu;
static char s_category_root[BOOK_STORE_PATH_MAX];
static void shelf_category_init(void);
static void shelf_category_remember(app_ctx_t *ctx);
static void shelf_category_restore(app_ctx_t *ctx);
static void shelf_categories_recount(void);
static void draw_category_menu(uint8_t *fb);
static const char *shelf_category_name(int index);
static bool shelf_category_matches(const char *path);
static int shelf_category_add(const char *path);
static app_redraw_t shelf_category_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev);
static bool s_reader_favorite;
static shelf_entry_t s_managed;
static bool s_delete_confirm, s_file_removed;
static char s_shelf_warning[128], s_manage_message[128];
static char s_query[65], s_search_draft[65], s_batch_message[128];
static ui_text_edit_t s_search_input;
static book_view_t s_search_parent;
// 批量操作需要二次确认的三类：删文件、清进度、从书架移除。前两类动数据，后一类只动书架。
// Three batch actions need confirmation: delete files, clear progress, remove from the shelf.
// The first two touch stored data; the last one only touches shelf membership.
typedef enum {
    BATCH_DELETE,
    BATCH_CLEAR,
    BATCH_UNSHELF,
} batch_kind_t;
static bool s_batch_confirm;
static batch_kind_t s_batch_kind;
typedef struct pending_progress {
    char path[BOOK_STORE_PATH_MAX];
    book_progress_t value;
    bool dirty, progress_saved;
    book_progress_watch_t* watch;
    struct pending_progress* next;
} pending_progress_t;
static pending_progress_t* s_pending;
typedef struct delete_retry {
    shelf_entry_t entry;
    struct delete_retry* next;
} delete_retry_t;
static delete_retry_t* s_delete_retries;
static char s_latest_path[BOOK_STORE_PATH_MAX];
static char s_editor_title[121], s_editor_notice[96];
static ui_text_edit_t s_editor_input;
static uint8_t* s_editor_cover;
static bool s_save_failed;
static bool s_pending_invalidated;
static int64_t s_save_retry_ms;
static unsigned s_store_revision;
static uint32_t s_progress_revision=UINT32_MAX;
static bool s_shelf_cache_valid, s_cache_sd_present, s_cache_sd_mounted;
static uint8_t s_cache_shelf_style;
static bool s_scan_pending, s_clear_confirm;
static reader_panel_t s_reader_panel;
static bool s_reader_fullscreen;
static int64_t s_reader_clock_minute = -1, s_reader_clock_painted_minute = -1;
static bool s_reader_clock_pending;
static char s_message[128], s_storage[128], s_path[BOOK_STORE_PATH_MAX], s_title[128];
// gen18（18-a）：读 push.txt 期间 sink 后台保活；手机回连时本页自动切回传书页
// 接客。这个标记告诉 book_on_exit「此次离页是交接，不收摊」——摊子留给用户
// 真正离开阅读器时收（裁决：收摊点挂 book_on_exit）。
// gen18 (18-a): the sink stays background-alive while push.txt is read; a
// returning phone bounces this page back to the transfer page to receive. The
// flag tells book_on_exit "this exit is a handoff, do not tear down" - the
// teardown waits for a real reader exit (the ruling: teardown hangs off
// book_on_exit).
static bool s_push_handoff;
// gen26.5 Step10 听书：已推送页哨兵，只记「成功投过 0x19 的 (章, 页)」。
// 推送失败不更新——下轮原样重发（对端在 seq==0 处自然重置，无半页残留）；
// 开关打开时重置保首推；关书时清哨兵但不停开关（偏好跨书保持，换书后同
// (章,页) 不撞车）。开关态本身在 tts_page.c（NVS rp_tts），此处不存第二份。
// gen26.5 Step10 TTS: the pushed-page sentinel records only (chapter, page)
// pairs whose 0x19 page text went out. A failed push leaves it untouched - the
// next round resends as-is (the peer resets at seq==0, no half-page residue);
// turning the switch on resets it so the current page always gets a first push;
// closing a book clears it but never stops the switch (the preference survives
// books and a new book's same (chapter,page) never collides). The switch state
// itself lives in tts_page.c (NVS rp_tts); no second copy is kept here.
static size_t s_tts_last_chapter = SIZE_MAX, s_tts_last_page = SIZE_MAX;
// 上一 tick 的链接态：用于检测「刚重连」跃迁，触发 D2B 重连补发（断线期间
// 关开关、重连后设备开关为关 → 补推空页让对端停朗读）。/ Last tick's link
// state: detects the just-reconnected edge to fire D2B's reconnect resend
// (switch turned off while disconnected -> on reconnect, re-push an empty
// page so the peer stops the read-aloud).
static bool s_tts_link_was_up = false;
/* gen30 伴读案 · 步 3：进/出伴读只置 pending，置位与重排由 on_tick 同批原子完成，
   与 s_fonts_reload_pending 同款（触摸回调栈太深，一章排下来会穿栈）。
   gen30 read-along step 3: enter/exit only set a pending flag; the state flip and
   the reflow happen atomically in one on_tick batch - same shape as the font
   reload pending (a reflow inside a touch callback overruns the main task stack). */
static bool s_companion_enter_pending, s_companion_exit_pending;
/* 伴读案 30 批 · 步 4：右栏摘要显示层（R0 冻结件 2/3，步 2 回执 §6 格 3）。
   冻结件 2：摘要按「发送帽」在设备侧做码点硬截二次截断，不依赖 APK 自觉；
   冻结件 3：右栏独立计数，累积 12 次 DU 局部刷做一次右栏区域 GC16 清残影。
   占位三态「等待连接／摘要生成中…／本次未生成」与侧栏 padding 亦属本步。
   计数器独立于正文 s_du_count（R0 明定「右栏独立计数」，不复用、不改其账）。

   修复轮二轮（现场第二轮回执）：发送帽 56 → 192 字；字号不再写死 21 px，改为
   在内框里取「整段放得下的最大字号」（18..34），短摘要自动放大、长摘要自动收到
   能容下的字号，既不留白也不挤成一条线。几何仍守 70/30，改档只动本节常量。

   修复轮三轮（07:37 篇幅扩充令）：帽 192 → 250 字，下限 18 → 22 px。下限抬到
   22 是硬要求——右栏宁可末尾截断补省略号，也不许为了塞下更多字把字号压到
   看不清。实测几何（内框 301x618，34 px 时 131 汉字排 12 行）反推：22 px 每行
   约 17 字，250 字约 15 行、行高约 33 px，总计约 495 px，装得下；正常输出
   180~220 字会落在 22~24 px，比二轮的 34 px 略收但仍远高于「蚂蚁」。 */
#define READER_COMPANION_SUMMARY_CAP 250
#define READER_COMPANION_TEXT_MAX_PX 34
#define READER_COMPANION_TEXT_MIN_PX 22
#define READER_COMPANION_GC16_EVERY 12u
#define READER_COMPANION_PAD 14
static char s_companion_summary[READER_COMPANION_SUMMARY_CAP * 4 + 1];
static uint8_t s_companion_summary_idx, s_companion_summary_total;
static bool s_companion_summary_has;
static unsigned s_companion_du_count;
static char s_book_title[128];
static char s_chapter_heading_title[128], s_chapter_heading_label[32];
static size_t s_chapter_lead_skip;
static unsigned s_chapter_lead_height;
// 横排题头该占的行高，与当前模式无关地记着：首块是插图时它是 0，
// 切回横排要用它，而不是拿竖排留下的 0 去算。
// The row height a horizontal heading should take, remembered independently of the current mode:
// it is zero when the chapter opens with an illustration, and switching back must use it rather
// than the zero a vertical heading left behind.
static unsigned s_chapter_lead_row_height;
// 竖排首页为题头预留的列宽；横排为 0。/ Columns reserved for a vertical heading; 0 horizontally.
static unsigned s_chapter_lead_width;
static char s_font_path[192];
static char* s_text;
bool app_book_reader_body_visible(void) {
    return s_view == READING && s_text && s_reader_panel == READER_PANEL_NONE && !s_clear_confirm;
}
static blk_t* s_blocks;
// 书内字体的 run 表与块表同生共死：都来自同一次 html_text 解析，排版直接借用。
// The face run table lives and dies with the block table: both come from the same html_text
// parse, and the layout borrows them.
static html_run_t* s_runs;
static size_t s_run_count;
// 上一次排版用的书内字体开关：字体页改过之后，回阅读页要重排当前章才看得出来。
// The embedded-face setting the current layout was built with: after the font page changes
// it, the reader must re-typeset the chapter before the change is visible.
static bool s_book_fonts_applied;
// 字体开关改了之后要重排当前章，但那次重排不能在触摸回调里做：栈太深。
// The font switch needs the current chapter re-typeset, but not from the touch callback: the
// stack there is too deep.
static bool s_fonts_reload_pending;
static char** s_images;
static size_t s_image_count;
// PR #7 的当前页图片集合：只保留显示页的灰阶，不限制为八张。
// PR #7's current-page image set retains visible grayscale only, without an eight-image limit.
typedef struct { uint8_t *gray; int width, height; } reader_image_t;
static reader_image_t *s_page_images;
static int s_page_image_count;
static size_t s_page_images_for = SIZE_MAX;
static uint32_t s_page_images_generation;
static size_t s_block_count;
static size_t s_text_len, s_chapter, s_page;

/// M3 情景释义：点词时的定位快照（页 + 词在整章字节流中的区间）。点卡上的
/// 「情景解读 ›」时 s_text/s_page 必须仍指向同一页，否则放弃（ctx_fail）。
/// 上下文缓冲 static：1024B 放栈会重演 gen26.6.4 的栈教训（主任务栈 12KB）。
/// M3 scene lookup: the tap snapshot (page + word byte range in the chapter).
/// When "情景解读 ›" is tapped, s_text/s_page must still point at the same page,
/// else the request is abandoned (ctx_fail). The 1024B buffer is static - putting
/// it on the stack repeats the gen26.6.4 lesson (12KB main-task stack).
static size_t s_scene_page;
static size_t s_scene_start, s_scene_end;
static bool s_scene_valid;
static char s_scene_ctx[PUSH_SCENE_CTX_MAX + 1];

/// M3-B 情景请求待命态：0x1A 发出后等 0x08 回流。**关联键 = 词 + 发起页 +
/// 期限**——0x08 帧里没有「这是情景答案」的标记（协议零改动），设备只能靠这三
/// 者互认；任何一项对不上就不认，宁可不做。状态机本体在 scene_req.c（纯函数，
/// 宿主可逐条断言）。
/// 引用条直接复用 s_scene_ctx（同一份上下文，不另开缓冲）；新点词会把
/// s_scene_valid 清零，此处同步清 armed，旧请求自然作废。
/// M3-B scene-request pending state: armed when 0x1A goes out, waiting for 0x08.
/// The correlation key is word + originating page + deadline - the 0x08 frame
/// carries no "this is a scene answer" mark (zero protocol change), so the device
/// can only match on those three. The state machine lives in scene_req.c (pure,
/// asserted line by line on the host). The quote strip reuses s_scene_ctx (the very
/// same context, no second buffer); a new tap clears s_scene_valid and disarms
/// here, so the stale request dies by itself.
static scene_req_t s_scene_req;
/// 等 0x08 的时限：与 APK 的请求超时（15s）同量级，略长，让 APK 先放弃。
/// The 0x08 wait: same order as the APK's request timeout (15s) and slightly
/// longer, so the APK gives up first.
#define SCENE_REQ_TIMEOUT_MS 16000

/// 26.11.3-C2（审计第三刀：设备侧等待窗）：**整段解析的窗口不能被词卡的窗口管着**。
/// 词卡是 8-10 s 生成，16 s 够；段落是 2,000+ 字、40-70 s 生成 + 50 帧回传，仍在
/// 16 s 上等的后果是**设备先超时销号、0x08 回来无人认领**——用户等到一篇完整解析却
/// 落在空处，比慢更糟。窗口按「生成上界 + 回传上界 + 余量」算：70 + 3 + 17 = 90 s。
/// 90 s 与 APK 的 120 s 读取超时同向（设备先放弃，杜绝僵尸态）。
/// 26.11.3-C2 (audit knife 3, the device-side wait window): the passage window must
/// not be governed by the word-card window. A word answer takes 8-10 s, so 16 s is
/// fine; a passage is 2,000+ characters - 40-70 s to generate plus 50 frames back -
/// and waiting 16 s means the DEVICE expires first and the returning 0x08 is claimed
/// by nobody. The window is generation + return + slack: 70 + 3 + 17 = 90 s, which
/// sits under the APK's 120 s read timeout so the device still gives up first.
#define EXCERPT_REQ_TIMEOUT_MS 90000

static size_t s_selected_toc = SIZE_MAX;
static bool s_toc_jump_open, s_toc_jump_drag;
static int s_toc_jump_percent;
static size_t s_jump_offset = SIZE_MAX, s_jump_page;
static uint32_t s_file_size;
static int s_px, s_margin, s_line_spacing, s_turns, s_unsaved;
static int64_t s_poll_ms, s_last_turn_ms;
static int64_t s_stats_last_ms, s_stats_activity_ms;
static uint32_t s_stats_pending_ms, s_stats_pending_turns;
static uint32_t s_session_read_ms, s_session_turns;
static int s_font_page;
static int s_reader_slider = -1;
static bool s_reader_slider_endpoint;
static int s_reader_preview_px, s_reader_preview_margin, s_reader_preview_line, s_reader_preview_para;
static int s_reader_preview_tracking;
static uint32_t s_recent_days[30];
static bool s_recent_days_valid;
static char s_reader_notice[64];
static int64_t s_reader_notice_until;
// 26.11.3-C2 等待期提示：整段解析发出后补的第二条气泡。「AI 已发」只回答
// 「发出去了吗」，2 s 后自然消失；40-70 s 的安静期需要一句「还在算」，
// 否则用户以为死机。0 = 无待发。
// 26.11.3-C2 wait notice: the second toast after a passage request. "AI 已发"
// answers "did it go out" and expires in 2 s; the 40-70 s of silence needs one
// "still working" line or the wait reads as a hang. 0 = nothing scheduled.
static int64_t s_ai_wait_notice_at_ms;
static int s_bookmark_page;
static bool s_bookmark_edit, s_bookmark_delete_confirm, s_bookmark_delete_error;
static uint32_t s_bookmark_selected;
typedef struct { char path[288]; } reader_background_item_t;
static reader_background_item_t *s_backgrounds;
static int s_background_count, s_background_selected, s_background_page;
#define READER_BACKGROUND_ROWS 7
static bool s_background_ready;
static char s_background_notice[96];
static char s_background_preview_notice[96];
static bool s_background_prepare_pending;
static EpdRect s_area;
static enum EpdDrawMode s_mode = MODE_GL16;
static bool s_input_settle;
static bool s_reader_cleanup;
// gen24.4：推送收官的开书要一次整屏 GC16 打底。BLE 推流从武装页切进阅读页，
// 两页墨迹差异大（武装页满是灰底卡片与边框），GL16 差分第一页会把旧墨留着——
// 真机 gen24.3 验收报「推送完成后打开第一页也有残影」。只由
// app_book_request_open_clean 置位、present 消费一次；书架/首页开书沿用既有
// 节拍，不抢这次全刷。/ Gen24.4: the push-completed open wants one full-screen
// GC16 underneath. The BLE push finale switches from the arm page (full of gray
// cards and borders) into the reader, and a GL16 differential first page keeps
// the old ink - gen24.3 hardware acceptance reported "the first page after a
// completed push also has ghosting". Set only by app_book_request_open_clean
// and consumed once by present; shelf and home opens keep their cadence.
static bool s_open_full_once;
// gen25：让路收官留下的「推送内容首开需 GC16 打底」标记，消费一次即止。
// Gen25: the "pushed text's first open needs a GC16 base" flag left by the yield
// finale; consumed once.
static bool s_push_first_open_once;
static bool s_reader_footer_pending;
// ---- 31b 第 2 条 B′：掉盘阅读态 -------------------------------------------
// 拔卡时不再 `s_view = SHELF`（那是「跳书架」），而是留在阅读器、按**字节偏移**
// 重排当前页：正文与已解码位图都在 RAM，卡没了照样能读这一章。
// 跨章（任何要读卡的动作）一律被拦住，给「存储卡已移除」——静默失败比拒绝更坏。
// 26.9.31a 收口里被挂账的「拔卡瞬间跳书架」就在这里结清。
//
// B′: the reader no longer bounces to the shelf on card removal. It stays put and
// re-typesets the current page from a **byte offset**; body text and decoded bitmaps
// live in RAM, so this chapter stays readable without the card. Anything that needs
// the card (any chapter load) is refused with "存储卡已移除" - a silent failure is worse
// than an honest refusal.
static bool s_media_offline;
// 掉盘快照：`book_close()` 会 memset 书籍元数据，章数与字节基准必须在这之前留下。
// Offline snapshot: book_close() zeroes the book metadata, so chapter count and byte
// bases must be captured before the handle is closed.
static size_t s_off_chapters;
static uint32_t s_off_total_bytes;
static uint32_t s_off_chapter_offset;
// 页脚文案（B′ 条件 2 的固定位）：掉盘时左端恒为这两条之一，不随章节名让位。
// Footer copy (B′ condition 2, fixed slot): the left end is always one of these two
// while offline; it never yields to the chapter title.
#define READER_OFFLINE_NOTE "TF 卡已移除"
#define READER_OFFLINE_TAIL "存储卡已移除"
static bool s_water_turn_pending;
static e0470_turn_dir_t s_water_turn_dir;
static int s_pressed_control = -1;
static bool s_shelf_feedback_pending;
static bool s_cover_batch_present_pending;
static bool s_shelf_page_pending;
static book_reader_tap_t s_reader_tap;
static unsigned s_reader_key_pick;
static char s_reader_key_error[64];
static int64_t s_du_ms;
static unsigned s_du_count;
static EpdRect s_du_area;
static SemaphoreHandle_t s_draw_lock, s_prep_done;
static TaskHandle_t s_prep_task;
static atomic_bool s_prep_stop;
static book_layout_draw_cursor_t s_prep_cursor;
static uint32_t s_next_generation;
static uint8_t* s_next_fb;
static int s_next_page = -1, s_prep_page = -1;
// gen25（高水位采样，用户裁定「要」）：book_prep 任务栈的 min-so-far 剩余量。
// 本 port 的 portSTACK_TYPE=uint8_t，故 FreeRTOS 的 HWM 计数单位=1 字节
// （不是 4 B——gen25 曾误读为「4 B/word」并据此错下「9.2K 已占用→不降栈」；
// gen26 纠错：9444 是**最低剩余字节**，峰值=12288-9444≈2.8K，故 12K→6K 降栈，
// 省 6K 内部堆，峰值仍留 ~3.3K 余量）。任务在**自删前**打这一行，覆盖整个
// 生命周期；只在外部删时打会漏掉中途低谷。
// Gen25 (high-water sampling, the user ruled "yes"): book_prep's stack
// min-so-far free. This port defines portSTACK_TYPE=uint8_t, so FreeRTOS's HWM
// count unit is 1 byte (NOT 4 B - gen25 misread it as "4 B/word" and wrongly
// concluded "9.2K used -> keep 12K"; gen26 corrects: 9444 is the *minimum free
// bytes*, peak = 12288-9444 ~= 2.8K, so stack 12K->6K, saving 6K internal heap
// with ~3.3K peak headroom). The task logs the line **before self-deleting** so
// the whole lifetime is covered; logging only from the outside misses troughs.
static UBaseType_t s_prep_hwm_words;
// book_prep 任务栈深（FreeRTOS 以 StackType_t 为单位，本 port=1 字节）：6*1024=6 KiB。
// 峰值实测 ~2.8K，留 ~3.3K 余量（gen26 由 12K 降下）。
// book_prep stack depth (FreeRTOS unit = StackType_t = 1 byte here): 6*1024 = 6 KiB.
// Measured peak ~2.8K leaves ~3.3K headroom (gen26 cut from 12K).
#define BOOK_PREP_STACK_SZ (6 * 1024)
// 停机等待上限：一次 prep 的工作单元是「预绘一页」的行循环（每行都查一次
// s_prep_stop），真机一页预绘在百毫秒量级，这里给 1s——约 3-10 倍余量，足以
// 覆盖最大书最长 prep。关键是**超时不强删**（见 prep_task_request_stop），
// 所以这个值只影响「本次能否立刻归还 6K」，不影响安全。
// Stop-wait ceiling: one prep unit is the row loop that pre-renders a page
// (every row re-checks s_prep_stop); a page pre-render runs in the hundreds of
// milliseconds on hardware, so 1 s leaves roughly 3-10x headroom. What matters
// is that a timeout never force-deletes (see prep_task_request_stop), so this
// only decides whether the 6K comes back right now, never safety.
#define BOOK_PREP_STOP_WAIT_MS 1000

#define s_toolbar (s_reader_panel != READER_PANEL_NONE)

static void render(app_ctx_t* ctx, uint8_t* fb);
static void draw_reader_panel(uint8_t* fb);
static EpdRect reader_slider_rect(int slider);
// M2 Step9：app_book_ai_def_ready 的页脚快照要用视觉页号，声明提前到使用点
// 之前（定义在后部）。/ M2 Step9: the footer snapshot in app_book_ai_def_ready
// needs the visual page number; declared ahead of its use (defined later).
static size_t reader_visual_index(size_t logical);
static void scan_shelf(app_ctx_t* ctx);
static void copy_text(char* dst, size_t cap, const char* src);
static void fit_text(char* text, int px, int width);
static void free_book(void);
static void save_progress(void);
// B′：load_chapter_at 的掉盘闸在函数定义之前使用，声明前移（定义在阅读器尾部）。
// B′: the offline gate in load_chapter_at uses this before its definition.
static void reader_offline_refuse(void);
// commit 3 的读者释放要在 lock_draw/unlock_draw 定义之前试取绘制锁（不阻塞）。
// Commit 3's reader release try-takes the draw lock before those helpers are defined.
static void unlock_draw(void);
static void prep_task_request_stop(const char *why); // gen25：让路链要提前停机 / gen25: the yield chain needs to stop prep early
static void invalidate_prep(void);
// 26.11.7 S2：账在 present() 里结，定义在阅读器尾部（计数逻辑与翻页语义同处）。
// 26.11.7 S2: the ledger settles in present(); the definition lives at the reader's tail.
static bool reader_note_turn(void);
static void prepare_inline_image(void);
static void release_page_images(void);
static void sort_shelf(app_ctx_t* ctx);
static void prepare_covers(app_ctx_t* ctx);
static app_redraw_t toggle_reader_fullscreen(app_ctx_t* ctx);
static app_redraw_t toggle_reader_landscape(app_ctx_t* ctx);
static app_redraw_t resize_text(app_ctx_t* ctx, int dir);
/* gen30 伴读案 · 步 3：伴读态判据与门槛判据定义在文件中部（步 2 落的
   s_reader_companion 处），听书面板绘制更早，故在此前置声明；两个 pending 由
   on_tick 同批原子消费，触摸回调只置位（重排不落深栈）。
   gen30 read-along step 3: the companion predicate and the 0x19 gate predicate
   are defined mid-file (next to step 2's state), while the TTS panel draws
   earlier - hence the forward declarations. The two pending flags are consumed
   atomically in on_tick; a touch callback only sets them (no reflow in a deep stack). */
static bool reader_companion_active(void);
static bool reader_tts_active(void);
bool app_book_request_open(const char* path) {
    if (!path || !path[0] || strnlen(path, sizeof(s_requested_open)) >= sizeof(s_requested_open)) return false;
    copy_text(s_requested_open, sizeof(s_requested_open), path);
    s_requested_open_home = false;
    s_requested_fullscreen = true;
    // 26.11.5：这里**不**无条件置 s_open_full_once。开书首屏的整屏 GC16 由 present 的
    // 视图边沿判据负责（书架/文件页/首页卡/推送五条入口都跨视图，边沿一定成立）；
    // 在这里再置一次只会让「同视图重绘」也白付一次全刷，而边沿判据比「每次开书都多刷」
    // 更准。gen25 的推送首开标记照旧按条件消费。
    // 26.11.5: no unconditional s_open_full_once here - present's view-edge rule already
    // covers every entry (all five cross a view), and a blanket flag would charge an extra
    // full refresh to same-view repaints. The gen25 push first-open flag is unchanged.
    // gen25：让路收官留下的推送内容首开标记在这里消费——B 的第一次手动打开
    // 也享受一次整屏 GC16，避免「被让路推来的书第一次打开有残影」。
    // Gen25: the yield finale's first-open flag is consumed here - B's first
    // manual open also gets one screen-wide GC16, so a push that arrived via the
    // yield does not show ghosting on its first open.
    if (s_push_first_open_once && !strcmp(path, PUSH_TEXT_PATH)) {
        s_push_first_open_once = false;
        s_open_full_once = true;
    }
    return true;
}
// gen24.4（渲染发灰根治）：带一次整屏 GC16 的开书。供推送收官使用——武装页
// 切阅读页时旧底必须整屏擦净，第一页才不会有残影；消费点在 present 的
// update_display_full 分支，一次即止。/ Gen24.4 (the gray-render root fix): an
// open carrying one full-screen GC16. For the push finale - switching from the
// arm page to the reader must wipe the old base screen-wide, or the first page
// keeps the ghosting; consumed in present's update_display_full branch, once.
bool app_book_request_open_clean(const char* path) {
    if (!app_book_request_open(path)) return false;
    s_open_full_once = true;
    return true;
}
// gen25：让路场景下被留在书架的推送内容，它的「首开 GC16 打底」要单独保证。
// 非让路推送走收官自动开书（clean 打底，gen24.4）；让路推送改走「恢复原书」，
// 推送内容 B 留在书架等用户手动打开——若不管，B 第一次打开就是差分，会带残影。
// 这里由让路收官置位，下一次打开 B 时消费一次。
// Gen25: the pushed text left on the shelf by a yield needs its own "first open
// gets a GC16 base". A normal push opens it automatically at the finale (clean,
// gen24.4); a yielded push restores the book instead, leaving B on the shelf for
// a manual open - unhandled, B's first open would be a differential with
// ghosting. The yield finale sets this flag and the next open of B consumes it.
void app_book_mark_push_first_open(void) { s_push_first_open_once = true; }
bool app_book_request_open_from_home(const char* path) {
    if (!app_book_request_open(path)) return false;
    s_requested_open_home = true;
    return true;
}
bool app_book_request_resume(const char *path, bool fullscreen) {
    if (!app_book_request_open_from_home(path)) return false;
    s_requested_fullscreen = fullscreen;
    // gen25.1（resume 首屏 GC16 打底）：resume 的三种来路——首页「继续阅读」卡、
    // 开机恢复（boot recovery）、让路 restore 恢复原书——全部从旧屏差分进阅读页，
    // 首屏走 APP_PAGE_REFRESH_MODE(GL16) 差分，旧墨（首页大卡/底栏/传书页卡片）
    // 会留在第一页上；真机报「最近阅读点开有残影」。与 gen24.4 open_clean 同族
    // 同一标志：present 的 update_display_full 分支消费一次即止，后续翻页节拍
    // 零变化（gen24.4 红线不破）。开书失败时标志残留的最坏后果是下次开书多一次
    // 整屏 GC16，与 open_clean 同款语义，不单独造轮子。
    // Gen25.1 (resume first screen GC16 base): all three resume sources - the
    // home "continue reading" card, boot recovery, and the yield restore of the
    // original book - enter the reader differentially, and the first screen's
    // APP_PAGE_REFRESH_MODE (GL16) differential keeps the old ink (home card /
    // nav bar / transfer cards); hardware reported ghosting when opening recent.
    // Same flag family as gen24.4's open_clean: consumed once by present's
    // update_display_full branch; later turns keep their cadence (the gen24.4
    // red line holds). If the open fails the flag's worst case is one extra
    // screen-wide GC16 on the next open - same semantics as open_clean, no new
    // mechanism.
    s_open_full_once = true;
    return true;
}

/* ---- gen25 让路链（yield / restore）---- */
// 预检不过时把当前这本「临时卸下」，把它的 6K prep 栈与整批解析缓冲还给内部
// 堆，让 BLE 闸门过得去；推送收官后再自动重开、回到原位置（位置走既有
// book_progress 单一真源，不引入第二套机制）。
// Yield the current book away when the precheck fails, handing its 6K prep stack
// and the whole parse buffer set back to the internal heap so the BLE gate
// passes; the push finale then reopens it at the saved position (position uses
// the existing book_progress single source - no second mechanism).
static bool s_book_yielded;
static char s_yield_path[BOOK_STORE_PATH_MAX];

bool app_book_yield_active(void) { return s_book_yielded; }

// gen25 接线裁定 #1（异步等待形态）：把停机拆成「非阻塞请求」与「查询是否已停」
// 两步，让调用方（传书页）能**先出提示、再分拍等**——同步等 done 最长 1000ms
// 会让按钮按下去 1 秒无 UI 变化，那正是「点了没反应」的死感。
// Gen25 wiring ruling #1 (async wait shape): split stopping into a non-blocking
// request and a "stopped?" query so the caller (the transfer page) can **show a
// hint first and then wait across ticks** - waiting for done synchronously for up
// to 1000 ms would leave the tap with a full second of no UI change, exactly the
// "tapped and nothing happened" dead feel.
void app_book_request_prep_stop(void) {
    if (!s_prep_task) return;
    atomic_store_explicit(&s_prep_stop, true, memory_order_release);
    xTaskNotifyGive(s_prep_task); // 只请求，不等 / request only, never wait
}
bool app_book_prep_stopped(void) { return !s_prep_task; }
bool app_book_has_open_book(void) { return s_text && s_path[0]; }

bool app_book_request_yield_for_push(void) {
    if (s_book_yielded) return true;             // 幂等：已卸过就不再卸 / idempotent
    if (!s_text || !s_path[0]) return true;      // 无书可卸 = 无需让路，直接就绪 / nothing open: nothing to yield, already ready
    // ① prep 停机（6K 栈归还）。/ Stop prep first (returns the 6K stack).
    prep_task_request_stop("yield");
    if (s_prep_task) return false;               // 超时未停 → 按失败处理，不原地空等 / still busy: report failure, never wait it out here
    // ② 无活跃渲染：拿得到绘制锁才算空闲。/ No rendering in flight: idle means we can take the draw lock.
    bool idle = false;
    if (s_draw_lock && xSemaphoreTake(s_draw_lock, 0) == pdTRUE) { xSemaphoreGive(s_draw_lock); idle = true; }
    else if (!s_draw_lock) idle = true;
    if (!idle) return false;
    // ③ 存位置 → 卸书（free_book 释放 text/blocks/runs/images/page_images 并
    //    book_close）。/ Save the position, then unload (free_book releases
    //    text/blocks/runs/images/page_images and calls book_close).
    save_progress();
    copy_text(s_yield_path, sizeof(s_yield_path), s_path);
    free_book();
    s_book_yielded = true;
    ESP_LOGI(TAG, "yield for push: unloaded, internal free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return true;
}

bool app_book_restore_after_push(void) {
    if (!s_book_yielded) return false;
    s_book_yielded = false;
    if (!s_yield_path[0]) return false;
    // 让路期间用户自己开了别的书 → 放弃恢复，不抢用户操作。
    // The user opened something else during the yield: give up rather than
    // override them.
    if (s_text && s_path[0]) { s_yield_path[0] = 0; return false; }
    // gen25.1：restore 复用 request_resume，因此恢复原书的首屏顺带获得一次
    // 整屏 GC16 打底（resume 真身置位 s_open_full_once），无需单独处理。
    // Gen25.1: restore reuses request_resume, so the restored book's first
    // screen gets the one-time GC16 base for free (the resume body sets
    // s_open_full_once) - no separate handling needed.
    bool ok = app_book_request_resume(s_yield_path, true);
    ESP_LOGI(TAG, "restore after push: %s (%s)", ok ? "requested" : "FAILED", s_yield_path);
    s_yield_path[0] = 0;
    return ok;
}
bool app_book_resume_context(char *path, size_t capacity, bool *fullscreen) {
    if (!s_text || !s_path[0] || s_view == SHELF || s_view == MANAGE || s_view == BULK ||
        !path || !fullscreen || strlen(s_path) >= capacity || s_save_failed) return false;
    copy_text(path, capacity, s_path);
    *fullscreen = s_reader_fullscreen;
    return true;
}
void app_book_request_manage(void) { s_requested_manage = true; }

/* ---- 26.9.30 commit 3：读者句柄释放（机制二）/ releasing the reader's handle ---- */
// 提交侧的三个动作是 rename(push.txt→push.bak)、rename(part→push.txt)、
// remove(push.bak)。读者若正读着 push.txt，book_txt_open() 留在 s_book.file 的
// FILE* 依然指向那一串簇；最后那步 remove 把簇还给 FAT，此后每次 fread 都落在
// 已释放的簇上——现场表现为乱码页、0x101、或直接崩。这是「机制二」，与提交段的
// 封送（commit 2）是两件事：封送只保证四拍不跑在 BLE 栈上，不保证读者脚下没句柄。
// The commit side runs rename(push.txt->push.bak), rename(part->push.txt) then
// remove(push.bak). A reader sitting on push.txt still holds the FILE* left in
// s_book.file; the last remove hands those clusters back to FAT, so every later
// fread reads freed clusters - garbage pages, 0x101, or a crash. That is
// mechanism #2, and it is separate from commit 2: offloading the four beats keeps
// them off the BLE stack, not out from under the reader's feet.
//
// 「只关句柄、保留阅读态」是假修复：book_close() 会把 s_book memset 成 0，
// 翻页立刻 fread(NULL) 变成半死阅读器。所以这里走**完整关书**（free_book()），
// 代价是阅读位置与页缓存一起没——反正随后的提交就是要把这本书换掉。
// "Close the handle but keep the reader state" is a fake fix: book_close()
// memsets s_book, so the next turn freads NULL and the reader half-dies. This
// goes through a full close (free_book()) instead; the reading position and page
// cache go with it, which is fine - the commit is about to replace this very book.

// noinline 的理由与 render_context.h 那条同族：这是「持有判据」的唯一出处，
// 内联进 release_hold 之后二进制里就没有这个符号了，送审按符号核对时判据与实现
// 之间的链会断（看着像没落码）。保留一个独立出口，代价是一条 3 字节的跳转。
// noinline for the same reason as render_context.h: this is the only home of the
// "does the reader hold this path" predicate, and inlining it into release_hold
// would erase the symbol - the review's symbol check would then see a chain with
// a missing link. One extra 3-byte jump is the price.
__attribute__((noinline)) bool app_book_holds_path(const char *path) {
    return path && path[0] && s_text && s_path[0] && strcmp(s_path, path) == 0;
}

uint32_t app_book_hold_busy(void) {
    if (!s_text || !s_path[0]) return 0;   // 无书 = 无占用 / no book, nothing held
    uint32_t busy = 0;
    // ① prep 任务：它的行循环每行都读 s_text；free_book() 只**请求**停机，超时不强删
    //    （prep_task_request_stop 的注释：强删会漏内存）。所以「它还在」就是硬阻。
    //    Bit 0: the prep task's row loop reads s_text every row; free_book() only
    //    *requests* a stop and never force-deletes, so "still alive" is a hard block.
    if (s_prep_task) busy |= BOOK_HOLD_PREP;
    // ② 绘制锁有人持有 = 有渲染在飞（与 lock_draw 的自检同源：非递归互斥量下
    //    「已有人持有」只有渲染这一种合法来源）。
    //    Bit 1: the draw lock has a holder = a render is in flight (same source of
    //    truth as lock_draw's self-check; a non-recursive mutex has one owner).
    if (s_draw_lock && xSemaphoreGetMutexHolder(s_draw_lock) != NULL) busy |= BOOK_HOLD_DRAW;
    // ③ 正文翻页/正文帧排期：free_book() 在释放 text 之前先清这两个标志，说明它们
    //    在书的生命周期内会被置位——置位期间排期队列里握着页偏移。
    //    Bit 2: free_book() clears both flags before releasing text, which proves
    //    they can be up during a book's life; while up, the queue holds page offsets.
    if (s_reader_turn_pending || s_reader_text_frame) busy |= BOOK_HOLD_TURN;
    // ④ 水墨翻页在飞：波形切换正在用屏缓冲，此时清场会把半帧留在屏上。
    //    Bit 3: a water-mode turn owns the panel buffer; clearing mid-wave leaves half a frame.
    if (s_water_turn_pending) busy |= BOOK_HOLD_WATER;
    // ⑤ 页脚刷新排期：页脚要重绘当前页文案，重绘时读 s_text。
    //    Bit 4: the footer repaint reads the current page's text.
    if (s_reader_footer_pending) busy |= BOOK_HOLD_FOOTER;
    // ⑥ 框选：选区偏移 (off_start/off_end) 直接索引 s_text，清场后失效。
    //    Bit 5: the selection's byte offsets index s_text directly.
    if (boxsel_active()) busy |= BOOK_HOLD_BOXSEL;
    // ⑦ 阅读面板：词典/详解覆盖层引用当前页文本与卡上词典，属于「看得见这本书」。
    //    Bit 6: the dict/detail overlay references this page and the card's dictionary.
    if (s_reader_panel != READER_PANEL_NONE) busy |= BOOK_HOLD_PANEL;
    return busy;
}

bool app_book_release_hold(const char *path) {
    if (!app_book_holds_path(path)) return true;   // 不持有 = 已无占用 / not held: nothing to release
    const uint32_t busy = app_book_hold_busy();
    // 硬阻只有两位：都是「别的执行体正在用 s_text」，清不掉，只能等它自己走完。
    // 这里**非阻塞**地请求停机就把本拍让出去——等 1 s 会把主循环钉住，
    // 与 gen25 接线裁定 #1（页面不能有「按下去 1 秒没反应」）同一条纪律。
    // Only two bits are hard blocks: other executors are using s_text and cannot be
    // cleared, only out-waited. The stop is *requested* non-blockingly and this
    // frame is handed back - waiting the 1 s here would pin the main loop, the same
    // discipline as gen25 wiring ruling #1.
    if (busy & BOOK_HOLD_PREP) {
        app_book_request_prep_stop();   // 只置位 + 唤醒，它自己走完 / flag + notify only
        ESP_LOGI(TAG, "hold: prep alive, stop requested; retry next frame");
        return false;
    }
    if (busy & BOOK_HOLD_DRAW) {
        ESP_LOGI(TAG, "hold: draw lock held; retry next frame");
        return false;
    }
    // 软项就地清：书都要换了，丢一帧、丢一次翻页、收一个面板都不算代价。
    // Soft bits are cleared in place: the book is being replaced, so a dropped
    // frame, a dropped turn and a closed panel cost nothing.
    boxsel_boundary_reset();
    if (s_reader_panel != READER_PANEL_NONE) dict_popup_reset();
    s_reader_panel = READER_PANEL_NONE;
    s_reader_slider = -1;
    s_reader_tap.pending = false;
    s_reader_turn_pending = s_reader_text_frame = false;
    s_water_turn_pending = false;
    s_reader_footer_pending = false;
    s_clear_confirm = s_batch_confirm = false;
    s_pressed_control = -1;
    // 试取而不是死等：位 1 的检查与取锁之间有竞态窗口（prep 可能刚抢到），
    // 拿不到就把本拍让出去，绝不 portMAX_DELAY 钉住主循环。
    // Try-take rather than wait: prep may have won the window between bit 1 and
    // here; giving the frame back beats pinning the loop on portMAX_DELAY.
    const bool locked = s_draw_lock != NULL;
    if (locked && xSemaphoreTake(s_draw_lock, 0) != pdTRUE) {
        ESP_LOGI(TAG, "hold: draw lock lost the race; retry next frame");
        return false;
    }
    save_progress();
    free_book();   // 完整关书：text/blocks/runs/images/page_images + book_close() 关句柄
    if (locked) unlock_draw();
    // 预绘缓冲与编辑封面都引用旧页内容，随书一起释放（book_on_exit 同款）。
    free(s_next_fb); s_next_fb = NULL;
    free(s_editor_cover); s_editor_cover = NULL;
    // 视图必须离开 READING：s_text 已经没了，任何正文绘制都会踩空。
    // 回书架后由提交成功侧的 ui_post_open 打开新书（中间可能闪一帧书架，可接受）。
    s_view = SHELF;
    s_shelf_cache_valid = false;   // push.txt 内容要换，标题/封面/进度重扫
    s_scan_pending = true;
    ESP_LOGI(TAG, "hold: reader released on %s (busy=0x%02x)", path,
             (unsigned)busy);
    return true;
}

static void draw_control(uint8_t* fb, EpdRect rect, const char* label, int id) {
    if (id == 112 && s_query[0]) {
        ui_fill_round_rect(fb, rect, UI_BTN_RADIUS, UI_GRAY_BLACK);
        ui_text_vc(fb, rect.x + rect.width / 2, rect.y + rect.height / 2,
                   UI_PX_BTN, label, EPD_DRAW_ALIGN_CENTER, true);
        return;
    }
    if (s_pressed_control == id) ui_draw_pressed_round_rect(fb, rect, UI_BTN_RADIUS);
    ui_draw_button(fb, rect, label, false);
}
// gen26.6.3 运行时兜底：s_draw_lock 由 xSemaphoreCreateMutex() 创建，是**非递归**
// 互斥量——同一任务二次 Take 即 portMAX_DELAY 永久阻塞（真机「打开详解整机挂死、
// 无 panic 转储」的根因）。取锁前自检持有者：命中就断言，同类死锁从「挂死、无转储、
// 只能断电」变成「当场断言 + 转储」，可定位、可预防。非递归语义下同任务重入永远
// 是 bug，没有合法场景，所以断言而非降级放行。
// gen26.6.3 runtime backstop: s_draw_lock comes from xSemaphoreCreateMutex() and is
// a NON-recursive mutex - a second Take by the same task blocks forever on
// portMAX_DELAY (the on-device "opening the detail view hangs the whole device, no
// panic dump" root cause). Self-check the holder before taking: on a hit, assert, so
// this class of deadlock turns from "hang, no dump, power-cycle only" into "assert
// right here with a dump" - locatable and preventable. Under non-recursive semantics a
// same-task re-entry is always a bug with no legitimate use, hence assert, not
// degrade-and-continue.
static void lock_draw(void) {
    if (!s_draw_lock) return;
    configASSERT(xSemaphoreGetMutexHolder(s_draw_lock) != (void*)xTaskGetCurrentTaskHandle());
    xSemaphoreTake(s_draw_lock, portMAX_DELAY);
}
static void unlock_draw(void) { if (s_draw_lock) xSemaphoreGive(s_draw_lock); }
static size_t fb_bytes(void) { return (size_t)epd_width() * epd_height() / 2; }
#define READER_FULLSCREEN_PROGRESS_TOP (UI_LOCK_HEIGHT - 12)
// 正文上方留出一致的呼吸空间，下沿靠近进度区；两种模式共用排版与刷新边界。
// Give the body a consistent top inset and extend it toward the progress strip in both modes.
static EpdRect reader_area(void) {
    const int top = s_reader_fullscreen ? (app_settings_reader_immersive() ? 0 : 80) : 160;
    // 全屏进度线单独差分，翻页时不让正文刷新反复驱动整条横线。
    // Keep the full-screen progress strip out of body updates so turns do not drive the whole bar.
    const int bottom = s_reader_fullscreen ? READER_FULLSCREEN_PROGRESS_TOP : UI_BAR_TOP + 8;
    return (EpdRect){0, top, UI_LOCK_WIDTH, bottom - top};
}
static EpdRect reader_fullscreen_progress_area(void) {
    return (EpdRect){0, READER_FULLSCREEN_PROGRESS_TOP,
                     UI_LOCK_WIDTH, UI_LOCK_HEIGHT - READER_FULLSCREEN_PROGRESS_TOP};
}
static EpdRect body_rect_for_tracking(int tracking_index) {
    int top = s_reader_fullscreen ? (app_settings_reader_immersive() ? 24 : 96) : 188;
    int margin = s_margin;
    // Leave only a narrow strip for the full-book progress line in full screen.
    int bottom = s_reader_fullscreen ? READER_FULLSCREEN_PROGRESS_TOP - 2 : UI_BAR_TOP + 8;
    int tracking = (tracking_index - 2) * 2;
    // 竖排把「边距」放到上下两端，左右只留一条窄边；整字收齐的是列高，不是行宽。
    // Vertical setting moves the margin setting to the top and bottom edges, keeps a narrow side
    // inset, and rounds the column height to whole glyphs instead of rounding a line's width.
    if (app_settings_book_vertical()) {
        EpdRect upright = {24, top + margin, UI_LOCK_WIDTH - 48, bottom - top - 2 * margin};
        EpdRect turned = book_layout_balanced_rect(
            (EpdRect){upright.y, upright.x, upright.height, upright.width}, s_px, tracking);
        return (EpdRect){turned.y, turned.x, turned.height, turned.width};
    }
    EpdRect outer = {margin, top, UI_LOCK_WIDTH - 2 * margin, bottom - top};
    return book_layout_balanced_rect(outer, s_px, tracking);
}
static EpdRect body_rect(void) {
    return body_rect_for_tracking(app_settings_book_tracking());
}
static int reader_margin_width(int margin, int px, int tracking_px) {
    EpdRect outer = {margin, 0, UI_LOCK_WIDTH - 2 * margin, 1};
    return book_layout_balanced_rect(outer, px, tracking_px).width;
}
static int reader_margin_levels(int px, int tracking_px,
                                int widths[BOOK_MARGIN_CHOICES], int values[BOOK_MARGIN_CHOICES]) {
    int count = 0;
    for (int i = 0; i < BOOK_MARGIN_CHOICES; ++i) {
        int value = BOOK_MARGIN_MIN + i * 6;
        int width = reader_margin_width(value, px, tracking_px);
        if (!count || widths[count-1] != width) {
            widths[count] = width;
            values[count++] = value;
        } else if (count > 1 && abs(value - 24) < abs(values[count-1] - 24)) {
            values[count-1] = value;
        }
    }
    // 同字格宽度只显示一个档位；极值仍到0/48，已有边距不因重画滑杆改写。
    // Show one stop per distinct column width, retaining the 0/48 extremes and existing saved margins.
    values[0] = BOOK_MARGIN_MIN;
    if (count > 1) values[count-1] = BOOK_MARGIN_MAX;
    return count;
}
static int reader_margin_level_for(int margin, int px, int tracking_px) {
    if (margin < BOOK_MARGIN_MIN) margin = BOOK_MARGIN_MIN;
    if (margin > BOOK_MARGIN_MAX) margin = BOOK_MARGIN_MAX;
    int widths[BOOK_MARGIN_CHOICES], values[BOOK_MARGIN_CHOICES];
    int count = reader_margin_levels(px, tracking_px, widths, values);
    int width = reader_margin_width(margin, px, tracking_px);
    for (int i = 0; i < count; ++i) if (width >= widths[i]) return i;
    return count - 1;
}
static int reader_margin_for_level(int level, int current, int px, int tracking_px) {
    int widths[BOOK_MARGIN_CHOICES], values[BOOK_MARGIN_CHOICES];
    int count = reader_margin_levels(px, tracking_px, widths, values);
    if (level < 0) level = 0;
    if (level >= count) level = count - 1;
    return level == reader_margin_level_for(current, px, tracking_px) ? current : values[level];
}
static EpdRect progress_rect(void) {
    // 阅读页没有右侧菜单按钮；进度信息应和页眉分隔线一样铺满内容宽度。
    // The reader has no right-hand menu button; the footer spans the full content width.
    return (EpdRect){36, UI_BAR_TOP, UI_LOCK_WIDTH - 72, UI_BAR_H};
}
static int shelf_rows(void) { return s_view == SHELF && app_settings_shelf_style() == 5 ? 4 : BOOK_GRID_ROWS; }
#define SHELF_BOOK_LIFT_PX 16
#define SHELF_LIST_BOUNCE_PX 8
#define SHELF_FAST_COVER_WIDTH 164
#define SHELF_FAST_COVER_HEIGHT 214
#define SHELF_FAST_COVER_STRIDE ((SHELF_FAST_COVER_WIDTH + 7) / 8)
#define SHELF_FAST_COVER_BYTES (SHELF_FAST_COVER_STRIDE * SHELF_FAST_COVER_HEIGHT)
#define SHELF_FAST_CACHE_RESERVE (512u * 1024u)
#define SHELF_GUARD_CACHE_BYTES (644u * 90u / 2u)
typedef struct { EpdRect rect; int rotation, contrast; bool binary, valid; } shelf_guard_key_t;
static uint8_t *s_guard_cache;
static shelf_guard_key_t s_guard_keys[3];
static bool s_guard_cache_attempted;
static void release_guard_cache(void) {
    free(s_guard_cache); s_guard_cache=NULL; s_guard_cache_attempted=false;
    memset(s_guard_keys,0,sizeof(s_guard_keys));
}
// 缓存精确的输入和成品；逐行比对底图，任何封面、位置或模式变化均重绘。
// Cache exact inputs and finished pixels; compare every input row so art, position and mode changes redraw.
static void draw_shelf_guard(uint8_t *fb,EpdRect rect,int slot) {
    const bool binary=s_view==SHELF&&app_settings_main_fast_refresh();
    EpdRect p=ui_rotate_rect_to_fb(rect);
    const bool fits=slot>=0&&slot<3&&!(p.x&1)&&!(p.width&1)&&p.x>=0&&p.y>=0&&
        p.x+p.width<=epd_width()&&p.y+p.height<=epd_height()&&
        (size_t)p.width*p.height/2<=SHELF_GUARD_CACHE_BYTES;
    if(fits&&!s_guard_cache&&!s_guard_cache_attempted) {
        s_guard_cache_attempted=true;
        size_t bytes=6u*SHELF_GUARD_CACHE_BYTES;
        if(heap_caps_get_free_size(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)>=bytes+2u*1024u*1024u)
            s_guard_cache=heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    }
    if(!fits||!s_guard_cache){ui_draw_acrylic_guard(fb,rect);if(binary)ui_acrylic_bw_rect(fb,rect);return;}
    shelf_guard_key_t *key=&s_guard_keys[slot];
    size_t stride=(size_t)epd_width()/2,span=(size_t)p.width/2;
    uint8_t *input=s_guard_cache+2u*slot*SHELF_GUARD_CACHE_BYTES,*output=input+SHELF_GUARD_CACHE_BYTES;
    bool same=key->valid&&key->rotation==epd_get_rotation()&&key->contrast==app_settings_system_contrast()&&
        key->binary==binary&&!memcmp(&key->rect,&rect,sizeof(rect));
    for(int y=0;same&&y<p.height;++y)
        same=!memcmp(fb+(size_t)(p.y+y)*stride+p.x/2,input+(size_t)y*span,span);
    if(!same) {
        key->valid=false;
        for(int y=0;y<p.height;++y)memcpy(input+(size_t)y*span,fb+(size_t)(p.y+y)*stride+p.x/2,span);
        ui_draw_acrylic_guard(fb,rect);if(binary)ui_acrylic_bw_rect(fb,rect);
        for(int y=0;y<p.height;++y)memcpy(output+(size_t)y*span,fb+(size_t)(p.y+y)*stride+p.x/2,span);
        *key=(shelf_guard_key_t){.rect=rect,.rotation=epd_get_rotation(),.contrast=app_settings_system_contrast(),.binary=binary,.valid=true};
    } else for(int y=0;y<p.height;++y)memcpy(fb+(size_t)(p.y+y)*stride+p.x/2,output+(size_t)y*span,span);
}
static EpdRect row_rect(int row) {
    if (s_view == SHELF && app_settings_shelf_style() == 5)
        return (EpdRect){36, 184 + row * 197, 612, 197};
    if (s_view != BULK) {
        int col = row % 3, line = row / 3;
        int cell = (ui_content_width() - 2 * UI_GAP) / 3;
        return (EpdRect){UI_MARGIN + col * (cell + UI_GAP), (s_view == SHELF ? 192 : 224) + line * 282, cell, 260};
    }
    return (EpdRect){UI_MARGIN, 308 + row * (UI_BTN_H + UI_GAP), ui_content_width(), UI_BTN_H};
}
static EpdRect shelf_cover_image(int row) {
    EpdRect card = row_rect(row);
    if (s_view != SHELF || app_settings_shelf_style() != 5)
        return (EpdRect){card.x + (card.width - 164) / 2, card.y, 164, 214};
    // 列表所有封面统一尺寸；原图按比例铺满并居中裁切。/ Uniform list boxes use aspect fill and center crop.
    return (EpdRect){71, card.y + (197-156)/2, 120, 156};
}
static unsigned s_cover_generation;
static void invalidate_covers(void) {
    book_cover_jobs_cancel(s_cover_generation);s_cover_generation=0;
    release_guard_cache();
    s_cover_pending_mask = s_cover_decode_mask = s_cover_warm_mask = 0;
    s_cover_dirty_mask = s_cover_present_mask = 0;
    for (int i = 0; i < BOOK_ROWS; ++i) {
        free(s_covers[i].gray);
        free(s_covers[i].fast_bits);
        s_covers[i].gray = NULL;
        s_covers[i].fast_bits = NULL;
        s_covers[i].index = -1;
    }
}
static void release_fast_covers(void) {
    for (int i=0;i<BOOK_ROWS;++i) {
        free(s_covers[i].fast_bits);
        s_covers[i].fast_bits=NULL;
    }
}
static bool fast_cover_matches(int row, EpdRect image) {
    return s_covers[row].fast_bits && s_covers[row].fast_contrast==app_settings_system_contrast() &&
        s_covers[row].fast_phase==((image.x&3)|((image.y&3)<<2));
}
static bool cover_favorite_needs_white_edge(const uint8_t *gray, EpdRect image, unsigned sw, unsigned sh);
static void prepare_fast_covers(void) {
    // 转换成功即释放灰阶；不释放仍有效的黑白缓存，避免重复切页重新解码。
    // Release gray immediately after conversion, retaining valid packed artwork across tab changes.
    if (s_view != SHELF || !app_settings_main_fast_refresh()) { release_fast_covers(); return; }
    for (int row = 0; row < BOOK_ROWS; ++row) {
        if (!s_covers[row].gray) continue;
        EpdRect image = shelf_cover_image(row);
        if (row < shelf_rows() && row < BOOK_GRID_ROWS && !fast_cover_matches(row, image)) {
            free(s_covers[row].fast_bits); s_covers[row].fast_bits = NULL;
            // 转换后回收的灰阶空间也计入保留量；内存不足时显示标题，不长期保留灰阶副本。
            // Count reclaimed gray toward the reserve; on OOM show a title rather than retain a gray duplicate.
            size_t free_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) + BOOK_COVER_W * BOOK_COVER_H;
            uint8_t *bits = free_after >= SHELF_FAST_CACHE_RESERVE + SHELF_FAST_COVER_BYTES
                ? heap_caps_calloc(1, SHELF_FAST_COVER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
            if (bits) {
                unsigned sw = app_settings_shelf_style() == 5 ? s_covers[row].width : BOOK_COVER_W;
                unsigned sh = app_settings_shelf_style() == 5 ? s_covers[row].height : BOOK_COVER_H;
                book_crop_t crop = book_cover_crop(sw, sh, image.width, image.height);
                unsigned sample_x[SHELF_FAST_COVER_WIDTH];
                uint8_t tones[256];
                for(int x=0;x<image.width;++x)sample_x[x]=crop.x+(uint64_t)(unsigned)x*crop.width/(unsigned)image.width;
                for(unsigned v=0;v<256;++v)tones[v]=(ui_contrast_gray((uint8_t)v)>>4)*17u;
                for (int y = 0; y < image.height; ++y) {
                    const unsigned sy = crop.y + (uint64_t)(unsigned)y * crop.height / (unsigned)image.height;
                    for (int x = 0; x < image.width; ++x) {
                        const unsigned sx = sample_x[x];
                        uint8_t tone = tones[s_covers[row].gray[sy * sw + sx]];
                        if (ui_image_dither_cover_bw(tone, image.x + x, image.y + y))
                            bits[y * SHELF_FAST_COVER_STRIDE + x / 8] |= (uint8_t)(1u << (x & 7));
                    }
                }
                s_covers[row].fast_white_edge = cover_favorite_needs_white_edge(s_covers[row].gray, image, sw, sh);
                s_covers[row].fast_bits = bits;
                s_covers[row].fast_contrast = app_settings_system_contrast();
                s_covers[row].fast_phase = (image.x & 3) | ((image.y & 3) << 2);
            }
        }
        free(s_covers[row].gray); s_covers[row].gray = NULL;
    }
}
void app_book_cover_mode_changed(void) { invalidate_covers(); }

// gen26 AI 释义卡：阅读态接住手机回流（0x07）或预取缓存命中的释义，暂存后直接
// 走 READER_PANEL_DICT 同一绘制/命中/保存链路。返回是否真的改了卡面（主循环据此
// 决定是否呈现）。/ gen26 AI definition card: while reading, take the
// phone-returned (0x07) or prefetch-cache-hit definition and ride the same
// READER_PANEL_DICT draw/tap/save chain. Returns whether the card actually
// changed (the main loop uses it to decide whether to present).
//
// gen26.3 ④ 双门（本轮最需小心的交互，两个门按词比对、语义分开）：
// 门 A（原语义保留）：无面板 —— 直接开卡；有**其他**面板（用户手开的词典卡、
//   设置页等）—— 不抢焦点，释义丢弃（它已在预取缓存里，下次点同一词秒出）。
// 门 B（新）：开着的正是**本词的「AI 查询中…」卡** —— 原地更新为释义，面板不变，
//   差分局部刷新，不二次全刷打底。不同词不更新（卡片已翻到别的词，门 A 照旧）。
// gen26.3 double gate (the round's most delicate interaction - two gates, both
// compare by word, semantics kept apart):
// gate A (original semantics kept): no panel - open the card; any OTHER panel
//   (a user-opened dict card, a settings page, ...) - never steal focus, the
//   def is dropped (it is already in the prefetch cache; the next tap on the
//   same word shows it instantly).
// gate B (new): the open card is THIS word's "AI querying..." card - update it
//   in place with the panel unchanged: a differential local refresh, no second
//   full repaint. A different word never updates (the card moved on to another
//   word; gate A still applies).
// M3-B：情景答案回流。**必须在 app_book_ai_def_ready 之前**被调用——0x08 帧
// 没有「这是情景答案」的标记（协议零改动），先由待命态认领；认领失败才回落到
// 普通释义卡的双门路径。认领是一次性的：认了无论成败都销号，避免同一条答案
// 既开视图又落卡。
// M3-B: the scene answer coming back. MUST run before app_book_ai_def_ready -
// the 0x08 frame carries no scene mark (zero protocol change), so the pending
// state claims it first; only an unclaimed frame falls through to the ordinary
// card's double-gate path. The claim is one-shot: claimed means disarmed whether
// or not the view opens, so one answer can never both open a view and land a card.
bool app_book_scene_def_ready(const char* word, const char* def) {
    // 认领：词 + 发起页 + 期限三重匹配，成功即销号（scene_req.c 的纯状态机）。
    // Claim: word + originating page + deadline, one-shot on success (the pure
    // state machine in scene_req.c).
    if (!scene_req_claim(&s_scene_req, word, s_page, esp_timer_get_time() / 1000)) return false;
    if (!def || !*def) {
        ESP_LOGW(TAG, "scene empty answer: word=\"%s\"", s_scene_req.word);
        return false;
    }
    // 页面必须还是发起时那一页：翻过页说明用户在读别处，硬开视图等于把 A 页的
    // 分析贴到 B 页上。宁可不做。（页不配的情况已在认领里销号。）
    // The page must still be the one the request came from: after a turn the user
    // is reading elsewhere, and opening the view would pin page A's analysis onto
    // page B. Better nothing. (A page mismatch was already disarmed by the claim.)
    if (s_view != READING || !s_text || s_page != s_scene_req.page) {
        ESP_LOGW(TAG, "scene drop: view=%d has_text=%d page=%u req_page=%u",
                 (int)s_view, (int)(s_text != NULL), (unsigned)s_page,
                 (unsigned)s_scene_req.page);
        return false;
    }
    ai_reader_source_t src;
    memset(&src, 0, sizeof(src));
    src.chapter = s_chapter;
    src.page = reader_visual_index(s_page);      // 视觉页号（页脚展示）/ visual page for the footer
    src.page_start_offset = s_page;              // 逻辑页索引（锚点兜底）/ logical page index
    copy_text(src.title, sizeof(src.title), s_book_title[0] ? s_book_title : s_title);
    // 26.11.3-C：整段请求开「选段」视图（整段引用条、不高亮词）；点词请求照旧开词条
    // 情景视图。分流的依据只有发起时记下的 excerpt 位——0x08 自己没有这个标记。
    // A passage request opens the 选段 view (whole-selection strip, no word highlight);
    // a word request opens the word scene view. Only the mark recorded at send time can
    // tell them apart: the 0x08 frame itself carries no such bit.
    const bool excerpt = s_scene_req.excerpt;
    const bool opened = excerpt
        ? ai_reader_open_excerpt("选段", def, s_scene_ctx, &src)
        : ai_reader_open_scene(s_scene_req.word, def, s_scene_ctx,
                               s_scene_req.word_off, s_scene_req.word_len, &src);
    if (!opened) {
        return false;
    }
    dict_popup_reset();
    s_reader_panel = READER_PANEL_NONE;
    ai_reader_set_scene_saved(vocab_log_contains(s_scene_req.word));
    invalidate_prep();
    ESP_LOGI(TAG, "scene view opened: word=\"%s\" def=%u quote=%u off=%u excerpt=%d",
             s_scene_req.word, (unsigned)strlen(def), (unsigned)strlen(s_scene_ctx),
             (unsigned)s_scene_req.word_off, (int)excerpt);
    return true;
}

bool app_book_ai_def_ready(const char* word, const char* def) {
    if (s_view != READING || !s_text) return false;
    if (s_reader_panel == READER_PANEL_NONE) {
        // M2 Step9：新释义到达时旧详解会话让位——新词接管，无论本次落卡还是
        // 再开详解。详解页→卡面的版面切换同样要 GC16 打底（双 GC16 语义：
        // 详解会话的每次「进出」都是 FULL）。/ M2 Step9: a new definition
        // evicts an older detail session (the new word takes over, whether it
        // lands on a card or opens a fresh session). A detail-page-to-card
        // layout switch also needs a GC16 base - every enter/exit of the
        // session is FULL under the double-GC16 rule.
        if (ai_reader_active()) {
            ai_reader_close();
            s_ai_def_full = true;
        }
        if (!dict_popup_show_def(word, def)) return false;
        // M2 Step9：卡面装不下（折行触顶仍有剩文）→ 不开卡，改开详解阅读会话
        // ——词卡管快、阅读视图管全。原书章节缓冲不动，关闭即回原书当前页。
        // M2 Step9: when the card cannot hold it (the wrap hit the line cap
        // with text left over), open the detail session instead of the card.
        // The source chapter buffer stays put; closing re-presents the source
        // page in place.
        if (dict_popup_ai_truncated()) {
            dict_popup_reset();
            s_reader_panel = READER_PANEL_NONE;
            ai_reader_source_t src;
            memset(&src, 0, sizeof(src));
            src.chapter = s_chapter;
            src.page = reader_visual_index(s_page);           // 视觉页号（页脚展示）
            src.page_start_offset = s_page;                   // 逻辑页索引（锚点兜底）
            copy_text(src.title, sizeof(src.title),
                      s_book_title[0] ? s_book_title : s_title);
            if (ai_reader_open(word, def, &src)) {
                s_ai_def_full = true; // 双 GC16 的 open 侧 / open side of the double GC16
                ESP_LOGI(TAG, "ai detail session opened for \"%s\" (card truncated)", word);
                return true;
            }
            // 开会话失败（空词/空释义——上游已校验，纯防御）：卡面重开，至少
            // 用户看到（可能截断的）卡而不是什么都没有。/ Session-open failure
            // (empty word/def - already validated upstream, pure defence):
            // re-stage the card so the user at least sees the card.
            if (dict_popup_show_def(word, def)) s_reader_panel = READER_PANEL_DICT;
            return s_reader_panel == READER_PANEL_DICT;
        }
        s_reader_panel = READER_PANEL_DICT;
        return true;
    }
    if (s_reader_panel == READER_PANEL_DICT && dict_popup_is_pending(word)) {
        // 门 B：原地更新。staging 失败（空释义等）不留一张空白卡假活跃——
        // 关卡并如实提示，主循环照旧呈现（状态确实变了）。/ Gate B: in-place
        // update. A staging failure (empty def, ...) leaves no blank card
        // faking activity - close the card and say so; the main loop still
        // presents (the state really changed).
        if (dict_popup_show_def(word, def)) return true;
        dict_popup_reset();
        s_reader_panel = READER_PANEL_NONE;
        copy_text(s_reader_notice, sizeof(s_reader_notice), "AI 查询失败");
        s_reader_notice_until = esp_timer_get_time() / 1000 + 2200;
        return true;
    }
    return false; // 门 A：其他面板开着，释义丢弃进预取缓存 / gate A: another panel, drop into the cache
}

// M2 Step9：present 档位查询——app_book_ai_def_ready 刚开（或关过）详解会话
// 时为 true（主循环用 FULL 呈现，双 GC16 的 open 侧）。一次性消费。
// M2 Step9: the present-tier query - true when app_book_ai_def_ready just
// opened (or closed) a detail session (the main loop presents FULL: the open
// side of the double GC16). One-shot consumption.
bool app_book_ai_def_full(void) {
    const bool full = s_ai_def_full;
    s_ai_def_full = false;
    return full;
}

// M2 Step9：详解会话活跃谓词（app_loop 的 KEY_2 豁免判定用）。
// M2 Step9: the detail-session-active predicate (app_loop's KEY_2 exemption).
bool app_book_ai_detail_active(void) {
    return ai_reader_active();
}

// M2 Step9：关闭详解会话并请求 FULL 重呈现（双 GC16 的 close 侧）。原书章节
// 缓冲从未卸载，render 随即走原阅读路径画回原书当前页——阅读位置零丢失。
// M2 Step9: close the session and ask for a FULL re-present (the close side of
// the double GC16). The source chapter buffer was never unloaded, so render
// walks the plain reader path straight back to the source page - zero
// reading-position loss.
static app_redraw_t ai_reader_close_full(void) {
    ai_reader_close();
    s_ai_def_full = false;
    return APP_REDRAW_FULL;
}

// M4-A：静默撤销——双指成立瞬间，把单指可能刚打开的浮层收回去。
// 只撤「词卡」与「详解会话」这两类单指点词的结果；工具栏、字体选择等用户主动
// 打开的面板不动。不触发查词、不落生词本——撤销本身不写任何记录。
// M4-A: silent retract - the moment two fingers are established, take back what
// the single finger may just have opened. Only the lookup card and the detail
// session qualify (both are results of one tap); panels the user opened on
// purpose (toolbar, font picker) stay. No lookup runs and no vocab entry is
// written - a retract records nothing.
static app_redraw_t dual_retract(app_ctx_t* ctx) {
    (void)ctx;
    if (ai_reader_active()) return ai_reader_close_full();
    if (s_reader_panel == READER_PANEL_DICT) {
        dict_popup_reset();
        s_reader_panel = READER_PANEL_NONE;
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

// M4-A：双指意图出口。三个动作全部复用既有实现，不新造第二条路径：
// 方向切换走 toggle_reader_landscape（render 按设置项同步面板方向），字号走
// resize_text（一档 = BOOK_PX_STEP，越界时它自己返回 NONE，等于天然钳位），
// 撤销走上面的 dual_retract。
// M4-A: two-finger intent outlet. All three actions ride existing paths instead
// of inventing a second one: orientation through toggle_reader_landscape (render
// syncs the panel from the setting), font through resize_text (one step =
// BOOK_PX_STEP; out-of-range makes it return NONE, which is the clamp), retract
// through dual_retract above.
static app_redraw_t dual_gesture_event(app_ctx_t* ctx, const ui_dual_event_t* ev) {
    // 双指在框选态一律只当「退出」：不翻页、不换横竖、不调字号（E6/R2）。
    // In the selection mode a two-finger gesture means exit only: no page turn, no
    // orientation flip and no font step (E6/R2).
    if (boxsel_active()) { return boxsel_exit(BOXSEL_EXIT_DUAL); }
    if (ev == NULL) return APP_REDRAW_NONE;
    switch (ev->type) {
        case UI_DUAL_EVT_RETRACT:
            return dual_retract(ctx);
        case UI_DUAL_EVT_ORIENT:
            return s_view == READING ? toggle_reader_landscape(ctx) : APP_REDRAW_NONE;
        case UI_DUAL_EVT_FONT_STEP:
            return (s_view == READING && ev->steps != 0) ? resize_text(ctx, ev->steps)
                                                         : APP_REDRAW_NONE;
        default:
            return APP_REDRAW_NONE;
    }
}
static uint8_t* load_cover_gray(const char* source, const char* title, const char* author,
                                bool decode, bool* pending) {
    *pending = false;
    if (!pico_boot_asset_allowed(source)) return NULL;
    uint8_t* gray = heap_caps_malloc(BOOK_COVER_W * BOOK_COVER_H, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!gray) return NULL;
    if (!book_cover_load_gray(source, title, author, gray, decode, pending)) {
        free(gray); return NULL;
    }
    return gray;
}
static uint8_t *load_shelf_cover_gray(int row, const char *source, const char *title,
                                      const char *author, bool decode, bool *pending) {
    if (s_view != SHELF || app_settings_shelf_style() != 5)
        return load_cover_gray(source, title, author, decode, pending);
    *pending = false;
    if (!pico_boot_asset_allowed(source)) return NULL;
    s_covers[row].width = BOOK_COVER_W; s_covers[row].height = BOOK_COVER_H;
    uint8_t *gray = heap_caps_malloc(BOOK_COVER_W * BOOK_COVER_H, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (gray && !book_cover_load_list_gray(source, title, author, gray, decode, pending,
                                          &s_covers[row].width, &s_covers[row].height)) {
        free(gray); gray = NULL;
    }
    return gray;
}

// 直接读最终点阵：不分配灰阶工作区，也不在每次书架切页重新转换。
// Read the finished raster: no gray workspace or repeated conversion on shelf paging.
static bool load_shelf_cover_fast(int row, int index) {
    if (s_view != SHELF || !app_settings_main_fast_refresh() || row >= BOOK_GRID_ROWS ||
        !pico_boot_asset_allowed(s_shelf[index].path)) return false;
    EpdRect image = shelf_cover_image(row);
    size_t bytes = (size_t)SHELF_FAST_COVER_STRIDE * image.height;
    if (bytes > SHELF_FAST_COVER_BYTES || heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
        < SHELF_FAST_CACHE_RESERVE + SHELF_FAST_COVER_BYTES) return false;
    uint8_t *bits = heap_caps_calloc(1, SHELF_FAST_COVER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!bits) return false;
    book_fast_cover_key_t key = {(unsigned)image.width, (unsigned)image.height, SHELF_FAST_COVER_STRIDE,
        app_settings_system_contrast(), (image.x&3)|((image.y&3)<<2)};
    bool edge;
    if (!book_cover_fast_cache_read(s_shelf[index].path, s_shelf[index].name, s_shelf[index].author,
                                   key, bits, &edge)) { free(bits); return false; }
    s_covers[row].fast_bits = bits; s_covers[row].fast_white_edge = edge;
    s_covers[row].fast_contrast = key.contrast; s_covers[row].fast_phase = key.phase;
    return true;
}
static void save_shelf_cover_fast(int row, int index) {
    if (!s_covers[row].fast_bits) return;
    EpdRect image = shelf_cover_image(row);
    book_fast_cover_key_t key = {(unsigned)image.width, (unsigned)image.height, SHELF_FAST_COVER_STRIDE,
        s_covers[row].fast_contrast, s_covers[row].fast_phase};
    book_cover_fast_cache_write(s_shelf[index].path, s_shelf[index].name, s_shelf[index].author,
                                key, s_covers[row].fast_bits, s_covers[row].fast_white_edge);
}

static unsigned prepare_cover_batch(app_ctx_t *ctx,bool cache_only) {
    if((s_view!=SHELF&&s_view!=MANAGE)||(!cache_only&&ctx->touch&&ctx->touch->touched))return 0;
    unsigned changed=0;
    for(int row=0;row<shelf_rows();++row){
        unsigned bit=1u<<row;if(!(s_cover_pending_mask&bit))continue;
        if(s_view==SHELF&&app_poll_main_navigation(ctx))return changed;
        int index=ctx->leaf*shelf_rows()+row;
        if(index<0||index>=s_visible_count||s_covers[row].index!=index||s_shelf[index].removed){s_cover_pending_mask&=~bit;continue;}
        EpdRect image=shelf_cover_image(row);
        book_cover_job_t request={.list=s_view==SHELF&&app_settings_shelf_style()==5,
            .fast=s_view==SHELF&&app_settings_main_fast_refresh(),.width=image.width,.height=image.height,
            .stride=SHELF_FAST_COVER_STRIDE,.contrast=app_settings_system_contrast(),
            .phase=(image.x&3)|((image.y&3)<<2),.x=image.x,.y=image.y};
        copy_text(request.path,sizeof(request.path),s_shelf[index].path);
        copy_text(request.title,sizeof(request.title),s_shelf[index].name);
        copy_text(request.author,sizeof(request.author),s_shelf[index].author);
        if(!s_cover_generation)s_cover_generation=book_cover_jobs_begin();
        if(!book_cover_jobs_submit(s_cover_generation,row,&request)){
            s_cover_generation=book_cover_jobs_begin();
            if(!book_cover_jobs_submit(s_cover_generation,row,&request)){s_cover_pending_mask&=~bit;continue;}
        }
        book_cover_result_t result={0};int state=book_cover_jobs_take(s_cover_generation,row,&result);
        if(!state)continue;
        s_cover_pending_mask&=~bit;s_cover_decode_mask&=~bit;
        if(state<0)continue;
        free(s_covers[row].gray);free(s_covers[row].fast_bits);
        s_covers[row].gray=result.fast?NULL:result.pixels;
        s_covers[row].fast_bits=result.fast?result.pixels:NULL;
        s_covers[row].width=result.width;s_covers[row].height=result.height;
        s_covers[row].fast_white_edge=result.white_edge;
        s_covers[row].fast_contrast=request.contrast;s_covers[row].fast_phase=request.phase;
        changed|=bit;s_cover_dirty_mask|=bit;
    }
    return changed;
}

// 分批只用于准备，不能让几张已解码封面先出现在屏幕或点击反馈中。
// Chunk preparation only: neither page painting nor press feedback reveals an incomplete cover group.
static bool shelf_cover_frame_ready(void) {
    return s_view != SHELF || s_cover_pending_mask == 0;
}
static unsigned shelf_cover_reveal_mask(void) {
    return shelf_cover_frame_ready() ? s_cover_dirty_mask : 0;
}

static void prepare_covers(app_ctx_t* ctx) {
    if (s_view != SHELF && s_view != MANAGE) return;
    int rows = shelf_rows();
    if (s_covers[0].index != ctx->leaf * rows) {
        book_cover_jobs_cancel(s_cover_generation);s_cover_generation=book_cover_jobs_begin();
        s_cover_pending_mask = s_cover_decode_mask = s_cover_warm_mask = 0;
        s_cover_dirty_mask = s_cover_present_mask = 0;
    }
    for (int row = 0; row < rows; ++row) {
        int index = ctx->leaf * rows + row;
        EpdRect image = shelf_cover_image(row);
        bool packed = s_view == SHELF && app_settings_main_fast_refresh();
        if (s_covers[row].index == index &&
            (packed ? fast_cover_matches(row, image) : s_covers[row].gray != NULL)) continue;
        if (s_covers[row].index == index && !s_covers[row].gray && !s_covers[row].fast_bits) continue;
        free(s_covers[row].gray); free(s_covers[row].fast_bits);
        s_covers[row].gray = s_covers[row].fast_bits = NULL;
        s_covers[row].index = index;
        s_cover_dirty_mask &= ~(1u<<row);
        if (index >= s_visible_count || index < 0 || s_shelf[index].removed) continue;
        // Queue each slot, then gather cheap disk-cache hits before painting; decode misses later.
        s_cover_pending_mask |= 1u << row;
        s_cover_warm_mask |= 1u << row;
        s_cover_warm_after_ms = ctx->now_ms + 1500;
    }
    prepare_fast_covers();
    (void)prepare_cover_batch(ctx, true);
}

// 首帧短暂汇合TF缓存命中，让题头、书架和封面一起显示；冷解码不无限阻塞。
// Briefly gather warm TF hits for a complete first frame; never wait indefinitely for cold decoding.
static void gather_cover_entry(app_ctx_t *ctx) {
    if(s_view!=SHELF||!s_cover_pending_mask||ctx->request_app||
       (s_presented_view==(int)SHELF&&!s_shelf_page_pending))return;
    const int64_t deadline=esp_timer_get_time()+60000;
    while(s_cover_pending_mask&&esp_timer_get_time()<deadline){
        if(app_poll_main_navigation(ctx))return;
        vTaskDelay(1);
        (void)prepare_cover_batch(ctx,true);
        if(ctx->request_app)return;
    }
}

// 画面和操作优先，预生成只在封面加载完、无触摸的空闲书架运行一次一本。
// Present and input take priority; warm one cover only on an idle shelf after visible loading completes.
static void warm_shelf_lock_cover(app_ctx_t *ctx) {
    if (s_view != SHELF || app_settings_lock_style() != 2 || s_clear_confirm || s_batch_confirm ||
        s_cover_pending_mask || !s_cover_warm_mask || ctx->now_ms < s_cover_warm_after_ms ||
        (ctx->touch && ctx->touch->touched) || ctx->request_app || ctx->request_return || ctx->request_menu) return;
    if (app_poll_main_navigation(ctx)) return;
    for (int row = 0; row < shelf_rows(); ++row) {
        const unsigned bit = 1u << row;
        if (!(s_cover_warm_mask & bit)) continue;
        s_cover_warm_mask &= ~bit; s_cover_warm_after_ms = ctx->now_ms + 1000;
        int index = ctx->leaf * shelf_rows() + row;
        if (index >= 0 && index < s_visible_count && s_covers[row].index == index && !s_shelf[index].removed)
            (void)book_lock_collage_warm_cover(s_shelf[index].path);
        (void)app_poll_main_navigation(ctx);
        break;
    }
}

static void draw_favorite_icon(uint8_t *fb, int x, int y, int w, int h, bool filled, uint8_t ink) {
    // 只绘制标记本身，封面上不加矩形衬底。/ Draw only the glyph, without a rectangular cover underlay.
    const int notch_y = y + h - 9;
    if (filled) {
        // 把 V 形尾部也填满，避免选中后只剩矩形黑块、尾部仍是空心。
        // Fill the V-shaped tail as well so the selected glyph is solid throughout.
        for (int py = y; py <= y + h; ++py) {
            int arm = py <= notch_y ? w / 2 : (w / 2) * (y + h - py) / 9;
            ui_fill_rect_fast((EpdRect){x, py, arm + 1, 1}, ink, fb);
            ui_fill_rect_fast((EpdRect){x + w - arm, py, arm + 1, 1}, ink, fb);
        }
        return;
    }
    // 空心态使用与阅读工具栏相近的两像素线宽。
    // Keep the outline close to the other reader toolbar icons in visual weight.
    ui_fill_rect_fast((EpdRect){x, y, w + 1, 2}, ink, fb);
    ui_fill_rect_fast((EpdRect){x, y, 2, h - 1}, ink, fb);
    ui_fill_rect_fast((EpdRect){x + w - 1, y, 2, h - 1}, ink, fb);
    epd_draw_line(x, y + h, x + w / 2, notch_y, ink, fb);
    epd_draw_line(x + 1, y + h - 1, x + w / 2, notch_y + 1, ink, fb);
    epd_draw_line(x + w / 2, notch_y, x + w, y + h, ink, fb);
    epd_draw_line(x + w / 2, notch_y + 1, x + w - 1, y + h - 1, ink, fb);
}
static void draw_shelf_favorite_icon(uint8_t *fb, int x, int y, int w, int h, bool white_edge) {
    if (white_edge) {
        // 仅沿书签轮廓扩两像素白边，深封面上也不出现白色矩形底块。
        // Dilate only the bookmark silhouette by two pixels, never a white backing box.
        const int notch_y = y + h - 9;
        for (int py = y; py <= y + h; ++py) {
            int arm = py <= notch_y ? w / 2 : (w / 2) * (y + h - py) / 9;
            ui_fill_rect_fast((EpdRect){x - 2, py - 2, arm + 5, 5}, UI_GRAY_WHITE, fb);
            ui_fill_rect_fast((EpdRect){x + w - arm - 2, py - 2, arm + 5, 5}, UI_GRAY_WHITE, fb);
        }
    }
    draw_favorite_icon(fb, x, y, w, h, true, UI_GRAY_BLACK);
}
static bool cover_favorite_needs_white_edge(const uint8_t *gray, EpdRect image, unsigned sw, unsigned sh) {
    if (!gray || !sw || !sh || image.width <= 0 || image.height <= 0) return false;
    unsigned sum = 0, count = 0;
    const book_crop_t crop = book_cover_crop(sw, sh, image.width, image.height);
    // 只在标记周围采样；图标始终为黑色，深封面才加白边。
    // Sample the badge area; keep the glyph black and outline it only on dark covers.
    for (int y = 9; y < 39; y += 5) {
        for (int x = 11; x < 35; x += 4) {
            unsigned sx = crop.x + (uint64_t)(unsigned)x * crop.width / (unsigned)image.width;
            unsigned sy = crop.y + (uint64_t)(unsigned)y * crop.height / (unsigned)image.height;
            if (sx >= sw) sx = sw - 1;
            if (sy >= sh) sy = sh - 1;
            sum += ui_contrast_gray(gray[sy * sw + sx]);
            ++count;
        }
    }
    return sum < count * 145;
}
static void draw_shelf_cover(uint8_t* fb, EpdRect card, int row, const char* name, bool favorite) {
    EpdRect image = shelf_cover_image(row);
    image.y += card.y - row_rect(row).y;
    const bool artwork = shelf_cover_frame_ready();
    ui_fill_rect_fast(image, UI_GRAY_LIGHT, fb);
    if (artwork && app_settings_main_fast_refresh() && s_view==SHELF && fast_cover_matches(row,image)) {
        ui_blit_mono_bitmap(fb,image,s_covers[row].fast_bits,SHELF_FAST_COVER_STRIDE);
    } else if (artwork && s_covers[row].gray) {

        const uint8_t *gray = s_covers[row].gray;
        // 封面缓冲是 176×240、这一格是 164×214；按长边铺满 + 居中裁剪，避免被压扁。
        // The cover buffer is 176x240 and this frame is 164x214; fill by the longer side and
        // centre-crop so the artwork is not squashed.
        const unsigned frame_width = (unsigned)image.width, frame_height = (unsigned)image.height;
        unsigned sw = s_view == SHELF && app_settings_shelf_style() == 5 ? s_covers[row].width : BOOK_COVER_W;
        unsigned sh = s_view == SHELF && app_settings_shelf_style() == 5 ? s_covers[row].height : BOOK_COVER_H;
        const book_crop_t crop = book_cover_crop(sw, sh, frame_width, frame_height);
        unsigned sample_x[SHELF_FAST_COVER_WIDTH];
        uint8_t tones[256];
        const bool sampled=image.width<=SHELF_FAST_COVER_WIDTH;
        const bool binary=app_settings_main_fast_refresh()&&s_view==SHELF;
        if(sampled)for(int x=0;x<image.width;++x)sample_x[x]=crop.x+(uint64_t)(unsigned)x*crop.width/frame_width;
        for(unsigned v=0;v<256;++v)tones[v]=ui_contrast_gray((uint8_t)v);
        for (int y = 0; y < image.height; ++y) {
            const int sy = (int)(crop.y + (uint64_t)(unsigned)y * crop.height / frame_height);
            for (int x = 0; x < image.width; ++x) {
                const unsigned sx = sampled?sample_x[x]:crop.x+(uint64_t)(unsigned)x*crop.width/frame_width;
                const uint8_t tone = tones[gray[sy * sw + sx]];
                epd_draw_pixel(image.x + x, image.y + y,
                               binary
                                   ? ui_image_dither_cover_bw((tone>>4)*17u, image.x + x, image.y + y)
                                   : ui_image_dither_gray(tone, image.x + x, image.y + y), fb);
            }
        }
    } else {
        char title[80]; copy_text(title, sizeof(title), name);
        char *dot = strrchr(title, '.'); if (dot) *dot = 0;
        fit_text(title, 26, image.width - 22);
        ui_text_vc(fb, image.x + image.width / 2, image.y + image.height / 2, 26,
                   title, EPD_DRAW_ALIGN_CENTER, false);
    }
    ui_draw_round_rect(fb, image, 0, UI_GRAY_BLACK);
    if (favorite && image.width >= 28 && image.height >= 42) {
        draw_shelf_favorite_icon(fb, image.x + 10, image.y + 8, 24, 31,
                                 artwork && ((app_settings_main_fast_refresh() && s_view == SHELF && fast_cover_matches(row, image))
                                     ? s_covers[row].fast_white_edge : cover_favorite_needs_white_edge(s_covers[row].gray, image,
                                         app_settings_shelf_style() == 5 ? s_covers[row].width : BOOK_COVER_W,
                                         app_settings_shelf_style() == 5 ? s_covers[row].height : BOOK_COVER_H)));
    }
}
static EpdRect paint_list_cover_feedback(app_ctx_t *ctx, int row) {
    EpdRect image = shelf_cover_image(row);
    EpdRect area = image;
    area.y -= SHELF_LIST_BOUNCE_PX;
    area.height += SHELF_LIST_BOUNCE_PX;
    // 两位置均位于白色列表卡片内；不清整行，也不绘制额外反馈框。
    // Both positions stay within the white list card; never clear a whole row or add a feedback frame.
    ui_fill_rect_fast(area, UI_GRAY_WHITE, ctx->fb);
    EpdRect card = row_rect(row);
    if (s_pressed_control == row) card.y -= SHELF_LIST_BOUNCE_PX;
    int index = ctx->leaf * shelf_rows() + row;
    if (index >= 0 && index < s_visible_count)
        draw_shelf_cover(ctx->fb, card, row, s_shelf[index].name, s_shelf[index].favorite);
    return area;
}
static void draw_list_title(uint8_t *fb, const char *name, int top) {
    char lines[2][128] = {{0}};
    int px = ttf_em_height_px(27);
    const unsigned char *at = (const unsigned char *)name;
    for (int line = 0; line < 2 && *at; ++line) {
        size_t used = 0;
        while (*at && used < sizeof(lines[line])-5) {
            unsigned n = *at < 128 ? 1 : (*at & 0xe0)==0xc0 ? 2 : (*at & 0xf0)==0xe0 ? 3 : 4;
            if (strlen((const char *)at) < n) break;
            memcpy(lines[line]+used, at, n); lines[line][used+n] = 0;
            if (ui_text_fixed_width_px(px, lines[line]) > 378) { lines[line][used] = 0; break; }
            used += n; at += n;
        }
        if (line == 1 && *at) fit_text(lines[line], px, 352);
        ui_text_fixed_vc(fb, 244, top + line*35, px, lines[line], EPD_DRAW_ALIGN_LEFT, false);
    }
}
static void draw_list_row(uint8_t *fb, int row, const shelf_entry_t *item) {
    EpdRect card = row_rect(row);
    draw_list_title(fb, item->name, card.y + 42);
    char author[128]; copy_text(author, sizeof(author), item->author[0] ? item->author : "作者未知");
    int author_px = ttf_em_height_px(21), progress_px = ttf_em_height_px(19);
    fit_text(author, author_px, 378);
    int author_y = ui_text_fixed_width_px(ttf_em_height_px(27), item->name) > 378 ? 106 : 83;
    ui_text_fixed_ink_vc(fb, 244, card.y + author_y, author_px, author, EPD_DRAW_ALIGN_LEFT, 0x60);
    unsigned progress = item->has_progress ? item->pct : 0;
    if (progress > 100) progress = 100;
    char label[48]; snprintf(label, sizeof(label), progress == 100 ? "已读完 · %u%%" :
                              progress ? "已读 %u%%" : "未开始 · %u%%", progress);
    ui_text_fixed_vc(fb, 244, card.y + 143, progress_px, label, EPD_DRAW_ALIGN_LEFT, false);
    EpdRect bar = {244, card.y + 163, 378, 11};
    ui_fill_round_rect(fb, bar, 5, 0xd0);
    if (progress) {
        EpdRect fill = bar; fill.width = bar.width * progress / 100;
        if (fill.width < 1) fill.width = 1;
        ui_fill_round_rect(fb, fill, 5, 0x40);
    }
    ui_draw_control_frame(fb, bar, 5, 0x50);
    if (row < 3) ui_draw_separator(fb, card.y + 196, 50, 584, 0x60);
}
static void draw_shelf_furniture(uint8_t* fb) {
    uint8_t style = app_settings_shelf_style();
    if (!style || style == 5) return;
    for (int row = 0; row < 3; ++row) {
        int top = (s_view == SHELF ? 192 : 224) + row * 282;
        if (style == 1) {
            ui_fill_rect_fast((EpdRect){36, top + 214, 612, 12}, 0x30, fb);
            ui_hairline(fb, top + 214, 36, 612, 0x58);
            ui_hairline(fb, top + 225, 36, 612, 0x10);
        } else if (style == 2) {
            EpdRect acrylic = {20, top + 136, 644, 90};
            draw_shelf_guard(fb, acrylic, row);
            if(s_view==SHELF&&app_settings_main_refresh_mode()==APP_MAIN_REFRESH_NORMAL){
                EpdRect bands[3];for(int i=0;i<3;++i)bands[i]=(EpdRect){0,328+i*282,UI_LOCK_WIDTH,90};
                display_main_transition_gray_bands(bands,3);
            }

        }
    }
}
static EpdRect tool_rect(int i) {
    int x0 = i * UI_LOCK_WIDTH / BOOK_TOOL_COUNT;
    int x1 = (i + 1) * UI_LOCK_WIDTH / BOOK_TOOL_COUNT;
    return (EpdRect){x0, 1096, x1 - x0, 120};
}
// 工具条的标签与图标必须是逐项对齐的一份清单。设置页那一侧有 _Static_assert 盯着
// 图标编号表，这里原本没有——少加一个数组元素会让「点生词本触发目录」这类错位
// 静默发生，所以两张表都断言成同一长度。
// Label and icon tables must line up item by item. The settings page has a
// _Static_assert watching its icon slot table; this one had none, and a missing
// entry would silently shift every tap (tapping the vocabulary book opening the
// TOC). Both tables are now asserted against the same count.
static const char* const k_tool_labels[BOOK_TOOL_COUNT] = {
    "目录", "书签", "阅读统计", "阅读设置", "字体设置", "生词本", "听书",
};
static const ui_icon_t k_tool_icons[BOOK_TOOL_COUNT] = {
    UI_ICON_LIST,          // 0 目录 / TOC
    UI_ICON_BOOKMARK,      // 1 书签 / bookmark
    UI_ICON_CHART_COLUMN,  // 2 阅读统计 / reading stats
    UI_ICON_BOOK_OPEN,     // 3 阅读设置 / reading settings
    UI_ICON_TYPE,          // 4 字体设置 / font settings
    UI_ICON_A_LARGE_SMALL, // 5 生词本 / vocabulary book (Lucide "a-large-small")
    UI_ICON_VOLUME_2,      // 6 听书 / TTS read-aloud (Lucide "volume-2")
};
_Static_assert(sizeof(k_tool_labels) / sizeof(k_tool_labels[0]) == BOOK_TOOL_COUNT,
               "every reader tool needs a label");
_Static_assert(sizeof(k_tool_icons) / sizeof(k_tool_icons[0]) == BOOK_TOOL_COUNT,
               "every reader tool needs an icon");
static int leaves(void) {
    if (s_view == TOC) return book_toc_pages(book_navigation_count());
    int count = s_visible_count;
    int rows = s_view == BULK ? BOOK_BULK_ROWS : shelf_rows();
    return count ? 1 + (count - 1) / rows : 1;
}
static EpdRect shelf_manage_rect(void) { return (EpdRect){442, 94, 97, 54}; }
static EpdRect shelf_import_rect(void) { return (EpdRect){551, 94, 97, 54}; }
// P0-5（26.11.2）入口甲：摘抄按钮 + 三处命中矩形的编译期互斥。标题预算 230（36..266）、
// 下拉箭头 293..319、摘抄按钮 333..430、管理按钮 442 起——谁改布局谁改这套断言。
// P0-5 entrance A: the excerpt button plus compile-time mutual exclusion of the three
// header hit rectangles. Change the layout, change these asserts.
static EpdRect shelf_excerpt_rect(void) { return (EpdRect){333, 94, 97, 54}; }
/// 26.11.3：分类下拉的**感应区**（点标题/箭头弹菜单），不是弹开后的菜单框。
/// 它曾是匿名裸字面量 `{24,79,388,74}`——右界 412 默默盖住摘抄按钮 81.4% 面积，
/// 26 天无人发现，因为那块在加按钮前本就是没人点的空白。提成命名矩形 + 右界 333
/// （零重叠），让"谁盖住谁"能被断言看见。
/// 26.11.3: the dropdown's SENSING rect (tap the title/chevron), not the opened menu.
/// It used to be an anonymous `{24,79,388,74}` whose right edge at 412 silently ate
/// 81.4% of the excerpt button. Named now, right edge pinned to 333: zero overlap.
static EpdRect shelf_category_tap_rect(void) { return (EpdRect){24, 79, 309, 74}; }
_Static_assert(36 + 230 + 27 == 293, "P0-5: dropdown arrow must start right after the 230px title budget");
_Static_assert(333 > 319, "P0-5: excerpt button must clear the dropdown arrow (293..319)");
_Static_assert(333 + 97 <= 442, "P0-5: excerpt button must not overlap the manage button (442..)");
// 26.11.3：跨矩形关系——原来缺的就是这一条。感应区右界必须让开按钮左界（零重叠，
// 不设缓冲：留余量等于留重叠，手感问题归行为层不归几何层）。
// 26.11.3: the cross-rectangle relation that was missing. The sensing rect's right edge
// must clear the button's left edge - zero overlap, no fudge margin.
_Static_assert(24 + 309 <= 333, "26.11.3: category sensing rect must not reach the excerpt button");
_Static_assert(36 + 230 + 27 + 26 <= 333, "26.11.3: dropdown arrow at full title budget stays left of the button");
/// 26.11.3 第三层：命中优先级不变式。几何会腐烂（有人在上面几条断言之外再动一手），
/// 但"按钮永远优先"这条路由层的不变式兜底。
/// Third layer: the routing invariant. Geometry can rot; "buttons always win" cannot.
static bool shelf_header_button_hit(int x, int y) {
    return ui_rect_hit(shelf_excerpt_rect(), x, y) || ui_rect_hit(shelf_manage_rect(), x, y)
        || ui_rect_hit(shelf_import_rect(), x, y);
}
/// P1-2（26.11.2）：队列非空时按钮就是全局出口的徽标——「摘抄 N」，N 是待落盘真值。
/// 缓冲由调用方给（栈上）——本批 bss 目标 0，不许为一行标签长静态量。
/// 26.11.3：三个按钮共用一个字号，所以文案必须放得下——全角间隔符 20px 会让
/// 「摘抄·10」溢出 73px 可用宽，改空格分隔；队列封顶 EXCERPT_QUEUE_MAX，真值永远
/// 有两位数位置，「9+」收敛分支随之删除（少一个特例）。
/// P1-2 (26.11.2): a non-empty queue turns the button into the global exit badge
/// "excerpt N". 26.11.3: one font size across the three buttons means the label must
/// fit - a full-width separator pushes "excerpt 10" past the 73px budget, so a space
/// it is; the queue caps at EXCERPT_QUEUE_MAX, the true count always fits, and the
/// "9+" clamp branch goes away with it.
static const char *shelf_excerpt_label(char *buf, size_t cap) {
    const unsigned n = excerpt_store_pending();
    if (!n) copy_text(buf, cap, "摘抄");
    else snprintf(buf, cap, "摘抄 %u", n);
    return buf;
}
static void draw_shelf_header_button(uint8_t *fb, EpdRect r, int id, const char *label, int font) {
    bool pressed = s_pressed_control == id;
    ui_fill_round_rect(fb, r, 18, UI_GRAY_WHITE);
    ui_draw_control_frame(fb, r, 18, 0x70);
    if (pressed) ui_draw_pressed_round_rect(fb, r, 18);
    ui_text_vc(fb, r.x + r.width / 2, r.y + r.height / 2 + (pressed ? 2 : 0), font,
               label, EPD_DRAW_ALIGN_CENTER, false);
}
static EpdRect shelf_page_arrow_rect(int direction) {
    return (EpdRect){direction < 0 ? 168 : 444, 1008, 72, 60};
}
static void draw_shelf_pager(uint8_t *fb, int count, int page, int pages) {
    char label[64];
    snprintf(label, sizeof(label), "%d本书 · %02d/%02d", count, page + 1, pages);
    ui_text_vc(fb, 342, 1038, 21, label, EPD_DRAW_ALIGN_CENTER, false);
    ui_draw_icon(fb, 204, 1038, 28, UI_ICON_CHEVRON_LEFT, page > 0 ? 0 : 0x90);
    ui_draw_icon(fb, 480, 1038, 28, UI_ICON_CHEVRON_RIGHT, page + 1 < pages ? 0 : 0x90);
}
static app_redraw_t shelf_turn_page(app_ctx_t *ctx, int direction);
static EpdRect import_rect(int index) { return (EpdRect){36, 230 + index * 164, 612, 136}; }
static void clean_filename(char *dst, size_t cap, const char *filename) {
    copy_text(dst, cap, filename);
    char *ext = strrchr(dst, '.');
    if (ext) *ext = 0;
    char *start = dst;
    while (*start == ' ' || *start == '\t') ++start;
    if (start != dst) memmove(dst, start, strlen(start) + 1);
    size_t len = strlen(dst);
    while (len && (dst[len - 1] == ' ' || dst[len - 1] == '\t')) dst[--len] = 0;
}

// 截断必须停在UTF8字符边界。/ Truncation must stop at a UTF8 character boundary.
static void copy_text(char* dst, size_t cap, const char* src) {
    if (!cap) return;
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n && ((unsigned char)src[n] & 0xc0) == 0x80) --n;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}
static void fit_text(char* text, int px, int width) {
    while (*text && ttf_text_width_px(ui_text_effective_px(px), text) > width) {
        size_t n = strlen(text) - 1;
        while (n && ((unsigned char)text[n] & 0xc0) == 0x80) --n;
        text[n] = 0;
    }
}

static void fit_fixed_text(char* text, int px, int width) {
    while (*text && ui_text_fixed_width_px(px, text) > width) {
        size_t n = strlen(text) - 1;
        while (n && ((unsigned char)text[n] & 0xc0) == 0x80) --n;
        text[n] = 0;
    }
}

static void reader_footer_strip_number(char *dst, size_t cap, const char *source) {
    char title[128];
    copy_text(title, sizeof(title), source ? source : "");
// name 只被读取，但下面会把字符串字面量赋给它。CI 的 sdkconfig.ci 开了
// CONFIG_COMPILER_WARN_WRITE_STRINGS，字面量因此是 const char[]，赋给 char* 会丢 const 并在
// -Werror 下报错。声明成 const 指针即可。
// name is only read from, but it is assigned a string literal below. CI's sdkconfig.ci enables
// CONFIG_COMPILER_WARN_WRITE_STRINGS, which makes literals const char[], so assigning one to a
// char* discards the qualifier and fails under -Werror. A const pointer is enough.
    const char *name = title;
    while (*name == ' ' || *name == '\t') ++name;
    char *chapter_mark = !strncmp(name, "第", strlen("第")) ? strstr(name, "章") : NULL;
    if (chapter_mark && chapter_mark - name < 24) {
        char *after = chapter_mark + strlen("章");
        while (*after == ' ' || *after == '\t' || *after == ':' || *after == '-' ||
               !strncmp(after, "：", strlen("：")) || !strncmp(after, "、", strlen("、")) ||
               !strncmp(after, "·", strlen("·"))) {
            if (*after == ' ' || *after == '\t' || *after == ':' || *after == '-') ++after;
            else after += strlen("：");
        }
        name = after;
    }
    if (!*name || !strcmp(name, "本章") || !strcmp(name, "章节")) name = "未命名章节";
    copy_text(dst, cap, name);
}
static void reader_footer_chapter_name(char *dst, size_t cap) {
    char source[128] = {0};
    bool selected = s_selected_toc < book_navigation_count() &&
        book_navigation_chapter(s_selected_toc) == s_chapter &&
        book_navigation_title(s_selected_toc, source, sizeof(source)) == ESP_OK && source[0];
    if (!selected) {
        if (s_chapter_heading_title[0]) copy_text(source, sizeof(source), s_chapter_heading_title);
        else if (book_chapter_title(s_chapter, source, sizeof(source)) != ESP_OK) source[0] = 0;
    }
    reader_footer_strip_number(dst, cap, source);
}
static uint32_t chapter_end(void) {
    return s_chapter + 1 < book_chapter_count()
        ? book_chapter_byte_offset(s_chapter + 1) : book_total_bytes();
}
static size_t reader_page_offset(size_t page) {
    size_t off = book_layout_page_start_offset(page);
    if (s_jump_offset != SIZE_MAX && page == s_jump_page && s_jump_offset > off)
        off = s_jump_offset;
    return off;
}
// 视觉页寻址 API 前向声明：percent 的末页判定要走视觉页口径。定义留在地图区块内
// （互证测试按区块抽取，percent 留在区块外，抽取内容因此逐字节不变）。
// Forward declarations of the visual-page API: percent's last-page check takes the
// visual-page view. The definitions stay in the map block (which the cross-check
// test extracts); keeping percent outside leaves the extracted text byte-identical.
static size_t reader_visual_count(void);
static size_t reader_visual_index(size_t logical);
static unsigned percent(size_t page) {
    // 掉盘：书籍元数据已随句柄关闭，百分比只能用快照算。章内偏移仍是内存版面的真值，
    // 所以这一页的进度和拔卡前逐位一致——不是估的。
    // Offline: the book metadata left with the handle, so the percentage comes from the
    // snapshot. The in-chapter offset is still the real one from the in-memory layout, so
    // this page's progress matches the pre-removal value exactly - it is not an estimate.
    if (s_media_offline) {
        if (!s_off_total_bytes) return 0;
        uint64_t off = (uint64_t)s_off_chapter_offset + reader_page_offset(page);
        return (unsigned)(off * 100 / s_off_total_bytes);
    }
    uint32_t total = book_total_bytes();
    if (!total) return 0;
    if (s_chapter + 1 == book_chapter_count() &&
        reader_visual_index(page) + 1 == reader_visual_count()) return 100;
    uint32_t off = book_position_bytes(book_chapter_byte_offset(s_chapter), chapter_end(),
                                       reader_page_offset(page), s_text_len);
    return (unsigned)((uint64_t)off * 100 / total);
}
static pending_progress_t* pending_find(const char* path) {
    for (pending_progress_t* p = s_pending; p; p = p->next) if (!strcmp(p->path, path)) return p;
    return NULL;
}

// ---- 26.9.26 S26-2：摘抄回执走页脚（审计钉死项 1/2）----------------------
// 26.9.26 S26-2: excerpt receipts live on the footer (audit items one and two).
//
// 为什么是页脚：48px 动作栏随 EXIT 的整页刷新消失，「按钮行原地提示」是空头支票。
// 页脚每页都在、普通翻页必刷新，「待落盘 N」的常显由载体本身兑现。
// Why the footer: the 48 px action bar vanishes with the EXIT repaint, so a hint there is a
// promise nobody can see. The footer is on every page and refreshes on every ordinary turn,
// so "pending N" stays visible by construction.
//
// 双状态并存规则（钉死项 2，写代码前定死）：**并列，不覆盖**。回执（瞬时）在前、
// 警示（持久）在后，两者同时为真时同屏显示 —— 隐藏任何一条都是撒谎。
// Dual-state rule (item two, fixed before coding): SHOW BOTH, never overwrite. The receipt
// (transient) comes first, the warning (persistent) second; when both are true they appear
// together, because hiding either one would be a lie.
static char s_excerpt_hint[32];       // 单发回执 / one-shot receipt text
static size_t s_excerpt_hint_page;    // 回执只对这一页有效（翻页即作废）/ valid on this page only
static bool s_excerpt_footer_pending; // 局部刷页脚的 pending 标志（照 reader_clock_tick 范式）/ footer-only refresh flag
// 26.11.1 真机轮第 2 回执：队列深度变化必须**跨拍**比对。
// 入队发生在两次 tick 之间，tick 内采的 `before` 永远等于 `after`——队列从 0 涨到 3，
// 页脚一次都没重画。基线因此存在模块静态里：谁改深度谁就欠一次重绘，跟 tick 的时序无关。
// The queue-depth baseline must live across ticks. An enqueue happens between ticks, so a
// `before` sampled inside the tick always equals `after` - the count went 0 -> 3 and the
// footer never repainted once. Whoever changes the depth now owes one repaint.
static unsigned s_excerpt_footer_seen;
// 回执等级（S26-4 收口账 5）：坏消息不被好消息顶掉。/ Receipt class: bad news outranks good.
typedef enum {
    EXCERPT_HINT_INFO = 0,  // 已存 ✓ / AI 已发 —— 顺耳的那类 / the pleasant kind
    EXCERPT_HINT_ALERT = 1, // 未发送 / 摘抄已满 / 内存不足 / [截] —— 要用户决定的那类
} excerpt_hint_class_t;
static uint8_t s_excerpt_hint_class;  // 槽里那条的等级 / class of the text in the slot

static void excerpt_hint_set(const char *text, excerpt_hint_class_t cls) {
    // 异常优先（S26-4 收口账 5）：同一拍里两条都真时，说坏的那条——它才是用户要处理的
    // 事。反过来的顶替（`已存 ✓` 盖掉 `未发送`）在 y 口径下是积压场景的常态，不是窄窗口。
    // Alert-first: when both are true in the same beat, keep the bad one - it is the thing
    // the user has to act on. The reverse overwrite was routine, not a narrow race.
    if (s_excerpt_hint[0] && s_excerpt_hint_page == s_page &&
        s_excerpt_hint_class == (uint8_t)EXCERPT_HINT_ALERT &&
        cls == EXCERPT_HINT_INFO)
        return;
    copy_text(s_excerpt_hint, sizeof(s_excerpt_hint), text);
    s_excerpt_hint_page = s_page;
    s_excerpt_hint_class = (uint8_t)cls;
}

/// 尽力时间戳（细化账 2/4）：拿不到可信 UTC 就返回 0，由 excerpt_store 退化为当次开机
/// 递增序号 `#n`——不扫文件、不承诺跨掉电连续。门槛取 2020-01-01：未校时的 RTC/新 `time()`
/// 起点（1970 附近、或被清成极小值）一律当作「不知道」，绝不写假时间戳。
/// Best-effort timestamp (items two and four): without a credible UTC we return 0 and let
/// excerpt_store fall back to the per-boot `#n` sequence - no file scan, no cross-power
/// continuity promise. The floor is 2020-01-01: an unset RTC or a fresh epoch start reads
/// as "unknown" rather than as a fabricated date.
static uint32_t book_now_unix(void) {
    const time_t now = time(NULL);
    return (now >= (time_t)1577836800) ? (uint32_t)now : 0u;
}

/// 页脚前缀：`未保存 · 回执 · 待落盘 N · `（各段可缺），按像素预算逐档降级。
/// Footer prefix, e.g. "未保存 · receipt · pending 3 · ", degraded step by step against a
/// pixel budget.
///
/// S26-3 细化账「多警示并存先算宽度」：三种警示（未保存 / 瞬时回执 / 待落盘）同屏时，
/// 全量前缀在 24 px 字号下的模型宽度是 **96 + 102 + 126 = 324 px**（CJK 24 px、ASCII
/// 半宽）；页脚可用宽度 = 728 − 进度串（最坏 ≈270）− 16 ≈ 442 px，扣掉章节名至少
/// 6 个全宽字（144 px）后，前缀预算 ≈298 px —— **放不下**。于是定死降级顺序：
///   L0 `未保存 · 回执 · 待落盘 N · `（324）
///   L1 `未保存 · 回执 · 待N · `（276，队列号改紧凑写法）
///   L2 `未保存 · 待落盘 N · `（222，丢瞬时回执）
///   L3 `未保存 · 待N · `（174）
/// **未保存与待落盘在任何一档都不丢**——它们是「卡上到底有没有」的唯一屏幕证据；
/// 回执只对本页负责，先让位。宽度用真实字宽现算，不靠上面的模型猜。
/// The hard facts (unsaved progress, queued excerpts) survive every level; the transient
/// receipt yields first. Widths are measured with the real face, not assumed from the model.
static bool excerpt_footer_fits(int width, int budget_px) {
    return budget_px <= 0 || width <= budget_px;
}

static void excerpt_footer_prefix(char *dst, size_t cap, int px, int budget_px) {
    dst[0] = 0;
    if (cap == 0) return;
    const bool hint_valid = s_excerpt_hint[0] && s_excerpt_hint_page == s_page;
    const unsigned pending = excerpt_store_pending();
    const char *unsaved = s_save_failed ? "未保存 · " : "";
    const int w_unsaved = ui_text_fixed_width_px(px, unsaved);

    char hint[40] = {0};
    int w_hint = 0;
    if (hint_valid) {
        copy_text(hint, sizeof(hint), s_excerpt_hint);
        strncat(hint, " · ", sizeof(hint) - strlen(hint) - 1);
        w_hint = ui_text_fixed_width_px(px, hint);
    }
    // 32/24：`%u` 最坏 10 位（"待落盘 4294967295 · " = 23B+NUL），编译器按最坏值算。
    // Sized for a ten-digit %u: the compiler assumes the worst case, and so do we.
    char warn_full[32] = {0}, warn_short[24] = {0};
    int w_full = 0, w_short = 0;
    if (pending) {
        snprintf(warn_full, sizeof(warn_full), "待落盘 %u · ", pending);
        snprintf(warn_short, sizeof(warn_short), "待%u · ", pending);
        w_full = ui_text_fixed_width_px(px, warn_full);
        w_short = ui_text_fixed_width_px(px, warn_short);
    }

    // 降级只走一条路：先压队列号，再丢回执，最后只留硬信息。
    // One ladder only: compact the count, then drop the receipt, then keep the hard facts.
    bool use_hint = hint_valid, use_warn = pending != 0, compact = false;
    if (use_warn &&
        !excerpt_footer_fits(w_unsaved + (use_hint ? w_hint : 0) + w_full, budget_px))
        compact = true;
    if (use_hint &&
        !excerpt_footer_fits(w_unsaved + w_hint + (use_warn ? (compact ? w_short : w_full) : 0),
                             budget_px))
        use_hint = false;
    if (use_warn && !compact &&
        !excerpt_footer_fits(w_unsaved + (use_hint ? w_hint : 0) + w_full, budget_px))
        compact = true;

    snprintf(dst, cap, "%s%s%s", unsaved, use_hint ? hint : "",
             use_warn ? (compact ? warn_short : warn_full) : "");
}

/// 页脚文字带（不含进度条），从 draw_reader 抽出以便异步回执单独重绘这一段。
/// The footer text band (without the progress bar), extracted from draw_reader so an
/// asynchronous receipt can repaint just this strip.
static void reader_footer_band_paint(uint8_t *fb, size_t page) {
    EpdRect track = progress_rect();
    // ---- B′ 条件 2：掉盘页脚，固定位 ----------------------------------------
    // 左端恒为「TF 卡已移除 [· 待落盘 N]」，不再是章节名。为什么固定：拔卡后章节名
    // 来源（书内导航 / 章节标题）都在卡上，拿不到就会退化成「未命名章节」——那是在
    // 用一个假标题盖住「卡没了」这个真事实。N 取 excerpt_store_pending()（唯一真值，
    // 不是估的），掉盘时摘抄照样入队、tick 失败留队，所以这个数会诚实地涨。
    // Fixed left slot while offline: chapter names come from the card, so falling back to
    // "untitled" would hide the real fact behind a fake title. N is the store's only truth;
    // excerpts still enqueue while the card is gone, so the count honestly grows.
    if (s_media_offline) {
        char left[64];
        const unsigned pending = excerpt_store_pending();
        if (pending) snprintf(left, sizeof(left), READER_OFFLINE_NOTE " · 待落盘 %u", pending);
        else copy_text(left, sizeof(left), READER_OFFLINE_NOTE);
        char offline_progress[48];
        // 掉盘不显示「全书 x%」：总字节是快照，百分比只在页脚右侧留页码，宁可少说一句。
        // No whole-book percentage while offline: keep the page numbers, say less.
        snprintf(offline_progress, sizeof(offline_progress), "%u/%u",
                 (unsigned)reader_visual_index(page) + 1, (unsigned)reader_visual_count());
        fit_fixed_text(left, 24, track.width - ui_text_fixed_width_px(24, offline_progress) - 16);
        ui_text_fixed(fb, track.x, track.y + 35, 24, left, EPD_DRAW_ALIGN_LEFT, false);
        ui_text_fixed(fb, track.x + track.width, track.y + 35, 24,
                      offline_progress, EPD_DRAW_ALIGN_RIGHT, false);
        return;
    }
    char chapter_name[72] = {0};
    reader_footer_chapter_name(chapter_name, sizeof(chapter_name));
    const int footer_px = 24;
    char progress[48];
    // 页码按视觉页计：双栏时一屏两页，总数不再"书变厚了"。
    // Page numbers count visual pages: two columns halve the total instead of fattening the book.
    snprintf(progress, sizeof(progress), "%u/%u · 全书 %u%%",
             (unsigned)reader_visual_index(page) + 1, (unsigned)reader_visual_count(), percent(page));
    char chapter_line[208];
    char excerpt_prefix[64];
    // 宽度预算（S26-3）：先扣掉右侧页码串，再给章节名留 6 个全宽字，剩下的才是前缀额度。
    // Width budget: subtract the right-aligned progress, reserve six full-width glyphs for
    // the chapter title, and give the rest to the prefix.
    const int avail_px = track.width - ui_text_fixed_width_px(footer_px, progress) - 16;
    const int chapter_min_px = footer_px * 6;
    const int prefix_budget = avail_px > chapter_min_px ? avail_px - chapter_min_px : avail_px;
    excerpt_footer_prefix(excerpt_prefix, sizeof(excerpt_prefix), footer_px, prefix_budget);
    snprintf(chapter_line, sizeof(chapter_line), "%s%s", excerpt_prefix, chapter_name);
    fit_fixed_text(chapter_line, footer_px, avail_px);
    // 页码和全书进度靠右固定，长章节名仅截短左侧；字号不跟随正文设置。
    // Keep page and book progress right-aligned; trim only long chapter titles and ignore body font settings.
    ui_text_fixed(fb, track.x, track.y + 35, footer_px, chapter_line, EPD_DRAW_ALIGN_LEFT, false);
    ui_text_fixed(fb, track.x + track.width, track.y + 35, footer_px,
                  progress, EPD_DRAW_ALIGN_RIGHT, false);
}

/// 顶部气泡的几何（画在 {(ui_w()-248)/2, 160, 248, 48}）：擦除/回执共用一个矩形，
/// 不许再写死坐标——26.11.1 实测：擦除矩形是竖屏时代硬编码的 {208,150,268,68}，
/// 横屏下气泡中心随 ui_w() 右移，硬编码擦不到，气泡只能等翻页陪葬。
/// One geometry for paint/erase/receipt. Portrait (684-248)/2-10 = 208, byte-identical
/// to the old constant; landscape follows ui_w(), where the constant missed entirely.
static EpdRect reader_notice_area(void) {
    return (EpdRect){(ui_w() - 248) / 2 - 10, 150, 268, 68};
}

/// 气泡的唯一写入口（26.11.4 B63：show/hide 各一行 log）。
/// 26.11.1 的教训是「视觉子句无日志出证」——用户说看见了、日志里一个字都没有，
/// 复盘只能靠记忆。show 在这里、hide 在主 tick 的擦除分支，两头都带方向与原文。
/// The single writer for the toast (26.11.4 B63: one log line on show AND on hide).
/// 26.11.1's lesson: a visual clause with no log trail can only be replayed from memory.
static void reader_notice_set(const char *text, uint32_t ms) {
    copy_text(s_reader_notice, sizeof(s_reader_notice), text);
    s_reader_notice_until = esp_timer_get_time() / 1000 + ms;
    ESP_LOGI(TAG, "notice: show \"%s\" %u ms", text, (unsigned)ms);
}

/// 回执的唯一可靠载体是**顶部气泡**。26.11.1 实测（toast 回执）：当前产品的全部
/// 开书入口都落 `s_requested_fullscreen = true`（281 行静态默认 + 各调用方全传 true），
/// 全屏不画页脚带——挂在页脚的「已存 ✓」= 构造上不可见。气泡在到期时由主 tick
/// 的擦除分支移除（同一几何），横竖屏都成立。
/// The toast is the only carrier that is visible by construction: every known open
/// path lands full-screen, and the full-screen reader never paints the footer band.
static bool excerpt_notice(app_ctx_t *ctx, const char *text) {
    if (s_view != READING || !s_text) return false;
    reader_notice_set(text, 2000);
    // 先整帧重画进气泡，再只推气泡区域——照主 tick 擦除分支的既有范式。
    // Render with the toast in, then drive only the toast region - the expiry path's pattern.
    render(ctx, ctx->fb);
    s_area = reader_notice_area();
    s_mode = MODE_GL16;
    return true;
}

/// 家务片驱动的一次落盘 + 回执。**页脚变化时返回 true**（调用方据此请求一次局部刷新），
/// 没有变化就什么都不做——不为一帧墨水屏多花一次 DU。
/// One flush driven by the housekeeping slot, plus the receipt. Returns true when the footer
/// changed (the caller asks for one local refresh); otherwise nothing happens - no extra DU
/// for a frame nobody asked for.
static bool excerpt_footer_tick(app_ctx_t *ctx) {
    // E-2（§19.4）：让行的观测点就放在**写卡前一行**。
    //
    // 挂载发生在 `sd_probe` 任务里，`poll_media` 最早下一拍才知道；而家务片每片都在跑。
    // 把「读代号 + 写让行」压在这里，观测与写卡之间没有别的事情可插——窗口物理上为 0，
    // 比「同一循环片内」更强。幂等由被调方守，与 poll_media 那一处重复调用是设计。
    // E-2: observe the epoch on the line right before the write attempt. The mount happens
    // on another task, so poll_media can be a tick late; the housekeeping slot runs every
    // turn. Observation and write end up adjacent - a zero-width window, stronger than
    // "same loop turn". The duplicate call with poll_media is intentional; the callee is
    // idempotent.
    excerpt_store_mount_yield(read_pico_sd_mount_epoch(), ctx->now_ms);
    const excerpt_flush_t r = excerpt_store_tick(ctx->now_ms);
    if (r == EXCERPT_FLUSH_IDLE) return false; // 无事可做 / nothing queued

    if (r == EXCERPT_FLUSH_OK) {
        // 「已存 ✓」的唯一来源：fsync 已经返回 0（先写后亮）。
        // The one source of "saved": fsync already returned 0 (write first, tick second).
        (void)excerpt_store_take_saved();
        excerpt_hint_set("已存 ✓", EXCERPT_HINT_INFO);
        ESP_LOGI(TAG, "excerpt: flushed, pending=%u", excerpt_store_pending());
        // 回执必须同时上顶部气泡：hint 槽只在页脚带消费，全屏不可达（见 excerpt_notice）。
        // The receipt must also hit the toast: the hint slot is footer-only, unreachable full-screen.
        if (excerpt_notice(ctx, "已存 ✓")) return true;
    } else if (r == EXCERPT_FLUSH_DROPPED) {
        // 26.9.29：卡上已有半行，这条被弃。**必须亮红**——静默丢用户数据比丢数据更坏。
        // 26.9.29: half a line is already on the card and this entry was dropped. It must go
        // red: silently losing a user's excerpt is worse than losing it.
        excerpt_hint_set("存储失败", EXCERPT_HINT_ALERT);
        ESP_LOGE(TAG, "excerpt: dropped after a partial write (%u so far), pending=%u",
                 excerpt_store_dropped(), excerpt_store_pending());
        // 丢数据比存数据更响：气泡同样要亮，且 2 s 不够就被用户下次操作顶掉。
        // Losing data must be louder than saving it: the toast fires here too.
        if (excerpt_notice(ctx, "存储失败")) return true;
    } else if (r == EXCERPT_FLUSH_FAILED) {
        ESP_LOGW(TAG, "excerpt: flush failed, keeping %u queued", excerpt_store_pending());
    }
    if (s_view != READING || !s_text || s_reader_fullscreen) return false;
    // 判据从「tick 内前后是否相等」改成「屏幕上的数是不是这个数」。
    // 旧写法漏的正是入队：深度在两次 tick 之间变了，`before == after` 恒成立，
    // 2050 行当场 `return false`，页脚永远停在旧数字（真机 3 次入队 / 0 次刷新）。
    // The test is no longer "did the count change inside this tick" but "is the number on
    // screen still this number". The old form missed enqueues by construction.
    if (r != EXCERPT_FLUSH_OK && excerpt_store_pending() == s_excerpt_footer_seen) return false;
    // 只重绘页脚这一条带（照 reader_clock_tick 的范式：fb 局部涂抹 + pending 标志）。
    // Repaint only the footer strip (the reader_clock_tick pattern: patch fb locally, flag pending).
    lock_draw(); reader_footer_band_paint(ctx->fb, s_page); unlock_draw();
    s_excerpt_footer_seen = excerpt_store_pending();
    s_excerpt_footer_pending = true;
    return true;
}

static int layout_name(uint8_t* fb, const char* name, int y, bool draw);
static EpdRect manage_panel(void) {
    int height = layout_name(NULL, s_managed.name, 0, false) + 495;
    return (EpdRect){UI_MARGIN - 16, 190 + (876 - height) / 2, ui_content_width() + 32, height};
}
static EpdRect manage_rect(int index, int count) {
    EpdRect panel = manage_panel();
    return ui_row_rect(index, count, panel.y + panel.height - 94, 76);
}
// 六个批量按钮排成两行各三格：上排选择与重扫，下排三个需要二次确认的操作。
// Six batch controls in two rows of three: selection and rescan on top, the three actions that
// ask for confirmation below.
static EpdRect batch_rect(int id) {
    return (EpdRect){36 + (id % 3) * 208, id < 3 ? 918 : 988, 196, 58};
}
static EpdRect manage_back_rect(void) { return (EpdRect){36, 92, 44, 44}; }
static EpdRect bulk_filter_rect(int id) { return (EpdRect){36 + id * 208, 216, 196, 70}; }
static EpdRect search_rect(int id) {
    if (id < 40) {
        int width = (ui_content_width() - 54) / 10;
        return (EpdRect){UI_MARGIN + id % 10 * (width + 6), 388 + id / 10 * 100, width, 88};
    }
    if (id < 43) return ui_row_rect(id - 40, 3, 808, 80);
    return ui_bar_rect(id - 43, 2);
}
static size_t selected_count(void) {
    size_t selected = 0;
    for (int i = 0; i < s_count; ++i) if (s_shelf[i].selected) ++selected;
    return selected;
}
static void clear_selection(void) {
    for (int i = 0; i < s_count; ++i) s_shelf[i].selected = false;
}
static void toggle_selection(int index) {
    if (index >= 0 && index < s_visible_count) s_shelf[index].selected = !s_shelf[index].selected;
}
static void select_page(int page) {
    for (int i = page * BOOK_BULK_ROWS; i < s_visible_count && i < (page + 1) * BOOK_BULK_ROWS; ++i) s_shelf[i].selected = true;
}

static void search_begin(void) {
    s_search_parent = s_view;
    memcpy(s_search_draft, s_query, sizeof(s_query));
    ui_text_edit_init(&s_search_input, s_search_draft, sizeof(s_search_draft));
    ui_keyboard_begin(&s_search_input, false);
    s_view = SEARCH;
}
static void refresh_search_matches(void) {
    for (int i = 0; i < s_count; ++i)
        s_shelf[i].search_match = read_pico_search_match(s_shelf[i].name, s_query);
}
static void search_finish(app_ctx_t* ctx, bool apply) {
    ui_keyboard_end();
    s_view = s_search_parent;
    if (apply) {
        memcpy(s_query, s_search_draft, sizeof(s_query));
        refresh_search_matches();
        clear_selection();
        sort_shelf(ctx);
        s_batch_message[0] = 0;
    }
    memset(s_search_draft, 0, sizeof(s_search_draft));
    ui_text_edit_init(&s_search_input, s_search_draft, sizeof(s_search_draft));
}

static bool pending_reserve(const char* path) {
    if (pending_find(path)) return true;
    pending_progress_t* p = heap_caps_malloc(sizeof(*p), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(sizeof(*p));
    if (!p) return false;
    memset(p, 0, sizeof(*p));
    copy_text(p->path, sizeof(p->path), path);
    // 阅读页内注册；传书 HTTP 尚未启动，退出后也不注销失败项。
    // Register in reading before transfer HTTP starts; failed records survive page exit.
    p->watch = book_progress_watch_create(path);
    if (!p->watch) { free(p); return false; }
    pending_progress_t** tail = &s_pending;
    while (*tail) tail = &(*tail)->next;
    *tail = p;
    return true;
}
static bool pending_restore(const char* path, uint32_t size, book_progress_t* out) {
    pending_progress_t* pending = pending_find(path);
    if (!pending || !pending->dirty || pending->value.file_size != size) return false;
    *out = pending->value;
    return true;
}
static void pending_discard(const char* path) {
    pending_progress_t** p = &s_pending;
    while (*p) {
        if (!strcmp((*p)->path, path)) {
            pending_progress_t* old = *p;
            *p = old->next;
            book_progress_watch_destroy(old->watch);
            free(old);
            break;
        }
        p = &(*p)->next;
    }
    s_save_failed = false;
    for (pending_progress_t* item = s_pending; item; item = item->next) if (item->dirty) s_save_failed = true;
}
static void pending_mark_latest(const char* path) {
    pending_progress_t** item = &s_pending;
    while (*item && strcmp((*item)->path, path)) item = &(*item)->next;
    if (*item) {
        pending_progress_t* current = *item;
        *item = current->next;
        current->next = NULL;
        pending_progress_t** tail = &s_pending;
        while (*tail) tail = &(*tail)->next;
        *tail = current;
    }
    copy_text(s_latest_path, sizeof(s_latest_path), path);
}
static void pending_drop_invalidated(void) {
    pending_progress_t* p = s_pending;
    while (p) {
        pending_progress_t* next = p->next;
        if (book_progress_watch_invalidated(p->watch)) {
            if (p->dirty) s_pending_invalidated = true;
            pending_discard(p->path);
        }
        p = next;
    }
}
static delete_retry_t* delete_retry_find(const char* path) {
    for (delete_retry_t* p = s_delete_retries; p; p = p->next)
        if (!strcmp(p->entry.path, path)) return p;
    return NULL;
}
static bool pending_flush(pending_progress_t* p) {
    if (!p->dirty) return true;
    if (!p->progress_saved) {
        if (book_progress_save(p->path, &p->value) != ESP_OK) return false;
        p->progress_saved = true;
    }
    if (!strcmp(p->path, s_latest_path) && book_progress_set_last_path(p->path) != ESP_OK) return false;
    p->dirty = false;
    return true;
}
static void retry_progress(void) {
    s_save_failed = false;
    bool had_dirty = false;
    pending_progress_t* p = s_pending;
    while (p) {
        pending_progress_t* next = p->next;
        bool dirty = p->dirty;
        had_dirty |= dirty;
        if (s_text && !strcmp(p->path, s_path)) { p = next; continue; }
        if (!pending_flush(p)) s_save_failed = true;
        else if (strcmp(p->path, s_path)) pending_discard(p->path);
        else if (dirty) s_unsaved = 0;
        p = next;
    }
    if (s_text && (s_unsaved || had_dirty)) save_progress();
    s_save_failed = false;
    for (pending_progress_t* item = s_pending; item; item = item->next) if (item->dirty) s_save_failed = true;
}

/* ---- 管理详情 / Management details ---- */
static int layout_name(uint8_t* fb, const char* name, int y, bool draw) {
    const char* at = name;
    while (*at) {
        char line[256];
        size_t used = 0;
        while (at[used]) {
            unsigned char first = (unsigned char)at[used];
            size_t n = first < 0x80 ? 1 : first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
            size_t remain = strlen(at + used);
            if (n > remain) n = 1;
            if (used + n >= sizeof(line)) break;
            memcpy(line + used, at + used, n);
            line[used + n] = 0;
            if (ttf_text_width_px(UI_PX_CAPTION, line) > ui_content_width() && used) break;
            used += n;
        }
        if (!used) break;
        line[used] = 0;
        if (draw) ui_text(fb, UI_MARGIN, y, UI_PX_CAPTION, line, EPD_DRAW_ALIGN_LEFT, false);
        y += 36;
        at += used;
    }
    return y;
}
static int draw_wrapped_name(uint8_t* fb, const char* name, int y) {
    return layout_name(fb, name, y, true);
}
static void draw_manage(uint8_t* fb) {
    EpdRect panel = manage_panel();
    ui_fill_round_rect(fb, panel, UI_BTN_RADIUS, UI_GRAY_WHITE);
    ui_draw_control_frame(fb, panel, UI_BTN_RADIUS, UI_GRAY_BLACK);
    ui_text(fb, UI_MARGIN, panel.y + 18, UI_PX_BODY,
            s_clear_confirm ? (s_delete_confirm ? "确认删除文件？" : "确认清除进度？") : "图书详情", EPD_DRAW_ALIGN_LEFT, false);
    int y = draw_wrapped_name(fb, s_managed.name, panel.y + 74) + 12;
    char info[96];
    snprintf(info, sizeof(info), "%s · %s · %.2f MB", s_managed.is_flash ? "内置存储" : "TF 卡",
             strrchr(s_managed.name, '.') ? strrchr(s_managed.name, '.') + 1 : "", s_managed.size / 1048576.0);
    ui_text(fb, UI_MARGIN, y, UI_PX_CAPTION, info, EPD_DRAW_ALIGN_LEFT, false);
    snprintf(info, sizeof(info), s_managed.has_progress ? "阅读进度 %u%%" : "尚无阅读进度", s_managed.pct);
    ui_text(fb, UI_MARGIN, y + 44, UI_PX_CAPTION, info, EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, UI_MARGIN, y + 88, UI_PX_CAPTION,
            s_managed.is_flash ? "/flash/books" : !strncmp(s_managed.path, "/sdcard/book/", 13) ? "/sdcard/book" : app_settings_books_dir(),
            EPD_DRAW_ALIGN_LEFT, false);
    if (s_manage_message[0]) ui_text(fb, UI_MARGIN, panel.y + panel.height - 270, UI_PX_CAPTION, s_manage_message, EPD_DRAW_ALIGN_LEFT, false);
    if (s_clear_confirm) {
        ui_text(fb, UI_MARGIN, panel.y + panel.height - 142, UI_PX_CAPTION, s_delete_confirm ? "删除后文件无法恢复" : "仅清阅读进度，保留图书文件", EPD_DRAW_ALIGN_LEFT, false);
        draw_control(fb, manage_rect(0, 2), "取消", 300);
        draw_control(fb, manage_rect(1, 2), s_delete_confirm ? "确认删除" : "确认清除", 301);
    } else if (s_file_removed) {
        draw_control(fb, manage_rect(0, 2), "关闭", 400);
        draw_control(fb, manage_rect(1, 2), "重试清理", 403);
    } else {
        draw_control(fb, (EpdRect){UI_MARGIN, panel.y + panel.height - 190, ui_content_width(), 68}, "前往文件管理重命名", 404);
        draw_control(fb, manage_rect(0, 3), "关闭", 400);
        draw_control(fb, manage_rect(1, 3), "清进度", 401);
        draw_control(fb, manage_rect(2, 3), "删除文件", 402);
    }
}

static void editor_start(void) {
    free(s_editor_cover);
    bool pending = false;
    s_editor_cover = load_cover_gray(s_managed.path, s_managed.name, "", false, &pending);
    copy_text(s_editor_title, sizeof(s_editor_title), s_managed.name);
    ui_text_edit_init(&s_editor_input, s_editor_title, sizeof(s_editor_title));
    s_editor_notice[0] = 0;
    ui_keyboard_begin(&s_editor_input, false);
    s_view = EDIT;
}



static void draw_editor(uint8_t* fb) {
    ui_clear_page(fb);
    ui_nav_status(fb);
    ui_nav_back(fb, 36, 79);
    ui_text_vc(fb, UI_LOCK_WIDTH / 2, 107, 31, "编辑书籍", EPD_DRAW_ALIGN_CENTER, false);
    ui_text_vc(fb, 646, 107, 26, "完成", EPD_DRAW_ALIGN_RIGHT, false);
    ui_hairline(fb, 133, 36, 612, UI_GRAY_LIGHT);
    EpdRect cover = {36, 175, 112, 152};
    ui_fill_rect_fast(cover, UI_GRAY_LIGHT, fb);
    if (s_editor_cover) {

        for (int y = 0; y < cover.height; ++y) {
            for (int x = 0; x < cover.width; ++x) {
                const uint8_t tone = ui_contrast_gray(
                    s_editor_cover[(y * BOOK_COVER_H / cover.height) * BOOK_COVER_W +
                                   x * BOOK_COVER_W / cover.width]);
                epd_draw_pixel(cover.x + x, cover.y + y,
                    ui_image_dither_gray(tone, cover.x + x, cover.y + y), fb);
            }
        }
    } else {
        char title[80]; copy_text(title, sizeof(title), s_managed.name);
        fit_text(title, 22, cover.width - 16);
        ui_text_vc(fb, cover.x + cover.width / 2, cover.y + cover.height / 2, 22, title, EPD_DRAW_ALIGN_CENTER, false);
    }
    ui_draw_round_rect(fb, cover, 0, UI_GRAY_BLACK);
    char title[128]; copy_text(title, sizeof(title), s_managed.name);
    fit_text(title, 30, 460);
    ui_text(fb, 174, 192, 30, title, EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, 174, 244, 22, "只修改书架显示名称", EPD_DRAW_ALIGN_LEFT, false);
    ui_hairline(fb, 356, 36, 612, UI_GRAY_LIGHT);
    ui_text(fb, 36, 378, 24, "书名", EPD_DRAW_ALIGN_LEFT, false);
    EpdRect field = {36, 424, 612, 83};
    ui_text_input_draw(fb, &s_editor_input, field, 31, false, NULL);
    if (s_editor_notice[0]) ui_text_fixed_vc(fb, 36, 530, 22, s_editor_notice, EPD_DRAW_ALIGN_LEFT, false);
    ui_keyboard_draw(fb, 560);
}
static void draw_search(uint8_t* fb) {
    ui_clear_page(fb); ui_nav_status(fb);
    ui_nav_back(fb, 36, 79);
    ui_text_vc(fb, 342, 107, 34, "搜索图书", EPD_DRAW_ALIGN_CENTER, false);
    ui_text_vc(fb, 646, 107, 25, "应用", EPD_DRAW_ALIGN_RIGHT, false);
    ui_text_input_draw(fb, &s_search_input, (EpdRect){UI_MARGIN, 200, ui_content_width(), 88}, UI_PX_BODY, false, NULL);
    ui_text_fixed_vc(fb, 36, 337, 23, "可输入中文、完整拼音、首字母或英文", EPD_DRAW_ALIGN_LEFT, false);
    ui_draw_button(fb, (EpdRect){36, 392, 180, 66}, "清空", false);
    ui_keyboard_draw(fb, 560);
}
static void draw_import(uint8_t* fb) {
    ui_clear_page(fb);
    ui_nav_status(fb);
    ui_nav_back(fb, 36, 79);
    ui_text_vc(fb, 342, 107, 34, "导入图书", EPD_DRAW_ALIGN_CENTER, false);
    ui_text(fb, 36, 166, 20, "选择导入方式", EPD_DRAW_ALIGN_LEFT, false);
    ui_hairline(fb, 207, 36, 612, UI_GRAY_LIGHT);
    static const char *titles[] = {"浏览 TF 卡", "WiFi 传书", "热点传书", "USB 读卡"};
    static const char *details[] = {
        "查看目录，打开或管理已有图书", "手机与墨水屏连接同一网络",
        "连接 kiikoread 热点后上传图书", "连接电脑，把图书放入 books 文件夹"
    };
    for (int i = 0; i < 4; ++i) {
        EpdRect r = import_rect(i);
        ui_fill_round_rect(fb, r, 22, UI_GRAY_WHITE);
        ui_draw_control_frame(fb, r, 22, 0xb0);
        ui_text(fb, r.x + 24, r.y + 25, 27, titles[i], EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, r.x + 24, r.y + 78, 19, details[i], EPD_DRAW_ALIGN_LEFT, false);
        ui_text_vc(fb, r.x + r.width - 24, r.y + r.height / 2, 26, "›", EPD_DRAW_ALIGN_RIGHT, false);
    }
    ui_text(fb, 36, 925, 19, "图书放入 TF 卡的 books 文件夹后会自动出现在书架。",
            EPD_DRAW_ALIGN_LEFT, false);
    ui_nav_draw(fb, 1);
}
static void draw_batch_confirmation(uint8_t* fb) {
    if (!s_batch_confirm) return;
    EpdRect panel = {54, 406, 576, 330};
    ui_fill_round_rect(fb, panel, 25, UI_GRAY_WHITE);
    ui_draw_control_frame(fb, panel, 25, 0x48);
    char title[96];
    const char* verb = s_batch_kind == BATCH_DELETE ? "删除" :
                       s_batch_kind == BATCH_CLEAR ? "清除进度：" : "从书架移除";
    const char* note = s_batch_kind == BATCH_DELETE ? "删除文件不可撤销，失败项可重试" :
                       s_batch_kind == BATCH_CLEAR ? "仅清阅读进度，所有文件保留" :
                       "只移出书架，文件与阅读进度都保留";
    snprintf(title, sizeof(title), "%s %u 本图书？", verb, (unsigned)selected_count());
    ui_text_vc(fb, 342, 477, 28, title, EPD_DRAW_ALIGN_CENTER, false);
    ui_text_vc(fb, 342, 541, 19, note, EPD_DRAW_ALIGN_CENTER, false);
    draw_control(fb, ui_row_rect(0, 2, 620, UI_BTN_H), "取消", 600);
    draw_control(fb, ui_row_rect(1, 2, 620, UI_BTN_H), "确认", 601);
}

static EpdRect bulk_nav_rect(int i) { return (EpdRect){36 + i * 208, UI_BAR_TOP, 196, UI_BAR_H}; }
static int bulk_nav_hit(int x, int y) {
    for (int i=0;i<3;++i) if(ui_rect_hit(bulk_nav_rect(i),x,y)) return i;
    return -1;
}
static void draw_bulk(uint8_t* fb, int leaf) {
    ui_clear_page(fb);
    ui_nav_status(fb);
    ui_nav_back(fb, 36, 79);
    ui_text_vc(fb, 342, 107, 34, "管理书架", EPD_DRAW_ALIGN_CENTER, false);
    char summary[96];
    snprintf(summary, sizeof(summary), "%d 本图书 · 已选 %u 本", s_visible_count, (unsigned)selected_count());
    ui_text(fb, 36, 166, 20, s_batch_message[0] ? s_batch_message : summary, EPD_DRAW_ALIGN_LEFT, false);
    ui_hairline(fb, 200, 36, 612, 0x70);
    const char *filters[] = {s_filter == 0 ? "全部来源" : s_filter == 1 ? "TF 卡" : "内置",
                             s_recent_sort ? "按最近" : "按名称", s_query[0] ? "搜索中" : "搜索"};
    for (int i = 0; i < 3; ++i) {
        EpdRect r = bulk_filter_rect(i);
        ui_fill_round_rect(fb, r, 20, UI_GRAY_WHITE);
        ui_draw_control_frame(fb, r, 20, 0x70);
        ui_text_vc(fb, r.x + r.width / 2, r.y + 35, 21, filters[i], EPD_DRAW_ALIGN_CENTER, false);
    }
    for (int row = 0; row < BOOK_BULK_ROWS; ++row) {
        int index = leaf * BOOK_BULK_ROWS + row;
        if (index >= s_visible_count) break;
        EpdRect r = row_rect(row);
        bool selected = s_shelf[index].selected;
        ui_fill_round_rect(fb, r, 18, selected ? 0xd0 : UI_GRAY_WHITE);
        ui_draw_control_frame(fb, r, 18, selected ? 0x60 : 0x70);
        char title[128]; copy_text(title, sizeof(title), s_shelf[index].name);
        fit_text(title, 26, 495);
        ui_text_vc(fb, r.x + 22, r.y + 31, 26, title, EPD_DRAW_ALIGN_LEFT, false);
        char detail[48];
        snprintf(detail, sizeof(detail), "%s · %s", s_shelf[index].is_flash ? "内置" : "TF 卡",
                 s_shelf[index].has_progress ? "已有阅读进度" : "未开始阅读");
        ui_text_vc(fb, r.x + 22, r.y + 62, 20, detail, EPD_DRAW_ALIGN_LEFT, false);
        epd_draw_circle(r.x + r.width - 37, r.y + 42, 12, 0x58, fb);
        if (selected) epd_fill_circle(r.x + r.width - 37, r.y + 42, 7, 0x38, fb);
    }
    if (!s_visible_count)
        ui_text_vc(fb, 342, 570, 26, s_message[0] ? s_message : "当前筛选没有图书", EPD_DRAW_ALIGN_CENTER, false);
    // 三列格宽只有 193 像素，标签控制在四字以内，放大系统字号也不会顶出按钮。
    // A three-column cell is 193 px wide, so labels stay within four characters and still fit
    // once the system font scale is turned up.
    static const char *labels[] = {"本页全选", "清除勾选", "重新扫描",
                                   "删除所选", "清进度", "移出书架"};
    for (int i = 0; i < 6; ++i) {
        EpdRect r = batch_rect(i);
        ui_fill_round_rect(fb, r, 18, UI_GRAY_WHITE);
        ui_draw_control_frame(fb, r, 18, 0x70);
        ui_text_vc(fb, r.x + r.width / 2, r.y + r.height / 2, 20, labels[i], EPD_DRAW_ALIGN_CENTER, false);
    }
    const char *nav[] = {"上一页", "完成", "下一页"};
    char page[48];
    snprintf(page, sizeof(page), "%d / %d 页", leaf + 1, leaves());
    ui_text_vc(fb, 342, 1072, 20, page, EPD_DRAW_ALIGN_CENTER, false);
    for (int i = 0; i < 3; ++i) {
        EpdRect r = bulk_nav_rect(i);
        bool available = i == 1 || (i == 0 ? leaf > 0 : leaf + 1 < leaves());
        ui_fill_round_rect(fb, r, 20, i == 1 ? 0x20 : UI_GRAY_WHITE);
        ui_draw_control_frame(fb, r, 20, available ? 0x70 : 0xb0);
        ui_text_vc(fb, r.x + r.width / 2, r.y + r.height / 2, 25, nav[i], EPD_DRAW_ALIGN_CENTER, i == 1);
    }
    draw_batch_confirmation(fb);
}
static void save_progress(void) {
    if (!s_text || !s_path[0] || !book_layout_page_count()) return;
    bool was_failed = s_save_failed;
    pending_progress_t* pending = pending_find(s_path);
    if (!pending) { s_save_failed = true; return; }
    book_progress_t p = {
        .file_size = s_file_size, .chapter = (uint16_t)s_chapter,
        .byte_off = (uint32_t)reader_page_offset(s_page),
        .px = (uint8_t)s_px, .pct = (uint8_t)percent(s_page),
        .last_open_s = 0,
    };
    if (!pending->dirty || pending->value.file_size != p.file_size || pending->value.chapter != p.chapter ||
        pending->value.byte_off != p.byte_off || pending->value.px != p.px || pending->value.pct != p.pct) {
        pending->value = p;
        pending->progress_saved = false;
    }
    pending->dirty = true;
    if (pending_flush(pending)) {
        s_unsaved = 0;
        s_save_failed = false;
        for (pending_progress_t* item = s_pending; item; item = item->next) if (item->dirty) s_save_failed = true;
    }
    else { if (!s_unsaved) s_unsaved = 1; s_save_failed = true; }
    if (was_failed != s_save_failed) invalidate_prep();
}
static void invalidate_prep(void) { s_next_page = s_prep_page = -1; s_next_generation=0; memset(&s_prep_cursor,0,sizeof(s_prep_cursor)); }

// 目录使用系统字体；切换前等待预渲染结束，回正文时恢复阅读字体与字形缓存。
// The directory uses the system face; join painting before switching and restore the reader face on return.
static void set_reader_view(book_view_t view) {
    s_reader_tap.pending = false;
    lock_draw();
    invalidate_prep();
    if (view == READING) app_font_activate_reading();
    else app_font_activate_system();
    s_view = view;
    unlock_draw();
}

/* ---- 阅读工具层 / Reader overlays ---- */
typedef struct {
    uint32_t magic, file_size, byte_off;
    uint16_t chapter;
    char path[BOOK_STORE_PATH_MAX];
} legacy_reader_bookmark_t;

typedef struct {
    uint16_t chapter;
    uint16_t reserved;
    uint32_t byte_off;
    uint32_t saved_s;
} reader_bookmark_entry_t;

typedef struct {
    uint32_t magic, file_size;
    uint16_t count, reserved;
    char path[BOOK_STORE_PATH_MAX];
    reader_bookmark_entry_t entries[BOOKMARK_MAX];
} reader_bookmarks_t;

static void bookmark_key(const char* path, char key[11]) {
    uint32_t hash = UINT32_C(2166136261);
    for (const unsigned char* p = (const unsigned char*)path; *p; ++p)
        hash = (hash ^ *p) * UINT32_C(16777619);
    snprintf(key, 11, "m_%08lx", (unsigned long)hash);
}
// 收藏按完整路径持久化，与章节书签分别存放。/ Persist whole-book favorites separately from page bookmarks.
static void favorite_key(const char *path, char key[11]) {
    uint32_t hash = UINT32_C(2166136261);
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p)
        hash = (hash ^ *p) * UINT32_C(16777619);
    snprintf(key, 11, "f_%08lx", (unsigned long)hash);
}
static bool favorite_read_handle(nvs_handle_t h, const char *path) {
    uint8_t value = 0;
    char key[11]; favorite_key(path, key);
    return nvs_get_u8(h, key, &value) == ESP_OK && value == 1;
}
static bool favorite_load(const char *path) {
    nvs_handle_t h;
    if (nvs_open("rp_favs", NVS_READONLY, &h) != ESP_OK) return false;
    bool value = favorite_read_handle(h, path);
    nvs_close(h);
    return value;
}
static bool favorite_save(const char *path, bool value) {
    nvs_handle_t h;
    if (nvs_open("rp_favs", NVS_READWRITE, &h) != ESP_OK) return false;
    char key[11]; favorite_key(path, key);
    esp_err_t err = value ? nvs_set_u8(h, key, 1) : nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND && !value) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

// 从书架移除只隐藏条目，不动文件也不清进度；再次打开这本书会重新上书架。
// Removing from the shelf hides the entry only: no file or progress is touched, and opening the
// book again puts it back. 值为完整路径，命中的同键记录属于别的书时拒绝覆盖。
// The value is the full path, so a colliding key owned by a different book is never overwritten.
#define SHELF_HIDDEN_NS "rp_shelf"
static void shelf_hidden_key(const char *path, char key[11]) {
    uint32_t hash = UINT32_C(2166136261);
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p)
        hash = (hash ^ *p) * UINT32_C(16777619);
    snprintf(key, 11, "h_%08lx", (unsigned long)hash);
}
static bool shelf_hidden_read_handle(nvs_handle_t h, const char *path) {
    char stored[BOOK_STORE_PATH_MAX];
    char key[11];
    shelf_hidden_key(path, key);
    size_t len = sizeof(stored);
    return nvs_get_str(h, key, stored, &len) == ESP_OK && strcmp(stored, path) == 0;
}
static bool shelf_hidden_load(const char *path) {
    nvs_handle_t h;
    if (nvs_open(SHELF_HIDDEN_NS, NVS_READONLY, &h) != ESP_OK) return false;
    bool hidden = shelf_hidden_read_handle(h, path);
    nvs_close(h);
    return hidden;
}
static bool shelf_hidden_save(const char *path, bool hidden) {
    nvs_handle_t h;
    if (nvs_open(SHELF_HIDDEN_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    char key[11];
    shelf_hidden_key(path, key);
    char stored[BOOK_STORE_PATH_MAX];
    size_t len = sizeof(stored);
    esp_err_t existing = nvs_get_str(h, key, stored, &len);
    esp_err_t err = existing == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : existing;
    if (existing == ESP_OK && strcmp(stored, path) != 0) err = ESP_ERR_INVALID_STATE;
    // 恢复上架也校验路径，避免擦掉碰撞键属于另一书的隐藏记录。
    // Check ownership when unhiding too, so a collision cannot erase a different book removal.
    if (err == ESP_OK && hidden) err = nvs_set_str(h, key, path);
    else if (err == ESP_OK) {
        err = nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if(err==ESP_OK)book_lock_collage_invalidate();
    return err == ESP_OK;
}
static bool bookmark_read_handle(nvs_handle_t h, reader_bookmarks_t* marks) {
    if (!marks || !s_path[0]) return false;
    memset(marks, 0, sizeof(*marks));
    char key[11]; bookmark_key(s_path, key);
    size_t size = 0;
    if (nvs_get_blob(h, key, NULL, &size) != ESP_OK) return false;
    if (size == sizeof(legacy_reader_bookmark_t)) {
        legacy_reader_bookmark_t old;
        size = sizeof(old);
        if (nvs_get_blob(h, key, &old, &size) != ESP_OK ||
            old.magic != UINT32_C(0x52504d31) || old.file_size != s_file_size ||
            strcmp(old.path, s_path)) return false;
        marks->magic = UINT32_C(0x52504d32);
        marks->file_size = old.file_size;
        marks->count = 1;
        copy_text(marks->path, sizeof(marks->path), old.path);
        marks->entries[0] = (reader_bookmark_entry_t){
            .chapter = old.chapter, .byte_off = old.byte_off,
        };
        return true;
    }
    if (size != sizeof(*marks) || nvs_get_blob(h, key, marks, &size) != ESP_OK) return false;
    return marks->magic == UINT32_C(0x52504d32) && marks->file_size == s_file_size &&
        marks->count <= BOOKMARK_MAX && !strcmp(marks->path, s_path);
}

static bool bookmark_load(reader_bookmarks_t* marks) {
    if (!marks || !s_path[0]) return false;
    nvs_handle_t h;
    if (nvs_open("rp_marks", NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = bookmark_read_handle(h, marks);
    nvs_close(h);
    return ok;
}
static bool bookmark_is_current(void) {
    reader_bookmarks_t marks;
    if (!bookmark_load(&marks)) return false;
    size_t offset = book_layout_page_start_offset(s_page);
    for (size_t i = 0; i < marks.count; ++i)
        if (marks.entries[i].chapter == s_chapter && marks.entries[i].byte_off == offset) return true;
    return false;
}
static size_t bookmark_count(void) {
    reader_bookmarks_t marks;
    return bookmark_load(&marks) ? marks.count : 0;
}
static int bookmark_row_step(void) {
    int scale=app_settings_system_font_size();
    if(scale<100)scale=100;
    if(scale>200)scale=200;
    return 82+(scale-100)/2;
}
static int bookmark_rows(void) { return (s_bookmark_edit?424:492)/bookmark_row_step(); }
static EpdRect bookmark_row_rect(int row) {
    int step=bookmark_row_step();
    return (EpdRect){36,656+row*step,612,step-10};
}
static int bookmark_pages(size_t count) {
    return count ? ((int)count + bookmark_rows() - 1) / bookmark_rows() : 1;
}
static unsigned bookmark_selected_count(void) {
    unsigned count = 0;
    for (unsigned i = 0; i < BOOKMARK_MAX; ++i) if (s_bookmark_selected & (UINT32_C(1) << i)) ++count;
    return count;
}
static void bookmark_compact(reader_bookmarks_t* marks, uint32_t selected) {
    size_t write = 0;
    for (size_t i = 0; i < marks->count; ++i)
        if (!(selected & (UINT32_C(1) << i))) marks->entries[write++] = marks->entries[i];
    marks->count = (uint16_t)write;
}
static bool bookmark_delete_selected(void) {
    if (!s_bookmark_selected || !s_path[0]) return false;
    nvs_handle_t h;
    if (nvs_open("rp_marks", NVS_READWRITE, &h) != ESP_OK) return false;
    reader_bookmarks_t marks;
    bool valid = bookmark_read_handle(h, &marks);
    esp_err_t err = valid ? ESP_OK : ESP_ERR_NOT_FOUND;
    if (valid) {
        bookmark_compact(&marks, s_bookmark_selected);
        char key[11]; bookmark_key(s_path, key);
        err = marks.count ? nvs_set_blob(h, key, &marks, sizeof(marks)) : nvs_erase_key(h, key);
        if (err == ESP_OK) err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) return false;
    s_bookmark_selected = 0;
    s_bookmark_page = 0;
    s_bookmark_edit = marks.count != 0;
    return true;
}
static bool bookmark_toggle(void) {
    if (!s_path[0]) return false;
    nvs_handle_t h;
    if (nvs_open("rp_marks", NVS_READWRITE, &h) != ESP_OK) return false;
    char key[11]; bookmark_key(s_path, key);
    reader_bookmarks_t marks;
    bool valid = bookmark_read_handle(h, &marks);
    if (!valid) {
        memset(&marks, 0, sizeof(marks));
        marks.magic = UINT32_C(0x52504d32);
        marks.file_size = s_file_size;
        copy_text(marks.path, sizeof(marks.path), s_path);
    }
    uint32_t offset = (uint32_t)book_layout_page_start_offset(s_page);
    size_t found = marks.count;
    for (size_t i = 0; i < marks.count; ++i)
        if (marks.entries[i].chapter == s_chapter && marks.entries[i].byte_off == offset) { found = i; break; }
    bool remove = found < marks.count;
    if (remove) {
        memmove(&marks.entries[found], &marks.entries[found + 1],
                (marks.count - found - 1) * sizeof(marks.entries[0]));
        --marks.count;
    } else {
        if (marks.count == BOOKMARK_MAX) {
            memmove(&marks.entries[0], &marks.entries[1],
                    (BOOKMARK_MAX - 1) * sizeof(marks.entries[0]));
            --marks.count;
        }
        time_t now = time(NULL);
        marks.entries[marks.count++] = (reader_bookmark_entry_t){
            .chapter = (uint16_t)s_chapter, .byte_off = offset,
            .saved_s = now >= 1704067200 ? (uint32_t)now : 0,
        };
    }
    esp_err_t err = marks.count ? nvs_set_blob(h, key, &marks, sizeof(marks)) : nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND && !marks.count) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        copy_text(s_reader_notice, sizeof(s_reader_notice), remove ? "书签已移除" : "书签已添加");
        s_reader_notice_until = esp_timer_get_time() / 1000 + 1600;
    } else {
        copy_text(s_reader_notice, sizeof(s_reader_notice), "书签保存失败");
        s_reader_notice_until = esp_timer_get_time() / 1000 + 2200;
    }
    return err == ESP_OK;
}

static void draw_reader_tool_icon(uint8_t* fb, int cx, int cy, int kind, bool selected) {
    // 已保存的书签用带勾的一档区分未保存；其余图标取自 k_tool_icons。
    // A saved bookmark uses the check variant; every other icon comes from k_tool_icons.
    if (kind < 0 || kind >= BOOK_TOOL_COUNT) return;
    const ui_icon_t icon = (kind == 1 && selected) ? UI_ICON_BOOKMARK_CHECK : k_tool_icons[kind];
    ui_draw_icon(fb, cx, cy, READER_TOOL_ICON_PX, icon, 0x38);
}

// 单行辅助文字保持在当前控件内；本机只使用当前活动字库，按原轮廓重新光栅化。
// Keep one-line UI text inside its slot, rasterizing native outlines in the existing active font.
static void reader_panel_text_vc(uint8_t *fb,int x,int cy,int px,const char *text,
                                  enum EpdFontFlags align,bool inverted) {
    int width=align==EPD_DRAW_ALIGN_LEFT?642-x:align==EPD_DRAW_ALIGN_RIGHT?x-42:
              2*(x<342?x-42:642-x);
    int left=align==EPD_DRAW_ALIGN_LEFT?x:align==EPD_DRAW_ALIGN_RIGHT?x-width:x-width/2;
    ui_text_box(fb,(EpdRect){left,cy-22,width,44},px,text,align,inverted);
}
static void reader_panel_text(uint8_t *fb,int x,int y,int px,const char *text,
                               enum EpdFontFlags align,bool inverted) {
    int height=ui_text_effective_px(px);if(height>44)height=44;
    reader_panel_text_vc(fb,x,y+height/2,px,text,align,inverted);
}

static void draw_sheet(uint8_t* fb, int top, const char* title) {
    EpdRect sheet = {0, top, UI_LOCK_WIDTH, UI_LOCK_HEIGHT - top + 28};
    ui_fill_round_rect(fb, sheet, 28, UI_GRAY_WHITE);
    ui_fill_rect_fast((EpdRect){0, top + 28, UI_LOCK_WIDTH, UI_LOCK_HEIGHT - top - 28}, UI_GRAY_WHITE, fb);
    ui_draw_control_frame(fb, sheet, 28, 0x58);
    ui_fill_round_rect(fb, (EpdRect){292, top + 14, 100, 7}, 3, 0x48);
    ui_text_box(fb,(EpdRect){110,top+26,464,66},30,title,EPD_DRAW_ALIGN_CENTER,false);
}

// 二级面板左上角的返回键：圆底加 Lucide chevron-left，各面板共用。
// Secondary-panel back control: a circle with the Lucide chevron-left, shared by the panels.
static void draw_sheet_back(uint8_t* fb, int top) {
    ui_click_feedback_register(fb, (EpdRect){32, top + 25, 64, 64}, UI_CLICK_BACK, UI_ICON_CHEVRON_LEFT);
    epd_draw_circle(64, top + 57, 24, UI_NAV_BACK_BORDER_GRAY, fb);
    epd_draw_circle(64, top + 57, 23, UI_NAV_BACK_BORDER_GRAY, fb);
    ui_draw_icon(fb, 64, top + 57, READER_SHEET_BACK_PX, UI_ICON_CHEVRON_LEFT, 0x38);
}

static void draw_pill_slider(uint8_t* fb, EpdRect r, const char* left, const char* right,
                             const char* value, int index, int count, int left_px, int right_px) {
    ui_fill_round_rect(fb, r, r.height / 2, 0xb8);
    ui_draw_control_frame(fb, r, r.height / 2, 0x50);
    int cy = r.y + r.height / 2;
    ui_text_box(fb,(EpdRect){r.x+12,cy-26,40,52},left_px,left,EPD_DRAW_ALIGN_CENTER,false);
    ui_text_box(fb,(EpdRect){r.x+r.width-52,cy-26,40,52},right_px,right,EPD_DRAW_ALIGN_CENTER,false);
    int span = r.width - 128;
    int cx = r.x + 64 + (count > 1 ? span * index / (count - 1) : 0);
    EpdRect track = {r.x + 59, cy - 3, r.width - 118, 6};
    ui_fill_round_rect(fb, track, 3, 0x78);
    track.width = cx - track.x;
    if (track.width > 0) ui_fill_round_rect(fb, track, 3, 0x40);
    epd_fill_circle(cx, cy, 37, UI_GRAY_WHITE, fb);
    epd_draw_circle(cx, cy, 37, 0x48, fb);
    ui_text_box(fb,(EpdRect){cx-31,cy-28,62,56},22,value,EPD_DRAW_ALIGN_CENTER,false);
}

static EpdRect reader_slider_refresh_rect(int slider) {
    EpdRect r = reader_slider_rect(slider);
    // The knob extends four pixels outside the pill. Include that margin so
    // moving it restores the old white footprint and the rail beneath it.
    r.x -= 8; r.y -= 8; r.width += 16; r.height += 16;
    return r;
}

static const char* font_friendly_name(const ttf_font_item_t* item) {
    return item ? ttf_font_localized_name(item->name) : "";
}

static void draw_font_name_with_current_face(uint8_t* fb, int x, int y, const char* name, int width) {
    int above = 0, below = 0;
    int px=ui_text_effective_px(25);
    while (px>1 && ttf_text_width_px(px,name)>width) --px;
    ttf_measure_line_px(px, name, &above, &below);
    ttf_draw_text_px(fb, x, y + (above - below) / 2, px, name,
                     EPD_DRAW_ALIGN_CENTER, UI_INK_BLACK, UI_INK_WHITE);
}

// 用户确认双列：字体/字重在第一行，书内字体/排版在第二行；绘制与命中共用矩形。
// Approved paired rows: face/weight, then embedded faces/layout; paint and touch share rectangles.
// 放大文字时向上展开面板，保持双列与底部完成键的位置；命中也使用这些矩形。
// Expand sheets upward for larger UI text, retaining paired columns and the Done button; hits share these rectangles.
static int reader_ui_extra(int maximum) {
    int scale = app_settings_system_font_size();
    if (scale < 100) scale = 100;
    if (scale > 200) scale = 200;
    return (scale-100)*maximum/100;
}
static int reader_font_top(void) { return 580-reader_ui_extra(112); }
static EpdRect reader_font_card(int index) {
    int extra=reader_ui_extra(48), footer=reader_ui_extra(16);
    if (index>=4) return (EpdRect){index==4?36:350,1054-footer,298,72+footer};
    int row=index/2;
    return (EpdRect){index%2?350:36,796+row*132-(2-row)*extra-footer,
                     298,(row?110:116)+extra};
}
#define s_font_card reader_font_card(0)
#define s_weight_card reader_font_card(1)
#define s_bookfont_card reader_font_card(2)
#define s_layout_card reader_font_card(3)
#define s_rule_card reader_font_card(4)
#define s_antialias_card reader_font_card(5)
static void draw_reading_switch(uint8_t *fb, EpdRect track, bool on);
static const EpdRect s_rule_offset_up = {36, 980, 190, 88};
static const EpdRect s_rule_offset_reset = {246, 980, 192, 88};
static const EpdRect s_rule_offset_down = {458, 980, 190, 88};

static int background_compare(const void *a, const void *b) {
    return strcasecmp(((const reader_background_item_t *)a)->path, ((const reader_background_item_t *)b)->path);
}
// 字体页入口已按用户要求移除；保留内部旧面板处理，便于安全结束正在运行的预览。
// The requested font-sheet entry is removed; retain the internal legacy panel to end an active preview safely.
static void reader_background_scan(void) __attribute__((unused));
static void reader_background_scan(void) {
    free(s_backgrounds); s_backgrounds = NULL; s_background_count = s_background_selected = s_background_page = 0;
    s_background_notice[0] = 0; s_background_ready = false;
    s_background_preview_notice[0] = 0; s_background_prepare_pending = false;
    DIR *dir = opendir("/sdcard/pictures");
    if (!dir) { copy_text(s_background_notice, sizeof(s_background_notice), "请在 pictures 中放入 JPG 或 PNG 图片"); return; }
    // A bounded catalogue, independent of book count. No image data in the catalogue.
    s_backgrounds = heap_caps_calloc(128, sizeof(*s_backgrounds), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_backgrounds) copy_text(s_background_notice, sizeof(s_background_notice), "内存不足，请关闭图书后重试");
    struct dirent *ent;
    while (s_backgrounds && (ent = readdir(dir))) {
        // AppleDouble ._ files and other hidden metadata are not images.
        if (ent->d_name[0] == '.') continue;
        const char *ext = strrchr(ent->d_name, '.');
        if (!ext || (strcasecmp(ext, ".jpg") && strcasecmp(ext, ".jpeg") && strcasecmp(ext, ".png"))) continue;
        char path[288]; struct stat st;
        int n = snprintf(path, sizeof(path), "/sdcard/pictures/%s", ent->d_name);
        if (n < 0 || n >= (int)sizeof(path) || stat(path, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0) continue;
        if (s_background_count == 128) { copy_text(s_background_notice, sizeof(s_background_notice), "仅显示前 128 张图片"); break; }
        strcpy(s_backgrounds[s_background_count++].path, path);
    }
    closedir(dir);
    if (s_background_count) qsort(s_backgrounds, s_background_count, sizeof(*s_backgrounds), background_compare);
    for (int i = 0; i < s_background_count; ++i)
        if (!strcmp(s_backgrounds[i].path, app_settings_book_background_path())) s_background_selected = i;
    s_background_page = s_background_selected / READER_BACKGROUND_ROWS;
    if (!s_background_count && !s_background_notice[0])
        copy_text(s_background_notice, sizeof(s_background_notice), "pictures 中暂无 JPG 或 PNG 图片");
}
static int reader_background_pages(void) {
    return s_background_count ? (s_background_count + READER_BACKGROUND_ROWS - 1) / READER_BACKGROUND_ROWS : 1;
}
static EpdRect reader_background_row(int row) { return (EpdRect){36, 344 + row * 96, 612, 96}; }
static void draw_background_directory(uint8_t *fb) {
    draw_sheet(fb, 200, "阅读背景"); draw_sheet_back(fb, 200);
    reader_panel_text(fb, 42, 303, 23, "pictures", EPD_DRAW_ALIGN_LEFT, false);
    char count[32]; snprintf(count, sizeof(count), "%d 张图片", s_background_count);
    reader_panel_text(fb, 642, 303, 22, count, EPD_DRAW_ALIGN_RIGHT, false);
    ui_fill_round_rect(fb, (EpdRect){36,344,612,672}, 20, UI_GRAY_WHITE);
    ui_draw_control_frame(fb, (EpdRect){36,344,612,672}, 20, 0x58);
    for (int row = 0; row < READER_BACKGROUND_ROWS; ++row) {
        int index = s_background_page * READER_BACKGROUND_ROWS + row;
        if (index >= s_background_count) break;
        EpdRect r = reader_background_row(row);
        bool active = !strcmp(s_backgrounds[index].path, app_settings_book_background_path());
        if (active) ui_fill_round_rect(fb, (EpdRect){r.x+6,r.y+6,r.width-12,r.height-12}, 16, 0xe0);
        if (row) ui_hairline(fb, r.y, r.x+20, r.width-40, 0x78);
        ui_draw_icon(fb, 78, r.y+48, 30, UI_ICON_FILE, 0x38);
        const char *base = strrchr(s_backgrounds[index].path, '/');
        char name[128]; copy_text(name, sizeof(name), base ? base+1 : s_backgrounds[index].path);
        fit_text(name, 25, 470);
        reader_panel_text_vc(fb, 112, r.y+48, 25, name, EPD_DRAW_ALIGN_LEFT, false);
        if (active) epd_fill_circle(595, r.y+48, 4, UI_GRAY_BLACK, fb);
        ui_draw_icon(fb, 623, r.y+48, 23, UI_ICON_CHEVRON_RIGHT, 0x38);
    }
    if (s_background_notice[0]) {
        char notice[96]; copy_text(notice, sizeof(notice), s_background_notice);
        fit_text(notice, 21, 572);
        reader_panel_text_vc(fb, 342, s_background_count ? 1098 : 655, 21, notice, EPD_DRAW_ALIGN_CENTER, false);
    }
    char page[32]; snprintf(page, sizeof(page), "%d / %d", s_background_page+1, reader_background_pages());
    ui_draw_button(fb, (EpdRect){36,1040,190,52}, "上一页", false);
    reader_panel_text_vc(fb, 342, 1066, 22, page, EPD_DRAW_ALIGN_CENTER, false);
    ui_draw_button(fb, (EpdRect){458,1040,190,52}, "下一页", false);
    ui_draw_button(fb, (EpdRect){36,1120,294,72}, "无背景", false);
    ui_draw_button(fb, (EpdRect){354,1120,294,72}, "返回", false);
}
static void draw_background_picker(uint8_t *fb) {
    draw_sheet(fb, 200, "背景预览"); draw_sheet_back(fb, 200);
    EpdRect preview = {157, 316, 369, 656}; // 9:16, matching the complete wallpaper canvas.
    ui_fill_rect_fast(preview, UI_GRAY_WHITE, fb);
    if (s_background_ready) ui_reader_background_paint(fb, preview, false);
    ui_draw_control_frame(fb, preview, 0, UI_GRAY_BLACK);
    char name[128] = "暂无图片", position[48];
    if (s_background_count) {
        const char *base = strrchr(s_backgrounds[s_background_selected].path, '/');
        copy_text(name, sizeof(name), base ? base + 1 : "背景");
    }
    fit_text(name, 24, 584);
    reader_panel_text_vc(fb, 342, 1002, 24, name, EPD_DRAW_ALIGN_CENTER, false);
    snprintf(position, sizeof(position), "%d/%d · 上下滑动预览", s_background_count ? s_background_selected + 1 : 0, s_background_count);
    reader_panel_text_vc(fb, 342, 1046, 21, s_background_preview_notice[0] ? s_background_preview_notice : position, EPD_DRAW_ALIGN_CENTER, false);
    ui_draw_button(fb, (EpdRect){36, 1100, 190, 76}, "无背景", false);
    ui_draw_button(fb, (EpdRect){246, 1100, 192, 76}, "返回", false);
    ui_draw_button(fb, (EpdRect){458, 1100, 190, 76}, "使用背景", true);
}
static void reader_background_preview(int index) {
    if (index < 0 || index >= s_background_count) return;
    s_background_selected = index; s_background_page = index / READER_BACKGROUND_ROWS;
    s_reader_panel = READER_PANEL_BACKGROUND_PREVIEW;
    s_background_ready = false; s_background_prepare_pending = true;
    s_background_preview_notice[0] = 0;
}
static app_redraw_t reader_background_step(int dir) {
    if (s_reader_panel == READER_PANEL_BACKGROUND) {
        int next = s_background_page + dir;
        if (next < 0 || next >= reader_background_pages()) return APP_REDRAW_NONE;
        s_background_page = next; return APP_REDRAW_PAGE;
    }
    if (s_reader_panel == READER_PANEL_BACKGROUND_PREVIEW)
        reader_background_preview(s_background_selected + dir);
    return APP_REDRAW_NONE; // Preview decoding is deferred to the shallow tick stack.
}
static app_redraw_t reader_background_action(uint16_t x, uint16_t y) {
    const EpdRect back = {32,225,64,64};
    if (s_reader_panel == READER_PANEL_BACKGROUND_PREVIEW) {
        bool close = y < 200 || ui_rect_hit(back, x, y);
        if (y >= 1100 && y < 1176) {
            if (x >= 36 && x < 226) {
                if (!app_settings_set_book_background_path("")) return APP_REDRAW_NONE;
                close = true;
            } else if (x >= 246 && x < 438) close = true;
            else if (x >= 458 && x < 648) {
                if (!s_background_count || !s_background_ready ||
                    !app_settings_set_book_background_path(s_backgrounds[s_background_selected].path)) return APP_REDRAW_NONE;
                close = true;
            }
        }
        if (!close) return APP_REDRAW_NONE;
        s_background_prepare_pending = false; s_background_ready = false;
        s_reader_panel = READER_PANEL_BACKGROUND;
        invalidate_prep(); return APP_REDRAW_PAGE;
    }
    if (y < 200 || ui_rect_hit(back, x, y) || ui_rect_hit((EpdRect){354,1120,294,72}, x, y)) {
        s_reader_panel = READER_PANEL_FONT_SETTINGS;
        s_background_prepare_pending = false;
        free(s_backgrounds); s_backgrounds = NULL; s_background_count = s_background_page = 0;
        invalidate_prep(); return APP_REDRAW_PAGE;
    }
    if (ui_rect_hit((EpdRect){36,1120,294,72}, x, y)) {
        if (!app_settings_set_book_background_path("")) return APP_REDRAW_NONE;
        invalidate_prep(); return APP_REDRAW_PAGE;
    }
    if (ui_rect_hit((EpdRect){36,1040,190,52}, x, y)) return reader_background_step(-1);
    if (ui_rect_hit((EpdRect){458,1040,190,52}, x, y)) return reader_background_step(1);
    for (int row = 0; row < READER_BACKGROUND_ROWS; ++row) {
        int index = s_background_page * READER_BACKGROUND_ROWS + row;
        if (index < s_background_count && ui_rect_hit(reader_background_row(row), x, y)) {
            reader_background_preview(index); invalidate_prep(); return APP_REDRAW_NONE;
        }
    }
    return APP_REDRAW_NONE;
}
static app_redraw_t reader_background_tick(void) {
    if (s_view != READING || s_reader_panel != READER_PANEL_BACKGROUND_PREVIEW || !s_background_prepare_pending)
        return APP_REDRAW_NONE;
    s_background_prepare_pending = false;
    s_background_ready = s_background_count && s_background_selected >= 0 &&
        s_background_selected < s_background_count && ui_reader_background_prepare(s_backgrounds[s_background_selected].path);
    if (!s_background_ready) copy_text(s_background_preview_notice, sizeof(s_background_preview_notice), "图片无法解码或内存不足，请换一张");
    invalidate_prep(); return APP_REDRAW_PAGE;
}
static void draw_font_pair(uint8_t *fb, EpdRect card, const char *title, const char *value) {
    ui_fill_round_rect(fb, card, 20, UI_GRAY_WHITE);
    ui_draw_control_frame(fb, card, 20, 0x70);
    ui_text_box(fb,(EpdRect){card.x+21,card.y+6,card.width-42,card.height/2-12},23,title,EPD_DRAW_ALIGN_LEFT,false);
    char shown[64]; copy_text(shown, sizeof(shown), value);
    if (!strcmp(title,"阅读字体")) fit_fixed_text(shown,ui_text_effective_px(27),card.width-76);
    ui_text_box(fb,(EpdRect){card.x+21,card.y+card.height/2,card.width-76,card.height/2-8},27,shown,EPD_DRAW_ALIGN_LEFT,false);
    ui_draw_icon(fb,card.x+card.width-23,card.y+3*card.height/4,26,UI_ICON_CHEVRON_RIGHT,0x38);
}
static void reader_weight_value(char *out,size_t size,int steps){
    if(!steps){snprintf(out,size,"标准");return;}
    unsigned amount=steps<0?-steps:steps,whole=amount/16,fraction=(amount%16)*625;
    snprintf(out,size,"%c%u.%04u",steps<0?'-':'+',whole,fraction);
    size_t n=strlen(out);while(n&&out[n-1]=='0')out[--n]=0;if(n&&out[n-1]=='.')out[--n]=0;
}
static void draw_font_settings(uint8_t* fb) {
    draw_sheet(fb, reader_font_top(), "字体设置");
    int shown_px = s_reader_slider >= 0 ? s_reader_preview_px : s_px;
    char value[16]; snprintf(value, sizeof(value), "%d", shown_px);
    draw_pill_slider(fb, reader_slider_rect(0), "A", "A", value,
                     shown_px - BOOK_PX_MIN, BOOK_PX_MAX - BOOK_PX_MIN + 1, 20, 31);
    draw_font_pair(fb, s_font_card, "阅读字体", ttf_font_display_name());
    char weight_value[20];reader_weight_value(weight_value,sizeof(weight_value),app_settings_book_weight_fine());
    draw_font_pair(fb, s_weight_card, "字重", weight_value);
    draw_font_pair(fb, s_layout_card, "排版设置", "边距 · 行距");
    const bool book_fonts = app_settings_book_fonts();
    EpdRect card = s_bookfont_card;
    ui_fill_round_rect(fb, card, 20, UI_GRAY_WHITE); ui_draw_control_frame(fb, card, 20, 0x70);
    ui_text_box(fb,(EpdRect){card.x+21,card.y+6,card.width-42,card.height/2-12},23,"书内自带字体",EPD_DRAW_ALIGN_LEFT,false);
    ui_text_box(fb,(EpdRect){card.x+21,card.y+card.height/2,card.width-122,card.height/2-8},27,book_fonts?"开启":"关闭",EPD_DRAW_ALIGN_LEFT,false);
    EpdRect toggle = {card.x+card.width-89,card.y+3*card.height/4-19,69,39};
    ui_fill_round_rect(fb, toggle, 19, book_fonts ? 0x38 : 0xd0);
    int knob = toggle.x + (book_fonts ? 49 : 20);
    epd_fill_circle(knob, toggle.y + 19, 16, UI_GRAY_WHITE, fb);
    epd_draw_circle(knob, toggle.y + 19, 16, 0x78, fb);
    EpdRect rule = s_rule_card;
    ui_fill_round_rect(fb, rule, 20, UI_GRAY_WHITE); ui_draw_control_frame(fb, rule, 20, 0x70);
    static const char *const rule_names[] = {"无", "虚线", "点线"};
    int selected_rule = app_settings_book_reading_line();
    ui_text_box(fb,(EpdRect){rule.x+22,rule.y+10,142,rule.height-20},25,"阅读线",EPD_DRAW_ALIGN_LEFT,false);
    ui_text_box(fb,(EpdRect){rule.x+172,rule.y+10,77,rule.height-20},25,rule_names[selected_rule],EPD_DRAW_ALIGN_RIGHT,false);
    ui_draw_icon(fb,rule.x+rule.width-24,rule.y+rule.height/2,27,UI_ICON_CHEVRON_RIGHT,0x38);
    EpdRect aa = s_antialias_card;
    ui_fill_round_rect(fb, aa, 20, UI_GRAY_WHITE); ui_draw_control_frame(fb, aa, 20, 0x70);
    ui_text_box(fb,(EpdRect){aa.x+21,aa.y+10,aa.width-122,aa.height-20},25,"抗锯齿",EPD_DRAW_ALIGN_LEFT,false);
    draw_reading_switch(fb,(EpdRect){aa.x+aa.width-89,aa.y+aa.height/2-19,69,39},app_settings_book_antialias());
    ui_fill_round_rect(fb, (EpdRect){36, 1141, 612, 58}, 20, UI_GRAY_BLACK);
    reader_panel_text_vc(fb, 342, 1170, 28, "完成", EPD_DRAW_ALIGN_CENTER, true);
}

static EpdRect reader_weight_rect(int index) { return (EpdRect){index?542:56,909,86,86}; }
static int reader_weight_step(int x,int y) {
    if(ui_rect_hit(reader_weight_rect(0),x,y))return -1;
    if(ui_rect_hit(reader_weight_rect(1),x,y))return 1;
    return 0;
}
static void draw_weight_settings(uint8_t *fb) {
    draw_sheet(fb, 580, "字重");
    draw_sheet_back(fb, 580);
    reader_panel_text_vc(fb, 342, 730, 22, "调整阅读文字粗细", EPD_DRAW_ALIGN_CENTER, false);
    ttf_font_select(TTF_FONT_SLOT_SYSTEM);
    int weight=app_settings_book_weight_fine(),prior_weight=ttf_get_stroke_adjust_fine();
    ttf_set_stroke_adjust_fine(weight);
    ttf_draw_text_px(fb, 342, 833, 48, "阅读文字 Aa 123", EPD_DRAW_ALIGN_CENTER, 0, 15);
    ttf_set_stroke_adjust_fine(prior_weight);
    ui_fill_round_rect(fb,(EpdRect){36,895,612,114},57,0xe0);
    ui_draw_control_frame(fb,(EpdRect){36,895,612,114},57,0x30);
    for(int i=0;i<2;++i){EpdRect control=reader_weight_rect(i);epd_fill_circle(control.x+43,952,43,UI_GRAY_WHITE,fb);ui_draw_control_frame(fb,control,43,0x30);}
    ui_fill_rect_fast((EpdRect){87,951,24,3},weight<=-APP_BOOK_WEIGHT_FINE_MAX?0xa0:0x20,fb);
    ui_fill_rect_fast((EpdRect){573,951,24,3},weight>=APP_BOOK_WEIGHT_FINE_MAX?0xa0:0x20,fb);
    ui_fill_rect_fast((EpdRect){584,941,3,24},weight>=APP_BOOK_WEIGHT_FINE_MAX?0xa0:0x20,fb);
    char value[20];reader_weight_value(value,sizeof(value),weight);
    reader_panel_text_vc(fb,342,936,44,value,EPD_DRAW_ALIGN_CENTER,false);
    reader_panel_text_vc(fb,342,975,20,"像素 · 每次 1/16",EPD_DRAW_ALIGN_CENTER,false);
    reader_panel_text_vc(fb, 342, 1030, 20, "标准保留字体原始笔画", EPD_DRAW_ALIGN_CENTER, false);
    reader_panel_text_vc(fb, 342, 1070, 20, "同时适用于导入字体和书内字体", EPD_DRAW_ALIGN_CENTER, false);
    ui_fill_round_rect(fb, (EpdRect){36, 1141, 612, 58}, 20, UI_GRAY_BLACK);
    reader_panel_text_vc(fb, 342, 1170, 26, "完成", EPD_DRAW_ALIGN_CENTER, true);
}

static EpdRect rule_style_rect(int index) {
    return (EpdRect){36 + index * 208, 788, 196, 120};
}

static void draw_rule_settings(uint8_t *fb) {
    const int top = 580;
    draw_sheet(fb, top, "阅读线");
    draw_sheet_back(fb, top);
    reader_panel_text(fb, 42, 741, 21, "选择样式", EPD_DRAW_ALIGN_LEFT, false);
    static const char *const names[] = {"无", "虚线", "点线"};
    int selected = app_settings_book_reading_line();
    for (int i = 0; i < 3; ++i) {
        EpdRect card = rule_style_rect(i);
        ui_fill_round_rect(fb, card, 20, selected == i ? 0xc8 : 0xe8);
        ui_draw_control_frame(fb, card, 20, selected == i ? 0x40 : 0x78);
        reader_panel_text_vc(fb, card.x + card.width / 2, card.y + 37, 24, names[i], EPD_DRAW_ALIGN_CENTER, false);
        if (i) {
            int span = i == 1 ? 14 : 3, step = i == 1 ? 24 : 13;
            int start = card.x + 25, end = card.x + card.width - 25;
            for (int x = start; x < end; x += step)
                ui_fill_rect_fast((EpdRect){x, card.y + 83, x + span <= end ? span : end - x, 2}, 0x48, fb);
        } else reader_panel_text_vc(fb, card.x + card.width / 2, card.y + 83, 17, "不显示", EPD_DRAW_ALIGN_CENTER, false);
    }
    char offset[36];
    snprintf(offset, sizeof(offset), "上下位置  %+d", app_settings_book_reading_line_offset());
    reader_panel_text(fb, 42, 936, 21, offset, EPD_DRAW_ALIGN_LEFT, false);
    const EpdRect controls[] = {s_rule_offset_up, s_rule_offset_reset, s_rule_offset_down};
    const char *const labels[] = {"上移 2px", "恢复居中", "下移 2px"};
    for (int i = 0; i < 3; ++i) {
        ui_fill_round_rect(fb, controls[i], 20, selected ? 0xe0 : 0xf0);
        ui_draw_control_frame(fb, controls[i], 20, selected ? 0x60 : 0xa0);
        reader_panel_text_vc(fb, controls[i].x + controls[i].width / 2,
                   controls[i].y + controls[i].height / 2, 22, labels[i], EPD_DRAW_ALIGN_CENTER, false);
    }
    if (!selected) reader_panel_text(fb, 42, 1080, 18, "选择虚线或点线后可调整位置", EPD_DRAW_ALIGN_LEFT, false);
    EpdRect done = {36, 1113, 612, 72};
    ui_fill_round_rect(fb, done, 20, UI_GRAY_BLACK);
    reader_panel_text_vc(fb, 342, 1149, 23, "返回字体设置", EPD_DRAW_ALIGN_CENTER, true);
}

static EpdRect indent_adjust_rect(int index) {
    const EpdRect controls[] = {{214,1050,100,64}, {322,1050,208,64}, {538,1050,100,64}};
    return controls[index];
}
static void draw_layout_settings(uint8_t* fb) {
    const int top = 500;
    draw_sheet(fb, top, "排版设置");
    draw_sheet_back(fb, top);
    int shown_margin = s_reader_slider >= 0 ? s_reader_preview_margin : s_margin;
    int shown_line = s_reader_slider >= 0 ? s_reader_preview_line : app_settings_book_line_spacing();
    int shown_para = s_reader_slider >= 0 ? s_reader_preview_para : app_settings_book_paragraph_spacing();
    int shown_tracking = s_reader_slider >= 0 ? s_reader_preview_tracking : app_settings_book_tracking();
    int margin_widths[BOOK_MARGIN_CHOICES], margin_values[BOOK_MARGIN_CHOICES];
    int margin_count = reader_margin_levels(s_px, (shown_tracking - 2) * 2,
                                             margin_widths, margin_values);
    draw_pill_slider(fb, reader_slider_rect(1), "小", "大", "边距",
                     reader_margin_level_for(shown_margin, s_px, (shown_tracking - 2) * 2),
                     margin_count, 21, 21);
    int line_i = shown_line - 110;
    if (line_i < 0) line_i = 0;
    if (line_i > 40) line_i = 40;
    draw_pill_slider(fb, reader_slider_rect(2), "紧", "松", "行距", line_i, 41, 21, 21);
    reader_panel_text(fb, 42, 715, 21, "段距", EPD_DRAW_ALIGN_LEFT, false);
    char value[16]; snprintf(value, sizeof(value), "%d%%", shown_para);
    draw_pill_slider(fb, reader_slider_rect(3), "紧", "松", value, shown_para / 25, 4, 21, 21);
    reader_panel_text(fb, 42, 835, 21, "字间距", EPD_DRAW_ALIGN_LEFT, false);
    static const char* track_names[] = {"-4", "-2", "默认", "+2", "+4"};
    draw_pill_slider(fb, reader_slider_rect(4), "紧", "松", track_names[shown_tracking], shown_tracking, 5, 21, 21);
    ui_text_box(fb,(EpdRect){42,970,154,64},24,"首行缩进",EPD_DRAW_ALIGN_LEFT,false);
    static const char* indent_names[] = {"无", "1字", "2字", "3字"};
    int indent = app_settings_book_indent();
    for (int i = 0; i < 4; ++i) {
        EpdRect choice = {214 + i * 108, 970, 100, 64};
        bool selected = i == indent;
        ui_fill_round_rect(fb, choice, 16, selected ? UI_GRAY_BLACK : 0xe4);
        ui_draw_control_frame(fb, choice, 16, selected ? UI_GRAY_BLACK : 0x98);
        ui_text_box(fb,(EpdRect){choice.x+8,choice.y+5,choice.width-16,choice.height-10},23,indent_names[i],EPD_DRAW_ALIGN_CENTER,selected);
    }
    ui_text_box(fb,(EpdRect){42,1050,154,64},24,"缩进微调",EPD_DRAW_ALIGN_LEFT,false);
    char adjustment[28]; snprintf(adjustment, sizeof(adjustment), "%+d px · 复位", app_settings_book_indent_adjust());
    const char *labels[] = {"−", adjustment, "+"};
    for (int i = 0; i < 3; ++i) {
        EpdRect control = indent_adjust_rect(i);
        ui_fill_round_rect(fb, control, 16, 0xf0);
        ui_draw_control_frame(fb, control, 16, 0x50);
        ui_text_box(fb,(EpdRect){control.x+8,control.y+5,control.width-16,control.height-10},i==1?22:31,labels[i],EPD_DRAW_ALIGN_CENTER,false);
    }
    EpdRect back = {36, 1132, 612, 72};
    ui_fill_round_rect(fb, back, 20, UI_GRAY_BLACK);
    reader_panel_text_vc(fb, 342, 1168, 24, "返回字体设置", EPD_DRAW_ALIGN_CENTER, true);
}

/* 阅读设置面板：上游 r91 的半屏双列卡片壳，行表（我方阶段 0 的不变量）驱动槽位几何、
   绘制、热区与派发，加一项只改这张表。第八槽「横屏阅读」与第九槽「横屏双栏」（第 4 步）
   都是我方功能，故为两列五行（第 4 步起：槽高 76、间距 10，垂直包络与 8 槽时代逐位一致
   ——末行底仍停在 1122，「完成」上约 19px；第 9 槽独占末行左半格）。
   Upstream r91's two-column card shell; a row table (our stage-0 invariant) drives slot
   geometry, drawing, hot zones and dispatch, so adding an entry means editing this table
   only. Slot 8 (横屏阅读) and slot 9 (横屏双栏, step 4) are ours, hence two columns x five
   rows (from step 4: 76px cards, 10px gaps, the vertical envelope unchanged from the
   8-slot era — the last row's bottom still rests at 1122, ~19px above Done; slot 9 sits
   alone in the last row's left half). */
#define READING_ROW_KEYS   4   ///< 按键控制：进按键子面板 / keys row: opens the key sub-panel
#define READING_ROW_FULL   5   ///< 自动全刷值循环卡片 / auto full-refresh cycling card
#define READING_ROW_WAVE   6   ///< 翻页效果卡片 / page-turn effect card
#define READING_ROW_TURN   7   ///< 翻页模式：进子面板 / turn mode: opens the turn sub-panel
#define READING_ROW_COUNT  11  ///< 槽位数（仿生阅读新增第 11 槽）/ slot count (bionic adds the 11th)
#define READING_ROW_COLS   2   ///< 每行槽位数 / slots per row
#define READING_ROW_HEIGHT 76  ///< 标准槽位高上限（6 行时按可用高度收窄）/ upper bound of a slot; six rows narrow it to the available height
#define READING_ROW_GAP    6   ///< 行间距（6 行重排后收紧）/ row gap after the six-row reflow

typedef enum {
    READING_CELL_TOGGLE,  ///< 开关槽：走 apply_reader_option(kind) / toggle: apply_reader_option(kind)
    READING_CELL_VALUE,   ///< 值循环槽：点击切下一个取值 / cycling value: a tap advances it
    READING_CELL_PAGE,    ///< 子面板槽：点击进子面板 / sub-page: a tap opens it
} reading_cell_t;

typedef struct {
    uint8_t kind;          ///< apply_reader_option() 的 kind；非开关槽见 READING_ROW_* / kind for apply_reader_option(); non-toggle slots use READING_ROW_*
    reading_cell_t cell;   ///< 槽位类型 / cell type
    const char *title;     ///< 标题 / title
    const char *detail;    ///< 副标题；值循环槽用它当取值文案 / subtitle; value cards show it as the value line
} reading_row_t;

static const reading_row_t s_reading_rows[READING_ROW_COUNT] = {
    {READING_ROW_FULL, READING_CELL_VALUE,  "自动全刷",     NULL},
    {READING_ROW_WAVE, READING_CELL_VALUE,  "翻页效果",     NULL},
    {READING_ROW_TURN, READING_CELL_PAGE,   "翻页模式",     NULL},
    {0,                READING_CELL_TOGGLE, "电源键翻页",   "短按下一页，长按锁屏"},
    {READING_ROW_KEYS, READING_CELL_PAGE,   "按键控制",     "自定义三键"},
    {1,                READING_CELL_TOGGLE, "全屏沉浸",     "全屏时隐藏状态栏"},
    {2,                READING_CELL_TOGGLE, "关闭书内图片", "跳过正文插图，保留原书文件"},
    {3,                READING_CELL_TOGGLE, "横屏阅读",     "正文横向铺满，点词查译照常"},
    /* kind=8：开关 kind 0-3 与子面板槽 READING_ROW_*(4-7) 之后顺延，避开命名空间。
       kind 8: toggle kinds 0-3 then past the non-toggle READING_ROW_* markers (4-7). */
    {8,                READING_CELL_TOGGLE, "横屏双栏",     "横屏时正文分两栏"},
    /* kind=9：排版增强总开关（[124] 开关批）。关掉 = 回到 26.9 的经典左对齐态：
       无两端对齐、不断词、无字距。三条路径同开同关，半开态没有基线可比。 */
    {9,                READING_CELL_TOGGLE, "排版增强",     "关掉退回经典左对齐"},
    /* kind=10：仿生阅读（26.11.16 备案 v2）。50% 前半截、无档位；只作用英文词，CJK 直通。
       伪加粗不改 advance，切换后当前页即时重绘、不重排。 */
    {10,               READING_CELL_TOGGLE, "仿生阅读",     "英文词首加粗 50%"},
};

static int reading_row_count(void) { return READING_ROW_COUNT; }

static bool reading_row_on(int index) {
    switch (s_reading_rows[index].kind) {
        case 0: return app_settings_reader_power_turn();
        case 1: return app_settings_reader_immersive();
        case 2: return app_settings_reader_hide_images();
        case 3: return app_settings_reader_landscape();
        case 8: return app_settings_reader_two_column();
        case 9: return app_typography_enhanced();
        case 10: return app_settings_bionic();
        default: return false; // 非开关槽没有开关状态 / non-toggle cells carry no state
    }
}

// 槽位几何（26.11.15 仿生阅读新增第 11 槽后重排为 6 行）：两列，首行顶恒为 702（面板顶
// 580 + 122），末行底恒为 1122（「完成」y=1141 之上 19px）。行高取「可用高度按 6 行均分」
// 与「76+extra 上限」的较小值：100% 字号 65px、150% 82px、200% 99px，任意字号都不越界。
// 11 槽为奇数，第 11 槽独占末行左半格。
// Six rows after the 11th slot (bionic reading). The first row's top is always 702
// (panel top 580 + 122) and the last row's bottom always 1122, 19px above Done. Row
// height is the smaller of "available height split across six rows" and the 76+extra
// cap: 65px at 100%, 82px at 150%, 99px at 200%. Eleven slots is odd, so slot 11 sits
// alone in the last row's left half.
static int reading_row_height(void) {
    const int top = 702 - reader_ui_extra(204);
    const int rows = (READING_ROW_COUNT + READING_ROW_COLS - 1) / READING_ROW_COLS;
    const int share = (1122 - top - (rows - 1) * READING_ROW_GAP) / rows;
    const int cap = READING_ROW_HEIGHT + reader_ui_extra(41);
    return share < cap ? share : cap;
}
static EpdRect reading_setting_rect(int index) {
    if (index < 0 || index >= READING_ROW_COUNT) return (EpdRect){0, 0, 0, 0};
    int col = index % READING_ROW_COLS, row = index / READING_ROW_COLS;
    int height = reading_row_height(), pitch = height + READING_ROW_GAP;
    return (EpdRect){36 + col * 314, 702 - reader_ui_extra(204) + row * pitch, 298, height};
}
static int reading_settings_top(void) { return 580 - reader_ui_extra(204); }
// 行表按槽位编号寻址：绘制、热区、派发三处共用，间隙天然不穿透。
// The table is addressed by slot index: drawing, hot zones and dispatch all share it, so the
// gaps between cards stay inert by construction.
static EpdRect reading_toggle_rect(int index) { return reading_setting_rect(index); }
static void draw_reading_switch(uint8_t *fb, EpdRect track, bool on) {
    ui_fill_round_rect(fb, track, 19, on ? 0x38 : 0xc4);
    int cx = track.x + (on ? 49 : 20);
    epd_fill_circle(cx, track.y + 19, 16, UI_GRAY_WHITE, fb);
    epd_draw_circle(cx, track.y + 19, 16, 0x78, fb);
}
static void draw_reading_card(uint8_t *fb, int index, const char *title,
                              const char *value, bool cycle) {
    EpdRect card = reading_setting_rect(index);
    ui_fill_round_rect(fb, card, 20, UI_GRAY_WHITE);
    ui_draw_control_frame(fb, card, 20, 0x68);
    // 标题区按卡高比例取：6 行重排后卡高 65px，标题仍保留 23px，正文行不被挤没。
    // The title band scales with the card: after the six-row reflow a 65px card still
    // keeps a 23px title, so the value line is never squeezed out.
    const int title_h = (card.height - 12) * 45 / 100;
    ui_text_box(fb,(EpdRect){card.x+21,card.y+6,card.width-42,title_h},23,title,EPD_DRAW_ALIGN_LEFT,false);
    char shown[48]; copy_text(shown,sizeof(shown),value);
    ui_text_box(fb,(EpdRect){card.x+21,card.y+8+title_h,222,card.height-title_h-14},26,shown,EPD_DRAW_ALIGN_LEFT,false);
    ui_draw_icon(fb,card.x+272,card.y+3*card.height/4,26,
                 cycle ? UI_ICON_ROTATE_CW : UI_ICON_CHEVRON_RIGHT, 0x38);
}
// 开关槽：标题 + 副标题（无副标题时退化为「开启/关闭」）+ 开关；副标题经 ui_text_box
// 自动缩放，半宽槽位也放得下。/ Toggle slot: title, subtitle (or 开启/关闭 when it has
// none) and the switch; the subtitle auto-fits via ui_text_box, so half-width slots still fit.
static void draw_reading_toggle(uint8_t *fb, int index) {
    EpdRect row = reading_toggle_rect(index);
    const reading_row_t *r = &s_reading_rows[index];
    bool on = reading_row_on(index);
    ui_fill_round_rect(fb, row, 20, UI_GRAY_WHITE);
    ui_draw_control_frame(fb, row, 20, 0x68);
    const int title_h = (row.height - 12) * 45 / 100;
    ui_text_box(fb,(EpdRect){row.x+21,row.y+6,row.width-42,title_h},23,r->title,EPD_DRAW_ALIGN_LEFT,false);
    char shown[48];
    if (r->detail) copy_text(shown,sizeof(shown),r->detail);
    else copy_text(shown,sizeof(shown),on?"开启":"关闭");
    ui_text_box(fb,(EpdRect){row.x+21,row.y+8+title_h,row.width-108,row.height-title_h-14},19,shown,EPD_DRAW_ALIGN_LEFT,false);
    draw_reading_switch(fb,(EpdRect){row.x+row.width-88,row.y+(row.height-39)/2,69,39},on);
}
static void draw_reading_settings(uint8_t* fb) {
    draw_sheet(fb, reading_settings_top(), "阅读设置");
    char interval[24];
    unsigned pages = app_settings_reader_full_pages();
    if (pages) snprintf(interval, sizeof(interval), "每 %u 页", pages);
    else copy_text(interval, sizeof(interval), "不强制");
    // 值循环卡片取设置值，标题与子面板卡片取值都来自行表。
    // Cycling cards read the settings; titles and the sub-page card's value come from the table.
    draw_reading_card(fb, 0, s_reading_rows[0].title, interval, true);
    // 26.11.5 裁决 (c)：水波纹是显式选择，**代价亮在门口**——开启即接受「动画 +
    // 收尾清底」的单次约 0.9 s，不许用户翻两页自己发现。默认关：清晰是默认。
    //
    // 26.11.7 W3：门口那句话跟着 W1 改口径。原来写「0.9秒/页」，说的是时间；真机
    // 让人不适的不是那 0.9 s，是**整屏**黑一下白一下。W1 把收尾收到正文区之后，
    // 该亮在门口的就是范围：闪的只是正文，状态栏和页脚不动。时间量级没变（收尾
    // 仍是全像素 GC16，约 0.45 s，加动画约 0.44 s），但它是第二位的。
    // Ruling (c) keeps the cost at the door. 26.11.7 W3 restates it for W1: what hurt
    // was the screen-wide flash, not the 0.9 s. After W1 the flash is confined to the
    // body; the clock is unchanged, so it is stated second.
    draw_reading_card(fb, 1, s_reading_rows[1].title,
                      app_settings_reader_turn_effect() ? "水波纹 仅正文区" : "普通（全刷）", true);
    draw_reading_card(fb, 2, s_reading_rows[2].title, app_settings_reader_vertical_turn() ? "上下翻页" : "左右翻页", false);
    draw_reading_card(fb, 4, s_reading_rows[4].title, s_reading_rows[4].detail, false);
    for (int i = 0; i < reading_row_count(); ++i)
        if (s_reading_rows[i].cell == READING_CELL_TOGGLE) draw_reading_toggle(fb, i);
    ui_fill_round_rect(fb, (EpdRect){36, 1141, 612, 58}, 20, UI_GRAY_BLACK);
    reader_panel_text_vc(fb, 342, 1170, 26, "完成", EPD_DRAW_ALIGN_CENTER, true);
}

static void draw_turn_settings(uint8_t *fb) {
    draw_sheet(fb, 270, "翻页模式");
    draw_sheet_back(fb, 270);
    bool vertical = app_settings_reader_vertical_turn();
    for (int i = 0; i < 2; ++i) {
        int y = 370 + i * 322;
        bool selected = vertical == (i != 0);
        EpdRect card = {36, y, 612, 292};
        ui_fill_round_rect(fb, card, 22, selected ? 0xe0 : 0xf0);
        ui_draw_control_frame(fb, card, 22, 0x50);
        ui_draw_control_frame(fb, (EpdRect){37, y + 1, 610, 290}, 21, 0x50);
        EpdRect diagram = {64, y + 37, 182, 220};
        ui_fill_round_rect(fb, diagram, 12, UI_GRAY_WHITE);
        ui_draw_control_frame(fb, diagram, 12, 0x58);
        if (i) {
            ui_hairline(fb, diagram.y + 73, 65, 180, 0x58);
            reader_panel_text_vc(fb, 155, diagram.y + 36, 21, "上一页", EPD_DRAW_ALIGN_CENTER, false);
            reader_panel_text_vc(fb, 155, diagram.y + 147, 21, "下一页", EPD_DRAW_ALIGN_CENTER, false);
        } else {
            ui_fill_rect_fast((EpdRect){155, diagram.y + 1, 1, 218}, 0x58, fb);
            ui_text_box(fb,(EpdRect){110-39,diagram.y+94-15,78,30},21,"上一",EPD_DRAW_ALIGN_CENTER,false);
            ui_text_box(fb,(EpdRect){110-39,diagram.y+126-15,78,30},21,"页",EPD_DRAW_ALIGN_CENTER,false);
            ui_text_box(fb,(EpdRect){201-39,diagram.y+94-15,78,30},21,"下一",EPD_DRAW_ALIGN_CENTER,false);
            ui_text_box(fb,(EpdRect){201-39,diagram.y+126-15,78,30},21,"页",EPD_DRAW_ALIGN_CENTER,false);
        }
        ui_text_box(fb,(EpdRect){274,y+43,350,44},32,i ? "上下翻页" : "左右翻页",EPD_DRAW_ALIGN_LEFT,false);
        ui_text_box(fb,(EpdRect){274,y+112,350,44},23,i ? "上 1/3：上一页" : "左 1/2：上一页",EPD_DRAW_ALIGN_LEFT,false);
        ui_text_box(fb,(EpdRect){274,y+158,350,44},23,i ? "下 2/3：下一页" : "右 1/2：下一页",EPD_DRAW_ALIGN_LEFT,false);
        ui_text_box(fb,(EpdRect){274,y+213,350,44},21,"左右滑动均可翻页",EPD_DRAW_ALIGN_LEFT,false);
        epd_draw_circle(610, y + 61, 12, 0x38, fb);
        if (selected) epd_fill_circle(610, y + 61, 7, UI_GRAY_BLACK, fb);
    }
    reader_panel_text_vc(fb, 342, 1051, 21, "中间区域双击切换全屏 · 始终可用", EPD_DRAW_ALIGN_CENTER, false);
    ui_fill_round_rect(fb, (EpdRect){36, 1141, 612, 58}, 20, UI_GRAY_BLACK);
    reader_panel_text_vc(fb, 342, 1170, 26, "完成", EPD_DRAW_ALIGN_CENTER, true);
}

// 听书面板：状态卡（链路 + 跟随态）+ 开关卡 + 完成键。三刻度全对表：
// sheet top=270、状态卡 {36,370,612,240}、开关卡 {36,650,612,96}、
// 提示行 y=1051、完成键 {36,1141,612,58}——PAD 36 / 卡间距 40（GAP 24 的同族
// 圆整值）/ META_LINE_GAP 8 无一处偏离。/ TTS panel: status card (link + follow
// state) + switch card + done key. All three scales line up: sheet top=270,
// status card {36,370,612,240}, switch card {36,650,612,96}, hint line y=1051,
// done key {36,1141,612,58} - PAD 36 / card gap 40 (the rounded sibling of
// GAP 24) / META_LINE_GAP 8, no deviation anywhere.
// 状态文案只陈述设备侧可知的事实：链路通不通、开关开不开——「手机是否真的在
// 发声」设备无从验证，不假报。/ The status lines state only device-side facts:
// whether the link is up and the switch is on - whether the phone is truly
// speaking is unknowable here and never faked.
static void draw_tts(uint8_t *fb) {
    draw_sheet(fb, 270, "听书");
    draw_sheet_back(fb, 270);
    const bool up = push_sink_link_up();
    const bool on = tts_switch_on();
    // 状态卡：左图右文，与翻页模式卡同族（圆角 22、双层框）。
    // Status card: diagram left, text right - the same family as the turn-mode
    // cards (radius 22, double frame).
    EpdRect card = {36, 370, 612, 240};
    ui_fill_round_rect(fb, card, 22, 0xf0);
    ui_draw_control_frame(fb, card, 22, 0x50);
    ui_draw_control_frame(fb, (EpdRect){37, 371, 610, 238}, 21, 0x50);
    EpdRect diagram = {64, 407, 182, 166};
    ui_fill_round_rect(fb, diagram, 12, UI_GRAY_WHITE);
    ui_draw_control_frame(fb, diagram, 12, 0x58);
    ui_draw_icon(fb, diagram.x + diagram.width / 2, diagram.y + diagram.height / 2, 72,
                 UI_ICON_VOLUME_2, on && up ? 0x00 : 0x40);
    const char *main_line, *sub_line;
    if (!up) {
        main_line = "手机未连接";
        sub_line = "连接后自动推送当前页文字";
    } else if (on) {
        main_line = "跟随朗读 · 已开启";
        sub_line = "翻到新页自动推送文本";
    } else {
        main_line = "跟随朗读 · 已关闭";
        sub_line = "打开开关，手机会朗读当前页";
    }
    ui_text_box(fb, (EpdRect){274, 413, 350, 44}, 30, "听书朗读", EPD_DRAW_ALIGN_LEFT, false);
    ui_text_box(fb, (EpdRect){274, 480, 350, 40}, 23, main_line, EPD_DRAW_ALIGN_LEFT, false);
    ui_text_box(fb, (EpdRect){274, 524, 350, 40}, 21, sub_line, EPD_DRAW_ALIGN_LEFT, false);
    // 开关卡：整宽槽，标题 + 开/关副标题 + 开关（draw_reading_toggle 的同族版式，
    // 副标题即开关态）。/ Switch card: full-width slot with title, on/off subtitle
    // and switch (the same family as draw_reading_toggle; the subtitle is the state).
    EpdRect toggle = {36, 650, 612, 96};
    ui_fill_round_rect(fb, toggle, 20, UI_GRAY_WHITE);
    ui_draw_control_frame(fb, toggle, 20, 0x68);
    ui_text_box(fb, (EpdRect){toggle.x + 21, toggle.y + 6, toggle.width - 42, toggle.height / 2 - 16},
                23, "听书开关", EPD_DRAW_ALIGN_LEFT, false);
    ui_text_box(fb, (EpdRect){toggle.x + 21, toggle.y + toggle.height / 2, toggle.width - 108, toggle.height / 2 - 8},
                19, on ? "开启" : "关闭", EPD_DRAW_ALIGN_LEFT, false);
    draw_reading_switch(fb, (EpdRect){toggle.x + toggle.width - 88, toggle.y + toggle.height / 2 - 19, 69, 39}, on);
    /* 伴读案 30 批 · 步 4（② 面板正式排版与占位文案）：步 3 的「最小热区 + 纯数字
       状态回显」升级为正式两行排版——标题行（恒定）+ 状态／模式名行（运行时取值）。
       命中框几何 {36,780,612,96} / {36,900,612,96} 与 tap 侧硬编码逐个对齐、一字节
       不改；既有状态卡 370 / 开关卡 650 / 提示 1051 / 完成键 1141 亦零动。模式名逐字
       取自 push_proto.h 四模式在案口径（外语原著／小说网文／社科非虚构／古文诗词），
       不自造。副标题为占位文案，不预写任何链路／真机结论。
       Step-4 finished panel: two-line cards (title + live state/mode). Hit-box geometry
       stays byte-identical to the tap side; mode names come verbatim from push_proto.h. */
    static const char *const k_companion_mode_names[PUSH_RIDE_MODE_COUNT] = {
        "外语原著", "小说网文", "社科非虚构", "古文诗词"
    };
    const uint8_t mode_now = (uint8_t)(app_settings_companion_mode() % PUSH_RIDE_MODE_COUNT);
    const bool comp_on = reader_companion_active();
    EpdRect companion_box = {36, 780, 612, 96};
    EpdRect mode_box = {36, 900, 612, 96};
    ui_fill_round_rect(fb, companion_box, 20, UI_GRAY_WHITE);
    ui_draw_control_frame(fb, companion_box, 20, 0x68);
    ui_fill_round_rect(fb, mode_box, 20, UI_GRAY_WHITE);
    ui_draw_control_frame(fb, mode_box, 20, 0x68);
    ui_text_box(fb, (EpdRect){57, 787, 560, 34}, 19, "伴读模式", EPD_DRAW_ALIGN_LEFT, false);
    ui_text_box(fb, (EpdRect){57, 821, 560, 44}, 23,
                comp_on ? "已开启 · 右栏显示摘要" : "未开启 · 点此进入",
                EPD_DRAW_ALIGN_LEFT, false);
    ui_text_box(fb, (EpdRect){57, 907, 560, 34}, 19, "朗读模式", EPD_DRAW_ALIGN_LEFT, false);
    ui_text_box(fb, (EpdRect){57, 941, 560, 44}, 23,
                k_companion_mode_names[mode_now], EPD_DRAW_ALIGN_LEFT, false);
    reader_panel_text_vc(fb, 342, 1051, 21, "朗读由手机完成 · 翻页自动跟随", EPD_DRAW_ALIGN_CENTER, false);
    ui_fill_round_rect(fb, (EpdRect){36, 1141, 612, 58}, 20, UI_GRAY_BLACK);
    reader_panel_text_vc(fb, 342, 1170, 26, "完成", EPD_DRAW_ALIGN_CENTER, true);
}

static const char *reader_key_label(app_reader_key_action_t action) {
    static const char *const labels[] = {"上一页", "下一页", "阅读工具栏", "切换全屏", "返回主页", "手动全刷", "不操作"};
    return labels[action >= 0 && action < APP_READER_KEY_COUNT ? action : APP_READER_KEY_NONE];
}
static EpdRect reader_key_card(unsigned key) { return (EpdRect){36 + (int)key * 208, 506, 196, 114}; }
static EpdRect reader_key_preset_card(unsigned preset) { return (EpdRect){36, 810 + (int)(preset - 1) * 148, 612, 136}; }
static bool reader_key_preset_selected(unsigned preset) {
    static const app_reader_key_action_t actions[2][3] = {
        {APP_READER_KEY_PREV, APP_READER_KEY_TOOLS, APP_READER_KEY_NEXT},
        {APP_READER_KEY_HOME, APP_READER_KEY_FULLSCREEN, APP_READER_KEY_TOOLS}
    };
    for (unsigned i = 0; i < 3; ++i) if (app_settings_reader_key_action(i) != actions[preset - 1][i]) return false;
    return app_settings_reader_hold_action() == (preset == 1 ? APP_READER_KEY_HOME : APP_READER_KEY_REFRESH);
}
static void draw_key_settings(uint8_t *fb) {
    draw_sheet(fb, 270, "按键控制");
    draw_sheet_back(fb, 270);
    static const char *const names[] = {"左键", "中键", "右键"};
    for (unsigned i = 0; i < 3; ++i) {
        int cx = 134 + (int)i * 208;
        if (i == 1) {
            epd_draw_circle(cx, 418, 25, UI_GRAY_BLACK, fb);
            epd_draw_circle(cx, 418, 24, UI_GRAY_BLACK, fb);
        } else ui_fill_round_rect(fb, (EpdRect){cx - 26, 415, 52, 6}, 3, UI_GRAY_BLACK);
        reader_panel_text_vc(fb, cx, 473, 22, names[i], EPD_DRAW_ALIGN_CENTER, false);
        EpdRect card = reader_key_card(i);
        ui_fill_round_rect(fb, card, 20, 0xf0);
        ui_draw_control_frame(fb, card, 20, 0x58);
        ui_text_box(fb,(EpdRect){card.x+17,card.y+7,card.width-57,36},20,"单击",EPD_DRAW_ALIGN_LEFT,false);
        ui_text_box(fb,(EpdRect){card.x+12,card.y+52,card.width-24,52},24,reader_key_label(app_settings_reader_key_action(i)),EPD_DRAW_ALIGN_CENTER,false);
        ui_draw_icon(fb, card.x + card.width - 23, card.y + 24, 20, UI_ICON_CHEVRON_DOWN, 0x38);
    }
    EpdRect hold = {36, 646, 612, 96};
    ui_fill_round_rect(fb, hold, 20, 0xf0); ui_draw_control_frame(fb, hold, 20, 0x58);
    ui_text_box(fb,(EpdRect){58,652,238,48},26,"中键长按",EPD_DRAW_ALIGN_LEFT,false);
    ui_text_box(fb,(EpdRect){322,652,270,48},25,reader_key_label(app_settings_reader_hold_action()),EPD_DRAW_ALIGN_RIGHT,false);
    ui_draw_icon(fb, 625, 679, 20, UI_ICON_CHEVRON_DOWN, 0x38);
    reader_panel_text(fb, 58, 710, 19, "点击选择长按功能，例如返回主页", EPD_DRAW_ALIGN_LEFT, false);
    reader_panel_text_vc(fb, 342, 775, 20, "屏幕中间双击切换全屏，不受按键设置影响", EPD_DRAW_ALIGN_CENTER, false);
    for (unsigned preset = 1; preset <= 2; ++preset) {
        EpdRect card = reader_key_preset_card(preset);
        bool selected = reader_key_preset_selected(preset);
        ui_fill_round_rect(fb, card, 20, selected ? 0xd8 : UI_GRAY_WHITE);
        ui_draw_control_frame(fb, card, 20, 0x58);
        char title[32]; snprintf(title, sizeof(title), "推荐设置 %u", preset);
        ui_text_box(fb,(EpdRect){58,card.y+7,344,49},26,title,EPD_DRAW_ALIGN_LEFT,false);
        ui_text_box(fb,(EpdRect){435,card.y+7,186,49},20,selected?"已应用":"点击应用",EPD_DRAW_ALIGN_RIGHT,false);
        ui_text_box(fb,(EpdRect){58,card.y+59,564,40},21,preset==1?"左上一页 · 中工具栏 · 右下一页":"左回首页 · 中全屏 · 右工具栏",EPD_DRAW_ALIGN_LEFT,false);
        ui_text_box(fb,(EpdRect){58,card.y+103,564,34},19,preset==1?"中键长按：返回主页":"中键长按：手动全刷",EPD_DRAW_ALIGN_LEFT,false);
    }
    ui_fill_round_rect(fb, (EpdRect){36, 1141, 612, 58}, 20, UI_GRAY_BLACK);
    reader_panel_text_vc(fb, 342, 1170, 26, "完成", EPD_DRAW_ALIGN_CENTER, true);
}
static void draw_key_picker(uint8_t *fb) {
    static const char *const titles[] = {"左键功能", "中键功能", "右键功能", "中键长按功能"};
    draw_sheet(fb, 270, titles[s_reader_key_pick < 4 ? s_reader_key_pick : 0]);
    draw_sheet_back(fb, 270);
    for (int i = 0; i < APP_READER_KEY_COUNT; ++i) {
        EpdRect card = {36, 390 + i * 94, 612, 78};
        bool selected = (s_reader_key_pick == 3 ? app_settings_reader_hold_action() : app_settings_reader_key_action(s_reader_key_pick)) == i;
        ui_fill_round_rect(fb, card, 20, selected ? 0xd8 : UI_GRAY_WHITE);
        ui_draw_control_frame(fb, card, 20, 0x58);
        reader_panel_text_vc(fb, 58, card.y + 39, 27, reader_key_label(i), EPD_DRAW_ALIGN_LEFT, false);
        epd_draw_circle(608, card.y + 39, 11, 0x38, fb);
        if (selected) epd_fill_circle(608, card.y + 39, 6, UI_GRAY_BLACK, fb);
    }
    reader_panel_text_vc(fb, 342, 1112, 20, s_reader_key_error[0] ? s_reader_key_error : "从底部上滑可随时打开阅读工具栏", EPD_DRAW_ALIGN_CENTER, false);
    ui_fill_round_rect(fb, (EpdRect){36, 1141, 612, 58}, 20, UI_GRAY_BLACK);
    reader_panel_text_vc(fb, 342, 1170, 26, "返回", EPD_DRAW_ALIGN_CENTER, true);
}

static int reader_font_picker_top(void) { return app_settings_system_font_size()>=150?270:584; }
static EpdRect reader_font_pick_rect(int slot) {
    bool large=app_settings_system_font_size()>=150;
    int columns=large?2:3;
    return (EpdRect){36+(slot%columns)*(large?314:204),
                     reader_font_picker_top()+(large?150:131)+(slot/columns)*146,
                     large?298:180,126};
}
static void draw_font_picker(uint8_t* fb) {
    const int top = reader_font_picker_top();
    draw_sheet(fb, top, "字体");
    epd_draw_line(55, top + 49, 64, top + 58, UI_GRAY_BLACK, fb);
    epd_draw_line(64, top + 58, 73, top + 49, UI_GRAY_BLACK, fb);
    int count = ttf_font_count();
    int pages = count > 0 ? (count + BOOK_FONT_PAGE - 1) / BOOK_FONT_PAGE : 1;
    char current[96];
    snprintf(current, sizeof(current), "当前选择 · %s  %d/%d", ttf_font_display_name(), s_font_page + 1, pages);
    fit_text(current, 20, 540);
    ui_text_box(fb,(EpdRect){90,top+94,504,32},20,current,EPD_DRAW_ALIGN_CENTER,false);
    int start = s_font_page * BOOK_FONT_PAGE;
    char active[TTF_FONT_PATH_MAX]; copy_text(active, sizeof(active), ttf_font_path());
    bool active_builtin = ttf_font_is_builtin();
    for (int slot = 0; slot < BOOK_FONT_PAGE && start + slot < count; ++slot) {
        const ttf_font_item_t* item = ttf_font_item(start + slot);
        EpdRect card = reader_font_pick_rect(slot);
        bool selected = !strcmp(active, item->path);
        ui_fill_round_rect(fb, card, 18, selected ? 0xc2 : 0xe4);
        ui_draw_control_frame(fb, card, 18, selected ? 0x48 : 0x98);
        char label[TTF_FONT_NAME_MAX];
        copy_text(label, sizeof(label), font_friendly_name(item));
        if (ttf_font_open(item->path) == ESP_OK) {
            fit_text(label,25,card.width-32);
            draw_font_name_with_current_face(fb, card.x + card.width/2, card.y + 63, label,card.width-32);
        } else {
            fit_fixed_text(label, ui_text_effective_px(25), card.width - 32);
            ui_text_box(fb,ui_inset_rect(card,16),25,label,EPD_DRAW_ALIGN_CENTER,false);
        }
        if (selected) epd_fill_circle(card.x+card.width-22,card.y+22,7,UI_GRAY_BLACK,fb);
    }
    if (active_builtin) (void)ttf_font_open_builtin();
    else if (active[0]) (void)ttf_font_open(active);
    if (!count) reader_panel_text_vc(fb, 342, 780, 24, "所选字体目录中没有可用字体", EPD_DRAW_ALIGN_CENTER, false);
}

/* ---- 伴读案 30 批 · 步 2：伴读态 + 70/30 分栏几何（判据定稿 R0 · 分栏路径 A） ----
 * 冻结口径：伴读态视觉页 = 主栏 1 个正文逻辑页 + 右栏摘要覆盖层（非逻辑页）；
 * 右栏宽 =（版心宽 − gutter）× 0.30、主栏 × 0.70，gutter 沿用 reader_column_gutter()
 * 现值。本步射程止于「状态 + 栏几何 + 页语义换轨 + 覆盖层区域」：不接菜单/协议/NVS/
 * 刷新波形，重排由置位方同批经 reader_relayout() 触发（步 3 进/出伴读动作）。
 * 关态（s_reader_companion == false）下栏几何、栏数、页地图、寻址与旧路径逐位一致。
 * Companion case batch 30 step 2: companion state + 70/30 column geometry (frozen R0,
 * path A). A companion visual page is one body logical page in the main column plus a
 * non-logical summary overlay on the right; the gutter keeps its current value. Step 2
 * stops at state/geometry/page-semantics/overlay area: no menu, protocol, NVS or refresh
 * wiring; the setter's caller must reflow through reader_relayout() in the same batch.
 * With the state off, every geometry/count/map/addressing result is bit-identical. */
static bool s_reader_companion;                 /* 新增静态量 1 B；默认关 / +1 B, default off */
static bool reader_companion_active(void) { return s_reader_companion; }
/* 步 3 进/出伴读动作的落点。置位后必须同批重排：建版矩形随栏几何变为 70% 主栏宽，
   页地图随伴读态按 1 建，两处失步时 reader_prep_slice 的切片宽守卫会拒画闭环，绝不
   静默错栏。/ Step 3 entry point; the caller must reflow in the same batch. */
void reader_companion_set(bool on) { s_reader_companion = on; }

/* gen30 伴读案 · 修复轮（23:31 终裁）：0x19 页面文本的推送门槛。
 *
 * 重要：这个判据管的是「推不推页文本」，**不是「念不念」**。23:31 终裁把朗读与摘要
 * 解耦后，两者分属不同机制：
 *   - 推送门槛（本函数）：伴读态恒推（恒真是为了摘要链恒通，翻页即 0x1C+0x19）；
 *     非伴读态随听书开关，退化为 tts_switch_on()，逐位不变。
 *   - 朗读执行：听书开关经 TTS_ENABLE(0x1D) 传到手机端，由手机决定念不念。
 * 因此「伴读态下关掉听书开关」仍会推页文本（摘要照常生成），只是手机不发声——
 * 这正是现场异议要求的「只看不听」。函数体沿用原式，语义由「朗读归伴读管」
 * 改写为「摘要链恒通」，一行未动。
 *
 * gen30 repair round (23:31 final ruling): the 0x19 page-text push gate. Note this
 * gates PUSHING, not SPEAKING. After decoupling, pushing stays companion-always
 * (so the summary chain is always live: every page turn airs 0x1C+0x19) and follows
 * the listening switch off-companion, degenerating to tts_switch_on() unchanged.
 * Playback is decided on the phone via TTS_ENABLE(0x1D). Turning the switch off in
 * companion mode therefore keeps pushing text (summaries keep coming) and only
 * silences the phone - exactly the "watch without listening" the field asked for.
 * The expression is unchanged; only its meaning was restated. */
static bool reader_tts_active(void) {
    return tts_switch_on() || reader_companion_active();
}

/* 与 app_transfer.c 的 PUSH_ENTRY_WINDOW_MS 同款（30 分钟开窗），不跨 TU 引头文件；
   进伴读「未武装才开窗」复用该同款窗口（格 2 终裁 A + 复用）。
   Same 30-minute window as app_transfer.c's PUSH_ENTRY_WINDOW_MS; no cross-TU
   header dependency. Entering companion reuses this window only when unarmed
   (ruling grid 2 plan A + reuse). */
#define READER_COMPANION_ARM_WINDOW_MS (30u * 60u * 1000u)

// 栏几何：版心按栏数切条，栏间留 gutter。count<=1 时宽/偏移/矩形全部恒等（逐位返回
// 联合区），关形态不改变任何上游可观测行为。右栏绘制 x 与右栏命中减法同出
// reader_column_offset——横屏坑②配方，两边各写一遍必烂；rect 建在 offset 之上，
// 「同出」由构造保证。（签名比 v2 §3 草图多带 area 参数：保持纯函数，几何可单测。）
// Column geometry: the body area splits into strips with a gutter between them. At
// count<=1 the width, offset and rect are all identities (the joint area comes back
// bit-for-bit), so the closed form changes nothing observable upstream. The right
// column's draw x and its hit-test subtraction both come out of reader_column_offset —
// the pitfall-2 recipe — writing either side twice is how this rots; rect is built on
// offset, so "same source" holds by construction. (The signatures carry area where the
// v2 section 3 sketch did not: they stay pure functions, testable in isolation.)
static int reader_column_gutter(void) {
    // 版心边距优先，下限 48：两栏时间隙不能窄过一个字身，否则视觉上连成一片。
    // The body margin wins, floored at 48: the gutter must not fall below one glyph
    // body or the two columns read as one.
    int m = s_margin ? s_margin : 36;
    return m > 48 ? m : 48;
}
static int reader_column_width(EpdRect area, int count) {
    if (count <= 1) return area.width;
    const int avail = area.width - reader_column_gutter();
    if (reader_companion_active()) {
        /* 70/30 扣 gutter：主栏取整 70%，右栏在 reader_column_rect 里取余量（和恰为
           avail，零 1px 空洞）。/ Main column takes 70% of avail; the right takes the
           remainder in reader_column_rect, so the two sum to avail with no gap. */
        return avail * 70 / 100;
    }
    return avail / count;
}
static int reader_column_offset(EpdRect area, int count, int i) {
    if (count <= 1 || i <= 0) return 0;
    const int step = reader_column_width(area, count) + reader_column_gutter();
    if (reader_companion_active())
        return step;    /* 伴读态只有两栏：右栏起点 = 主栏宽 + gutter；i>=2 不生成第三栏 */
    return i * step;
}
static EpdRect reader_column_rect(EpdRect area, int count, int i) {
    if (count <= 1) return area;
    const int offset = reader_column_offset(area, count, i);
    int width = reader_column_width(area, count);
    if (reader_companion_active() && i > 0) {
        /* 右栏宽 = avail − 主栏宽（30% 余量）；与 offset 同源，两者和恰为 avail。 */
        width = area.width - reader_column_gutter() - width;
    }
    return (EpdRect){area.x + offset, area.y, width, area.height};
}

/* ---- 视觉页地图 / Visual page map ---- */
// 视觉页 = 用户眼里的一屏。双栏时一个视觉页覆盖两个逻辑页（左右栏），整页插图独占一个
// 视觉页；单栏时视觉页恒等于逻辑页。续读、目录、书签、翻页与页码全部经本模块寻址，
// 散点禁止直连 book_layout 页码——地图与版面失步是这套模型唯一的死法。
// A visual page is one screen. Two columns cover two logical pages per visual page and a
// whole-page image owns one alone; single column maps visual pages identically. Resume,
// TOC, bookmarks, turns and page numbers all address through this module; scattered
// book_layout page access is forbidden — a map out of step with the layout is the only
// way this model dies.
//
// P0-1 落地退化形态：建图与寻址全套就绪，双栏开关与第二次绘制在后续阶段接入；单栏下
// 所有寻址结果与旧逻辑页逐位一致（互证测试①守这件事）。
// P0-1 lands the degenerate form: building and addressing are complete while the
// two-column switch and the second draw arrive in later stages; single-column results
// match the old logical pages exactly (cross-check test 1 guards this).

// 与 book_layout 的 4096 页上限对齐；条目数不超过逻辑页数，静态数组无需分配。
// Aligned with book_layout's 4096-page cap; entries never exceed logical pages, so the
// static array needs no allocation.
#define READER_PAGE_MAX 4096u

// 栏模式：普通文字页与题头页左右两栏；整页插图独占视觉页（draw_page 对它是 no-op，图由调用方画）。
// Column mode: ordinary text and lead pages use two columns; a whole-page image owns its
// visual page alone (draw_page is a no-op for it and the caller paints the image).
#define READER_VISUAL_MODE_COLUMNS2 0
#define READER_VISUAL_MODE_JOINT     1

// 视觉页条目：起始逻辑页（左栏；JOINT 页即其自身）与栏模式。
// Visual page entry: the starting logical page (the left column; a JOINT page is itself)
// and the column mode.
typedef struct { uint16_t start; uint8_t mode; } reader_visual_page_t;

static reader_visual_page_t s_page_map[READER_PAGE_MAX];
static size_t s_page_map_count;
static size_t s_page_map_pages;      // 建图时的逻辑页数，仅作诊断。/ Logical page count at build time; diagnostics only.
static uint32_t s_page_map_generation; // 建图时的布局代次，失步判定用它。/ Layout generation at build time; staleness is judged by it.
static int s_page_map_columns;       // 建图时的栏数，退化公式随它变。/ Column count at build time; the fallback formula follows it.
static bool s_page_map_degenerate;   // true = 公式快路径，数组不参与寻址。/ true = formula fast path; the array is not consulted.

// 整页插图回退路径：页级插图序号有效且没有混排槽。
// Whole-page image fallback: a page-level image index with no inline slot.
static bool reader_page_is_joint(size_t page) {
    return book_layout_page_image(page) >= 0 && book_layout_page_image_count(page) == 0;
}

// 建图：全章走一遍分类，每页两次 O(1) 查询。单栏恒等；双栏把普通页顺序两两配对、
// JOINT 页独占、末页或下一页是 JOINT 时左栏独占。全章无 JOINT 页时置 degenerate，
// 寻址走公式，数组不参与热路径；互证测试断言论两条路径等价。
// Build the map by classifying the chapter once, two O(1) queries per page. Single column
// is the identity; two columns pair ordinary pages in order, give JOINT pages their own
// visual page, and leave the left column alone at the tail or before a JOINT page. A
// chapter without JOINT pages sets the degenerate flag so addressing takes the formula
// and the array stays off the hot path; the cross-check test asserts both paths agree.
static void reader_rebuild_page_map(int columns) {
    s_page_map_columns = columns < 1 ? 1 : columns;
    size_t n = book_layout_page_count();
    if (n > READER_PAGE_MAX) n = READER_PAGE_MAX;
    s_page_map_pages = n;
    s_page_map_generation = book_layout_generation();
    s_page_map_count = 0;
    if (s_page_map_columns <= 1) {
        // 数组仍照实填写，互证测试据此比对公式与建图两条路径。
        // The array is still filled so the cross-check test can compare both paths.
        for (size_t k = 0; k < n; ++k) {
            s_page_map[k].start = (uint16_t)k;
            s_page_map[k].mode = READER_VISUAL_MODE_COLUMNS2;
        }
        s_page_map_count = n;
        s_page_map_degenerate = true;
        return;
    }
    bool joint = false;
    size_t k = 0;
    while (k < n && s_page_map_count < READER_PAGE_MAX) {
        if (reader_page_is_joint(k)) {
            joint = true;
            s_page_map[s_page_map_count].start = (uint16_t)k;
            s_page_map[s_page_map_count].mode = READER_VISUAL_MODE_JOINT;
            ++s_page_map_count;
            ++k;
            continue;
        }
        // 下一页存在且不是 JOINT 才配对，否则这一视觉页只有左栏。
        // Pair only when the next page exists and is not JOINT; otherwise the left column stands alone.
        size_t step = k + 1 < n && !reader_page_is_joint(k + 1) ? 2 : 1;
        s_page_map[s_page_map_count].start = (uint16_t)k;
        s_page_map[s_page_map_count].mode = READER_VISUAL_MODE_COLUMNS2;
        ++s_page_map_count;
        k += step;
    }
    s_page_map_degenerate = !joint;
}

// 地图与版面失步（重排或释放后没有重建）时退化到公式寻址并告警一次，绝不静默错页。
// A map out of step with the layout (a reflow or free without a rebuild) degrades to the
// formula and warns once; a wrong page is never silent.
static void reader_map_check_sync(void) {
    if (book_layout_generation() == s_page_map_generation) return;
    if (!s_page_map_degenerate) {
        s_page_map_degenerate = true;
        ESP_LOGW(TAG, "page map stale: built gen %u, layout gen %u; formula fallback",
                 (unsigned)s_page_map_generation, (unsigned)book_layout_generation());
    }
}

// 失步判定后的视觉页总数（内部复用）。/ Visual page count after the sync check (shared internally).
static size_t reader_visual_count_checked(void) {
    size_t n = book_layout_page_count();
    if (!n) return 0;
    if (s_page_map_degenerate) return s_page_map_columns <= 1 ? n : (n + 1) / 2;
    return s_page_map_count;
}

// 把视觉页下标解析成条目（钳下标 + 失步退化）；空布局返回 false。
// Resolve a visual index into an entry (clamped, with stale degradation); false means an empty layout.
static bool reader_visual_entry(size_t visual, size_t* start, int* mode) {
    size_t count = reader_visual_count_checked();
    if (!count) return false;
    if (visual >= count) visual = count - 1;
    if (s_page_map_degenerate) {
        size_t n = book_layout_page_count();
        *start = s_page_map_columns <= 1 ? visual : visual * 2;
        if (*start >= n) *start = n - 1;
        *mode = READER_VISUAL_MODE_COLUMNS2;
        return true;
    }
    *start = s_page_map[visual].start;
    *mode = s_page_map[visual].mode;
    return true;
}

// 视觉页总数（页脚分母）。/ Total visual pages (the footer denominator).
static size_t reader_visual_count(void) {
    reader_map_check_sync();
    return reader_visual_count_checked();
}

// 逻辑页 → 视觉页下标。/ Logical page to visual page index.
static size_t reader_visual_index(size_t logical) {
    reader_map_check_sync();
    size_t n = book_layout_page_count();
    if (!n) return 0;
    if (logical >= n) logical = n - 1;
    if (s_page_map_degenerate)
        return s_page_map_columns <= 1 ? logical : logical / 2;
    size_t index = 0;
    for (size_t i = 0; i < s_page_map_count; ++i) {
        if (s_page_map[i].start <= logical) index = i;
        else break;
    }
    return index;
}

// 视觉页 → 起始逻辑页（= s_page 落点）。/ Visual page to its starting logical page (the s_page landing).
static size_t reader_visual_start(size_t visual) {
    reader_map_check_sync();
    size_t start;
    int mode;
    if (!reader_visual_entry(visual, &start, &mode)) return 0;
    return start;
}

// 视觉页 → 栏模式。/ Visual page to its column mode.
static int reader_visual_mode(size_t visual) {
    reader_map_check_sync();
    size_t start;
    int mode;
    if (!reader_visual_entry(visual, &start, &mode)) return READER_VISUAL_MODE_COLUMNS2;
    return mode;
}

// 视觉页 → 左右逻辑页；右栏不存在时为 SIZE_MAX（JOINT、单栏或落单左栏）。
// Visual page to its left and right logical pages; the right side is SIZE_MAX when
// absent (JOINT, single column, or a lone left column).
static void reader_logical_pages(size_t visual, size_t* left, size_t* right) {
    size_t start = reader_visual_start(visual);
    if (left) *left = start;
    if (reader_companion_active()) {
        /* R0：伴读态返回 1 —— 右栏是摘要覆盖层、不占逻辑页；主栏恒为视觉页起始页。
           显式短路，不依赖建图栏数，双保险（关态逐位不变）。 */
        if (right) *right = SIZE_MAX;
        return;
    }
    size_t next = SIZE_MAX;
    size_t n = book_layout_page_count();
    if (n && s_page_map_columns > 1 && reader_visual_mode(visual) == READER_VISUAL_MODE_COLUMNS2) {
        // 右栏属于同一视觉页：数组路径看下一条目是否正好从 start+1 起（JOINT 或落单
        // 左栏则不是）；公式路径只看 start+1 是否越界。
        // The right column belongs to this visual page: on the array path the next entry
        // must not start at start+1 (a JOINT page or a lone left column does); the
        // formula path only checks the bound.
        if (s_page_map_degenerate) {
            if (start + 1 < n) next = start + 1;
        } else {
            size_t count = reader_visual_count();
            size_t index = visual < count ? visual : count - 1;
            if (start + 1 < n &&
                (index + 1 >= s_page_map_count || s_page_map[index + 1].start != start + 1))
                next = start + 1;
        }
    }
    if (right) *right = next;
}

/* ---- gen26.5 Step6：阅读上下文锚点（book_id / pos）----
 * Reading context anchors (book_id / pos) carried by the contextual 0x17 frame.
 *
 * book_id 是**跨端契约**：算法与口径收在 main/book/book_id.c（纯逻辑、宿主可测），
 * 手机侧（Python/Java 参考实现）同口径重算才能把查询对回书本——这里不再内联一份，
 * 避免两处算法漂移。book_id is a **cross-end contract**: the algorithm and its rules
 * live in main/book/book_id.c (pure logic, host-testable) and the phone side
 * recomputes it with the same rules; no second copy here, so the two cannot drift.
 */

/**
 * 当前视觉页在章内的起始字节偏移（双栏取左栏）；不可得返回 0。
 * Start byte offset of the current visual page inside the chapter (left column when
 * two-column); 0 when unknown.
 */
static size_t reader_page_start_offset(void) {
    size_t left = 0, right = SIZE_MAX;
    reader_logical_pages(s_page, &left, &right);
    return book_layout_page_start_offset(left);
}

// 字节偏移 → 视觉页（续读、目录与书签落点）；偏移经 book_layout 夹到末页再归视觉页。
// Byte offset to visual page (resume, TOC and bookmark landings); the offset is clamped
// to the last page by book_layout before it is folded into a visual page.
static size_t reader_visual_for_offset(size_t off) {
    return reader_visual_index(book_layout_page_for_offset(off));
}

// 任意逻辑页钳到某视觉页的起始逻辑页（= s_page 落点）。
// Clamp any logical page onto the starting logical page of its visual page (the s_page landing).
static size_t reader_clamp_visual_page(size_t logical) {
    return reader_visual_start(reader_visual_index(logical));
}

static void draw_reader_stats(uint8_t* fb) {
    const int top = 768;
    draw_sheet(fb, top, "阅读统计");
    char values[3][32];
    snprintf(values[0], sizeof(values[0]), "%u%%", percent(s_page));
    snprintf(values[1], sizeof(values[1]), "%lu 分钟", (unsigned long)(s_session_read_ms / 60000));
    snprintf(values[2], sizeof(values[2]), "%lu 次", (unsigned long)s_session_turns);
    const char* labels[] = {"阅读进度", "本次阅读", "翻页"};
    for (int i = 0; i < 3; ++i) {
        int cx = 114 + i * 228;
        ui_text_box(fb,(EpdRect){cx-100,top+100,200,48},33,values[i],EPD_DRAW_ALIGN_CENTER,false);
        ui_text_box(fb,(EpdRect){cx-100,top+154,200,36},20,labels[i],EPD_DRAW_ALIGN_CENTER,false);
        if (i < 2) ui_fill_rect_fast((EpdRect){228 + i * 228, top + 99, 1, 74}, 0x98, fb);
    }
    ui_text_box(fb,(EpdRect){54,top+192,576,42},21,"本书进度",EPD_DRAW_ALIGN_LEFT,false);
    EpdRect track = {54, top + 245, 576, 12};
    ui_fill_round_rect(fb, track, 6, 0xd0);
    EpdRect fill = track; fill.width = fill.width * (int)percent(s_page) / 100;
    if (fill.width) ui_fill_round_rect(fb, fill, 6, 0x40);
    if (book_chapter_count() == 1) {
        char left[32], right[32];
            snprintf(left, sizeof(left), "第 %u 页", (unsigned)reader_visual_index(s_page) + 1);
            snprintf(right, sizeof(right), "共 %u 页", (unsigned)reader_visual_count());
            // 视觉页换算（我方 page_map）+ 上游 r91 的面板文本助手（跟随系统字号）。
            // Visual-page maths (our page_map) plus upstream r91 panel text helper
            // (which follows the system font size).
            reader_panel_text(fb, 54, top + 272, 19, left, EPD_DRAW_ALIGN_LEFT, false);
            reader_panel_text(fb, 630, top + 272, 19, right, EPD_DRAW_ALIGN_RIGHT, false);
    } else {
        reader_panel_text(fb, 54, top + 272, 19, "当前页 —", EPD_DRAW_ALIGN_LEFT, false);
        reader_panel_text(fb, 630, top + 272, 19, "总页数 —", EPD_DRAW_ALIGN_RIGHT, false);
    }
    EpdRect details = {36, top + 319, 294, 78}, recent = {354, top + 319, 294, 78};
    ui_fill_round_rect(fb, details, 20, 0xd8); ui_draw_control_frame(fb, details, 20, 0x68);
    ui_fill_round_rect(fb, recent, 20, 0xd8); ui_draw_control_frame(fb, recent, 20, 0x68);
    char marks[32]; snprintf(marks, sizeof(marks), "查看书签 · %u", (unsigned)bookmark_count());
    ui_text_box(fb,(EpdRect){48,top+331,270,54},24,marks,EPD_DRAW_ALIGN_CENTER,false);
    ui_text_box(fb,(EpdRect){366,top+331,270,54},24,"最近 30 天",EPD_DRAW_ALIGN_CENTER,false);
}

static void draw_reader_bookmarks(uint8_t* fb) {
    const int top = 560;
    draw_sheet(fb, top, "书签");
    epd_draw_line(55, top + 49, 64, top + 58, UI_GRAY_BLACK, fb);
    epd_draw_line(64, top + 58, 73, top + 49, UI_GRAY_BLACK, fb);
    reader_bookmarks_t marks = {0};
    bool valid = bookmark_load(&marks);
    int pages = bookmark_pages(valid ? marks.count : 0);
    if (s_bookmark_page >= pages) s_bookmark_page = pages - 1;
    if (valid && marks.count) {
        EpdRect manage = {544, top + 30, 104, 54};
        ui_fill_round_rect(fb, manage, 20, s_bookmark_edit ? 0xd8 : 0xe8);
        ui_draw_control_frame(fb, manage, 20, 0x78);
        reader_panel_text_vc(fb, 596, top + 57, 20, s_bookmark_edit ? "完成" : "管理", EPD_DRAW_ALIGN_CENTER, false);
    }
    if (!valid || !marks.count) {
        reader_panel_text_vc(fb, 342, top + 260, 26, "还没有保存书签", EPD_DRAW_ALIGN_CENTER, false);
        reader_panel_text_vc(fb, 342, top + 309, 18, "阅读时点按“书签”即可保存当前位置", EPD_DRAW_ALIGN_CENTER, false);
        return;
    }
    int first = s_bookmark_page * bookmark_rows();
    for (int row = 0; row < bookmark_rows() && first + row < marks.count; ++row) {
        int newest = (int)marks.count - 1 - (first + row);
        const reader_bookmark_entry_t* mark = &marks.entries[newest];
        EpdRect item = bookmark_row_rect(row);
        bool current = mark->chapter == s_chapter && mark->byte_off == book_layout_page_start_offset(s_page);
        bool selected = s_bookmark_edit && (s_bookmark_selected & (UINT32_C(1) << newest));
        ui_fill_round_rect(fb, item, 14, selected ? 0xc0 : current ? 0xc8 : 0xe8);
        ui_draw_control_frame(fb, item, 14, current ? 0x50 : 0x98);
        char chapter[96];
        if (book_chapter_title(mark->chapter, chapter, sizeof(chapter)) != ESP_OK)
            snprintf(chapter, sizeof(chapter), "第 %u 节", (unsigned)mark->chapter + 1);
        char where[52];
        if (mark->chapter == s_chapter)
            snprintf(where, sizeof(where), "第 %u 页", (unsigned)reader_visual_for_offset(mark->byte_off) + 1);
        else snprintf(where, sizeof(where), "章节 %u", (unsigned)mark->chapter + 1);
        int end=item.x+item.width-(s_bookmark_edit?70:20);
        int where_width=ui_text_fixed_width_px(ui_text_effective_px(20),where);
        if(where_width>220)where_width=220;
        int chapter_width=end-where_width-item.x-34;
        fit_text(chapter,24,chapter_width);
        ui_text_box(fb,(EpdRect){item.x+18,item.y+5,chapter_width,item.height/2-10},24,chapter,EPD_DRAW_ALIGN_LEFT,false);
        ui_text_box(fb,(EpdRect){end-where_width,item.y+5,where_width,item.height/2-10},20,where,EPD_DRAW_ALIGN_RIGHT,false);
        ui_text_box(fb,(EpdRect){item.x+18,item.y+item.height/2,item.width-90,item.height/2-5},18,
                    s_bookmark_edit?"点按选择":current?"当前阅读位置":"点按跳转到此处",EPD_DRAW_ALIGN_LEFT,false);
        if (s_bookmark_edit) {
            epd_draw_circle(item.x + item.width - 35, item.y + 36, 12, 0x48, fb);
            if (selected) epd_fill_circle(item.x + item.width - 35, item.y + 36, 7, 0x38, fb);
        }
    }
    if (s_bookmark_edit) {
        EpdRect all = {36, top + 520, 258, 64}, remove = {312, top + 520, 336, 64};
        ui_draw_button(fb, all, "全选本页", false);
        char label[40]; snprintf(label, sizeof(label), "删除已选 · %u", bookmark_selected_count());
        ui_draw_button(fb, remove, label, bookmark_selected_count() > 0);
    }
    char page[32]; snprintf(page, sizeof(page), "‹   %d / %d   ›", s_bookmark_page + 1, pages);
    reader_panel_text_vc(fb, 342, top + 606, 22, page, EPD_DRAW_ALIGN_CENTER, false);
    if (s_bookmark_delete_confirm) {
        EpdRect panel = {54, top + 196, 576, 240};
        ui_fill_round_rect(fb, panel, 24, UI_GRAY_WHITE);
        ui_draw_control_frame(fb, panel, 24, 0x48);
        reader_panel_text_vc(fb, 342, top + 252, 27,
                   s_bookmark_delete_error ? "删除失败，请重试" : "删除选中的书签？", EPD_DRAW_ALIGN_CENTER, false);
        reader_panel_text_vc(fb, 342, top + 298, 18, "只删除书签，不影响阅读进度", EPD_DRAW_ALIGN_CENTER, false);
        ui_draw_button(fb, (EpdRect){76, top + 348, 244, 64}, "取消", false);
        ui_draw_button(fb, (EpdRect){364, top + 348, 244, 64}, "确认删除", true);
    }
}

// 栏数（第 4 步起读设置）：横屏 + 双栏开关 = 2，其余 1。建版矩形与地图栏数的同批
// 切换由 reader_relayout 单出口保证——出口内一次栏数快照同时驱动建版矩形（栏数>1
// 即左栏矩形）与 rebuild_page_map 的栏数，两处永不失步。
// Column count (reads the setting from step 4 on): landscape plus the two-column switch
// means 2, otherwise 1. The build rect and the map's column count switch in the same
// batch through the reader_relayout single exit — one snapshot inside the exit drives
// both the columnized build rect and rebuild_page_map, so they can never drift apart.
static int reader_column_count(void) {
    if (reader_companion_active()) return 2;  /* 伴读态：几何两栏 70/30，页地图按 1 建（见 relayout 出口） */
    int cols = app_settings_reader_landscape() && app_settings_reader_two_column() ? 2 : 1;
    return cols < 1 ? 1 : cols;
}

// 建版矩形：栏数>1 时为左栏矩形（P3 结论：take_line 按 s_rect.width 断行，建版必须
// 按栏宽建，度量/绘制同源恢复；栏数<=1 时恒等返回 body——关形态逐位不变）。
// Layout build rect: with more than one column this is the left column rect (P3's
// conclusion: take_line wraps at s_rect.width, so the layout must be built at column
// width to restore measure/draw agreement; at one column this is the identity and the
// closed form stays byte-identical).
static EpdRect reader_layout_rect_for(EpdRect body, int cols) {
    return reader_column_rect(body, cols, 0);
}
static EpdRect reader_layout_rect(void) {
    return reader_layout_rect_for(body_rect(), reader_column_count());
}

// 唯一的建版出口（做法方案 §2.4 定案，第 4 步随双栏开关落地）：建版 + 建图不许拆散。
// 栏数快照在本出口内同批驱动建版矩形（栏化统一在此进行，调用方只传版心矩形）与
// rebuild_page_map——地图与版面失步是这套模型唯一的死法。栏数<=1 时建版矩形即版心、
// 地图按 1 建图，与旧路径逐位一致。
// The single layout exit (plan section 2.4, landed with the step-4 switch): build and
// map may never be split. One column snapshot inside the exit drives both the
// columnized build rect (columnization lives here; callers pass the body rect) and
// rebuild_page_map — a map out of step with the layout is the only way this model dies.
// At one column the build rect is the body rect and the map builds at 1, matching the
// old path exactly.
static bool reader_relayout_cols(const char *text, size_t len, blk_t *blocks, size_t count,
                                 EpdRect body, int px, int cols) {
    EpdRect build_rect = reader_layout_rect_for(body, cols);
    // G7-A1 修复：出血宽随栏数快照在本出口内同批发布，先于建版——resolve_image_rect()
    // 在 build_blocks 内跑，读的必须是当次栏数下的发布值。单栏（cols<=1）维持整屏宽
    // 不变（单栏时代「栏=屏」，通栏插图语义无损）；双栏改发栏宽，插图 band 收进栏内，
    // 通栏大图不再越本栏双界、盖住对侧栏文字。发布所有权自 on_enter 收归此处。
    // G7-A1 fix: publish the bleed width at the same column snapshot inside this exit,
    // before the build — resolve_image_rect() runs inside build_blocks and must read the
    // value for this column count. One column keeps the panel width (column == screen in
    // the single-column era, so full-bleed art stays intact); two columns publish the
    // column width so the illustration band stays inside the column and full-bleed art no
    // longer crosses both column bounds and covers the facing column's text. Publish
    // ownership moved here from on_enter.
    book_layout_set_image_bleed_width(cols <= 1 ? UI_LOCK_WIDTH : build_rect.width);
    if (!book_layout_build_blocks(text, len, blocks, count, build_rect, px))
        return false;
    /* 伴读态：右栏是摘要覆盖层（非逻辑页），视觉页恒 1 个正文逻辑页 → 地图按 1 建；
       建版矩形仍是 70% 主栏宽（本出口同批发布，两处不失步）。关态逐字不变。 */
    reader_rebuild_page_map(reader_companion_active() ? 1 : cols);
    return true;
}
static bool reader_relayout(EpdRect body) {
    return reader_relayout_cols(s_text, s_text_len, s_blocks, s_block_count, body, s_px,
                                reader_column_count());
}

// 预绘切片钩子（双栏休眠接线的 prep 侧；P1 侦查答案：draw_slice 一次只画一页一行，
// 双栏必须在 app 层拼两次切片）。reader_prefetch_step 以块作用域弱声明经 volatile
// 指针调它：产品构建同 TU 强定义在场，走双栏拼接；上游 139 行 harness 的链接闭包
// 不含本符号，指针为 0，步进函数自带退化分支直调 draw_slice——harness 逐字节不动。
// 右栏游标函数内自持：被抽取的十个 prep 函数不引用任何新静态，harness 前奏因此无需
// 新增桩。左栏游标代次为 0（新目标或 invalidate 后同目标重踢）即作废右栏状态，防止
// 重踢时右栏续画陈旧行。休眠期（建图栏数 1）右栏恒不存在，整个钩子逐位退化为直调
// draw_slice；双栏下落单左栏（末页或下一个是 JOINT）也只画左栏、同样走左栏矩形——
// 联合区提前返回只允许出现在 cols<=1。栏数>1 而建版矩形仍是联合区时切片宽守卫拒画
// （-1 → invalidate），预绘失败闭环而非画出错栏——「建版矩形与地图栏数同批切换」
// 由这个失败模式机械看守。
// Prefetch slice hook (the prep side of the dormant two-column wiring; P1's answer:
// draw_slice paints one row of one page per call, so two columns must be stitched from
// two slices at the app layer). reader_prefetch_step calls it through a block-scope weak
// declaration and a volatile pointer: product builds have the strong definition in this
// TU and take the two-column stitch; the upstream 139-line harness's link closure lacks
// the symbol, the pointer reads 0 and the step function's own fallback calls draw_slice
// directly — the harness stays untouched. The right cursor lives in function statics: none
// of the ten extracted prep functions reference a new static, so the harness prologue
// needs no new stubs. A left-cursor generation of 0 (a new target, or a same-target
// re-kick after invalidate) voids the right state, stopping a re-kick from resuming
// stale right rows. While dormant (map columns 1) no right column exists and the whole
// hook degenerates to the direct draw_slice call; a lone left column under two columns
// (tail page or a JOINT page next) also paints only the left side, through the left
// column rect just the same — the joint-area early return exists only at cols<=1. With
// columns>1 against a still-joint build rect the slice width guard refuses (-1 ->
// invalidate) so prefetch fails closed instead of mis-columns — "the build rect and the
// map's column count switch in the same batch" is mechanically guarded by that failure
// mode.
int reader_prep_slice(uint8_t* fb, size_t left, EpdRect area, int px,
                      book_layout_draw_cursor_t* cursor) {
    int cols = reader_column_count();
    EpdRect left_rect = reader_column_rect(area, cols, 0);
    size_t right = SIZE_MAX;
    if (cols > 1) reader_logical_pages(reader_visual_index(left), NULL, &right);
    if (right == SIZE_MAX) return book_layout_draw_slice(fb, left, left_rect, px, cursor);
    static book_layout_draw_cursor_t s_right_cursor;
    static size_t s_right_left = SIZE_MAX;
    if (!cursor->generation) s_right_left = SIZE_MAX;   // 左栏新起：右栏状态作废
    int result = book_layout_draw_slice(fb, left, left_rect, px, cursor);
    if (result != 1) return result;                     // 0 = 左栏未尽；-1 = 失败
    if (s_right_left != left) {                         // 右栏首行：重建游标
        memset(&s_right_cursor, 0, sizeof(s_right_cursor));
        s_right_left = left;
    }
    return book_layout_draw_slice(fb, right, reader_column_rect(area, cols, 1), px, &s_right_cursor);
}

static void draw_reader_stats_recent(uint8_t* fb) {
    const int top = 584;
    draw_sheet(fb, top, "最近 30 天");
    epd_draw_line(55, top + 49, 64, top + 58, UI_GRAY_BLACK, fb);
    epd_draw_line(64, top + 58, 73, top + 49, UI_GRAY_BLACK, fb);
    if (!s_recent_days_valid) {
        reader_panel_text_vc(fb, 342, top + 280, 23, "请先完成日期与时间设置", EPD_DRAW_ALIGN_CENTER, false);
        reader_panel_text_vc(fb, 342, top + 326, 18, "完成对时后开始记录每天的阅读时长", EPD_DRAW_ALIGN_CENTER, false);
        return;
    }
    uint64_t sum = 0;
    uint32_t maximum = 0;
    for (int i = 0; i < 30; ++i) {
        sum += s_recent_days[i];
        if (s_recent_days[i] > maximum) maximum = s_recent_days[i];
    }
    char total[64];
    snprintf(total, sizeof(total), "累计 %llu 小时 %llu 分钟",
             (unsigned long long)(sum / 3600), (unsigned long long)((sum / 60) % 60));
    reader_panel_text_vc(fb, 342, top + 112, 28, total, EPD_DRAW_ALIGN_CENTER, false);
    const int base_y = top + 438, max_h = 250;
    ui_hairline(fb, base_y, 42, 600, 0xb0);
    for (int i = 0; i < 30; ++i) {
        int h = maximum ? (int)((uint64_t)s_recent_days[i] * max_h / maximum) : 0;
        if (s_recent_days[i] && h < 5) h = 5;
        if (h) ui_fill_round_rect(fb, (EpdRect){47 + i * 20, base_y - h, 10, h}, 5, 0x38);
    }
    reader_panel_text(fb, 42, base_y + 22, 19, "30 天前", EPD_DRAW_ALIGN_LEFT, false);
    reader_panel_text(fb, 642, base_y + 22, 19, "今天", EPD_DRAW_ALIGN_RIGHT, false);
}

static void draw_reader_panel(uint8_t* fb) {
    if (s_reader_panel==READER_PANEL_NONE) return;
    ui_text_set_system_scale(true);
    if (s_reader_panel == READER_PANEL_TOOLS) {
        ui_fill_rect_fast((EpdRect){0, 1096, UI_LOCK_WIDTH, 120}, UI_GRAY_WHITE, fb);
        ui_hairline(fb, 1096, 0, UI_LOCK_WIDTH, 0x98);
        for (int i = 0; i < BOOK_TOOL_COUNT; ++i) {
            EpdRect rect = tool_rect(i);
            int cx = rect.x + rect.width / 2;
            bool bookmarked = i == 1 && bookmark_is_current();
            draw_reader_tool_icon(fb, cx, 1138, i, bookmarked);
            // 用上游 r91 的 ui_text_box（面板跟随系统字号后按盒子排布），标签仍取我方
            // 六项表（第六项「生词本」是我方功能）。/ Upstream r91 lays the label out in
            // a box now that panels follow the system font size; labels still come from
            // our six-entry table (the sixth, 生词本, is ours).
            ui_text_box(fb,(EpdRect){rect.x+6,1159,rect.width-12,40},20,bookmarked?"已书签":k_tool_labels[i],EPD_DRAW_ALIGN_CENTER,false);
        }
    } else if (s_reader_panel == READER_PANEL_FONT_SETTINGS) draw_font_settings(fb);
    else if (s_reader_panel == READER_PANEL_WEIGHT_SETTINGS) draw_weight_settings(fb);
    else if (s_reader_panel == READER_PANEL_LAYOUT_SETTINGS) draw_layout_settings(fb);
    else if (s_reader_panel == READER_PANEL_RULE_SETTINGS) draw_rule_settings(fb);
    else if (s_reader_panel == READER_PANEL_BACKGROUND) draw_background_directory(fb);
    else if (s_reader_panel == READER_PANEL_BACKGROUND_PREVIEW) draw_background_picker(fb);
    else if (s_reader_panel == READER_PANEL_FONT_PICKER) draw_font_picker(fb);
    else if (s_reader_panel == READER_PANEL_STATS) draw_reader_stats(fb);
    else if (s_reader_panel == READER_PANEL_BOOKMARKS) draw_reader_bookmarks(fb);
    else if (s_reader_panel == READER_PANEL_STATS_RECENT) draw_reader_stats_recent(fb);
    else if (s_reader_panel == READER_PANEL_REFRESH_SETTINGS) draw_reading_settings(fb);
    else if (s_reader_panel == READER_PANEL_TURN_SETTINGS) draw_turn_settings(fb);
    else if (s_reader_panel == READER_PANEL_KEY_SETTINGS) draw_key_settings(fb);
    else if (s_reader_panel == READER_PANEL_KEY_PICKER) draw_key_picker(fb);
    else if (s_reader_panel == READER_PANEL_DICT) dict_popup_draw(fb);
    else if (s_reader_panel == READER_PANEL_TTS) draw_tts(fb);
    // 上游 r91：面板文字跟随系统字号，画完复位；我方词典卡片走同一条 if 链。
    // Upstream r91: panel text follows the system font size and resets afterwards;
    // our dictionary card rides the same if-chain.
    ui_text_set_system_scale(false);
}

/* ---- 绘制与预渲染 / Drawing and preparation ---- */
static uint8_t inline_ink_gray(uint8_t gray) {
    // 解码器已提供灰阶；再次加深并叠加系统对比度会压死抖动前的中间色阶。
    // The decoder already supplies grayscale; extra darkening and UI contrast clip midtones.
    return gray;
}

// 收藏图标右对齐：竖屏 x=605(右端留 49)，横屏按当前逻辑宽度右对齐；绘制与点击共用，避免点空。
// Right-align the favorite icon: portrait x=605 (49px right margin); landscape right-aligns to
// the current logical width. Draw and tap share this so the hit region never drifts from the icon.
static int reader_favorite_x(void) {
    return epd_get_rotation() == EPD_ROT_LANDSCAPE ? ui_w() - 79 : 605;
}
static void draw_reader_header(uint8_t *fb) {
    ui_nav_back(fb, 36, 79);
    char header[128];
    const char *sample = s_book_title[0] ? s_book_title : s_title;
    copy_text(header, sizeof(header), sample);
    int title_px = ui_text_title_fit(header, 32, 450, sample);
    // 返回键、书名与收藏使用同一视觉中心线；字体切换不改变对齐。
    // Back, title and favorite share one visual centerline across font changes.
    ui_text_title_vc(fb, UI_LOCK_WIDTH / 2, 114, title_px, header, sample, EPD_DRAW_ALIGN_CENTER);
    draw_favorite_icon(fb, reader_favorite_x(), 96, 30, 36, s_reader_favorite, 0x38);
    ui_hairline(fb, 151, 36, ui_w() - 72, UI_GRAY_LIGHT);
}
// 竖排章节题头：右列是编号（第三章），左列是章名，读序依旧自右向左；两列整体留出列宽，
// 排版用同一个宽度在首页右边缘预留，绘制与预留因此不会错位。
// A vertical chapter heading: the number column (第三章) on the right and the title on the left,
// still read right to left. Both columns reserve their width, and the layout reserves the same
// width at the right edge of the first page, so drawing and reservation cannot drift apart.
typedef struct {
    int width;       ///< 首页右起需要预留的列宽 / columns to reserve at the first page's right
    int title_px;    ///< 章名字号 / title type size
    int title_x;     ///< 章名列左边缘 / title column's left edge
    int label_px;    ///< 编号字号，无编号时为 0 / number size, 0 without a label
    int label_x;     ///< 编号列左边缘 / label column's left edge
    int text_height; ///< 较长一列的高度，用来垂直居中 / taller column, used to center
} reader_heading_t;

static reader_heading_t reader_heading_metrics(const char *title, const char *label, EpdRect body) {
    reader_heading_t metrics = {0};
    if (title == NULL || title[0] == 0 || body.height <= 0 || body.width <= 0) return metrics;
    // 题头跟横排的题头一样走系统字体：正文换卡上字体不改变章节标题。
    // The heading uses the system face exactly like the horizontal one, so swapping the body face
    // never changes chapter titles.
    ttf_font_select(TTF_FONT_SLOT_SYSTEM);
    const int avail = body.height - 48;
    int px = 48;
    int need = ttf_vertical_text_height_px(px, title, 0);
    // 一列放不下就缩字号；竖排的题头不折列，缩到 24 为止。
    // Shrink the size when one column cannot hold the title; a vertical heading does not break
    // into more columns and stops at 24.
    if (need > avail && avail > 0) {
        px = px * avail / need;
        px &= ~3;
        if (px < 24) px = 24;
    }
    int label_px = label != NULL && label[0] ? 24 : 0;
    const int gap = 12;
    metrics.title_px = px;
    metrics.label_px = label_px;
    metrics.width = px + (label_px ? label_px + gap : 0);
    // 题头最多占正文带的三分之一，留够列给正文。
    // A heading takes at most a third of the body band, leaving the rest for body columns.
    if (metrics.width > body.width / 3) {
        int scale = body.width / 3 * 100 / metrics.width;
        metrics.title_px = metrics.title_px * scale / 100;
        if (metrics.title_px < 24) metrics.title_px = 24;
        metrics.label_px = label_px ? label_px * scale / 100 : 0;
        if (metrics.label_px && metrics.label_px < 18) metrics.label_px = 18;
        metrics.width = metrics.title_px + (metrics.label_px ? metrics.label_px + gap : 0);
    }
    const int right = body.x + body.width;
    metrics.title_x = right - metrics.width;
    metrics.label_x = right - metrics.label_px;
    int title_h = ttf_vertical_text_height_px(metrics.title_px, title, 0);
    int label_h = metrics.label_px ? ttf_vertical_text_height_px(metrics.label_px, label, 0) : 0;
    metrics.text_height = title_h > label_h ? title_h : label_h;
    return metrics;
}

static void draw_vertical_heading(uint8_t *fb, EpdRect body) {
    reader_heading_t metrics = reader_heading_metrics(s_chapter_heading_title, s_chapter_heading_label, body);
    if (!metrics.width) return;
    int y = body.y + (body.height - metrics.text_height) / 2;
    if (y < body.y + 8) y = body.y + 8;
    ttf_font_select(TTF_FONT_SLOT_SYSTEM);
    if (metrics.label_px)
        ttf_draw_text_px_vertical(fb, metrics.label_x, y, metrics.label_px, s_chapter_heading_label, 0, 0, 15);
    ttf_draw_text_px_vertical(fb, metrics.title_x, y, metrics.title_px, s_chapter_heading_title, 0, 0, 15);
}

// 重排前把章节题头的几何重新说给排版：竖排留右侧列宽、横排留顶部行高，
// 绘制侧读的是同一组 s_chapter_lead_*。切模式、切全屏、改字号都必须走这里，
// 否则排版按旧模式预留、题头按新模式绘制，题头就会压到正文上。
// Tell the layout the chapter heading geometry before every re-layout: columns to the right in
// vertical setting, rows at the top in horizontal. Drawing reads the same s_chapter_lead_*
// values. Switching mode or full screen or changing the type size must all come through here;
// otherwise the layout reserves for the old mode while the heading draws in the new one and lands
// on top of the body.
// 题头几何按当前模式重算：竖排留右侧列宽、横排留顶部行高。
// height 由调用方按原有规矩算好（首块是插图、或没有可用标题时为 0），这里不再自己推导——
// 无条件按 214 算会把不该出现的题头带出来，正文里那份标题就与它并排成了两个标题。
// Heading geometry for the current mode: columns to the right vertically, rows at the top
// horizontally. `height` arrives already decided by the caller (zero when the first block is an
// illustration or no title is usable); deriving it here unconditionally brought a heading onto
// sheets that should have none, leaving it side by side with the body's own copy.
static void apply_chapter_lead(const char *title, const char *label, size_t skip, unsigned height) {
    s_chapter_lead_skip = skip;
    if (book_kind() != BOOK_KIND_EPUB || title == NULL || title[0] == 0 || !height) {
        s_chapter_lead_height = s_chapter_lead_width = 0;
        book_layout_set_chapter_lead(0, 0);
        book_layout_set_chapter_lead_width(0);
        return;
    }
    if (app_settings_book_vertical()) {
        // 量竖排题头会光栅化字形，调用方须已持有绘制锁。
        // Measuring the vertical heading rasterizes glyphs; the caller must hold the draw lock.
        s_chapter_lead_height = 0;
        s_chapter_lead_width = (unsigned)reader_heading_metrics(title, label, body_rect()).width;
        book_layout_set_chapter_lead(skip, 0);
        book_layout_set_chapter_lead_width(s_chapter_lead_width);
    } else {
        s_chapter_lead_width = 0;
        s_chapter_lead_height = height;
        book_layout_set_chapter_lead(skip, s_chapter_lead_height);
        book_layout_set_chapter_lead_width(0);
    }
}

// 重排前把题头几何交给排版；调用方须持有绘制锁。所有正文重排都要走这一句，
// 否则换了模式或字号之后，排版按上一次的题头尺寸预留，题头就会压住正文。
// Hand the heading geometry to the layout before a re-layout; the caller holds the draw lock.
// Every body re-layout needs this line, otherwise a mode or size change reserves room for the
// previous heading and the heading lands on the body.
static void refresh_chapter_lead(void) {
    apply_chapter_lead(s_chapter_heading_title, s_chapter_heading_label, s_chapter_lead_skip,
                       s_chapter_lead_row_height);
}

// PR9 未知尺寸整页图没有混排槽；在调用侧适配原有位图缓存。/ PR9's unknown-size page has no inline slot; adapt the bitmap cache here.
static int reader_image_slots(size_t page) {
    int count = book_layout_page_image_count(page);
    return count ? count : book_layout_page_image(page) >= 0 ? 1 : 0;
}
static bool reader_image_slot(size_t page, int slot, int *index, int *y, int *width, int *height) {
    if (book_layout_page_image_count(page))
        return book_layout_page_image_at(page, slot, index, y, width, height);
    int image = book_layout_page_image(page);
    if (slot != 0 || image < 0) return false;
    // 未知尺寸的整页图同样通栏，不能拿正文栏宽当上限。
    // An unknown-size whole-page image bleeds as well, so the body column is not its ceiling.
    EpdRect band = book_layout_image_rect();
    if (index) *index = image;
    if (y) *y = 0;
    if (width) *width = band.width;
    if (height) *height = band.height;
    return true;
}
static void draw_reader_images(uint8_t *fb, size_t page, EpdRect body) {
    // 位图和布局代次必须匹配；预渲染另一页不能复用当前页插图。
    // Match page and layout generation; a prefetched page must never borrow current-page bitmaps.
    if (!app_settings_reader_hide_images() && page == s_page &&
        s_page_images_for == page && s_page_images_generation == book_layout_generation()) {
        // 横向按插图区域居中：通栏时图片铺满整屏，不再留正文边距。
        // Center horizontally in the illustration region: with full bleed the image spans the
        // panel and no longer carries the body margins.
        EpdRect band = book_layout_image_rect();
        for (int i = 0; i < reader_image_slots(page); ++i) {
            int y, width, height;
            if (!reader_image_slot(page, i, NULL, &y, &width, &height)) continue;
            const reader_image_t *image = s_page_images && i < s_page_image_count ? &s_page_images[i] : NULL;
            if (image && image->gray) {
                int left = band.x + (band.width - image->width) / 2;
                int top = band.y + y + (height - image->height) / 2;
                // 页首插图贴到阅读区顶端：正文上方那点呼吸空间只属于正文，不该把插图压下来。
                // A page-start illustration sits at the top of the reading area: the inset above
                // body text belongs to text and must not push the illustration down.
                if (y == 0) top = reader_area().y;

                for (int iy = 0; iy < image->height; ++iy)
                    for (int ix = 0; ix < image->width; ++ix)
                        epd_draw_pixel(left + ix, top + iy,
                            ui_image_dither_gray(inline_ink_gray(image->gray[(size_t)iy * image->width + ix]),
                                                 left + ix, top + iy), fb);
            } else if (height >= UI_PX_CAPTION + 4 && width >= 70) {
                char notice[64]; copy_text(notice, sizeof(notice), "此插图暂无法显示");
                fit_text(notice, UI_PX_CAPTION, width - 8);
                ui_text_vc(fb, body.x + body.width / 2, body.y + y + height / 2,
                           UI_PX_CAPTION, notice, EPD_DRAW_ALIGN_CENTER, false);
            }
        }
    }
}

// 正文块必须与当前显示方向同代：横屏阅读里面板是竖屏的，一开面板就要按竖屏重排、
// 关回去再按横屏重排；从书架/目录/书签进入正文时块是按进入前的方向建的，也在这里补齐。
// 只在版面尺寸真正变化时重排——竖屏模式零额外开销；页码按字节偏移重映射，阅读位置不跳。
// Body blocks must match the orientation on screen: panels are portrait, so opening one
// during landscape reading reflows to portrait and closing it reflows back; entries from
// the shelf/TOC/bookmarks build under the previous orientation and are fixed up here too.
// Reflow only when the body size actually changed — portrait mode pays nothing — and remap
// pages by byte offset so the reading position does not jump.
static size_t ensure_body_orientation(size_t page) {
    if (!s_text) return page;
    EpdRect want = reader_layout_rect();
    EpdRect built = book_layout_body_rect();
    if (built.width == want.width && built.height == want.height) return page;
    size_t page_off = reader_page_offset(page < book_layout_page_count() ? page : 0);
    bool jump_page = s_jump_offset != SIZE_MAX && page == s_jump_page;
    size_t current_off = reader_page_offset(s_page < book_layout_page_count() ? s_page : 0);
    if (!reader_relayout(body_rect())) {
        // 失败就回滚旧版面：尺寸守卫至少放过这一帧，不会画出错版文字。/ Roll back to the
        // previous layout on failure: the size guard still lets this frame through.
        // 出血宽按旧建版的栏数同批回滚发布（s_page_map_columns 与 built 同批，规则同
        // reader_relayout_cols 出口）：band 由 build_blocks 内的 resolve_image_rect 现推，
        // 漏发布会让插图 band 挂在错栏数下的值，版面回滚了 band 没回滚。
        // The rollback republishes the bleed width at the old build's column count in the
        // same batch (s_page_map_columns shipped with built; same rule as the
        // reader_relayout_cols exit): the band is derived inside build_blocks by
        // resolve_image_rect, so a missed publish leaves the band at the wrong column
        // count while the layout rolled back.
        (void)book_layout_set_image_bleed_width(s_page_map_columns <= 1 ? UI_LOCK_WIDTH
                                                                       : built.width);
        (void)book_layout_build_blocks(s_text, s_text_len, s_blocks, s_block_count, built, s_px);
        // 回滚后版面与进函数时一致；地图按旧建版的栏数重建（s_page_map_columns 与
        // built 同批），不跟当前设置走——否则地图-版面失步。
        // The rollback restores the entry layout; the map rebuilds at the old build's
        // column count (s_page_map_columns shipped with `built`), never at the current
        // setting — a mismatched count would desync map and layout.
        reader_rebuild_page_map(s_page_map_columns);
        copy_text(s_reader_notice, sizeof(s_reader_notice), "排版失败，请重试");
        s_reader_notice_until = esp_timer_get_time() / 1000 + 1800;
        return page;
    }
    // 地图已在 reader_relayout 单出口内按同一栏数快照重建（建版+建图同批）。
    // The map already rebuilt inside the reader_relayout single exit at the same column
    // snapshot (build and map in one batch).
    size_t count = book_layout_page_count();
    size_t mapped = book_layout_page_for_offset(page_off);
    if (count && mapped >= count) mapped = count - 1;
    size_t current = book_layout_page_for_offset(current_off);
    if (count && current >= count) current = count - 1;
    s_page = reader_clamp_visual_page(current);
    size_t landing = reader_clamp_visual_page(mapped);
    if (jump_page) s_jump_page = landing;
    // 预渲染帧属于旧分页：作废，下一次 kick 按新分页重来。/ A prefetched frame belongs to
    // the old pagination: drop it and let the next kick redo it.
    invalidate_prep();
    return landing;
}
static void draw_reader_outer(uint8_t *fb,size_t page) {
    ui_text_set_system_scale(false);
    if (!(s_reader_fullscreen && app_settings_reader_immersive())) ui_nav_status(fb);
    if (!s_reader_fullscreen) draw_reader_header(fb);
    EpdRect body=body_rect();
    if (page == 0 && s_chapter_heading_title[0] &&
        (s_chapter_lead_height || s_chapter_lead_width)) {
        if (app_settings_book_vertical()) {
            // 竖排题头站在首页预留的那条列里，正文从它左边接着排。
            // The vertical heading stands in the columns the first page reserved; the body
            // continues to its left.
            draw_vertical_heading(fb, body);
        } else {
            char heading[128]; copy_text(heading, sizeof(heading), s_chapter_heading_title);
            char label[sizeof(s_chapter_heading_label)]; copy_text(label, sizeof(label), s_chapter_heading_label);
            // 题头一期只占左栏（决策⑥）：双栏时中心、fit 宽度与装饰线全部改走左栏几何；
            // 单栏三项保持既有表达式——balanced 收齐与奇数字号舍入会让「看起来等价」的
            // 改写产生 ±1px 漂移，关形态恒等不许这种漂移（装饰线的 210/264 是肖像硬值，
            // 横屏下本就 stale，本步不借双栏顺手改它）。
            // The heading takes the left column only for now (decision 6): two columns
            // move the center, fit width and rule onto the left column's geometry while
            // single column keeps the existing expressions — balanced rounding and odd
            // font-size rounding make "equivalent" rewrites drift by ±1px, which the
            // closed-form identity does not allow (the rule's 210/264 are portrait
            // constants, already stale in landscape; this step does not smuggle a fix
            // in through the two-column work).
            int center_x = UI_LOCK_WIDTH / 2, fit_w = ui_content_width();
            int rule_x = 210, rule_w = 264;
            int cols = reader_column_count();
            if (cols > 1) {
                EpdRect col0 = reader_column_rect(body, cols, 0);
                center_x = col0.x + col0.width / 2;
                fit_w = col0.width;
                rule_w = col0.width < 264 ? col0.width : 264;
                rule_x = center_x - rule_w / 2;
            }
            int heading_px = ui_text_title_fit(heading, 48, fit_w, s_chapter_heading_title);
            int label_px = ui_text_title_fit(label, 24, fit_w, s_chapter_heading_label);
            ui_text_title_vc(fb, center_x, body.y + 29, label_px, label,
                              s_chapter_heading_label, EPD_DRAW_ALIGN_CENTER);
            ui_text_title_vc(fb, center_x, body.y + 112, heading_px, heading,
                              s_chapter_heading_title, EPD_DRAW_ALIGN_CENTER);
            ui_hairline(fb, body.y + 174, rule_x, rule_w, UI_GRAY_LIGHT);
        }
    }
    if (s_reader_panel != READER_PANEL_BACKGROUND_PREVIEW)
        ui_reader_background_paint(fb, (EpdRect){0,0,UI_LOCK_WIDTH,UI_LOCK_HEIGHT}, true);
    draw_reader_images(fb, page, body);
    if (s_reader_fullscreen) {
        // Full-book progress uses the last few screen rows and no footer text.
        EpdRect track = {36, READER_FULLSCREEN_PROGRESS_TOP + 3, UI_LOCK_WIDTH - 72, 3};
        ui_fill_round_rect(fb, track, 1, 0x90);
        track.width = track.width * (int)percent(page) / 100;
        if (track.width) ui_fill_round_rect(fb, track, 1, UI_GRAY_BLACK);
    } else {
        EpdRect track = progress_rect();
        EpdRect bar = {track.x, track.y + 16, track.width, 12};
        ui_fill_round_rect(fb, bar, 5, UI_GRAY_LIGHT);
        bar.width = bar.width * (int)percent(page) / 100;
        if (bar.width) ui_fill_round_rect(fb, bar, 5, UI_GRAY_BLACK);
        reader_footer_band_paint(fb, page);
    }
    if (s_reader_notice[0]) {
        // 气泡跟随屏幕宽度居中：竖屏(684)与原坐标一致，横屏阅读时仍在视觉中心。
        // Center the toast on the screen width: identical to the old coordinates in
        // portrait (684) and visually centered during landscape reading.
        EpdRect notice = {(ui_w() - 248) / 2, 160, 248, 48};
        ui_fill_round_rect(fb, notice, 24, 0xd0);
        ui_text_vc(fb, ui_w() / 2, 184, 19, s_reader_notice, EPD_DRAW_ALIGN_CENTER, false);
    }
    draw_reader_panel(fb);
}
/* 伴读案 30 批 · 步 4（① 右栏摘要显示层）：步 2 只立「区域可见性」外框，本步接
   摘要数据、码点硬截二次截断、占位三态与侧栏 padding。刷新波形（DU 局部刷 /
   N=12 区域 GC16）由 companion_summary_poll() 在 on_tick 侧驱动，本函数只画帧
   缓冲。字号由 ui_text_block_fit 在内框里运行时取最大可容纳值——A/B（页眉页脚
   是否收进主栏）已裁 A 案，字号上限因此可写在本节常量里。
   Step-4 right column: frame + data + codepoint clip + three placeholder states +
   pad. Refresh (DU / every-12th GC16) lives in the tick-side poll; type size is
   chosen at runtime by ui_text_block_fit. */
static size_t reader_companion_clip_cap(const char *src, char *dst, size_t cap) {
    size_t n = 0, chars = 0;
    while (src[n] && chars < READER_COMPANION_SUMMARY_CAP) {
        const unsigned char c = (unsigned char)src[n];
        size_t step = c < 0x80 ? 1u : (c & 0xe0) == 0xc0 ? 2u
                    : (c & 0xf0) == 0xe0 ? 3u : (c & 0xf8) == 0xf0 ? 4u : 1u;
        size_t k = 1;
        while (k < step && src[n + k] && ((unsigned char)src[n + k] & 0xc0) == 0x80) ++k;
        if (k < step) step = 1u;   /* 截断／非法序列按单字节收，绝不越界或切半个字 */
        if (n + step >= cap) break;
        memcpy(dst + n, src + n, step);
        n += step;
        ++chars;
    }
    dst[n] = 0;
    return n;
}
static void reader_companion_overlay(uint8_t* fb, EpdRect right_rect) {
    ui_draw_control_frame(fb, right_rect, 8, 0x48);
    EpdRect inner = {right_rect.x + READER_COMPANION_PAD,
                     right_rect.y + READER_COMPANION_PAD,
                     right_rect.width - 2 * READER_COMPANION_PAD,
                     right_rect.height - 2 * READER_COMPANION_PAD};
    if (inner.width <= 0 || inner.height <= 0) return;
    /* A2（14:26 裁定）：三态判据抽到 app_companion_state.h 的纯函数，app_book.c 与
       宿主测试调用同一份真实代码——守卫面 ④ 断言「has=false → 摘要生成中…上屏」
       测的就是这里，不是复制品。文案与逐字口径不变。 */
    const char *text = companion_placeholder(push_sink_link_up(),
                                             s_companion_summary_has,
                                             s_companion_summary);
    /* 修复轮（真机故障一）：这里原来调 ui_text_box——单行自适应缩放控件，把整段
       文字当一行量宽、塞不进就按比例缩字号。56 字摘要放进右栏内宽时字号被压到
       1~3 px，现场「挤成一条线、字体极小无法识别」正是这一语义的必然结果，与
       70/30 几何无关（几何经实拍量测成立）。改块排版：按码点断行、固定行高、
       行数受内框高度约束、溢出截断，字号有 14 px 下限。
       二轮（第二轮回执）：现场要求「字再大、字再多」。字号改由 fit 在 18..34 内
       取能完整放下整段的最大值——短摘要直接到 34，长摘要自动退到放得下的档；
       发送帽同步放宽到 192 字（多帧拼装，见 push_sink 与 APK 分片）。
       Repair round (field fault #1): ui_text_box is a single-line auto-shrink
       control - it measures the paragraph as one line and scales the font down
       until it fits, driving 56 CJK characters to 1-3 px inside this column.
       That is the structural cause of "squeezed into one line, unreadably
       small", not the 70/30 geometry (verified correct by on-device measurement).
       Round 2: ui_text_block_fit picks the largest size in 18..34 that fits the
       whole string, so short summaries get large type and long ones step down
       just enough; the send cap is lifted to 192 chars via frame sharding.
       12:24 字体发糊归因：字数/字号/换行都对了，但中文笔画仍团成一团。根因是
       块排版原先逐行调 ui_text_fixed —— 阅读页 s_system_ttf=false 使它恒走内建
       点阵 + 24px 补充位图放大路径（1-bit 位图插值再经覆盖率抬升），而清晰的
       情景释义卡走 ui_text_reader（矢量 TTF 轮廓 + 灰阶抗锯齿）。此处改调
       ui_text_block_fit_reader，把量宽/行高/绘制整套切到同源 reader 链路。
       同屏左栏英文正文也是 TTF 轮廓、且同为 DU 快刷却清晰，故波形非分水岭，
       R0 冻结件 3 的 DU/GC16 计数一律零动。 */
    int used_px = 0;
    const int lines = ui_text_block_fit_reader(fb, inner, READER_COMPANION_TEXT_MAX_PX,
                                               READER_COMPANION_TEXT_MIN_PX, text, 6, &used_px);
    /* 现场判读用：右栏最终用了多大字号、排了几行、走哪条字体后端。
       12:24 字体发糊修复后的在机判别就靠 face=reader —— 只出现 face=bitmap
       说明跑的还是旧固件（或现场无字体卡），不必再靠照片量像素倒推。 */
    ESP_LOGI(TAG, "companion: text px=%d lines=%d box=%dx%d face=%s",
             used_px, lines, inner.width, inner.height,
             ttf_font_ready() ? "reader" : "bitmap");
}

static void draw_reader(uint8_t *fb,size_t page) {
    // 先同步方向再取正文矩形：翻横屏/开合面板后的第一帧就在这里重排。
    // Sync the orientation before taking the body rect: the first frame after an
    // orientation flip or a panel open/close reflows right here.
    page = ensure_body_orientation(page);
    ui_text_set_system_scale(false);ui_clear_page(fb);
    // 视觉页寻址：单栏下左栏即传入页、右栏不存在，与旧路径逐位一致；双栏时同一入口
    // 给出两栏逻辑页，右栏按栏矩形增绘（本步休眠：cols==1 时右栏分支与 JOINT 分支
    // 都退化成与旧路径相同的单次 draw_page）。JOINT 页独占视觉页，draw_page 对它
    // 本是 no-op，整页插图由 draw_reader_images 铺满联合区。
    // Visual addressing: single column yields the passed page as the left column with no
    // right side, matching the old path exactly; two columns resolve both logical pages
    // here and the right column paints into its own column rect (dormant this step: at
    // cols==1 both the right-column and JOINT branches degenerate to the same single
    // draw_page as the old path). A JOINT page owns its visual page alone, draw_page is
    // a no-op for it, and the whole-page image fills the joint area via
    // draw_reader_images.
    size_t left = page, right = SIZE_MAX;
    int mode = READER_VISUAL_MODE_COLUMNS2;
    if (page < book_layout_page_count()) {
        size_t visual = reader_visual_index(page);
        reader_logical_pages(visual, &left, &right);
        mode = reader_visual_mode(visual);
    }
    EpdRect area = body_rect();
    int cols = reader_column_count();
    if (mode == READER_VISUAL_MODE_JOINT) {
        book_layout_draw_page(fb, left, area, s_px);
    } else {
        book_layout_draw_page(fb, left, reader_column_rect(area, cols, 0), s_px);
        if (cols > 1 && right != SIZE_MAX)
            book_layout_draw_page(fb, right, reader_column_rect(area, cols, 1), s_px);
    }
    /* 伴读态：右栏不画第二个逻辑页（reader_logical_pages 返 1），只叠覆盖层区域。
       插图页同样走 else 分支——地图按 1 建时 mode 恒 COLUMNS2、JOINT 分支不可达；
       出血宽随本出口发布为主栏宽，页内插图 band 天然收在主栏内，右栏不被盖。 */
    if (reader_companion_active())
        reader_companion_overlay(fb, reader_column_rect(area, cols, 1));
    draw_reader_outer(fb, page);
    boxsel_draw(fb);
}
// 预渲染目标解析：给定当前逻辑页，求「下一视觉页」的左右逻辑页。
// Resolve the prefetch target: from the current logical page, the next visual page's
// left and right logical pages.
//
// 弱符号钩子（M1-M4 寻址改形的机械基础）：强定义在本 TU（page_map 在场），prep 决策
// 函数（kick_prep / reader_prefetch_idle）以块作用域弱声明调用它——产品构建走映射
// 路径；上游 139 行 pthread harness 的链接闭包不含 page_map 符号，弱声明地址为 0，
// 退化为上游 s_page+1 语义，harness 逐字节不动保持绿。单栏下 left≡s_page+1、
// right≡SIZE_MAX，谓词集合与上游逐位相同（关形态恒等）。
// 调用点经 volatile 函数指针取址：同 TU 强定义在场时 GCC 判「地址永真」
// （-Waddress，-Werror 即挂，产品构建已实证），volatile 中介防常量折叠；符号缺席
// 时指针为 0，走调用点自带的退化分支。两场景（在场/缺席）均经宿主编译 + 运行实测。
// Weak-symbol hook (the mechanical basis of the M1-M4 addressing change): the strong
// definition lives in this TU (page_map present); the prep decision functions
// (kick_prep / reader_prefetch_idle) call it via a block-scope weak declaration —
// product builds take the mapped path; the upstream 139-line pthread harness's link
// closure has no page_map symbol, the weak address is 0, and it degrades to the
// upstream s_page+1 semantics with the harness untouched. Single column yields
// left≡s_page+1 and right≡SIZE_MAX, so the predicate set is bit-identical to upstream
// (closed-form identity). Callers take the address through a volatile function
// pointer: with the strong definition in the same TU GCC proves the address always
// true (-Waddress, fatal under -Werror — observed in a real product build), and the
// volatile indirection defeats that folding; with the symbol absent the pointer is 0
// and the caller's own fallback branch runs. Both scenarios were verified by host
// compilation and execution.
bool reader_prep_target(size_t page, size_t* left, size_t* right) {
    size_t visual = reader_visual_index(page) + 1;
    if (visual >= reader_visual_count()) return false;
    reader_logical_pages(visual, left, right);
    return true;
}
// 预绘只在同步推屏期间借用字体；结束前逐行停止并交还，输入回调不等待整页预绘。
// Prefetch borrows fonts only during synchronous output; stop between rows before returning, never joining a whole page for input.
#define BOOK_PREP_RESERVE (1024u * 1024u)
#define BOOK_PREP_IDLE_US 4000
static int reader_prefetch_step(void) {
    if(!s_next_fb||s_prep_page<0)return -1;
    if(!s_prep_cursor.generation)memset(s_next_fb,0xff,fb_bytes());
    EpdRect area=body_rect();
    // 双栏切片经弱钩子（本步休眠接线）：产品构建把右栏拼进同一帧（钩子强定义见
    // reader_prep_slice）；上游 harness 链接闭包无钩子符号，volatile 指针取到 0，
    // 直调 draw_slice——参数与逐位行为跟改形前一致，上游断言原样成立。
    // Two-column slicing goes through the weak hook (this step's dormant wiring):
    // product builds stitch the right column into the same frame (the hook's strong
    // definition, reader_prep_slice); the upstream harness's link closure has no hook
    // symbol, the volatile pointer reads 0 and draw_slice is called directly — same
    // arguments, bit-identical behavior, upstream assertions hold as before.
    extern int reader_prep_slice(uint8_t* fb,size_t left,EpdRect area,int px,
                                 book_layout_draw_cursor_t* cursor) __attribute__((weak));
    int (* volatile slice_hook)(uint8_t*,size_t,EpdRect,int,book_layout_draw_cursor_t*)=reader_prep_slice;
    int (*slice)(uint8_t*,size_t,EpdRect,int,book_layout_draw_cursor_t*)=slice_hook;
    int result=slice?slice(s_next_fb,(size_t)s_prep_page,area,s_px,&s_prep_cursor)
                    :book_layout_draw_slice(s_next_fb,(size_t)s_prep_page,area,s_px,&s_prep_cursor);
    if(result<0)invalidate_prep();
    else if(result>0){s_next_page=s_prep_page;s_next_generation=book_layout_generation();}
    return result;
}
static bool reader_prefetch_buffer(void) {
    size_t need=fb_bytes();
    if(!s_next_fb&&heap_caps_get_free_size(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)>need+BOOK_PREP_RESERVE)
        s_next_fb=heap_caps_aligned_alloc(16,need,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    return s_next_fb!=NULL;
}
static void prep_task_self_stop(void); // 任务自删出口，prep_task 循环里调用 / the task's self-delete exit, called from prep_task's loop

static void prep_task(void *arg) {
    (void)arg;
    for(;;){
        ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        // gen25（自删形态）：醒来先看停机标志——外部只置位 + notify，绝不
        // vTaskDelete 本任务。强删一个正在干活的任务，FreeRTOS 只回收栈与 TCB，
        // 任务内部 malloc 的缓冲/临时指针会漏还，那才是新的碎片源，甚至可能
        // 持锁被删留下死锁隐患。这里由任务自己在安全点（无锁、无临时缓冲）
        // 交还：置空句柄 → give(done) → vTaskDelete(NULL)。
        // Gen25 (self-delete shape): check the stop flag first thing after waking -
        // the outside only sets the flag and notifies, it never vTaskDelete()s this
        // task. Force-deleting a task that is mid-work reclaims only the stack and
        // TCB; whatever the task malloc'ed leaks, and that becomes a fresh
        // fragmentation source - plus a task deleted while holding a lock invites
        // deadlock. The task hands itself back at a safe point instead (no lock,
        // no live scratch): clear the handle -> give(done) -> vTaskDelete(NULL).
        if (atomic_load_explicit(&s_prep_stop, memory_order_acquire)) { prep_task_self_stop(); }
        lock_draw();
        while(!atomic_load_explicit(&s_prep_stop,memory_order_acquire)){
            if(reader_prefetch_step()!=0)break;
        }
        unlock_draw();
        // 高水位 min-so-far：每轮工作结束采一次，取最小值。
        // High-water min-so-far: sampled at the end of every work round, keeping the minimum.
        UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
        if (!s_prep_hwm_words || hwm < s_prep_hwm_words) s_prep_hwm_words = hwm;
        if (atomic_load_explicit(&s_prep_stop, memory_order_acquire)) { prep_task_self_stop(); }
        xSemaphoreGive(s_prep_done);
    }
}

// 任务自删的唯一出口。/ The single exit the task takes when it stops itself.
static void prep_task_self_stop(void) {
    ESP_LOGI(TAG, "prep stack HWM: %u B free (peak used %u B) of %u B stack",
             (unsigned)(s_prep_hwm_words * sizeof(StackType_t)),
             (unsigned)((BOOK_PREP_STACK_SZ - s_prep_hwm_words) * sizeof(StackType_t)),
             (unsigned)(BOOK_PREP_STACK_SZ * sizeof(StackType_t)));
    // 先置空句柄（外部据此判定已停，天然幂等），再 give 让等待者继续，最后自删。
    // Clear the handle first (the outside reads it to see "stopped", which makes
    // this idempotent by construction), then give so the waiter proceeds, then
    // delete self.
    s_prep_task = NULL;
    if (s_prep_done) xSemaphoreGive(s_prep_done);
#if CONFIG_BOOK_PREP_STACK_PSRAM
    // 26.9.13 P0-a：栈是用 xTaskCreatePinnedToCoreWithCaps 建的（静态缓冲，内核不会
    // 替你回收），必须走配套的 vTaskDeleteWithCaps，且**传自己的句柄**才能命中它的
    // 自删分支。用 vTaskDelete(NULL) 会漏掉 6 K 的 PSRAM 栈与 TCB——恰好把这项开关的
    // 全部收益赔回去，而且漏得无声无息。
    // 代价（写在这里，免得下一个人以为是免费的）：IDF 的自删分支会临时建一个
    // configMINIMAL_STACK_SIZE 的**内部 RAM** 清理任务，建不出来就 abort。当前内部
    // 可用 ~32 K / 最大块 ~14 K，够；但这一条是实验开关默认关闭的又一个理由。
    // 26.9.13 P0-a: the stack came from xTaskCreatePinnedToCoreWithCaps (static buffers the
    // kernel will not reclaim), so it must go through vTaskDeleteWithCaps **with its own
    // handle** to hit the self-delete branch. A plain vTaskDelete(NULL) would quietly leak
    // the 6 K PSRAM stack and the TCB - giving back exactly the win this switch buys.
    // The cost, recorded so nobody assumes it is free: IDF's self-delete branch creates a
    // temporary configMINIMAL_STACK_SIZE cleanup task in INTERNAL RAM, and aborts if that
    // allocation fails. Internal free is ~32 K / largest ~14 K today, so it fits - and it
    // is one more reason this stays an experiment switch, off by default.
    vTaskDeleteWithCaps(xTaskGetCurrentTaskHandle());
#else
    vTaskDelete(NULL);
#endif
}

// 外部停机请求：置位 → 唤醒 → 等任务自删；**超时从不强删**，只记日志保留任务
// （6K 这次不还，下次让路链/开书再收）。/ The outside's stop request: set the
// flag, wake the task, wait for it to delete itself; a timeout never force-deletes,
// it only logs and leaves the task in place (the 6K waits for the next pass).
static void prep_task_request_stop(const char *why) {
    if (!s_prep_task) return;
    atomic_store_explicit(&s_prep_stop, true, memory_order_release);
    xTaskNotifyGive(s_prep_task); // 唤醒，让它自查退出 / wake it so it checks the flag
    bool stopped = false;
    if (s_prep_done) {
        (void)xSemaphoreTake(s_prep_done, 0); // 清掉上一轮的残留 / drain a stale give
        stopped = xSemaphoreTake(s_prep_done, pdMS_TO_TICKS(BOOK_PREP_STOP_WAIT_MS)) == pdTRUE;
    }
    if (stopped) {
        ESP_LOGI(TAG, "prep task stopped (%s): 6K stack returned", why ? why : "close");
    } else {
        // 不强删：任务仍持有内部缓冲/可能持锁，强删会漏内存。保留它，等它自己
        // 走到安全点退出；期间 kick_prep 会复用同一个任务，不会重复创建。
        // Never force-delete: the task may still hold internal buffers or a lock,
        // and deleting it would leak. Leave it; it exits at its own safe point,
        // and kick_prep reuses that same task meanwhile instead of creating a second.
        ESP_LOGW(TAG, "prep task still busy after %d ms (%s): kept alive, 6K stack stays for now",
                 BOOK_PREP_STOP_WAIT_MS, why ? why : "close");
    }
}
static bool kick_prep(void) {
    if(s_view!=READING||!s_text||s_toolbar||s_clear_confirm||s_reader_panel!=READER_PANEL_NONE||
       book_layout_page_image_count(s_page)||app_settings_book_background_path()[0])return false;
    // 目标视觉页走地图（M1-M4）：产品构建经弱钩子取下一视觉页的左右逻辑页；harness
    // 链接闭包无 page_map 符号时经 volatile 指针取到 0，退化为上游 s_page+1。末页
    // （M4）与图片守卫（M3）随映射一起收口——JOINT 配对下右栏有图同样拒预绘。
    // The target visual page resolves through the map (M1-M4): product builds take the
    // next visual page's left/right logical pages via the weak hook; the harness reads
    // 0 through the volatile pointer when its link closure lacks page_map and degrades
    // to upstream s_page+1. The last-page (M4) and image (M3) guards close over the
    // mapping together — under a JOINT pair an image in the right column refuses
    // prefetch just the same.
    extern bool reader_prep_target(size_t page,size_t *left,size_t *right) __attribute__((weak));
    bool (* volatile resolve_hook)(size_t,size_t*,size_t*)=reader_prep_target;
    bool (*resolve)(size_t,size_t*,size_t*)=resolve_hook;
    size_t left,right;
    if(resolve){if(!resolve(s_page,&left,&right))return false;}
    else{if(s_page+1>=book_layout_page_count())return false;left=(size_t)s_page+1;right=SIZE_MAX;}
    if(book_layout_page_image_count(left)||(right!=SIZE_MAX&&book_layout_page_image_count(right)))return false;
    int target=(int)left;
    if(s_next_page==target&&s_next_generation==book_layout_generation())return false;
    if(s_prep_page!=target||s_prep_cursor.generation!=book_layout_generation()){
        s_next_page=-1;s_next_generation=0;s_prep_page=target;memset(&s_prep_cursor,0,sizeof(s_prep_cursor));
    }
    if(!reader_prefetch_buffer())return false;
    if(!s_draw_lock)s_draw_lock=xSemaphoreCreateMutex();
    if(!s_prep_done)s_prep_done=xSemaphoreCreateBinary();
    if(!s_prep_task&&s_draw_lock&&s_prep_done&&
#if CONFIG_BOOK_PREP_STACK_PSRAM
       // 26.9.19 起**默认开启**（审计裁定，推翻 26.9.13「永不启用」）。当年关掉的理由是
       // 「有风险且没到绝境」；26.9.18 真机的 boot-post 块表把绝境摆到了台面上：这 6,144 B
       // 后面紧跟着一个 100 B 的常驻块，关书归还后与后面 13,632 B 的空闲块**不相邻、
       // 不合并**，largest 永远停在 13,312 < 面板红线 15,360，而这块栈峰值只用 2,788 B。
       // 26.9.19 ships this ON (audit ruling, overruling 26.9.13's "never"). The 26.9.18
       // boot-post table made the corner plain: a permanent 100-byte block sits right behind
       // these 6,144 bytes, so the returned stack never merges into the 13,632-byte free
       // block behind it; largest sticks at 13,312, under the panel's 15,360 red line, while
       // this stack's peak use is only 2,788 B.
       // 26.9.13 P0-a：栈落 PSRAM。
       // 为什么可以推翻 app_book.c 里「任务栈是每帧热路径，不可搬 PSRAM」那条冷热纪律
       // （00:4707 附近，原文保留在原处不删）：prep **不是每帧路径**，它是翻页预排——
       // 一次只跑几十到几百毫秒，晚一拍用户看不见；而它的重缓冲（字体 working 区、
       // 预取页位图）本来就在 PSRAM，栈跟过去不改变任何访问局部性。
       // 为什么这就是 P0 的根因解法：开书瞬间内部最大连续块 21,504 → 14,336，
       // 差 7,168 = 6,144（栈）+ 1,024（TCB 及对齐），关书后栈已归还但洞合不回去。
       // 把 6,144 从内部堆里**永久**拿走，这一代就再也不用为「开过书」付 7,168。
       // 风险与守卫（审计裁定要求写明）：真正的危险是 cache 关闭窗口（NVS 提交 /
       // OTA 写 flash）期间被调度——那时读 PSRAM 会崩。两条都不成立：
       //   ① prep_task 的调用链只有 lock_draw/unlock_draw、book_layout_draw_slice、
       //      字体栅格与 fb 写入，**一次性**核对过，不含 nvs_commit / esp_ota_* /
       //      esp_flash_*；
       //   ② flash 操作在关中断 + 另一核被 stall 的窗口里进行，窗口内不会发生任务切换，
       //      PSRAM 栈任务只能在 cache 恢复之后才被恢复运行。
       // 仍然保留开关而不是删掉：真机判据只有一行——
       // `book: open start ... largest=` 应从 13,312 回到 19,456 左右（见 26.9.13 报告 §2.6、
       // 26.9.18 真机账本）。19,456 仍低于 BLE 闸门 20,480，闸门放行不归本开关管。
       // 26.9.13 P0-a (experiment, default off): the stack lives in PSRAM.
       // Why this may overrule the hot/cold discipline at app_book.c:4707 ("a task stack is
       // per-frame hot, never move it to PSRAM"), which stays where it is: prep is NOT a
       // per-frame path - it pre-renders the next page over tens to hundreds of ms, and one
       // tick of delay is invisible - and its heavy buffers (font working area, prefetched
       // page bitmap) already live in PSRAM, so moving the stack changes no locality.
       // Why this is the root-cause fix: opening a book drops the internal largest block
       // from 21,504 to 14,336, exactly 7,168 = 6,144 (stack) + 1,024 (TCB/alignment), and
       // closing never merges the hole back. Taking those 6,144 out of the internal heap
       // for good means this boot generation never pays the 7,168 again.
       // Risks and guards (required by adjudication): the real danger is being scheduled
       // inside a cache-off window (NVS commit / OTA flash write), where touching PSRAM
       // crashes. Neither applies: (1) prep_task's call chain is lock_draw/unlock_draw,
       // book_layout_draw_slice, font raster and fb writes - checked line by line, no
       // nvs_commit / esp_ota_* / esp_flash_*; (2) flash operations run with interrupts off
       // and the other core stalled, so no task switch can happen inside the window and a
       // PSRAM-stack task only resumes after the cache is back.
       // It stays a switch rather than a new default because it is an inference, not a heap
       // dump. The hardware verdict is one line: `book: open start ... largest=` should come
       // back from 13,312 to about 19,456 (the 6,144 this stack used to eat). 19,456 still
       // sits below the BLE gate of 20,480 - opening that gate is a later job, not this one.
       xTaskCreatePinnedToCoreWithCaps(prep_task,"book_prep",BOOK_PREP_STACK_SZ,NULL,3,&s_prep_task,1,
                                       MALLOC_CAP_SPIRAM)!=pdPASS)s_prep_task=NULL;
#else
       xTaskCreatePinnedToCore(prep_task,"book_prep",BOOK_PREP_STACK_SZ,NULL,3,&s_prep_task,1)!=pdPASS)s_prep_task=NULL;
#endif
    if(!s_prep_task)return false;
    xSemaphoreTake(s_prep_done,0);
    atomic_store_explicit(&s_prep_stop,false,memory_order_release);
    xTaskNotifyGive(s_prep_task);
    return true;
}
static void reader_prefetch_end(bool started) {
    if(!started)return;
    atomic_store_explicit(&s_prep_stop,true,memory_order_release);
    // 最多交还当前一行，之后主循环才可改字体、章节或离页，避免竞态。
    // Hand back at the current row boundary before the loop may change fonts, chapters or pages.
    xSemaphoreTake(s_prep_done,portMAX_DELAY);
}
static bool reader_prefetch_resume(size_t page) {
    if(!s_next_fb||s_prep_page!=(int)page||!s_prep_cursor.started||
       s_prep_cursor.generation!=book_layout_generation())return false;
    // 连按到尚未完成的目标时接着画，保留已画文字，不清空重画整页。
    // A rapid turn completes the remaining rows of its target, retaining work already drawn.
    while(reader_prefetch_step()==0){}
    return s_next_page==(int)page&&s_next_generation==book_layout_generation();
}
static void reader_prefetch_idle(app_ctx_t *ctx) {
    if(s_view!=READING||!s_text||s_toolbar||s_clear_confirm||s_reader_panel!=READER_PANEL_NONE||
       ctx->consumed||ctx->request_app||ctx->request_menu||ctx->request_return||
       (ctx->touch&&ctx->touch->touched))return;
    // 激活条件与图片/末页守卫同步映射（M2）：与 kick_prep 同公式，否则 idle 永不激活
    // 或错激活；harness 经同一 volatile 指针走退化（s_page+1）。
    // The activation condition and image/last-page guards map in step (M2): same formula
    // as kick_prep, or idle never activates or activates wrongly; the harness degrades
    // the same way (s_page+1) through the same volatile pointer.
    extern bool reader_prep_target(size_t page,size_t *left,size_t *right) __attribute__((weak));
    bool (* volatile resolve_hook)(size_t,size_t*,size_t*)=reader_prep_target;
    bool (*resolve)(size_t,size_t*,size_t*)=resolve_hook;
    size_t left,right;
    if(resolve){if(!resolve(s_page,&left,&right))return;}
    else{if(s_page+1>=book_layout_page_count())return;left=(size_t)s_page+1;right=SIZE_MAX;}
    if(s_prep_page!=(int)left)return;
    if(app_settings_book_background_path()[0]||book_layout_page_image_count(left)||
       (right!=SIZE_MAX&&book_layout_page_image_count(right)))return;
    if(s_next_page==s_prep_page&&s_next_generation==book_layout_generation())return;
    if(!reader_prefetch_buffer())return;
    const int64_t deadline=esp_timer_get_time()+BOOK_PREP_IDLE_US;
    for(unsigned row=0;row<8;++row){
        if(reader_prefetch_step()!=0||esp_timer_get_time()>=deadline)break;
    }
}
static size_t current_toc_position(void) {
    size_t count = book_navigation_count();
    if (s_selected_toc < count && book_navigation_chapter(s_selected_toc) == s_chapter)
        return s_selected_toc;
    size_t selected = SIZE_MAX;
    size_t best_chapter = 0;
    for (size_t i = 0; i < count; ++i) {
        size_t chapter = book_navigation_chapter(i);
        if (chapter > s_chapter) break;
        if (selected == SIZE_MAX || chapter > best_chapter) { selected = i; best_chapter = chapter; }
    }
    return selected;
}
static void render(app_ctx_t* ctx, uint8_t* fb) {
    // 阅读横屏：仅 READING 视图按设置切到横屏，其它视图(书架/TOC/编辑/导入等)保持竖屏。
    // 方向只在视图切换时改变，而视图切换必走 PAGE/FULL 全刷——全刷会重绘整帧并隐藏方向翻转，
    // 翻页的局部差分刷沿用上次方向，与 epdiy 的 _inverse_rotated_area(横屏为恒等)天然一致。
    // Landscape reading: only the READING view switches per setting; every other view stays
    // portrait. Orientation changes only on view switches, which are always PAGE/FULL full
    // refreshes that repaint the whole frame and hide the flip; page-turn partial diffs keep
    // the last orientation, matching epdiy's _inverse_rotated_area (identity under landscape).
    // 只有"纯阅读正文"与"点词释义卡"走横屏；工具栏/设置/统计/书签等面板保持竖屏(像素级不变、
    // 零触摸映射风险)，打开时借全刷翻回竖屏、关闭翻回横屏。释义卡是阅读最高频操作，单独保持横屏。
    // Only the plain body and the tap-to-lookup card go landscape; toolbar/settings/stats/bookmark
    // panels stay portrait (pixel-identical, zero touch-mapping risk), flipping back on a full
    // refresh when opened. The lookup card is the highest-frequency reading action, so it stays landscape.
    enum EpdRotation want = (s_view == READING && app_settings_reader_landscape() &&
                             (s_reader_panel == READER_PANEL_NONE || s_reader_panel == READER_PANEL_DICT))
                                ? EPD_ROT_LANDSCAPE : EPD_ROT_INVERTED_PORTRAIT;
    if (epd_get_rotation() != want) epd_set_rotation(want);
    ui_text_set_system_scale(s_view != READING && s_view != TOC);
    lock_draw();
    ui_text_set_system_scale(s_view != READING);
    if (s_view == EDIT) { draw_editor(fb); unlock_draw(); return; }
    if (s_view == SEARCH) { draw_search(fb); unlock_draw(); return; }
    if (s_view == IMPORT) { draw_import(fb); unlock_draw(); return; }
    if (s_view == BULK) { draw_bulk(fb, ctx->leaf); unlock_draw(); return; }
    if (s_view == MANAGE) {
        ui_clear_page(fb);
        ui_nav_status(fb);
        ui_nav_back(fb, 36, 79);
        ui_text_vc(fb, 342, 107, 34, "图书详情", EPD_DRAW_ALIGN_CENTER, false);
        ui_hairline(fb, 176, 36, 612, 0x70);
        draw_manage(fb);
        unlock_draw();
        return;
    }
    if (s_view == TOC) {
        book_toc_render(fb, s_book_title[0] ? s_book_title : s_title,
                        book_navigation_count(), current_toc_position(), ctx->leaf, s_message);
        if (s_toc_jump_open)
            book_toc_render_jump(fb, book_navigation_count(), s_toc_jump_percent);
        unlock_draw();
        return;
    }
    // M2 Step9：AI 详解覆盖会话优先于阅读正文——它是 READING 视图上的一层，
    // 方向沿用阅读设置（横屏阅读者看横屏详解；ai_reader_render 内部按实际
    // 版面实算每行字数/每页行数，分页与所见一致）。原书正文不画、章节缓冲
    // 不动（铁律②）；render 只绘图，会话状态由输入链管理。
    // M2 Step9: the AI detail overlay takes precedence over the reader body -
    // it is a layer on the READING view and inherits the reading rotation (a
    // landscape reader sees a landscape detail; metrics are computed from the
    // measured layout inside ai_reader_render so pagination matches the view).
    // The source body is not painted and its chapter buffer is untouched (law
    // 2); render only draws, the input chain owns the session state.
    if (s_view == READING && ai_reader_active()) {
        // gen26.6.3 死锁修复：本函数入口（上方 lock_draw）**已经持有** s_draw_lock，
        // 这里再 lock_draw() 就是同一任务对非递归互斥量的二次获取 → portMAX_DELAY
        // 永久阻塞，设备整机挂死（真机首次打开详解即死机、日志停在
        // "detail session opened" 后无 panic 转储的根因）。分支只需在返回前解锁一次，
        // 与其它分支同构。/ gen26.6.3 deadlock fix: this function already holds
        // s_draw_lock (the lock_draw above); taking it again here is a second
        // acquire of a NON-recursive mutex by the same task -> blocks forever on
        // portMAX_DELAY and hangs the whole device (the root cause of the
        // on-device freeze right after "detail session opened", with no panic
        // dump because it is a hang, not an exception). The branch only has to
        // unlock once before returning, like every other branch here.
        // gen26.6.5：详解页排版**直接取正文**——字号 s_px、行距 s_px×行距%/100（与
        // book_layout 同一公式）、版心 body_rect()（已按整字格收齐）。上一版在
        // ai_reader 里写死 72px/92/36，正文是 51px/76/整字格 → 真机「大字报」。
        // 度量只有一套，这里注入，那边不再自算。
        // gen26.6.5: the detail view takes the LIVE body's metrics - s_px for size,
        // s_px*spacing%/100 for pitch (the same formula book_layout uses) and
        // body_rect() for the measure (already snapped to glyph cells). The previous
        // build hard-coded 72px/92/36 inside ai_reader while the body ran 51/76 - the
        // on-device "billboard". One set of metrics: injected here, never re-derived.
        const EpdRect detail_body = body_rect();
        const ai_reader_style_t detail_style = {
            .px = s_px,
            .line_h = s_px * s_line_spacing / 100,
            .left = detail_body.x,
            .width = detail_body.width,
        };
        ai_reader_set_style(&detail_style);
        // M3-B：情景卡是整屏视图，版心几何交给 ai_reader_scene_geom——上沿
        // AI_READER_SCENE_TOP=96（= 阅读页全屏非沉浸正文顶，状态栏下），下沿扣
        // 48px 动作栏。**不再是 236**：236 是详解「标题区之后」的正文顶，情景卡
        // 没有标题区，照抄会留下 0..236 白板（26.9.2 修复）。详解不注入（NULL），
        // 走自己的 236 / h-72，逐像素不变。
        // M3-B: the scene card is a full-screen view; geometry comes from
        // ai_reader_scene_geom - top AI_READER_SCENE_TOP=96 (the reader's
        // fullscreen non-immersive body top, under the status bar), bottom above
        // the 48px action bar. NOT 236 anymore: 236 is the detail view's
        // "after-the-title-zone" top, and the card has no title zone, so copying
        // it left 0..236 white (fixed in 26.9.2). The detail path injects nothing
        // (NULL), keeps 236 / h-72 and stays pixel-identical.
        if (ai_reader_mode() == AI_READER_MODE_SCENE) {
            const ai_reader_geom_t scene_geom =
                ai_reader_scene_geom(&detail_style, UI_LOCK_HEIGHT);
            ai_reader_set_geometry(&scene_geom);
        } else {
            ai_reader_set_geometry(NULL);
        }
        ai_reader_render(fb);
        unlock_draw();
        return;
    }
    if (s_view == READING && s_text) {
        draw_reader(fb, s_page);
        unlock_draw();
        return;
    }
    ui_clear_page(fb);
    // 普通和水波纹共享灰阶画面，只有快刷使用黑白底色。
    // Normal and ripple share gray artwork; only fast refresh uses the BW background.
    ui_fill_rect_fast((EpdRect){0, 0, UI_LOCK_WIDTH, UI_NAV_TOP},
                  app_settings_main_fast_refresh() ? 0xf0 : 0xe0, fb);
    ui_nav_status(fb);
    char category_title[BOOK_STORE_PATH_MAX];
    copy_text(category_title,sizeof(category_title),s_category_selected ? shelf_category_name(s_category_selected) : "书架");
    // P0-5（26.11.2）入口甲：书架第三按钮「摘抄」挤进标题行，标题预算 326 → 230。
    // 三处命中矩形（标题 36..266 / 下拉箭头 293..319 / 摘抄按钮 333..430）由下方
    // _Static_assert 编译期咬死，谁改一处谁改牙。
    // P0-5 entrance A: the third header button squeezes the title budget to 230;
    // the three hit rectangles are locked at compile time below.
    fit_text(category_title,52,230);
    ui_text(fb, 36, 90, 52, category_title, EPD_DRAW_ALIGN_LEFT, false);
    int title_width=ui_text_fixed_width_px(ui_text_effective_px(52),category_title);
    ui_draw_icon(fb,36+title_width+27,120,26,
                 s_category_menu ? UI_ICON_CHEVRON_UP : UI_ICON_CHEVRON_DOWN,UI_GRAY_BLACK);
    char excerpt_label[24];
    draw_shelf_header_button(fb, shelf_excerpt_rect(), 118, shelf_excerpt_label(excerpt_label, sizeof(excerpt_label)), 20);
    draw_shelf_header_button(fb, shelf_manage_rect(), 114, "管理", 20);
    draw_shelf_header_button(fb, shelf_import_rect(), 115, "+ 导入", 20);
    if (s_message[0] || s_shelf_warning[0])
        ui_text(fb, UI_MARGIN, 991, 17, s_message[0] ? s_message : s_shelf_warning,
                EPD_DRAW_ALIGN_LEFT, false);
    if (app_settings_shelf_style() == 5) {
        ui_fill_round_rect(fb, (EpdRect){36,184,612,788}, 20, UI_GRAY_WHITE);
        ui_draw_control_frame(fb, (EpdRect){36,184,612,788}, 20, 0x50);
    }
    for (int row = 0; row < shelf_rows(); ++row) {
        int i = ctx->leaf * shelf_rows() + row;
        if (i >= s_visible_count) break;
        EpdRect r = row_rect(row);
        char name[128]; copy_text(name, sizeof(name), s_shelf[i].name);
        // 封面抽出只改变书本位置，层板及触摸目标保持原位。
        // Lifting changes the book position only; shelf furniture and touch targets stay put.
        if (s_pressed_control == row)
            r.y -= app_settings_shelf_style() == 5 ? SHELF_LIST_BOUNCE_PX : SHELF_BOOK_LIFT_PX;
        draw_shelf_cover(fb, r, row, name, s_shelf[i].favorite);
        if (app_settings_shelf_style() == 5) draw_list_row(fb, row, &s_shelf[i]);
    }
    draw_shelf_furniture(fb);
    draw_shelf_pager(fb, s_visible_count, ctx->leaf, leaves());
    ui_nav_draw(fb, 1);
    if (s_category_menu) draw_category_menu(fb);
    unlock_draw();
}
static EpdRect reader_status_area(void) { return (EpdRect){0,0,UI_LOCK_WIDTH,UI_STATUS_STRIP_H}; }
static bool reader_status_visible(void) {
    return s_view==READING && s_text && !(s_reader_fullscreen && app_settings_reader_immersive());
}
static void reader_status_paint(uint8_t *fb) {
    EpdRect area=reader_status_area();
    ui_fill_rect_fast(area,UI_GRAY_WHITE,fb);
    ui_nav_status(fb);
    if (s_reader_panel!=READER_PANEL_BACKGROUND_PREVIEW && app_settings_book_background_path()[0])
        ui_reader_background_paint(fb,area,true);
}
static app_redraw_t reader_clock_tick(app_ctx_t *ctx) {
    if (!reader_status_visible() || (ctx->touch && ctx->touch->touched)) return APP_REDRAW_NONE;
    if (ui_nav_clock_minute()==s_reader_clock_minute) return APP_REDRAW_NONE;
    s_reader_clock_painted_minute=ui_nav_clock_minute();
    lock_draw(); reader_status_paint(ctx->fb); unlock_draw();
    s_reader_clock_pending=true;
    return APP_REDRAW_AREA;
}
static bool present(app_ctx_t* ctx, app_redraw_t redraw) {
    if (s_reader_clock_pending) {
        s_reader_clock_pending=false;
        if (redraw==APP_REDRAW_AREA && reader_status_visible()) {
            // 时钟单独短刷，不准备插图、不预排正文，也不累计正文全刷次数。
            // Update status alone, without image preparation, pagination, or reader cleanup counters.
            enum EpdDrawError err=update_display_area_diff_with(ctx->hl,&E0470_WAVEFORM,MODE_GL16,reader_status_area());
            if (err==EPD_DRAW_SUCCESS) s_reader_clock_minute=s_reader_clock_painted_minute;
            guard_draw_result(ctx->hl,err);
            return true;
        }
    }
    // 26.9.26 S26-2：摘抄回执只重画页脚这一条带（照上面的时钟范式）。
    // 26.9.26 S26-2: the excerpt receipt repaints one footer strip, the same way the clock
    // branch above repaints the status strip.
    if (s_excerpt_footer_pending) {
        s_excerpt_footer_pending = false;
        if (redraw == APP_REDRAW_AREA && s_view == READING && s_text && !s_reader_fullscreen) {
            guard_draw_result(ctx->hl, update_display_area_diff_with(
                ctx->hl, &E0470_WAVEFORM, MODE_GL16, progress_rect()));
            return true;
        }
    }
    // 输入不准备封面或正文；候选停顿只整理词栏，不累计整页清屏次数。
    // Input skips cover/body preparation; idle candidate settling affects only its strip and not whole-page cleanup counting.
    if (redraw == APP_REDRAW_AREA && (s_view == EDIT || s_view == SEARCH)) {
        if (s_input_settle) {
            guard_draw_result(ctx->hl, update_display_area_full_with(ctx->hl, &E0470_WAVEFORM, MODE_GL16, s_area));
            s_input_settle = false;
            s_du_count = 0;
            return true;
        }
        guard_draw_result(ctx->hl, update_display_area_diff_with(ctx->hl,
            s_mode == MODE_DU ? &E0470_FOLLOW_WAVEFORM : &E0470_WAVEFORM, s_mode, s_area));
        s_du_count = 0;
        return true;
    }
    if (redraw == APP_REDRAW_NONE || redraw == APP_REDRAW_DONE || ctx->request_app) {
        s_reader_page_advance = false;      // 这一拍没播屏，不记账 / nothing presented, nothing booked
        return true;
    }
    // 26.11.7 S2：页码前进的账只在这里结一次——全工程唯一的 s_turns 自增点。
    //
    // 为什么搬进来：26.11.6 的计数在 turn_page()/reader_ai_turn() 里自增，比实际播屏
    // 早了一步。中间任何一条提前 return（request_app、摘抄回执带、输入态整理）都会
    // 让「数了 5 页」和「闪了 5 次」对不上——而 26.11.6 的真机数正是对不上：设置 5、
    // 62 次翻页、零次清底。账要跟着屏走，不能跟着意图走。
    //
    // `n &&` 不能省：N=0 是「不强制」档，语义是**永不因计数清底**。去掉 n 之后
    // `++s_turns >= 0` 恒真，这一档会退化成每拍清底——正好是它要避免的那个结果。
    // 宿主真值表 N=0 那一格的断言方向因此是「永不 cleanup」，不是「每拍 cleanup」。
    // The `n &&` guard is load-bearing: n == 0 means "never clean by count". Without it
    // `++count >= 0` is always true and the "off" setting becomes a per-turn full refresh;
    // the host truth table asserts "never cleanup" for that row, not "cleanup every turn".
    if (s_reader_page_advance && s_view == READING) {
        const uint8_t full_pages = app_settings_reader_full_pages();
        const bool due = reader_note_turn();
        if (due) s_reader_cleanup = true;
        // 灰账的出处：每拍一行，翻页次数与清底次数都能从这里数出来。
        // The gray ledger's provenance: one line per turn, so both counts are countable here.
        ESP_LOGI(TAG, "r_turn adv=1 n=%u turns=%d clean=%d", (unsigned)full_pages, s_turns, (int)due);
    }
    if (s_presented_view != (int)s_view && redraw == APP_REDRAW_AREA) redraw = APP_REDRAW_PAGE;
    // 26.11.5 刀 A：跨视图**进出阅读**一律整屏 GC16。
    //
    // 判据是视图边沿，不是「用户点了哪个按钮」——书架、文件页、首页卡、推送、
    // resume 五条入口一次全覆盖。26.11.4 真机照片里的那层叠影正是这条：旧页的
    // 字形留在第一屏上，因为进阅读走的是差分（只驱动变化像素），旧墨没人擦。
    //
    // 只用 AREA/PAGE 进来的这一层之前是 PAGE（整屏差分 GL16，37 帧），比整屏
    // GC16（36 帧）还贵 15 ms 却不擦底——本批把它换成 FULL，不是「为清晰变慢」。
    // 26.11.5 cut A: crossing into or out of the reader is a screen-wide GC16.
    // The predicate is the view edge, not which button was tapped, so all five
    // entry paths are covered at once. The 26.11.4 photo is exactly this: the old
    // page's glyphs stay on the first screen because entering the reader went
    // through a differential (changed pixels only) with nobody wiping the old ink.
    // That path used to be PAGE (screen-wide GL16 diff, 37 frames) - 15 ms more
    // expensive than a full GC16 (36 frames) and still without a white reset.
    if (s_presented_view != (int)s_view &&
        (s_view == READING || s_presented_view == READING))
        redraw = APP_REDRAW_FULL;
    if ((s_view == SHELF || s_view == MANAGE) &&
        !(redraw == APP_REDRAW_AREA && (s_shelf_feedback_pending || s_cover_batch_present_pending))) {
        prepare_covers(ctx);
        gather_cover_entry(ctx);
    }
    if (ctx->request_app) return true;
    if (s_view == READING && s_text) {
        if (s_reader_panel != READER_PANEL_BACKGROUND_PREVIEW)
            (void)ui_reader_background_prepare(app_settings_book_background_path());
        prepare_inline_image();
    }
    int64_t start = esp_timer_get_time();
    const int64_t frame_minute=ui_nav_clock_minute();
    if (redraw == APP_REDRAW_PAGE || redraw == APP_REDRAW_FULL) render(ctx, ctx->fb);
    bool prep = kick_prep();
    int64_t drawn = esp_timer_get_time();
    // 本地细点阵挡板已是黑白目标，不注册灰区，省去暂存和第二轮GL16。
    // Local fine-dot guards are already BW targets; omit gray bands to skip staging and the second GL16 pass.
    if (s_view == SHELF && s_shelf_page_pending) display_main_transition_shelf_page();
    s_shelf_page_pending = false;
    enum EpdDrawError err;
    bool image_gray_refresh = s_reader_image_refresh_pending && s_view == READING &&
        s_reader_panel == READER_PANEL_NONE && !s_toolbar && !s_clear_confirm;
    if (image_gray_refresh && redraw == APP_REDRAW_AREA && !s_reader_cleanup && s_presented_view == (int)READING) {
        // 灰阶只驱动正文，避免每张插图都把整屏先清白再闪回。
        // Drive the grayscale body from the current frame instead of clearing the whole screen to white.
        err = app_settings_reader_turn_effect() == 1
            ? update_display_area_diff_with(ctx->hl, &E0470_WAVEFORM, MODE_GL16, reader_area())
            : update_display_area_with(ctx->hl, &E0470_FULL_WAVEFORM, MODE_GC16, reader_area());
        if (err == EPD_DRAW_SUCCESS && s_reader_footer_pending)
            err = update_display_area_diff_with(ctx->hl, &E0470_WAVEFORM, MODE_GL16, progress_rect());
        if (err == EPD_DRAW_SUCCESS && s_reader_fullscreen)
            err = update_display_area_diff_with(ctx->hl, &E0470_WAVEFORM, MODE_GL16,
                                                reader_fullscreen_progress_area());
    }
    else if (image_gray_refresh) err = app_settings_reader_turn_effect() == 1 &&
        redraw != APP_REDRAW_FULL && !s_reader_cleanup
        ? update_display_mode_diff(ctx->hl, MODE_GL16)
        : update_display_with(ctx->hl, &E0470_FULL_WAVEFORM, MODE_GC16);
    // gen24.4：推送收官（s_open_full_once）与常规全刷/清屏同一出口——整屏
    // GC16 擦旧底，第一页纯黑纯白无残影。/ Gen24.4: the push finale
    // (s_open_full_once) shares the regular full/cleanup exit - one screen-wide
    // GC16 wipes the old base, so the first page is pure black on pure white.
    else if (redraw == APP_REDRAW_FULL || s_reader_cleanup || s_open_full_once) {
        s_open_full_once = false;
        s_turns = 0;                        // 不变式：任何整屏清底后计数归零
        err = update_display_full(ctx->hl);
    }
    else if (redraw == APP_REDRAW_AREA && s_view == SHELF && s_shelf_feedback_pending) {
        // 抽出与复位只驱动发生变化的像素，避免整张封面被反复压黑。
        // Lift and restore only changed pixels, avoiding a dark pulse over the whole cover.
        err = update_display_area_diff_with(ctx->hl,
            s_mode == MODE_DU ? &E0470_FOLLOW_WAVEFORM : &E0470_WAVEFORM, s_mode, s_area);
    }
    else if (redraw == APP_REDRAW_AREA) {
        // 快档每次显式设置并在推屏后恢复，不能把阅读节拍带入主页水波纹。
        // Explicitly select fast pacing per turn and restore afterward, without leaking reader timing into main-page ripple.
        const int prior_tick = s_water_turn_pending ? e0470_page_turn_tick_us() : 0;
        if (s_water_turn_pending) e0470_page_turn_set_tick_us(E0470_TURN_FAST_TICK_US);
        // 26.11.5 cut B 曾以「差分也不省时间」为由把这里换成 `_area_full` + GC16；
        // 26.11.6 撤回，理由见下方 T0 分支的整段说明。历史保留一行，免得下一次
        // 有人翻到旧结论又把它改回去。
        // 26.11.5 cut B swapped this for `_area_full` + GC16 citing "diff is not
        // cheaper"; 26.11.6 reverts it - see the T0 branch below for why.
        if (s_water_turn_pending) {
            err = update_display_water_turn(ctx->hl, s_area, s_water_turn_dir);
            // 26.11.5 裁决 (c)：水波纹是**显式选择**，开启即接受「动画 + 收尾清底」
            // 的代价，设置页写明。默认关——清晰是默认。只对真翻页补，菜单/气泡的
            // AREA 不动。
            //
            // 26.11.7 W1：收尾从**整屏**收成**正文区**。26.11.6 真机报「每次翻页都
            // 闪屏」，日志 1091 ms 里 `1091 − 643 = 448 ms` 就是那一记整屏 GC16——
            // 动画本身只脏正文区（`dirty_lines`/`dirty_columns` 只标正文），把整屏
            // 都擦一遍是裁决 (c) 方向对、范围错。收尾仍要 GC16 全像素：动画扫过的
            // 那块必须重写成白底，差分补不回来。
            // Ruling (c) stands, but 26.11.7 W1 narrows the closing wipe from the whole
            // screen to the body area: the animation only dirties the body, and the
            // 448 ms GC16 (1091 - 643 in the log) was what made every turn flash.
            // Still GC16 full-pixel inside that area - the swept region must be rebuilt
            // white, which a differential cannot do.
            if (err == EPD_DRAW_SUCCESS && s_reader_turn_pending)
                err = update_display_area_full_with(ctx->hl, &E0470_WAVEFORM, MODE_GC16, reader_area());
        } else if (s_reader_clear_from_image) {
            // 26.11.6 T3：上一页有灰阶、这一页纯文字 —— 正文区整区清一次。
            //
            // 判据是「灰阶内容离开了」，不是面积。面积在翻页上恒真（翻页的脏区
            // 永远等于正文区），26.11.5 就是被这个恒真判据带着把所有翻页都判成
            // 了「该整页重绘」。灰阶残影比文字难消，所以这一档值得一次区清；
            // 图 → 图不触发，有图页是少数。
            // 26.11.6 T3: the previous frame carried grayscale, this one is text only,
            // so clean the whole body area once. The predicate is "grayscale left",
            // not area - area is vacuously true for any turn, which is exactly how
            // 26.11.5 ended up treating every turn as a whole-page repaint.
            err = update_display_area_full_with(ctx->hl, &E0470_WAVEFORM, MODE_GC16, reader_area());
        } else if (s_reader_turn_pending) {
            // 26.11.8 ②：正文翻页＝上游同款「柔和波形 × 全覆盖」。
            //   上游 `_area_full_with(display_reader_soft_turn_waveform(), GL16, s_area)`。
            //
            // 26.11.7 这里是 GC16 + 差分（36 相裁剪表），真机三档同时炸：
            //   ① 每页整块闪（GC16 相序里黑相在前）；
            //   ② 左缘竖带（单次「深→白」推进不足，少数列停在灰，约 4 页累积可见）；
            //   ③ 反色停留（`s_inv_on` 的逐行过渡态推不过去，停住被看见）。
            // 三者同源：**一张裁剪表在差分的单次推进里给不出足够的白**。
            // 全像素刷能推白（B1 实测带消失），所以表不是坏的，缺的是推进量。
            //
            // kiko rc104 同屏同表同驱动，只换这一行：竖带消失、无反色、每页不闪。
            // 上游那一格是「柔和波形 × 全覆盖」——取 GL16 的 37 相 LUT，砍尾部
            // 无动作相，插 12 静止相 + 4 黑推相 = 48 相，白目标优先、黑相靠后。
            //
            // 26.11.8 ②: body turns use the upstream "soft waveform x full coverage"
            // cell. 26.11.7 used GC16 + differential with the 36-phase trimmed table,
            // and the panel showed all three symptoms at once: whole-block flash, the
            // left-edge band, and inverted text holding mid-transition. One cause - a
            // trimmed table cannot supply the white push a differential needs. The
            // upstream 48-phase soft profile does, and kiko on the same panel showed
            // none of the three.
            err = update_display_area_full_with(ctx->hl, display_reader_soft_turn_waveform(),
                                                MODE_GL16, s_area);
        } else {
            err = update_display_area_with(ctx->hl, &E0470_WAVEFORM, s_mode, s_area);
        }
        if (s_water_turn_pending) e0470_page_turn_set_tick_us(prior_tick);
        if (s_reader_fullscreen && err == EPD_DRAW_SUCCESS) {
            // 百分比不变时差分直接跳过；变化时只驱动最底部的少量像素。
            // Skip unchanged percentages; otherwise update only the small bottom strip.
            err = update_display_area_diff_with(ctx->hl, &E0470_WAVEFORM, MODE_GL16,
                                                reader_fullscreen_progress_area());
        }
        if (s_reader_footer_pending) {
            // 普通翻页页脚只驱动变化像素；全刷由上方整屏分支一次完成。
            // Ordinary turns drive only changed footer pixels; the full-screen branch handles cleanup at once.
            err = (enum EpdDrawError)(err | update_display_area_with(ctx->hl, &E0470_FOLLOW_WAVEFORM,
                                                                     MODE_DU, progress_rect()));
            ESP_LOGI(TAG, "reader regions body_h=%d footer=diff pct=%u", s_area.height, percent(s_page));
        }
    }
    else if (s_view == READING) {
        // 26.11.6：阅读视图的整页出口拆成两路。
        //
        //   页码前进（AI 解析页翻页）→ 正文区差分，和正文翻页同一档；
        //   进入 / 跳章 / 跳页 / 换书 → 整屏 GC16（T2）。
        //
        // 26.11.5 把两路并成一条整屏 GC16（刀 C），AI 页每翻一页闪一次——那是
        // 第二个闪源。判据用 `s_reader_body_turn`，由调用侧显式置位：这里没有
        // 几何签名可猜，翻页的脏区永远等于正文区。
        // 26.11.6: the reader's whole-page exit splits in two - page advances (AI detail
        // turns) use the body differential, while entering a book, chapter/page jumps
        // and book switches stay screen-wide GC16. 26.11.5 merged both into one full
        // refresh (cut C), so every AI page flashed; that was the second flash source.
        if (s_reader_cleanup) {
            s_turns = 0;                    // 不变式：整屏清底后计数归零
            err = update_display_full(ctx->hl);
        } else if (s_reader_body_turn) {
            // 26.11.9：AI 解析页翻页收编同款 ②——柔和波形 × 全覆盖，与正文一字不差。
            // 26.11.7 让 AI 页走 GC16 + 差分（裁剪表），那正是造带的组合：差分的单次
            // 「深→白」推进里，36 相裁剪表给不出足够的白。残影不看内容来源，波形也不该分。
            // Same as the body turn since 26.11.9: soft waveform, full-pixel GL16. The old
            // GC16 + diff path was the band-making combination, and ghosting does not care
            // where the ink came from.
            err = update_display_area_full_with(ctx->hl, display_reader_soft_turn_waveform(),
                                                MODE_GL16, reader_area());
        } else {
            s_turns = 0;                    // T2 同为整屏清底，不变式一致
            err = update_display_full(ctx->hl);
        }
    }
    else err = update_display_fast_page(ctx->hl);
    int64_t displayed = esp_timer_get_time();
    reader_prefetch_end(prep);
    if (redraw == APP_REDRAW_AREA && s_mode == MODE_DU &&
        !s_reader_cleanup && !image_gray_refresh && !s_shelf_feedback_pending) {
        s_du_area = s_du_count ? ui_rect_union(s_du_area, s_area) : s_area;
        ++s_du_count;
        s_du_ms = esp_timer_get_time() / 1000;
    } else s_du_count = 0;
    ESP_LOGI(TAG, "present draw=%lld display=%lld join=%lld ms", (drawn - start) / 1000,
             (displayed - drawn) / 1000, (esp_timer_get_time() - displayed) / 1000);
    guard_draw_result(ctx->hl, err);
    if ((s_view==SHELF || s_view==MANAGE) && err==EPD_DRAW_SUCCESS) {
        if ((redraw==APP_REDRAW_PAGE || redraw==APP_REDRAW_FULL) && shelf_cover_frame_ready()) s_cover_dirty_mask=0;
        else if (s_cover_batch_present_pending) s_cover_dirty_mask &= ~s_cover_present_mask;
    }
    s_cover_present_mask=0;
    if (err==EPD_DRAW_SUCCESS && reader_status_visible() &&
        (redraw==APP_REDRAW_PAGE || redraw==APP_REDRAW_FULL || s_reader_cleanup))
        s_reader_clock_minute=frame_minute;
    s_reader_text_frame = err == EPD_DRAW_SUCCESS && s_view == READING && s_text &&
        s_reader_panel == READER_PANEL_NONE && !s_toolbar && !s_clear_confirm &&
        book_layout_page_image_count(s_page) == 0 && !app_settings_book_background_path()[0];
    s_reader_cleanup = false;
    s_reader_image_refresh_pending = false;
    s_presented_view = (int)s_view;
    s_reader_footer_pending = false;
    s_water_turn_pending = false;
    s_shelf_feedback_pending = false;
    s_cover_batch_present_pending = false;
    s_reader_turn_pending = false;
    s_reader_body_turn = false;
    s_reader_page_advance = false;
    s_reader_clear_from_image = false;
    s_reader_frame_had_image = err == EPD_DRAW_SUCCESS && s_view == READING && s_text &&
        book_layout_page_image_count(s_page) > 0;
    s_mode = MODE_GL16;
    return true;
}
static app_redraw_t paint_reading(app_ctx_t* ctx, enum EpdDrawMode mode) {
    s_water_turn_pending = false;
    s_reader_turn_pending = false;
    int64_t started = esp_timer_get_time();
    (void)ui_reader_background_prepare(app_settings_book_background_path());
    prepare_inline_image();
    s_reader_image_refresh_pending = !app_settings_reader_hide_images() &&
        book_layout_page_image_count(s_page) > 0 && s_page_images;
    lock_draw();
    bool cached = !s_toolbar && !s_clear_confirm && s_next_fb &&
                  ((s_next_page == (int)s_page && s_next_generation == book_layout_generation()) ||
                   reader_prefetch_resume(s_page));
    if (cached) {
        memcpy(ctx->fb, s_next_fb, fb_bytes());
        draw_reader_outer(ctx->fb,s_page);
    }
    else draw_reader(ctx->fb, s_page);
    // 下一页可能预渲染于数分钟前，复制后以当前时间重绘状态栏。
    // A prefetched page can be minutes old; replace its status with the current time.
    if (cached && reader_status_visible()) reader_status_paint(ctx->fb);
    unlock_draw();
    ESP_LOGI(TAG, "paint cached=%d ms=%lld", cached, (esp_timer_get_time() - started) / 1000);
    s_area = reader_area();
    s_reader_footer_pending = !s_reader_fullscreen;
    s_mode = mode;
    return APP_REDRAW_AREA;
}

/* ---- 文件与进度 / Files and progress ---- */
typedef struct {
    size_t chapter;
    char** images;
    size_t count;
} reader_dims_ctx_t;
static reader_dims_ctx_t s_reader_dims_ctx;

// 按 PR9 通过完整资源解包测量，兼容 SVG 包装及较晚的 JPEG 尺寸头。/ Use PR9's full resource measurement for SVG wrappers and late JPEG headers.
static bool reader_image_dims(void* ctx, int image, int* width, int* height) {
    const reader_dims_ctx_t* c = (const reader_dims_ctx_t*)ctx;
    if (!c || image < 0 || (size_t)image >= c->count) return false;
    uint8_t* encoded = NULL;
    size_t size = 0;
    bool png = false;
    if (book_chapter_image(c->chapter, c->images[image], &encoded, &size, &png) != ESP_OK) return false;
    unsigned w = 0, h = 0;
    const bool ok = book_image_dimensions(encoded, size, png, &w, &h) && w && h;
    free(encoded);
    if (!ok) return false;
    if (width) *width = (int)w;
    if (height) *height = (int)h;
    return true;
}
static void flush_ticket_stats(void) {
    uint32_t seconds = s_stats_pending_ms / 1000;
    if (!seconds && !s_stats_pending_turns) return;
    if (book_ticket_record(seconds, s_stats_pending_turns) == ESP_OK) {
        s_stats_pending_ms -= seconds * 1000;
        s_stats_pending_turns = 0;
    }
}
static void track_ticket_stats(app_ctx_t* ctx) {
    if (s_view != READING || !s_text) { s_stats_last_ms = ctx->now_ms; return; }
    if (s_stats_last_ms > 0 && ctx->now_ms >= s_stats_last_ms &&
        ctx->now_ms - s_stats_last_ms <= 5000 &&
        ctx->now_ms - s_stats_activity_ms <= 300000 && !s_toolbar) {
        uint32_t elapsed = (uint32_t)(ctx->now_ms - s_stats_last_ms);
        s_stats_pending_ms += elapsed;
        s_session_read_ms += elapsed;
    }
    s_stats_last_ms = ctx->now_ms;
    if (s_stats_pending_ms >= 30000) flush_ticket_stats();
}
static void free_book(void) {
    // gen24 诊断：关书前后各记一笔内部堆快照（free/最大块）。与 `book: open start`
    // 那行同口径，开大书的瞬时峰值与关书残留直接对表——gen25 定位「长使用周期
    // 内部堆布局退化」的数据点。/ gen24 diagnostics: snapshot the internal heap
    // before and after closing a book (free/largest), same units as the
    // `book: open start` line, so a big open's transient peak and the post-close
    // residue line up directly - a data point for gen25's long-run internal-heap
    // layout investigation.
    ESP_LOGI(TAG, "close before internal free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    flush_ticket_stats();
    e0470_page_turn_release();
    s_water_turn_pending = false;
    s_reader_turn_pending = s_reader_text_frame = false;
    pending_progress_t* pending = pending_find(s_path);
    if (pending && !pending->dirty) pending_discard(s_path);
    invalidate_prep();
    book_layout_set_image_dims(NULL, NULL);
    s_reader_dims_ctx = (reader_dims_ctx_t){0};
    book_layout_free();
    book_layout_set_chapter_lead(0, 0);
    book_layout_set_chapter_lead_width(0);
    s_chapter_lead_skip = s_chapter_lead_height = 0;
    s_chapter_lead_width = 0;
    s_chapter_heading_title[0] = s_chapter_heading_label[0] = 0;
    free(s_text);
    free(s_blocks);
    free(s_runs);
    for (size_t i = 0; i < s_image_count; ++i) free(s_images[i]);
    free(s_images);
    s_images = NULL; s_image_count = 0;
    release_page_images();
    s_reader_image_refresh_pending = false;
    s_blocks = NULL;
    s_block_count = 0;
    s_runs = NULL;
    s_run_count = 0;
    book_layout_set_runs(NULL, 0);
    s_text = NULL;
    s_text_len = 0;
    // gen26.5 Step6：正文卸下 = 上下文源不再就绪（0x18 撤掉 CONTEXT 位，0x17 回 legacy）。
    // Chapter text released = the context source is gone (0x18 drops CONTEXT, 0x17 legacy).
    push_sink_ai_set_context_ready(false);
    // gen26.5 Step10：听书哨兵随书清（开关不停）。换书后同 (章,页) 不撞车，
    // 新书当前页保首推。/ Step10: the TTS sentinel clears with the book (the
    // switch stays on). A new book's same (chapter,page) never collides, so its
    // current page always gets a first push.
    s_tts_last_chapter = s_tts_last_page = SIZE_MAX;
    s_selected_toc = SIZE_MAX;
    s_jump_offset = SIZE_MAX;
    // gen25（池化 · 生命周期收口）+ gen26（降栈 12K→6K）：book_prep 的 6K 任务栈随书归还。
    // 数据层硬事实：内部最大块健康常驻 31744，开书后跌到 25600（gen25 在 12K 栈时
    // 跌到 19456）——差正好 6144 = 6×1024，即 kick_prep() 里 `xTaskCreatePinnedToCore
    // (prep_task, "book_prep", 6*1024, ...)` 建在内部 DRAM 的任务栈。任务栈是每帧热路径，
    // 按冷热纪律**不可搬 PSRAM**，所以解法是把它变成边界清晰的租户：关书即停即删，
    // 下次开书 kick_prep 会按 `if (!s_prep_task && ...)` 重建。gen25 时 19456 < 闸门 20K
    // 致推送预检被拒（「开大书后点推送被拒」）；gen26 降栈后开书 largest≈25600 > 20K
    // 闸门，预检直接过、让路鲜少触发（让路机制保留为保险，不删）。
    // Gen25 (pooling, lifetime closure) + gen26 (stack 12K->6K): book_prep's 6K
    // task stack is returned with the book. Data-layer fact: the internal largest
    // block sits at 31744 when healthy and drops to 25600 once a book is open
    // (at gen25's 12K stack it dropped to 19456) - the difference is exactly
    // 6144 = 6*1024, the stack kick_prep() creates in internal DRAM with
    // xTaskCreatePinnedToCore(prep_task, "book_prep", 6*1024, ...). A task stack
    // is per-frame hot, so the冷热 discipline forbids moving it to PSRAM; the fix
    // is to make it a well-bounded tenant: stop and delete it on close, and let
    // the next open rebuild it (kick_prep()'s `if (!s_prep_task && ...)` already
    // does). At gen25's 12K the 19456 < 20K gate blocked the push precheck (the
    // "push refused after opening a big book" report); after gen26's cut the
    // book-open largest is ~25600 > 20K, so the precheck passes and yield rarely
    // fires (the yield path stays as insurance, not removed).
    // 停机协议见 prep_task_request_stop()：**只请求，不强删**——任务自己在
    // 安全点（无锁、无临时缓冲）退出并回收栈，task self-delete 天然幂等，
    // 两条路径（关书 / 离页）先后调用也不会重复删。
    // The stop protocol lives in prep_task_request_stop(): **request, never
    // force-delete** - the task exits at its own safe point (no lock, no live
    // scratch) and reclaims the stack; self-deletion is idempotent by
    // construction, so the two paths (close / exit) calling it in sequence can
    // never double-delete.
    prep_task_request_stop("close");
    s_prep_page = -1;
    s_next_page = -1;
    s_next_generation = 0;
    s_prep_cursor.started = false;
    book_close();
    s_path[0] = 0;
    // 掉盘态随书一起结束：下一次开书走正常链路，不该继承上一条命的上一次事故。
    // The offline state ends with the book: the next open takes the normal path and must
    // not inherit the previous book's accident.
    s_media_offline = false;
    s_off_chapters = 0; s_off_total_bytes = 0; s_off_chapter_offset = 0;
    s_reader_favorite = false;
    s_book_title[0] = 0;
    app_font_activate_system();
    ESP_LOGI(TAG, "close after internal free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}
// 通栏之后一页位图更大：装不下就按最大空闲块等比缩一档——图小一圈也胜过整页消失。
// A full-bleed page bitmap is larger: when it does not fit, step the aspect-preserving size down
// into the largest free block. A slightly smaller illustration still beats a missing one.
static uint8_t *alloc_page_bitmap(int *width, int *height, size_t *pixels) {
    uint8_t *gray = heap_caps_malloc(*pixels, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (gray) return gray;
    size_t room = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (room == 0) return NULL;
    for (int guard = 0; guard < 24 && *pixels > room; ++guard) {
        *width = *width > 1 ? *width * 7 / 8 : 1;
        *height = *height > 1 ? *height * 7 / 8 : 1;
        *pixels = (size_t)*width * (size_t)*height;
    }
    if (*pixels > room) return NULL;
    gray = heap_caps_malloc(*pixels, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (gray)
        ESP_LOGW(TAG, "inline illustration shrunk to %dx%d, free block %u KB",
                 *width, *height, (unsigned)(room / 1024));
    return gray;
}
static void release_page_images(void) {
    for (int i = 0; i < s_page_image_count; ++i) free(s_page_images[i].gray);
    free(s_page_images); s_page_images = NULL; s_page_image_count = 0;
    s_page_images_for = SIZE_MAX;
}
static void prepare_inline_image(void) {
    // B′ 条件 4：掉盘时当前页位图已经解码在 PSRAM，**绝不再读卡**——这里的每一次
    // 读取都会落到一个已卸载的卷上。缓存保持原样（重排后由 reader_offline_rebind_images
    // 把页号与布局代次重新绑上），翻到别的页则自然不画（槽位不匹配），页脚有「卡已移除」。
    // Offline, the current page's bitmaps are already decoded in PSRAM: never touch the card
    // again here. The cache is kept as-is and re-bound after the reflow; other pages simply
    // paint no illustration, with the footer already saying why.
    if (s_media_offline) return;
    if (app_settings_reader_hide_images()) { release_page_images(); return; }
    uint32_t generation = book_layout_generation();
    if (s_page_images_for == s_page && s_page_images_generation == generation) return;
    // 纯文字跨页不应作废已排好的下一页；布局代次改变或任一侧有插图时才失效。
    // Preserve the prefetched next text page; invalidate only across layout generations or image-bearing pages.
    const bool changed_layout_or_image = s_page_images_generation != generation || s_page_image_count > 0;
    release_page_images();
    s_page_images_for = s_page; s_page_images_generation = generation;
    // 插图不显示没有别的症状，每条失败路径以前都是静默 continue；这里把结局记下来，
    // 免得继续靠猜。
    // A missing illustration has no other symptom and every failure path used to be a silent
    // continue; record the outcome instead of guessing.
    int count = reader_image_slots(s_page);
    if (changed_layout_or_image || count > 0) invalidate_prep();
    size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!count || count > (int)HTML_TEXT_MAX_BLOCKS) {
        ESP_LOGI(TAG, "inline page %u: %d image slots", (unsigned)s_page, count);
        return;
    }
    s_page_images = heap_caps_calloc((size_t)count, sizeof(*s_page_images), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_page_images) {
        // 分配失败时计数必须跟着归零：release_page_images 会按计数遍历这张表。
        // The count has to follow the table to zero: release_page_images walks it by count.
        s_page_image_count = 0;
        ESP_LOGW(TAG, "inline page %u: no PSRAM for %d slots", (unsigned)s_page, count);
        return;
    }
    s_page_image_count = count;
    // 逐张解码并立即释放压缩数据；全页灰阶位图的总面积不超过插图区域。
    // Decode serially and release each encoded buffer; the page's bitmap area stays within its
    // illustration region.
    EpdRect band = book_layout_image_rect();
    size_t pixels_left = (size_t)band.width * band.height;
    int decoded = 0;
    for (int i = 0; i < count; ++i) {
        int index = -1, width = 0, height = 0;
        if (!reader_image_slot(s_page, i, &index, NULL, &width, &height) ||
            index < 0 || (size_t)index >= s_image_count || width <= 0 || height <= 0) {
            ESP_LOGW(TAG, "inline %d: slot i=%d width=%d height=%d", i, index, width, height);
            continue;
        }
        uint8_t *encoded = NULL; size_t size = 0; bool png = false;
        esp_err_t err = book_chapter_image(s_chapter, s_images[index], &encoded, &size, &png);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "inline %d %s: read %s", i, s_images[index], esp_err_to_name(err));
            free(encoded);
            continue;
        }
        unsigned source_w = 0, source_h = 0;
        if (book_image_dimensions(encoded, size, png, &source_w, &source_h) && source_w && source_h) {
            // 回退槽也使用真实宽高等比缩小，避免尺寸探测失败时拉伸图片。
            // Aspect-fit even fallback slots from decoded dimensions, avoiding stretched unknown-size images.
            uint64_t w = source_w, h = source_h;
            if (w > (unsigned)width) { h = h * width / w; w = width; }
            if (h > (unsigned)height) { w = w * height / h; h = height; }
            width = w ? (int)w : 1; height = h ? (int)h : 1;
            size_t pixels = (size_t)width * height;
            if (pixels > pixels_left) {
                ESP_LOGW(TAG, "inline %d: %ux%u over %u px budget",
                         i, (unsigned)width, (unsigned)height, (unsigned)pixels_left);
                free(encoded);
                continue;
            }
            uint8_t *gray = alloc_page_bitmap(&width, &height, &pixels);
            if (gray == NULL) {
                ESP_LOGW(TAG, "inline %d: no PSRAM for %u px (free %u)",
                         i, (unsigned)pixels,
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                free(encoded);
                continue;
            }
            if (book_image_grayscale(encoded, size, png, width, height, gray,
                                     2u * 1024u * 1024u)) {
                s_page_images[i] = (reader_image_t){gray, width, height};
                pixels_left -= pixels;
                ++decoded;
            } else {
                // 现场三件套（gen26 真机观察项「JPEG 单图降级」销项）：图片名（URL）、
                // 字节数、格式；SOF 类型与走的库由 book_cover.c 内的 jpeg_gray/png_gray_input
                // 各自打（那里才拿得到解码器内部状态）。
                // The three-part scene (closing the gen26 device observation "a lone
                // JPEG degrades"): image name (URL), byte count, format; the SOF type
                // and the library that ran come from jpeg_gray/png_gray_input inside
                // book_cover.c, the only place that can see decoder-internal state.
                ESP_LOGW(TAG, "inline %d %s: grayscale decode failed (%u bytes, png=%d)",
                         i, s_images[index], (unsigned)size, (int)png);
                free(gray);
            }
        } else {
            ESP_LOGW(TAG, "inline %d: unknown dimensions (%u bytes, png=%d)", i, (unsigned)size, (int)png);
        }
        free(encoded);
        vTaskDelay(1);
    }
    // 只在有图没画出来时汇总：插图缺失没有别的症状，而每次都打会把日志淹掉。
    // Summary only when something did not make it: a missing illustration has no other symptom,
    // and logging every page would drown the console.
    if (decoded < count) {
        ESP_LOGW(TAG, "inline page %u: %d/%d decoded, psram %u->%u KB", (unsigned)s_page, decoded, count,
                 (unsigned)(psram_before / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) / 1024));
    }
}
// 标题匹配要宽松：书内导航与正文的写法常常不同（"第一章 出发" 对 "出发"、"1. Prologue" 对
// "序章"，竖排书尤其如此）。逐字比较失败时正文标题块不会被跳过，题头与正文就各显示一次。
// Heading matching has to be forgiving: an authored navigation title rarely matches the body text
// verbatim ("Chapter 1 Departure" against "Departure", a numbered English title against its
// Japanese one), and a strict compare leaves the body heading on the page, so it shows up twice.
static bool heading_text_matches(const char *text, const char *toc, const char *heading,
                                 const char *label) {
    if (text == NULL || text[0] == 0) return false;
    const char *candidates[3] = {toc, heading, label};
    for (int i = 0; i < 3; ++i) {
        const char *cand = candidates[i];
        if (cand == NULL || cand[0] == 0) continue;
        // 互相包含即视为同一标题；短于三个字节的候选不作为依据，免得误吞正文。
        // Either containing the other counts as the same heading; candidates shorter than three
        // bytes are ignored so ordinary body text cannot be swallowed.
        if (strlen(cand) < 3) continue;
        if (strstr(text, cand) != NULL || strstr(cand, text) != NULL) return true;
    }
    return false;
}

static bool load_chapter_at(app_ctx_t* ctx, size_t chapter, size_t offset,
                            bool last_page, const char *anchor, size_t source_offset) {
    // ---- 掉盘闸（B′ 条件 3）------------------------------------------------
    // 单一闸：**所有**跨章/重排路径都过这里（TOC、书签、跳转、字号重排、章末翻页），
    // 所以只在这一处拒绝，就不会有第二条读卡的路漏过去。
    // 拒绝要说出来：「静默失败」在墨水屏上是「按了没反应」，用户会以为机器坏了。
    // One gate: every chapter/reflow path goes through here, so refusing at this single
    // point cannot leave a second route that still reads the card. And it refuses out
    // loud: on e-paper a silent failure reads as "the button is broken".
    if (s_media_offline) { reader_offline_refuse(); return false; }
    bool clear_toc = chapter != s_chapter || (!anchor && source_offset == SIZE_MAX);
    html_text_t loaded = {0};
    size_t anchor_offset = 0;
    esp_err_t err = book_chapter_load_blocks_target(chapter, anchor, source_offset,
                                                    &anchor_offset, &loaded);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "chapter load failed: %s", esp_err_to_name(err));
        copy_text(s_message, sizeof(s_message), "加载失败，请检查存储卡后重试");
        return false;
    }
    char heading[128] = {0}, label[32] = {0}, toc_title[128] = {0};
    size_t lead_skip = 0;
    unsigned lead_height = 0;
    if (book_kind() == BOOK_KIND_EPUB && loaded.len) {
        // 已识别的正文编号题头优先；没有时沿用书内导航或备用标题。
        // Prefer a recognized numbered body heading, then authored navigation or a fallback title.
        if (book_chapter_title(chapter, toc_title, sizeof(toc_title)) != ESP_OK)
            snprintf(toc_title, sizeof(toc_title), "第 %u 节", (unsigned)chapter + 1);
        copy_text(heading, sizeof(heading), toc_title);
        char *chapter_mark = strstr(heading, "章");
        if (chapter_mark && chapter_mark - heading < 24) {
            char *after = chapter_mark + strlen("章");
            while (*after == ' ' || *after == '\t' || *after == ':' || *after == '-' ||
                   !strncmp(after, "：", strlen("：")) || !strncmp(after, "、", strlen("、"))) {
                if (*after == ' ' || *after == '\t' || *after == ':' || *after == '-') ++after;
                else after += strlen("：");
            }
            if (*after && (size_t)(chapter_mark + strlen("章") - heading) < sizeof(label)) {
                size_t label_len = (size_t)(chapter_mark + strlen("章") - heading);
                memcpy(label, heading, label_len); label[label_len] = 0;
                memmove(heading, after, strlen(after) + 1);
            }
        }
        size_t skip_blocks = 0;
        // 最多看四个块：标题可能被拆成"第一章"、"出发"、"——副标题"这样几块。
        // Up to four blocks: a heading can arrive split across several.
        while (skip_blocks < loaded.count && skip_blocks < 4 &&
               loaded.blocks[skip_blocks].heading && loaded.blocks[skip_blocks].image < 0) {
            char body_heading[128] = {0};
            size_t len = loaded.blocks[skip_blocks].len;
            if (len >= sizeof(body_heading)) break;
            memcpy(body_heading, loaded.utf8 + loaded.blocks[skip_blocks].offset, len);
            while (len && (body_heading[len - 1] == ' ' || body_heading[len - 1] == '\t')) body_heading[--len] = 0;
            const char *trimmed = body_heading;
            while (*trimmed == ' ' || *trimmed == '\t') ++trimmed;
            if (!heading_text_matches(trimmed, toc_title, heading, label)) break;
            ++skip_blocks;
        }
        if (skip_blocks) lead_skip = skip_blocks < loaded.count ? loaded.blocks[skip_blocks].offset : loaded.len;
        // 首块是插图时不预留题头：那页整页留给插图，标题在它后面才排。
        // 这个条件一直是题头该不该出现的判据，横排竖排都一样。
        // An illustration at the head of the chapter reserves nothing: it owns that page and the
        // heading follows. This condition has always decided whether a sheet gets a heading at all,
        // in either setting.
        if (skip_blocks < loaded.count && loaded.blocks[skip_blocks].image >= 0) {
            lead_skip = 0;
            lead_height = 0;
        } else {
            lead_height = body_rect().height > 214 ? 214 : 0;
        }
    }
    size_t old_lead_skip = s_chapter_lead_skip;
    unsigned old_lead_height = s_chapter_lead_height;
    unsigned old_lead_width = s_chapter_lead_width;
    reader_dims_ctx_t old_dims_ctx = s_reader_dims_ctx;
    lock_draw();
    invalidate_prep();
    // 题头几何按当前模式重算，横排留顶部行高、竖排留右侧列宽，绘制与预留不会错开。
    // Recompute the heading geometry for the current mode -- rows at the top horizontally,
    // columns at the right vertically -- so drawing and reservation cannot drift apart.
    apply_chapter_lead(heading, label, lead_skip, lead_height);
    s_reader_dims_ctx = (reader_dims_ctx_t){chapter, loaded.images, loaded.image_count};
    book_layout_set_image_dims(reader_image_dims, &s_reader_dims_ctx);
    book_layout_set_runs(loaded.runs, loaded.run_count);
    bool ok = reader_relayout_cols(loaded.utf8, loaded.len, loaded.blocks, loaded.count,
                                   body_rect(), s_px, reader_column_count());
    if (!ok) {
        // 回滚前恢复旧章上下文，避免尺寸回调引用已释放的新章图片。/ Restore the previous context before rollback to avoid a freed image list.
        s_reader_dims_ctx = old_dims_ctx;
        html_text_free(&loaded);
        // run 表随 loaded 一起没了，先把排版还回旧章的那份，否则重排会读到已释放的内存。
        // The run table dies with `loaded`, so put the previous chapter's back before the
        // rollback layout reads freed memory.
        book_layout_set_runs(s_runs, s_run_count);
        // 题头几何也跟着回滚，否则这一章的题头参数会留在失败的那次上。
        // Roll the heading geometry back too, or the failed chapter's parameters would stick.
        s_chapter_lead_skip = old_lead_skip;
        s_chapter_lead_height = old_lead_height;
        s_chapter_lead_width = old_lead_width;
        book_layout_set_chapter_lead(old_lead_skip, old_lead_height);
        book_layout_set_chapter_lead_width(old_lead_width);
        bool restored = s_text && reader_relayout(body_rect());
        unlock_draw();
        if (!restored) {
            free_book();
            s_view = SHELF;
            ctx->leaf = 0;
        }
        copy_text(s_message, sizeof(s_message), "排版失败：内存不足或章节过长");
        return false;
    }
    book_excerpt_offer(s_path, &loaded);
    free(s_text);
    free(s_blocks);
    free(s_runs);
    s_text = loaded.utf8;
    s_text_len = loaded.len;
    // gen26.5 Step6：正文在手 = 上下文源就绪（书锚点 + 位置都可采），此后 0x18 才
    // 上报 CAP_AI_CONTEXT（不虚报能力）。/ Chapter text in hand = the context source
    // is ready, and only then does 0x18 advertise CAP_AI_CONTEXT (no false claims).
    push_sink_ai_set_context_ready(s_text != NULL);
    s_blocks = loaded.blocks;
    s_block_count = loaded.count;
    s_runs = loaded.runs;
    s_run_count = loaded.run_count;
    for (size_t i = 0; i < s_image_count; ++i) free(s_images[i]);
    free(s_images);
    s_images = loaded.images; s_image_count = loaded.image_count;
    release_page_images();
    s_chapter = chapter;
    if (clear_toc) s_selected_toc = SIZE_MAX;
    s_chapter_lead_skip = lead_skip;
    s_chapter_lead_row_height = lead_height;
    copy_text(s_chapter_heading_title, sizeof(s_chapter_heading_title), heading);
    copy_text(s_chapter_heading_label, sizeof(s_chapter_heading_label), label);
    // 末页与续读落点都经地图钳到视觉页起始逻辑页；单栏下与旧值逐位一致。
    // Both the last page and resume landings clamp onto a visual page's starting logical
    // page through the map; single column matches the old values exactly.
    s_page = last_page ? reader_visual_start(reader_visual_count() - 1) :
             reader_visual_start(reader_visual_for_offset(anchor || source_offset != SIZE_MAX ? anchor_offset : offset));
    s_jump_offset = anchor || source_offset != SIZE_MAX ? anchor_offset : SIZE_MAX;
    s_jump_page = s_page;
    if (book_chapter_title(chapter, s_title, sizeof(s_title)) != ESP_OK)
        snprintf(s_title, sizeof(s_title), "第 %u 节", (unsigned)chapter + 1);
    copy_text(s_font_path, sizeof(s_font_path), ttf_font_path());
    s_message[0] = 0;
    unlock_draw();
    prepare_inline_image();
    // 开书这条链上有解析、解压和字体装载，栈曾经在这里溢出过；留一个可观测的水位。
    // 顺带量一下章节排完后的 PSRAM 余量：书内字体的装载预算要靠这个数来定，不能靠估。
    // Parsing, inflate and face loading all ride this chain, and it overflowed once before;
    // keep the headroom observable. The PSRAM left after a chapter is laid out is also the
    // number the face budget should be derived from rather than guessed.
    ESP_LOGI(TAG, "chapter %u stack free %u psram free %u largest %u", (unsigned)chapter,
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return true;
}
static bool load_chapter(app_ctx_t* ctx, size_t chapter, size_t offset, bool last_page) {
    return load_chapter_at(ctx, chapter, offset, last_page, NULL, SIZE_MAX);
}
// 关闭图片时跳过纯插图页，避免出现空白翻页。/ Skip illustration-only pages when images are disabled.
// 隐藏图页在地图里有正式名分（JOINT 类）：跳转走地图标记定位下一个可显示视觉页，
// 不做「逻辑页 ±1 直移再换算」的独立算术——两条路各算各的迟早分叉（连跳多个隐藏
// 图页的场景）。s_page 恒为视觉页起始逻辑页，故 JOINT 判定看落点即看视觉页类。
// Hidden image pages have a formal class in the map (JOINT): the skip walks the map's
// marks to the next displayable visual page instead of moving a logical page by one and
// converting afterwards — two separate computations eventually diverge (runs of hidden
// pages). s_page is always a visual page's starting logical page, so testing the landing
// for JOINT tests the visual page's class.
static void skip_hidden_image_pages(app_ctx_t *ctx, int direction) {
    if (!app_settings_reader_hide_images()) return;
    for (int guard = 0; guard < 128 && reader_page_is_joint(s_page); ++guard) {
        size_t visual = reader_visual_index(s_page);
        if (direction > 0) {
            if (visual + 1 < reader_visual_count()) s_page = reader_visual_start(visual + 1);
            else if (s_chapter + 1 < book_chapter_count()) {
                if (!load_chapter(ctx, s_chapter + 1, 0, false)) break;
            } else break;
        } else {
            if (visual > 0) s_page = reader_visual_start(visual - 1);
            else if (s_chapter > 0) {
                if (!load_chapter(ctx, s_chapter - 1, 0, true)) break;
            } else break;
        }
    }
}
static bool open_book_impl(app_ctx_t* ctx, const char* path) {
    s_reader_return_home = false;
    if (!strncmp(path, "/sdcard/", 8)) {
        read_pico_sd_info_t sd = {0};
        read_pico_sd_get_info(&sd);
        if (!sd.present || !sd.mounted) {
            copy_text(s_message, sizeof(s_message), "TF 卡不可用，请重新挂载后打开");
            return false;
        }
    }
    // 自动续读也不能绕过同路径旧文件的清理重试。
    // Automatic resume must also finish cleanup of the old file at this path.
    if (delete_retry_find(path)) {
        copy_text(s_message, sizeof(s_message), "文件已删除，进度清理失败，请重试");
        return false;
    }
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size > UINT32_MAX) {
        copy_text(s_message, sizeof(s_message), "文件不可读，请检查 TF 卡和书籍文件");
        return false;
    }
    if (strncmp(path, "/flash/", 7) == 0 && st.st_size > BOOK_STORE_FLASH_FILE_MAX) {
        copy_text(s_message, sizeof(s_message), "内置存储单本限 1 MB");
        return false;
    }
    read_pico_transfer_status_t network = {0};
    read_pico_transfer_get_status(&network);
    // 书架封面和上一本书的预渲染缓冲可以重建；先释放，再解析 EPUB。
    // Cover and previous-page caches are rebuildable; release them before EPUB parsing.
    invalidate_covers();
    app_home_release_clock_cache();
    lock_draw();
    invalidate_prep();
    free(s_next_fb);
    s_next_fb = NULL;
    unlock_draw();
    if (network.network_ready) ttf_font_cache_clear();
    // 开书前把字形缓存全还回去并量一下能腾多少：书内字体要的正是这块 PSRAM，而系统字体
    // 的缓存是里面最容易回收的。字体本身保持装载，翻页时按需重新缓存。
    // Hand every glyph cache back before opening and measure what that was worth: embedded
    // faces need exactly this PSRAM, and the system face's cache is the easiest part to
    // reclaim. The faces stay loaded and re-cache on demand.
    size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t reclaimed = ttf_font_cache_clear_all();
    size_t psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    // 26.9.15 E3：加 blocks=free/alloc 两个字段。开书会让内部最大块掉 1,024 B
    // （21,504 → 20,480），但「掉的是新分配」还是「又切了一刀」在 free/largest 上看不出来：
    // 前者表现为 alloc 块数 +1，后者表现为 free 块数 −1。两个数字并列就能当场分辨。
    // Add blocks=free/alloc. Opening a book costs 1,024 B of the largest internal block
    // (21,504 -> 20,480), and free/largest alone cannot say whether that is a NEW allocation
    // (alloc count +1) or another cut (free count -1). Side by side, they answer it on sight.
    multi_heap_info_t bsi;
    memset(&bsi, 0, sizeof(bsi));
    heap_caps_get_info(&bsi, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "open start wifi=%d internal=%u/%u blocks=%u/%u psram=%u/%u cache=%uK freed=%uK",
             network.network_ready,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)bsi.free_blocks, (unsigned)bsi.allocated_blocks,
             (unsigned)psram_after,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)(reclaimed / 1024),
             (unsigned)((psram_after - psram_before) / 1024));
    // 26.9.20 S10：每开一次书 largest 永久少 1,024 B，位置已缩到「open start 与
    // close before 之间那 1 ms」。五个路标按执行顺序摆开，相邻两行的 `delta=` 直接
    // 点名——本版**只加打点、不动手**（审计 26.9.18 的「先归因后动刀」仍然适用）。
    // 26.9.20 S10: opening a book permanently costs 1,024 B of largest, narrowed to the
    // 1 ms between `open start` and `close before`. Five landmarks in execution order;
    // neighbouring `delta=` values name the culprit. Instrumentation only, no fix yet.
    pico_heap_mark("reserve1");
    if (!pending_reserve(path)) {
        copy_text(s_message, sizeof(s_message), "内存不足，无法保留待保存进度");
        return false;
    }
    save_progress();
    pico_heap_mark("save");
    free_book();
    pico_heap_mark("free");
    s_reader_fullscreen = s_requested_fullscreen;
    app_font_activate_reading();
    pico_heap_mark("font");
    if (!pending_reserve(path)) {
        copy_text(s_message, sizeof(s_message), "内存不足，无法打开图书");
        app_font_activate_system();
        return false;
    }
    pico_heap_mark("reserve2");
    esp_err_t err = book_open(path);
    // 书源打开后再把书内字体开关接上：EPUB 才有效，TXT 是空操作。
    // Wire the embedded-face switch only after the source is open: it only applies to EPUB,
    // and is a no-op for TXT.
    if (err == ESP_OK) {
        s_book_fonts_applied = app_settings_book_fonts();
        book_set_embedded_fonts(s_book_fonts_applied);
    }
    if (err != ESP_OK) {
        pending_progress_t* pending = pending_find(path);
        if (pending && !pending->dirty) pending_discard(path);
        copy_text(s_message, sizeof(s_message),
                  err == ESP_ERR_NOT_SUPPORTED ? "文件格式或压缩方式暂不支持" :
                  err == ESP_ERR_NO_MEM ? "内存不足，请重启后重试" :
                  err == ESP_ERR_INVALID_SIZE ? "图书资源过大或章节过多" :
                  "无法打开图书，请检查文件");
        ESP_LOGW(TAG, "open failed: %s internal=%u/%u psram=%u/%u", esp_err_to_name(err),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        app_font_activate_system();
        return false;
    }
    copy_text(s_path, sizeof(s_path), path);
    // gen21（裁决 A「打开即已读」）：push.txt 实际打开成功即清未读位。此处是
    // 全部开书路径（首页大卡/传书自动开/书架点开）的唯一汇合点——传书页推完
    // 自动开书随即清位（不亮假牌）；用户在别处收推则无人清位，回首页徽章亮。
    // 上行 EVT_READ（notify_read）保持在 book_on_exit 原位，手机侧语义不变。
    // gen21 (ruling A "open means read"): opening push.txt clears the unread
    // flag. open_book_impl is the single funnel for every open path (home card,
    // transfer auto-open, shelf tap) - the auto-open chain clears right away (no
    // false badge), while a push landing elsewhere stays flagged until the home
    // badge lights. The outbound EVT_READ stays in book_on_exit untouched.
    if (strcmp(s_path, PUSH_TEXT_PATH) == 0) push_sink_clear_unread();
    s_reader_favorite = favorite_load(path);
    const char* filename = strrchr(path, '/');
    filename = filename ? filename + 1 : path;
    if (!book_title_from_path(path, s_book_title, sizeof(s_book_title)))
        clean_filename(s_book_title, sizeof(s_book_title), filename);
    s_file_size = (uint32_t)st.st_size;
    book_progress_t p = {0};
    bool resume = pending_restore(path, s_file_size, &p) || book_progress_load(path, s_file_size, &p);
    // 排版字号以全局阅读设置为准；旧书进度只决定阅读位置。
    // Use the shared reading size for every book; per-book progress restores position only.
    s_px = app_settings_book_px();
    if (s_px < BOOK_PX_MIN || s_px > BOOK_PX_MAX) s_px = 48;
    size_t chapter = resume && p.chapter < book_chapter_count() ? p.chapter : 0;
    if (!load_chapter(ctx, chapter, resume && p.chapter == chapter ? p.byte_off : 0, false)) {
        free_book();
        return false;
    }
    skip_hidden_image_pages(ctx, 1);
    if (!s_text) { free_book(); return false; }
    s_view = READING;
    s_reader_image_refresh_pending = !app_settings_reader_hide_images() &&
        book_layout_page_image_count(s_page) > 0 && s_page_images;
    s_stats_last_ms = s_stats_activity_ms = ctx->now_ms;
    s_reader_panel = READER_PANEL_NONE;
    s_clear_confirm = s_batch_confirm = false;
    s_session_read_ms = s_session_turns = 0;
    s_reader_notice[0] = 0;
    s_turns = s_unsaved = 0;
    pending_mark_latest(s_path);
    save_progress();
    // 打开即重新上书架：移出书架只隐藏条目，重新读这本书就是它该在架上的信号。
    // Opening puts the book back on the shelf: removal only hides the entry, and reading it
    // again is the signal that it belongs there.
    if (shelf_hidden_load(s_path)) (void)shelf_hidden_save(s_path, false);
    ESP_LOGI(TAG, "opened kind=%d chapters=%u pages=%u px=%d", book_kind(), (unsigned)book_chapter_count(), (unsigned)reader_visual_count(), s_px);
    return true;
}
static bool open_book(app_ctx_t *ctx, const char *path) {
    if (!book_cover_jobs_quiesce(2000)) {
        copy_text(s_message,sizeof(s_message),"封面任务正在收尾，请稍后再试");return false;
    }
    if (pico_boot_book_begin(path) != ESP_OK) {
        copy_text(s_message, sizeof(s_message), "启动记录保存失败，请重试");
        return false;
    }
    bool ok = open_book_impl(ctx, path);
    pico_boot_book_end(ok);
    return ok;
}
static bool open_requested_book(app_ctx_t* ctx) {
    if (!s_requested_open[0]) return false;
    char path[BOOK_STORE_PATH_MAX];
    copy_text(path, sizeof(path), s_requested_open);
    bool from_home = s_requested_open_home;
    s_requested_open[0] = 0;
    s_requested_open_home = false;
    bool opened = open_book(ctx, path);
    s_requested_fullscreen = true;
    if (!opened) return false;
    s_reader_return_home = from_home;
    return true;
}

/* ---- 生词本虚拟书 / The vocabulary-book virtual book ----
 * 把 vocab.txt 渲染成一本排版好的 TXT，交给现成阅读器打开：排版、翻页、长按查词、
 * 释义卡和保存 chip 全都是复用，不用新写任何列表 UI。
 * Render vocab.txt into a laid-out TXT and open it with the existing reader:
 * layout, page turns, long-press lookup, the definition card and the save chip are
 * all reused — no new list UI.
 *
 * 走 open_book_impl() 而不是 open_book()：后者会 pico_boot_book_begin() 把生词本
 * 写进启动恢复记录，那样下次开机/唤醒会直接落到生词本，顶掉真正在读的那本书。
 * Use open_book_impl() rather than open_book(): the latter records the boot book,
 * which would resume into the vocabulary view on the next boot and displace the
 * book actually being read.
 */
static app_redraw_t open_vocab_view(app_ctx_t* ctx, bool restore_position) {
    size_t chapter = s_chapter, page = s_page;
    size_t count = 0;
    // 有词条才值得打开；空的就给一句提示，留在原处。提示走阅读页的 notice
    // （s_message 只显示在书架/文件页上，阅读页里看不见）。
    // Only open it when there is something to read; otherwise explain and stay put.
    // The hint uses the reader notice: s_message only surfaces on the shelf/files pages.
    const char* notice = !vocab_log_render(&count) ? "生词本视图生成失败"
                       : count == 0 ? "生词本还是空的：长按英文单词后点「保存」"
                       : NULL;
    if (notice) {
        copy_text(s_reader_notice, sizeof(s_reader_notice), notice);
        s_reader_notice_until = esp_timer_get_time() / 1000 + 2200;
        invalidate_prep();
        return APP_REDRAW_PAGE;
    }
    dict_popup_reset();
    s_reader_panel = READER_PANEL_NONE;
    if (!open_book_impl(ctx, vocab_log_view_path())) {
        invalidate_prep();
        return APP_REDRAW_PAGE;
    }
    // 路径推出来的书名是 "vocab_view"，换成中文。
    // The path-derived title is "vocab_view"; use a readable one.
    copy_text(s_book_title, sizeof(s_book_title), "我的生词本");
    if (restore_position && (chapter || page)) {
        // 删掉一条后文件整体前移，字节偏移已经不可信：先把排版建出来，
        // 再按**页码**跳回去，视觉上还停在原来那一页。
        // After a removal the byte offsets shift; rebuild the layout first and jump
        // by page index so the reader stays on the same page visually.
        if (chapter >= book_chapter_count()) chapter = book_chapter_count() - 1;
        (void)load_chapter(ctx, chapter, 0, false);
        size_t pages = book_layout_page_count();
        size_t target = page < pages ? page : (pages ? pages - 1 : 0);
        (void)load_chapter(ctx, chapter, book_layout_page_start_offset(target), false);
    }
    invalidate_prep();
    return APP_REDRAW_PAGE;
}

/* ---- 摘抄本虚拟书 / The excerpt-book virtual book (26.11.2 P0-5) ----
 * excerpts.txt → 渲染（排序倒序 / 按月分章 / 坏行计数 / 待落盘节置顶）→ 复用阅读器。
 * 与生词本同一套做法：open_book_impl() 而不是 open_book()——不写启动恢复记录，
 * 下次开机不落到摘抄本。删除/编辑是 v2（成品路径四拍 + 半行判据交叉，单独立项）。
 * Render excerpts.txt and open it with the existing reader, exactly like the
 * vocabulary book; open_book_impl() keeps the view out of the boot-resume record.
 */
static app_redraw_t open_excerpt_view(app_ctx_t* ctx, bool restore_position) {
    (void)restore_position; // v1 不做删除/编辑，文件不会前移，无需按页跳回 / v1 has no removal
    size_t count = 0;
    const char* notice = !excerpt_store_render_view(&count) ? "摘抄本视图生成失败"
                       : count == 0 ? "还没有摘抄：读书时框选文字，点「存摘抄」"
                       : NULL;
    if (notice) {
        copy_text(s_reader_notice, sizeof(s_reader_notice), notice);
        s_reader_notice_until = esp_timer_get_time() / 1000 + 2200;
        invalidate_prep();
        return APP_REDRAW_PAGE;
    }
    dict_popup_reset();
    s_reader_panel = READER_PANEL_NONE;
    if (!open_book_impl(ctx, excerpt_store_view_path())) {
        invalidate_prep();
        return APP_REDRAW_PAGE;
    }
    // 路径推出来的书名是 "excerpt_view"，换成中文。
    // The path-derived title is "excerpt_view"; use a readable one.
    copy_text(s_book_title, sizeof(s_book_title), "我的摘抄本");
    invalidate_prep();
    return APP_REDRAW_PAGE;
}
/* ---- 书架数据 / Shelf data ---- */
static int shelf_category_add(const char *path) {
    for (int i=2;i<s_category_count;++i) {
        if (!strcmp(s_categories[i].path,path)) { s_categories[i].available=true; return i; }
    }
    if (s_category_count==SHELF_CATEGORY_LIMIT) return -1;
    if (s_category_count==s_category_capacity) {
        int capacity=s_category_capacity ? s_category_capacity*2 : 16;
        if (capacity>SHELF_CATEGORY_LIMIT) capacity=SHELF_CATEGORY_LIMIT;
        shelf_category_t *next=heap_caps_realloc(s_categories,(size_t)capacity*sizeof(*next),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if (!next) return -1;
        s_categories=next; s_category_capacity=capacity;
    }
    int index=s_category_count++;
    memset(&s_categories[index],0,sizeof(s_categories[index]));
    copy_text(s_categories[index].path,sizeof(s_categories[index].path),path);
    s_categories[index].available=true;
    return index;
}
static void shelf_category_init(void) {
    const char *root=app_settings_books_dir();
    if (strcmp(s_category_root,root)) {
        free(s_categories); s_categories=NULL;
        s_category_count=s_category_capacity=s_category_selected=s_category_menu_page=0;
        copy_text(s_category_root,sizeof(s_category_root),root);
        s_shelf_cache_valid=false;
    }
    if (s_categories) return;
    s_categories=heap_caps_calloc(16,sizeof(*s_categories),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!s_categories) return;
    s_category_capacity=16; s_category_count=2;
    s_categories[0].available=s_categories[1].available=true;
}
static const char *shelf_category_name(int index) {
    if (index==1) return "未分类";
    if (!s_categories || index<2 || index>=s_category_count) return "全部书籍";
    const char *slash=strrchr(s_categories[index].path,'/');
    return slash ? slash+1 : s_categories[index].path;
}
static bool shelf_category_matches(const char *path) {
    if (!s_category_selected || !s_categories) return true;
    if (s_category_selected==1) {
        char child[BOOK_STORE_PATH_MAX];
        return !book_shelf_child(s_category_root,path,child,sizeof(child));
    }
    size_t size=strlen(s_categories[s_category_selected].path);
    return !strncmp(path,s_categories[s_category_selected].path,size) && path[size]=='/';
}
static void shelf_category_remember(app_ctx_t *ctx) {
    if (s_categories && s_view==SHELF && !s_scan_pending)
        s_categories[s_category_selected].pages[app_settings_shelf_style()==5]=ctx->leaf;
}
static void shelf_category_restore(app_ctx_t *ctx) {
    int layout=app_settings_shelf_style()==5;
    int saved=s_categories ? s_categories[s_category_selected].pages[layout] : 0;
    ctx->leaf=book_shelf_saved_page(saved,s_visible_count,shelf_rows());
    if (s_categories) s_categories[s_category_selected].pages[layout]=ctx->leaf;
}
static void shelf_categories_recount(void) {
    if (!s_categories) return;
    for (int i=0;i<s_category_count;++i) s_categories[i].count=0;
    for (int i=0;i<s_count;++i) {
        if (s_shelf[i].removed) continue;
        ++s_categories[0].count;
        char child[BOOK_STORE_PATH_MAX],path[BOOK_STORE_PATH_MAX];
        if (!book_shelf_child(s_category_root,s_shelf[i].path,child,sizeof(child))) {
            ++s_categories[1].count; continue;
        }
        int written=snprintf(path,sizeof(path),"%s/%s",s_category_root,child);
        if (written<0 || (size_t)written>=sizeof(path)) continue;
        int index=shelf_category_add(path);
        if (index>=2) ++s_categories[index].count;
    }
    if (s_category_selected>=s_category_count || !s_categories[s_category_selected].available)
        s_category_selected=0;
}
static int shelf_category_at(int ordinal) {
    for (int i=0;i<s_category_count;++i) {
        if (s_categories[i].available && ordinal--==0) return i;
    }
    return -1;
}
static int shelf_category_available(void) {
    int count=0;
    for (int i=0;i<s_category_count;++i) if (s_categories[i].available) ++count;
    return count;
}
static EpdRect shelf_category_box(void) {
    int count=shelf_category_available()-s_category_menu_page*SHELF_CATEGORY_ROWS;
    if (count>SHELF_CATEGORY_ROWS) count=SHELF_CATEGORY_ROWS;
    if (count<1) count=1;
    return (EpdRect){36,158,400,16+count*83+(shelf_category_available()>SHELF_CATEGORY_ROWS ? 56 : 0)};
}
static void draw_category_menu(uint8_t *fb) {
    EpdRect box=shelf_category_box();
    ui_fill_round_rect(fb,box,23,UI_GRAY_WHITE);
    ui_draw_control_frame(fb,box,23,0x50);
    for (int row=0;row<SHELF_CATEGORY_ROWS;++row) {
        int index=shelf_category_at(s_category_menu_page*SHELF_CATEGORY_ROWS+row);
        if (index<0) break;
        int y=box.y+8+row*83;
        bool selected=index==s_category_selected;
        if (selected) ui_fill_round_rect(fb,(EpdRect){box.x+8,y,box.width-16,79},16,0xd0);
        ui_draw_icon(fb,box.x+35,y+40,27,index<2 ? UI_ICON_LIBRARY_BIG : UI_ICON_FOLDER_OPEN,UI_GRAY_BLACK);
        char name[BOOK_STORE_PATH_MAX],count[24];
        snprintf(count,sizeof(count),"%d",s_categories[index].count);
        int name_width=290-ui_text_fixed_width_px(ui_text_effective_px(22),count);
        if (name_width>228) name_width=228;
        if (name_width<60) name_width=60;
        copy_text(name,sizeof(name),shelf_category_name(index)); fit_text(name,28,name_width);
        ui_text_vc(fb,box.x+62,y+40,28,name,EPD_DRAW_ALIGN_LEFT,false);
        ui_text_vc(fb,box.x+box.width-38,y+40,22,count,EPD_DRAW_ALIGN_RIGHT,false);
        if (selected) epd_fill_circle(box.x+box.width-20,y+40,4,UI_GRAY_BLACK,fb);
    }
    if (shelf_category_available()>SHELF_CATEGORY_ROWS) {
        int y=box.y+box.height-28;
        ui_draw_icon(fb,box.x+48,y,28,UI_ICON_CHEVRON_LEFT,s_category_menu_page ? 0 : 0x90);
        char pages[24]; snprintf(pages,sizeof(pages),"%d / %d",s_category_menu_page+1,
            (shelf_category_available()+SHELF_CATEGORY_ROWS-1)/SHELF_CATEGORY_ROWS);
        ui_text_vc(fb,box.x+box.width/2,y,22,pages,EPD_DRAW_ALIGN_CENTER,false);
        ui_draw_icon(fb,box.x+box.width-48,y,28,UI_ICON_CHEVRON_RIGHT,0);
    }
}
static bool shelf_matches(const shelf_entry_t* item) {
    return shelf_category_matches(item->path) && item->search_match && (s_filter == 0 || (s_filter == 1 && !item->is_flash) || (s_filter == 2 && item->is_flash));
}
static int compare_books(const void* a, const void* b) {
    const shelf_entry_t* x = a;
    const shelf_entry_t* y = b;
    if (shelf_matches(x) != shelf_matches(y)) return shelf_matches(x) ? -1 : 1;
    if (x->favorite != y->favorite)
        return x->favorite ? -1 : 1;
    if (s_recent_sort && x->recent != y->recent) return x->recent > y->recent ? -1 : 1;
    int name = strcasecmp(x->name, y->name);
    return name ? name : strcmp(x->path, y->path);
}
static void sort_shelf(app_ctx_t* ctx) {
    invalidate_covers();
    if (s_count > 1) qsort(s_shelf, s_count, sizeof(*s_shelf), compare_books);
    s_visible_count = 0;
    while (s_visible_count < s_count && shelf_matches(&s_shelf[s_visible_count])) ++s_visible_count;
    ctx->leaf = 0;
}
static app_redraw_t shelf_category_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    if (ev->type!=UI_GESTURE_TAP && ev->type!=UI_GESTURE_SWIPE_U && ev->type!=UI_GESTURE_SWIPE_D)
        return APP_REDRAW_NONE;
    EpdRect previous=shelf_category_box();
    if (ev->type==UI_GESTURE_TAP && ui_nav_hit(ev->x0,ev->y0)>=0) {
        s_category_menu=false; ui_nav_request(ctx,ui_nav_hit(ev->x0,ev->y0)); return APP_REDRAW_NONE;
    }
    if (!s_category_menu) {
        if (ev->type!=UI_GESTURE_TAP || !s_categories) return APP_REDRAW_NONE;
        s_category_menu=true; s_category_menu_page=0;
        int ordinal=0;
        for (int i=0;i<s_category_selected;++i) if (s_categories[i].available) ++ordinal;
        s_category_menu_page=ordinal/SHELF_CATEGORY_ROWS;
    } else {
        int pages=(shelf_category_available()+SHELF_CATEGORY_ROWS-1)/SHELF_CATEGORY_ROWS;
        int direction=ev->type==UI_GESTURE_SWIPE_U ? 1 : ev->type==UI_GESTURE_SWIPE_D ? -1 : 0;
        if (!direction && ui_rect_hit(previous,ev->x0,ev->y0)) {
            int row=((int)ev->y0-previous.y-8)/83;
            bool footer=pages>1 && ev->y0>=previous.y+previous.height-56;
            if (footer) direction=ev->x0<previous.x+previous.width/2 ? -1 : 1;
            else {
                int index=ev->y0>=previous.y+8 && row<SHELF_CATEGORY_ROWS
                    ? shelf_category_at(s_category_menu_page*SHELF_CATEGORY_ROWS+row) : -1;
                if (index>=0) {
                    shelf_category_remember(ctx); s_category_selected=index;
                    s_category_menu=false; clear_selection(); sort_shelf(ctx); shelf_category_restore(ctx);
                    return APP_REDRAW_PAGE;
                }
            }
        } else if (!direction) s_category_menu=false;
        if (direction) {
            int next=s_category_menu_page+direction;
            if (next<0 || next>=pages) return APP_REDRAW_NONE;
            s_category_menu_page=next;
        }
    }
    EpdRect next=shelf_category_box();
    int bottom=previous.y+previous.height>next.y+next.height ? previous.y+previous.height : next.y+next.height;
    s_area=(EpdRect){24,79,424,bottom-79+4}; s_mode=MODE_GL16;
    render(ctx,ctx->fb);
    return APP_REDRAW_AREA;
}
static bool shelf_reserve(void) {
    if ((size_t)s_count < s_shelf_capacity) return true;
    if (s_count == INT_MAX) return false;
    size_t cap = s_shelf_capacity ? s_shelf_capacity * 2 : 32;
    if (cap > INT_MAX || cap > SIZE_MAX / sizeof(*s_shelf)) return false;
    shelf_entry_t* entries = heap_caps_realloc(s_shelf, cap * sizeof(*entries), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!entries) return false;
    s_shelf = entries;
    s_shelf_capacity = cap;
    return true;
}

// 在删除文件前预留记录，避免删除成功后才遇到内存不足。
// Reserve before unlink so allocation failure never loses a completed deletion's retry.
static delete_retry_t* delete_retry_reserve(const shelf_entry_t* entry) {
    delete_retry_t* existing = delete_retry_find(entry->path);
    if (existing) return existing;
    delete_retry_t* p = heap_caps_malloc(sizeof(*p), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(sizeof(*p));
    if (!p) return NULL;
    p->entry = *entry;
    p->next = s_delete_retries;
    s_delete_retries = p;
    return p;
}
static void delete_retry_discard(const char* path) {
    delete_retry_t** p = &s_delete_retries;
    while (*p) {
        if (!strcmp((*p)->entry.path, path)) {
            delete_retry_t* old = *p;
            *p = old->next;
            free(old);
            return;
        }
        p = &(*p)->next;
    }
}
// 显式目录栈放在 PSRAM；主任务只有 8 KB 栈，递归进多级 books 会在封面出现前溢出。
// Keep directory frames in PSRAM: recursive books scanning can overflow the 8 KB main-task stack before covers appear.
#define BOOK_SCAN_DEPTH_LIMIT 12
typedef struct {
    DIR *dir;
    char path[BOOK_STORE_PATH_MAX];
} shelf_scan_frame_t;
static void scan_shelf_dir(app_ctx_t *ctx, const char *root, bool is_flash, nvs_handle_t favorites,
                           nvs_handle_t hidden, bool *truncated, bool *unreadable,
                           bool *skipped) {
    if (*truncated) return;
    shelf_scan_frame_t *frames = heap_caps_calloc(BOOK_SCAN_DEPTH_LIMIT + 1, sizeof(*frames),
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frames) { *truncated = true; return; }
    size_t root_len = strnlen(root, sizeof(frames[0].path));
    if (root_len >= sizeof(frames[0].path)) { *skipped = true; free(frames); return; }
    frames[0].dir = opendir(root);
    if (!frames[0].dir) { *unreadable = true; free(frames); return; }
    memcpy(frames[0].path, root, root_len + 1);
    int depth = 0;
    while (depth >= 0 && !*truncated) {
        if (s_view == SHELF && app_poll_main_navigation(ctx)) break;
        errno = 0;
        struct dirent *ent = readdir(frames[depth].dir);
        if (!ent) {
            if (errno) *unreadable = true;
            closedir(frames[depth].dir);
            frames[depth].dir = NULL;
            --depth;
            continue;
        }
        if (ent->d_name[0] == '.') continue;
        char path[BOOK_STORE_PATH_MAX];
        int len = snprintf(path, sizeof(path), "%s/%s", frames[depth].path, ent->d_name);
        if (len < 0 || (size_t)len >= sizeof(path)) { *skipped = true; continue; }
        struct stat st;
        if (stat(path, &st) != 0) { *unreadable = true; continue; }
        if (S_ISDIR(st.st_mode)) {
            if (!is_flash && depth==0 && !strcmp(root,app_settings_books_dir())) shelf_category_add(path);
            if (depth == BOOK_SCAN_DEPTH_LIMIT) { *skipped = true; continue; }
            DIR *child = opendir(path);
            if (!child) { *unreadable = true; continue; }
            ++depth;
            frames[depth].dir = child;
            memcpy(frames[depth].path, path, (size_t)len + 1);
            continue;
        }
        if (!S_ISREG(st.st_mode)) continue;
        const char *ext = strrchr(ent->d_name, '.');
        if (!ext || (strcasecmp(ext, ".txt") && strcasecmp(ext, ".epub"))) continue;
        // 显式移出书架的书不再列入；文件仍在存储卡上，随时可以重新读回书架。
        // Books the user removed stay off the shelf; the file stays on the card and reading it
        // again puts it back.
        if (hidden && shelf_hidden_read_handle(hidden, path)) continue;
        if (strlen(ent->d_name) >= sizeof(((shelf_entry_t *)0)->name) ||
            st.st_size < 0 || (uint64_t)st.st_size > UINT32_MAX) { *skipped = true; continue; }
        if (!shelf_reserve()) { *truncated = true; break; }
        shelf_entry_t candidate = {0};
        shelf_entry_t *item = &candidate;
        copy_text(item->path, sizeof(item->path), path);
        if (!book_title_from_path(item->path, item->name, sizeof(item->name)))
            clean_filename(item->name, sizeof(item->name), ent->d_name);
        if (!strcasecmp(ext, ".epub")) {
            char title[sizeof(item->name)] = {0};
            char author[sizeof(item->author)] = {0};
            // Listing must stay cheap and bounded: never parse every EPUB before
            // the shelf is first painted. Opening a book populates this cache.
            // 首屏列书只读已有元数据缓存，不在封面出现前逐本解析 EPUB。
            if (book_epub_metadata_cached(item->path, title, sizeof(title), author, sizeof(author)) == ESP_OK)
                copy_text(item->author, sizeof(item->author), author);
        }
        item->search_match = read_pico_search_match(item->name, s_query);
        item->size = st.st_size;
        item->is_flash = is_flash;
        book_progress_t p;
        item->has_progress = book_progress_load(item->path, item->size, &p);
        item->pct = item->has_progress ? p.pct : 0;
        item->chapter = item->has_progress ? p.chapter : 0;
        item->recent = item->has_progress ? p.last_open_s : 0;
        item->favorite = favorites && favorite_read_handle(favorites, item->path);
        s_shelf[s_count++] = candidate;
    }
    for (int i = 0; i <= depth; ++i)
        if (frames[i].dir) closedir(frames[i].dir);
    free(frames);
}

// 阅读过的书即使不在书根目录内也要上书架：文件管理可以直接打开存储卡任意位置的 TXT/EPUB，
// 这些书以前读完就消失了。文件已删除或内容已被替换的记录不上架，与进度不可恢复的口径一致。
// A book that was read belongs on the shelf even outside the book roots, because the file
// manager can open a TXT or EPUB anywhere on the card. Records whose file is gone, or whose
// contents were replaced, stay off the shelf, matching the rule that changed sizes never resume.
typedef struct {
    nvs_handle_t favorites;
    nvs_handle_t hidden;
    bool truncated;
} shelf_backfill_t;

static bool shelf_backfill_visit(const char* path, const book_progress_t* progress, void* ctx) {
    shelf_backfill_t* scan = ctx;
    if (scan->truncated) return false;
    for (int i = 0; i < s_count; ++i)
        if (!strcmp(s_shelf[i].path, path)) return true;   // 书根目录已经收录
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uint64_t)st.st_size > UINT32_MAX || (uint32_t)st.st_size != progress->file_size)
        return true;
    if (scan->hidden && shelf_hidden_read_handle(scan->hidden, path)) return true;
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (strlen(base) >= sizeof(((shelf_entry_t*)0)->name)) return true;
    if (!shelf_reserve()) { scan->truncated = true; return false; }
    shelf_entry_t candidate = {0};
    shelf_entry_t* item = &candidate;
    copy_text(item->path, sizeof(item->path), path);
    if (!book_title_from_path(item->path, item->name, sizeof(item->name)))
        clean_filename(item->name, sizeof(item->name), base);
    if (!item->name[0]) return true;
    const char* ext = strrchr(base, '.');
    if (ext && !strcasecmp(ext, ".epub") && pico_boot_asset_allowed(item->path)) {
        char title[sizeof(item->name)] = {0};
        char author[sizeof(item->author)] = {0};
        if (book_epub_metadata_cached(item->path, title, sizeof(title), author, sizeof(author)) == ESP_OK)
            copy_text(item->author, sizeof(item->author), author);
    }
    item->size = (uint32_t)st.st_size;
    // 内置书根固定在 /flash 下，与 book_store.c 和 open_book 的前缀判断一致。
    // The internal root lives under /flash, matching the prefix test in book_store.c and open_book.
    item->is_flash = !strncmp(path, "/flash/", 7);
    item->has_progress = true;
    item->pct = progress->pct;
    item->chapter = progress->chapter;
    item->recent = progress->last_open_s;
    item->search_match = read_pico_search_match(item->name, s_query);
    item->favorite = scan->favorites && favorite_read_handle(scan->favorites, item->path);
    s_shelf[s_count++] = candidate;
    return true;
}

// 把书根目录之外读过的书补进书架，返回是否新增了条目。
// 阅读只写进度、不动存储版本，所以缓存复用的路径也必须调用它，否则刚读完的书要等手动重扫。
// Backfill books read outside the roots and report whether anything was added. Reading only
// writes progress and never bumps the store revision, so the cached path has to call this too
// or a freshly read book stays invisible until a manual rescan.
static bool shelf_backfill_read_books(bool* truncated) {
    nvs_handle_t favorites = 0;
    nvs_handle_t hidden = 0;
    (void)nvs_open("rp_favs", NVS_READONLY, &favorites);
    (void)nvs_open(SHELF_HIDDEN_NS, NVS_READONLY, &hidden);
    const int before = s_count;
    shelf_backfill_t backfill = {.favorites = favorites, .hidden = hidden};
    (void)book_progress_list(shelf_backfill_visit, &backfill);
    if (favorites) nvs_close(favorites);
    if (hidden) nvs_close(hidden);
    if (truncated && backfill.truncated) *truncated = true;
    return s_count != before;
}

// 26.9.21 S15：书架只读容量**快照**，绝不在扫描路径上现场扫表。
// 26.9.20 的真账：`scan_shelf()` 里这句「TF余 X MB」调 `book_store_free_bytes()` →
// `f_getfree()`，而 FSInfo 刚被挂载流程作废，于是整张 8 MB FAT 要重扫一遍、全程持卷锁。
// 界面线程排在探测任务后面，书架就被压到 3.667 s——三轮方差 1 ms，因为它由表大小决定。
// 现在分两件事：介质可用就列目录；空间没算完就写「统计中」，由后台回填后重画。
// 26.9.21 S15: the shelf reads a capacity SNAPSHOT only. The "TF free X MB" string used to
// call book_store_free_bytes() -> f_getfree() right after the FSInfo had been invalidated,
// rescanning 8 MB while holding the volume lock - that is what held the shelf at 3.667 s.
static void capacity_text(char* out, size_t cap, const book_store_root_t* root) {
    const char* label = root->is_flash ? "内置余" : "TF余";
    uint64_t free_bytes = 0;
    // 26.9.21：字面量「统计中」而不是「0.0 MB」——0.0 MB 会被读成「卡满了」。
    // Say "counting" rather than "0.0 MB": the latter reads as "the card is full".
    if (!book_store_free_snapshot(root, &free_bytes)) {
        snprintf(out, cap, "%s 统计中", label);
        return;
    }
    snprintf(out, cap, "%s %.1f MB", label, free_bytes / 1048576.0);
}

static void scan_shelf(app_ctx_t* ctx) {
    const uint32_t progress_revision=book_progress_revision();
    shelf_category_init();
    for (int i=2;i<s_category_count;++i) s_categories[i].available=false;
    s_count = 0;
    s_visible_count = 0;
    s_message[0] = 0;
    s_shelf_warning[0] = 0;
    // 内置分区每次启动都必须重新挂载；已有文件不会因此格式化。
    // Flash mounts on each boot; existing files are not formatted by that mount.
    book_store_root_t roots[BOOK_STORE_ROOT_MAX];
    int n = 0;
    esp_err_t root_err = book_store_roots(roots, &n);
    bool truncated = false, unreadable = root_err != ESP_OK || book_store_roots_degraded(), skipped = false;
    nvs_handle_t favorites = 0;
    nvs_handle_t hidden = 0;
    (void)nvs_open("rp_favs", NVS_READONLY, &favorites);
    (void)nvs_open(SHELF_HIDDEN_NS, NVS_READONLY, &hidden);
    s_storage[0] = 0;
    for (int i = 0; i < n; ++i) {
        if (i == 0 || roots[i].is_flash || roots[i - 1].is_flash) {
            char capacity[64], one[48];
            capacity_text(one, sizeof(one), &roots[i]);
            snprintf(capacity, sizeof(capacity), "%s%s", s_storage[0] ? " · " : "", one);
            strncat(s_storage, capacity, sizeof(s_storage) - strlen(s_storage) - 1);
        }
        if (s_view == SHELF && app_poll_main_navigation(ctx)) break;
        scan_shelf_dir(ctx, roots[i].path, roots[i].is_flash, favorites, hidden,
                       &truncated, &unreadable, &skipped);
    }
    if (ctx->request_app) {
        if (favorites) nvs_close(favorites);
        if (hidden) nvs_close(hidden);
        s_shelf_cache_valid = false; s_scan_pending = true;
        return;
    }
    // 同路径重新出现也先完成旧清理，避免新阅读进度被后续重试擦掉。
    // Finish old cleanup even if the path reappears, before new reading can create progress.
    for (delete_retry_t* p = s_delete_retries; p; p = p->next) {
        int i = 0;
        while (i < s_count && strcmp(s_shelf[i].path, p->entry.path)) ++i;
        if (i == s_count) {
            if (!shelf_reserve()) { truncated = true; continue; }
            ++s_count;
        }
        s_shelf[i] = p->entry;
        s_shelf[i].selected = false;
        s_shelf[i].search_match = read_pico_search_match(p->entry.name, s_query);
    }
    if (favorites) nvs_close(favorites);
    if (hidden) nvs_close(hidden);
    // 补齐书根目录之外读过的书；显式移出书架的除外。
    // Backfill books read outside the roots, minus the ones removed from the shelf.
    (void)shelf_backfill_read_books(&truncated);
    shelf_categories_recount();
    sort_shelf(ctx);
    if (s_view==SHELF) shelf_category_restore(ctx);
    s_latest_path[0] = 0;
    char last_path[BOOK_STORE_PATH_MAX];
    if (book_progress_last_path(last_path, sizeof(last_path))) {
        for (int i = 0; i < s_count; ++i) {
            if (!strcmp(s_shelf[i].path, last_path) && !s_shelf[i].removed) {
                copy_text(s_latest_path, sizeof(s_latest_path), last_path);
                break;
            }
        }
    }
    if (!n) copy_text(s_storage, sizeof(s_storage), "存储不可用，请检查 TF 卡");
    if (truncated) snprintf(s_shelf_warning, sizeof(s_shelf_warning), "内存不足，仅列 %d 本；释放后重扫", s_count);
    else if (unreadable) copy_text(s_shelf_warning, sizeof(s_shelf_warning), "部分目录或文件不可读，请检查后重扫");
    else if (skipped) copy_text(s_shelf_warning, sizeof(s_shelf_warning), "部分文件过大或名称过长，未列入");
    else if (s_pending_invalidated) copy_text(s_shelf_warning, sizeof(s_shelf_warning), "图书已更新，旧待保存进度已作废");
    if (!s_count && !unreadable && !truncated) copy_text(s_message, sizeof(s_message), "暂无图书，请传入 TXT 或 EPUB");
    read_pico_sd_info_t cache_sd = {0};
    read_pico_sd_get_info(&cache_sd);
    s_cache_sd_present = cache_sd.present;
    s_cache_sd_mounted = cache_sd.mounted;
    s_cache_shelf_style = app_settings_shelf_style();
    s_store_revision = book_store_revision();
    s_progress_revision = progress_revision;
    s_shelf_cache_valid = !unreadable && !truncated;
    ESP_LOGI(TAG, "shelf books=%d roots=%d", s_count, n);
}

static void refresh_cached_progress(app_ctx_t* ctx) {
    const uint32_t revision=book_progress_revision();
    if(s_progress_revision==revision){if(s_view==SHELF)shelf_category_restore(ctx);return;}
    s_progress_revision=revision;
    // 目录没变也要补一次：阅读只写进度，不改存储版本，否则刚读完的书不会出现。
    // Backfill even when the listing is unchanged: reading writes progress and never bumps the
    // store revision, so a freshly read book would otherwise stay invisible until a manual rescan.
    const bool added = shelf_backfill_read_books(NULL);
    bool recent_changed = false;
    char latest[BOOK_STORE_PATH_MAX];
    if (book_progress_last_path(latest, sizeof(latest))) {
        for (int i = 0; i < s_count; ++i) {
            shelf_entry_t *item = &s_shelf[i];
            if (strcmp(item->path, latest) || item->removed) continue;
            book_progress_t progress;
            item->has_progress = book_progress_load(item->path, item->size, &progress);
            uint32_t next_recent=item->has_progress?progress.last_open_s:0;
            recent_changed=item->recent!=next_recent;
            item->pct = item->has_progress ? progress.pct : 0;
            item->chapter = item->has_progress ? progress.chapter : 0;
            item->recent = next_recent;
            copy_text(s_latest_path, sizeof(s_latest_path), latest);
            break;
        }
    }
    // 新增条目必须重排才能重算可见列表；只刷新进度时沿用原有顺序。
    // Added entries need a re-sort to recompute the visible list; a progress refresh alone
    // keeps the current order.
    if (added || (s_recent_sort && recent_changed)) { shelf_categories_recount(); sort_shelf(ctx); }
    if (s_view==SHELF) shelf_category_restore(ctx);
}
static void return_to_cached_shelf(app_ctx_t* ctx) {
    if (s_shelf_cache_valid && s_store_revision == book_store_revision()) refresh_cached_progress(ctx);
    else scan_shelf(ctx);
}

static void refresh_capacity(void) {
    book_store_root_t roots[BOOK_STORE_ROOT_MAX];
    int count = 0;
    s_storage[0] = 0;
    book_store_roots(roots, &count);
    for (int i = 0; i < count; ++i) {
        if (i && !roots[i].is_flash && !roots[i - 1].is_flash) continue;
        char line[64], one[48];
        capacity_text(one, sizeof(one), &roots[i]);
        snprintf(line, sizeof(line), "%s%s", s_storage[0] ? " · " : "", one);
        strncat(s_storage, line, sizeof(s_storage) - strlen(s_storage) - 1);
    }
}
static void manage_apply(app_ctx_t* ctx) {
    if (!strcmp(s_path, s_managed.path)) free_book();
    esp_err_t err;
    if (s_delete_confirm && !s_file_removed) {
        delete_retry_t* retry = delete_retry_reserve(&s_managed);
        if (!retry) {
            copy_text(s_manage_message, sizeof(s_manage_message), "内存不足，无法保留删除重试记录");
            s_clear_confirm = false;
            return;
        }
        bool removed = false;
        err = book_store_delete(s_managed.path, &removed);
        if (removed) {
            retry->entry.removed = true;
            s_file_removed = true;
            pending_discard(s_managed.path);
            book_store_notify_changed();
            s_store_revision = book_store_revision();
            for (int i = 0; i < s_count; ++i) if (!strcmp(s_shelf[i].path, s_managed.path)) s_shelf[i].removed = true;
            refresh_capacity();
        } else delete_retry_discard(s_managed.path);
    } else {
        pending_discard(s_managed.path);
        err = book_progress_forget(s_managed.path);
    }
    s_clear_confirm = false;
    if (err != ESP_OK) {
        copy_text(s_manage_message, sizeof(s_manage_message), s_file_removed ?
                  "文件已删除，进度清理失败，请重试" : s_delete_confirm ?
                  "文件删除失败，请检查存储后重试" : "进度清理失败，请重新尝试");
        return;
    }
    delete_retry_discard(s_managed.path);
    s_view = SHELF;
    int write = 0, leaf = ctx->leaf;
    for (int i = 0; i < s_count; ++i) {
        if (!strcmp(s_shelf[i].path, s_managed.path)) {
            if (s_file_removed) continue;
            s_shelf[i].has_progress = false; s_shelf[i].pct = 0; s_shelf[i].chapter = 0; s_shelf[i].recent = 0;
        }
        s_shelf[write++] = s_shelf[i];
    }
    s_count = write;
    sort_shelf(ctx);
    ctx->leaf = leaf < leaves() ? leaf : leaves() - 1;
}

/* ---- 输入与生命周期 / Input and lifecycle ---- */

static bool reader_turn_chrome(app_ctx_t *ctx) {
    bool changed = !s_reader_fullscreen || s_toolbar;
    if (!s_reader_fullscreen) (void)toggle_reader_fullscreen(ctx);
    s_reader_panel = READER_PANEL_NONE;
    s_reader_slider = -1;
    if (changed) { invalidate_prep(); s_reader_text_frame = false; }
    return changed;
}
// 长按正文：定位单词 → 离线查词 → 打开释义卡；失败只提示且不改页面状态。
// Long press on the body: locate the word, look it up offline and open the card;
// on failure show a notice without changing the reader state.
// 词典只有英文词头；中文、标点、空格必然查不到，单独提示比「未收录」准确。
// The dictionary holds English heads only; Chinese, punctuation and spaces can never hit,
// so say so instead of claiming the word is missing.
// ==== M3 情景释义：句界判定 + 上下文窗口（只读原文，不改排版）====
// 句末标点：ASCII .!?; 与换行 + 中文 。！？；…（各 3 字节）。V1 不处理缩写点号。
// Sentence terminators: ASCII .!?; and newline, plus the CJK marks (3 bytes each).
// V1 does not special-case abbreviations.
static size_t scene_term_at(const char* text, size_t pos, size_t len) {
    if (!text || pos >= len) return 0;
    const unsigned char c = (unsigned char)text[pos];
    if (c == '.' || c == '!' || c == '?' || c == ';' || c == '\n') return 1;
    if (pos + 3u <= len) {
        const char* p = text + pos;
        if (!memcmp(p, "。", 3) || !memcmp(p, "！", 3) || !memcmp(p, "？", 3) ||
            !memcmp(p, "；", 3) || !memcmp(p, "…", 3)) return 3;
    }
    return 0;
}

static size_t scene_prev_cp(const char* text, size_t pos, size_t floor) {
    if (pos == 0) return 0;
    size_t p = pos - 1;
    while (p > floor && ((unsigned char)text[p] & 0xC0u) == 0x80u) --p;
    return p;
}

static size_t scene_next_cp(const char* text, size_t pos, size_t len) {
    if (pos >= len) return len;
    const unsigned char c = (unsigned char)text[pos];
    size_t step = 1;
    if ((c & 0xE0u) == 0xC0u) step = 2;
    else if ((c & 0xF0u) == 0xE0u) step = 3;
    else if ((c & 0xF8u) == 0xF0u) step = 4;
    if (pos + step > len) step = 1;
    return pos + step;
}

// 把 [ws,we) 扩成「本页±1 + 句界」的纯文本上下文，居中裁剪到 cap-1，输出词偏移。
// 失败（跨页超界 / 无字符）返回 false —— 调用方降级普通 AI 释义并记 ctx_fail。
// Expand [ws,we) into the "page +/-1 + sentence boundary" plain text, centre-clip
// to cap-1 and report the word offset. False (page overflow / no text) makes the
// caller fall back to the plain AI definition and log ctx_fail.
static bool scene_build_context(size_t page, size_t ws, size_t we,
                                char* out, size_t cap, uint16_t* word_off, uint8_t* flags) {
    if (!out || cap < 2 || !s_text || s_text_len == 0) return false;
    if (ws >= we || we > s_text_len) return false;
    const size_t pages = book_layout_page_count();
    if (page >= pages) return false;
    size_t lo = book_layout_page_start_offset(page > 0 ? page - 1 : 0);
    size_t hi = (page + 2 < pages) ? book_layout_page_start_offset(page + 2) : s_text_len;
    if (lo > ws || hi < we || hi <= lo) return false;

    size_t slo = lo, shi = hi;
    for (size_t p = ws; p > lo;) {
        const size_t prev = scene_prev_cp(s_text, p, lo);
        const size_t n = scene_term_at(s_text, prev, hi);
        if (n && prev + n <= ws) { slo = prev + n; break; }
        if (prev >= p) break;
        p = prev;
    }
    for (size_t q = we; q < hi;) {
        const size_t n = scene_term_at(s_text, q, hi);
        if (n) { shi = q + n; break; }
        const size_t next = scene_next_cp(s_text, q, hi);
        if (next <= q) break;
        q = next;
    }
    // 任一侧不是句界（落在页界/窗口边）→ 句界回退，真机据此判断上下文完整性。
    // Either side not closed by punctuation (page/window edge) marks the fallback.
    if (slo == lo || shi == hi) *flags |= PUSH_SCENE_FLAG_SENTENCE_FALLBACK;

    const size_t limit = cap - 1;
    if (shi - slo > limit) {
        *flags |= PUSH_SCENE_FLAG_CTX_TRUNCATED;
        const size_t wlen = we - ws;
        const size_t half = (limit > wlen) ? (limit - wlen) / 2 : 0;
        slo = (ws > half) ? ws - half : 0;
        shi = we + half;
        if (shi > s_text_len) shi = s_text_len;
        if (shi - slo > limit) shi = slo + limit;
        // 两端退到 UTF-8 字符边界，绝不切半个汉字。
        while (slo < ws && slo < s_text_len && ((unsigned char)s_text[slo] & 0xC0u) == 0x80u) ++slo;
        while (shi > we && shi < s_text_len && ((unsigned char)s_text[shi] & 0xC0u) == 0x80u) --shi;
        if (slo > ws || shi < we || shi <= slo) return false;
    }
    const size_t n = shi - slo;
    if (n == 0 || n > limit) return false;
    memcpy(out, s_text + slo, n);
    out[n] = 0;
    *word_off = (uint16_t)(ws - slo);
    return true;
}

static bool word_is_english(const char* w) {
    bool alpha = false;
    for (const unsigned char* p = (const unsigned char*)w; *p; ++p) {
        if (*p >= 0x80) return false; // 多字节（中文等）不是词头 / multibyte never matches
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) alpha = true;
    }
    return alpha;
}

static app_redraw_t start_dict_lookup(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    (void)ctx;
    const int64_t t_start = esp_timer_get_time();
    char word[DICT_WORD_MAX] = {0};
    bool hit = false, english = false;
    size_t hit_start = 0, hit_end = 0;
    s_scene_valid = false; // 每次点词重置：旧快照绝不能喂给新卡 / reset per tap
    // M3-B：点新词即撤掉旧的情景待命——引用条与词偏移都跟着 s_scene_ctx 走，
    // 旧请求的答案回来时会因词/页对不上被丢弃（宁可不做）。
    // M3-B: a new tap disarms any pending scene request - the quote strip and the
    // word offset follow s_scene_ctx, so a stale answer is dropped on the word
    // and page check anyway (better nothing than the wrong thing).
    scene_req_disarm(&s_scene_req);
    lock_draw();
    if (s_text && s_view == READING) {
        size_t start = 0, end = 0;
        // 判栏 + 右栏坐标换算（v2 §5.1）：双栏时右栏命中要把 x 减回 s_rect 绝对空间，
        // 减去的量与右栏绘制 x 同出 reader_column_offset（横屏坑②配方）；落在重放空间
        // 之外显式失败并告警，绝不静默错位。单栏恒等：page=s_page、cx=x，调用与改形前
        // 逐位一致。休眠期 cols==1 恒不走右栏分支。
        // Column judge and right-column conversion (v2 section 5.1): a two-column
        // right-column hit subtracts x back into the s_rect absolute space — the same
        // reader_column_offset the right column's draw x comes from (the pitfall-2
        // recipe); landing outside the replay space fails loudly instead of silently
        // mis-hitting. Single column is the identity: page=s_page, cx=x, byte-identical
        // to the pre-change call. The right branch stays dormant while cols==1.
        EpdRect area = body_rect();
        int cols = reader_column_count();
        size_t page = s_page;
        int cx = x;
        bool outside = false;
        if (cols > 1 && x >= area.x + area.width / 2) {
            size_t right;
            reader_logical_pages(reader_visual_index(s_page), NULL, &right);
            if (right != SIZE_MAX) {
                page = right;
                cx = x - reader_column_offset(area, cols, 1);
                EpdRect built = book_layout_body_rect();
                if (cx < built.x || cx >= built.x + built.width) {
                    ESP_LOGW(TAG, "dict tap outside the right column: x=%d cx=%d", (int)x, cx);
                    outside = true;
                }
            }
        }
        if (!outside && book_layout_hit_test(page, cx, y, &start, &end) && end > start &&
            end - start < sizeof(word)) {
            memcpy(word, s_text + start, end - start);
            word[end - start] = 0;
            hit_start = start;
            hit_end = end;
            hit = true;
            english = word_is_english(word);
            // M3：留住定位快照（页 + 整章字节区间），供词卡「情景解读 ›」取上下文。
            // 点击 chip 时若 s_page/s_text 已变，scene_build_context 会拒答（ctx_fail）。
            // M3: keep the tap snapshot (page + chapter byte range) for the scene
            // entry. If the page/text moved by tap time, scene_build_context refuses.
            s_scene_page = page;
            s_scene_start = start;
            s_scene_end = end;
            s_scene_valid = true;
        }
    }
    unlock_draw();
    // 原句抓取放在解锁之后：扫描是有界的字符串操作，不该拉长持锁时间。
    // 此时还没翻页，s_text 一定仍是刚才命中的那一章。
    // Capture the sentence after unlocking: the scan is bounded string work and must
    // not extend the time the lock is held. No page has turned, so s_text is still the
    // chapter the hit came from.
    if (hit && english) vocab_log_capture(s_path, s_text, hit_start, hit_end);
    // gen26.3 A①：AI 查词是本地词典的补充，不是替代——只有本地词典没有收录（或词典
    // 整体不可用）的英文点词才喂给 push_sink：命中预取缓存秒出卡、连上发 0x17 请手机
    // 算、离线入搭车队列待下次连接回流。词典命中的词绝不再走 LLM：一词一答，杜绝
    // 「词典有释义却显示词典内容、与 APK 返回不一致」与「一词两答」；M1 联测实证
    // 14 次点词仅 2 次真 miss，其余全是白烧。M2 场景（ai 查句/翻译段落/联系上下文）
    // 不经本入口，协议不动。
    // gen26.3 A①: AI lookup supplements the local dictionary, it does not
    // replace it - only English taps the dictionary does NOT hold (or when the
    // dictionary is entirely unavailable) feed push_sink: a prefetch-cache hit
    // surfaces the card instantly, a live link sends 0x17 for the phone to
    // compute, and an offline tap queues for the next connection. A word the
    // dictionary holds never reaches the LLM - one tap, one answer, ending the
    // "dict shows its own entry while the APK answer differs" split and the
    // wasted LLM calls (M1 live test: 14 taps, only 2 real misses). The M2
    // scenarios (sentence lookup / paragraph translation / context) never pass
    // through this entry point, so the protocol stays put.
    const int64_t t_hit = esp_timer_get_time();
    // 弹窗避让锚点：按点词 x 选弹出侧——tap 分界与 gutter 中心重合，单栏横屏下同一谓词
    // 成立（与 v2 §5.1 判栏同一谓词）；分半几何随双栏生效，当前单栏横屏维持铺满
    // （split=false，第 4 步双栏开关落地后随栏状态翻开）。锚点必须先于 open 设置：
    // 开放时按最终卡片宽度折行释义。/ Popup avoidance anchor: pick the side by the tap x —
    // the tap boundary coincides with the gutter center, so the same predicate holds in
    // single-column landscape (the same predicate as the v2 section 5.1 column judge);
    // the split geometry takes effect with two columns while single column stays full
    // width (split=false; flipped with the column state once the two-column switch lands
    // at step 4). The anchor must be set before open: the definition wraps to the final
    // card width at open time.
    EpdRect lookup_body = body_rect();
    dict_popup_set_anchor(x < lookup_body.x + lookup_body.width / 2 ? 0 : 1, false);
    // 26.9.10 B-2：去掉入口的 `english` 总闸，语言中立——中文命中本地词典同样开卡。
    // 摘抄本（M5-A 后）要支持中文长句，入口若仍以 english 拦截，中文连卡都弹不出。
    // 26.9.10 B-2: drop the entry-level `english` gate; stay language-neutral - a
    // Chinese word that hits the local dictionary opens the card too. The excerpt
    // book (after M5-A) needs Chinese, and an english gate here would block it.
    bool found = hit && dict_popup_open(word);
    // 26.9.5 入口卡：拆掉 `english` 作为「开门」的总闸。本地没有释义时（英文未收录，
    // 或根本是中文——本地词典只收英文），给一张**静态入口卡**：词头 + 状态行 +
    // AI/情景入口 chip。它不挂超时、不被 0x07 认领（没发过请求），点「AI 释义 ›」
    // 才转 pending 并居中发 0x17——入口卡是 pending 的**前态**，不是终态。
    //
    // 只在 AI 开着时弹：入口卡的全部价值就是那两个入口 chip，AI 关时 chip 全被
    // 开关收起，弹出来只是一张说「点 AI 释义」却没有可点之物的空卡——那是骗人。
    // AI 关仍走原来的 notice（不静默：提示照给）。
    //
    // 26.9.5 entry card: `english` stops being the master gate for "does a card
    // open". When the dictionary holds nothing (English miss, or Chinese at all -
    // the local dictionary is English-only) show a **static entry card**: head +
    // status line + AI/scene entry chips. It has no timeout and 0x07 never claims
    // it (nothing was requested); tapping "AI 释义 ›" switches to pending and
    // sends 0x17. The entry card is the PREFIX state of pending, not a terminal one.
    //
    // Only with AI on: the card's entire value is those two chips, and with AI off
    // both are hidden by their switches - opening it would show a card that says
    // "tap AI" with nothing to tap, which is a lie. AI off keeps the old notice
    // (never silent: the user is still told why).
    bool entry = false;
    if (hit && !found && push_sink_ai_enabled()) {
        // 文案两态（审计裁定③）：中文说明**原因**（本地只收英文），英文未收录说明
        // **结果**（本地没有）——两类用户各得其所，不共用一句糊弄话。
        // Two wordings (adjudication 3): Chinese states the REASON (local dictionary
        // is English-only), an English miss states the OUTCOME (not in the local
        // dictionary) - each reader gets the truth, no shared hand-wave.
        const char* notice = english ? "本地未收录 · 点 AI 释义"
                                     : "本地词典仅收英文 · 点 AI 释义";
        entry = dict_popup_show_entry(word, notice);
    }
    // gen26.4：词典命中卡按 AI 开关显示「AI」入口 chip——点击可强制发起 AI 查询
    // （绕过 found 门，复用查询卡状态机）。miss/pending 卡不显示（查询态自带状态行）。
    // gen26.4: the dict-hit card shows the "AI" entry chip per the AI switch - a tap
    // forces an AI query (bypassing the found gate, reusing the querying state
    // machine). Miss/pending cards never show it (the querying state has its own line).
    if (found || entry) {
        dict_popup_set_ai_enabled(push_sink_ai_enabled());
        // M3：情景入口 = AI 开 + 情景开 + 本次点词确有上下文快照。三者缺一不显示，
        // 避免点了才报 ctx_fail。
        // M3: the scene entry needs AI on, scene on, and a real tap snapshot. Any
        // one missing hides it, so the user never taps into an immediate ctx_fail.
        dict_popup_set_scene_enabled(push_sink_ai_enabled() && push_sink_scene_enabled() &&
                                     push_sink_ai_scene_view_enabled() &&
                                     s_scene_valid);
    }
    ESP_LOGI(TAG, "dict tap word=\"%s\" hit=%d english=%d hittest=%lldms card=%lldms total=%lldms",
             word, (int)hit, (int)english, (long long)((t_hit - t_start) / 1000),
             (long long)((esp_timer_get_time() - t_hit) / 1000),
             (long long)((esp_timer_get_time() - t_start) / 1000));
    // 词典 miss（含词典整体不可用）才请 AI 补位；命中时一词一答，0x17 不再发出。
    // Only a dictionary miss (including the dictionary being entirely
    // unavailable) asks AI to fill in; on a hit there is one tap one answer
    // and 0x17 is never sent.
    //
    // gen26.3 ④：AI 开着就地弹「AI 查询中…」卡——真实状态，不是假卡；0x07 到达
    // 由 app_book_ai_def_ready 的门 B 原地更新为释义（预取缓存命中时释义已在 UI
    // 槽，下一个轮询 tick ≤500ms 完成更新）。AI 关着才走「词典未收录」提示。
    // gen26.3: with AI on, open the "AI querying..." card in place - a real
    // state, not a fake card; when 0x07 lands, app_book_ai_def_ready's gate B
    // updates it in place (on a prefetch-cache hit the def is already in the UI
    // slot and the next poll tick, <=500ms, completes the update). The "not
    // collected" notice only fires when AI is off.
    // 26.9.5：点词不再自动发 0x17 —— 英文未收录也先给入口卡，由用户点「AI 释义 ›」
    // 触发（入口卡是 pending 的前态）。0x17 与 AI 生词批随之从「点词即发」后移到
    // 「用户确认要查」：TAP_AI 分支里既有 push_sink_ai_tap_word_ctx + vocab_log_ai_queue
    // 已覆盖，中文走同一条路（裁定②：中文与英文 miss 同批，AI 交互过的词才落）。
    // 协议零改动：word[64] 注明明写 UTF-8，中文直接可发。
    // 26.9.5: a tap no longer fires 0x17 by itself - even an English miss shows the
    // entry card first and the user confirms via "AI 释义 ›" (the entry card is
    // pending's prefix). 0x17 and the AI vocabulary batch move from "on tap" to "on
    // the user asking": the TAP_AI branch already does push_sink_ai_tap_word_ctx +
    // vocab_log_ai_queue, and Chinese takes that same path (adjudication 2: Chinese
    // and English misses share one batch; only AI-interacted words are logged).
    // Protocol untouched: word[64] is documented as UTF-8, so Chinese goes as-is.
    if (found || entry) {
        s_reader_panel = READER_PANEL_DICT;
    } else {
        dict_popup_reset();
        s_reader_panel = READER_PANEL_NONE;
        const char* notice = !dict_ready() ? "TF 卡 dict 目录缺少词典"
                           : (hit && !english) ? "只支持查询英文单词"
                           : "词典未收录该词";
        copy_text(s_reader_notice, sizeof(s_reader_notice), notice);
        s_reader_notice_until = esp_timer_get_time() / 1000 + 2200;
    }
    invalidate_prep();
    return APP_REDRAW_PAGE;
}

// T1：阅读页**每一次页码前进**都从这里过账——正文翻页、AI 解析页翻页、全屏翻页。
// 返回 true 表示这一拍该整屏清底，且计数已经归零。
//
// 为什么要有这个函数：26.11.5 之前 `s_turns` 只在 turn_page() 里自增，AI 页翻页、
// 跳页、全屏翻页都不经过它，于是「每 N 页清底」从来没数过那些路径——现成的旋钮
// 转不动，用户看到的「设置是 5 却没生效」就是这条路。
//
// 26.11.7 S2：**调用点全工程只有 present() 一处**。26.11.6 在 turn_page/reader_ai_turn
// 各调一次，数的是「意图」；搬到 present 之后数的是「真的播了这一屏」。谁调用它，
// 谁就负责在整屏分支把 s_turns 归零——这条不变式由结构保证，不靠记忆。
// 26.11.7 S2: present() is the only caller. 26.11.6 counted intent in two places;
// counting after the frame is decided counts what was actually presented.
//
// 不变式：**任何整屏清底（T1/T2）之后 s_turns == 0**，于是「距上次整屏清底 ≤ N 页」
// 全局成立，不需要逐案记。T3 是区清，不归零（页脚带没清，账照常记）。
// Invariant: every screen-wide cleanup (T1/T2) resets s_turns, so "<= N pages since the
// last screen-wide cleanup" holds globally. T3 is area-only and does not reset.
static bool reader_note_turn(void) {
    const uint8_t full_pages = app_settings_reader_full_pages();
    if (!full_pages) { s_turns = 0; return false; }   // 「不强制」= 永不因计数清底
    if (++s_turns < full_pages) return false;
    s_turns = 0;
    return true;
}

// 26.11.6：AI 解析页翻页也要过账、也要走局刷。
//
// 26.11.5 的刀 C 让 `ai_reader_turn()` 返回 PAGE，落进「阅读视图整页出口」的整屏
// GC16——于是 AI 页每翻一页闪一次，这是第二个闪源。这里显式标记「这是一次页码
// 前进」并过账，present 据此走正文区差分；**进入 / 退出** AI 页仍由 s_ai_def_full
// 走整屏全刷（大动作，保留）。
// 26.11.6: AI detail page turns count and use the local refresh too. Cut C routed
// ai_reader_turn()'s PAGE into the reader's whole-page GC16, so every detail page
// flashed - the second source. Here we mark the frame as a page advance and book it;
// entering/leaving the detail view still goes through s_ai_def_full (a big action).
static app_redraw_t reader_ai_turn(int dir) {
    const int before = ai_reader_page();
    app_redraw_t result = ai_reader_turn(dir);
    if (result != APP_REDRAW_PAGE || ai_reader_page() == before) return result;
    s_reader_body_turn = true;
    s_reader_page_advance = true;        // 26.11.7 S2：账在 present 里结 / booked in present()
    return result;
}

static app_redraw_t turn_page(app_ctx_t* ctx, int dir) {
    if (!s_text || s_clear_confirm) return APP_REDRAW_NONE;
    // 页面图片表可能随跨章加载替换，旧页资格以实际成功显示的画面为准。
    // Chapter loads may replace the image table; the old-page eligibility comes from the displayed frame.
    bool from_text_frame = s_reader_text_frame;
    // 翻页后单词位置失效，点译卡片必须关闭。/ Word positions change after a turn; close the card.
    if (s_reader_panel == READER_PANEL_DICT) {
        dict_popup_reset();
        s_reader_panel = READER_PANEL_NONE;
    }
    bool changed = true;
    // 26.11.6：跨章要强制整屏清底（用户裁决：换章节全刷），计数随之归零。
    // 26.11.6: a chapter crossing forces a screen-wide cleanup, and the counter resets.
    const size_t chapter_before = s_chapter;
    // T3 的上一页资格必须取「上一次成功呈现的帧」，不能读 s_page——它马上要变，
    // 而且翻页失败时根本不会变。/ T3 reads the last successfully presented frame,
    // not s_page: it is about to move, and on a failed turn it does not move at all.
    const bool page_had_image = s_reader_frame_had_image;
    // 翻页语义视觉页化：一次翻页跨过整个视觉页（双栏时一次两栏）；跨章分支不变。
    // Turns advance by visual pages: one turn crosses a whole visual page (both columns
    // when two-column); the cross-chapter branches are unchanged.
    size_t visual = reader_visual_index(s_page);
    if (dir > 0 && visual + 1 < reader_visual_count()) s_page = reader_visual_start(visual + 1);
    else if (dir < 0 && visual) s_page = reader_visual_start(visual - 1);
    // ---- B′ 条件 3：翻到本章末尾 -------------------------------------------
    // 卡不在，下一章读不出来。此处必须**明说**并留在这一页——若放它走到下面的
    // load_chapter，失败分支会 `set_reader_view(TOC)`，用户看到的是「一翻页就跳目录」，
    // 比没反应更像故障。章内翻页不受影响（版面在内存里）。
    // B′ condition 3: the next chapter needs the card. Say so and stay on this page;
    // letting it reach load_chapter would jump to the TOC on failure, which looks more
    // like a fault than a dead key. In-chapter turns are unaffected (the layout is in RAM).
    else if (s_media_offline) { reader_offline_refuse(); return APP_REDRAW_PAGE; }
    else if (dir > 0 && s_chapter + 1 < book_chapter_count()) { save_progress(); changed = load_chapter(ctx, s_chapter + 1, 0, false); }
    else if (dir < 0 && s_chapter) { save_progress(); changed = load_chapter(ctx, s_chapter - 1, 0, true); }
    else return reader_turn_chrome(ctx) ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
    if (!changed) { set_reader_view(s_text ? TOC : SHELF); ctx->leaf = s_text ? s_chapter / BOOK_TOC_ROWS : 0; return APP_REDRAW_PAGE; }
    skip_hidden_image_pages(ctx, dir);
    if (!s_text) { s_view = SHELF; return APP_REDRAW_PAGE; }
    // 先选择旧布局的目标页，再切全屏，保留目标位置而不是跨过未读段落。
    // Choose the destination in the old layout first, then reflow to retain its reading position.
    bool chrome_changed = reader_turn_chrome(ctx);
    if (!s_text) return APP_REDRAW_PAGE;
    s_jump_offset = SIZE_MAX;
    s_last_turn_ms = s_stats_activity_ms = ctx->now_ms;
    ++s_stats_pending_turns;
    ++s_session_turns;
    // T1：所有页码前进共用一本账（正文、跨章、全屏）；跨章额外强制清底。
    // 26.11.7 S2 把自增搬进 present()：这里只置标记，不再自己数。
    // T1: every page advance shares one counter; a chapter crossing also forces cleanup.
    // 26.11.7 S2 moved the increment into present(); only the flag is set here.
    const bool chapter_changed = s_chapter != chapter_before;
    s_reader_page_advance = true;
    s_reader_cleanup = chapter_changed;     // 整屏分支负责把计数归零 / the full branch resets it
    if (s_unsaved < 8) ++s_unsaved;
    if (s_unsaved >= 8 && !s_save_failed) save_progress();
    ESP_LOGI(TAG, "turn chapter=%u page=%u/%u pct=%u", (unsigned)s_chapter, (unsigned)reader_visual_index(s_page) + 1, (unsigned)reader_visual_count(), percent(s_page));
    /* A2（14:26 裁定）翻页即清：所有成功翻页分支在此汇合（失败/边界分支均在上方
       return，s_page 未变故不清）。置 has=false + 清旧摘要缓冲首字节，让紧随其后的
       paint_reading 整帧重画把「摘要生成中…」一并画进右栏——上一页旧字绝不滞留到
       新页框。借既有整帧重画上屏，不新增独立区域刷：R0 冻结件 3 的 DU/GC16 计数、
       波形、发送帽、分帧一律零动（裁定 §二第 3 条：A2 不构成判据修订）。
       与既有三处 has 写入点语义正交：enter/exit 管会话边界、poll 管收帧、此处管
       翻页瞬间；只对伴读态生效，不污染非伴读态。清空动作抽到 app_companion_state.h
       纯函数，宿主守卫 ④ 测的就是同一份真实被调代码。 */
    if (reader_companion_active()) {
        companion_turn_clear(&s_companion_summary_has,
                             s_companion_summary, sizeof(s_companion_summary));
    }
    // 预渲染未命中时也使用 GL16；到达设定页数后在本次翻页整屏全刷。
    // A cache miss still uses GL16; the selected turn count triggers a full-screen refresh on this turn.
    app_redraw_t redraw = paint_reading(ctx, MODE_GL16);
    s_reader_turn_pending = !chrome_changed && redraw == APP_REDRAW_AREA && from_text_frame &&
        book_layout_page_image_count(s_page) == 0 && !app_settings_book_background_path()[0];
    // 26.11.6 T3：图 → 文字才算「灰阶离开」。图 → 图不触发（用户裁决），
    // 那一点边缘残留由 T1 的窗口兜底。
    // 26.11.6 T3: only image -> text is "grayscale left". Image -> image does not fire
    // (ruled); its edge residue is covered by the T1 window.
    s_reader_clear_from_image = !s_reader_cleanup && page_had_image &&
        book_layout_page_image_count(s_page) == 0;
    s_water_turn_pending = !chrome_changed && redraw == APP_REDRAW_AREA && !s_reader_cleanup &&
                           app_settings_reader_turn_effect() == 1 && from_text_frame &&
                           book_layout_page_image_count(s_page) == 0 && !app_settings_book_background_path()[0];
    s_water_turn_dir = dir > 0 ? E0470_TURN_RTL : E0470_TURN_LTR;
    return chrome_changed ? APP_REDRAW_PAGE : redraw;
}
static app_redraw_t resize_text(app_ctx_t* ctx, int dir) {
    int next = s_px + dir * BOOK_PX_STEP;
    if (next < BOOK_PX_MIN || next > BOOK_PX_MAX) return APP_REDRAW_NONE;
    size_t off = book_layout_page_start_offset(s_page);
    lock_draw();
    invalidate_prep();
    int old = s_px;
    if (!reader_relayout_cols(s_text, s_text_len, s_blocks, s_block_count, body_rect(), next,
                              reader_column_count())) {
        bool restored = reader_relayout_cols(s_text, s_text_len, s_blocks, s_block_count,
                                             body_rect(), old, reader_column_count());
        unlock_draw();
        if (!restored) {
            free_book();
            s_view = SHELF;
            copy_text(s_message, sizeof(s_message), "排版失败，请重新扫描并打开图书");
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    s_px = next;
    s_page = reader_visual_start(reader_visual_for_offset(off));
    unlock_draw();
    app_settings_set_book_px(s_px);
    save_progress();
    return paint_reading(ctx, MODE_DU);
}

static app_redraw_t apply_reader_layout(app_ctx_t* ctx, int next_px, int next_margin,
                                        uint8_t next_line, uint8_t next_para) {
    (void)ctx;
    if (next_px < BOOK_PX_MIN || next_px > BOOK_PX_MAX ||
        next_margin < BOOK_MARGIN_MIN || next_margin > BOOK_MARGIN_MAX ||
        next_line < 110 || next_line > 150 ||
        next_para > 75 || next_para % 25) return APP_REDRAW_NONE;
    if (next_px == s_px && next_margin == s_margin && next_line == app_settings_book_line_spacing() &&
        next_para == app_settings_book_paragraph_spacing()) return APP_REDRAW_NONE;
    size_t off = book_layout_page_start_offset(s_page);
    int old_px = s_px, old_margin = s_margin;
    uint8_t old_line = app_settings_book_line_spacing();
    uint8_t old_para = app_settings_book_paragraph_spacing();
    lock_draw();
    invalidate_prep();
    s_px = next_px;
    s_margin = next_margin;
    s_line_spacing = next_line;
    book_layout_set_spacing(next_line, next_para);
    // 正文带随字号与边距变了，题头的预留跟着重算。
    // The body band moved with the size and margin, so the heading's reservation follows.
    refresh_chapter_lead();
    bool ok = reader_relayout(body_rect());
    if (ok) s_page = reader_visual_start(reader_visual_for_offset(off));
    else {
        s_px = old_px;
        s_margin = old_margin;
        s_line_spacing = old_line;
        book_layout_set_spacing(old_line, old_para);
        refresh_chapter_lead();
        (void)reader_relayout(body_rect());
    }
    unlock_draw();
    if (!ok) return APP_REDRAW_NONE;
    app_settings_set_book_px((uint8_t)s_px);
    app_settings_set_book_margin((uint8_t)s_margin);
    app_settings_set_book_line_spacing(next_line);
    app_settings_set_book_paragraph_spacing(next_para);
    save_progress();
    return APP_REDRAW_PAGE;
}

static app_redraw_t apply_reader_typography(app_ctx_t* ctx, int tracking_index) {
    (void)ctx;
    if (!s_text || tracking_index < 0 || tracking_index > 4)
        return APP_REDRAW_NONE;
    int old_index = app_settings_book_tracking();
    if (tracking_index == old_index) return APP_REDRAW_NONE;
    size_t off = book_layout_page_start_offset(s_page);
    lock_draw();
    invalidate_prep();
    book_layout_set_typography((tracking_index - 2) * 2);
    bool ok = reader_relayout(body_rect_for_tracking(tracking_index));
    if (ok) s_page = reader_visual_start(reader_visual_for_offset(off));
    else {
        book_layout_set_typography((old_index - 2) * 2);
        (void)reader_relayout(body_rect());
    }
    unlock_draw();
    if (!ok) return APP_REDRAW_NONE;
    app_settings_set_book_tracking((uint8_t)tracking_index);
    save_progress();
    return APP_REDRAW_PAGE;
}

static app_redraw_t apply_reader_indent(app_ctx_t* ctx, int em, int adjustment) {
    (void)ctx;
    if (!s_text || em < 0 || em > 3 || adjustment < -20 || adjustment > 20 ||
        (em == app_settings_book_indent() && adjustment == app_settings_book_indent_adjust()))
        return APP_REDRAW_NONE;
    size_t off = book_layout_page_start_offset(s_page);
    uint8_t old_em = app_settings_book_indent();
    int old_adjustment = app_settings_book_indent_adjust();
    lock_draw();
    invalidate_prep();
    book_layout_set_first_line_indent((unsigned)em);
    book_layout_set_first_line_indent_adjust(adjustment);
    bool ok = reader_relayout(body_rect());
    if (ok) s_page = reader_visual_start(reader_visual_for_offset(off));
    else {
        book_layout_set_first_line_indent(old_em);
        book_layout_set_first_line_indent_adjust(old_adjustment);
        (void)reader_relayout(body_rect());
    }
    unlock_draw();
    if (!ok) return APP_REDRAW_NONE;
    app_settings_set_book_indent((uint8_t)em);
    app_settings_set_book_indent_adjust((int8_t)adjustment);
    save_progress();
    return APP_REDRAW_PAGE;
}

static app_redraw_t reader_manual_refresh(app_ctx_t* ctx) {
    (void)ctx;
    if (!s_text) return APP_REDRAW_NONE;
    s_reader_panel = READER_PANEL_NONE;
    s_reader_slider = -1;
    s_reader_footer_pending = s_reader_image_refresh_pending = false;
    s_water_turn_pending = s_reader_cleanup = false;
    s_du_count = s_turns = 0;
    invalidate_prep();
    return APP_REDRAW_FULL;
}

static app_redraw_t apply_reader_option(app_ctx_t* ctx, int kind) {
    (void)ctx;
    if (kind == 0) {
        app_settings_set_reader_power_turn(!app_settings_reader_power_turn());
        return APP_REDRAW_PAGE;
    }
    if (kind == 3) return toggle_reader_landscape(ctx);
    if (kind == 8) {
        /* gen30 步 3：伴读态内引发重排的设置变更一律「先出伴读再切」（哨位裁定
           §三格 1，实现形态按成本自选＝引导，不改设置、不自动改用户动作）。
           gen30 step 3: any reflow-inducing settings change inside companion mode
           must leave companion first (sentinel ruling section 3 grid 1; the chosen
           low-cost form is a hint - no automatic setting flip). */
        if (reader_companion_active()) {
            copy_text(s_reader_notice, sizeof(s_reader_notice), "请先退出伴读再切换");
            s_reader_notice_until = esp_timer_get_time() / 1000 + 2200;
            return APP_REDRAW_PAGE;
        }
        // 横屏双栏开关（第 4 步，kind=8——0-3 是既有开关、4-7 是 READING_ROW_* 子面板
        // 槽标记，命名空间顺延）：只翻设置并保存位置，重排由 draw_reader 的
        // ensure_body_orientation 惰性同步——建版矩形（栏宽）与地图栏数在其单出口
        // reader_relayout 内同批切换。横屏未开时开关只存偏好、无几何效果（副标题口径
        // 「横屏时正文分两栏」）；重排失败时 ensure_body_orientation 的回滚路径兜底。
        // Two-column switch (step 4): flip the setting and save the position; the reflow
        // is ensure_body_orientation's lazy sync, where the build rect (column width) and
        // the map's column count switch in one batch inside the reader_relayout single
        // exit. With landscape off the toggle stores a preference with no geometry effect
        // (the subtitle says so); ensure_body_orientation's rollback covers reflow failure.
        app_settings_set_reader_two_column(!app_settings_reader_two_column());
        if (s_text) save_progress();
        return APP_REDRAW_PAGE;
    }
    if (kind == 10) {
        /* 仿生阅读：伪加粗不改 advance，行宽/断行/字距全不变，因此只翻开关 + 重绘本页，
           不触发重排（当前页阅读位置天然保持）。/ Bionic reading never moves an advance,
           so the switch only repaints this page; no reflow, and the reading position is
           preserved by construction. */
        app_settings_set_bionic(!app_settings_bionic());
        return APP_REDRAW_PAGE;
    }
    if (!s_text || (kind != 1 && kind != 2 && kind != 9)) return APP_REDRAW_NONE;
    bool previous = kind == 1 ? app_settings_reader_immersive()
                  : kind == 2 ? app_settings_reader_hide_images()
                              : app_typography_enhanced();
    // 重新开启插图时，先补齐尺寸；文件读取必须在绘制锁外。
    // Probe missing dimensions before re-enabling images, outside the drawing lock.
    if (kind == 2 && previous) for (size_t i = 0; i < s_block_count; ++i) {
        blk_t *block = &s_blocks[i];
        if (block->image < 0 || (size_t)block->image >= s_image_count ||
            (block->image_width && block->image_height)) continue;
        unsigned width = 0, height = 0;
        if (book_chapter_image_dimensions(s_chapter, s_images[block->image], &width, &height) == ESP_OK) {
            block->image_width = width; block->image_height = height;
        }
        vTaskDelay(1);
    }
    lock_draw();
    size_t off = book_layout_page_start_offset(s_page);
    invalidate_prep();
    if (kind == 1) app_settings_set_reader_immersive(!previous);
    else if (kind == 9) app_settings_set_typography_enhanced(!previous);
    else {
        app_settings_set_reader_hide_images(!previous);
        book_layout_set_images_visible(previous);
    }
    bool ok = reader_relayout(body_rect());
    if (ok) s_page = reader_visual_start(reader_visual_for_offset(off));
    else {
        if (kind == 1) app_settings_set_reader_immersive(previous);
        else if (kind == 9) app_settings_set_typography_enhanced(previous);
        else {
            app_settings_set_reader_hide_images(previous);
            book_layout_set_images_visible(!previous);
        }
        (void)reader_relayout(body_rect());
        copy_text(s_reader_notice, sizeof(s_reader_notice), "排版失败，请重试");
        s_reader_notice_until = esp_timer_get_time() / 1000 + 1800;
    }
    unlock_draw();
    if (ok) {
        release_page_images();
        prepare_inline_image();
        s_jump_offset = SIZE_MAX;
        save_progress();
    }
    s_reader_footer_pending = s_reader_image_refresh_pending = false;
    s_water_turn_pending = false;
    return APP_REDRAW_PAGE;
}

// 阅读设置面板热区：固定卡片保持硬编码（一次性面板，无行模型可言），
// 开关行走行表驱动，与绘制锚点同一事实源。/ Refresh-settings hot zones: the fixed
// cards stay hardcoded (one-off panel chrome), the toggle rows are table-driven so
// hit-testing and drawing share one source of truth.
// 阅读设置的点击派发：行表驱动——标题区与「完成」先判，再逐槽命中，按槽位类型分发。
// 旧版那几张固定卡片（手动全刷/自动全刷/翻页效果）在 r91 已并入卡片槽位，派发随之改走行表。
// Tap dispatch for the reading settings: table driven. Header and Done are checked first,
// then the first slot hit decides and the cell type dispatches. r91 folded the old fixed
// cards (manual refresh / auto refresh / turn effect) into slots, so dispatch follows the table.
// 沿用上游 r91 的函数名：上游新增的 reading_settings_host_test 按名抽取它，
// 实现仍是我方行表驱动（槽位类型决定派发）。/ Keep upstream r91's name: its new
// reading_settings_host_test extracts it by name; the body stays our table-driven
// dispatcher (the cell type decides).
static app_redraw_t reading_settings_action(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (y < reading_settings_top() || ui_rect_hit((EpdRect){36, 1141, 612, 58}, x, y))
        s_reader_panel = READER_PANEL_NONE;
    else {
        int hit = -1;
        for (int i = 0; i < reading_row_count(); ++i) {
            if (ui_rect_hit(reading_toggle_rect(i), x, y)) { hit = i; break; }
        }
        if (hit < 0) return APP_REDRAW_NONE;
        switch (s_reading_rows[hit].kind) {
            case READING_ROW_KEYS: s_reader_panel = READER_PANEL_KEY_SETTINGS; break;
            case READING_ROW_TURN: s_reader_panel = READER_PANEL_TURN_SETTINGS; break;
            case READING_ROW_FULL: {
                static const uint8_t options[] = {5, 10, 15, 30, 0};
                unsigned next = 0;
                for (unsigned i = 0; i < 5; ++i)
                    if (app_settings_reader_full_pages() == options[i]) { next = (i + 1) % 5; break; }
                app_settings_set_reader_full_pages(options[next]);
                s_turns = 0;
                break;
            }
            case READING_ROW_WAVE:
                app_settings_set_reader_turn_effect(!app_settings_reader_turn_effect());
                break;
            default: return apply_reader_option(ctx, s_reading_rows[hit].kind);
        }
    }
    invalidate_prep();
    return APP_REDRAW_PAGE;
}

// 返回主页以本次开书来源为准；分类页码已在书架开书前记忆，不被目录/工具栏覆盖。
// Return to this opening's origin; shelf category/page were saved before opening, independently of TOC/tools.
static app_redraw_t reader_return(app_ctx_t *ctx) {
    save_progress();
    if (s_reader_return_home) {
        ui_nav_request(ctx, 0);
        return APP_REDRAW_NONE;
    }
    free_book();
    s_view = SHELF;
    s_reader_panel = READER_PANEL_NONE;
    return_to_cached_shelf(ctx);
    return APP_REDRAW_PAGE;
}

static app_redraw_t toggle_reader_fullscreen(app_ctx_t* ctx) {
    (void)ctx;
    if (!s_text) return APP_REDRAW_NONE;
    size_t off = book_layout_page_start_offset(s_page);
    lock_draw();
    invalidate_prep();
    s_reader_fullscreen = !s_reader_fullscreen;
    // 全屏切换会换掉整个正文带，题头的预留必须跟着换。
    // Toggling full screen swaps the whole body band, so the heading's reservation must follow.
    refresh_chapter_lead();
    bool ok = reader_relayout(body_rect());
    bool restored = true;
    if (ok) s_page = reader_visual_start(reader_visual_for_offset(off));
    else {
        s_reader_fullscreen = !s_reader_fullscreen;
        refresh_chapter_lead();
        restored = reader_relayout(body_rect());
    }
    unlock_draw();
    if (!ok) {
        if (!restored) {
            free_book();
            s_view = SHELF;
            copy_text(s_message, sizeof(s_message), "排版失败，请重新扫描并打开图书");
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    release_page_images();
    prepare_inline_image();
    s_jump_offset = SIZE_MAX;
    s_reader_panel = READER_PANEL_NONE;
    save_progress();
    return APP_REDRAW_PAGE;
}

static app_redraw_t toggle_reader_landscape(app_ctx_t* ctx) {
    (void)ctx;
    /* gen30 步 3：伴读态内横屏切换会引发重排，一律「先出伴读再切」（哨位裁定
       §三格 1）；双指手势与阅读设置两条入口都经本函数，故守卫落在这里。
       gen30 step 3: a landscape flip inside companion mode triggers a reflow, so
       it must leave companion first (sentinel ruling section 3 grid 1). Both the
       two-finger gesture and the reading-settings row route through here. */
    if (reader_companion_active()) {
        copy_text(s_reader_notice, sizeof(s_reader_notice), "请先退出伴读再切换");
        s_reader_notice_until = esp_timer_get_time() / 1000 + 2200;
        return APP_REDRAW_PAGE;
    }
    // 只翻设置并保存阅读位置(字节偏移与方向无关，重排后按偏移映射回同一处)；
    // 方向切换由 render() 在面板关闭时一次完成，重排由 draw_reader 的
    // ensure_body_orientation() 按版面尺寸惰性同步，这里不做临时旋转。
    // Flip the setting and save the position (the byte offset is orientation
    // independent and maps back to the same spot after the reflow); render() flips the
    // orientation once when the panel closes and draw_reader's ensure_body_orientation()
    // reflows lazily by body size — no temporary rotation dance here.
    app_settings_set_reader_landscape(!app_settings_reader_landscape());
    if (s_text) save_progress();
    return APP_REDRAW_PAGE;
}

static app_redraw_t select_reading_font_item(app_ctx_t* ctx, const ttf_font_item_t* item) {
    if (!item || !strcmp(ttf_font_path(), item->path)) return APP_REDRAW_NONE;
    char previous[TTF_FONT_PATH_MAX];
    copy_text(previous, sizeof(previous), ttf_font_path());
    if (ttf_font_open(item->path) != ESP_OK) return APP_REDRAW_NONE;
    size_t off = book_layout_page_start_offset(s_page);
    lock_draw();
    invalidate_prep();
    bool ok = reader_relayout(body_rect());
    if (ok) s_page = reader_visual_start(reader_visual_for_offset(off));
    else if (previous[0]) {
        (void)ttf_font_open(previous);
        (void)reader_relayout(body_rect());
    }
    unlock_draw();
    if (!ok) return APP_REDRAW_NONE;
    app_settings_set_font_path(item->path);
    copy_text(s_font_path, sizeof(s_font_path), item->path);
    save_progress();
    return APP_REDRAW_PAGE;
}

static int slider_index(EpdRect rect, int x, int count) {
    int span = rect.width - 128;
    int pos = x - rect.x - 64;
    int index = span > 0 && count > 1 ? (pos * (count - 1) + span / 2) / span : 0;
    if (index < 0) index = 0;
    if (index >= count) index = count - 1;
    return index;
}
static int reader_margin_input(EpdRect rect, int x, int current, int px, int tracking_px) {
    int widths[BOOK_MARGIN_CHOICES], values[BOOK_MARGIN_CHOICES];
    int count = reader_margin_levels(px, tracking_px, widths, values);
    int level = reader_margin_level_for(current, px, tracking_px);
    if (x < rect.x + 64) --level;
    else if (x >= rect.x + rect.width - 64) ++level;
    else level = slider_index(rect, x, count);
    return reader_margin_for_level(level, current, px, tracking_px);
}

static EpdRect reader_slider_rect(int slider) {
    // 字号滑杆在双列字体卡片上方，绘制与拖动共用位置。/ The size rail sits above the paired cards; paint and drag share its position.
    if (slider == 0) return (EpdRect){36,700-reader_ui_extra(112),612,76};
    if (slider == 1) return (EpdRect){36, 620, 294, 66};
    if (slider == 2) return (EpdRect){354, 620, 294, 66};
    if (slider == 3) return (EpdRect){36, 740, 612, 66};
    return (EpdRect){36, 860, 612, 66};
}

static int reader_slider_at(uint16_t x, uint16_t y) {
    int first = s_reader_panel == READER_PANEL_FONT_SETTINGS ? 0 : 1;
    int end = s_reader_panel == READER_PANEL_FONT_SETTINGS ? 1 : 5;
    for (int i = first; i < end; ++i) if (ui_rect_hit(reader_slider_rect(i), x, y)) return i;
    return -1;
}

static void reader_slider_map(int slider, int x) {
    if (slider == 0) s_reader_preview_px = BOOK_PX_MIN + slider_index(reader_slider_rect(0), x, 37);
    else if (slider == 1) {
        int widths[BOOK_MARGIN_CHOICES], values[BOOK_MARGIN_CHOICES];
        int count = reader_margin_levels(s_px, (app_settings_book_tracking() - 2) * 2, widths, values);
        s_reader_preview_margin = reader_margin_for_level(
            slider_index(reader_slider_rect(1), x, count), s_reader_preview_margin,
            s_px, (app_settings_book_tracking() - 2) * 2);
    }
    else if (slider == 2) s_reader_preview_line = 110 + slider_index(reader_slider_rect(2), x, 41);
    else if (slider == 3) s_reader_preview_para = slider_index(reader_slider_rect(3), x, 4) * 25;
    else if (slider == 4) s_reader_preview_tracking = slider_index(reader_slider_rect(4), x, 5);
}

static void reader_slider_begin(int slider, int x) {
    s_reader_slider = slider;
    s_reader_preview_px = s_px;
    s_reader_preview_margin = s_margin;
    s_reader_preview_line = app_settings_book_line_spacing();
    s_reader_preview_para = app_settings_book_paragraph_spacing();
    s_reader_preview_tracking = app_settings_book_tracking();
    EpdRect rect = reader_slider_rect(slider);
    int *value = slider == 0 ? &s_reader_preview_px : slider == 1 ? &s_reader_preview_margin :
                 slider == 2 ? &s_reader_preview_line : slider == 3 ? &s_reader_preview_para :
                 &s_reader_preview_tracking;
    s_reader_slider_endpoint = x < rect.x + 64 || x >= rect.x + rect.width - 64;
    if (slider == 1) {
        *value = reader_margin_input(rect, x, *value, s_px, (app_settings_book_tracking() - 2) * 2);
        return;
    }
    int min = slider == 0 ? BOOK_PX_MIN : slider == 2 ? 110 : 0;
    int max = slider == 0 ? BOOK_PX_MAX : slider == 2 ? 150 : slider == 3 ? 75 : 4;
    int step = slider == 3 ? 25 : 1;
    if (x < rect.x + 64) *value -= step;
    else if (x >= rect.x + rect.width - 64) *value += step;
    else reader_slider_map(slider, x);
    if (*value < min) *value = min;
    if (*value > max) *value = max;
}

static void reader_slider_move(int x) {
    int slider = s_reader_slider;
    if (slider < 0) return;
    s_reader_slider_endpoint = false;
    reader_slider_map(slider, x);
}

static app_redraw_t reader_slider_commit(app_ctx_t* ctx) {
    if (s_reader_slider < 0) return APP_REDRAW_NONE;
    int slider = s_reader_slider;
    int px = s_reader_preview_px, margin = s_reader_preview_margin, line = s_reader_preview_line;
    int para = s_reader_preview_para;
    int tracking = s_reader_preview_tracking;
    s_reader_slider = -1;
    if (slider == 4) return apply_reader_typography(ctx, tracking);
    return apply_reader_layout(ctx, px, margin, line, (uint8_t)para);
}


static app_redraw_t reader_panel_action(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (s_reader_panel == READER_PANEL_BACKGROUND || s_reader_panel == READER_PANEL_BACKGROUND_PREVIEW)
        return reader_background_action(x, y);
    // 点译卡片（我方）：与上面的阅读背景面板互斥，任何点击都先由它处理。
    // The lookup card (ours): mutually exclusive with the background panel above,
    // and it handles every tap first.
    // 点译卡片：任何点击都关闭，避免误触下面的正文翻页。
    // The lookup card closes on any tap so body page turns cannot fire beneath it.
    if (s_reader_panel == READER_PANEL_DICT) {
        // chip 上切换生词本并保留卡片；点其它地方照旧关闭。
        // The chip toggles the vocabulary book and keeps the card; anywhere else closes.
        dict_popup_tap_t tap = dict_popup_tap(x, y);
        // gen26.4：命中词典卡上的「AI」入口——强制发起 AI 查询（绕过 found 门），卡切
        // 「AI 查询中…」查询态；0x07 到达原地更新为释义，超时回退词典释义。AI 关着则
        // 如实提示、卡片不动。/ gen26.4: the "AI" entry on a dict-hit card forces an
        // AI query (bypassing the found gate), switching the card to "AI querying...";
        // 0x07 updates it in place, a timeout falls back to the dict definition. When
        // AI is off, say so and leave the card untouched.
        if (tap == DICT_POPUP_TAP_AI) {
            if (!push_sink_ai_enabled()) {
                copy_text(s_reader_notice, sizeof(s_reader_notice), "AI 释义未开启");
                s_reader_notice_until = esp_timer_get_time() / 1000 + 2200;
                return APP_REDRAW_PAGE;
            }
            // gen26.4.1 真机 defect 根治：dict_popup_word() 返回的就是卡内词头
            // 指针（&s_word），跨 force_ai/show_pending 的入口 reset 传递会被别名
            // 清空——0x17 发的是旧值（时序上在 reset 前）、查询态却建不起来
            // （「!*word」误判），词典卡被清空、0x07 回来门 B 失配被丢弃 = 释义
            // 全丢。取词立即拷贝到局部缓冲，词的生命周期与卡状态解耦。
            // gen26.4.1 device-defect fix at the call site: dict_popup_word()
            // returns the card's own head pointer (&s_word); passing it across
            // force_ai/show_pending's entry reset aliases it away - the 0x17
            // went out with the old value (sent before the reset) while the
            // query state never formed ("!*word" misread), the dict card was
            // wiped, and when 0x07 landed gate B missed so gate A dropped it =
            // every definition lost. Copy the word into a local buffer at once;
            // its lifetime is decoupled from the card state.
            char wbuf[64];
            copy_text(wbuf, sizeof(wbuf), dict_popup_word());
            // gen26.5 Step6：词典卡入口没有命中区间，pos 退到当前视觉页起始偏移
            // （比 0 有用，仍属「有书上下文」）。/ The dict-card entry has no hit span,
            // so pos falls back to the current visual page's start offset (better than
            // 0, still "has book context").
            push_sink_ai_tap_word_ctx(wbuf, book_id_from_path(s_path), reader_page_start_offset());
            // gen26.5 M2 Step8：词典命中卡上主动点「AI」chip 即入 AI 生词批（用户
            // 裁定语义第二条）。上方已判 push_sink_ai_enabled()——不到这里 AI 就是
            // 关着的，所以无需再判。攒批写由 vocab_log_ai_flush() 5s 后合批落盘。
            // gen26.5 M2 Step8: a deliberate "AI" chip tap on a dict-hit card joins
            // the AI vocabulary batch (adjudicated rule 2). push_sink_ai_enabled()
            // was already checked above - a disabled AI never reaches here - so no
            // second check. vocab_log_ai_flush() lands the batch 5s later.
            vocab_log_ai_queue(wbuf);
            // 26.11.31：push_sink_ai_tap_word_ctx 内部已分流——已连接即发 0x17，
            // 未连接则入挂起缓存 + 启动 60s 广播武装。据此选卡面：挂起词在位说明走
            // 了武装路径，卡切「等待手机连接…」（60s 超时）；否则连上直查，卡切
            // 「AI 查询中…」（15s 超时）。两态共用同一 pending 状态机与 over_dict 回退。
            if (push_sink_dict_ai_pending()) {
                dict_popup_force_ai_armed(wbuf);
                ESP_LOGI(TAG, "ai force armed \"%s\": dict card AI entry (offline)", wbuf);
            } else {
                dict_popup_force_ai(wbuf);
                ESP_LOGI(TAG, "ai force \"%s\": dict card AI entry", wbuf);
            }
            invalidate_prep();
            return APP_REDRAW_PAGE;
        }
        // gen26.6.2：命中「……」详解入口——卡面装不下的全文交给详解阅读会话
        // （词卡管快、阅读视图管全）。三个点本身就是入口（此前热区只有字宽，真机
        // 按不到、按下去还被判 MISSED 关卡）；此处只负责开会话，几何与命中在
        // dict_popup 内钉死。/ gen26.6.2: the "……" detail entry - hand the text the
        // card cannot hold to the detail session (the card is quick, the view is
        // complete). The three dots ARE the entry (its hit zone used to be glyph-wide:
        // unhittable on hardware, and a near-miss closed the card as MISSED). Geometry
        // and hit-testing live in dict_popup; this only opens the session.
        // M3 情景解读入口：点词快照 → 句界窗口 → 0x1A（M3-A 只验链路，
        // 情景视图留 M3-B）。取不到上下文就降级普通 AI 释义并记 ctx_fail，
        // 用户无感（回退矩阵第 2 行）。
        // M3 scene entry: tap snapshot -> sentence window -> 0x1A (M3-A verifies
        // the link only; the view lands in M3-B). A failed window falls back to
        // the plain AI definition and logs ctx_fail - invisible to the user.
        if (tap == DICT_POPUP_TAP_SCENE) {
            char wbuf[DICT_WORD_MAX];
            copy_text(wbuf, sizeof(wbuf), dict_popup_word());
            uint8_t flags = 0;
            uint16_t word_off = 0;
            bool ctx_ok = s_scene_valid && s_page == s_scene_page;
            if (ctx_ok) {
                ctx_ok = scene_build_context(s_scene_page, s_scene_start, s_scene_end,
                                             s_scene_ctx, sizeof(s_scene_ctx),
                                             &word_off, &flags);
            }
            const size_t ctx_len = ctx_ok ? strlen(s_scene_ctx) : 0;
            if (ctx_len && push_sink_ai_scene_send(wbuf, s_scene_ctx, ctx_len,
                                                   word_off, flags)) {
                // M3-B：置待命态 + 卡片转「情景解读中…」。答案到了由
                // app_book_scene_def_ready 开情景视图；等不到由 on_tick 的超时清态，
                // 卡片照既有 pending 超时降级为失败提示（不无限期假活）。
                // M3-B: arm the pending state and switch the card to "reading the
                // scene...". The answer opens the scene view via
                // app_book_scene_def_ready; if it never comes, on_tick disarms and
                // the existing pending timeout degrades the card - never a card
                // that fakes life forever.
                scene_req_arm(&s_scene_req, wbuf, word_off,
                              (uint8_t)strnlen(wbuf, SCENE_REQ_WORD_MAX - 1),
                              s_scene_page, ctx->now_ms + SCENE_REQ_TIMEOUT_MS);
                if (dict_popup_show_pending_scene(wbuf)) {
                    s_reader_panel = READER_PANEL_DICT;
                }
                ESP_LOGI(TAG, "scene request sent: word=\"%s\" off=%u flags=0x%02x ctx=%u",
                         wbuf, (unsigned)word_off, (unsigned)flags, (unsigned)ctx_len);
                invalidate_prep();
                return APP_REDRAW_PAGE;
            }
            scene_req_disarm(&s_scene_req); // 没发出去就不该等 / nothing sent, nothing to wait for
            ESP_LOGW(TAG, "ctx_fail: word=\"%s\" page=%u tap=%u start=%u end=%u valid=%d ctx=%u",
                     wbuf, (unsigned)s_page, (unsigned)s_scene_page,
                     (unsigned)s_scene_start, (unsigned)s_scene_end,
                     (int)s_scene_valid, (unsigned)ctx_len);
            push_sink_ai_tap_word_ctx(wbuf, book_id_from_path(s_path), s_scene_start);
            if (push_sink_ai_enabled() && dict_popup_show_pending(wbuf)) {
                s_reader_panel = READER_PANEL_DICT;
            } else {
                dict_popup_reset();
                s_reader_panel = READER_PANEL_NONE;
                invalidate_prep();
            }
            return APP_REDRAW_PAGE;
        }
        if (tap == DICT_POPUP_TAP_MORE) {
            const char* full = dict_popup_full_text();
            if (!full) { // 全文缓冲分配失败：不静默吞点击，照旧关卡片
                dict_popup_reset();
                s_reader_panel = READER_PANEL_NONE;
                invalidate_prep();
                return APP_REDRAW_PAGE;
            }
            // 同 gen26.4.1：dict_popup_word() 是卡内指针，取词必须落局部缓冲
            // （后续 reset 会把它清掉）。/ Same as gen26.4.1: dict_popup_word() is the
            // card's own pointer; copy it before any reset clears it.
            char wbuf[64];
            copy_text(wbuf, sizeof(wbuf), dict_popup_word());
            ai_reader_source_t src;
            memset(&src, 0, sizeof(src));
            src.chapter = s_chapter;
            src.page = reader_visual_index(s_page);           // 视觉页号（页脚展示）
            src.page_start_offset = s_page;                   // 逻辑页索引（锚点兜底）
            copy_text(src.title, sizeof(src.title),
                      s_book_title[0] ? s_book_title : s_title);
            // 先开会话再收卡片：ai_reader 内部自持副本，s_full 随 reset 释放
            // （顺序颠倒会拿一块已释放的内存去开会话）。/ Open first, reset after: the
            // detail view keeps its own copy and s_full dies with the reset (the other
            // order would hand freed memory to the session).
            if (ai_reader_open(wbuf, full, &src)) {
                // gen26.6.3：直接请求整屏全刷，与关闭详解的 ai_reader_close_full 同构
                // （开/关各一次 GC16 的「双 GC16」）。此前置 s_ai_def_full 再返回 PAGE：
                // 该标志只有**推送回流**那条路径（app_loop 消费）会兑现，点词路径下它
                // 会滞留到下一次推送释义才被消费，凭空多出一次全刷；且本帧仍走局部刷新，
                // 词卡→详解的版面跨度会留残影。返回 FULL 把「档位」直接交出去，语义单一。
                // gen26.6.3: ask for a screen-wide FULL refresh right here, mirroring
                // ai_reader_close_full (one GC16 on open, one on close = the "double
                // GC16"). The old code set s_ai_def_full and returned PAGE: that flag is
                // only honoured by the PUSH-return path (consumed in app_loop), so on the
                // tap path it lingered until the next pushed definition and caused an
                // extra full refresh out of nowhere - while THIS frame still went partial
                // and ghosted across the card->detail layout jump. Returning FULL hands
                // the tier over directly, one meaning only.
                ESP_LOGI(TAG, "detail session opened for \"%s\" (more entry tapped)", wbuf);
                dict_popup_reset();
                s_reader_panel = READER_PANEL_NONE;
                invalidate_prep();
                return APP_REDRAW_FULL;
            }
            return APP_REDRAW_NONE; // 开会话失败：卡片保持，不静默吞掉点击
        }
        // gen26.3 查询态卡：chip 点击忽略（存进去会是状态行），零变化不 redraw。
        // gen26.3 querying card: chip tap ignored (a save would store the
        // status line); zero state change, so no redraw.
        if (tap == DICT_POPUP_TAP_NOT_READY) return APP_REDRAW_NONE;
        if (tap == DICT_POPUP_TAP_MISSED) {
            dict_popup_reset();
            s_reader_panel = READER_PANEL_NONE;
        } else if (tap == DICT_POPUP_TAP_REMOVED && vocab_log_is_view_path(s_path)) {
            // 在生词本视图里删了一条：重新渲染并回到原来那一页。
            // Removed an entry while reading the vocabulary view: re-render and land
            // back on the same page.
            dict_popup_reset();
            s_reader_panel = READER_PANEL_NONE;
            return open_vocab_view(ctx, true);
        }
        invalidate_prep();
        return APP_REDRAW_PAGE;
    }
    if (s_reader_panel == READER_PANEL_TOOLS) {
        for (int i = 0; i < BOOK_TOOL_COUNT; ++i) if (ui_rect_hit(tool_rect(i), x, y)) {
            invalidate_prep();
            if (i == 0) {
                save_progress();
                set_reader_view(TOC);
                s_message[0] = 0;
                s_toc_jump_open = s_toc_jump_drag = false;
                s_reader_panel = READER_PANEL_NONE;
                size_t toc = current_toc_position();
                ctx->leaf = toc == SIZE_MAX ? 0 : (int)(toc / BOOK_TOC_ROWS);
            } else if (i == 1) {
                (void)bookmark_toggle();
            } else if (i == 2) {
                s_reader_panel = READER_PANEL_STATS;
            } else if (i == 3) {
                s_reader_panel = READER_PANEL_REFRESH_SETTINGS;
            } else if (i == 4) {
                s_line_spacing = app_settings_book_line_spacing();
                s_reader_slider = -1;
                s_reader_panel = READER_PANEL_FONT_SETTINGS;
            } else if (i == 5) {
                // 5 生词本：渲染虚拟书并打开，原书进度由 open_book_impl 先存好。
                // Vocabulary book: render the virtual book and open it; the current
                // book's progress is saved first by open_book_impl().
                return open_vocab_view(ctx, false);
            } else {
                // 6 听书：打开 TTS 面板。开关在面板里（NVS rp_tts 持久化），打开
                // 面板本身不改变开关态。/ TTS: open the TTS panel. The switch lives
                // in the panel (persisted in NVS rp_tts); opening the panel alone
                // never changes the switch.
                s_reader_panel = READER_PANEL_TTS;
            }
            return APP_REDRAW_PAGE;
        }
        s_reader_panel = READER_PANEL_NONE;
        invalidate_prep();
        return APP_REDRAW_PAGE;
    }
    if (s_reader_panel == READER_PANEL_STATS) {
        const int top = 768;
        EpdRect details = {36, top + 319, 294, 78}, recent = {354, top + 319, 294, 78};
        if (ui_rect_hit(details, x, y)) {
            s_bookmark_page = 0;
            s_bookmark_edit = s_bookmark_delete_confirm = s_bookmark_delete_error = false;
            s_bookmark_selected = 0;
            s_reader_panel = READER_PANEL_BOOKMARKS;
        }
        else if (ui_rect_hit(recent, x, y)) {
            flush_ticket_stats();
            s_recent_days_valid = book_ticket_recent_days(s_recent_days);
            s_reader_panel = READER_PANEL_STATS_RECENT;
        } else s_reader_panel = y < top ? READER_PANEL_NONE : READER_PANEL_TOOLS;
        invalidate_prep();
        return APP_REDRAW_PAGE;
    }
    if (s_reader_panel == READER_PANEL_BOOKMARKS) {
        const int top = 560;
        if (s_bookmark_delete_confirm) {
            if (y >= top + 348 && y < top + 412) {
                if (x < 342) s_bookmark_delete_confirm = s_bookmark_delete_error = false;
                else if (bookmark_delete_selected()) s_bookmark_delete_confirm = s_bookmark_delete_error = false;
                else s_bookmark_delete_error = true;
            }
        } else if (y < top) s_reader_panel = READER_PANEL_NONE;
        else if (y < top + 86) {
            if (x >= 544 && bookmark_count()) {
                s_bookmark_edit = !s_bookmark_edit;
                s_bookmark_selected = 0;
                s_bookmark_page = 0;
            } else s_reader_panel = READER_PANEL_STATS;
        } else {
            reader_bookmarks_t marks = {0};
            bool valid = bookmark_load(&marks);
            int pages = bookmark_pages(valid ? marks.count : 0);
            if (s_bookmark_edit && y >= top + 520 && y < top + 584) {
                if (x < 303) {
                    int first = s_bookmark_page * bookmark_rows();
                    for (int row = 0; row < bookmark_rows() && first + row < marks.count; ++row)
                        s_bookmark_selected |= UINT32_C(1) << (marks.count - 1 - (first + row));
                } else if (s_bookmark_selected) s_bookmark_delete_confirm = true;
            } else if (y >= top + (s_bookmark_edit ? 590 : 570)) {
                if (x < UI_LOCK_WIDTH / 2 && s_bookmark_page > 0) --s_bookmark_page;
                else if (x >= UI_LOCK_WIDTH / 2 && s_bookmark_page + 1 < pages) ++s_bookmark_page;
            } else if (valid && y >= top + 96) {
                int row = ((int)y - (top + 96)) / bookmark_row_step();
                int position = s_bookmark_page * bookmark_rows() + row;
                if (row >= 0 && row < bookmark_rows() && position < marks.count &&
                    ui_rect_hit(bookmark_row_rect(row),x,y)) {
                    int newest = (int)marks.count - 1 - position;
                    reader_bookmark_entry_t mark = marks.entries[newest];
                    if (s_bookmark_edit) s_bookmark_selected ^= UINT32_C(1) << newest;
                    else if (mark.chapter < book_chapter_count() && load_chapter(ctx, mark.chapter, mark.byte_off, false)) {
                        s_reader_panel = READER_PANEL_NONE;
                        save_progress();
                    }
                }
            }
        }
        invalidate_prep();
        return APP_REDRAW_PAGE;
    }
    if (s_reader_panel == READER_PANEL_STATS_RECENT) {
        int top = 584;
        if (y < top) s_reader_panel = READER_PANEL_NONE;
        else if (y < top + 112) s_reader_panel = READER_PANEL_STATS;
        invalidate_prep();
        return APP_REDRAW_PAGE;
    }
        if (s_reader_panel == READER_PANEL_REFRESH_SETTINGS) return reading_settings_action(ctx, x, y);
    if (s_reader_panel == READER_PANEL_KEY_SETTINGS || s_reader_panel == READER_PANEL_KEY_PICKER) {
        bool picker = s_reader_panel == READER_PANEL_KEY_PICKER;
        if (y < 270) s_reader_panel = READER_PANEL_NONE;
        else if (ui_rect_hit((EpdRect){36, 299, 56, 56}, x, y) || ui_rect_hit((EpdRect){36, 1141, 612, 58}, x, y))
            s_reader_panel = picker ? READER_PANEL_KEY_SETTINGS : READER_PANEL_REFRESH_SETTINGS;
        else if (picker) {
            for (int i = 0; i < APP_READER_KEY_COUNT; ++i) if (ui_rect_hit((EpdRect){36, 390 + i * 94, 612, 78}, x, y)) {
                bool saved = s_reader_key_pick == 3 ? app_settings_set_reader_hold_action(i)
                    : app_settings_set_reader_key_action(s_reader_key_pick, i);
                if (!saved) {
                    copy_text(s_reader_key_error, sizeof(s_reader_key_error), "保存失败，请重试");
                    return APP_REDRAW_PAGE;
                }
                s_reader_panel = READER_PANEL_KEY_SETTINGS;
                return APP_REDRAW_PAGE;
            }
            return APP_REDRAW_NONE;
        } else {
            for (unsigned i = 0; i < 3; ++i) if (ui_rect_hit(reader_key_card(i), x, y)) {
                s_reader_key_pick = i;
                s_reader_key_error[0] = 0;
                s_reader_panel = READER_PANEL_KEY_PICKER;
                return APP_REDRAW_PAGE;
            }
            if (ui_rect_hit((EpdRect){36, 646, 612, 96}, x, y)) {
                s_reader_key_pick = 3;
                s_reader_key_error[0] = 0;
                s_reader_panel = READER_PANEL_KEY_PICKER;
                return APP_REDRAW_PAGE;
            }
            for (unsigned i = 1; i <= 2; ++i) if (ui_rect_hit(reader_key_preset_card(i), x, y)) {
                app_settings_set_reader_key_preset(i);
                return APP_REDRAW_PAGE;
            }
            return APP_REDRAW_NONE;
        }
        invalidate_prep();
        return APP_REDRAW_PAGE;
    }
    if (s_reader_panel == READER_PANEL_TURN_SETTINGS) {
        if (y < 270) s_reader_panel = READER_PANEL_NONE;
        else if (ui_rect_hit((EpdRect){36, 299, 56, 56}, x, y)) s_reader_panel = READER_PANEL_REFRESH_SETTINGS;
        else if (ui_rect_hit((EpdRect){36, 1141, 612, 58}, x, y)) s_reader_panel = READER_PANEL_REFRESH_SETTINGS;
        else {
            for (int i = 0; i < 2; ++i) {
                if (!ui_rect_hit((EpdRect){36, 370 + i * 322, 612, 292}, x, y)) continue;
                app_settings_set_reader_vertical_turn(i != 0);
                invalidate_prep();
                return APP_REDRAW_PAGE;
            }
            return APP_REDRAW_NONE;
        }
        invalidate_prep();
        return APP_REDRAW_PAGE;
    }
    if (s_reader_panel == READER_PANEL_TTS) {
        // 版面：y<270（状态栏）直接收面板；返回键与完成键都回工具条（父面板，
        // 与翻页模式子面板同构）；开关卡切听书开关。/ Layout: a tap above y=270
        // (the status bar) closes the panel; the back chevron and the done key
        // both return to the tools bar (the parent panel, same shape as the
        // turn-mode sub-panel); the switch card toggles read-aloud.
        if (y < 270 || ui_rect_hit((EpdRect){36, 1141, 612, 58}, x, y) ||
            ui_rect_hit((EpdRect){36, 299, 56, 56}, x, y)) {
            s_reader_panel = READER_PANEL_TOOLS;
        } else if (ui_rect_hit((EpdRect){36, 650, 612, 96}, x, y)) {
            const bool on = tts_switch_on();
            tts_switch_set(!on);
            /* gen30 修复轮 · 故障二终裁（23:31 裁定）：听书开关只管声音，摘要链在伴读态恒通。
             *
             * 23:18 首裁曾把「关朗读＝停摘要」当作可接受后果，被项目所有者以三条产品事实
             * 推翻：视听分离（图书馆里只看右栏卡片是正当需求）、架构归因（摘要与朗读同挂
             * 0x19 是既成通道复用、不是产品约束）、体验判断（关听书即断摘要更新，伴读价值
             * 坍塌）。终裁：开关只控制声音；翻页即推 0x1C+0x19，AI 摘要照常生成、右栏照常
             * 更新，不受开关影响。
             *
             * 解耦靠新增的 TTS_ENABLE(0x1D) 轻量帧：设备把「开关状态」作为独立事实告诉手机，
             * 0x19 从此只承载文本。手机端收到页文本一律请求摘要，只有使能位为开时才朗读。
             * 因此这里的两个动作与是否伴读无关，统一执行：
             *   开到关 → 先发使能位=0（此后页文本静音）+ 再发空页帧（立即停当前这句）；
             *   关到开 → 先发使能位=1 + 重置哨兵让当前页重推（恢复朗读；摘要走同页去重）。
             * 非伴读态的可见行为与旧版逐位一致：开关关本就不推流，空页帧语义不变。
             *
             * gen30 repair round, fault #2 final ruling (23:31): the listening switch
             * controls sound only; the summary chain stays live in companion mode. The
             * earlier ruling treated "switch off = no summary" as acceptable and was
             * overruled on three product facts (see the ruling text). Decoupling rides
             * on TTS_ENABLE(0x1D): the device states the switch separately, 0x19 carries
             * text only, and the phone always requests a summary but speaks only when
             * enabled. Both actions below therefore run regardless of companion state;
             * off-state visible behaviour stays bit-identical to the old build. */
            /* 分支判据用「切换前」的 on：on==true 表示这一下是「开到关」。
               首版写成 if (!on)，把两个分支对调了——现场因此出现「关是朗读、
               开是闭嘴」的反向行为（第二轮回执坏消息 1），此处按语义钉死。 */
            if (on) {
                /* 开到关：先禁读，再立即停当前句。断线时跳过空页帧（D2B 重连补发兜底）；
                   使能位即便断线也尝试一次，未投递则下次变化重发。 */
                (void)push_sink_tts_enable_send(false);
                if (push_sink_link_up()) push_sink_tts_send_page("", 0);
            } else {
                /* 关到开：先允许朗读，再重置哨兵让当前页重推——恢复朗读当前句；
                   摘要请求同页同模式去重，不会因此重复生成。 */
                (void)push_sink_tts_enable_send(true);
                s_tts_last_chapter = s_tts_last_page = SIZE_MAX;
            }
        } else if (ui_rect_hit((EpdRect){36, 780, 612, 96}, x, y)) {
            /* gen30 步 3 接线 2/4：进/出伴读只置 pending，置位 + 重排 + 武装复用 +
               FAIR 闩锁 + 0x1B/0x1C/0x19 由 on_tick 同批原子执行。关面板回到正文，
               让 70/30 版面可见。/ Enter/exit only sets a pending flag; the atomic
               batch (state flip + reflow + arm reuse + FAIR latch + frame stream)
               runs in on_tick. Close the panel so the 70/30 layout is visible. */
            if (reader_companion_active()) s_companion_exit_pending = true;
            else s_companion_enter_pending = true;
            s_reader_panel = READER_PANEL_NONE;
        } else if (ui_rect_hit((EpdRect){36, 900, 612, 96}, x, y)) {
            /* gen30 步 3 接线 5：模式循环 → NVS 记录 → 0x1B(source=0) → 重发当前页
               （重置哨兵，本 tick 的 tts_follow_poll 自然重推 0x1C+0x19 流）。未连接仅
               NVS 记录，连接后首个 PAGE_BEGIN 自然同步（PAGE_BEGIN 帧体带 mode）。
               gen30 step 3 item 5: cycle the mode -> persist -> MODE_SET(source=DEVICE)
               -> resend the current page (sentinel reset makes this tick's follow
               poll re-air PAGE_BEGIN+0x19). Offline: NVS only; the first PAGE_BEGIN
               after connect syncs the mode (the frame body carries mode). */
            const uint8_t mode = (uint8_t)((app_settings_companion_mode() + 1u) % PUSH_RIDE_MODE_COUNT);
            app_settings_set_companion_mode(mode);
            if (push_sink_link_up()) {
                (void)push_sink_companion_mode_set(mode, PUSH_MODE_SRC_DEVICE);
                s_tts_last_chapter = s_tts_last_page = SIZE_MAX;
            }
        } else {
            return APP_REDRAW_NONE;
        }
        invalidate_prep();
        return APP_REDRAW_PAGE;
    }
    if (s_reader_panel == READER_PANEL_WEIGHT_SETTINGS) {
        if (y < 690 || y >= 1141) {
            s_reader_panel = y < 580 ? READER_PANEL_NONE : READER_PANEL_FONT_SETTINGS;
            return APP_REDRAW_PAGE;
        }
        int step=reader_weight_step(x,y),weight=app_settings_book_weight_fine();
        if(step) {
            int next=weight+step;if(next < -APP_BOOK_WEIGHT_FINE_MAX || next > APP_BOOK_WEIGHT_FINE_MAX)return APP_REDRAW_NONE;
            lock_draw();
            invalidate_prep();
            bool ok = app_settings_set_book_weight_fine(next);
            if (ok) ttf_set_stroke_adjust_fine(next);
            unlock_draw();
            if (!ok) {
                copy_text(s_reader_notice, sizeof(s_reader_notice), "保存失败，请重试");
                s_reader_notice_until = esp_timer_get_time() / 1000 + 1800;
            }
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_reader_panel == READER_PANEL_FONT_SETTINGS) {
        const int top = reader_font_top();
        EpdRect size = reader_slider_rect(0);
        EpdRect font = s_font_card;
        EpdRect layout = s_layout_card;
        EpdRect rule = s_rule_card;
        EpdRect bookfont = s_bookfont_card;
        if (ui_rect_hit((EpdRect){36, 1141, 612, 58}, x, y)) {
            s_reader_panel = READER_PANEL_NONE;
            invalidate_prep(); return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(size, x, y)) {
            int px = BOOK_PX_MIN + slider_index(size, x, BOOK_PX_MAX - BOOK_PX_MIN + 1);
            return apply_reader_layout(ctx, px, s_margin, app_settings_book_line_spacing(),
                                       app_settings_book_paragraph_spacing());
        }
        if (ui_rect_hit(font, x, y)) {
            ttf_font_scan();
            s_font_page = 0;
            int count = ttf_font_count();
            const char* active = ttf_font_path();
            for (int i = 0; i < count; ++i) {
                const ttf_font_item_t* item = ttf_font_item(i);
                if (item && !strcmp(active, item->path)) { s_font_page = i / BOOK_FONT_PAGE; break; }
            }
            s_reader_panel = READER_PANEL_FONT_PICKER;
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(s_weight_card, x, y)) {
            s_reader_panel = READER_PANEL_WEIGHT_SETTINGS;
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(s_antialias_card, x, y)) {
            lock_draw();
            invalidate_prep();
            bool on = !app_settings_book_antialias();
            bool ok = app_settings_set_book_antialias(on);
            if (ok) ttf_set_antialias(on);
            unlock_draw();
            if (!ok) {
                copy_text(s_reader_notice, sizeof(s_reader_notice), "保存失败，请重试");
                s_reader_notice_until = esp_timer_get_time() / 1000 + 1800;
            }
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(layout, x, y)) {
            s_reader_slider = -1;
            s_reader_panel = READER_PANEL_LAYOUT_SETTINGS;
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(rule, x, y)) {
            s_reader_panel = READER_PANEL_RULE_SETTINGS;
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(bookfont, x, y)) {
            // 触摸回调的栈比 on_tick 深得多：在这里直接重排一章会把主任务栈顶穿（实测溢出过一次）。
            // 只记下意愿，交给下一轮 on_tick 在浅栈上做。
            // The touch callback runs far deeper than on_tick, and laying a chapter out from here
            // overran the main task stack (measured once). Record the intent and let the next
            // on_tick do it on a shallow stack.
            bool on = !app_settings_book_fonts();
            app_settings_set_book_fonts(on);
            s_book_fonts_applied = on;
            book_set_embedded_fonts(on);
            s_fonts_reload_pending = true;
            invalidate_prep();
            return APP_REDRAW_PAGE;
        }
        s_reader_panel = y < top ? READER_PANEL_NONE : READER_PANEL_TOOLS;
        invalidate_prep();
        return APP_REDRAW_PAGE;
    }
    if (s_reader_panel == READER_PANEL_LAYOUT_SETTINGS) {
        const int top = 500;
        EpdRect back_icon = {36, top + 29, 56, 56};
        EpdRect back_button = {36, 1132, 612, 72};
        if (ui_rect_hit(back_icon, x, y) || ui_rect_hit(back_button, x, y)) {
            s_reader_slider = -1;
            s_reader_panel = READER_PANEL_FONT_SETTINGS;
            return APP_REDRAW_PAGE;
        }
        EpdRect margin = reader_slider_rect(1);
        EpdRect line = reader_slider_rect(2);
        EpdRect paragraph = reader_slider_rect(3);
        EpdRect tracking = reader_slider_rect(4);
        if (ui_rect_hit(margin, x, y)) {
            return apply_reader_layout(ctx, s_px,
                                       reader_margin_input(margin, x, s_margin, s_px,
                                                           (app_settings_book_tracking() - 2) * 2),
                                       app_settings_book_line_spacing(), app_settings_book_paragraph_spacing());
        }
        if (ui_rect_hit(line, x, y)) {
            uint8_t value = (uint8_t)(110 + slider_index(line, x, 41));
            return apply_reader_layout(ctx, s_px, s_margin, value,
                                       app_settings_book_paragraph_spacing());
        }
        if (ui_rect_hit(paragraph, x, y))
            return apply_reader_layout(ctx, s_px, s_margin, app_settings_book_line_spacing(),
                                       (uint8_t)(slider_index(paragraph, x, 4) * 25));
        if (ui_rect_hit(tracking, x, y))
            return apply_reader_typography(ctx, slider_index(tracking, x, 5));
        for (int i = 0; i < 4; ++i) {
            EpdRect choice = {214 + i * 108, 970, 100, 64};
            if (ui_rect_hit(choice, x, y)) return apply_reader_indent(ctx, i, app_settings_book_indent_adjust());
        }
        for (int i = 0; i < 3; ++i) {
            if (!ui_rect_hit(indent_adjust_rect(i), x, y)) continue;
            int adjustment = i == 1 ? 0 : app_settings_book_indent_adjust() + (i == 0 ? -1 : 1);
            if (adjustment < -20) adjustment = -20;
            if (adjustment > 20) adjustment = 20;
            return apply_reader_indent(ctx, app_settings_book_indent(), adjustment);
        }
        s_reader_panel = y < top ? READER_PANEL_NONE : READER_PANEL_FONT_SETTINGS;
        invalidate_prep();
        return APP_REDRAW_PAGE;
    }
    if (s_reader_panel == READER_PANEL_RULE_SETTINGS) {
        if (y < 729 || ui_rect_hit((EpdRect){36, 1113, 612, 72}, x, y)) {
            s_reader_panel = READER_PANEL_FONT_SETTINGS;
            return APP_REDRAW_PAGE;
        }
        for (int i = 0; i < 3; ++i) {
            if (!ui_rect_hit(rule_style_rect(i), x, y)) continue;
            app_settings_set_book_reading_line((uint8_t)i);
            book_layout_set_reading_line((unsigned)i);
            invalidate_prep();
            return APP_REDRAW_PAGE;
        }
        if (app_settings_book_reading_line()) {
            int offset = app_settings_book_reading_line_offset();
            if (ui_rect_hit(s_rule_offset_up, x, y)) offset -= 2;
            else if (ui_rect_hit(s_rule_offset_reset, x, y)) offset = 0;
            else if (ui_rect_hit(s_rule_offset_down, x, y)) offset += 2;
            else return APP_REDRAW_NONE;
            if (offset < -8) offset = -8;
            if (offset > 8) offset = 8;
            app_settings_set_book_reading_line_offset((int8_t)offset);
            book_layout_set_reading_line_offset(offset);
            invalidate_prep();
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_reader_panel == READER_PANEL_FONT_PICKER) {
        const int top = reader_font_picker_top();
        if (y < top + 112) {
            s_reader_panel = READER_PANEL_FONT_SETTINGS;
            return APP_REDRAW_PAGE;
        }
        int count = ttf_font_count(), start = s_font_page * BOOK_FONT_PAGE;
        for (int slot = 0; slot < BOOK_FONT_PAGE && start + slot < count; ++slot) {
            EpdRect card = reader_font_pick_rect(slot);
            if (ui_rect_hit(card, x, y)) return select_reading_font_item(ctx, ttf_font_item(start + slot));
        }
        return APP_REDRAW_NONE;
    }
    return APP_REDRAW_NONE;
}
static app_redraw_t adjust_spacing(app_ctx_t* ctx, int control) {
    uint8_t old_line = app_settings_book_line_spacing();
    uint8_t old_para = app_settings_book_paragraph_spacing();
    uint8_t line = old_line, para = old_para;
    if (control == 3 && line > 120) line -= 30;
    if (control == 4 && line < 180) line += 30;
    if (control == 5 && para > 0) para -= 25;
    if (control == 6 && para < 75) para += 25;
    if (control == 17) { line = 150; para = 50; }
    if (line == old_line && para == old_para) return APP_REDRAW_NONE;
    size_t off = book_layout_page_start_offset(s_page);
    lock_draw();
    invalidate_prep();
    book_layout_set_spacing(line, para);
    bool ok = reader_relayout(body_rect());
    if (ok) s_page = reader_visual_start(reader_visual_for_offset(off));
    else {
        book_layout_set_spacing(old_line, old_para);
        (void)reader_relayout(body_rect());
    }
    unlock_draw();
    if (!ok) return APP_REDRAW_NONE;
    app_settings_set_book_line_spacing(line);
    app_settings_set_book_paragraph_spacing(para);
    save_progress();
    return paint_reading(ctx, MODE_DU);
}
static app_redraw_t select_reading_font(app_ctx_t* ctx, int style) {
    static const char* filenames[] = {
        "Song.ttf", "Hei.ttf", "Kai.ttf", "WenKai.ttf", "FangSong.ttf", "KingHwa.ttf",
        "CangErYunHei05.ttf", "ChillKai.ttf"
    };
    static const char* names[] = {"宋体", "黑体", "楷体", "文楷", "仿宋", "京华老宋", "仓耳云黑05", "寒蝉正楷"};
    char path[160];
    if (snprintf(path, sizeof(path), "%s/%s", app_settings_fonts_dir(), filenames[style]) >= sizeof(path)) return APP_REDRAW_NONE;
    if (!strcmp(ttf_font_path(), path)) return APP_REDRAW_NONE;
    struct stat font_stat;
    if (stat(path, &font_stat) != 0) {
        snprintf(s_message, sizeof(s_message), "请将 %s 字体放入所选字体目录", names[style]);
        return APP_REDRAW_PAGE;
    }
    if (ttf_font_open(path) != ESP_OK) {
        snprintf(s_message, sizeof(s_message), "请将 %s 字体放入所选字体目录", names[style]);
        return APP_REDRAW_PAGE;
    }
    app_settings_set_font_path(path);
    s_message[0] = 0;
    size_t off = book_layout_page_start_offset(s_page);
    save_progress();
    lock_draw();
    invalidate_prep();
    bool ok = reader_relayout(body_rect());
    if (ok) s_page = reader_visual_start(reader_visual_for_offset(off));
    copy_text(s_font_path, sizeof(s_font_path), ttf_font_path());
    unlock_draw();
    if (!ok) {
        free_book(); s_view = SHELF;
        copy_text(s_message, sizeof(s_message), "字体重排失败，请重新打开图书");
        return APP_REDRAW_PAGE;
    }
    save_progress();
    return paint_reading(ctx, MODE_GL16);
}
static app_redraw_t manage_action(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (ui_rect_hit(manage_back_rect(), x, y)) {
        if (s_clear_confirm) s_clear_confirm = false;
        else s_view = SHELF;
        return APP_REDRAW_PAGE;
    }
    if (!s_clear_confirm && !s_file_removed && ui_rect_hit((EpdRect){UI_MARGIN, manage_panel().y + manage_panel().height - 190, ui_content_width(), 68}, x, y)) {
        extern void app_files_request_folder(int folder);
        app_files_request_folder(0);
        ui_nav_request(ctx, 2);
        return APP_REDRAW_NONE;
    }
    int count = s_clear_confirm || s_file_removed ? 2 : 3;
    for (int i = 0; i < count; ++i) if (ui_rect_hit(manage_rect(i, count), x, y)) {
        if (s_clear_confirm) { if (!i) s_clear_confirm = false; else manage_apply(ctx); }
        else if (!i) s_view = SHELF;
        else if (s_file_removed) manage_apply(ctx);
        else { s_clear_confirm = true; s_delete_confirm = i == 2; s_manage_message[0] = 0; }
        break;
    }
    return APP_REDRAW_PAGE;
}
static app_redraw_t editor_save(app_ctx_t* ctx) {
    if (ui_keyboard_pending()) {
        copy_text(s_editor_notice, sizeof(s_editor_notice), "请先选择候选字");
        return APP_REDRAW_PAGE;
    }
    char *start = s_editor_title;
    while (*start == ' ') ++start;
    if (start != s_editor_title) memmove(s_editor_title, start, strlen(start) + 1);
    size_t len = strlen(s_editor_title);
    while (len && s_editor_title[len - 1] == ' ') s_editor_title[--len] = 0;
    if (!s_editor_title[0]) {
        copy_text(s_editor_notice, sizeof(s_editor_notice), "书名不能为空");
        return APP_REDRAW_PAGE;
    }
    esp_err_t err = book_title_set(s_managed.path, s_editor_title);
    if (err != ESP_OK) {
        copy_text(s_editor_notice, sizeof(s_editor_notice), "保存失败，请检查书名或存储");
        return APP_REDRAW_PAGE;
    }
    for (int i = 0; i < s_count; ++i)
        if (!strcmp(s_shelf[i].path, s_managed.path)) copy_text(s_shelf[i].name, sizeof(s_shelf[i].name), s_editor_title);
    free(s_editor_cover); s_editor_cover = NULL;
    ui_keyboard_end();
    s_view = SHELF;
    sort_shelf(ctx);
    return APP_REDRAW_PAGE;
}
static app_redraw_t editor_paint(app_ctx_t* ctx, bool field) {
    bool search = s_view == SEARCH;
    ui_keyboard_update_t update = ui_keyboard_update(ctx->fb, 560,
        search ? (EpdRect){UI_MARGIN, 200, ui_content_width(), 88} : (EpdRect){36, 424, 612, 83},
        search ? UI_PX_BODY : 31, false, NULL, field);
    s_area = update.area; s_mode = update.layout ? MODE_GL16 : MODE_DU; s_input_settle = update.settle;
    return update.area.width ? APP_REDRAW_AREA : APP_REDRAW_NONE;
}
static app_redraw_t editor_action(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (y < 134) {
        if (x < 160) { ui_keyboard_end(); free(s_editor_cover); s_editor_cover = NULL; s_view = SHELF; return APP_REDRAW_PAGE; }
        if (x > 510) return editor_save(ctx);
        return APP_REDRAW_NONE;
    }
    if (ui_text_input_tap(&s_editor_input, (EpdRect){36, 424, 612, 83}, 31,
                          false, NULL, x, y)) return editor_paint(ctx, true);
    ui_keyboard_result_t result = ui_keyboard_tap(x, y, 560, ctx->now_ms);
    if (result == UI_KEYBOARD_DONE) return editor_save(ctx);
    return result == UI_KEYBOARD_CHANGED ? editor_paint(ctx, false) : APP_REDRAW_NONE;
}
// 成功项退出选择，已删文件的失败项仅重试元数据。/ Deselect successes; removed-file failures retry metadata only.
static void batch_apply(app_ctx_t* ctx) {
    unsigned done = 0, failed = 0;
    int write = 0;
    const bool unshelf = s_batch_kind == BATCH_UNSHELF;
    for (int i = 0; i < s_count; ++i) {
        shelf_entry_t item = s_shelf[i];
        bool discard = false;
        if (item.selected) {
            esp_err_t err = ESP_OK;
            if (unshelf) {
                // 只隐藏条目：文件与阅读进度都留在原处，再次打开这本书会重新上书架。
                // Hide the entry only: file and progress stay, and reopening the book puts it back.
                if (shelf_hidden_save(item.path, true)) discard = true;
                else err = ESP_FAIL;
            } else {
                if (!strcmp(s_path, item.path)) free_book();
                if (s_batch_kind == BATCH_DELETE && !item.removed) {
                    delete_retry_t* retry = delete_retry_reserve(&item);
                    if (!retry) { ++failed; s_shelf[write++] = item; continue; }
                    bool removed = false;
                    err = book_store_delete(item.path, &removed);
                    if (removed) {
                        retry->entry.removed = true;
                        item.removed = true; pending_discard(item.path);
                        book_store_notify_changed(); s_store_revision = book_store_revision();
                    } else delete_retry_discard(item.path);
                } else { pending_discard(item.path); err = book_progress_forget(item.path); }
            }
            if (err == ESP_OK) {
                delete_retry_discard(item.path);
                ++done; item.selected = false;
                if (!unshelf) {
                    item.has_progress = false; item.pct = 0;
                    item.chapter = 0; item.recent = 0;
                }
                discard = discard || item.removed;
            } else ++failed;
        }
        if (!discard) s_shelf[write++] = item;
    }
    s_count = write;
    refresh_capacity();
    int leaf = ctx->leaf;
    sort_shelf(ctx);
    ctx->leaf = leaf < leaves() ? leaf : leaves() - 1;
    s_batch_confirm = false;
    if (unshelf)
        snprintf(s_batch_message, sizeof(s_batch_message), "已移出书架 %u 本，失败 %u 本%s",
                 done, failed, failed ? "；所选可重试" : "");
    else
        snprintf(s_batch_message, sizeof(s_batch_message), "成功 %u 本，失败 %u 本%s",
                 done, failed, failed ? "；所选可重试" : "");
}
static app_redraw_t batch_action(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (s_batch_confirm) {
        if (ui_rect_hit(ui_row_rect(0, 2, 620, UI_BTN_H), x, y)) {
            s_batch_confirm = false;
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(ui_row_rect(1, 2, 620, UI_BTN_H), x, y)) {
            batch_apply(ctx);
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    for (int i = 0; i < 6; ++i) if (ui_rect_hit(batch_rect(i), x, y)) {
        s_batch_message[0] = 0;
        if (!i) select_page(ctx->leaf);
        else if (i == 1) clear_selection();
        else if (i == 2) { clear_selection(); s_batch_message[0] = 0; scan_shelf(ctx); }
        else if (selected_count()) {
            s_batch_kind = i == 3 ? BATCH_DELETE : i == 4 ? BATCH_CLEAR : BATCH_UNSHELF;
            s_batch_confirm = true;
        }
        else copy_text(s_batch_message, sizeof(s_batch_message), "请先选择图书");
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}
static app_redraw_t bulk_turn_page(app_ctx_t* ctx, int direction) {
    int next = ctx->leaf + direction;
    if (s_scan_pending || s_batch_confirm || next < 0 || next >= leaves()) return APP_REDRAW_NONE;
    ctx->leaf = next;
    return APP_REDRAW_PAGE;
}
static app_redraw_t bulk_finish(app_ctx_t* ctx) {
    int first = ctx->leaf * BOOK_BULK_ROWS;
    clear_selection();
    s_batch_message[0] = 0;
    s_batch_confirm = false;
    s_view = SHELF;
    ctx->leaf = first / shelf_rows();
    if (ctx->leaf >= leaves()) ctx->leaf = leaves() - 1;
    return APP_REDRAW_PAGE;
}
// 批量管理单独路由，不复用带示例菜单的旧底栏。
// Route batch management independently of the legacy bar and demo menu.
static app_redraw_t bulk_action(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (ui_rect_hit(manage_back_rect(), x, y)) {
        if (s_batch_confirm) { s_batch_confirm = false; return APP_REDRAW_PAGE; }
        return bulk_finish(ctx);
    }
    app_redraw_t result = batch_action(ctx, x, y);
    if (result != APP_REDRAW_NONE || s_batch_confirm || s_scan_pending) return result;
    for (int i = 0; i < 3; ++i) if (ui_rect_hit(bulk_filter_rect(i), x, y)) {
        if (i == 2) { search_begin(); return APP_REDRAW_PAGE; }
        if (i == 0) s_filter = (s_filter + 1) % 3;
        else { s_recent_sort = !s_recent_sort; app_settings_set_shelf_recent_sort(s_recent_sort); }
        clear_selection(); s_batch_message[0] = 0;
        sort_shelf(ctx);
        return APP_REDRAW_PAGE;
    }
    int nav = bulk_nav_hit(x, y);
    if (nav == 1) return bulk_finish(ctx);
    if (nav == 0 || nav == 2) return bulk_turn_page(ctx, nav == 0 ? -1 : 1);
    for (int row = 0; row < BOOK_BULK_ROWS; ++row) {
        int index = ctx->leaf * BOOK_BULK_ROWS + row;
        if (index < s_visible_count && ui_rect_hit(row_rect(row), x, y)) {
            toggle_selection(index); s_batch_message[0] = 0;
            return APP_REDRAW_PAGE;
        }
    }
    return APP_REDRAW_NONE;
}
static int bulk_control_at(app_ctx_t* ctx, uint16_t x, uint16_t y, EpdRect* rect) {
    *rect = manage_back_rect();
    if (ui_rect_hit(*rect, x, y)) return 711;
    if (s_batch_confirm) {
        for (int i = 0; i < 2; ++i) {
            *rect = ui_row_rect(i, 2, 620, UI_BTN_H);
            if (ui_rect_hit(*rect, x, y)) return 600 + i;
        }
        return -1;
    }
    for (int i = 0; i < 3; ++i) {
        *rect = bulk_filter_rect(i);
        if (ui_rect_hit(*rect, x, y)) return 110 + i;
        *rect = bulk_nav_rect(i);
        if (ui_rect_hit(*rect, x, y)) return 100 + i;
    }
    for (int i = 0; i < 6; ++i) {
        *rect = batch_rect(i);
        if (ui_rect_hit(*rect, x, y)) return 610 + i;
        if (ctx->leaf * BOOK_BULK_ROWS + i < s_visible_count) {
            *rect = row_rect(i);
            if (ui_rect_hit(*rect, x, y)) return i;
        }
    }
    return -1;
}
static app_redraw_t action_at(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (s_view == BULK) return bulk_action(ctx, x, y);
    if (s_view == IMPORT) {
        if (y < 160 && x < 120) { s_view = SHELF; return APP_REDRAW_PAGE; }
        int tab = ui_nav_hit(x, y);
        if (tab == 1) { s_view = SHELF; return APP_REDRAW_PAGE; }
        if (tab >= 0) { ui_nav_request(ctx, tab); return APP_REDRAW_NONE; }
        for (int i = 0; i < 4; ++i) if (ui_rect_hit(import_rect(i), x, y)) {
            if (i == 0) {
                extern const app_desc_t app_files;
                ctx->request_app = &app_files;
            } else {
                extern const app_desc_t app_transfer;
                if (i == 1) app_transfer_request_wifi_upload();
                else if (i == 2) app_transfer_request_hotspot_start();
                else app_transfer_request_usb_start();
                ctx->request_app = &app_transfer;
            }
            return APP_REDRAW_NONE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_view == TOC) {
        if (s_toc_jump_open) {
            int choice = book_toc_jump_hit(x, y);
            if (choice == BOOK_TOC_JUMP_TRACK) {
                s_toc_jump_percent = book_toc_jump_percent(x);
                return APP_REDRAW_PAGE;
            }
            if (choice == BOOK_TOC_JUMP_CONFIRM)
                ctx->leaf = book_toc_page_from_percent(s_toc_jump_percent, leaves());
            s_toc_jump_open = s_toc_jump_drag = false;
            return APP_REDRAW_PAGE;
        }
        int target = book_toc_hit(x, y, ctx->leaf, book_navigation_count());
        if (target == BOOK_TOC_BACK) {
            s_message[0] = 0;
            set_reader_view(READING);
            s_reader_panel = READER_PANEL_NONE;
            return APP_REDRAW_PAGE;
        }
        if (target == BOOK_TOC_PREV || target == BOOK_TOC_NEXT ||
            target == BOOK_TOC_SKIP_PREV || target == BOOK_TOC_SKIP_NEXT) {
            int step = target == BOOK_TOC_SKIP_PREV || target == BOOK_TOC_SKIP_NEXT ? 10 : 1;
            int next = ctx->leaf + (target == BOOK_TOC_PREV || target == BOOK_TOC_SKIP_PREV ? -step : step);
            if (next < 0) next = 0;
            if (next >= leaves()) next = leaves() - 1;
            if (next != ctx->leaf) { ctx->leaf = next; return APP_REDRAW_PAGE; }
            return APP_REDRAW_NONE;
        }
        if (target == BOOK_TOC_JUMP && book_navigation_count()) {
            s_toc_jump_open = true;
            s_toc_jump_percent = (ctx->leaf + 1) * 100 / leaves();
            return APP_REDRAW_PAGE;
        }
        if (target >= 0 && !s_message[0]) {
            size_t chapter = book_navigation_chapter((size_t)target);
            if (chapter >= book_chapter_count()) return APP_REDRAW_NONE;
            save_progress();
            set_reader_view(READING);
            if (load_chapter_at(ctx, chapter, 0, false, book_navigation_anchor((size_t)target),
                                book_navigation_source_offset((size_t)target))) {
                s_selected_toc = (size_t)target;
                char entry_title[sizeof(s_title)];
                if (book_navigation_title((size_t)target, entry_title, sizeof(entry_title)) == ESP_OK)
                    copy_text(s_title, sizeof(s_title), entry_title);
                s_view = READING;
                s_reader_panel = READER_PANEL_NONE;
                save_progress();
            } else set_reader_view(s_text ? TOC : SHELF);
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_view == MANAGE) return manage_action(ctx, x, y);
    if (s_view == SEARCH) {
        if (y < 160 && x < 160) { search_finish(ctx, false); return APP_REDRAW_PAGE; }
        if (ui_text_input_tap(&s_search_input, (EpdRect){UI_MARGIN, 200, ui_content_width(), 88},
                              UI_PX_BODY, false, NULL, x, y)) return editor_paint(ctx, true);
        if (ui_rect_hit((EpdRect){36, 392, 180, 66}, x, y)) { s_search_draft[0] = 0; ui_text_edit_init(&s_search_input, s_search_draft, sizeof(s_search_draft)); ui_keyboard_begin(&s_search_input, false); return APP_REDRAW_PAGE; }
        ui_keyboard_result_t result = ui_keyboard_tap(x, y, 560, ctx->now_ms);
        if (result == UI_KEYBOARD_DONE || (y < 160 && x > 510 && !ui_keyboard_pending())) { search_finish(ctx, true); return APP_REDRAW_PAGE; }
        if (result == UI_KEYBOARD_CHANGED) return editor_paint(ctx, false);
        return APP_REDRAW_NONE;
    }
    if (s_view == READING) {
        if (s_toolbar && !(s_reader_panel == READER_PANEL_TOOLS && y < 1096 && ui_rect_hit(body_rect(), x, y)))
            return reader_panel_action(ctx, x, y);
        if (!s_reader_fullscreen && ui_rect_hit((EpdRect){32, 75, 64, 64}, x, y))
            return reader_return(ctx);
        if (!s_reader_fullscreen && x >= reader_favorite_x() - 21 && y >= 76 && y < 145) {
            bool next = !s_reader_favorite;
            if (!favorite_save(s_path, next)) {
                copy_text(s_reader_notice, sizeof(s_reader_notice), "收藏保存失败");
                s_reader_notice_until = esp_timer_get_time() / 1000 + 1800;
                return APP_REDRAW_PAGE;
            }
            s_reader_favorite = next;
            for (int i = 0; i < s_count; ++i)
                if (!strcmp(s_shelf[i].path, s_path)) s_shelf[i].favorite = next;
            sort_shelf(ctx);
            render(ctx, ctx->fb);
            s_area = (EpdRect){584, 76, 64, 70};
            s_mode = MODE_GL16;
            return APP_REDRAW_AREA;
        }
        // 通栏插图铺满整屏，点在图两侧的边条上同样要翻页；文字页仍按正文栏判定。
        // A full-bleed illustration spans the panel, so taps on its side strips must turn pages
        // too; text pages keep the body column.
        EpdRect tap_area = body_rect();
        if (reader_image_slots(s_page)) tap_area = ui_rect_union(tap_area, book_layout_image_rect());
        if (!ui_rect_hit(tap_area, x, y)) return APP_REDRAW_NONE;
        int target = book_reader_tap_feed(&s_reader_tap, tap_area, x, y,
                                         app_settings_reader_vertical_turn(),
                                         app_settings_book_vertical(), ctx->now_ms);
        if (target == BOOK_READER_TAP_FULLSCREEN) return toggle_reader_fullscreen(ctx);
        if (target) return turn_page(ctx, target);
        return APP_REDRAW_NONE;
    }
    if (s_view == SHELF) {
        if (s_view == SHELF) {
            int tab = ui_nav_hit(x, y);
            if (tab >= 0) { ui_nav_request(ctx, tab); return APP_REDRAW_NONE; }
            if (ui_rect_hit(shelf_page_arrow_rect(-1), x, y)) return shelf_turn_page(ctx, -1);
            if (ui_rect_hit(shelf_page_arrow_rect(1), x, y)) return shelf_turn_page(ctx, 1);
        }
        if (s_view == SHELF && ui_rect_hit(shelf_excerpt_rect(), x, y)) {
            // P1-2 / P0-5 入口甲：书架头部「摘抄」按钮——队列空是入口，队列非空是
            // 全局出口的徽标（待落盘 N 在摘抄本顶部一节可见）。
            // Entrance A: the header button opens the excerpt book; with pending
            // entries it is the global exit and the pending section shows on top.
            return open_excerpt_view(ctx, false);
        }
        if (s_view == SHELF && ui_rect_hit(shelf_import_rect(), x, y)) {
            s_view = IMPORT;
            return APP_REDRAW_PAGE;
        }
        if (s_view == SHELF && ui_rect_hit(shelf_manage_rect(), x, y)) {
            clear_selection();
            s_batch_confirm = false;
            s_batch_message[0] = 0;
            s_view = BULK;
            ctx->leaf = 0;
            return APP_REDRAW_PAGE;
        }
        if (s_view == SHELF && s_save_failed && y >= UI_CONTENT_BOTTOM - UI_PX_CAPTION && y < UI_CONTENT_BOTTOM) {
            retry_progress();
            return APP_REDRAW_PAGE;
        }
    }
    if (s_message[0] && !((s_view == SHELF || s_view == BULK) && s_visible_count)) return APP_REDRAW_NONE;
    int rows = s_view == BULK ? BOOK_BULK_ROWS : shelf_rows();
    for (int row = 0; row < rows; ++row) if (ui_rect_hit(row_rect(row), x, y)) {
        int i = ctx->leaf * rows + row;
        if (s_view == SHELF && i < s_visible_count) {
            shelf_category_remember(ctx);
            if (s_shelf[i].removed) {
                s_managed = s_shelf[i]; s_view = MANAGE; s_file_removed = s_delete_confirm = true; s_clear_confirm = false;
                copy_text(s_manage_message, sizeof(s_manage_message), "文件已删除，进度清理失败，请重试");
            } else open_book(ctx, s_shelf[i].path);
        }
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}
static void on_enter(app_ctx_t* ctx) {
    shelf_category_init();
    s_category_menu=false;
    s_reader_tap.pending = false;
    s_shelf_page_pending = false;
    // 设置页试按的旧事件不得在开书后翻页。/ Discard remote tests from earlier pages before opening a book.
    ble_pt_event_t old_key;
    ble_pt_raw_t old_raw;
    while (ble_pt_pop_key(&old_key)) {}
    while (ble_pt_pop_raw(&old_raw)) {}
    book_layout_set_spacing(app_settings_book_line_spacing(), app_settings_book_paragraph_spacing());
    book_layout_set_images_visible(!app_settings_reader_hide_images());
    // 插图出血宽不在此发布：所有权已收归建版单出口 reader_relayout_cols（G7-A1 修复）——
    // 通栏语义随栏数快照同批驱动（单栏整屏宽、双栏栏宽），进页只设排版参数。
    // The illustration bleed width is not published here: ownership moved to the single
    // layout exit reader_relayout_cols (G7-A1 fix) so full-bleed semantics ride the column
    // snapshot (panel width at one column, column width at two); entry sets typography
    // knobs only.
    // 竖排是阅读设置里的手动开关，进页时同步给排版；关着就是原来的横排。
    // Vertical setting is a manual reader switch, published to the layout on entry; off is the
    // original horizontal setting.
    book_layout_set_vertical(app_settings_book_vertical());
    book_layout_set_typography(((int)app_settings_book_tracking() - 2) * 2);
    book_layout_set_first_line_indent(app_settings_book_indent());
    book_layout_set_first_line_indent_adjust(app_settings_book_indent_adjust());
    book_layout_set_reading_line(app_settings_book_reading_line());
    book_layout_set_reading_line_offset(app_settings_book_reading_line_offset());
    // 字体页可能刚改过书内字体开关；回正文时重排当前章，别让用户对着旧排版猜。
    // The font page may have just changed the embedded-face switch; re-typeset this chapter
    // so the reader is not left guessing at stale type.
    bool book_fonts = app_settings_book_fonts();
    if (book_fonts != s_book_fonts_applied) {
        s_book_fonts_applied = book_fonts;
        book_set_embedded_fonts(book_fonts);
        if (s_text && book_kind() == BOOK_KIND_EPUB)
            (void)load_chapter(ctx, s_chapter, book_layout_page_start_offset(s_page), false);
    }
    s_reader_fullscreen = false;
    s_bookmark_edit = s_bookmark_delete_confirm = s_bookmark_delete_error = false;
    s_bookmark_selected = 0;
    clear_selection();
    s_view = s_requested_manage ? BULK : SHELF;
    s_reader_return_home = false;
    s_requested_manage = false;
    s_presented_view = -1;
    ctx->leaf = s_view==SHELF && s_categories ? s_categories[s_category_selected].pages[app_settings_shelf_style()==5] : 0;
    s_px = app_settings_book_px();
    s_recent_sort = app_settings_shelf_recent_sort();
    s_margin = app_settings_book_margin();
    s_line_spacing = app_settings_book_line_spacing();
    // 传书页已停止并 join HTTP，安全注销且只丢弃对应路径的旧进度。
    // Transfer has stopped and joined HTTP; safely unregister only invalidated paths.
    pending_drop_invalidated();
    s_reader_panel = READER_PANEL_NONE;
    dict_popup_reset();
    // 预热词典：把「首次长按需付的打开开销」挪到进阅读页时；失败不设退避（见 dict_warm）。
    // Warm the dictionary on entry so the first long press does not pay for opening it.
    if (!dict_ready()) {
        const int64_t warm_start = esp_timer_get_time();
        const bool warmed = dict_warm();
        ESP_LOGI(TAG, "dict warm ok=%d ms=%lld", (int)warmed,
                 (long long)((esp_timer_get_time() - warm_start) / 1000));
    }
    // 生词本索引同样预热，把读卡开销挪出查词路径（失败会退化成保存时扫文件）。
    // Warm the vocabulary index too, keeping the card read off the lookup path.
    vocab_log_warm();
    s_reader_slider = -1;
    s_clear_confirm = s_batch_confirm = false;
    s_pressed_control = -1;
    s_du_count = 0;
    s_shelf_feedback_pending = false;
    s_cover_batch_present_pending = false;
    s_reader_cleanup = false;
    s_poll_ms = 0;
    read_pico_sd_info_t sd = {0};
    esp_err_t media_err = read_pico_sd_get_info(&sd);
    bool reuse = s_shelf_cache_valid && media_err != ESP_ERR_NOT_FINISHED &&
                 sd.present == s_cache_sd_present && sd.mounted == s_cache_sd_mounted &&
                 s_cache_shelf_style == app_settings_shelf_style() &&
                 s_store_revision == book_store_revision();
    s_scan_pending = !reuse;
    if (reuse) {
        refresh_cached_progress(ctx);
        (void)open_requested_book(ctx);
    } else {
        s_count = s_visible_count = 0;
        s_message[0] = 0;
        s_storage[0] = 0;
        invalidate_covers();
        // 已指定文件可直接打开；书架目录留到真正进入书架时再扫描。
        // Open a requested file directly; scan the shelf only when it is actually shown.
        if (open_requested_book(ctx)) {
            s_scan_pending = false;
            return;
        }
        read_pico_sd_start_probe();
    }

}
static void book_on_exit(app_ctx_t* ctx) {
    /* gen30 步 3 · 状态卫生（任务书 7 项之外的一格，回执报备）：离开阅读器即退伴读，
       避免 s_reader_companion 与 FAIR 闩锁跨 app 残留（否则 BLE 会一直停在 15ms）。
       不动 s_sess / 广播 / 连接（格 2 终裁），不重排（书即将释放）。
       gen30 step 3 state hygiene (one extra item beyond the 7-item brief, reported
       in the receipt): leaving the reader exits companion mode so neither the state
       nor the FAIR latch survives across apps (otherwise BLE stays at 15 ms). No
       session/advertising/connection teardown (grid 2); no reflow (book is leaving). */
    s_companion_enter_pending = s_companion_exit_pending = false;
    if (reader_companion_active()) {
        reader_companion_set(false);
        if (push_sink_link_up()) (void)push_sink_tts_send_page("", 0);
        push_sink_companion_conn(false);
        s_tts_last_chapter = s_tts_last_page = SIZE_MAX;
    }
    boxsel_boundary_reset();
    // M3-B：离页即撤掉情景待命（第三个清零点，另两处是新点词与发送失败）。
    // 留着的等待单会在别的页面收到答案，那时 s_page 已不代表用户眼前的内容。
    // M3-B: leaving the page drops any pending scene request (the third clear point,
    // alongside a new tap and a failed send). A request left waiting would claim an
    // answer on some other page, where s_page no longer means what the user sees.
    scene_req_disarm(&s_scene_req);
    book_cover_jobs_cancel(s_cover_generation);s_cover_generation=0;
    shelf_category_remember(ctx);
    s_category_menu=false;
    (void)ctx;
    // 惊喜2 反向通道：离开的若是手机推来的虚拟书，就 notify EVT_READ(0x14)——
    // 「推→读→已读」连招的最后一环，手机收到后清剪贴板。放在退出处而非翻开处：
    // 读没读完只有退出这一刻才说得上，且不影响阅读页任何既有路径。
    // Surprise 2's reverse channel: if the book being left is the phone-pushed
    // virtual book, notify EVT_READ(0x14) — the last step of the
    // push->read->read chain, which clears the phone's clipboard. Placed on exit
    // rather than on open: "finished reading" is only knowable at exit, and no
    // existing reader path is touched.
    if (s_path[0] && strcmp(s_path, PUSH_TEXT_PATH) == 0) push_sink_notify_read();
    // gen19（真机 Bug3）：离开 push.txt 不再收摊——包括单词卡/空数据回合。
    // 真机实证：单词卡在 book app 渲染，退出走了旧 cancel（`armed canceled by
    // 退出阅读器，接收结束`），回首页状态卡直接「未开启」，与长文交接路径
    // 行为分裂。保活语义统一：窗口到期/系统级入口收摊，离页不收。
    // gen19 (real-hw Bug3): leaving push.txt no longer tears the session down -
    // including word-card/empty-payload rounds. Real-hw proof: the word card
    // renders inside the book app, so its exit hit the old cancel ("armed
    // canceled by reader exit") and the home card flipped to "off", diverging
    // from the long-text handoff path. Unified semantics: the window expiry
    // and system-level entries close up; leaving a page never does.
    if (s_path[0] && strcmp(s_path, PUSH_TEXT_PATH) == 0) {
        s_push_handoff = false; // 交接标记照旧清位，防旧标记过夜 / clear the stale handoff flag as before
    }
    // 离开阅读即退出全工程唯一的横屏语境：物理旋转若停在 LANDSCAPE，后续任何
    // 页面（Home/菜单/其他 app）都按竖屏绘制——真机实证长按退出后 Home 竖屏
    // 布局画在横屏物理屏上（半屏错位）。这里兜底复位；唯一一次刷新由 app_loop
    // 随后的 present 完成（旋转已归位，竖屏 framebuffer 全量重画后差分收敛），
    // 不在此处自行刷新。与 prepare_lock 的复位互为两面：本处覆盖 app 间切换，
    // 那处覆盖不经切换的系统画面（锁屏/睡眠/关机）。
    // Leaving the reader exits the only landscape context in the tree: if the
    // panel is still LANDSCAPE, every following page (Home/menu/other apps)
    // draws portrait — observed on hardware as a half-screen Home after a
    // long-press exit. Reset here as the fallback; app_loop's subsequent
    // present performs the single refresh (the portrait framebuffer repaints
    // fully and the diff converges), so no extra refresh here. This and
    // prepare_lock's reset are two faces of one boundary: this one covers
    // app-to-app switches, that one covers system frames without a switch
    // (lock/sleep/shutdown).
    if (epd_get_rotation() == EPD_ROT_LANDSCAPE) epd_set_rotation(EPD_ROT_INVERTED_PORTRAIT);
    s_reader_tap.pending = false;
    s_shelf_page_pending = false;
    ui_keyboard_end();
    s_presented_view = -1;
    free(s_editor_cover); s_editor_cover = NULL;
    s_pressed_control = -1;
    s_reader_slider = -1;
    save_progress();
    s_reader_return_home = false;
    ui_reader_background_release();
    free(s_backgrounds); s_backgrounds = NULL; s_background_count = 0;
    free_book(); // 内部已请求停机（自删形态），离页前再要一次，幂等 / already requests the stop (self-delete shape); asking again before leaving is idempotent
    free(s_next_fb);
    s_next_fb = NULL;
    // gen25：与 free_book 同一协议——只请求、不强删。free_book 已经请求过一次，
    // 这里再请求一次是幂等的（s_prep_task 为空时直接返回）。
    // Gen25: same protocol as free_book - request, never force-delete. free_book
    // already asked once; asking again is idempotent (a NULL s_prep_task returns
    // immediately).
    prep_task_request_stop("exit");
    // 任务若因超时未停（仍在跑），它仍握着 s_prep_done / s_draw_lock 的句柄——
    // 此时删信号量会给它留下一对悬挂句柄。只有确认已停（句柄被任务自己置空）
    // 才回收信号量；未停就留着，等它自查退出。
    // If the task did not stop (timeout, still running) it still holds the
    // s_prep_done / s_draw_lock handles - deleting them now would leave it two
    // dangling handles. Reclaim the semaphores only once it is confirmed stopped
    // (the task cleared its own handle); otherwise leave them for its own exit.
    if (!s_prep_task) {
        if(s_prep_done){vSemaphoreDelete(s_prep_done);s_prep_done=NULL;}
        if (s_draw_lock) { vSemaphoreDelete(s_draw_lock); s_draw_lock = NULL; }
    }
    dict_popup_reset();
    dict_close();
    vocab_log_invalidate();
}
// 先停止使用旧卡句柄与字体预渲染，主循环随后切换内置字体。
// Stop old-card handles and font preparation before the loop switches to the builtin font.
// ---- 31b 第 2 条 B′：掉盘就地重排 ----------------------------------------
// 三条不变量，后面每一行都服务其中一条：
//   ① 锚点是**字节偏移**，不是页号——字面变了，页表全换，页号没有跨字面的意义；
//   ② 页脚文案位固定，「TF 卡已移除 · 待落盘 N」不许被章节名挤掉；
//   ③ 任何要读卡的动作在掉盘态一律拒绝并给出「存储卡已移除」，不静默失败。
// Three invariants; every line below serves one:
//   1) the anchor is a byte offset, not a page number - the face changes, the page table
//      is rebuilt, and a page number carries no meaning across two typesettings;
//   2) the footer slot is fixed and never yields to a chapter title;
//   3) anything that reads the card is refused out loud, never silently.
static void reader_offline_refuse(void) {
    copy_text(s_reader_notice, sizeof(s_reader_notice), READER_OFFLINE_TAIL);
    s_reader_notice_until = esp_timer_get_time() / 1000 + 2200;
}

/// 重排把页表整张换掉，位图缓存要重新绑到新页号与新布局代次上，否则
/// `draw_reader_images` 的代次守卫会一直拒绝——用户看到的是「图突然没了」。
/// The reflow replaces the whole page table, so the bitmap cache is re-bound to the new
/// page and layout generation; otherwise draw_reader_images' generation guard keeps
/// refusing and the illustration silently disappears.
static void reader_offline_rebind_images(void) {
    if (!s_page_images || s_page_image_count <= 0) return;
    if (reader_image_slots(s_page) <= 0) { release_page_images(); return; }
    s_page_images_for = s_page;
    s_page_images_generation = book_layout_generation();
}

/// 拔卡且正在阅读：留在阅读器，按字节偏移重排当前页。成功返回 true。
/// Card gone while reading: stay in the reader and re-typeset the current page from its
/// byte offset. Returns true when the reader stays up.
static bool book_enter_offline(app_ctx_t *ctx) {
    // 锚点必须最先取：重排一发生，旧页表就没了。
    // The anchor comes first: once the reflow starts, the old page table is gone.
    const size_t anchor = reader_page_offset(s_page);
    // 快照必须早于 book_close()——那之后书籍元数据被 memset 清零。
    s_off_chapters = book_chapter_count();
    s_off_total_bytes = book_total_bytes();
    s_off_chapter_offset = book_chapter_byte_offset(s_chapter);
    lock_draw();
    // 进度先记进内存 pending；落卡必然失败，`未保存` 的语义因此是诚实的。
    save_progress();
    invalidate_prep();
    s_reader_tap.pending = false;
    // 预排版任务等的是卡上的下一章，卡没了它只会空转失败；先请它退（只请求，不强删）。
    prep_task_request_stop("media-lost");
    s_prep_page = -1; s_next_page = -1; s_next_generation = 0;
    memset(&s_prep_cursor, 0, sizeof(s_prep_cursor));
    // 句柄必须在 app_loop 卸载卷之前关掉：顺序反了就是悬空句柄。
    book_close();
    // 卡上的字体已经读不到：切内置字面后再重排，否则排出来的是马上要换掉的字面。
    // app_loop 在回调返回后也会调一次，那次将成为 no-op（幂等）。
    if (ttf_font_ready() && !ttf_font_is_builtin()) ttf_font_open_builtin();
    ui_text_set_system_font(false);
    // 先置位再重排：重排链上任何一步想读卡都会被拦住。
    s_media_offline = true;
    bool ok = s_text && s_text_len && reader_relayout(body_rect());
    if (!ok) {
        // 内存不够重排：只能退回书架。这是 B′ 唯一允许跳书架的出口，必须留痕。
        ESP_LOGE(TAG, "offline relayout failed (text=%p len=%u): falling back to the shelf",
                 (void *)s_text, (unsigned)s_text_len);
        s_media_offline = false;
        release_page_images();
        unlock_draw();
        return false;
    }
    size_t count = book_layout_page_count();
    size_t mapped = book_layout_page_for_offset(anchor);
    if (count && mapped >= count) mapped = count - 1;
    if (count) s_page = reader_clamp_visual_page(mapped);
    s_jump_offset = SIZE_MAX;
    reader_offline_rebind_images();
    // 面板/词典/词汇索引都挂在卡上：一并收掉，避免留一个点到就崩的面板。
    s_reader_panel = READER_PANEL_NONE;
    dict_popup_reset();
    dict_close();
    vocab_log_invalidate();
    s_reader_slider = -1;
    s_clear_confirm = s_batch_confirm = false;
    s_pressed_control = -1;
    // 顶部消息不占正文：页脚已经承担了「卡没了」这件事，正文位置还给用户读书。
    s_message[0] = 0;
    copy_text(s_storage, sizeof(s_storage), READER_OFFLINE_NOTE);
    ESP_LOGW(TAG, "offline reading: anchor=%u -> page %u/%u (chapter %u/%u)",
             (unsigned)anchor, (unsigned)reader_visual_index(s_page) + 1,
             (unsigned)reader_visual_count(), (unsigned)s_chapter + 1, (unsigned)s_off_chapters);
    unlock_draw();
    return true;
}

/// 插回卡：清掉盘态并把书重新打开到同一章同一字节偏移。
/// Card back: clear the offline state and reopen the book at the same chapter and offset.
static void book_leave_offline(app_ctx_t *ctx) {
    if (!s_media_offline) return;
    const size_t anchor = reader_page_offset(s_page);
    s_media_offline = false;
    release_page_images();
    // 恢复用户字体选择（拔卡时被迫切了内置字面，不回切就是偷偷改了他的设置）。
    // Restore the user's face: it was forced to the builtin on removal, and not switching
    // back would quietly overwrite their choice.
    app_font_activate_reading();
    if (!s_path[0]) { s_view = SHELF; s_scan_pending = true; return; }
    // ---- 书源必须**先重开**（26.11.1 真机「插卡被踢回书架」的根因）------------
    // `book_enter_offline()` 在掉盘那一刻调了 `book_close()`——句柄不能跨卷卸载存活，
    // 否则就是悬空 FILE*。它同时把 `s_book.file` 关掉、`s_book.count` 归零。这里若不
    // 重开就直接 `load_chapter()`，`book_chapter_load()` 的 `!s_book.file` 立刻返回
    // `ESP_ERR_INVALID_ARG`，于是走「加载失败 → set_reader_view(SHELF)」——用户看到的
    // 就是插回卡的瞬间被踢回书架。
    // 真机日志实证（26.11.1 merged，t=300679 / 139446 两次同形）：
    //     sd: err=ESP_OK present=1 mounted=1   ← 挂载成功
    //     book: chapter load failed: ESP_ERR_INVALID_ARG
    //     book: shelf books=18 roots=2         ← 回书架重扫
    // The source was closed on removal; reopening is a prerequisite, not an optimisation.
    if (book_open(s_path) != ESP_OK) {
        ESP_LOGE(TAG, "reopen after media-ready failed (%s): falling back to the shelf", s_path);
        copy_text(s_message, sizeof(s_message), "存储卡内容已变化，请重新打开");
        set_reader_view(SHELF);
        s_scan_pending = true;
        return;
    }
    // 换了一张卡 / 内容被改过：章节数或总字节对不上，锚点就失去意义——回该章开头，
    // 不拿旧偏移去跳一本已经不是那本书的书。
    // A different card (or an edited file) invalidates the anchor: restart the chapter.
    size_t chapter = s_chapter;
    if (chapter >= book_chapter_count())
        chapter = book_chapter_count() ? book_chapter_count() - 1 : 0;
    const bool same_book = s_off_chapters && book_chapter_count() == s_off_chapters &&
                           book_total_bytes() == s_off_total_bytes;
    if (!load_chapter(ctx, chapter, same_book ? anchor : 0, false)) {
        set_reader_view(SHELF);
        s_scan_pending = true;
        return;
    }
    s_scan_pending = false;
}

static void book_on_media_lost(app_ctx_t* ctx) {
    boxsel_boundary_reset();
    shelf_category_remember(ctx);
    s_category_menu=false;
    s_reader_tap.pending = false;
    // ---- 出口 A：正在阅读 → B′ 就地重排（31b 第 2 条）----------------------
    // 这里是「拔卡瞬间跳书架」的结清点：只要正文还在 RAM，就没有理由把用户踢出书。
    // Exit A: still reading -> B′ re-typesets in place. This is where "card removal
    // bounces you to the shelf" is settled: while the body text is in RAM there is no
    // reason to throw the reader out of the book.
    if (s_view == READING && s_text && s_text_len && book_enter_offline(ctx)) return;
    // ---- 出口 B：其他视图 / 重排失败 → 原有清场 ---------------------------
    // Exit B: any other view, or the reflow failed -> the original teardown.
    lock_draw();
    save_progress();
    free_book();
    unlock_draw();
    free(s_editor_cover); s_editor_cover = NULL;
    s_view = SHELF;
    s_count = s_visible_count = 0;
    s_shelf_cache_valid = false;
    free(s_shelf); s_shelf = NULL; s_shelf_capacity = 0;
    invalidate_covers();
    ctx->leaf = 0;
    s_reader_panel = READER_PANEL_NONE;
    s_reader_slider = -1;
    s_clear_confirm = s_batch_confirm = false;
    s_pressed_control = -1;
    s_scan_pending = true;
    s_du_count = 0;
    dict_popup_reset();
    dict_close(); // 词典句柄来自失效的卡 / the dictionary file came from the lost card
    vocab_log_invalidate(); // 索引指向的卡已经没了 / the index points at a card that is gone
    copy_text(s_storage, sizeof(s_storage), "TF 卡已移除");
    copy_text(s_message, sizeof(s_message), "TF 卡已移除");
}
static void book_on_media_ready(app_ctx_t* ctx) {
    s_shelf_cache_valid = false;
    // B′：插回卡就把书重新打开到掉盘时读到的那一章、那一个字节偏移。
    // 用户在这段时间里读过的页由 `s_page` 的版面位置给出，不是回到拔卡前的旧页。
    if (s_media_offline) { book_leave_offline(ctx); return; }
    if (s_view == SHELF) s_scan_pending = true;
}
// 控件编号仅用于保持按下与抬起命中同一个目标。/ IDs pair a press with release on the same control.
static int control_at(app_ctx_t* ctx, uint16_t x, uint16_t y, EpdRect* rect) {
    // 26.11.3：这里也曾是同一个匿名框的副本——按下落在这块就拿不到控件号，
    // 于是按钮"按下去没反应、抬起来去弹菜单"。同一个盒子两个副本，一起收口。
    // 26.11.3: this was the same anonymous box's second copy - a press landing here got
    // no control id, so the button "pressed nothing, released into the dropdown".
    if (s_view==SHELF && (s_category_menu ||
        (ui_rect_hit(shelf_category_tap_rect(),x,y) && !shelf_header_button_hit(x,y)))) return -1;
    if (s_view == BULK) return bulk_control_at(ctx, x, y, rect);
    if (s_view == MANAGE) {
        *rect = manage_back_rect();
        if (ui_rect_hit(*rect, x, y)) return 711;
    }
    if (s_view == IMPORT) {
        *rect = (EpdRect){36, 79, 58, 58};
        if (ui_rect_hit(*rect, x, y)) return 700;
        for (int i = 0; i < 4; ++i) {
            *rect = import_rect(i);
            if (ui_rect_hit(*rect, x, y)) return 701 + i;
        }
        int tab = ui_nav_hit(x, y);
        if (tab >= 0) { *rect = (EpdRect){tab * 171, UI_NAV_TOP, 171, 120}; return 705 + tab; }
        return -1;
    }
    if (s_view == SEARCH) {
        for (int i = 0; i < 45; ++i) { *rect = search_rect(i); if (ui_rect_hit(*rect, x, y)) return 500 + i; }
        return -1;
    }
    if (s_view == MANAGE) {
        if (!s_clear_confirm && !s_file_removed) {
            *rect = (EpdRect){UI_MARGIN, manage_panel().y + manage_panel().height - 274, ui_content_width(), 68};
            if (ui_rect_hit(*rect, x, y)) return 405;
            *rect = (EpdRect){UI_MARGIN, manage_panel().y + manage_panel().height - 190, ui_content_width(), 68};
            if (ui_rect_hit(*rect, x, y)) return 404;
        }
        int count = s_clear_confirm || s_file_removed ? 2 : 3;
        for (int i = 0; i < count; ++i) {
            *rect = manage_rect(i, count);
            if (ui_rect_hit(*rect, x, y)) return s_clear_confirm ? 300 + i : s_file_removed && i == 1 ? 403 : 400 + i;
        }
        return -1;
    }
    if (s_clear_confirm) {
        for (int i = 0; i < 2; ++i) {
            *rect = ui_row_rect(i, 2, 610, UI_BTN_H);
            if (ui_rect_hit(*rect, x, y)) return 300 + i;
        }
        return -1;
    }
    if (s_view == READING) {
        if (s_reader_panel == READER_PANEL_TOOLS) for (int i = 0; i < BOOK_TOOL_COUNT; ++i) {
            *rect = tool_rect(i);
            if (ui_rect_hit(*rect, x, y)) return 200 + i;
        }
        return -1;
    }
    if (s_view != SHELF) for (int i = 0; i < 3; ++i) {
        *rect = ui_bar_rect(i, 3);
        if (ui_rect_hit(*rect, x, y)) return 100 + i;
    }
    if (s_view == SHELF && ui_rect_hit(shelf_excerpt_rect(), x, y)) { *rect = shelf_excerpt_rect(); return 118; }
    if (s_view == SHELF && ui_rect_hit(shelf_manage_rect(), x, y)) { *rect = shelf_manage_rect(); return 114; }
    if (s_view == SHELF && ui_rect_hit(shelf_import_rect(), x, y)) { *rect = shelf_import_rect(); return 115; }
    if (s_view == SHELF) {
        for (int i = 0; i < 2; ++i) {
            *rect = shelf_page_arrow_rect(i ? 1 : -1);
            if (ui_rect_hit(*rect, x, y)) return 116 + i;
        }
    }
    if (s_message[0] && !((s_view == SHELF || s_view == BULK) && s_visible_count)) return -1;
    int rows = s_view == BULK ? BOOK_BULK_ROWS : shelf_rows();
    int count = s_visible_count;
    for (int row = 0; row < rows && ctx->leaf * rows + row < count; ++row) {
        *rect = row_rect(row);
        if (ui_rect_hit(*rect, x, y)) return row;
    }
    return -1;
}
static app_redraw_t paint_control(app_ctx_t* ctx, EpdRect rect) {
    if (s_view == SHELF && app_settings_shelf_style() == 5) {
        for (int row = 0; row < shelf_rows(); ++row) {
            EpdRect target = row_rect(row);
            if (memcmp(&rect, &target, sizeof(rect))) continue;
            s_area = paint_list_cover_feedback(ctx, row);
            s_mode = app_settings_main_fast_refresh() ? MODE_DU : MODE_GL16;
            s_shelf_feedback_pending = true;
            return APP_REDRAW_AREA;
        }
    }
    // 小按钮反馈只重画按钮，不重新绘制全部封面；其余装饰沿用原绘制路径。
    // 26.11.3：三处矩形不再各写一份裸字面量——这是同一个盒子的第 4 个副本，
    // 副本不同步正是本批两个缺陷的共同病根（绘制 16 / 命中 388 / 断言漏写）。
    // Repaint small header buttons alone rather than every cover; other decorations retain
    // their path. 26.11.3: no more raw literals here - a fourth copy of the same boxes is
    // exactly how the two defects of this batch were born.
    // 注意：这里的注释不许出现撇号——ui_refresh_feedback_host_test.py 的 function()
    // 按引号配平提取函数体，注释里的 ' 会把后面整段吞成字符串（26.11.3 咬过一次）。
    // Note: no apostrophes in this comment - the host test extracts function bodies with a
    // quote-aware brace matcher, and a stray ' swallows the rest of the file.
    const EpdRect ex_btn = shelf_excerpt_rect();
    const EpdRect mg_btn = shelf_manage_rect();
    const EpdRect im_btn = shelf_import_rect();
    if (s_view == SHELF && !memcmp(&rect, &ex_btn, sizeof(rect))) {
        char excerpt_label[24];
        draw_shelf_header_button(ctx->fb, rect, 118,
                                 shelf_excerpt_label(excerpt_label, sizeof(excerpt_label)), 20);
    }
    else if (s_view == SHELF && !memcmp(&rect, &mg_btn, sizeof(rect)))
        draw_shelf_header_button(ctx->fb, rect, 114, "管理", 20);
    else if (s_view == SHELF && !memcmp(&rect, &im_btn, sizeof(rect)))
        draw_shelf_header_button(ctx->fb, rect, 115, "+ 导入", 20);
    else render(ctx, ctx->fb);
    s_mode = MODE_DU;
    s_shelf_feedback_pending = false;
    if (s_view == SHELF) {
        for (int row = 0; row < shelf_rows(); ++row) {
            EpdRect target = row_rect(row);
            if (rect.x != target.x || rect.y != target.y ||
                rect.width != target.width || rect.height != target.height) continue;
            // 覆盖抽出前后的并集，释放或取消时也擦净上方的旧封面。
            // Refresh both book positions so release or cancellation clears the lifted edge.
            rect.y -= SHELF_BOOK_LIFT_PX;
            rect.height += SHELF_BOOK_LIFT_PX;
            s_mode = MODE_GL16;
            s_shelf_feedback_pending = true;
            break;
        }
    }
    s_area = rect;
    return APP_REDRAW_AREA;
}
// 左右滑动始终可用；上下点按模式另外保留上下滑动，不改变中间轻点切换全屏。
// Horizontal swipes work in both tap modes; vertical mode also keeps vertical swipes.
// 从屏幕底部120px上滑呼出工具栏；其他面板保留自己的滑动，正文中部仍按翻页模式解释。
// Open tools from the bottom 120px; other panels keep their own swipes and mid-body gestures retain turn semantics.
static bool reader_tools_swipe(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    if (s_view != READING || !s_text || s_reader_panel != READER_PANEL_NONE ||
        s_clear_confirm || s_scan_pending || ctx->consumed || ev->type != UI_GESTURE_SWIPE_U ||
        ev->x0 >= UI_LOCK_WIDTH || ev->y0 < UI_LOCK_HEIGHT - 120 || ev->y0 >= UI_LOCK_HEIGHT) return false;
    s_reader_tap.pending = false;
    s_pressed_control = s_reader_slider = -1;
    s_reader_panel = READER_PANEL_TOOLS;
    s_stats_activity_ms = ctx->now_ms;
    ctx->user_activity = true;
    invalidate_prep();
    return true;
}
static int reader_swipe_direction(ui_gesture_type_t type, bool vertical) {
    int direction = 0;
    if (type == UI_GESTURE_SWIPE_L || (vertical && type == UI_GESTURE_SWIPE_U)) direction = 1;
    else if (type == UI_GESTURE_SWIPE_R || (vertical && type == UI_GESTURE_SWIPE_D)) direction = -1;
    if (direction && !vertical && app_settings_book_vertical()) direction = -direction;
    return direction;
}

// M3-B：情景卡「存生词 ›」。写入的**只有「词 + 本地词典释义」**——标准生词本
// 形态；四节情景分析不进生词本（审计裁定的内容边界）。词典没收录就不写，如实
// 留日志：宁可不做，也不写半条。
// M3-B: the scene card's "save word". It writes ONLY the word plus its local
// dictionary definition - the standard vocabulary shape. The four-section scene
// analysis never enters the vocabulary book (the ruling's content boundary). A word
// the dictionary does not hold is not written; the log says so: better nothing
// than half an entry.
static app_redraw_t scene_save_word(void) {
    char w[DICT_WORD_MAX];
    copy_text(w, sizeof(w), ai_reader_word());
    if (!w[0]) return APP_REDRAW_NONE;
    static char def[DICT_ENTRY_MAX]; // 1280B：同 dict_popup 的查询缓冲 / same as dict_popup's lookup buffer
    if (!dict_lookup_word(w, def, sizeof(def)) || !def[0]) {
        ESP_LOGW(TAG, "scene save \"%s\": not in dictionary, nothing written", w);
        return APP_REDRAW_NONE;
    }
    const bool ok = vocab_log_save(w, def);
    if (ok) ai_reader_set_scene_saved(true);
    ESP_LOGI(TAG, "scene save \"%s\": dict def %u bytes ok=%d", w, (unsigned)strlen(def), (int)ok);
    return ok ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
}

// ===================== 26.11.4 段落视图：存摘抄 =====================
// 同一个 chip、两种语义：词卡存「词 + 词典释义」进生词本（上面那个函数，一字未改），
// 段落存【原文对照】一节进摘抄本。第一版把段落也接到 scene_save_word 上，于是
// 真机日志里留下 6 次 `scene save "选段": not in dictionary` —— 按钮能按、永远不成功。
// One chip, two meanings: the card saves a word to the vocabulary book (unchanged
// above); a passage saves its 原文对照 section to the excerpt book.

/// 选段是不是中文。模板规定**只有英文选文**才出【原文对照】节，所以「没有这一节」
/// 有两义：中文选段的正常形态，或英文模板坏了。按选段语言分流，不猜——猜错的代价
/// 是给一篇没存下的段落亮「已存 ✓」。
/// Whether the selection is Chinese. Only English selections carry the section, so a
/// missing section is either normal (Chinese) or a broken template (English).
static bool excerpt_is_mostly_cjk(const char *s) {
    size_t cjk = 0, latin = 0;
    for (const unsigned char *p = (const unsigned char *)s; p && *p; ++p) {
        if (*p >= 0xE4 && *p <= 0xE9) ++cjk;                                  // CJK 首字节
        else if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) ++latin;
    }
    return cjk > 0 && cjk >= latin;
}

/// 切节缓冲：正文帽 700 B（摘抄行的正文预算，不是行帽 1024——行里还有书名与前缀）。
/// 放 PSRAM：内部 bss 只剩 8,720 B，为一个按钮的临时串吃 701 B 不值。
/// The cut buffer is the excerpt body budget (700 B) and lives in PSRAM: 701 B of
/// internal bss for one button's scratch string is a bad trade.
static EXT_RAM_BSS_ATTR char s_passage_line[EXCERPT_TEXT_MAX + 1u];

static app_redraw_t scene_save_passage(void) {
    const char *def = ai_reader_def();
    const char *quote = ai_reader_excerpt_text();
    if (!def || !*def) return APP_REDRAW_NONE;

    size_t n = 0;
    bool cut = false;
    const excerpt_section_status_t st =
        excerpt_section_pick(def, strlen(def), s_passage_line, sizeof(s_passage_line), &n, &cut);
    const char *body = s_passage_line;

    if (st == EXCERPT_SECTION_NO_MARKER) {
        if (!excerpt_is_mostly_cjk(quote)) {
            // 英文选文本该有对照节：模板坏了，或模型没听话。**诚实失败**——
            // 宁可说「无对照可存」，也不把一篇没有对照的答案存成摘抄。
            // An English selection should have carried the section: the template is
            // broken or the model ignored it. Fail loudly instead of saving a lie.
            reader_notice_set("无对照可存", 2000);
            ESP_LOGW(TAG, "scene save: no %s in an essay-length answer (def %u B, quote %u B)",
                     EXCERPT_SECTION_MARKER, (unsigned)strlen(def), (unsigned)strlen(quote));
            return APP_REDRAW_PAGE;
        }
        // 中文选段：模板不出对照节，兜底存选段原文（天然 ≤1,024 B，落盘按 700 截）。
        // Chinese selection: save the passage itself, the template never had a section.
        body = quote;
        n = strlen(body);
        cut = false;
        if (!n) {
            reader_notice_set("无可存内容", 2000);
            return APP_REDRAW_PAGE;
        }
    } else if (st == EXCERPT_SECTION_EMPTY) {
        reader_notice_set("无对照可存", 2000);
        ESP_LOGW(TAG, "scene save: %s is present but empty", EXCERPT_SECTION_MARKER);
        return APP_REDRAW_PAGE;
    }

    const ai_reader_source_t *src = ai_reader_source();
    bool cut_by_line = false;
    const excerpt_status_t q = excerpt_store_enqueue(
        s_book_title[0] ? s_book_title : (src && src->title[0] ? src->title : "未知"),
        src ? src->page : (size_t)reader_visual_index(s_page), book_now_unix(), body, &cut_by_line);
    switch (q) {
        case EXCERPT_OK:
            ai_reader_set_scene_saved(true);
            ESP_LOGI(TAG, "scene save excerpt: %s %u B (cut=%d, row_cut=%d) pending=%u",
                     body == s_passage_line ? "section" : "original", (unsigned)n, (int)cut,
                     (int)cut_by_line, excerpt_store_pending());
            return APP_REDRAW_PAGE;
        case EXCERPT_FULL:
            reader_notice_set("摘抄待落盘已满 · 请稍候", 2500);
            ESP_LOGW(TAG, "scene save excerpt: queue full (%u)", excerpt_store_pending());
            return APP_REDRAW_PAGE;
        case EXCERPT_NO_MEM:
            reader_notice_set("内存不足 · 未存", 2000);
            return APP_REDRAW_PAGE;
        case EXCERPT_EMPTY:
        default:
            reader_notice_set("无可存内容", 2000);
            return APP_REDRAW_PAGE;
    }
}

/// 情景卡 / 段落卡的保存动作分派。词卡语义不动（R1 纪律：行为变化只发生在新路径上）。
/// The save dispatcher. The word card's behaviour is untouched: the change is only
/// on the new path.
static app_redraw_t scene_save(void) {
    return ai_reader_is_excerpt() ? scene_save_passage() : scene_save_word();
}


// ============================ M5-A 框选（单页 · 两次点按） ============================
// 不变量：单页内选定；反色只在 book_layout 的 draw_page 那一次游走里发生；所有退出都走
// boxsel_reset()。本文件不自己算行矩形，点按命中一律交给 book_layout_hit_test()。
// Invariants: the range lives on one page, inversion happens inside book_layout's single
// draw_page walk, and every exit funnels through boxsel_reset(). This file never computes
// line geometry of its own; taps are resolved by book_layout_hit_test().
static box_selection_t s_boxsel;

static bool boxsel_active(void) {
    return s_boxsel.state != BOX_STATE_OFF && s_view == READING && s_text;
}

static void boxsel_reset(void) {
    box_select_cancel(&s_boxsel);
    book_layout_set_invert_range(0, 0);
}

// 底部条：提示与动作栏共用同一条基线（R4：提示绝不放顶部，那里是控制中心下拉区）。
// Bottom strip: the hint and the action bar share one baseline (R4: never at the top,
// which belongs to the quick-layer pull gesture).
static EpdRect boxsel_bar_rect(void) {
    const int h = 64;
    return (EpdRect){0, ui_h() - h, ui_w(), h};
}

static EpdRect boxsel_button_rect(int i) {
    const EpdRect bar = boxsel_bar_rect();
    const int pad = 14, gap = 10;
    const int w = (bar.width - pad * 2 - gap * 2) / 3;
    return (EpdRect){bar.x + pad + i * (w + gap), bar.y + 10, w, bar.height - 20};
}

// 进入框选：只有阅读正文、单栏横排、无面板/无详解/无清屏确认时才允许。
// Enter: only the reading body, horizontal single column, with no panel, detail session or
// clear-screen confirmation on top.
static app_redraw_t boxsel_enter(app_ctx_t* ctx) {
    if (s_view != READING || !s_text || s_reader_panel != READER_PANEL_NONE || s_toolbar ||
        s_clear_confirm || book_layout_vertical() || ai_reader_active())
        return APP_REDRAW_NONE;
    box_select_begin(&s_boxsel, s_page, ctx ? ctx->now_ms : 0);
    book_layout_set_invert_range(0, 0);
    ESP_LOGI(TAG, "boxsel: enter page=%u", (unsigned)s_page);
    return APP_REDRAW_PAGE;
}

static app_redraw_t boxsel_enter_cb(app_ctx_t* ctx) { return boxsel_enter(ctx); }

// 动作栏一期只打日志（不落库、不发 0x07）。缓冲区放大到 768 并放到静态区：
// 26.9.12 起选区是**整句**，一句四五行中文就是两三百字节，256 的栈缓冲会装不下——
// 而 box_select_copy_text 的契约是「装不下就一个字不写」，于是一条真实的整句会退化成
// `len=0 ""` 的空日志。那等于把「太长」伪装成「没内容」，与 26.9.9 那类「采不到装作
// 没发生」是同一个毛病。这里的处理是：装不下就**明说截断**（truncated=1 + 真实长度），
// 绝不悄悄少打一半。
// The action bar only logs in phase one (no persistence, no 0x07). The buffer is 768 bytes
// and lives in static storage: since 26.9.12 the range is a whole SENTENCE, and four or
// five lines of Chinese run to two or three hundred bytes - a 256-byte stack buffer would
// overflow, and box_select_copy_text's contract is "write nothing rather than a half copy",
// so a real sentence would degrade into `len=0 ""`. That dresses "too long" up as "empty",
// the same disease as 26.9.9's "cannot observe pretending nothing happened". The rule here:
// when it does not fit, say so (truncated=1 plus the true length) - never silently log half.
#define BOXSEL_ACTION_TEXT_CAP 768
// 「存摘抄」与「AI解析」共用这一块：两条路径互斥（点一下就退出框选），共用不是省，
// 是不给「开关一开就多 768 B」留口子。
// One buffer for both actions: they are mutually exclusive (a tap exits), and sharing it
// keeps flipping the switch from silently costing another 768 B of .bss.
static char s_boxsel_text[BOXSEL_ACTION_TEXT_CAP];

// 出口声明制（S26-2，B31 的结构前提）：**每一个离开框选态的出口都必须带标签**，
// 且只能通过 boxsel_exit() 走——「点了没反应」和「点了但自己走了」必须能在日志里区分。
// Exit declaration (S26-2, the premise of tooth B31): every exit from box-select carries a
// label and goes through boxsel_exit() only - "the tap was swallowed" and "the user left on
// their own" must be distinguishable in the log.
static const char *boxsel_exit_name(boxsel_exit_t why) {
    switch (why) {
        case BOXSEL_EXIT_SAVE: return "save";
        case BOXSEL_EXIT_AI: return "ai";
        case BOXSEL_EXIT_CANCEL: return "cancel";
        case BOXSEL_EXIT_TAP_OUT: return "tap-out";
        case BOXSEL_EXIT_DUAL: return "dual";
        case BOXSEL_EXIT_TIMEOUT: return "timeout";
        case BOXSEL_EXIT_KEY: return "key";
        case BOXSEL_EXIT_KEY_LONG: return "key-long";
        case BOXSEL_EXIT_BOUNDARY: return "boundary";
    }
    return "?";
}

static app_redraw_t boxsel_exit(boxsel_exit_t why) {
    boxsel_reset();
    ESP_LOGI(TAG, "boxsel: exit (%s)", boxsel_exit_name(why));
    return APP_REDRAW_PAGE;
}

// ---- S26-3：AI 解析上行（0x1A bit2）------------------------------------------
// 形态定案（书面统一，替换 26.9.26 S26-2 的临时形态）：**置灰不可点**。开关
// CONFIG_PICO_EXCERPT_AI_SEND 为 n 时，按钮画成浅灰、点了不消费（不退框选、不发帧）；
// 为 y 时才成为真按钮。S26-2 交付的「可点 + 页脚『AI 解析未开放』」按偏离归档作废——
// 一个按下去才有反应、反应还是「没有这个功能」的按钮，比不画它更糟。
// The button form is fixed here: GREYED when the switch is n. The S26-2 stopgap (pressable,
// then says "not available") is retired - a control whose only response is "no such feature"
// is worse than no control.
static bool excerpt_ai_enabled(void) {
#if CONFIG_PICO_EXCERPT_AI_SEND
    return true;
#else
    return false;
#endif
}

/// UTF-8 前缀长度：返回 ≤n 的最大字符边界。/ Longest character boundary <= n.
/// 发送与落盘共用：半个汉字上了 BLE 就是一条坏帧，写进文件就是一行坏文本。
static size_t utf8_prefix_len(const char *s, size_t n) {
    size_t i = 0, last = 0;
    while (i < n) {
        const unsigned char c = (unsigned char)s[i];
        const size_t need = c < 0x80u ? 1u
                          : (c & 0xE0u) == 0xC0u ? 2u
                          : (c & 0xF0u) == 0xE0u ? 3u
                          : (c & 0xF8u) == 0xF0u ? 4u : 1u;
        if (i + need > n) break;
        i += need;
        last = i;
    }
    return last;
}

// 以下三件只在开关为 y 时编译：n 时按钮置灰、发送路径压根不存在，源码里也就不该有
// 一段没人调用的死代码（`-Wunused-function` 会当场把它顶出来）。
// The three helpers below compile only with the switch on; with n the send path does not
// exist at all, so there is no dead code to rot.
#if CONFIG_PICO_EXCERPT_AI_SEND
/// 解一个码点（非法序列按 1 字节的 U+FFFD 前进）。/ Decode one code point.
static size_t utf8_next_cp(const char *s, size_t n, uint32_t *cp) {
    const unsigned char c = (unsigned char)s[0];
    size_t need;
    uint32_t v;
    if (c < 0x80u) { *cp = c; return 1; }
    if ((c & 0xE0u) == 0xC0u) { need = 2; v = c & 0x1Fu; }
    else if ((c & 0xF0u) == 0xE0u) { need = 3; v = c & 0x0Fu; }
    else if ((c & 0xF8u) == 0xF0u) { need = 4; v = c & 0x07u; }
    else { *cp = 0xFFFDu; return 1; }
    if (n < need) { *cp = 0xFFFDu; return 1; }
    for (size_t i = 1; i < need; ++i) {
        const unsigned char cc = (unsigned char)s[i];
        if ((cc & 0xC0u) != 0x80u) { *cp = 0xFFFDu; return 1; }
        v = (v << 6) | (cc & 0x3Fu);
    }
    *cp = v;
    return need;
}

/// 词的断点：空白与常见中英标点。/ Word break: whitespace and common CJK/ASCII punctuation.
static bool word_break_cp(uint32_t cp) {
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') return true;
    if (cp < 0x80u) return strchr(",.!?;:\"'()[]{}<>/\\|`~@#$%^&*-_=+", (int)cp) != NULL;
    return (cp >= 0x3000u && cp <= 0x303Fu) ||  // CJK 标点与全角空格
           (cp >= 0xFF01u && cp <= 0xFF20u) ||  // 全角 ！＂＃…＠
           (cp >= 0x2010u && cp <= 0x2027u) ||  // 引号、破折号、省略号
           (cp >= 0x300Au && cp <= 0x3011u) ||  // 《》「」『』【】
           cp == 0x00B7u;
}

/// 选区首词（0x1A 的 word 字段）：跳过开头的空白/标点，取到下一个断点或 63B 为止，
/// 按整字符收口。全篇无断点时取前 63B 的整字符前缀——word 不许为空，否则整条发不出。
/// Leading word for the 0x1A word field: skip leading blanks/punctuation, stop at the next
/// break or at 63 B, always on a character boundary. word is never empty: the request needs it.
static void excerpt_first_word(const char *s, char *out, size_t cap) {
    out[0] = '\0';
    if (!s || cap < 2) return;
    const size_t limit = cap - 1u < 63u ? cap - 1u : 63u; // word ≤63B（与 PUSH_SCENE_WORD_MAX 同源）
    const size_t n = strlen(s);
    size_t i = 0;
    while (i < n) {
        uint32_t cp = 0;
        const size_t step = utf8_next_cp(s + i, n - i, &cp);
        if (!word_break_cp(cp)) break;
        i += step;
    }
    size_t w = 0;
    while (i < n && w < limit) {
        uint32_t cp = 0;
        const size_t step = utf8_next_cp(s + i, n - i, &cp);
        if (word_break_cp(cp) || w + step > limit) break;
        i += step;
        w += step;
    }
    if (!w) { // 全篇只有一个超长词（或开头就是断点）：退回最前面的整字前缀
        w = utf8_prefix_len(s, limit);
        if (w) memcpy(out, s, w);
        out[w] = '\0';
        return;
    }
    memcpy(out, s + (i - w), w);
    out[w] = '\0';
}

/// 26.11.3-C 形态判的阈值：≤32 B 且无空白 = 一个词（点词路径），否则 = 一段话。
/// 32 B 够放下英文最长单词（不含连字符组合的极端造词），也够放下一个中文词。
/// Threshold for the shape test: <= 32 B and no whitespace means "one word" (tap-word
/// path); anything else is a passage. 32 B holds the longest English word and a CJK term.
#define BOXSEL_WORD_MAX 32

/// 选段里有没有空白（空格/制表/换行）——「一个词」的形态判据。
/// Whether the selection contains whitespace - the "one word" shape test.
static bool boxsel_has_space(const char* s, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return true;
    }
    return false;
}

// 「AI解析」：复用 0x1A（零新帧），bit2 = PUSH_SCENE_FLAG_EXCERPT 分流到整段解析。
// 三条纪律与「存摘抄」同源：点击只发一次、每个分支都 EXIT、提示只来自本次调用的返回值。
//
// 26.11.3-C 四笔（全部由 26.11.3-B 真机档② FAIL 照出来，逐笔都有日志证据）：
//   ① 上下文改用 s_scene_ctx（1025 B）：旧的 768 B 框选缓冲把 879/925 B 的段落砍到
//      767，用户拿到的是「半段被当成整段」的答案；
//   ② 拒发门判在**复制之前**：先复制再判，门就成了装饰品，>1024 的选段永远先被截断；
//   ③ 发出去必须 arm（scene_req_arm_excerpt）：只发不 arm，0x08 回来无人认领，答案
//      落成词卡——「情境释义」变成「单词卡」的第二个原因；
//   ④ 选段形态判：≤32 B 且无空白 = 一个词，走点词等价路径（0x17 + 词卡），不发 0x1A。
//
// Send the excerpt as one 0x1A with bit2 set. Four lessons from the 26.11.3-B hardware
// failure: the context buffer must be the protocol's 1025 (not a 768 box-select buffer),
// the over-cap refusal must run BEFORE the copy, a send must arm the scene state or its
// answer lands as a word card, and a single-word selection goes down the tap-word path.
static app_redraw_t boxsel_ai_send(void) {
    // 26.11.3-C：整段请求的上下文就装在 s_scene_ctx 里——点词路径用的是同一个缓冲，
    // 语义相同（「这一份要发出去的上下文」），不同源只会多一份迟早漂开的拷贝。
    // The passage request lives in s_scene_ctx: the tap-word path uses the same buffer
    // for the same meaning (the context about to go out); a second copy would drift.
    char *const text = s_scene_ctx;
    const size_t len = box_select_length(&s_boxsel);

    // 26.11.4 裁决（审计三笔之一）：**> PUSH_SCENE_CTX_MAX 的选区拒发**，不静默截到 1024。
    // 1024 是协议上界；把两屏文字悄悄砍掉一半再问 AI，答案会指着被砍掉的那段说话——
    // 用户拿到的是一份「看起来完整」的错误答案。拒发 + 明说，比发一份残缺的强。
    // A selection over the protocol cap is REFUSED, never silently cut to 1024: a truncated
    // question gets an answer about the wrong half, and the user cannot tell the difference.
    if (len > PUSH_SCENE_CTX_MAX) {
        reader_notice_set("选段过长 · 请缩小范围", 2000);
        excerpt_hint_set("选段过长 · 请缩小范围", EXCERPT_HINT_ALERT);
        ESP_LOGW(TAG, "boxsel: ai refused page=%u len=%u over ctx max=%u",
                 (unsigned)s_boxsel.page, (unsigned)len, (unsigned)PUSH_SCENE_CTX_MAX);
        return boxsel_exit(BOXSEL_EXIT_AI);
    }

    const size_t n = box_select_copy_text(&s_boxsel, s_text, s_text_len, text,
                                          sizeof(s_scene_ctx));
    if (!n) {
        // 过了上面那道门还能复制失败，只可能是选区与页文本对不上（翻页/重排）。
        // Reaching here means the selection no longer matches the page text.
        ESP_LOGI(TAG, "boxsel: ai nothing to send page=%u len=%u", (unsigned)s_boxsel.page, (unsigned)len);
        excerpt_hint_set("无选中文本", EXCERPT_HINT_ALERT);
        return boxsel_exit(BOXSEL_EXIT_AI);
    }

    // ④ 形态判（26.11.3-C 裁决，行为变更已申报）：一个词就按点词那条路走。
    // 框选一个词的用户要的是词卡，不是「整段」四节——发 0x1A 会让手机端按段落解析
    // 一个词，答案里三个小节只能编。这里省下的不只是一次误发，还有一次误解。
    // A single word goes down the tap-word path: that user wants a word card, and asking
    // the phone for a four-section passage analysis of one token only invites invention.
    if (n <= BOXSEL_WORD_MAX && !boxsel_has_space(text, n)) {
        char wbuf[PUSH_SCENE_WORD_MAX];
        copy_text(wbuf, sizeof(wbuf), text);
        // 单词路径不开情景视图：先撤掉可能还挂着的情景待命，免得旧单被这张卡认领。
        // No scene view on the word path: drop any pending scene request first so an
        // older request cannot claim this answer.
        scene_req_disarm(&s_scene_req);
        push_sink_ai_tap_word_ctx(wbuf, book_id_from_path(s_path), s_boxsel.off_start);
        if (push_sink_ai_enabled() && dict_popup_show_pending(wbuf)) {
            s_reader_panel = READER_PANEL_DICT;
        } else {
            dict_popup_reset();
            s_reader_panel = READER_PANEL_NONE;
        }
        ESP_LOGI(TAG, "boxsel: ai word path page=%u len=%u word=\"%s\"",
                 (unsigned)s_boxsel.page, (unsigned)len, wbuf);
        invalidate_prep();
        return boxsel_exit(BOXSEL_EXIT_AI);
    }

    char word[PUSH_SCENE_WORD_MAX];
    excerpt_first_word(text, word, sizeof(word));
    // 本地不再有任何截断 → bit0 恒为 0；协议上界由上面那道门守，回执不必再分档。
    // No local truncation survives, so bit0 is always 0 and the receipt has one form.
    const uint8_t flags = (uint8_t)PUSH_SCENE_FLAG_EXCERPT;
    // word 恒为 ctx 的前缀 → word_off = 0（登记簿 docs/PROTOCOL.md 里写死这条语义）。
    // word is always a prefix of ctx, hence word_off = 0 (fixed in docs/PROTOCOL.md).
    const bool ok = push_sink_ai_scene_send(word, text, n, 0, flags);
    // ③ 发出去就 arm：0x08 回流没有「这是段落答案」的位（协议零改动），只有发起时的
    // 记忆能把它分到「选段」视图而不是词卡。时钟与 scene_req_claim 同源（esp_timer ms）。
    // Arm on send: the returning 0x08 carries no "this is a passage" bit, so only the
    // originating state can route it; the clock is the same one scene_req_claim uses.
    if (ok) {
        scene_req_arm_excerpt(&s_scene_req, word, 0, (uint8_t)strnlen(word, sizeof(word)),
                              s_boxsel.page,
                              esp_timer_get_time() / 1000 + EXCERPT_REQ_TIMEOUT_MS);
    } else {
        scene_req_disarm(&s_scene_req); // 没发出去就不该等 / nothing sent, nothing to wait for
    }
    // 回执只报「发了/没发」：四条前置门（AI 开关 / 情景开关 / APK 能力 / 链路订阅）
    // 里哪条没过，这个 API 不告诉我们——猜一条写上去就是撒谎。
    // 26.11.4：回执必须上**顶部气泡**。全屏阅读不画页脚带，只写 hint 槽等于没写。
    // The receipt says sent or not and goes to the toast: full-screen reading paints no
    // footer band, so a hint-only receipt is invisible (the 26.11.1 trap).
    const char *receipt = ok ? "AI 已发" : "未发送";
    // 26.11.3-C2：发出去了就排一条等待期气泡（2.2 s 后，等「AI 已发」退场）。
    // 排了不一定会响：tick 到点时会再确认「这条整段请求还挂着」。
    // 26.11.3-C2: on a successful send, schedule the wait notice 2.2 s out (after
    // "AI 已发" has cleared). Scheduling is not showing: the tick re-checks that the
    // passage request is still armed before anything is painted.
    if (ok) s_ai_wait_notice_at_ms = esp_timer_get_time() / 1000 + 2200;
    excerpt_hint_set(receipt, ok ? EXCERPT_HINT_INFO : EXCERPT_HINT_ALERT);
    reader_notice_set(receipt, 2000);
    ESP_LOGI(TAG, "boxsel: ai send page=%u len=%u copy=%u word=\"%s\" flags=0x%02x ok=%d",
             (unsigned)s_boxsel.page, (unsigned)len, (unsigned)n, word,
             (unsigned)flags, (int)ok);
    return boxsel_exit(BOXSEL_EXIT_AI);
}
#endif

static app_redraw_t boxsel_action(int which) {
    char *const text = s_boxsel_text;

    // 动作栏二期（S26-2）：**真实业务闭环**。一期只打日志，这一期把「存摘抄」接到
    // excerpt_store。纪律三条：
    // The action bar, phase two (S26-2): a real closed loop. Phase one only logged; this one
    // wires the save button to excerpt_store. Three rules:
    //   1. 点击只做入队，**落盘不在这里发生**（B32'：事件路径只准与队列 API 说话）。
    //      A click only enqueues; the write never happens here (B32-prime).
    //   2. 每个分支都 EXIT：文本再长也不许留框选——超长照常退出并按上限摘录 + 页脚明说
    //      `[截]`，「例外迟早变漏洞」。
    //      Every branch exits, even for over-long text: it is saved truncated with a footer
    //      note, because an exception to the rule becomes a hole.
    //   3. UI 的每条提示都来自本次调用的返回值，**不猜结果**（没有 EXCERPT_OK 就一个字
    //      不许说已存）。Every hint comes from this call's return value - never a guess.
    if (which == 2) return boxsel_exit(BOXSEL_EXIT_CANCEL);
    if (which == 1) {
#if CONFIG_PICO_EXCERPT_AI_SEND
        return boxsel_ai_send();
#else
        // 正常不可达：置灰态在 boxsel_touch 的命中层就被挡下（不消费点击）。
        // Unreachable by design: the greyed key is filtered at the hit-test layer.
        ESP_LOGW(TAG, "boxsel: ai tapped while disabled (unreachable by design)");
        excerpt_hint_set("AI 解析未开放", EXCERPT_HINT_ALERT);
        return boxsel_exit(BOXSEL_EXIT_AI);
#endif
    }

    const size_t len = box_select_length(&s_boxsel);
    size_t n = box_select_copy_text(&s_boxsel, s_text, s_text_len, text, sizeof(s_boxsel_text));
    bool truncated = false;
    if (!n && len && s_text && s_boxsel.off_end <= s_text_len) {
        const size_t head = utf8_prefix_len(s_text + s_boxsel.off_start,
                                            len < sizeof(s_boxsel_text) - 1 ? len
                                                : sizeof(s_boxsel_text) - 1);
        if (head) {
            memcpy(text, s_text + s_boxsel.off_start, head);
            text[head] = '\0';
            n = head;
        }
        truncated = true;
    }
    // 膨胀不再 bail out：取不到文本也照常退出，并把「没内容」如实说出去。
    // No bail-out any more: even with no text we exit and say so.
    ESP_LOGI(TAG, "boxsel: excerpt page=%u [%u,%u) len=%u copy=%u truncated=%d \"%s\"",
             (unsigned)s_boxsel.page, (unsigned)s_boxsel.off_start, (unsigned)s_boxsel.off_end,
             (unsigned)len, (unsigned)n, (int)truncated, n ? text : "");

    bool cut_by_line = false;
    const excerpt_status_t st = excerpt_store_enqueue(
        s_book_title[0] ? s_book_title : s_title, (size_t)reader_visual_index(s_page),
        book_now_unix(), n ? text : NULL, &cut_by_line);
    switch (st) {
        case EXCERPT_OK:
            // 点击那一刻不许报喜——回执由 tick 的 f_sync 结果给出（先写后亮）。
            // No cheering at click time: the receipt comes from the tick's f_sync (write first).
            //
            // 但「已入队」必须当场说，而且要走**顶部气泡**，不许只靠页脚：
            // 26.11.1 真机轮第 2 回执——无卡连摘 3 条，屏幕上一个字都没有。页脚那条
            // 24 px 前缀（离线时是「TF 卡已移除 · 待落盘 N」）是全屏阅读不画、离开
            // READING 就失效、还得靠人低头去看的唯一载体；一个功能能不能被感知，
            // 不该押在「用户会不会去看屏幕最下面那一行」。先写后亮约束的是「已存」，
            // 不是「已入队」——说排队是实话，说存好才是撒谎。
            // Saying "queued" is not cheering: it is the literal queue depth. The receipt
            // "saved" still waits for f_sync. The toast exists because the footer is not
            // drawn in full-screen reading and dies with the view.
            {
                char toast[64];
                snprintf(toast, sizeof(toast), "已入队 · 待落盘 %u", excerpt_store_pending());
                reader_notice_set(toast, 2000);
            }
            if (cut_by_line || truncated) excerpt_hint_set("[截] 已按上限摘录", EXCERPT_HINT_ALERT);
            break;
        case EXCERPT_FULL:
            excerpt_hint_set("摘抄已满", EXCERPT_HINT_ALERT);
            break;
        case EXCERPT_NO_MEM:
            excerpt_hint_set("内存不足", EXCERPT_HINT_ALERT);
            break;
        case EXCERPT_EMPTY:
            excerpt_hint_set("无选中文本", EXCERPT_HINT_ALERT);
            break;
    }
    ESP_LOGI(TAG, "boxsel: excerpt enqueue -> %d (pending=%u)", (int)st, excerpt_store_pending());
    return boxsel_exit(BOXSEL_EXIT_SAVE);
}

// 框选态独占单指：滑动、长按、双击一律吞掉，只有 TAP 有语义。
// The mode owns the single finger: swipes, long presses and double taps are swallowed and
// only a TAP carries meaning.
static app_redraw_t boxsel_touch(app_ctx_t* ctx, const ui_gesture_event_t* ev) {
    if (!boxsel_active() || ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    ctx->user_activity = true;
    if (box_select_ready(&s_boxsel)) {
        for (int i = 0; i < 3; ++i) {
            if (!ui_rect_hit(boxsel_button_rect(i), ev->x0, ev->y0)) continue;
            if (i == 1 && !excerpt_ai_enabled()) {
                // 置灰键（S26-3 定案）：命中但**不消费**——不退框选、不发帧、不改状态，
                // 只留一行日志，好让「点了没反应」在日志里是「点了，但那个键本来就按不动」。
                // Greyed key: a hit is not consumed - no exit, no frame, no state change.
                ESP_LOGI(TAG, "boxsel: tap ai (greyed, switch off) page=%u", (unsigned)s_page);
                return APP_REDRAW_NONE;
            }
            return boxsel_action(i);
        }
        // READY 后点选区外 = 取消（E3）。ARM/TAIL 下点空则是「没点中」，不改状态（E-R5）。
        // A tap outside the bar while READY cancels (E3); a miss while ARM/TAIL is simply
        // "not a hit" and changes nothing (E-R5).
        return boxsel_exit(BOXSEL_EXIT_TAP_OUT);
    }
    size_t ws = 0, we = 0;
    // 26.9.14 D3：逐击日志。26.9.13 的真机日志里框选进了 5 次、只出 1 次摘抄，而这一
    // 段代码**一声不响**——「点了没反应」和「点了但用户自己走了」在日志里长得一模一样，
    // 这正是 P1 判不了的唯一原因。三句话覆盖三种结局：miss（没点中正文）/ ignored
    // （点中了但状态机不收）/ ok（收下并改了选区）。有了这三句，「点 10 次响应 10 次」
    // 才是可数的，而不是一句手感。
    // D3: per-tap logging. The 26.9.13 log had five boxsel entries and one excerpt, and
    // this code said nothing at all - "the tap was swallowed" and "the user moved on"
    // look identical on the wire, which is the only reason P1 could not be judged. Three
    // phrases cover three endings: miss (no text under the finger) / ignored (text hit,
    // state machine declined) / ok (taken, range changed). With them, "10 taps, 10
    // responses" becomes countable instead of a feeling.
    if (!book_layout_hit_test(s_page, ev->x0, ev->y0, &ws, &we)) {
        ESP_LOGI(TAG, "boxsel: tap (%d,%d) page=%u -> miss", (int)ev->x0, (int)ev->y0,
                 (unsigned)s_page);
        return APP_REDRAW_NONE;
    }
    // 按句吸附（26.9.12 P1-2）：首点向前找句首，尾点向后找句末。方向由**相对锚点的
    // 位置**决定，不由点击次序决定——先点尾再点头时，第二点落在锚前、成为新的起点，
    // 该向前拓；按次序定方向会把这种情况扩反。首次点按（ARM）没有锚点，默认向前。
    // 吸附只改字节区间，反色仍按整行涂抹（R-S4/R-S5）：选区与视觉解耦，不必引入
    // 逐字 x 区间，也就不会在墨水屏上制造局部刷新残影。
    // Sentence snapping (26.9.12 P1-2): the head tap walks back to the sentence start, the
    // tail tap forward to its end. The direction follows the tap's POSITION relative to the
    // anchor, not the order of taps - tapping the tail first makes the second (earlier) tap
    // the new start, which must extend backwards; going by order would extend it the wrong
    // way. The first tap (ARM) has no anchor and defaults to backwards.
    // Snapping changes the byte range only; inversion still paints whole lines (R-S4/R-S5),
    // so the range and the visuals stay decoupled - no per-glyph x intervals, and none of
    // the partial-refresh ghosting they would bring on this panel.
    const size_t page_start = book_layout_page_start_offset(s_page);
    size_t page_end = s_text_len;
    if (s_page + 1 < book_layout_page_count()) page_end = book_layout_page_start_offset(s_page + 1);
    const int dir = (s_boxsel.state == BOX_STATE_ARM || ws < s_boxsel.anchor_ws) ? -1 : 1;
    size_t snap_s = ws, snap_e = we;
    if (box_select_snap_sentence(s_text, s_text_len, page_start, page_end, ws, we, dir,
                                 &snap_s, &snap_e)) {
        ws = snap_s;
        we = snap_e;
    }
    const bool took = box_select_tap(&s_boxsel, ws, we, ctx->now_ms);
    ESP_LOGI(TAG, "boxsel: tap (%d,%d) page=%u word=[%u,%u) state=%d -> %s",
             (int)ev->x0, (int)ev->y0, (unsigned)s_page, (unsigned)ws, (unsigned)we,
             (int)s_boxsel.state, took ? "ok" : "ignored");
    if (!took) return APP_REDRAW_NONE;
    book_layout_set_invert_range(s_boxsel.off_start, s_boxsel.off_end);
    return APP_REDRAW_PAGE;
}

static void boxsel_draw(uint8_t* fb) {
    if (!boxsel_active()) return;
    const EpdRect bar = boxsel_bar_rect();
    ui_fill_rect_fast(bar, UI_GRAY_WHITE, fb);
    ui_draw_separator(fb, bar.y, bar.x, bar.width, 0x38);
    if (!box_select_ready(&s_boxsel)) {
        const char* hint = s_boxsel.state == BOX_STATE_ARM ? "框选：点首字" : "再点尾字";
        ui_text_vc(fb, bar.x + bar.width / 2, bar.y + bar.height / 2, 22, hint,
                   EPD_DRAW_ALIGN_CENTER, false);
        return;
    }
    static const char* labels[3] = {"存摘抄", "AI解析", "取消"};
    for (int i = 0; i < 3; ++i) {
        const EpdRect r = boxsel_button_rect(i);
        const bool greyed = (i == 1) && !excerpt_ai_enabled();
        // 置灰 = 边框与字一起浅下去（0x38 实心边框 + 黑字 → 0x90 边框 + 0x90 字）。
        // Greying dims border and ink together, so the key reads as "there, but not pressable".
        ui_draw_round_rect(fb, r, 10, greyed ? UI_GRAY_LIGHT : 0x38);
        if (greyed)
            ui_text_gray_vc(fb, r.x + r.width / 2, r.y + r.height / 2, 20, labels[i],
                            EPD_DRAW_ALIGN_CENTER, UI_GRAY_LIGHT);
        else
            ui_text_vc(fb, r.x + r.width / 2, r.y + r.height / 2, 20, labels[i],
                       EPD_DRAW_ALIGN_CENTER, false);
    }
}

// 退出统一收口：任务边界（离页、丢卡、锁屏）+ 视图/页码漂移都回到这里。
// One exit sink: task boundaries (leaving the page, media loss, lock) and any view or page
// drift all land here.
static void boxsel_boundary_reset(void) {
    // 没有例外：连清场也要报名字，否则日志里又出现一笔「不知道为什么没选中了」。
    // No exceptions: even a boundary clear names itself, or the log gains another
    // "the selection vanished for no recorded reason".
    if (s_boxsel.state != BOX_STATE_OFF) (void)boxsel_exit(BOXSEL_EXIT_BOUNDARY);
}

// 框选态要求**单指优先**（26.9.12 P1-1 的 B 方案，26.9.13 升成策略位）：这里两次
// 精确点按是唯一输入，双指一旦把单指饿死就等于「点了没反应」，要等 30 s 超时才恢复；
// 代价是双指落定那一瞬可能误选一个字，而双指会立刻退出框选，再点一次就纠正得回来。
// 这个取舍已由审计裁定接受：**点不出来（卡死 30 s）的伤害远大于点错一个字**。
// 26.9.13 起这一个位**同时**管住另一半：单指识别器遇 count>1 也不再取消 TAP
// （见 ui_gesture_feed）。26.9.12 只接线了前半，于是按下与抬起之间插进一帧幻点，
// 这次点按照样作废——用户说的「十次里有一两次没反应」就是那半条。
// The box-selection mode requires **single-finger priority** (26.9.12 P1-1 plan B,
// raised to a policy bit in 26.9.13): two precise taps are the only input here, and a
// dual session swallowing the single finger reads as "taps do nothing" until the 30 s
// timeout. The cost is one stray glyph the instant a pair lands - and a landed pair
// exits the mode at once, so one more tap corrects it. Adjudicated and accepted:
// **failing to respond for 30 s hurts far more than picking the wrong glyph once**.
// Since 26.9.13 the same bit governs the other half as well: the single-finger
// recognizer no longer cancels a TAP on count>1 (see ui_gesture_feed). 26.9.12 wired
// only the first half, so a single phantom two-contact frame between press and release
// still voided the tap - the "one in ten does nothing" the user reported.
static bool boxsel_single_finger_priority(app_ctx_t* ctx) {
    (void)ctx;
    return boxsel_active();
}

static app_redraw_t gesture_event(app_ctx_t* ctx, const ui_gesture_event_t* ev) {
    // 框选态独占输入（不变量 1）：单指只走框选，翻页/换页/点词一律到不了下面。
    // The mode owns input (invariant 1): the single finger goes to the selection and
    // never reaches page turns, panel switches or word lookup below.
    if (boxsel_active()) return boxsel_touch(ctx, ev);
    // M2 Step9 铁律①：详解态手势语义隔离——沿用阅读翻页方向映射（左右/上下
    // 滑按设置）翻详解页，首/末页停滞不退出；点击/长按/其他手势不穿透到原书
    // （点词、工具栏、导航、设置滑杆在覆盖层下全部不可达）。
    // M2 Step9 law 1: gesture semantics are isolated in a detail session - the
    // reader's turn mapping (swipes per settings) turns detail pages, stalling
    // at both ends; taps, long-presses and other gestures never fall through
    // to the source (word lookup, toolbar, nav and sliders are all unreachable
    // under the overlay).
    if (s_view == READING && ai_reader_active()) {
        const int direction = reader_swipe_direction(ev->type,
                                                     app_settings_reader_vertical_turn());
        if (direction) {
            ctx->user_activity = true;
            return reader_ai_turn(direction);
        }
        // M3-B：情景卡的「存生词 ›」是覆盖层上唯一可点的区域；其余点击/长按一律
        // 不穿透（铁律①不变）。详解态没有 chip，矩形为零，这里自然不命中。
        // M3-B: the scene card's "save word" chip is the ONLY tappable spot on the
        // overlay; every other tap or long press still falls nowhere (law 1 intact).
        // The detail view has no chip, so its rect is zero and never hits.
        if (ev->type == UI_GESTURE_TAP && ai_reader_mode() == AI_READER_MODE_SCENE) {
            const EpdRect chip = ai_reader_scene_save_rect();
            if (chip.width > 0 && ui_rect_hit(chip, ev->x0, ev->y0)) return scene_save();
        }
        return APP_REDRAW_NONE;
    }
    // 26.11.3：感应区收口到 shelf_category_tap_rect()（右界 333 零重叠），再加一层
    // 按钮优先守卫——几何被别人改坏时，点击按钮也绝不落进分类手势。注意是收窄
    // 条件让事件继续往下流到 action_at，不是在这里 return NONE（那会把点击吃掉）。
    // 26.11.3: the sensing rect is named and stops at 333, plus a button-first guard so a
    // future geometry edit cannot hand button taps to the dropdown. Narrowing the
    // condition (not returning NONE) keeps the tap flowing to action_at.
    if (s_view==SHELF && (s_category_menu ||
        (ui_rect_hit(shelf_category_tap_rect(),ev->x0,ev->y0) && !shelf_header_button_hit(ev->x0,ev->y0))))
        return shelf_category_gesture(ctx,ev);
    if (s_view == READING && (s_reader_panel == READER_PANEL_BACKGROUND ||
        s_reader_panel == READER_PANEL_BACKGROUND_PREVIEW) &&
        (ev->type == UI_GESTURE_SWIPE_U || ev->type == UI_GESTURE_SWIPE_D)) {
        return reader_background_step(ev->type == UI_GESTURE_SWIPE_U ? 1 : -1);
    }
    if (ev->type == UI_GESTURE_CANCEL || ev->type == UI_GESTURE_LONG_PRESS ||
        ev->type == UI_GESTURE_SWIPE_L || ev->type == UI_GESTURE_SWIPE_R ||
        ev->type == UI_GESTURE_SWIPE_U || ev->type == UI_GESTURE_SWIPE_D ||
        (ev->type == UI_GESTURE_PRESS && !book_reader_center(body_rect(), ev->x0, ev->y0)))
        s_reader_tap.pending = false;
    if (reader_tools_swipe(ctx, ev)) return APP_REDRAW_PAGE;
    bool input_feedback = false;
    if (s_view == SEARCH || s_view == EDIT) {
        if (ev->type == UI_GESTURE_PRESS)
            return ui_keyboard_press(ev->x0, ev->y0, 560, ctx->now_ms) ? editor_paint(ctx, false) : APP_REDRAW_NONE;
        input_feedback = ev->type != UI_GESTURE_LONG_PRESS && ui_keyboard_release();
    }
    if (s_view == SEARCH) {
        if (ev->type == UI_GESTURE_TAP) return action_at(ctx, ev->x0, ev->y0);
        if (ev->type == UI_GESTURE_SWIPE_L || ev->type == UI_GESTURE_SWIPE_R) {
            if (!ui_keyboard_page(ev->type == UI_GESTURE_SWIPE_L ? 1 : -1) && !input_feedback) return APP_REDRAW_NONE;
            return editor_paint(ctx, false);
        }
        return input_feedback ? editor_paint(ctx, false) : APP_REDRAW_NONE;
    }
    if (s_view == TOC) {
        if (s_toc_jump_open) {
            if (ev->type == UI_GESTURE_PRESS &&
                book_toc_jump_hit(ev->x0, ev->y0) == BOOK_TOC_JUMP_TRACK) {
                s_toc_jump_drag = true;
                s_toc_jump_percent = book_toc_jump_percent(ev->x0);
                return APP_REDRAW_PAGE;
            }
            if (s_toc_jump_drag && ev->type == UI_GESTURE_MOVE) {
                int next = book_toc_jump_percent(ev->x);
                if (next == s_toc_jump_percent) return APP_REDRAW_NONE;
                s_toc_jump_percent = next;
                render(ctx, ctx->fb);
                s_area = (EpdRect){44, 356, 596, 572};
                s_mode = MODE_GL16;
                return APP_REDRAW_AREA;
            }
            if (s_toc_jump_drag && ev->type != UI_GESTURE_PRESS) {
                s_toc_jump_drag = false;
                if (ev->type == UI_GESTURE_CANCEL) return APP_REDRAW_NONE;
                s_toc_jump_percent = book_toc_jump_percent(ev->x);
                return APP_REDRAW_PAGE;
            }
            if (ev->type == UI_GESTURE_TAP) return action_at(ctx, ev->x0, ev->y0);
            return APP_REDRAW_NONE;
        }
        if (ev->type == UI_GESTURE_TAP) {
            int start = book_toc_hit(ev->x0, ev->y0, ctx->leaf, book_navigation_count());
            int end = book_toc_hit(ev->x, ev->y, ctx->leaf, book_navigation_count());
            if (start != -1 && start == end) return action_at(ctx, ev->x0, ev->y0);
        }
        if (ev->type == UI_GESTURE_SWIPE_U || ev->type == UI_GESTURE_SWIPE_D) {
            int next = ctx->leaf + (ev->type == UI_GESTURE_SWIPE_U ? 1 : -1);
            if (next >= 0 && next < leaves()) { ctx->leaf = next; return APP_REDRAW_PAGE; }
        }
        return APP_REDRAW_NONE;
    }
    if (s_view == EDIT) {
        if (ev->type == UI_GESTURE_TAP) return editor_action(ctx, ev->x0, ev->y0);
        if (ev->type == UI_GESTURE_SWIPE_L || ev->type == UI_GESTURE_SWIPE_R)
            return (ui_keyboard_page(ev->type == UI_GESTURE_SWIPE_L ? 1 : -1) || input_feedback) ? editor_paint(ctx, false) : APP_REDRAW_NONE;
        return input_feedback ? editor_paint(ctx, false) : APP_REDRAW_NONE;
    }
    if (s_view == SHELF && ev->type == UI_GESTURE_TAP && ui_nav_hit(ev->x0, ev->y0) >= 0) {
        ui_nav_request(ctx, ui_nav_hit(ev->x0, ev->y0));
        return APP_REDRAW_NONE;
    }
    if (s_view == READING && ev->type != UI_GESTURE_PRESS) s_stats_activity_ms = ctx->now_ms;
    if (s_view == READING &&
        (s_reader_panel == READER_PANEL_FONT_SETTINGS || s_reader_panel == READER_PANEL_LAYOUT_SETTINGS)) {
        if (ev->type == UI_GESTURE_PRESS) {
            int slider = reader_slider_at(ev->x0, ev->y0);
            if (slider >= 0) {
                reader_slider_begin(slider, ev->x0);
                render(ctx, ctx->fb);
                s_area = reader_slider_refresh_rect(slider);
                s_mode = MODE_GL16;
                return APP_REDRAW_AREA;
            }
        } else if (s_reader_slider >= 0) {
            if (ev->type == UI_GESTURE_MOVE) {
                int slider = s_reader_slider;
                reader_slider_move(ev->x);
                render(ctx, ctx->fb);
                s_area = reader_slider_refresh_rect(slider);
                s_mode = MODE_GL16;
                return APP_REDRAW_AREA;
            }
            if (ev->type == UI_GESTURE_TAP || ev->type == UI_GESTURE_SWIPE_L ||
                ev->type == UI_GESTURE_SWIPE_R || ev->type == UI_GESTURE_SWIPE_U ||
                ev->type == UI_GESTURE_SWIPE_D || ev->type == UI_GESTURE_LONG_PRESS) {
                if (!s_reader_slider_endpoint) reader_slider_move(ev->x);
                return reader_slider_commit(ctx);
            }
            if (ev->type == UI_GESTURE_CANCEL) {
                s_reader_slider = -1;
                return APP_REDRAW_PAGE;
            }
        }
    }
    if (s_view == READING && s_reader_panel == READER_PANEL_FONT_PICKER &&
        (ev->type == UI_GESTURE_SWIPE_U || ev->type == UI_GESTURE_SWIPE_D)) {
        int count = ttf_font_count();
        int pages = count > 0 ? (count + BOOK_FONT_PAGE - 1) / BOOK_FONT_PAGE : 1;
        if (ev->type == UI_GESTURE_SWIPE_U && s_font_page + 1 < pages) ++s_font_page;
        else if (ev->type == UI_GESTURE_SWIPE_D && s_font_page > 0) --s_font_page;
        else return APP_REDRAW_NONE;
        return APP_REDRAW_PAGE;
    }
    EpdRect start_rect = {0}, end_rect = {0};
    int start = control_at(ctx, ev->x0, ev->y0, &start_rect);
    int end = control_at(ctx, ev->x, ev->y, &end_rect);
    if (ev->type == UI_GESTURE_PRESS) {
        s_pressed_control = start;
        return start >= 0 ? paint_control(ctx, start_rect) : APP_REDRAW_NONE;
    }
    bool decorated = s_pressed_control >= 0;
    s_pressed_control = -1;
    if (ev->type == UI_GESTURE_LONG_PRESS && start == end) {
        if (s_view == SHELF && start >= 0 && start < shelf_rows() && !s_clear_confirm) {
            s_managed = s_shelf[ctx->leaf * shelf_rows() + start];
            s_view = MANAGE;
            s_clear_confirm = false;
            s_delete_confirm = s_file_removed = s_managed.removed;
            copy_text(s_manage_message, sizeof(s_manage_message), s_file_removed ? "文件已删除，进度清理失败，请重试" : "");
            return APP_REDRAW_PAGE;
        }
        if (s_view == READING && !s_toolbar && !s_clear_confirm && ui_rect_hit(body_rect(), ev->x0, ev->y0)) {
            // 长按改为点词查译；目录仍可从工具条进入。
            // A long press now looks up the word; the TOC is still on the toolbar.
            return start_dict_lookup(ctx, ev->x0, ev->y0);
        }
    }
    if (ev->type == UI_GESTURE_TAP && !s_scan_pending) {
        if (start >= 0 && start == end) {
            // 列表先完成回弹，才释放封面缓存并开书；没有人为等待或整页预刷新。
            // Settle a list bounce before opening frees artwork, without a delay or whole-page pre-refresh.
            if (decorated && s_view == SHELF && app_settings_shelf_style() == 5 && start < shelf_rows())
                (void)present(ctx, paint_control(ctx, start_rect));
            app_redraw_t result = action_at(ctx, ev->x0, ev->y0);
            if (ctx->request_app || ctx->request_menu || ctx->request_return) return result;
            return result != APP_REDRAW_NONE ? result : paint_control(ctx, start_rect);
        }
        if (start < 0 && end < 0 && s_view == READING && !s_clear_confirm)
            return action_at(ctx, ev->x0, ev->y0);
    }
    if (!s_clear_confirm && !s_scan_pending) {
        if (s_view == READING && (!s_toolbar || (s_reader_panel == READER_PANEL_TOOLS && ev->y0 < 1096)) &&
            ui_rect_hit(body_rect(), ev->x0, ev->y0)) {
            int direction = reader_swipe_direction(ev->type, app_settings_reader_vertical_turn());
            if (direction) { ctx->user_activity = true; return turn_page(ctx, direction); }
        } else if (s_view == SHELF && (ev->type == UI_GESTURE_SWIPE_L || ev->type == UI_GESTURE_SWIPE_R)) {
            app_redraw_t result = shelf_turn_page(ctx, ev->type == UI_GESTURE_SWIPE_L ? 1 : -1);
            if (result != APP_REDRAW_NONE) return result;
        } else if ((s_view == BULK && !s_batch_confirm) &&
                   (ev->type == UI_GESTURE_SWIPE_U || ev->type == UI_GESTURE_SWIPE_D)) {
            return bulk_turn_page(ctx, ev->type == UI_GESTURE_SWIPE_U ? 1 : -1);
        }
    }
    return decorated ? paint_control(ctx, start_rect) : APP_REDRAW_NONE;
}
static app_redraw_t reader_key_action(app_ctx_t *ctx, app_reader_key_action_t action) {
    s_reader_tap.pending = false;
    switch (action) {
        case APP_READER_KEY_PREV: case APP_READER_KEY_NEXT:
            s_stats_activity_ms = ctx->now_ms;
            return turn_page(ctx, action == APP_READER_KEY_PREV ? -1 : 1);
        case APP_READER_KEY_HOME: return reader_return(ctx);
        case APP_READER_KEY_FULLSCREEN: return toggle_reader_fullscreen(ctx);
        case APP_READER_KEY_REFRESH: return reader_manual_refresh(ctx);
        case APP_READER_KEY_TOOLS:
            s_reader_slider = -1;
            s_reader_panel = s_reader_panel == READER_PANEL_TOOLS ? READER_PANEL_NONE : READER_PANEL_TOOLS;
            invalidate_prep();
            return APP_REDRAW_PAGE;
        default: return APP_REDRAW_NONE;
    }
}
static app_redraw_t on_key_impl(app_ctx_t* ctx, int key);
// 框选态按键：先退出，再照常执行原键义（E5）；键本身不改页面时补一次重绘，
// 免得反色留在屏上。
// A key in the selection mode exits first and then does its usual job (E5); when the
// key changes nothing we repaint once so no inverse band is left on screen.
static app_redraw_t on_key(app_ctx_t* ctx, int key) {
    if (boxsel_active()) {
        const bool left = boxsel_exit(BOXSEL_EXIT_KEY) == APP_REDRAW_PAGE;
        (void)left;
        const app_redraw_t r = on_key_impl(ctx, key);
        return r == APP_REDRAW_NONE ? APP_REDRAW_PAGE : r;
    }
    return on_key_impl(ctx, key);
}
static app_redraw_t on_key_impl(app_ctx_t* ctx, int key) {
    s_reader_tap.pending = false;
    s_pressed_control = -1;
    // M2 Step9 铁律①：详解会话期按键语义全隔离——KEY1/3 翻详解页（首/末页
    // 停滞，不自动退出），KEY2 = 关闭并回原书当前页，其余键忽略。覆盖层下
    // 原书 turn_page / 工具栏 / 设置面板永不可达。/ M2 Step9 law 1: key
    // semantics are fully isolated in a detail session - KEY1/3 turn detail
    // pages (stalling at both ends, never auto-exiting), KEY2 closes back to
    // the source page, other keys are ignored. The source turn_page, toolbar
    // and settings panels are unreachable under the overlay.
    if (s_view == READING && ai_reader_active()) {
        if (key == UI_KEY_1 || key == UI_KEY_3) {
            return reader_ai_turn(key == UI_KEY_1 ? -1 : 1);
        }
        if (key == UI_KEY_2) return ai_reader_close_full();
        return APP_REDRAW_NONE;
    }
    if (s_view == TOC && s_toc_jump_open) {
        if (key == UI_KEY_2) s_toc_jump_open = s_toc_jump_drag = false;
        else if (key == UI_KEY_1 || key == UI_KEY_3) {
            s_toc_jump_percent += key == UI_KEY_1 ? -5 : 5;
            if (s_toc_jump_percent < 0) s_toc_jump_percent = 0;
            if (s_toc_jump_percent > 100) s_toc_jump_percent = 100;
        }
        return APP_REDRAW_PAGE;
    }
    if (s_view != READING && s_view != TOC) {
        if (key == UI_KEY_2) { ui_nav_request(ctx, 0); return APP_REDRAW_NONE; }
        if (s_view == EDIT) { ui_keyboard_end(); free(s_editor_cover); s_editor_cover = NULL; s_view = MANAGE; return APP_REDRAW_PAGE; }
        if (s_view == SEARCH) { search_finish(ctx, false); return APP_REDRAW_PAGE; }
        if (s_batch_confirm) { s_batch_confirm = false; return APP_REDRAW_PAGE; }
        if (s_view == MANAGE || s_view == BULK || s_view == IMPORT) {
            if (s_view == BULK) return bulk_finish(ctx);
            s_view = SHELF; return APP_REDRAW_PAGE;
        }
        if (s_view == SHELF) { ui_nav_request(ctx, 0); return APP_REDRAW_NONE; }
    }
    if (s_scan_pending || s_clear_confirm) return APP_REDRAW_NONE;
    if (s_view == READING && (key == UI_KEY_1 || key == UI_KEY_2 || key == UI_KEY_3))
        return reader_key_action(ctx, app_settings_reader_key_action(key == UI_KEY_1 ? 0 : key == UI_KEY_2 ? 1 : 2));
    if (key == UI_KEY_2) {
        if (s_view == BULK) s_view = SHELF;
        else if (s_view == SHELF) { ui_nav_request(ctx, 3); return APP_REDRAW_NONE; }
        else if (s_view == READING) {
            s_reader_slider = -1;
            s_reader_panel = s_reader_panel == READER_PANEL_TOOLS ? READER_PANEL_NONE : READER_PANEL_TOOLS;
            invalidate_prep();
        } else if (s_view == TOC) {
            set_reader_view(s_text ? READING : SHELF);
            s_reader_panel = READER_PANEL_NONE;
        } else {
            ui_nav_request(ctx, 0);
            return APP_REDRAW_NONE;
        }
        return APP_REDRAW_PAGE;
    }
    if (key != UI_KEY_1 && key != UI_KEY_3) return APP_REDRAW_NONE;
    int dir = key == UI_KEY_1 ? -1 : 1;
    if (s_view == READING) { s_stats_activity_ms = ctx->now_ms; return turn_page(ctx, dir); }
    int next = ctx->leaf + dir;
    if (next < 0 || next >= leaves()) return APP_REDRAW_NONE;
    ctx->leaf = next;
    return APP_REDRAW_PAGE;
}
static app_redraw_t on_power_short(app_ctx_t* ctx) {
    s_reader_tap.pending = false;
    // M2 Step9 铁律①：详解态电源键 = 翻详解页（沿用用户的电源翻页设置；
    // 末页停滞回 APP_REDRAW_DONE，与原书路径同口径——按了不退出）。
    // M2 Step9 law 1: the power key turns detail pages (honouring the user's
    // power-turn setting; a stall answers APP_REDRAW_DONE, same as the source
    // path - pressed means pressed, never an exit).
    if (s_view == READING && ai_reader_active()) {
        if (!app_settings_reader_power_turn()) return APP_REDRAW_NONE;
        app_redraw_t result = reader_ai_turn(1);
        return result == APP_REDRAW_NONE ? APP_REDRAW_DONE : result;
    }
    if (s_view != READING || !s_text || s_clear_confirm ||
        !app_settings_reader_power_turn()) return APP_REDRAW_NONE;
    s_stats_activity_ms = ctx->now_ms;
    app_redraw_t result = turn_page(ctx, 1);
    return result == APP_REDRAW_NONE ? APP_REDRAW_DONE : result;
}
static app_redraw_t on_key_long_impl(app_ctx_t* ctx, int key);
// 框选态按键：先退出，再照常执行原键义（E5）；键本身不改页面时补一次重绘，
// 免得反色留在屏上。
// A key in the selection mode exits first and then does its usual job (E5); when the
// key changes nothing we repaint once so no inverse band is left on screen.
static app_redraw_t on_key_long(app_ctx_t* ctx, int key) {
    if (boxsel_active()) {
        (void)boxsel_exit(BOXSEL_EXIT_KEY_LONG);
        const app_redraw_t r = on_key_long_impl(ctx, key);
        return r == APP_REDRAW_NONE ? APP_REDRAW_PAGE : r;
    }
    return on_key_long_impl(ctx, key);
}
static app_redraw_t on_key_long_impl(app_ctx_t* ctx, int key) {
    s_reader_tap.pending = false;
    // M2 Step9 铁律①：详解态长按 KEY2 = 关闭详解回原书（不开工具栏、不返回
    // 主页——覆盖层只有一个出口）。其余长按忽略。/ M2 Step9 law 1: in a
    // detail session a KEY2 long-press closes back to the source (no toolbar,
    // no home - the overlay has exactly one exit). Other long-presses ignore.
    if (s_view == READING && ai_reader_active()) {
        if (key == UI_KEY_2) return ai_reader_close_full();
        return APP_REDRAW_NONE;
    }
    if (key != UI_KEY_2) return APP_REDRAW_NONE;
    if (s_view == TOC && s_toc_jump_open) {
        s_toc_jump_open = s_toc_jump_drag = false;
        return APP_REDRAW_PAGE;
    }
    if (s_view != READING && s_view != TOC) { ui_nav_request(ctx, 0); return APP_REDRAW_NONE; }
    if (s_view == READING) {
        return reader_key_action(ctx, app_settings_reader_hold_action());
    }
    s_message[0] = 0;
    set_reader_view(s_text ? READING : SHELF);
    s_reader_panel = READER_PANEL_NONE;
    return APP_REDRAW_PAGE;
}
static bool menu_handle_enabled(app_ctx_t* ctx) {
    (void)ctx;
    // 图书所有视图只使用产品导航，旧菜单不能抢占右侧分页按钮。
    // All book views use product navigation; the legacy menu must never steal the right pager.
    return false;
}
static app_redraw_t shelf_turn_page(app_ctx_t *ctx, int direction) {
    int next = ctx->leaf + direction;
    if (s_view != SHELF || s_scan_pending || s_clear_confirm || next < 0 || next >= leaves())
        return APP_REDRAW_NONE;
    ctx->leaf = next;
    shelf_category_remember(ctx);
    s_category_menu=false;
    s_shelf_page_pending = true;
    s_pressed_control = -1;
    prepare_covers(ctx);
    render(ctx, ctx->fb);
    s_area = (EpdRect){0, 176, UI_LOCK_WIDTH, 912};
    s_mode = MODE_GL16;
    return APP_REDRAW_AREA;
}

// 键事件与原始边沿可能属于同一份报告，每轮最多执行一次；其他子页只清理旧输入。
// Keys and raw edges may come from the same report: apply one turn per tick, discard on other subpages.
static int book_remote_direction(void) {
    int direction = 0;
    ble_pt_event_t event;
    while (ble_pt_pop_key(&event)) {
        ble_pt_action_t action = ble_pt_action_for_usage(event.usage, event.mods);
        if (action == BLE_PT_ACTION_PREV) direction = -1;
        else if (action == BLE_PT_ACTION_NEXT) direction = 1;
    }
    ble_pt_raw_t raw;
    while (ble_pt_pop_raw(&raw)) {
        if (!raw.pressed) continue;
        ble_pt_action_t action = ble_pt_action_for_frame(&raw.frame);
        if (action == BLE_PT_ACTION_PREV) direction = -1;
        else if (action == BLE_PT_ACTION_NEXT) direction = 1;
    }
    return direction;
}
/* ---- gen26.5 Step10：TTS 听书（0x19 页文本 + 跟随轮询）----
 * gen26.5 Step10: TTS read-aloud (0x19 page text + follow polling).
 *
 * 单一轮询源：app_book 的 on_tick。不接 GATT 回调、不挂翻页漏斗——页变了就
 * 推，没变就不推，哨兵去重。One polling source: app_book's on_tick. No GATT
 * callback wiring and no turn-page funnel hook - push when the page changed,
 * stay quiet when it did not, with the sentinel deduplicating.
 */

// 当前视觉页在章内的字节区间：双栏取「左栏起 → 右栏的下一页止」，JOINT/单栏/
// 落单左栏取「本页起 → 下一页止」，末页到章尾。s_page 恒为视觉页起始逻辑页
// （turn/load/跳转全路径保证），故先归视觉页再取左右栏，不把逻辑页当视觉页用。
// The current visual page's byte range inside the chapter: two-column runs from
// the left column to the page after the right one; JOINT/single/lone-left runs
// from this page to the next; the last page runs to the chapter end. s_page is
// always a visual page's starting logical page (guaranteed on every turn/load/
// jump path), so fold to the visual page first - never pass a logical page
// where a visual index is expected.
static bool tts_page_range(size_t *start, size_t *end) {
    if (s_view != READING || !s_text || !s_text_len) return false;
    size_t left = 0, right = SIZE_MAX;
    reader_logical_pages(reader_visual_index(s_page), &left, &right);
    /* 伴读态（R0 ⑤⑦）：reader_logical_pages 显式把 right 钉成 SIZE_MAX，
       after = left + 1，区间恒为主栏当前逻辑页——页文本上行天然只取左 70% 主栏，
       零分支、零新增拷贝（关态逐位不变）。 */
    const size_t begin = book_layout_page_start_offset(left);
    const size_t after = (right != SIZE_MAX ? right : left) + 1;
    size_t stop = after < book_layout_page_count() ? book_layout_page_start_offset(after)
                                                   : s_text_len;
    // 布局怪态（after 页起始回退）钳为空区间：推空页即静默页，不卡死轮询。
    // A layout oddity (a backwards page start) clamps to an empty range: an
    // empty page reads as silence and never stalls the poll.
    if (stop < begin) stop = begin;
    *start = begin;
    *end = stop;
    return true;
}

// 跟随轮询：开关开着 + 阅读中 + 链路就绪 + 页变了 → 抽当前视觉页文本推 0x19。
// 推送失败不更新哨兵，下轮原样重发；空页（纯插图/纯空白）不推——推了会把手机上
// 正在朗读的上一页切断，静默即是正确语义。抽取在 draw 锁内做（与点词取句同口径：
// 有界扫描不拉长持锁时间），发送在锁外做（BLE 通知可能带重投，不能堵渲染）。
// Follow polling: switch on + reading + link up + page changed -> extract the
// current visual page's text and push it as 0x19. A failed push leaves the
// sentinel untouched and the next round resends as-is; an empty page
// (illustration-only/blank) is never pushed - pushing it would cut off the
// phone's current utterance, and silence is the correct semantics. Extraction
// runs under the draw lock (the same rule as tap-to-lookup's sentence capture:
// a bounded scan must not stretch lock hold time) and the send runs outside it
// (a BLE notify may carry retries and must never stall rendering).
static void tts_follow_poll(void) {
    const bool up = push_sink_link_up();
    // D2B 重连补发：刚建立链接（down→up 跃迁）且开关为关，补推空页让对端停
    // 朗读。覆盖「断线期间关开关、重连后开关仍为关」的漏洞——此时 D2A 的即时
    // 空页在断线时发不出去，只能等重连这一刻补。开关为开则交给下方哨兵自然
    // 续推（重连后当前页首推），无需空页。/ D2B reconnect resend: a down->up
    // edge with the switch off re-pushes an empty page so the peer stops. This
    // closes the "switch turned off while disconnected" gap - D2A's immediate
    // empty page couldn't air during the outage, so it is made up at reconnect.
    // Switch on falls through to the sentinel re-push below (current page first
    // push after reconnect), no empty page needed.
    /* gen30 格 1：门槛改走 reader_tts_active() 单一判据（伴读态临时覆盖跟读开关）。
       D2B 重连补发空页在伴读态必须跳过——伴读朗读归伴读管，重连不应把手机 TTS 静音；
       关态（伴读未开）reader_tts_active() == tts_switch_on()，逐位不变。
       gen30 ruling grid 1: the gate goes through the single reader_tts_active()
       predicate. The D2B reconnect empty page is skipped in companion mode so a
       reconnect never mutes the phone; off-state behaviour is bit-identical. */
    if (up && !s_tts_link_was_up && !reader_tts_active()) {
        push_sink_tts_send_page("", 0);
    }
    if (up && !s_tts_link_was_up) {
        /* 修复轮解耦：重连时同步一次朗读使能位——手机端可能冷启（默认为开），
           必须与设备侧开关对齐，否则「开关关着却开念」会复现。 */
        (void)push_sink_tts_enable_send(tts_switch_on());
    }
    s_tts_link_was_up = up;
    if (!reader_tts_active() || s_view != READING || !s_text) return;
    if (!up) return;
    if (s_chapter == s_tts_last_chapter && s_page == s_tts_last_page) return;
    char text[TTS_PAGE_TEXT_MAX];
    size_t start = 0, end = 0, n = 0;
    lock_draw();
    const bool ranged = tts_page_range(&start, &end);
    if (ranged) n = tts_page_extract(s_text, s_text_len, start, end, text, sizeof(text));
    unlock_draw();
    if (!ranged) return; // 版面未就绪：哨兵不碰，下轮再试 / layout not ready: sentinel untouched
    if (n == 0) {
        // 空页：无字可读。记哨兵（这页算推过了），不发帧。/ Empty page: nothing to
        // read. Record the sentinel (this page counts as done) and air nothing.
        s_tts_last_chapter = s_chapter;
        s_tts_last_page = s_page;
        return;
    }
    /* gen30 步 3 接线 3（翻页流）：0x1C PAGE_BEGIN 前置挂在全仓唯一 0x19 推送点上，
       仅伴读态生效。对账基准先落再投递由组件层 push_sink_companion_page_begin() 完成
       （同批清上一页未取回包），设备端只改调用侧；关态一字节不变。空页（n==0）不发
       0x19，故也不发 0x1C，保证「0x1C+0x19」始终成对。0x19 失败时哨兵不动，下轮
       同页重发，组件层清旧回包保证幂等。
       gen30 step 3 item 3 (page stream): PAGE_BEGIN is prefixed onto the single 0x19
       push point, companion only. The component drops the reconciliation base and
       clears stale receipts in the same batch; the device side only changes the call
       site. Off-state is byte-identical. An empty page (n==0) airs neither frame, so
       PAGE_BEGIN+TTS_TEXT always travel as a pair. A failed 0x19 leaves the sentinel
       untouched; the same page is re-aired next round, idempotent thanks to the
       component clearing stale receipts. */
    if (reader_companion_active()) {
        const uint8_t mode = (uint8_t)(app_settings_companion_mode() % PUSH_RIDE_MODE_COUNT);
        (void)push_sink_companion_page_begin((uint16_t)s_page, mode);
    }
    if (push_sink_tts_send_page(text, n)) {
        s_tts_last_chapter = s_chapter;
        s_tts_last_page = s_page;
        ESP_LOGI(TAG, "tts push chapter=%u page=%u bytes=%u",
                 (unsigned)s_chapter, (unsigned)s_page, (unsigned)n);
    }
}

/* ---- gen30 伴读案 · 步 3 接线：进/出伴读动作 + TTS_DONE 消费（公主按板五格终局） ----
 * 射程：产品码「谁在什么时机调用、按什么语义调用」。组件层 5 个 API 与方案 B 分派层
 * 一字节不动；不动 s_sess / 广播 / 连接（格 2 终裁 A + 复用，延迟回收为已知边界）。
 * Scope: product-side "who calls what, when, with which semantics". The component's
 * five APIs and the plan-B dispatch layer stay untouched; s_sess / advertising /
 * the connection are never torn down (ruling grid 2 plan A + reuse). */

/* 接线 2：进伴读。横屏机械前置 → 置位 + reader_relayout() 同批原子 → 武装复用/开窗
   → FAIR 闩锁 → 0x1B 初始广播(source=2)，随后本 tick 的 tts_follow_poll 发 0x1C+0x19。 */
static void companion_enter(void) {
    // 横屏机械前置：未开横屏只引导，不自动改设置（哨位裁定 §三格 1，尊重用户主动动作）。
    // Landscape mechanical prerequisite: hint only, never auto-flip the setting.
    if (!app_settings_reader_landscape()) {
        copy_text(s_reader_notice, sizeof(s_reader_notice), "请先开启横屏阅读");
        s_reader_notice_until = esp_timer_get_time() / 1000 + 2200;
        return;
    }
    if (reader_companion_active()) return;
    // 同批原子：置位与重排连续完成，中间不渲染，不出现「置位先生效、重排后到」。
    // One atomic batch: the state flip and the reflow run back-to-back with no frame
    // in between; there is never a "state first, reflow later" intermediate state.
    reader_companion_set(true);
    if (!reader_relayout(body_rect())) {
        // 失败回滚到关态并按旧几何重建（与 select_reading_font_item 同款体例）；
        // 失步时既有切片宽守卫拒画亦为哨位裁定接受的可失败模式。
        // Roll back on failure and rebuild at the old geometry (same shape as the
        // font switch); a size-guard refusal is also an accepted failure mode.
        reader_companion_set(false);
        (void)reader_relayout(body_rect());
        copy_text(s_reader_notice, sizeof(s_reader_notice), "伴读开启失败");
        s_reader_notice_until = esp_timer_get_time() / 1000 + 2200;
        return;
    }
    invalidate_prep();
    // 格 2：已武装/已连接一律复用，未武装才开窗（PUSH_ENTRY_WINDOW_MS 同款 30 分钟）。
    // Grid 2: reuse an armed/connected session; open the window only when unarmed.
    if (!push_sink_armed()) (void)push_sink_arm(0, READER_COMPANION_ARM_WINDOW_MS);
    // 格 3：FAIR 闩锁（组件层 conn_params_apply 抑制既有 session 驱动的 SLOW 回落）。
    push_sink_companion_conn(true);
    // 格 1：先发 0x1B 初始广播（source=2），再重置哨兵让本 tick 的轮询强制首推当前页。
    // Grid 1: MODE_SET(init) first, then a sentinel reset forces the first page push.
    const uint8_t mode = (uint8_t)(app_settings_companion_mode() % PUSH_RIDE_MODE_COUNT);
    (void)push_sink_companion_mode_set(mode, PUSH_MODE_SRC_INIT);
    /* 修复轮解耦：进伴读即同步一次朗读使能位。开关关着进伴读＝只看不听（摘要照常
       生成、右栏照常更新）；开关开着＝边看边听。手机端只按这一位决定念不念，
       页文本推送本身不再表达「念不念」的语义。 */
    (void)push_sink_tts_enable_send(tts_switch_on());
    s_tts_last_chapter = s_tts_last_page = SIZE_MAX;
    s_companion_summary_has = false;   /* 步 4：进伴读从占位态起，不沿用上一会话摘要 */
    s_companion_du_count = 0;
}

/* 接线 4：出伴读。四动作同批原子，顺序＝先停流后清档；不动 s_sess / 广播 / 连接。 */
static void companion_exit(void) {
    if (!reader_companion_active()) return;
    // ① 停 0x1C 流 + 置位复位同批重排：置位为关后门槛失效，不再发 0x1C/0x19。
    // Stop the stream + flip the state with one atomic reflow: the gate closes.
    reader_companion_set(false);
    if (!reader_relayout(body_rect())) {
        // 失败回滚：保持伴读态，不发空页、不清档，避免「停了朗读却没退伴读」。
        // 步 4 ④：清单轮「交互留滞」已知边界承接——补一句气泡反馈，不再静默留态。
        reader_companion_set(true);
        (void)reader_relayout(body_rect());
        reader_notice_set("退出伴读失败 · 仍在伴读", 2200);
        return;
    }
    invalidate_prep();
    s_tts_last_chapter = s_tts_last_page = SIZE_MAX;
    // ② 空页帧停手机 TTS（格 1 终裁，复用既有 0x19 len=0 静默语义）。
    if (push_sink_link_up()) (void)push_sink_tts_send_page("", 0);
    // ③ 清档：清 FAIR 闩锁 + 显式回 SLOW（组件层一次完成，格 2/格 3）。
    push_sink_companion_conn(false);
    s_companion_summary_has = false;   /* 步 4：出伴读清占位态与右栏 DU 计数 */
    s_companion_du_count = 0;
}

/* 接线 6：TTS_DONE 按 NVS 设置消费（R0 拍板格 5 设备端自治，组件层不查询使能态）。
   页号对账：组件层已按 PAGE_BEGIN 基准过滤，产品侧再核当前页——页面已翻走即丢弃。
   仅 FINISHED 且 rd_companion_turn 开时自动翻页；ABORT/FAILED 与关态一律忽略。
   Item 6: consume TTS_DONE from the NVS setting (device autonomy; the component
   never queries the flag). The component already reconciled against PAGE_BEGIN;
   the product re-checks the current page so a stale receipt is dropped. Only a
   FINISHED receipt with auto-turn on advances one page; ABORT/FAILED and off are ignored. */
static app_redraw_t companion_tts_done_poll(app_ctx_t* ctx) {
    if (!reader_companion_active()) return APP_REDRAW_NONE;
    uint16_t page_no = 0;
    uint8_t status = 0;
    if (!push_sink_take_companion_tts_done(&page_no, &status)) return APP_REDRAW_NONE;
    if (page_no != (uint16_t)s_page) return APP_REDRAW_NONE;
    if (!app_settings_companion_turn() || status != PUSH_TTS_DONE_FINISHED) return APP_REDRAW_NONE;
    if (s_view != READING || !s_text || s_reader_panel != READER_PANEL_NONE) return APP_REDRAW_NONE;
    return turn_page(ctx, 1);
}

/* 步 4 ①：SUMMARY 消费点 + 右栏区域刷新决策（R0 冻结件 2/3）。
   组件层已按 PAGE_BEGIN 基准做页号对账，产品侧再核当前页（与 TTS_DONE 同款）；
   取到的文本做 56 字码点硬截二次截断后落右栏缓冲。刷新走既有 s_area/s_mode +
   APP_REDRAW_AREA 出口（同 excerpt_notice 范式：先整帧重画，再只推右栏区域）——
   不改 present()，不触碰其翻页刷新主链与宿主守卫抽取面。冻结件 3 的计数与正文
   s_du_count 完全独立；波形沿用 present() 通用出口，实际观感待真机十一格观察。
   Step-4 SUMMARY consumption + right-column refresh decision; the DU counter is
   deliberately independent of the body's s_du_count (R0 frozen item 3). */
static app_redraw_t companion_summary_poll(app_ctx_t* ctx) {
    if (!reader_companion_active() || s_view != READING || !s_text) return APP_REDRAW_NONE;
    uint8_t mode = 0, idx = 0, total = 0;
    uint16_t page_no = 0;
    /* 769 B 不放栈：on_tick 链上已有重排与绘制调用，栈余量比这 0.5 KB 贵。
       本函数只在主任务 on_tick 里跑，静态缓冲无重入。 */
    static char raw[PUSH_SUMMARY_TOTAL_MAX + 1u];
    if (!push_sink_take_companion_summary(&mode, &page_no, &idx, &total,
                                          raw, sizeof(raw))) return APP_REDRAW_NONE;
    (void)mode;
    if (page_no != (uint16_t)s_page) return APP_REDRAW_NONE;
    reader_companion_clip_cap(raw, s_companion_summary, sizeof(s_companion_summary));
    s_companion_summary_idx = idx;
    s_companion_summary_total = total;
    s_companion_summary_has = true;
    const bool gc16 = ++s_companion_du_count >= READER_COMPANION_GC16_EVERY;
    if (gc16) s_companion_du_count = 0;
    render(ctx, ctx->fb);
    s_area = reader_column_rect(body_rect(), reader_column_count(), 1);
    s_mode = gc16 ? MODE_GC16 : MODE_DU;
    /* 修复轮 S2 低侵入插桩（23:18 裁定 §四，随修复件带上）：
       打印右栏 DU 区域四元组与屏幕尺寸。故障三的三个待验证嫌疑之一是「右栏 DU
       区域未做屏边界钳制」；若换书序列仍 panic，这行日志能直接证实/证伪越界，
       省掉一轮专门刷插桩件的往返。低侵入：只在收到摘要时打一行，无分支改动。 */
    /* 取证日志③（14:26 裁定 §二第 5 条）：补打 page_no，与 turn chapter/page
       及手机侧「请求发起 page=」对齐，现场「前进三页退两页」据此逐页核对内容归属。 */
    ESP_LOGI(TAG, "companion: summary page_no=%u idx=%u/%u gc16=%d len=%u area=(%d,%d %dx%d) scr=%dx%d",
             (unsigned)page_no,
             (unsigned)s_companion_summary_idx, (unsigned)s_companion_summary_total,
             (int)gc16, (unsigned)strlen(s_companion_summary),
             s_area.x, s_area.y, s_area.width, s_area.height,
             epd_width(), epd_height());
    return APP_REDRAW_AREA;
}

static app_redraw_t on_tick(app_ctx_t* ctx) {
    /* gen30 步 3：本 tick 是否执行了进/出伴读同批原子动作——用于在本 tick 的
       tts_follow_poll() 之后收口一次整页重绘，保证 0x1B 与 0x1C+0x19 仍同批发出。
       gen30 step 3: whether this tick ran the atomic enter/exit batch; used to
       close the tick with one full-page redraw AFTER the follow poll, so MODE_SET
       and PAGE_BEGIN+TTS_TEXT still go out in the same batch. */
    bool companion_batch = false;
    // 框选超时 / 换页 / 换视图：三条路都收口到同一次复位（E4/E7）。
    // Timeout, page drift and view drift all land in the same reset (E4/E7).
    if (s_boxsel.state != BOX_STATE_OFF &&
        (box_select_expired(&s_boxsel, ctx->now_ms) || s_view != READING ||
         s_boxsel.page != s_page || !s_text)) {
        return boxsel_exit(BOXSEL_EXIT_TIMEOUT);
    }
    // S26-2 家务片：一个循环片至多落一条摘抄（答辩书一.4：书架扫描永远插得进来）。
    // 先写后亮：只有这里 f_sync 成功，页脚才许出现「已存 ✓」。
    // S26-2 housekeeping slot: at most one excerpt per loop turn (answer one point four: the
    // shelf scan always interleaves). Write first, tick second: only a successful f_sync
    // here earns the footer its "saved" receipt.
    if (excerpt_footer_tick(ctx)) return APP_REDRAW_AREA;
    // M3-B：情景请求超时销号。等不到答案就撤——卡片由既有 pending 超时降级为
    // 失败提示，这里只管 armed，杜绝「发了请求就永远在等」的僵尸态。
    // M3-B: expire the scene request. No answer means disarm; the card degrades
    // through the existing pending timeout, and this only owns `armed`, so a
    // request can never sit waiting forever.
    // 26.11.3-C2：日志不再打印常量。两条路径（词 16 s / 段落 90 s）各有窗口，
    // 打印单一常量等于对其中一条撒谎；改报「哪条路径 + 到期时刻」。
    // 26.11.3-C2: the log no longer prints the constant. Two paths have two windows
    // (word 16 s, passage 90 s), so a single figure would lie about one of them; it
    // reports the path and the expiry instant instead.
    if (scene_req_expire(&s_scene_req, ctx->now_ms)) {
        ESP_LOGW(TAG, "scene timeout: word=\"%s\" excerpt=%d deadline=%lldms",
                 s_scene_req.word, (int)s_scene_req.excerpt,
                 (long long)s_scene_req.deadline_ms);
    }
    // gen18（18-a 自动切回）：读手机推来的虚拟书时，sink 在后台保活；手机宽限内
    // 直接再发（COOLDOWN→RECEIVING）时，自动切回传书页接客——它的泵现成，进度/状态/
    // 开书全走既有路径。用户零手动操作。
    // gen18 (18-a auto-switch-back): while reading the phone-pushed virtual
    // book the sink is background-alive; when the phone pushes again inside the
    // grace (COOLDOWN->RECEIVING), bounce back to the transfer page to receive -
    // its pump is ready and progress, status and book-opening all follow
    // existing paths. Zero manual steps.
    // gen26.3② 收紧（M1 联测根因）：原条件把 CONNECTED 也算「推送在飞」，但 gen25
    // 后台保活让 APK 监听会话长驻 CONNECTED（链路就绪≠数据在飞），用户开 push.txt
    // 阅读时每次 tick 都被弹回传书页（联测三次 ~590ms 全弹）。现仅
    // RECEIVING/PROCESSING——数据真正在飞——才弹；CONNECTED 长驻不弹，
    // M1.1（B②）推完恢复监听的流程同样不弹。
    // gen26.3 scope-2 tightening (M1 live-test root cause): the old condition
    // counted CONNECTED as "a push in flight", but the gen25 background keep-alive
    // parks an APK listening session in CONNECTED (link up != data flying), so
    // every tick bounced the user out of push.txt back to the transfer page (all
    // three live-test opens bounced at ~590ms). Now only RECEIVING/PROCESSING -
    // data actually in flight - bounce; a parked CONNECTED never does, and
    // neither does M1.1's (B2) post-push listen-restore path.
    // COOLDOWN 不触发：刚开书那两秒手机还在，别把用户弹回去。
    // COOLDOWN does not trigger: the phone is still around for a couple of
    // seconds right after the book opens - do not bounce the user back then.
    if (s_view == READING && s_path[0] && strcmp(s_path, PUSH_TEXT_PATH) == 0 && push_sink_armed()) {
        const push_state_t st = push_sink_state();
        if (st == PUSH_STATE_RECEIVING || st == PUSH_STATE_PROCESSING) {
            // M2 Step9：推送在飞要弹回传书页——详解会话先关（状态卫生：不回
            // 到一个「详解还开着」的传书页）。原书位置由 app_transfer 链路保管。
            // M2 Step9: a push in flight bounces to the transfer page - close
            // any detail session first (state hygiene: never land on the
            // transfer page with a session still open). The source position is
            // kept by the app_transfer chain.
            if (ai_reader_active()) ai_reader_close();
            s_push_handoff = true;
            extern const app_desc_t app_transfer;
            ctx->request_app = &app_transfer;
            return APP_REDRAW_NONE;
        }
    }
    // gen26.3 ④ 查询卡超时降级：miss 点词弹了「AI 查询中…」卡，手机 15s 不回帧
    // （查询失败/链路断）就不能让卡片永远假活着——关卡 + 失败提示，与「真实状态
    // 非假卡」同口径。/ gen26.3 querying-card timeout: after a miss tap opened
    // the "AI querying..." card, a phone that never answers within 15s (query
    // failed / link down) must not leave the card faking forever - close the
    // card and show the failure, the same "real state, not a fake card" stance.
    // 26.11.31 退出路径单点收敛：挂起词还在、但卡片已不处于 armed 等待态
    // （翻页/换书/关卡通/用户取消都会 reset 卡片、清掉 armed_wait）→ 设备侧取消
    // 挂起并停广播，杜绝关卡后残留挂在空中、下次连接误发。连接建成 take 后 pending
    // 已为 false，不走此路；正常 armed 等待 pending=true && armed_wait=true 也不走。
    if (push_sink_dict_ai_pending() && !dict_popup_armed_wait()) {
        push_sink_cancel();
    }
    // 26.11.31：armed 等待卡按 60s 广播窗口判超时，连上直查卡仍按 15s。
    const int64_t dict_timeout_us = dict_popup_armed_wait()
                                        ? DICT_AI_ARMED_TIMEOUT_US
                                        : DICT_AI_PENDING_TIMEOUT_US;
    if (s_view == READING && s_reader_panel == READER_PANEL_DICT &&
        dict_popup_pending_expired(dict_timeout_us)) {
        ESP_LOGW(TAG, "ai pending card timed out after %llds (armed=%d)",
                 (long long)(dict_timeout_us / 1000000), (int)dict_popup_armed_wait());
        // gen26.4：查询态由词典命中卡的「AI」入口强制触发时——失败不丢词典内容，
        // 回退重开词典释义卡（该词此前命中，必再命中），面板保持 DICT。
        // gen26.4: when the querying state was forced from the dict card's AI entry,
        // a failure must not lose the dictionary content - fall back by re-opening
        // the dictionary definition card (the word hit before, so it hits again),
        // panel stays DICT.
        if (dict_popup_ai_over_dict()) {
            char w[64];
            copy_text(w, sizeof(w), dict_popup_word());
            dict_popup_open(w);
            dict_popup_set_ai_enabled(push_sink_ai_enabled()); // open 的 reset 清了它，重置
            invalidate_prep();
            return APP_REDRAW_PAGE;
        }
        dict_popup_reset();
        s_reader_panel = READER_PANEL_NONE;
        copy_text(s_reader_notice, sizeof(s_reader_notice), "AI 查询失败");
        s_reader_notice_until = esp_timer_get_time() / 1000 + 2200;
        invalidate_prep();
        return APP_REDRAW_PAGE;
    }
    /* gen30 步 3：进/出伴读动作在 on_tick 执行（触摸回调只置 pending），置位与重排
       同批原子。此处不 return——让本 tick 稍后的 tts_follow_poll() 把 0x1C+0x19 流
       接在 companion_enter() 的 0x1B 之后，收口重绘放在轮询之后。
       gen30 step 3: the atomic enter/exit batch runs here (touch callbacks only
       set a pending flag). No return yet - this tick's follow poll must append
       PAGE_BEGIN+TTS_TEXT after companion_enter()'s MODE_SET; the redraw follows
       the poll. */
    if (s_companion_enter_pending || s_companion_exit_pending) {
        const bool enter = s_companion_enter_pending;
        s_companion_enter_pending = s_companion_exit_pending = false;
        companion_batch = true;
        if (enter) companion_enter();
        else companion_exit();
    }
    // 字体开关留下的重排在这里做：触摸回调的栈太深，一章排下来会把主任务栈顶穿（实测溢出过）。
    // The font switch's re-typeset happens here: the touch callback runs too deep, and laying out a
    // chapter from there overran the main task stack (measured).
    if (s_fonts_reload_pending) {
        s_fonts_reload_pending = false;
        if (s_text && book_kind() == BOOK_KIND_EPUB)
            (void)load_chapter(ctx, s_chapter, book_layout_page_start_offset(s_page), false);
        return APP_REDRAW_PAGE;
    }
    // M2 Step9：详解态不清原书双击翻页判定——覆盖层下单击/双击不产生任何
    // 原书语义（防御性清理：pending 只可能由会话前的输入留下）。
    // M2 Step9: a detail session never feeds the source double-tap turn - under
    // the overlay a tap produces no source semantics at all (defensive clear:
    // a pending can only be left by pre-session input).
    if (s_view != READING || ai_reader_active() || (s_toolbar && s_reader_panel != READER_PANEL_TOOLS) || s_clear_confirm || ctx->consumed) s_reader_tap.pending = false;
    else {
        int tap = book_reader_tap_tick(&s_reader_tap, ctx->now_ms, ctx->touch && ctx->touch->touched);
        if (tap) return turn_page(ctx, tap);
    }
    // gen26.5 Step10 听书跟随轮询放在翻页判定之后：本轮翻页若改了页，下一 tick
    // 的轮询自然推到新页（哨兵比对 (章,页)）；此处调用只承担「页没变但链路刚
    // 通/开关刚开」的首推与失败重发。/ The Step10 TTS follow poll sits after the
    // turn judgement: a turn that changed the page is picked up by the next
    // tick's poll (the sentinel compares (chapter,page)); this call only covers
    // the first push when the link or the switch just came up, plus resends.
    /* gen30 步 3 接线 6：TTS_DONE 消费放在 0x19 轮询之前——若按 NVS 设置自动翻页，
       本 tick 直接收口，新页由下一 tick 的哨兵自然首推，不会先推旧页再翻。
       gen30 step 3 item 6: consume TTS_DONE before the 0x19 poll. If the NVS
       setting drives an auto turn, this tick closes out and the new page gets its
       first push from next tick's sentinel - the old page is never pushed first. */
    {
        const app_redraw_t done_redraw = companion_tts_done_poll(ctx);
        if (done_redraw != APP_REDRAW_NONE) return done_redraw;
    }
    tts_follow_poll();
    // 进/出伴读的同批动作在 0x1B / 空页帧发出后收口一次整页重绘（70/30 版面可见）。
    if (companion_batch) return APP_REDRAW_PAGE;
    {
        const app_redraw_t sum_redraw = companion_summary_poll(ctx);
        if (sum_redraw != APP_REDRAW_NONE) return sum_redraw;
    }
    track_ticket_stats(ctx);
    if (s_view == SEARCH || s_view == EDIT) {
        if (ctx->consumed) return APP_REDRAW_NONE;
        bool held = !ctx->released && ctx->touch && ctx->touch->touched && ctx->touch->count == 1;
        if (ui_keyboard_hold_tick(held, ctx->touch ? ctx->touch->x : 0,
            ctx->touch ? ctx->touch->y : 0, ctx->now_ms) == UI_KEYBOARD_CHANGED) return editor_paint(ctx, false);
        return ui_keyboard_idle_tick(ctx->touch && ctx->touch->touched, ctx->now_ms) == UI_KEYBOARD_CHANGED
            ? editor_paint(ctx, false) : APP_REDRAW_NONE;
    }
    if (ctx->consumed) return APP_REDRAW_NONE;
    // 上/左为上一页，下/右为下一页；书架复用滑动的局部刷新，不闪动题头和底栏。
    // Up/left turn back, down/right turn forward; the shelf reuses swipe refresh without flashing chrome.
    if (app_settings_ble_turner()) {
        int ble_dir = book_remote_direction();
        if (ble_dir && s_view == SHELF) {
            app_redraw_t redraw = shelf_turn_page(ctx, ble_dir);
            if (redraw != APP_REDRAW_NONE) return redraw;
        } else if (ble_dir && s_view == READING) {
            s_stats_activity_ms = ctx->now_ms;
            // M2 Step9 铁律①：BLE 遥控器翻页在详解态同样隔离——翻详解页，
            // 不到原书。/ M2 Step9 law 1: BLE remote turns are isolated too -
            // they turn detail pages, never the source book.
            if (ai_reader_active()) return reader_ai_turn(ble_dir);
            return turn_page(ctx, ble_dir);
        }
    }
    // 26.11.3-C2 等待期提示：到点且「整段请求仍挂着」才画。答案到了就销号
    // （scene_req_claim 已 armed=false），这条自然不响——不用另写清理。
    // 判据（复测卡）：等待期阅读可继续（只刷气泡区域，不重排正文）、卡到即弹、
    // 无假死（90 s 内必有一边落地：答案或超时日志）。
    // 26.11.3-C2 wait notice: paint only if the passage request is still armed when
    // the timer fires. A delivered answer disarms it, so this simply never fires and
    // needs no separate cleanup. Acceptance: reading stays usable (only the toast
    // region refreshes, the body is not re-typeset), the card pops on arrival, and
    // the wait never fakes life - within 90 s either the answer or the timeout lands.
    if (s_ai_wait_notice_at_ms && ctx->now_ms >= s_ai_wait_notice_at_ms) {
        s_ai_wait_notice_at_ms = 0;
        if (s_view == READING && s_scene_req.armed && s_scene_req.excerpt) {
            reader_notice_set("AI 解析中 · 最长约 1 分钟", 4000);
            render(ctx, ctx->fb);
            s_area = reader_notice_area();
            s_mode = MODE_GL16;
            return APP_REDRAW_AREA;
        }
    }
    if (s_reader_notice[0] && ctx->now_ms >= s_reader_notice_until) {
        ESP_LOGI(TAG, "notice: hide \"%s\"", s_reader_notice); // B63：show/hide 两头留痕
        s_reader_notice[0] = 0;
        if (s_view == READING) {
            // 区域刷新不会自动重绘帧缓冲；先移除提示，再只更新提示所在区域。
            // Area refreshes do not rerender the framebuffer; erase the toast before updating its region.
            // 几何与绘制处共用 reader_notice_area()：横屏下气泡中心随 ui_w() 移动，
            // 旧硬编码 {208,150,268,68} 擦不到（26.11.1 toast 回执第 2 条）。
            render(ctx, ctx->fb);
            s_area = reader_notice_area();
            s_mode = MODE_GL16;
            return APP_REDRAW_AREA;
        }
    }
    if (s_save_failed && ctx->now_ms - s_save_retry_ms >= 15000) {
        s_save_retry_ms = ctx->now_ms;
        bool failed = s_save_failed;
        retry_progress();
        if (failed != s_save_failed) { invalidate_prep(); return APP_REDRAW_PAGE; }
    }
    app_redraw_t background = reader_background_tick();
    if (background != APP_REDRAW_NONE) return background;
    // 26.9.21：250 ms 而不是 500。挂载本身只要 0.36 s，剩下的全在等这一拍；
    // 半秒的轮询把「秒出」卡在 0.9 s 的边缘上。`read_pico_sd_get_info()` 只读内存快照
    // 加一次 CD 引脚采样，代价可以忽略。
    // 26.9.21: 250 ms instead of 500. The mount takes 0.36 s; the rest was this poll.
    if (s_scan_pending && ctx->now_ms - s_poll_ms >= 250) {
        s_poll_ms = ctx->now_ms;
        read_pico_sd_info_t info = {0};
        if (read_pico_sd_get_info(&info) == ESP_ERR_NOT_FINISHED) return APP_REDRAW_NONE;
        s_scan_pending = false;
        scan_shelf(ctx);
        if (ctx->request_app) return APP_REDRAW_NONE;
        (void)open_requested_book(ctx);
        return APP_REDRAW_PAGE;
    }
    if ((s_view == SHELF || s_view == MANAGE) && !(ctx->touch && ctx->touch->touched)) {
        (void)prepare_cover_batch(ctx, false);
        // 整页准备好再合并推屏；占位页或失败输出不能消费已加载封面的待刷标记。
        // Reveal only a complete cover group; placeholders and failed output cannot consume pending artwork.
        unsigned changed=shelf_cover_reveal_mask();
        if (ctx->request_app) return APP_REDRAW_NONE;
        EpdRect area = {0};
        for (int row = 0; row < shelf_rows(); ++row) {
            if (!(changed & (1u << row))) continue;
            EpdRect r = row_rect(row);
            if (!area.width) area = r;
            else {
                int right = area.x+area.width > r.x+r.width ? area.x+area.width : r.x+r.width;
                int bottom = area.y+area.height > r.y+r.height ? area.y+area.height : r.y+r.height;
                if (r.x < area.x) area.x = r.x;
                if (r.y < area.y) area.y = r.y;
                area.width = right-area.x; area.height = bottom-area.y;
            }
        }
        if (area.width) {
            render(ctx, ctx->fb);
            s_area = area;
            s_mode = s_view == SHELF && app_settings_main_fast_refresh() ? MODE_DU : MODE_GL16;
            s_shelf_feedback_pending = s_view == SHELF;
            s_cover_batch_present_pending = true;
            s_cover_present_mask=changed;
            return APP_REDRAW_AREA;
        }
    }
    warm_shelf_lock_cover(ctx);
    if (ctx->request_app) return APP_REDRAW_NONE;
    // 目录暂借系统字体，不应把正文按系统字体重新分页。/ The directory borrows the system face without repaginating the reader.
    if (s_view == READING && s_text && strcmp(s_font_path, ttf_font_path())) {
        size_t off = book_layout_page_start_offset(s_page);
        save_progress();
        lock_draw();
        invalidate_prep();
        bool ok = reader_relayout(body_rect());
        if (ok) s_page = reader_visual_start(reader_visual_for_offset(off));
        copy_text(s_font_path, sizeof(s_font_path), ttf_font_path());
        unlock_draw();
        if (!ok) { free_book(); s_view = SHELF; copy_text(s_message, sizeof(s_message), "字体重排失败，请重新打开图书"); }
        return APP_REDRAW_PAGE;
    }
    app_redraw_t clock=reader_clock_tick(ctx);
    if(clock==APP_REDRAW_NONE)reader_prefetch_idle(ctx);
    return clock;
}
static void before_lock(app_ctx_t* ctx) {
    boxsel_boundary_reset();
    shelf_category_remember(ctx);
    s_category_menu=false;
    s_reader_tap.pending = false;
    track_ticket_stats(ctx);
    save_progress();
    // 锁屏前丢弃分行预绘，后续只按当前字体重建。/ Discard idle prefetch before lock; rebuild only with the current face.
    lock_draw();
    invalidate_prep();
    unlock_draw();
    // 锁屏票根始终以当前正在阅读的书为准；不要依赖书架最近项是否已经同步。
    if (s_text && s_path[0]) book_progress_set_last_path(s_path);
    flush_ticket_stats();
    s_stats_last_ms = 0;
}
static EpdRect area_hint(app_ctx_t* ctx) { (void)ctx; return s_area; }
static bool main_page_visible(app_ctx_t* ctx) {
    (void)ctx;
    return s_view == SHELF && !s_clear_confirm && !s_batch_confirm;
}
// 状态栏电量差刷只认书架主视图。阅读页有自己的一分钟整条重绘，沉浸态根本没有
// 状态栏——两条节拍同时开就会互相踩，所以这里一律 opt out。
// Battery-area gate: shelf only. The reader already repaints the whole strip every
// minute and hides it in immersive mode; two cadences on one strip step on each other.
static int status_bar_bg(app_ctx_t* ctx) {
    (void)ctx;
    if (s_view != SHELF || s_clear_confirm || s_batch_confirm) return -1;
    return app_settings_main_fast_refresh() ? 0xF0 : 0xE0;
}

const app_desc_t app_book = {
    .title = "图书 Books", .detail = "TF 卡 txt / epub 阅读", .enter_full = false, .owns_keys = true,
    .defer_middle_short = true,
    .menu_handle_enabled = menu_handle_enabled,
    .main_page_visible = main_page_visible, .status_bar_bg = status_bar_bg,
    .render = render, .present = present, .on_enter = on_enter, .on_exit = book_on_exit,
    .on_media_lost = book_on_media_lost, .on_media_ready = book_on_media_ready,
    .on_before_lock = before_lock,
    .on_gesture = gesture_event, .on_key = on_key, .on_key_long = on_key_long,
    .on_dual_gesture = dual_gesture_event,
    .single_finger_priority = boxsel_single_finger_priority,
    .on_power_short = on_power_short, .on_quick_box = boxsel_enter_cb,
    .on_tick = on_tick, .area_hint = area_hint,
};
