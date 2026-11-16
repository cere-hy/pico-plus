/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：双指状态机的实现（M4-A）。四态：DOWN2 → TRACK → FIRE / ABORT。
 * English: Two-finger state machine (M4-A). Four states: DOWN2 -> TRACK -> FIRE / ABORT.
 */
#include "ui_dualgesture.h"
#include <stddef.h> // NULL：不依赖 app.h 的传递包含 / NULL without relying on app.h

/// 钳位到 ±UI_G_DUAL_SWIPE_MAX_DZ。/ Clamp to +/-UI_G_DUAL_SWIPE_MAX_DZ.
static int clamp_steps(int n) {
    if (n > UI_G_DUAL_SWIPE_MAX_DZ) return UI_G_DUAL_SWIPE_MAX_DZ;
    if (n < -UI_G_DUAL_SWIPE_MAX_DZ) return -UI_G_DUAL_SWIPE_MAX_DZ;
    return n;
}

static int abs_i(int v) { return v < 0 ? -v : v; }

/// 整数平方根（无浮点、无 libm 依赖）：逐位法，O(log v)。两指间距只需整像素，
/// 完全不值得为一个日志字段把浮点拖进这个纯整数模块。
/// Integer square root (float- and libm-free) by the bitwise method, O(log v).
/// The contact gap only needs whole pixels - not worth dragging floating point into
/// this otherwise pure-integer module for one telemetry field.
static int isqrt_i(int v) {
    if (v <= 0) return 0;
    int r = 0;
    int bit = 1 << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

/// 两指落下时的欧氏间距；不足两点退化为 0。M5-0 打点专用，不影响判定。
/// Euclidean gap at landing; degrades to 0 with fewer than two contacts. M5-0
/// telemetry only - never feeds the verdict.
///
/// 26.9.5：判据改为喊 ui_dual_two_valid()。此前这里自己抄一份「严格版」（count>=2
/// 且两个 active），状态机却用「宽松版」（只看 count>=2）——同一件事两套判据，
/// 同一份 fill 文件里两个真相源。这类双定义的代价已经在 round2 结账：48% 的判定走了
/// 归零帧。现在两处都喊同一个函数，改判据只有一处可改。
/// 26.9.5: the predicate now calls ui_dual_two_valid(). This spot used to carry its
/// own copy of the strict form (count>=2 AND both active) while the machine used the
/// loose one (count>=2 only) - one fact, two predicates, two sources of truth in one
/// file. Round 2 priced that mistake at 48% of all verdicts going through a zeroed
/// frame. Both call sites now share one function, so there is one place to change.
static int two_point_gap(const cst836u_touch_t *t) {
    if (ui_dual_two_valid(t)) {
        const int dx = (int)t->points[0].x - (int)t->points[1].x;
        const int dy = (int)t->points[0].y - (int)t->points[1].y;
        return isqrt_i(dx * dx + dy * dy);
    }
    return 0;
}

// 定轴阈值必须小于一档距离：否则会出现"已经定了纵向轴、却连一档都凑不满"的空窗，
// 用户滑了却什么也没发生。/ The axis threshold must be shorter than one step, or a
// swipe can lock the vertical axis without ever reaching a step - the user swipes
// and nothing happens.
_Static_assert(UI_G_DUAL_SWIPE_TRIGGER_PX < UI_G_SWIPE_STEP_PX,
               "axis lock must trigger before a full font step is possible");
// 迟滞必须大于 1：等于 1 就是"谁大谁赢"，45° 斜滑会随机定轴——本次真机串扰的
// 直接成因。/ Hysteresis must exceed 1: at exactly 1 it is "largest wins" and a
// 45-degree swipe picks an axis at random - the direct cause of the cross-talk.
_Static_assert(UI_G_DUAL_AXIS_RATIO_NUM > UI_G_DUAL_AXIS_RATIO_DEN,
               "axis hysteresis must be strictly greater than 1.0");
// 兜底下限必须大于 1（否则 45° 也会开火，等于退回 C-full），且必须严格小于置信
// 迟滞（否则置信层永远走不到、1.6 形同虚设）。
// The fallback floor must exceed 1 (else 45 degrees fires, i.e. C-full), and must sit
// strictly below the confidence hysteresis (else the confidence tier is unreachable).
_Static_assert(UI_G_DUAL_FALLBACK_RATIO_NUM > UI_G_DUAL_FALLBACK_RATIO_DEN,
               "fallback floor must be strictly greater than 1.0 (no C-full)");
_Static_assert(UI_G_DUAL_AXIS_RATIO_NUM * UI_G_DUAL_FALLBACK_RATIO_DEN >
                   UI_G_DUAL_FALLBACK_RATIO_NUM * UI_G_DUAL_AXIS_RATIO_DEN,
               "fallback floor must sit below the confidence hysteresis");

/// 两指中点的整数坐标；**返回是否真的算出了中点**（26.9.5 起带返回值）。
/// Integer midpoint of the two contacts; now also reports whether it really computed
/// one (since 26.9.5).
///
/// 旧行为是「点数不足就退化成首点」——这条退化路径正是归零事故的传送带：抬手帧
/// count 仍是 2 但两个触点都已 inactive，它把中点算成 (0,0)，调用方照单全收写进
/// x_last。现在没有有效两点就**返回 false、一个字节都不写**，退化行为从「静默产出
/// 一个看起来合法的值」变成「显式报错」—— call site 必须决定怎么办，而不是假装
/// (0,0) 是一次正常观测。（(0,0) 是合法的左上角坐标，靠它做哨兵值本来就站不住。）
/// The old form degraded to the first contact when fewer than two were valid - and
/// that fallback was the conveyor belt of the zero-drift accident: on the lift tick
/// count is still 2 while both contacts are inactive, so it computed (0,0) and the
/// caller stored it as the last valid midpoint. Now a missing pair returns false and
/// writes nothing: the degenerate case becomes an explicit "cannot answer" instead of
/// silently producing a plausible value, forcing the caller to decide. ((0,0) is a
/// legal top-left coordinate, so using it as a sentinel was never sound.)
static bool midpoint(const cst836u_touch_t *t, int *mx, int *my) {
    if (!ui_dual_two_valid(t)) return false;
    *mx = ((int)t->points[0].x + (int)t->points[1].x) / 2;
    *my = ((int)t->points[0].y + (int)t->points[1].y) / 2;
    return true;
}

void ui_dual_reset(ui_dual_t *dual) {
    if (dual == NULL) return;
    *dual = (ui_dual_t){0};
    // 单指优先默认关闭：这是硬语义 2 的原意，只有框选态会临时打开（见 ui_dual_t）。
    // Single-finger priority is off by default - the original hard rule 2; only the
    // box-selection mode turns it on for the duration (see ui_dual_t).
    dual->single_finger_priority = false;
}

bool ui_dual_engaged(const ui_dual_t *dual) {
    if (dual == NULL) return false;
    // 26.9.13（P1 路径 A 的修法，审计裁定）：**先把策略位站在最前面**。
    // 26.9.12 写的是 `if (state != IDLE) return true;` 再 `return suppress && allow_suppress`，
    // 于是 `allow_suppress` 只在 IDLE 分支里生效——而框选态恰恰是在 DOWN2/TRACK 期间被
    // 饿死的（真机：两段 525/537 ms 输入真空，位移恒为 0）。头文件当时已经写明「置 false
    // 时不再驱动 ui_dual_engaged()」，代码没做到：契约与实然相反，是最难查的那一类。
    // 现在只要策略位为真就恒返回 false：整个双指态都不再挡住单指通道。
    // 26.9.13 (the fix for P1 path A, by adjudication): **the policy bit comes first**.
    // 26.9.12 read `if (state != IDLE) return true;` and only then
    // `return suppress && allow_suppress`, so `allow_suppress` only ever applied in the
    // IDLE branch - while the box mode was starved precisely during DOWN2/TRACK (field:
    // two input blackouts of 525/537 ms with zero travel). The header already promised
    // "false stops driving ui_dual_engaged()"; the code did not. A contract that says the
    // opposite of the code is the hardest kind to find. Now the policy bit short-circuits
    // to false: the whole dual session stops blocking the single path.
    if (dual->single_finger_priority) return false;
    if (dual->state != UI_DUAL_IDLE) return true;
    // 抑制只对「单指要不要派发」生效。`suppress` 本身仍照常置位——它不再是重入判据，
    // 重入由 need_lift 管（见 ui_dual_t 的说明）。
    // The swallow only governs single-finger dispatch. `suppress` is still set as usual -
    // it no longer gates re-entry, which need_lift owns (see ui_dual_t).
    return dual->suppress;
}

/// 收尾方式的**唯一**判定处（见 ui_dual_end_t）。只读 count 与坐标，**不查 active**。
/// 单一真相源：判定拍、融合窗中止、刷新期中止三处都喊它，免得同一件事写三份。
/// The one and only place that decides how a gesture ended (see ui_dual_end_t). It
/// reads `count` and coordinates and never `active`, and all three exits (verdict,
/// fusion-window slip, refresh gap) call it - one fact, one expression.
static ui_dual_end_t end_kind(const cst836u_touch_t *t) {
    if (t->count == 0) return UI_DUAL_END_LIFT;
    return t->count >= 2 ? UI_DUAL_END_ZERO : UI_DUAL_END_ONE;
}

bool ui_dual_feed(ui_dual_t *dual, const app_ctx_t *ctx, ui_dual_event_t *out) {
    if (dual == NULL || ctx == NULL || ctx->touch == NULL || out == NULL) return false;
    out->type = UI_DUAL_EVT_NONE;
    out->steps = 0;
    out->end = UI_DUAL_END_NONE;
    out->abort = UI_DUAL_ABORT_NONE;

    const cst836u_touch_t *t = ctx->touch;
    const int64_t now = ctx->now_ms;
    // 「此刻有两个有效触点」——本拍只判一次，下面所有分支共用这一个值（判据只有一份，
    // 见 ui_dual_two_valid 的说明）。/ "Two valid contacts right now" - judged once per
    // tick and shared by every branch below (the predicate has exactly one definition;
    // see ui_dual_two_valid).
    const bool two = ui_dual_two_valid(t);

    // FIRE / ABORT 是终止态，只存在一拍——下一次喂入先收敛回 IDLE。
    // FIRE and ABORT are terminal and last one tick - the next feed settles to IDLE.
    if (dual->state == UI_DUAL_FIRE || dual->state == UI_DUAL_ABORT) dual->state = UI_DUAL_IDLE;

    // 硬语义 3 · 刷新期丢弃：app_present() 阻塞期间采样点丢失，恢复后时间跳变超过
    // 冷却窗 → 直接 ABORT 回 IDLE，绝不把跨刷新的两段采样当成一次手势。
    // Hard rule 3 - refresh gaps: sampling stops while app_present() blocks; if the
    // resumed timestamp jumps past the cooldown window, abort rather than stitching
    // two samples across a refresh into one gesture.
    if (dual->state != UI_DUAL_IDLE && dual->last_ms != 0 &&
        now - dual->last_ms > (int64_t)UI_G_COOLDOWN_MS) {
        const bool swallowed = dual->suppress;
        dual->state = UI_DUAL_ABORT;
        dual->last_ms = now;
        dual->axis = 0;
        dual->peak_steps = 0;
        // 26.9.7：这条出口以前一句话都不说。/ 26.9.7: this exit used to say nothing.
        out->abort = UI_DUAL_ABORT_GAP;
        out->end = end_kind(t);
        // 抑制属于「这一次触摸」，不因刷新期丢弃而解除——否则抬手后单指会复活。
        // The swallow belongs to this contact, so a refresh abort keeps it; lifting
        // it would let a single finger resurrect after the refresh.
        dual->suppress = swallowed;
        return false;
    }

    // 解除单指抑制（硬语义 2 到此结束）。26.9.12 起有**两个**出口，少一个就会把抑制
    // 无限拖长：
    //  a) `count == 0`：两指真的都抬起了；
    //  b) `count >= 2` 却没有一对有效触点：**病灶收尾**——芯片仍报两点、坐标已归零。
    // 真机日志里抬手帧的形状正是 `end=zero`（见 ui_dual_end_t），count 停在 2 不掉，
    // 只认 a) 就永远解除不了：单指识别器整段收不到拍子，框选表现成「点了没反应」，
    // 要等 30 s 超时才退出。`count == 1`（不对称抬手）**仍然保持抑制**，那是硬语义 2
    // 的本意：剩下一指不得被解释成一次点击。
    // Release the single-finger swallow (the end of hard rule 2). Since 26.9.12 there are
    // TWO exits, and dropping either one drags the swallow out indefinitely:
    //  a) `count == 0` - both contacts really lifted;
    //  b) `count >= 2` with no valid pair - the SICK terminal frame (the chip still reports
    //     two contacts while the coordinates are already zero). That is exactly the lift
    //     frame the hardware logs (`end=zero`, see ui_dual_end_t): count stays at 2, so a)
    //     alone never fires, the single-finger recognizer is never fed, and the selection
    //     reads as "taps do nothing" until the 30 s timeout. `count == 1` (asymmetric lift)
    //     still HOLDS the swallow - that is the point of hard rule 2: the leftover finger
    //     must not read as a tap.
    // 重入守卫只管一件事：**两指全部抬离之前，不承认新手势**。它与抑制解除严格独立——
    // 抑制可以在病灶帧就解除（让单指立刻复活），但手势不能因此重开，否则同一段按住会被
    // 重新识别、再发一次 RETRACT。清零必须写在下面那个提前 return **之前**：`count==0`
    // 且已回 IDLE 时会直接返回，写在后面那一行永远执行不到（实测：need_lift 卡在 1，
    // 下一次真实手势被永久拒绝——比原缺陷更糟）。
    // The re-entry guard does exactly one thing: no new gesture until both contacts have
    // really left. It is strictly independent of the release above - the swallow may be
    // released on a sick frame (so the single finger revives at once), but the gesture must
    // not restart, or the same hold would be recognized again and fire a second RETRACT.
    // Clearing it must come BEFORE the early return below: with count==0 and the state
    // already back at IDLE that path returns immediately, so a clear placed after it never
    // runs (measured: need_lift stuck at 1 and the next real gesture refused forever -
    // worse than the defect being fixed).
    if (t->count == 0) dual->need_lift = false;
    if (t->count == 0 || (t->count >= 2 && !two)) {
        dual->suppress = false;
        if (t->count == 0 && dual->state == UI_DUAL_IDLE) {
            dual->last_ms = now;
            return false;
        }
    }

    // 26.9.6 **现象位锁存**：每拍都独立地看一眼「芯片还在声称 active、坐标却全零」的
    // 病灶点，与这一拍是不是判定拍无关。26.9.5 把它打在判定位，round-3 直接恒零——
    // 现象帧常常比判定帧早一拍（污染帧 count=2 → 判定帧 count 已降），帧级快照采不到。
    // 锁存到手势级后，「见没见过」与「在哪一拍判」彻底解耦。
    // 26.9.6 symptom latch: every tick independently looks for a sick point (the chip
    // still claiming active while the coordinates read all-zero), regardless of whether
    // this is the verdict tick. 26.9.5 marked it at the verdict site and round 3 read
    // zero every time - the symptom frame is often one tick ahead of the verdict (sick
    // frame has count==2, the verdict tick has already dropped). Latching at gesture
    // level decouples "was it seen" from "which tick judged".
    if (ui_dual_saw_zero_frame(t)) dual->saw_zero = true;

    // 26.9.5/26.9.6 归零修复：**状态机的 two 与 two_point_gap 共用同一个判据**。
    // `count` 是驱动透传的芯片字段——抬手那一拍坐标已报 0、active 却仍为真（round-3
    // 实测：`pt->active = event != 1`，只有芯片明确报 up 才置假，过渡帧报的仍是
    // contact），count 也仍是 2（cst836u.c 里 count = data[2] & 0x0F）。**所以判据信
    // 位置不信标志位**：26.9.6 的判据在 active 之外还要两点各自带坐标（见
    // ui_dual_point_valid），只信 count 或只信 active 都会把抬手帧当成「两指还在」，
    // 于是下面的记录拿到 (0,0)、最后的 x_last 变成 0、抬手净位移 = −落点坐标。
    // 26.9.5/26.9.6 zero-drift repair: the machine's `two` shares one predicate with
    // two_point_gap. `count` is a pass-through of a chip field - on the lift tick the
    // coordinates are already zero while `active` is STILL TRUE (round-3 hardware:
    // `pt->active = event != 1`, so only an explicit chip "up" clears it and transition
    // frames still report a contact), and count stays 2 (count = data[2] & 0x0F in
    // cst836u.c). Hence the predicate trusts position, not flags: 26.9.6 requires each
    // point to carry coordinates on top of being active (see ui_dual_point_valid) -
    // trusting count alone, or active alone, reads a lift frame as "still two fingers",
    // so the record below picks up (0,0), x_last becomes 0, and the lift travel turns
    // into minus the landing point.
    // `two` 在本函数开头就已判定（本拍唯一一份），这里不再重复调用。
    // `two` was judged once at the top of this function (one value per tick); not re-evaluated.
    // 只在**真有两个有效触点**时刷新「最后有效中点」——有效点不足 2 的帧**冻结不动**
    // （沿用「刷新期丢弃」的先例：宁可沿用旧值，也不把没有位置信息的帧写进状态）。
    // 抬手那一拍 points 已被驱动清空，此时算中点没有意义（midpoint 直接回 false），
    // 旧代码却把 (0,0) 收进来。判定统一读这份记录。
    // Refresh the last valid midpoint ONLY when two contacts really are valid - frames
    // with fewer than two valid points FREEZE the record (same stance as the refresh-gap
    // discard: keep the old value rather than writing a frame that carries no position).
    // On the lift tick the driver has cleared points and a midpoint is meaningless
    // (midpoint() says so), yet the old code stored its (0,0) anyway.
    int mx = 0, my = 0;
    if (midpoint(t, &mx, &my)) {
        dual->x_last = mx;
        dual->y_last = my;
    } else if (dual->state == UI_DUAL_DOWN2 || dual->state == UI_DUAL_TRACK) {
        // 26.9.7 佐证计数：这一次冻结发生了。**不查 active**——判据只看坐标，所以芯片
        // 发的是「诚实型」（active 已假）还是「撒谎型」（active 仍真）归零帧，这里都
        // 照样记一笔。lift0 只能看见撒谎型，这正是它补不上的那一格：freeze_n > 0 =
        // 「冻结保护这条路真的被走过」，而不是「一次都没触发」。
        // 真抬手那一帧同样计入（那里也没有位置可写，冻结是正确行为），所以任何一次
        // 手势的 freeze_n 至少是 1——它证明机制在跑，不证明遇到了病灶。
        // 26.9.7 corroborating counter: a freeze just happened. It never looks at
        // `active` - the predicate only reads coordinates - so honest (active already
        // false) and lying (active still true) zeroing frames both count. lift0 can only
        // see the lying kind, and this is exactly the cell it cannot cover: freeze_n > 0
        // proves the freeze path was walked, not that it never triggered. The real lift
        // tick counts too, so any gesture scores at least 1.
        dual->freeze_n++;
    }

    switch (dual->state) {
        case UI_DUAL_IDLE: {
            // 重入守卫（need_lift）：上一次手势还没完全抬手、或刚被刷新期丢弃时，
            // 不接受新手势——手一直按着时不重开，否则一次长按会在每次刷新后被重新
            // 识别一遍，用户手没动却可能连发撤销甚至误切方向。
            // 26.9.12：判据从 `suppress` 换成 `need_lift`。两者曾是同一个字段，而框选态
            // 必须关掉抑制（不饿死单指），一关就顺带打开了重入——病灶帧收尾后坐标恢复
            // 有效，同一段按住会被重新识别。拆开之后：抑制管派发、need_lift 管重开。
            // Re-entry guard (need_lift): no new gesture while the previous one has not
            // fully lifted, or a refresh gap just dropped it - a held contact must not
            // restart, or one long hold would be re-recognized after every refresh and
            // could fire retracts or an orientation flip with a still hand.
            // 26.9.12: the predicate moved from `suppress` to `need_lift`. They used to be
            // one field, and the box mode has to clear the swallow (so the single finger is
            // not starved) - which silently opened re-entry: once a sick frame's
            // coordinates came back valid, the same hold was recognized again. Split apart:
            // the swallow governs dispatch, need_lift governs restarting.
            if (!two || dual->need_lift) {
                dual->last_ms = now;
                return false;
            }
            // 冷却：上次 FIRE 后的一小段时间内不接受新手势，防一次手势连发。
            // Cooldown: ignore new gestures briefly after a FIRE so one gesture
            // cannot fire twice.
            if (dual->fire_ms != 0 && now - dual->fire_ms < (int64_t)UI_G_COOLDOWN_MS) {
                dual->last_ms = now;
                return false;
            }
            dual->state = UI_DUAL_DOWN2;
            dual->t0_ms = now;
            dual->last_ms = now;
            dual->axis = 0;
            dual->peak_steps = 0;
            // 现象位锁存属于「这一次手势」：落定清零，之前的病灶帧不算到本次头上。
            // The symptom latch belongs to THIS gesture: cleared on landing so earlier
            // sick frames are not charged to this one.
            dual->saw_zero = false;
            // 佐证计数同样属于「这一次手势」：落定清零，之前的冻结帧不算到本次头上。
            // The corroborating counter also belongs to THIS gesture: cleared on landing.
            dual->freeze_n = 0;
            // two 已为真 → 中点必然算得出来；回 false 意味着判据被改坏了，此时绝不能
            // 落一个 x0=y0=0 的假基准。/ two is already true here, so a midpoint must
            // exist; false would mean the predicate was broken - never plant a
            // zeroed fake origin instead.
            if (!midpoint(t, &dual->x0, &dual->y0)) { dual->last_ms = now; return false; }
            dual->gap0 = two_point_gap(t); // M5-0：落定间距打点（不参与判定）
            dual->x_last = dual->x0;
            dual->y_last = dual->y0;
            dual->suppress = true;
            dual->need_lift = true;
            // 硬语义 1 · 不挂起 + 撤销：单指早已照常派发（可能已弹出词卡），
            // 这里只在双指成立瞬间请求静默撤销，绝不为了等第二指而延迟单指。
            // Hard rule 1 - no suspension, just retract: the single finger already
            // dispatched (maybe opening a card); we only ask for a silent retract
            // the moment two fingers are established, never delaying the first.
            out->type = UI_DUAL_EVT_RETRACT;
            return true;
        }
        case UI_DUAL_DOWN2: {
            if (!two) {
                // 融合窗内抬手 = 误触，零动作；抑制保持到两指全抬。
                // Lifting inside the fusion window is a slip: no action, and the
                // swallow holds until both contacts are gone.
                dual->state = UI_DUAL_ABORT;
                dual->last_ms = now;
                // 26.9.7：双指快速点一下就落在这条出口上——畸形输入的高发面，必须有名字。
                // 26.9.7: a quick two-finger tap lands exactly here - the hot spot for
                // malformed input, so it must name itself.
                out->abort = UI_DUAL_ABORT_SLIP;
                out->end = end_kind(t);
                return false;
            }
            if (now - dual->t0_ms >= (int64_t)UI_G_DUAL_FUSE_MS) dual->state = UI_DUAL_TRACK;
            dual->last_ms = now;
            return false;
        }
        case UI_DUAL_TRACK: {
            if (!two) {
                // 抬手才判定（M4-A v2 核心）：滑动过程中只累计位置，**这一拍**用最终
                // 净位移做一次判定。旧实现每拍尝试定轴、首次满足即锁死——起手瞬间
                // 哪一轴先占到便宜就定终身，斜滑因此「偶尔」跑到错误动作上（用户
                // 实测反馈）。改成净位移判定后，中途怎么拐都不影响结论。
                // The verdict happens HERE and only here (M4-A v2): the swipe merely
                // accumulates, and this tick judges the **final net travel**. The old
                // form tried to lock an axis every tick and froze on the first hit -
                // whichever axis won the opening instant decided the gesture, which is
                // exactly how a diagonal "occasionally" landed on the wrong action.
                // Judging net travel means mid-gesture turns cannot change the verdict.
                // 「全程无有效帧 → ABORT」由**结构**保证，不写成运行时分支（不可达的
                // if 门禁杀不死，是伪防线）：进入 DOWN2 的前置条件就是 two 为真**且**
                // midpoint() 成功（落定那一段），所以走到这里 x_last 至少被写过一次
                // 真中点。真要是有人放宽了进入条件，这里会退化成 dx=dy=0 并被死区
                // ABORT——结果同样是「宁可不做」，且由 M17/M18 守着。
                // "No valid frame at all -> ABORT" is guaranteed structurally rather than
                // by a runtime branch (an unreachable if cannot be killed by any gate and
                // is a fake defence): entering DOWN2 requires two==true AND a successful
                // midpoint(), so x_last has been written at least once by the time we get
                // here. Should somebody loosen that, this degrades to dx=dy=0 and the dead
                // zone aborts it - the same "rather do nothing" outcome, held by M17/M18.
                // 用滑动期间记下的最后有效中点（见上面 x_last/y_last 的注释）。
                // Use the last valid midpoint recorded while tracking.
                const int dx = dual->x_last - dual->x0;
                const int dy = dual->y_last - dual->y0;
                const int ax = abs_i(dx), ay = abs_i(dy);
                out->dx = dx;
                out->dy = dy;
                out->judged = true;
                // 现象位打标：这次手势见过病灶帧（芯片报 active 却给全零坐标）就置位，
                // 与「这一拍判了什么」无关。
                // **它不是「修复成立」的必需项**（26.9.7 按审计裁定修正原口径）：判据信
                // 位置不信 active，芯片若发的是诚实型归零帧（active 已假），这标天然全无，
                // 而那正是修复成立该有的样子。日志里的 `lift0=1` 只说明「这条命是从那条
                // 缝里钻进来的」；`lift0` 全无 + 零误发射同样是好消息。
                // Symptom mark: set if this gesture ever saw a sick frame (active with
                // all-zero coordinates), independent of what this tick judged.
                // **It is NOT a prerequisite for "the repair holds"** (corrected in 26.9.7 by
                // adjudication): the predicate trusts position, so an honest chip (active
                // already false) never lights it - which is exactly how a working repair
                // looks. `lift0=1` only means "this verdict came through that seam"; zero
                // lift0 with zero mis-fires is equally good news.
                out->lift0 = dual->saw_zero;
                // 佐证计数随判定送出：>0 = 「有效点不足 → 冻结 x_last」这条保护真的跑过。
                // 它不依赖 active 语义，正好补 lift0 的盲区（见头文件）。
                // Corroborating counter rides out with the verdict: >0 means the "fewer than
                // two valid points -> freeze x_last" guard really ran. It never consults
                // `active`, covering exactly lift0's blind spot (see the header).
                out->freeze_n = dual->freeze_n;
                // 收尾方式（26.9.7）：只看 count 与「有没有可用位置」，**不查 active**。
                // 所以 `end=zero`（count 仍 >=2 却没有两个可用触点）就是那条缝的**active 无关**
                // 指纹——诚实型病灶帧也会被它抓住，正好补 lift0 的盲区。
                // How the gesture ended (26.9.7): `count` plus "any usable position", never
                // `active`. `end=zero` (count>=2 yet no usable position) is therefore the
                // active-independent fingerprint of the seam - it catches honest sick frames
                // too, which is exactly lift0's blind spot.
                out->end = end_kind(t);

                const long dist2 = (long)dx * dx + (long)dy * dy;
                // 速度门用的欧氏距离与按下时长；只在抬手判定处求值一次。
                // Euclidean travel and hold duration for the speed gate (once, at lift).
                const int dist_px = isqrt_i((int)dist2);
                const int64_t hold_ms = now - dual->t0_ms;
                if (dist2 < (long)UI_G_DUAL_SWIPE_TRIGGER_PX * UI_G_DUAL_SWIPE_TRIGGER_PX) {
                    // 死区：轻触或抖动，什么都不做 / dead zone: a light touch or jitter
                    dual->state = UI_DUAL_ABORT;
                    out->abort = UI_DUAL_ABORT_DEADZONE;
                } else if (hold_ms > 0 &&
                           (long)dist_px * UI_G_DUAL_MIN_SPEED_DEN < (long)UI_G_DUAL_MIN_SPEED_NUM * hold_ms) {
                    // 速度门（26.9.4）：净位移够大但走得太慢 = 按住/沉降，不是滑动。
                    // 真机：双指按住框选姿势 0.10–0.394 px/ms，快滑 0.478–3.3，零重叠。
                    // 判据 dist/dt < 21/50；若放行，2 秒按住会被判成横滑而误翻屏。
                    // Speed gate: enough travel but too slow = a hold, not a swipe. On
                    // hardware holds run 0.10-0.394 px/ms vs swipes 0.478-3.3 (no
                    // overlap). Without it a 2s framing pose flipped the screen.
                    dual->state = UI_DUAL_ABORT;
                    out->abort = UI_DUAL_ABORT_SLOW;
                } else if ((long)ax * UI_G_DUAL_AXIS_RATIO_DEN >=
                           (long)ay * UI_G_DUAL_AXIS_RATIO_NUM) {
                    // 横轴（|dx| ≥ 1.6|dy|，置信层）→ 切横竖屏 / horizontal (1.6) → orientation.
                    // 互斥：此分支只发 ORIENT，绝不碰字号。/ Exclusive: ORIENT only.
                    dual->axis = 1;
                    dual->state = UI_DUAL_FIRE;
                    out->type = UI_DUAL_EVT_ORIENT;
                } else if ((long)ay * UI_G_DUAL_AXIS_RATIO_DEN >=
                           (long)ax * UI_G_DUAL_AXIS_RATIO_NUM) {
                    // 纵轴（|dy| ≥ 1.6|dx|，置信层）→ 字号 ±档。下滑 dy>0 = 加档，上滑 = 减档；
                    // UI_G_SWIPE_STEP_PX 只是「距离→档位」的量化，不是字号取值本身。
                    // Vertical → font step; down grows, up shrinks. The 120px constant
                    // quantifies travel, it is not a font value.
                    dual->axis = 2;
                    dual->peak_steps = clamp_steps(dy / UI_G_SWIPE_STEP_PX);
                    if (dual->peak_steps != 0) {
                        dual->state = UI_DUAL_FIRE;
                        out->type = UI_DUAL_EVT_FONT_STEP;
                        out->steps = dual->peak_steps;
                    } else {
                        // 定了纵轴却不足一档（40..119px）：静默不做。
                        // Vertical, but under one step (40..119px): silently nothing.
                        dual->state = UI_DUAL_ABORT;
                        out->abort = UI_DUAL_ABORT_SUBSTEP;
                    }
                } else if ((long)ax * UI_G_DUAL_FALLBACK_RATIO_DEN >=
                           (long)ay * UI_G_DUAL_FALLBACK_RATIO_NUM) {
                    // 模糊带（1.15 ≤ |dx|/|dy| < 1.6）→ 按较大轴（横）动作（C-guard）。
                    // 把「略偏横」的真实意图捞回来，但 1.0–1.15 仍静默（见末支）。
                    // Ambiguous (1.15-1.6), horizontal dominant -> act on it (C-guard);
                    // 1.0-1.15 stays silent (see the final branch).
                    dual->axis = 1;
                    dual->state = UI_DUAL_FIRE;
                    out->type = UI_DUAL_EVT_ORIENT;
                } else if ((long)ay * UI_G_DUAL_FALLBACK_RATIO_DEN >=
                           (long)ax * UI_G_DUAL_FALLBACK_RATIO_NUM) {
                    // 模糊带（1.15 ≤ |dy|/|dx| < 1.6）→ 按较大轴（纵）动作（C-guard）。
                    // Ambiguous (1.15-1.6), vertical dominant -> act on it (C-guard).
                    dual->axis = 2;
                    dual->peak_steps = clamp_steps(dy / UI_G_SWIPE_STEP_PX);
                    if (dual->peak_steps != 0) {
                        dual->state = UI_DUAL_FIRE;
                        out->type = UI_DUAL_EVT_FONT_STEP;
                        out->steps = dual->peak_steps;
                    } else {
                        dual->state = UI_DUAL_ABORT;
                        out->abort = UI_DUAL_ABORT_SUBSTEP;
                    }
                } else {
                    // 真模糊带（1.0 ≤ ratio < 1.15，含 45°）→ 静默丢弃（铁律：宁可不做）。
                    // Truly ambiguous (1.0-1.15, incl. 45 degrees) - silently dropped.
                    dual->state = UI_DUAL_ABORT;
                    out->abort = UI_DUAL_ABORT_AMBIG;
                }
                dual->fire_ms = now;
                dual->last_ms = now;
                return out->type != UI_DUAL_EVT_NONE;
            }
            // 滑动中：只更新心跳，**不做任何判定、不发任何事件**。
            // While sliding: heartbeat only - no verdict, no event.
            dual->last_ms = now;
            return false;
        }
        default:
            dual->state = UI_DUAL_IDLE;
            dual->last_ms = now;
            return false;
    }
}
