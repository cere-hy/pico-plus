#pragma once
#include <stdbool.h>
#include <stdint.h>

// 状态栏差刷的「值变才刷」判据（26.11.16 电量批 / 26.11.18 节拍批）。
// 只做判定，不碰硬件：给定当前快照、当前分钟、可见性、时间，回答「这一拍要刷哪一块」。
// 采样不在这里——值链每 200 ms 新一次（搭的是锁屏键轮询的门，见盘点 §三发现 7），
// 这里补的是画闸门。
// Repaint gate for the status strip. Pure decision logic: given the snapshot, the minute,
// visibility and time, answer "which rect to repaint this tick". Sampling stays outside.

/// 上次上屏值未知（页面刚进来，render 已经画过正确值）：首帧只记录，不刷。
#define NAV_WATCH_UNKNOWN (-2)

/// 分钟源缺席：这一页有自己的分钟节拍（self_clock_tick），或时间尚未校时。
/// 与 `ui_nav_clock_minute()` 的 -1 同值——两层同义，不做转换，免得出现第三种「无时间」。
#define NAV_WATCH_NO_MINUTE (-1)

/// charging 变化后的去抖窗。插拔线电平会抖，不允许穿透成一串刷屏。
#define NAV_WATCH_DEBOUNCE_MS 2000

typedef struct {
    int16_t shown_percent;   // 上次真正上屏的百分比（NAV_WATCH_UNKNOWN = 未知）
    bool shown_charging;     // 上次真正上屏的充电位
    bool pending;            // charging 变化已挂起，等去抖窗
    uint32_t pending_since;  // 挂起起点（ms，允许回绕）
    // 分钟用 int32：epoch 分钟约 2.96e7，int16 会截断成低 16 位，
    // 每 65,536 分钟（≈45.5 天）回绕一次并误判「没变」。int32 够到公元 4085 年。
    // int32 because epoch minutes are ~2.96e7: int16 keeps the low bits and wraps every
    // 65,536 minutes (~45.5 days), misreading it as "unchanged".
    int32_t shown_minute;
} nav_watch_t;

typedef enum {
    NAV_WATCH_IDLE = 0,
    /// 只刷电量区（值变了，时钟没变）。
    NAV_WATCH_REPAINT = 1,
    /// 刷整条状态栏（分钟变了；它包含电量区，所以同拍双变也只刷这一次）。
    NAV_WATCH_REPAINT_STRIP = 2,
} nav_watch_action_t;

void nav_watch_init(nav_watch_t *w);

/// 每一拍调一次。返回非 IDLE 时调用方按返回值选矩形、画完调用 nav_watch_commit()。
/// `minute` 传 NAV_WATCH_NO_MINUTE 表示本拍不参与分钟判定（页面自己有节拍 / 时间无效）。
nav_watch_action_t nav_watch_step(nav_watch_t *w, int percent, bool charging, int64_t minute,
                                  bool visible, uint32_t now_ms);

/// 上屏成功后调用，把「上次上屏值」落定为这一组值（三个源一起提交，不是只提交触发源）。
void nav_watch_commit(nav_watch_t *w, int percent, bool charging, int64_t minute);
