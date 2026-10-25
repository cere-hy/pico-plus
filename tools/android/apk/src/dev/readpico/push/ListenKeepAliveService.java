/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：M1.3（v1.4.3）监听保活前台服务。点词监听（startListen）建立即启动、
 * 停止监听（stopListen）/ 退出即停——覆盖「扫描+连接+订阅+常驻监听」全段：
 * 息屏后 BLE notify 照常到达（不走网络栈），但 LLM 查询要走网络，Doze + 国产
 * ROM 会把后台网络挂起；FGS 提权保活让多数 ROM 恢复后台网络，BLE 连接稳定性
 * 同步受益。深度 Doze（桌面久置）下 FGS 不豁免网络挂起——那种场景由电池白名单
 * （MainActivity 引导式请求）+ 事件驱动重试（AiKeepAlive）兜底。
 *
 * 通知走静默低优先级渠道：用户不需要被「正在监听」打扰，状态行（保活：完整/
 * 受限）才是可见反馈；Android 13+ 的 POST_NOTIFICATIONS 被拒绝也不影响 FGS
 * 运行，仅通知不可见。
 *
 * English: the M1.3 (v1.4.3) listening keep-alive foreground service. Started
 * when the word-tap listener comes up, stopped when listening stops or the app
 * exits. BLE notify survives screen-off (it never touches the network stack),
 * but LLM queries do need the network, and Doze + Chinese-ROM power policies
 * suspend background networking; FGS elevates the process so most ROMs restore
 * it. Deep Doze is not exempted by FGS - that case falls to the battery
 * whitelist guide plus event-driven retry. The notification is a silent
 * low-priority channel; a denied POST_NOTIFICATIONS never stops the FGS.
 */

package dev.readpico.push;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;

/** 常驻监听保活：connectedDevice 类型（BLE 场景正名，Android 14 要求显式类型）。 */
public final class ListenKeepAliveService extends Service {

    private static final String CHANNEL_ID = "readpico_listen_keepalive";
    private static final int NOTIF_ID = 0x4C49; // 'LI'

    /** 启动（幂等）：调用方已在前台（按钮点击路径），不受后台启动 FGS 限制。 */
    public static void start(Context ctx) {
        try {
            Intent i = new Intent(ctx, ListenKeepAliveService.class);
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                ctx.startForegroundService(i);
            } else {
                ctx.startService(i);
            }
        } catch (Throwable t) {
            // 激进 ROM 可能拒启 FGS：不崩，如实降级（状态行会显示「受限」）。
            // An aggressive ROM may refuse the FGS: do not crash, degrade
            // honestly (the status row shows RESTRICTED).
        }
    }

    /** 停止（幂等）。 */
    public static void stop(Context ctx) {
        try {
            ctx.stopService(new Intent(ctx, ListenKeepAliveService.class));
        } catch (Throwable ignored) {
            // 停不掉的 service 不留悬挂状态：onDestroy 仍会跑。
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationManager nm = getSystemService(NotificationManager.class);
            if (nm != null) {
                NotificationChannel ch = new NotificationChannel(
                        CHANNEL_ID, "点词监听保活", NotificationManager.IMPORTANCE_LOW);
                ch.setDescription("保持后台监听与网络，息屏点词不断线（静默）");
                ch.setShowBadge(false);
                ch.enableVibration(false);
                ch.setSound(null, null);
                nm.createNotificationChannel(ch);
            }
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        Notification n = new Notification.Builder(this)
                .setSmallIcon(android.R.drawable.stat_notify_sync)
                .setContentTitle("点词监听中")
                .setContentText("保持后台连接，息屏点词照常出释义")
                .setOngoing(true)
                .setChannelId(Build.VERSION.SDK_INT >= Build.VERSION_CODES.O ? CHANNEL_ID : null)
                .build();
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                // API 29+：显式类型（connectedDevice = BLE 场景正名）。
                startForeground(NOTIF_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE);
            } else {
                startForeground(NOTIF_ID, n);
            }
        } catch (Throwable t) {
            // 类型声明与权限不匹配等系统性拒绝：降级为普通通知尝试，再不行就停。
            // A systemic refusal (type/permission mismatch): fall back to a
            // plain notification, and stop if even that fails.
            try {
                startForeground(NOTIF_ID, n);
            } catch (Throwable t2) {
                stopSelf();
            }
        }
        return START_NOT_STICKY; // 被系统杀掉不自动重建：重启后用户重开监听，状态如实
    }

    @Override
    public void onDestroy() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            stopForeground(STOP_FOREGROUND_REMOVE);
        } else {
            stopForeground(true);
        }
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null; // 仅启动式绑定 / started service only
    }
}
