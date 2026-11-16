/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：双指手势识别器（M4-A）。只接管 count==2，单指路径零接触。
 * 由主循环喂入触摸快照，产出「方向切换 / 字号步进 / 静默撤销」三种意图，
 * 不直接改变任何页面状态——动作出口在各 app 的 on_dual_gesture 里。
 *
 * English: Two-finger recognizer (M4-A). It owns count==2 only and never touches
 * the single-finger path. Fed by the main loop with touch snapshots, it emits
 * three intents - orientation toggle, font step, silent retract - and never
 * mutates page state itself; the action lives in each app's on_dual_gesture.
 *
 * 冻结（回正版 spec，过审后不再决策）：
 *  1. 双指横滑 = 切换横屏/竖屏（**不做翻页**，翻页已有单指路径）；
 *  2. 双指竖滑 = 字号 −1/+1 档对偶（上滑减、下滑加），单次手势最多 ±3 档；
 *  3. 状态机只有 DOWN2 / TRACK / FIRE / ABORT 四态，单指的长按/滑动/单击不进本层；
 *  4. 驱动零改动——CST836U 本就上报两点，只是旧应用层把 count>1 当取消丢掉。
 *
 * Frozen (the corrected spec, no re-deciding after review):
 *  1. Horizontal two-finger swipe = toggle landscape/portrait (NOT paging, which
 *     already has a single-finger path);
 *  2. Vertical two-finger swipe = font size -1/+1 step (up shrinks, down grows),
 *     at most +/-3 steps per gesture;
 *  3. The machine has exactly four non-idle states; single-finger press/swipe/tap
 *     stays out of this layer;
 *  4. Zero driver changes - the CST836U already reports two points; the old app
 *     layer merely discarded count>1 as a cancel.
 */
#pragma once
#include "app.h"
#include <stddef.h> // NULL：ui_dual_two_valid() 的空指针防御要用，头文件必须自包含
                    // NULL: ui_dual_two_valid() NULL-guards against it; keep this
                    // header self-contained.

/* ---- 常量：全部集中于此，禁止散落魔数 / All constants live here; no stray magic numbers ---- */

/// 双指融合窗：第二指在此窗内落下才算双指手势；窗内抬手视为误触。
/// Fusion window: the second contact must land inside it; lifting inside it is a slip.
#define UI_G_DUAL_FUSE_MS 100u

/// 双指滑动的**总位移**死区（欧氏距离）：没到这个距离不定轴、不发任何事件。
/// 48（单轴）→ 60（总位移）修掉了 45° 时门槛被 0.7 倍放宽的问题；60 → 40 是真机
/// 手感反馈：2.0 迟滞下短促干脆的滑动被判死区、「像卡了一样」。40px 仍是抖动
/// （<10px）的数十倍，不会误触。
/// Dead zone on **total travel** (euclidean): no axis decision and no event below
/// it. 48 (per-axis) -> 60 (total) fixed the 45-degree gate being 0.7x too wide;
/// 60 -> 40 is the hardware feel report: at 2.0 hysteresis crisp short swipes fell
/// into the dead zone and felt "stuck". 40px is still tens of times the jitter (<10px).
#define UI_G_DUAL_SWIPE_TRIGGER_PX 40

/// 速度门（M5-0 真机数据 · 2026-10-27）：抬手净位移 / 按下时长 < **0.42 px/ms**
/// 判为「按住/沉降」，不是滑动 → ABORT。双指按住做框选姿势时，手指微抖会产生
/// 187–425px 净位移，仅靠 40px 死区挡不住，过去会被判成横滑而误翻横屏。
/// 真机两群零重叠：按住 0.10–0.394、快滑 0.478–3.3，谷底 0.394→0.478，取 0.42。
/// 整数比 21/50 避免浮点；判据 `dist*50 < 21*dt` 数学等价于 `dist/dt < 0.42`。
/// Speed gate (M5-0 hardware data): net travel / press duration < **0.42 px/ms**
/// means a hold/settle, not a swipe -> ABORT. A two-finger framing pose drifts
/// 187-425px from jitter, which the 40px dead zone alone cannot stop; it used to be
/// judged as a horizontal swipe and flipped the screen. Hardware clusters do not
/// overlap: holds 0.10-0.394 vs swipes 0.478-3.3, valley 0.394->0.478, so 0.42.
/// Integer ratio 21/50 avoids floating point; `dist*50 < 21*dt` == `dist/dt < 0.42`.
#define UI_G_DUAL_MIN_SPEED_NUM 21
#define UI_G_DUAL_MIN_SPEED_DEN 50

/// 主轴迟滞（置信层）：主轴位移必须比副轴大 **1.6 倍**才认轴（偏角 32° 锥内）。
/// 用整数比（分子 8 / 分母 5）避免浮点。
/// Axis hysteresis (confidence tier): the dominant axis must be at least **1.6x**
/// the other (a 32-degree cone). Integer ratio 8/5 keeps it float-free.
///
/// 历史：1.5（3/2）真机偶发串扰；2.0 收紧后串扰消失但手感"卡"（12 次滑动被丢，
/// 8 次是 dy 600-1000 的大纵滑）。1.6 是数据定的折中：32° 锥、真 45° 仍不达。
/// History: 1.5 (3/2) cross-talked on hardware; 2.0 killed the cross-talk but felt
/// "stuck" (12 swipes dropped, 8 of them big 600-1000px verticals). 1.6 is the
/// data-driven middle: 32-degree cone, true 45 degrees still excluded.
#define UI_G_DUAL_AXIS_RATIO_NUM 8
#define UI_G_DUAL_AXIS_RATIO_DEN 5

/// 模糊带兜底下限（C-guard）：主/副比 **≥1.15** 时按较大轴动作，把 1.15–1.6 这段
/// 「略偏大轴」的真实意图捞回来；1.0–1.15（含 45°）视为真模糊，静默丢弃（宁可不做）。
/// 整数比 23/20 = 1.15。
/// Fallback floor (C-guard): when the dominant/secondary ratio is **>= 1.15** act on
/// the dominant axis, recovering the "slightly dominant" real intents in 1.15-1.6;
/// 1.0-1.15 (including 45 degrees) stays truly ambiguous and is silently dropped.
/// Integer ratio 23/20 = 1.15.
#define UI_G_DUAL_FALLBACK_RATIO_NUM 23
#define UI_G_DUAL_FALLBACK_RATIO_DEN 20

/// 滑动距离 → 字号档位映射：每 120px 一档。**与 app_settings 的字号取值表是两个概念**：
/// 本常量只做手势位移的量化，落到字号时再按 BOOK_PX_STEP 换算。
/// Distance-to-step mapping: one font step per 120px. **A different concept from
/// app_settings' font size values**: this only quantifies gesture travel; the
/// conversion to pixels goes through BOOK_PX_STEP.
#define UI_G_SWIPE_STEP_PX 120

/// 单次手势最多跨 ±3 个字号档（防一划到底）。
/// At most +/-3 font steps per gesture (no one-slide-to-the-end).
#define UI_G_DUAL_SWIPE_MAX_DZ 3

/// FIRE 后冷却，防一次手势连发；同时兼作刷新期丢弃的时间跳变阈值。
/// Post-FIRE cooldown against repeat firing; also the refresh-gap threshold.
#define UI_G_COOLDOWN_MS 150u

/// 框选 / 手势的**落定间距门槛**（M5-0 二轮重采定稿，2026-10-27 审计裁定③）：
/// `gap0 >= 300px` = 框选姿势（两指分开按句首句尾），`< 300px` = 靠拢的手势。
/// 数据形态：双峰清晰——小簇 119–289（n=17，手势）、大簇 312–1104（n=21，框选），
/// 空档 289→312。300 是唯一「零误伤 + 全命中」的取值：再高会漏掉 gap0=312 那笔
/// 慢而远的真手势（spd=0.468 / dt=919ms）。
/// 方向耦合（为什么统一阈值、不分档不归一化）：竖屏空档 224–486、横屏 203–556，
/// 300 同时落在两个空档里；机制上小簇受**手掌宽度**约束（与方向无关），大簇受
/// **句子物理长度**约束（横屏更长）——耦合只把阈值往安全侧推。
/// 26.9.5 起**常量先落地、暂不参与判定**：M5-A「反馈先于判定」接线时再挂上。
/// Framing-vs-gesture gap threshold (M5-0 second sampling; adjudication 3): gap0
/// >= 300px is a framing pose, < 300px is a gesture with fingers held close. Two
/// clean clusters - small 119-289 (n=17, gestures) and large 312-1104 (n=21,
/// framing) - with a 289->312 gap; 300 is the only zero-false-positive full-recall
/// point (higher misses the slow but far gesture at gap0=312, spd=0.468 / dt=919ms).
/// Orientation coupling (why one threshold, no tiers, no normalisation): the portrait
/// gap is 224-486 and the landscape one 203-556, so 300 sits inside both; the small
/// cluster is bounded by hand width (orientation-independent) and the large one by a
/// sentence's physical length (longer in landscape) - coupling only pushes it to the
/// safe side. The constant lands in 26.9.5 but does **not** feed a verdict yet.
#define UI_G_SEL_DIST_PX 300

/// **单点有效性**（26.9.6 B1）：这一条是本模块唯一允许谈论「触点有没有用」的地方。
/// 判据信**位置**、不信标志位：`active` 只是芯片的说法（round-3 已证它在抬手过渡帧
/// 仍为真），坐标才是物理事实——**两个坐标同时为 0 是物理上不可能的观测**（真实
/// 手指不可能精确压在左上角像素上并保持 count>=2），所以「active 但坐标全零」=
/// 这帧没有位置信息，不是有效触点。
/// Single-contact validity (26.9.6 B1): the only place allowed to say whether a contact
/// is usable. The predicate trusts POSITION, not flags: `active` is merely the chip's
/// claim (round 3 proved it stays true on the lift transition frame), while coordinates
/// are physical fact - both coordinates reading 0 is a physically impossible reading
/// (a real finger cannot sit on the top-left pixel and still report count>=2), so
/// "active but all-zero" means the frame carries no position and is not a contact.
static inline bool ui_dual_point_valid(const cst836u_point_t *p) {
    return p != NULL && p->active && (p->x != 0 || p->y != 0);
}

/// 26.9.5/26.9.6 归零修复 · **唯一真相源**：「此刻有两个有效触点」在本模块只有这一个
/// 判据。26.9.4 的 bug 正是它有两个版本——`two_point_gap()` 用严格版（还要看 active），
/// 状态机用宽松版（只看 count>=2）。同一件事两套判据 = 行为分叉：抬手帧驱动的
/// count 字段仍是 2、坐标报 0 但 active 已假，宽松版判定「两指还在」→ 中点退化到
/// (0,0) → 记为最后一次有效中点 → 抬手时净位移 = −落点坐标（"归零伪位移"），
/// 轴比退化成 |x0|/|y0|，**只由落点位置决定**，于是屏幕上"随机"翻屏/调字号。
/// 常量有两份只是算错数；判据有两份会让行为分叉——后者更危险。故做成函数：
/// 第二个定义在**结构上不可能存在**（任何调用方只能喊它）。
/// 为什么判「有没有有效触点」而不是「中点不是 (0,0)」：前者问原因（治因），后者是
/// 在给退化结果打补丁（治症），而 (0,0) 本身是合法的左上角坐标。
///
/// **26.9.6 收紧为「逐点」**（审计裁定①）：26.9.5 的公式是**成对 OR**——
/// `(p0.x|p0.y|p1.x|p1.y)!=0` 只要有一个点带坐标就整体判有效，于是 p0=(0,0)、p1 有效
/// 的帧照样被当成「两指有效」，中点被写成半个间距的假值。按原理实现：**两点各自
/// 都要过 `ui_dual_point_valid()`**，缺一个就不是双指态。
/// 26.9.5/26.9.6 zero-drift repair - the SINGLE source of truth for "two valid contacts
/// are down right now". The 26.9.4 bug was precisely having two of them: `two_point_gap()`
/// used the strict form (active included) while the machine used the loose one
/// (count>=2 alone). One fact, two predicates = divergent behaviour: on the lift tick
/// the driver still reports count==2 with zeroed coordinates and active==false, so the
/// loose form says "both fingers are down" -> the midpoint degrades to (0,0) -> that
/// garbage becomes the last valid midpoint -> at lift the net travel equals minus the
/// landing point (the "zero drift"), and the axis ratio degrades to |x0|/|y0|, decided
/// purely by WHERE the fingers landed - which is why the screen flipped "at random".
/// A duplicated constant merely computes the wrong number; a duplicated predicate
/// forks behaviour, and it is the more dangerous of the two. Hence a function: a
/// second definition is structurally impossible (every caller has to call this one).
/// Why "are the contacts valid" and not "the midpoint is not (0,0)": the former asks
/// about the cause, the latter patches up the degraded result - and (0,0) is itself a
/// legal top-left coordinate.
/// **26.9.6 tightened to per-point** (adjudication 1): the 26.9.5 form was a PAIRWISE
/// OR - `(p0.x|p0.y|p1.x|p1.y)!=0` passes as soon as ANY point carries a coordinate, so
/// a frame with p0=(0,0) and a valid p1 still counted as "two contacts" and stored a
/// midpoint half a gap away from the truth. Per the principle: **each point must pass
/// `ui_dual_point_valid()` on its own** - one bad point and it is not a pair.
static inline bool ui_dual_two_valid(const cst836u_touch_t *t) {
    return t != NULL && t->count >= 2 &&
           ui_dual_point_valid(&t->points[0]) && ui_dual_point_valid(&t->points[1]);
}

/// **现象位判据**（26.9.6，与上面的有效性判据刻意分开）：这一帧有没有出现「芯片
/// 还在声称 active、却报全零坐标」的病灶点。它**只回答观测到了什么**，不参与任何
/// 判定——标记「看到了病灶帧」，不是「判据说了什么」。
/// 两者必须分开：若用判据的返回值去打标，round-3 的 `lift0` 恒零就是这个下场——
/// 现象帧与判定帧常常不在同一拍，帧级快照必然恒零，必须**锁存到手势级**。
/// Symptom-side predicate (26.9.6, deliberately separate from the validity one): did
/// this frame carry a sick point - the chip still claiming `active` while reporting
/// all-zero coordinates? It answers WHAT WAS OBSERVED and never feeds a verdict: the
/// mark means "a sick frame was seen", not "the predicate said no".
/// Keeping them apart matters: marking from the predicate's return value is exactly how
/// round 3's `lift0` ended up permanently zero - the symptom frame and the verdict tick
/// are often not the same tick, so a frame-level snapshot is always zero and the flag
/// must be LATCHED at gesture level.
static inline bool ui_dual_saw_zero_frame(const cst836u_touch_t *t) {
    if (t == NULL || t->count < 2) return false;
    for (int i = 0; i < 2; i++) {
        if (t->points[i].active && t->points[i].x == 0 && t->points[i].y == 0) return true;
    }
    return false;
}

/* ---- 状态 / States ---- */

typedef enum {
    UI_DUAL_IDLE,  ///< 静止 / At rest
    UI_DUAL_DOWN2, ///< 双指已落下，融合窗内 / Two contacts down, inside the fusion window
    UI_DUAL_TRACK, ///< 融合窗已过，跟踪位移 / Past the window, tracking travel
    UI_DUAL_FIRE,  ///< 提交动作（终止态，只存在一拍）/ Committed (terminal, one tick)
    UI_DUAL_ABORT, ///< 静默取消（终止态，只存在一拍）/ Silently dropped (terminal, one tick)
} ui_dual_state_t;

/* ---- 事件 / Events ---- */

typedef enum {
    UI_DUAL_EVT_NONE = 0,
    /// 静默撤销：双指成立瞬间发出，让已打开的词卡/浮层无声关掉，不触发查词、不落生词本。
    /// 单指**不挂起**——它照常即时派发，撤销只是把已经发生的事收回去。
    /// Silent retract: emitted the moment two fingers are established so an open
    /// card/sheet closes without a lookup or a vocab entry. The single finger is
    /// never suspended - it dispatches as usual; the retract only undoes it.
    UI_DUAL_EVT_RETRACT,
    /// 双指横滑：切换横屏/竖屏。/ Horizontal two-finger swipe: toggle orientation.
    UI_DUAL_EVT_ORIENT,
    /// 双指竖滑：字号步进，steps 为 ±1..±3。/ Vertical swipe: font step, +/-1..+/-3.
    UI_DUAL_EVT_FONT_STEP,
} ui_dual_evt_t;

/// **收尾方式**（26.9.7）：判定这一拍「两指为什么不再有效」——三个互斥答案。
/// 它存在的唯一理由：`lift0` 只能看见**撒谎型**病灶帧（芯片仍报 active、坐标全零），
/// 看不见**诚实型**（active 已置假、坐标全零）——而后者恰恰是修复成立时的正常形态。
/// `end` 只读 `count` 与坐标，**完全不查 active**，所以两种病灶都逃不掉：
///  - `UI_DUAL_END_LIFT`：count==0，两指真的都不在了（正常抬手）；
///  - `UI_DUAL_END_ONE`：count==1，先抬起了一指（不对称抬手，正常）；
///  - `UI_DUAL_END_ZERO`：**count 仍 >=2 却没有两个可用触点**——这就是那条缝的收尾形态，
///    active 真不真都一样。复验里 `end=zero` 出现 = 现象还在（且被冻结住、零误发射
///    = 被正确忽略）；`lift0=1` 只是它的一个子集（撒谎型）。
/// How the gesture ended (26.9.7): why the two contacts stopped being usable - three
/// mutually exclusive answers. Its only reason to exist: `lift0` can see the LYING kind
/// of sick frame (active still true, coordinates zero) and is blind to the HONEST kind
/// (active already false, coordinates zero) - which is precisely what a working repair
/// looks like. `end` reads `count` and coordinates only, never `active`, so both kinds
/// are caught: LIFT = count==0 (both fingers really gone), ONE = count==1 (one finger
/// lifted first), ZERO = count>=2 with no usable pair - the seam's terminal shape,
/// active or not. In re-verification `end=zero` present means the symptom is still there
/// (and zero mis-fires means it is correctly ignored), while `lift0=1` is just its subset.
typedef enum {
    UI_DUAL_END_NONE = 0, ///< 没有判定拍（或非 TRACK 收尾）/ No verdict tick
    UI_DUAL_END_LIFT,     ///< count==0：两指真的都抬起 / Both fingers really gone
    UI_DUAL_END_ONE,      ///< count==1：先抬起一指 / One finger lifted first
    UI_DUAL_END_ZERO,     ///< count>=2 却无可用位置：病灶收尾 / count>=2, no usable position
} ui_dual_end_t;

/// **中止原因**（26.9.7，审计裁定⑤「静默是日志的敌人」）：ABORT 这条路径过去一句话
/// 都不说，日志里只看到「什么都没发生」。round-4 那 1 笔「66 落定 / 65 判定」的缺口
/// 就是一次静默中止，没有日志就只能靠猜——而「采不到」不能伪装成「没发生」。
/// 这里给每个 ABORT 出口一个名字，日志里 `reason=` 直接读出是哪一道门关掉的。
/// **Abort reason** (26.9.7, adjudication 5: silence is the log's enemy). The ABORT
/// path used to say nothing at all, so the log only ever showed "nothing happened".
/// Round 4's 1-in-66 gap ("66 landed, 65 verdicts") was one such silent abort; with no
/// line to read, the only honest step was a guess - and "cannot observe" must never be
/// dressed up as "did not happen". Every ABORT exit now names itself.
typedef enum {
    UI_DUAL_ABORT_NONE = 0, ///< 没有中止 / No abort
    UI_DUAL_ABORT_SLIP,     ///< 融合窗内抬手（双指快点一下的形态）/ Lifted inside the fusion window
    UI_DUAL_ABORT_GAP,      ///< 刷新期丢弃（present 阻塞导致时间跳变）/ Refresh-gap discard
    UI_DUAL_ABORT_DEADZONE, ///< 净位移不足触发距离 / Net travel under the trigger distance
    UI_DUAL_ABORT_SLOW,     ///< 速度门：走得太慢 = 按住 / Speed gate: too slow, a hold
    UI_DUAL_ABORT_SUBSTEP,  ///< 定了轴却不足一档 / Axis decided but under one step
    UI_DUAL_ABORT_AMBIG,    ///< 轴比 1.0–1.15 静默 / Ratio too close to call, stays silent
} ui_dual_abort_t;

typedef struct ui_dual_event_s {
    ui_dual_evt_t type; ///< 事件类型 / Event type
    int steps;          ///< UI_DUAL_EVT_FONT_STEP 的档位数（带符号，其余为 0）
                        ///< Steps for FONT_STEP (signed; 0 otherwise)
    /// 诊断：抬手判定用的最终净位移与判定结论。仅 type==NONE 且 judged 时表示
    /// 「判了但丢弃」（斜滑/死区），调用方据此打日志——本轮修的「偶尔串扰」正是
    /// 因为这里过去完全无打点，只能靠猜。
    /// Diagnostics: the final net travel and the verdict. type==NONE with judged
    /// set means "judged and dropped" (diagonal or dead zone) - the caller logs it.
    /// The cross-talk bug persisted precisely because nothing here was observable.
    int dx, dy;         ///< 抬手时的最终净位移 / final net travel at lift
    bool judged;        ///< 这一拍做了一次抬手判定 / a lift verdict happened this tick
    /// 26.9.6 **现象位**打标：这次手势期间**见到过病灶帧**（芯片仍报 active、坐标却
    /// 全零）。语义是「这条命是从那条缝里钻进来的」，与判定结论无关——它不表示
    /// 「判据判了什么」，只表示「现象确实发生了」。
    /// **判读口径（26.9.7 修正，审计裁定）：它不是「修复成立」的必需项。**
    /// 判据信位置、不信 active，所以只要芯片的归零帧是「诚实型」（active 已置假），
    /// 病灶帧就不会被这条打标认出来——但那正是**修复完全成立**的形态。把「lift0 全无」
    /// 读成可疑，会把成功判成失败，还会诱导人去找一个不该存在的靶帧。正确的读法是
    /// 四行矩阵（见 README）：启动串 → 零误发射 → lift0 有/无，其中「零误发射 + lift0
    /// 全无」= 修复成立（未遇靶帧，或芯片诚实型）。要证明「保护机制真的在跑」，靠
    /// `freeze_n`（不依赖 active 语义），不靠 lift0。
    /// 26.9.5 把这标打在判定位，round-3 直接恒零——现象帧与判定帧不同拍，帧级快照
    /// 采不到。26.9.6 起改为**手势级锁存**：TRACK 期间见过一次就置位。
    /// 26.9.6 symptom-side mark: a sick frame (chip still reporting `active` with all-zero
    /// coordinates) was SEEN during this gesture. It says "this verdict came in through
    /// that seam" and says nothing about the verdict itself - not "the predicate said
    /// no", only "the symptom really happened".
    /// **How to read it (corrected in 26.9.7 by adjudication): it is NOT a prerequisite for
    /// "the repair holds".** The predicate trusts position, not `active`, so if the chip's
    /// zeroing frames are the HONEST kind (active already false) this mark can never fire -
    /// and that is the shape of a fully working repair. Reading "lift0 absent" as suspicious
    /// would fail a success and send somebody hunting for a target frame that must not
    /// exist. The correct reading is the four-row matrix in the README: boot string ->
    /// zero mis-fires -> lift0 present/absent, where "zero mis-fires + lift0 absent" means
    /// the repair holds (no target frame met, or an honest chip). To prove the protection
    /// actually runs, use `freeze_n` (which never consults `active`), not this flag.
    /// 26.9.5 marked this at the verdict site and round 3 read it as permanently zero -
    /// the symptom frame and the verdict tick are different ticks, so a frame-level
    /// snapshot can never catch it. Since 26.9.6 it is LATCHED at gesture level: one
    /// sighting during tracking is enough.
    bool lift0;
    /// 26.9.7 **佐证计数**（审计裁定：补 lift0 的盲区）：这次双指手势期间，因「有效点
    /// 不足 2」而**冻结** `x_last` 的帧数。它不查 `active`——只要坐标全零就冻结——所以
    /// 芯片发的是诚实型还是撒谎型归零帧，它都照样计数：>0 就证明「冻结保护」这条路
    /// 真的被走过，而不是「一次都没触发」。
    /// 注意语义边界：真实抬手那一帧也计一次（那里同样没有位置可写，冻结是正确行为），
    /// 所以任何一次手势的 freeze_n 至少是 1；它证明的是**机制在跑**，不是「遇到了病灶」。
    /// 26.9.7 corroborating counter (adjudication: covers lift0's blind spot): how many
    /// frames of this gesture FROZE `x_last` because fewer than two points were valid. It
    /// never consults `active` - all-zero coordinates freeze the record either way - so it
    /// counts the same whether the chip lies or is honest: >0 proves the freeze path was
    /// actually walked rather than never triggered.
    /// Boundary: the real lift tick counts too (nothing valid to write there, freezing is
    /// correct), so any gesture scores at least 1. It proves "the mechanism runs", not
    /// "a sick frame was met" - that is what lift0 is for.
    /// **落地后的结构复核（26.9.7）**：状态机里 `!two` 的那一拍**立刻**结束手势（DOWN2 →
    /// ABORT / TRACK → 判定），所以每次手势只有这一拍会冻结——判定行上的 freeze_n
    /// **恒为 1**，它只是「冻结路径走过一次」的存在性证明，区分不了「诚实型病灶」与
    /// 「真抬手」。真正补 lift0 盲区的是同一包里的 `end`（只读 count 与坐标）。
    /// **Structural review after landing (26.9.7)**: the `!two` tick ENDS the gesture at
    /// once (DOWN2 -> ABORT / TRACK -> verdict), so exactly one frame can ever freeze -
    /// freeze_n reads 1 on every verdict line. It is an existence proof ("the freeze path
    /// ran") and cannot tell an honest sick frame from a real lift; `end` is what actually
    /// covers lift0's blind spot.
    int freeze_n;
    /// 26.9.7 收尾方式（见 ui_dual_end_t）：`end=zero` 是病灶收尾的**active 无关**证据。
    /// 26.9.7 how the verdict tick ended (see ui_dual_end_t): `end=zero` is the
    /// active-independent evidence of a sick terminal frame.
    ui_dual_end_t end;
    /// 26.9.7 中止原因（见 ui_dual_abort_t）：非 NONE 表示这一拍静默收尾，值为名字。
    /// 26.9.7 why it gave up (see ui_dual_abort_t): non-NONE means this tick ended
    /// silently, and the value is the name of the gate that closed.
    ui_dual_abort_t abort;
} ui_dual_event_t;

/* ---- 识别器状态 / Recognizer state ---- */

typedef struct {
    ui_dual_state_t state; ///< 当前状态 / Current state
    int64_t t0_ms;         ///< 双指落下的时刻 / When the two contacts landed
    int64_t last_ms;       ///< 上一次喂入的时刻，用于刷新期丢弃 / Last feed time, for refresh gaps
    int64_t fire_ms;       ///< 上次 FIRE 的时刻，用于冷却 / Last FIRE time, for the cooldown
    int x0, y0;            ///< 落下时两指的中点 / Midpoint of the two contacts on landing
    /// 落下时两指的**欧氏间距**（px）。M5-0 打点：框选与双指手势的分界要靠它说话——
    /// 手势两指靠拢（间距小），框选两指分开按句首句尾（间距大）。**只记录、不参与
    /// 判定**：本轮只收分布，SEL_DIST 定值由真机数据说话，绝不预先写死。
    /// Euclidean gap between the two contacts at landing (px). M5-0 telemetry: the
    /// framing-vs-gesture split must be data-driven - gesture fingers sit close, a
    /// framing pair spreads wide. **Recorded only, never part of the verdict**: this
    /// round collects the distribution; SEL_DIST is set from hardware data, not guessed.
    int gap0;
    /// 最后一个**有效**两指中点。抬手那一拍驱动已清空 points（cst836u_touch_t 只保留
    /// x/y 单点），若在抬手时现算中点会得到 (0,0)、净位移全错——所以滑动期间必须
    /// 自己记住最后位置，抬手指令用这份记录判定。
    /// The last **valid** two-finger midpoint. On the lift tick the driver has already
    /// cleared points (cst836u_touch_t keeps only the single x/y), so computing the
    /// midpoint there would yield (0,0) and a nonsense net travel - hence the track
    /// keeps the last position and the lift verdict reads it.
    int x_last, y_last;
    /// 26.9.6：本次手势是否见过病灶帧（现象位锁存，见 ui_dual_event_t.lift0）。
    /// 落定那一拍清零，抬手判定时读出后随手势结束自然失效。
    /// 26.9.6: whether this gesture ever saw a sick frame (the symptom latch described
    /// at ui_dual_event_t.lift0). Cleared on landing, read at the lift verdict.
    bool saw_zero;
    /// 26.9.7：本次手势因「有效点 <2」冻结 `x_last` 的帧数（见 ui_dual_event_t.freeze_n）。
    /// 落定那一拍清零；判定时读出。**不查 active**，所以它同时覆盖「诚实型」归零帧——
    /// 那正是 lift0 的盲区，也是这条计数存在的全部理由。
    /// 26.9.7: frames of this gesture that froze `x_last` because fewer than two points
    /// were valid (see ui_dual_event_t.freeze_n). Cleared on landing, read at the verdict.
    /// It never consults `active`, so it also covers the HONEST kind of zeroing frame -
    /// precisely lift0's blind spot, which is the only reason this counter exists.
    int freeze_n;
    int peak_steps;        ///< 判定出的档位数（带符号）/ Steps decided at the lift verdict
    int axis;              ///< 0=未定轴 1=横向 2=纵向 / 0=undecided 1=horizontal 2=vertical
    bool suppress;         ///< 吞掉后续单指，直到两指全部抬起（硬语义 2）
                           ///< Swallow later single-finger input until both lift (hard rule 2)
    /// **26.9.12 · 重入守卫**：两指还没全部抬离（count 从未回到 0）之前，不承认新手势。
    /// 它与 `suppress` 是**两件事**，必须分开：`suppress` 管「单指要不要派发」，本字段管
    /// 「能不能重开一次手势」。混成一个字段时，框选态为了不饿死单指而关掉 `suppress`，
    /// 会顺带打开重入——病灶帧收尾后坐标一旦恢复有效，同一段按住就会被重新识别、
    /// 再发一次 RETRACT，正是注释里警告过的「一次长按被刷新反复识别」。
    /// **26.9.12 - re-entry guard**: a new gesture is not accepted until both contacts have
    /// really left (count reached 0). It is a DIFFERENT fact from `suppress` and must stay
    /// separate: `suppress` decides whether the single finger dispatches, this one decides
    /// whether a new gesture may start. Folded into one field, the box mode has to clear
    /// `suppress` (to stop starving the single finger) and thereby opens re-entry - once a
    /// sick frame's coordinates come back valid the same hold is re-recognized and fires a
    /// second RETRACT, exactly the "one hold re-read after every refresh" the comment warns of.
    bool need_lift;
    /// **26.9.12/26.9.13 · 本次会话是否「单指优先」**。默认 false（硬语义 2 的原意：双指
    /// 判定后，剩下一指不得被解释成点击/翻页）。框选态置 true：那里两次精确点按是唯一
    /// 输入，"抑制把单指饿死"的代价远大于"双指瞬间误选一个字"——双指一旦成立就立刻退出
    /// 框选，误选可再点一次纠正，而饿死要等 30 s 超时才恢复。
    /// 置 true 时：双指**照常**判定方向/字号/落定撤销，但 `ui_dual_engaged()` **恒为 false**，
    /// 主循环因此整个双指态都不再挡住单指通道。`suppress` 本身照常置位，重入仍由
    /// `need_lift` 挡住——三件事各有各的字段，不许再合并。
    /// **26.9.13 极性翻转的来由**：这个字段 26.9.12 叫 `allow_suppress`（true = 允许抑制），
    /// 而 `ui_dual_engaged()` 写成 `if (state != IDLE) return true;`，把合取短路掉了——
    /// 置 false 根本不起作用。审计裁定把它升成策略位时改成正极性：**一个叫 "priority"
    /// 的布尔值，取反才是语义，读代码的人迟早会漏掉那个 `!`**。同一位同时约束
    /// 「不饿死单指」（本文件）与「count>1 不取消 TAP」（ui_gesture.c），一个意图一个名字。
    /// **26.9.12/26.9.13 - whether this session gives the single finger priority**. False by
    /// default (hard rule 2: once two fingers are judged, the leftover finger must not read
    /// as a tap or a page turn). The box-selection mode sets it true: two precise taps are
    /// the only input there, so starving the single finger costs far more than the
    /// occasional stray glyph picked up while two fingers land - a landed pair exits the
    /// mode at once, so a wrong pick is one tap away from fixed, whereas starvation waits
    /// out the 30 s timeout. With true the dual layer still judges orientation, font steps
    /// and the landing retract, but `ui_dual_engaged()` is **always false**, so the loop
    /// stops blocking the single path for the whole dual session. `suppress` is still set
    /// as usual and re-entry stays blocked by `need_lift` - three facts, three fields,
    /// never merged again.
    /// **26.9.13, why the polarity flipped**: in 26.9.12 this field was `allow_suppress`
    /// (true = may swallow) and `ui_dual_engaged()` short-circuited the conjunction with
    /// `if (state != IDLE) return true;`, so setting it false did nothing at all. Raised to
    /// a policy bit by adjudication, it now reads positively: **a boolean named "priority"
    /// whose meaning is the negation is a trap someone will eventually miss**. The same bit
    /// constrains "do not starve the single finger" (here) and "count>1 must not cancel a
    /// TAP" (ui_gesture.c) - one intent, one name.
    bool single_finger_priority;
} ui_dual_t;

/// 清除状态；进出页面时调用。/ Clear state when entering or leaving a page.
void ui_dual_reset(ui_dual_t *dual);

/// 喂入一拍触摸快照；返回 true 表示 out 里有一个事件。
/// 每次最多一个事件；无触摸快照时返回 false 且不改状态。
/// Feed one tick; true means out carries an event. At most one event per call;
/// a missing snapshot returns false and leaves the state alone.
bool ui_dual_feed(ui_dual_t *dual, const app_ctx_t *ctx, ui_dual_event_t *out);

/// 本层是否正在占用触摸。为真时主循环**不得**把这一拍喂给单指识别器——
/// 这既保证「单指零接触」，也实现「抬起一指不恢复成单指」。
/// Whether this layer owns the touch. When true the loop must NOT feed this tick
/// to the single-finger recognizer - that keeps the single path untouched and
/// stops a lifted finger from reverting to single-finger semantics.
bool ui_dual_engaged(const ui_dual_t *dual);
