/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：gen26.5 M2 Step10 听书朗读的前台保活服务。与 ListenKeepAliveService
 * 同型不同生：朗读期间提权保活（BLE notify 与 TextToSpeech 都不该被 Doze/国产
 * ROM 挂起），停读/断开即停；生命周期独立，不复用监听保活实例（两段会话可能
 * 同时存在：点词监听 + 听书朗读）。
 *
 * 通知走静默低优先级渠道：用户不需要被「正在朗读」打扰，状态行才是可见反馈；
 * Android 13+ 的 POST_NOTIFICATIONS 被拒不影响 FGS 运行，仅通知不可见。
 * 被系统杀掉不自动重建（START_NOT_STICKY）：重启后回到「未朗读」的真实状态，
 * 设备侧下一页推送自然续上——听书是跟随模式，无本地队列需要恢复。
 *
 * English: the gen26.5 M2 Step10 read-aloud keep-alive foreground service.
 * Same shape as ListenKeepAliveService but a separate life: elevated for the
 * duration of a read-aloud session (neither BLE notify nor TextToSpeech should
 * be suspended by Doze/ROM policy), stopped when reading stops or the link
 * drops. Its lifetime is independent - the two services never share an
 * instance, and both sessions may coexist (word-tap listening + read-aloud).
 *
 * The notification is a silent low-priority channel; a denied
 * POST_NOTIFICATIONS never stops the FGS. Not sticky: after a system kill the
 * honest state is "not reading", and the device's next page push resumes the
 * follow naturally - there is no local queue to restore.
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

/** 听书朗读保活：connectedDevice 类型（BLE 场景正名，Android 14 要求显式类型）。 */
public final class TtsKeepAliveService extends Service {

    private static final String CHANNEL_ID = "readpico_tts_keepalive";
    private static final int NOTIF_ID = 0x5454; // 'TT'

    /** 启动（幂等）：调用方已在前台（开关点击路径），不受后台启动 FGS 限制。 */
    public static void start(Context ctx) {
        try {
            Intent i = new Intent(ctx, TtsKeepAliveService.class);
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                ctx.startForegroundService(i);
            } else {
                ctx.startService(i);
            }
        } catch (Throwable t) {
            // 激进 ROM 可能拒启 FGS：不崩，如实降级（朗读继续，只是可能被挂起）。
            // An aggressive ROM may refuse the FGS: do not crash, degrade
            // honestly (reading continues, it may just get suspended).
        }
    }

    /** 停止（幂等）。 */
    public static void stop(Context ctx) {
        try {
            ctx.stopService(new Intent(ctx, TtsKeepAliveService.class));
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
                        CHANNEL_ID, "听书朗读保活", NotificationManager.IMPORTANCE_LOW);
                ch.setDescription("保持后台连接与朗读，息屏跟读不断（静默）");
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
                .setContentTitle("听书朗读中")
                .setContentText("保持后台连接，翻页自动跟读")
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
        return START_NOT_STICKY; // 被系统杀掉不自动重建：状态如实，设备续推自然接上
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
