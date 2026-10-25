/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：gen26.5 M2 Step10 听书朗读的引擎封装（android.speech.tts）。
 * 设计要点：
 *
 *  - **跟随模式**：设备每翻页推一页 0x19，本类的 speakFollow 即「打断当前 +
 *    读新页」——翻页即切，无本地队列（设备是单一真相源，APK 不缓存未读书页）。
 *  - **引擎可配**：语速/音调档位存 SharedPreferences（离散档，循环切换）；
 *    引擎选择走系统 TTS 设置入口（Intent），本类不私存引擎包名——系统装了什么
 *    引擎是用户的决定。
 *  - **init 异步 latch**：TextToSpeech 构造后 onInit 之前到达的页面暂存，
 *    init 成功即补读——「开书即推首推」与「引擎刚初始化」竞态不丢页。
 *  - **诚实降级**：引擎缺失/初始化失败只置 notReady，调用方照常推帧，状态行
 *    如实显示「引擎不可用」——不假装在朗读。
 *
 * English: the gen26.5 M2 Step10 read-aloud engine wrapper. Follow mode: the
 * device pushes one page per turn and speakFollow is "interrupt and read the
 * new page" - turn means switch, with no local queue (the device is the single
 * source of truth; the APK caches no unread pages). Engine configurable: rate
 * and pitch are discrete steps persisted in SharedPreferences; engine choice
 * goes through the system TTS settings intent - which engines exist is the
 * user's decision. An async init latch stashes a page that arrives before
 * onInit and reads it once init succeeds, so the device's first push never
 * races engine startup. Missing/failed engines only set notReady: frames keep
 * flowing and the status row honestly says "engine unavailable".
 */

package dev.readpico.push;

import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.speech.tts.TextToSpeech;
import android.util.Log;

public final class TtsSpeaker {

    private static final String TAG = "TtsSpeaker";
    private static final String PREF = "tts";
    private static final String PREF_RATE = "rate_idx";
    private static final String PREF_PITCH = "pitch_idx";

    /** 语速档位（索引持久化）。/ Speech-rate steps (index persisted). */
    public static final float[] RATE_STEPS = {0.75f, 1.0f, 1.25f, 1.5f};
    /** 音调档位。/ Pitch steps. */
    public static final float[] PITCH_STEPS = {0.75f, 1.0f, 1.25f};
    private static final int RATE_DEFAULT = 1;  // 1.0
    private static final int PITCH_DEFAULT = 1; // 1.0

    private TextToSpeech tts;
    private volatile boolean ready;
    /** onInit 之前到达的待补读页（latch；init 成功后立即消费）。 */
    private String pendingFollow;
    /** 应用上下文（只用于读档位 prefs；不持 Activity）。/ App ctx for prefs only. */
    private Context appCtx;

    /** 创建（不初始化引擎）：init() 做异步构造。/ Create without initializing. */
    public TtsSpeaker() {
    }

    /** 异步初始化引擎。onInit 成功前 speak* 只暂存不丢。/ Async engine init. */
    public void init(final Context ctx) {
        appCtx = ctx.getApplicationContext();
        final SharedPreferences sp = appCtx.getSharedPreferences(PREF, Context.MODE_PRIVATE);
        tts = new TextToSpeech(appCtx, new TextToSpeech.OnInitListener() {
            @Override
            public void onInit(int status) {
                if (status != TextToSpeech.SUCCESS) {
                    Log.w(TAG, "tts init failed status=" + status);
                    ready = false;
                    return;
                }
                ready = true;
                applyRatePitch(sp);
                String stash = pendingFollow;
                pendingFollow = null;
                if (stash != null) {
                    speakFollow(stash); // 补读 init 期间到达的页 / catch-up page
                }
            }
        });
    }

    /** 关闭引擎（退出时调）。/ Shut the engine down (on exit). */
    public void shutdown() {
        ready = false;
        pendingFollow = null;
        appCtx = null;
        if (tts != null) {
            try {
                tts.stop();
            } catch (Throwable ignored) {
                // 引擎已死时 stop 也可能抛：关闭路径不二错。
            }
            tts.shutdown();
            tts = null;
        }
    }

    /** 引擎是否就绪。/ Whether the engine is ready. */
    public boolean ready() {
        return ready;
    }

    /**
     * 跟随朗读一页：打断当前朗读（QUEUE_FLUSH）后读新页。空页 = 静默
     * （设备侧本不推空页；此处防御性处理，与设备侧空页语义一致）。
     * Follow-read a page: interrupt whatever is being read, then read the new
     * page. An empty page is silent (the device never pushes one; this is the
     * defensive twin of that rule).
     */
    public void speakFollow(String page) {
        if (page == null) {
            return;
        }
        if (page.isEmpty()) {
            stopSpeaking();
            return;
        }
        if (!ready) {
            pendingFollow = page; // init latch：不丢页 / do not drop the page
            return;
        }
        tts.setSpeechRate(rate(appCtx));
        tts.setPitch(pitch(appCtx));
        tts.speak(page, TextToSpeech.QUEUE_FLUSH, null, "tts_page");
    }

    /**
     * 查词发音（gen26.5 Step10「AI 联动」入口）：打断当前跟读，读「词 + 释义」。
     * 当前页剩余部分被切——用户的注意力在词上；下一页推送自然续上跟随。
     * Pronounce a looked-up word: interrupt the current page and read the word
     * plus its definition. The rest of the current page is cut - the user's
     * attention is on the word, and the next page push resumes the follow.
     */
    public void speakWord(String word, String def) {
        if (!ready || word == null || word.isEmpty()) {
            return;
        }
        StringBuilder sb = new StringBuilder(word);
        if (def != null && !def.isEmpty()) {
            sb.append('。').append(def);
        }
        tts.setSpeechRate(rate(appCtx));
        tts.setPitch(pitch(appCtx));
        tts.speak(sb.toString(), TextToSpeech.QUEUE_FLUSH, null, "tts_word");
    }

    /** 立即停止发声（不停引擎。/ Stop speaking (engine stays up). */
    public void stopSpeaking() {
        if (tts != null) {
            try {
                tts.stop();
            } catch (Throwable ignored) {
                // 同 shutdown：引擎异常时静默收尾。
            }
        }
    }

    /** 当前语速档索引（越界历史数据按默认档）。/ Rate index; odd data reads default. */
    public int rateIdx(Context ctx) {
        int i = ctx.getSharedPreferences(PREF, Context.MODE_PRIVATE)
                .getInt(PREF_RATE, RATE_DEFAULT);
        return (i < 0 || i >= RATE_STEPS.length) ? RATE_DEFAULT : i;
    }

    /** 当前音调档索引。/ Pitch step index. */
    public int pitchIdx(Context ctx) {
        int i = ctx.getSharedPreferences(PREF, Context.MODE_PRIVATE)
                .getInt(PREF_PITCH, PITCH_DEFAULT);
        return (i < 0 || i >= PITCH_STEPS.length) ? PITCH_DEFAULT : i;
    }

    /** 循环到下一档并持久化；返回新索引。/ Cycle to the next step, persist, return it. */
    public int cycleRate(Context ctx) {
        int next = (rateIdx(ctx) + 1) % RATE_STEPS.length;
        ctx.getSharedPreferences(PREF, Context.MODE_PRIVATE).edit()
                .putInt(PREF_RATE, next).apply();
        return next;
    }

    public int cyclePitch(Context ctx) {
        int next = (pitchIdx(ctx) + 1) % PITCH_STEPS.length;
        ctx.getSharedPreferences(PREF, Context.MODE_PRIVATE).edit()
                .putInt(PREF_PITCH, next).apply();
        return next;
    }

    /** 打开系统 TTS 设置（引擎选择/安装语音数据由系统管）。/ Open system TTS settings. */
    public static void openSystemTtsSettings(Context ctx) {
        try {
            Intent i = new Intent("com.android.settings.TTS_SETTINGS");
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            ctx.startActivity(i);
        } catch (Throwable t) {
            // 个别 ROM 没有这个 action：退回通用设置页，用户自己找语音入口。
            // Some ROMs lack this action: fall back to generic Settings.
            try {
                ctx.startActivity(new Intent(android.provider.Settings.ACTION_SETTINGS)
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            } catch (Throwable t2) {
                // 连设置都没有（极端）：静默，调用方状态行如实显示。
            }
        }
    }

    private static float rate(Context ctx) {
        int idx = ctx == null ? RATE_DEFAULT
                : ctx.getSharedPreferences(PREF, Context.MODE_PRIVATE).getInt(PREF_RATE, RATE_DEFAULT);
        if (idx < 0 || idx >= RATE_STEPS.length) idx = RATE_DEFAULT;
        return RATE_STEPS[idx];
    }

    private static float pitch(Context ctx) {
        int idx = ctx == null ? PITCH_DEFAULT
                : ctx.getSharedPreferences(PREF, Context.MODE_PRIVATE).getInt(PREF_PITCH, PITCH_DEFAULT);
        if (idx < 0 || idx >= PITCH_STEPS.length) idx = PITCH_DEFAULT;
        return PITCH_STEPS[idx];
    }

    /** init 成功时把持久档位刷进引擎。/ Apply persisted steps on init success. */
    private void applyRatePitch(SharedPreferences sp) {
        try {
            tts.setSpeechRate(RATE_STEPS[sp.getInt(PREF_RATE, RATE_DEFAULT)]);
            tts.setPitch(PITCH_STEPS[sp.getInt(PREF_PITCH, PITCH_DEFAULT)]);
        } catch (Throwable ignored) {
            // 档位越界（历史数据）：保持引擎默认，不崩。
        }
    }
}
