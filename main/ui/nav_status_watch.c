#include "nav_status_watch.h"

void nav_watch_init(nav_watch_t *w) {
    w->shown_percent = NAV_WATCH_UNKNOWN;
    w->shown_charging = false;
    w->pending = false;
    w->pending_since = 0;
    w->shown_minute = NAV_WATCH_NO_MINUTE;
}

nav_watch_action_t nav_watch_step(nav_watch_t *w, int percent, bool charging, int64_t minute,
                                  bool visible, uint32_t now_ms) {
    // 看不见就不刷，顺手清掉挂起与两个账：不可见期间的变化不该在回来时补刷一次旧值。
    // 同时把「上次上屏值」退回未知——覆盖层关掉或切页回来时，页面自己已经重绘过，
    // 这一拍只该登记，不该再补一刷。分钟账一起清：回来时页面画的就是当前分钟。
    // Hidden: drop the pending edge and forget both painted values. When the overlay
    // closes or the page returns, that page has just repainted; record, don't replay.
    if (!visible) {
        w->pending = false;
        w->shown_percent = NAV_WATCH_UNKNOWN;
        w->shown_minute = NAV_WATCH_NO_MINUTE;
        return NAV_WATCH_IDLE;
    }

    // 首帧：页面 render 用的就是当前快照与当前分钟，这里只登记，不再多刷一次。
    // First frame: render already painted this snapshot and this minute; record, don't repaint.
    if (w->shown_percent == NAV_WATCH_UNKNOWN) {
        nav_watch_commit(w, percent, charging, minute);
        return NAV_WATCH_IDLE;
    }

    // 分钟支先判：它刷整条，而整条包含电量区，所以「同拍双变」在这里被一次吞掉。
    // -1（未校时 / 本页自己有节拍）不参与判定——它不是一个「分钟值」，是「没有分钟」。
    // 反方向（曾有 N，忽然变 -1）同样不判：时间一旦校上就不会失效，真失效也该由页面
    // 的整页重绘去画 `--:--`，不在这里凭空刷一条。
    // Minute first: it repaints the whole strip, which contains the battery rect, so a
    // same-tick double change collapses into this one pass. -1 is not a minute value, it
    // is "no minute" (uncalibrated, or this page runs its own cadence).
    if (minute != NAV_WATCH_NO_MINUTE && minute != (int64_t)w->shown_minute) {
        w->pending = false;
        return NAV_WATCH_REPAINT_STRIP;
    }

    // 百分比是分钟级的事件，直接刷；charging 一并在这一刷里落定。
    // Percent moves on a minute scale: repaint at once, settling charging in the same pass.
    if (percent != (int)w->shown_percent) {
        w->pending = false;
        return NAV_WATCH_REPAINT;
    }

    // 抖动回到原值：窗内第二次变化被吞掉，不留尾巴。
    // Bounced back to the painted value: swallow the second edge inside the window.
    if (charging == w->shown_charging) {
        w->pending = false;
        return NAV_WATCH_IDLE;
    }

    if (!w->pending) {
        w->pending = true;
        w->pending_since = now_ms;
        return NAV_WATCH_IDLE;
    }
    // 无符号相减，回绕安全。
    if ((uint32_t)(now_ms - w->pending_since) < NAV_WATCH_DEBOUNCE_MS) return NAV_WATCH_IDLE;
    return NAV_WATCH_REPAINT;
}

void nav_watch_commit(nav_watch_t *w, int percent, bool charging, int64_t minute) {
    w->shown_percent = (int16_t)percent;
    w->shown_charging = charging;
    w->pending = false;
    // epoch 分钟 ~2.96e7 落在 int32 内（够到公元 4085 年）；-1 原样存，不做 0 填充，
    // 免得「未校时」被记成「1970 年那一分钟」。
    // Epoch minutes (~2.96e7) fit int32; store -1 as-is rather than mapping "uncalibrated"
    // onto "the minute of 1970".
    w->shown_minute = (int32_t)minute;
}
