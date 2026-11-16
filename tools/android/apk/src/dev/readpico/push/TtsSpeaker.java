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
import android.speech.tts.UtteranceProgressListener;
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

    /**
     * gen30 伴读案：跟随朗读的播完回调（R0 §四「APK 播完一律回 TTS_DONE」）。
     * 设备端按 NVS 设置自治消费（仅 FINISHED + 自动翻页开才翻页；ABORT/FAILED 忽略），
     * 所以 APK 侧如实上报三种终态即可，无需查询设备使能态。
     * gen30 read-along: follow-read completion callback. The device consumes it
     * autonomously (only FINISHED with auto-turn on advances); the APK reports the
     * three outcomes honestly and never queries the device's enable flag.
     */
    public interface FollowDoneListener {
        /**
         * @param pageNo 本次朗读发起时绑定的 PAGE_BEGIN 页号（>=0 才有对账基准）
         * @param status 与 PushProto.TTS_DONE_* 对齐：0 播完 / 1 中断 / 2 失败
         */
        void onFollowDone(int pageNo, int status);
    }

    /** utteranceId 前缀：把页号编进 id，QUEUE_FLUSH 打断上一页时不与新页串号。 */
    private static final String FOLLOW_UTTERANCE_PREFIX = "tts_page:";

    private TextToSpeech tts;
    private volatile boolean ready;
    /** onInit 之前到达的待补读页（latch；init 成功后立即消费）。 */
    private String pendingFollow;
    /** latch 页对应的 PAGE_BEGIN 页号；-1 = 纯听书（无对账基准，不回 TTS_DONE）。 */
    private int pendingFollowPageNo = -1;
    /** 播完回调（MainActivity 持有；shutdown 时清空，防回调打进已死 Activity）。 */
    private FollowDoneListener followDoneListener;
    /** 应用上下文（只用于读档位 prefs；不持 Activity）。/ App ctx for prefs only. */
    private Context appCtx;

    /** 创建（不初始化引擎）：init() 做异步构造。/ Create without initializing. */
    public TtsSpeaker() {
    }

    /** gen30：注册播完回调（MainActivity 在 init() 之前调用）。/ Register the callback. */
    public void setFollowDoneListener(FollowDoneListener l) {
        followDoneListener = l;
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
                installFollowDoneListener();
                String stash = pendingFollow;
                final int stashPageNo = pendingFollowPageNo;
                pendingFollow = null;
                pendingFollowPageNo = -1;
                if (stash != null) {
                    speakFollow(stash, stashPageNo); // 补读 init 期间到达的页 / catch-up page
                }
            }
        });
    }

    /** 关闭引擎（退出时调）。/ Shut the engine down (on exit). */
    public void shutdown() {
        ready = false;
        pendingFollow = null;
        pendingFollowPageNo = -1;
        followDoneListener = null; // 先清回调，下面的 tts.stop() 不会打进已死 Activity
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
        speakFollow(page, -1);
    }

    /**
     * gen30 重载：绑定 PAGE_BEGIN 页号，播完回调据此回 TTS_DONE（page_no 对账用）。
     * 不传页号（纯听书，无 0x1C 前置帧）时 utteranceId 不带页号，回调静默不回帧
     * ——设备端仅伴读态有 PAGE_BEGIN 对账基准，纯听书回帧本就会被丢弃（R0 §一）。
     * 每次朗读把页号编进 utteranceId：QUEUE_FLUSH 打断上一页时，旧朗读的 onStop
     * 仍带旧页号，不与新页串号；设备端「page_no ≠ 当前页忽略」再兜一层。
     * gen30 overload: bind the PAGE_BEGIN page number into the utterance id so a
     * flushed (interrupted) page cannot be mistaken for the new page.
     */
    public void speakFollow(String page, int pageNo) {
        if (page == null) {
            return;
        }
        if (page.isEmpty()) {
            stopSpeaking();
            return;
        }
        if (!ready) {
            pendingFollow = page; // init latch：不丢页 / do not drop the page
            pendingFollowPageNo = pageNo;
            return;
        }
        tts.setSpeechRate(rate(appCtx));
        tts.setPitch(pitch(appCtx));
        final String utteranceId = (pageNo >= 0)
                ? (FOLLOW_UTTERANCE_PREFIX + pageNo) : "tts_page";
        tts.speak(page, TextToSpeech.QUEUE_FLUSH, null, utteranceId);
    }

    /**
     * gen30：安装播完 / 中断 / 失败监听。只处理 "tts_page:&lt;no&gt;" 前缀；speakWord
     * 的 "tts_word" 与无页号的 "tts_page" 都不触发 TTS_DONE。回调线程由引擎决定，
     * 故调用方（MainActivity）自行切线程写帧，本类不碰 UI、不碰协议常量。
     */
    private void installFollowDoneListener() {
        if (tts == null) {
            return;
        }
        int rc = tts.setOnUtteranceProgressListener(new UtteranceProgressListener() {
            @Override public void onStart(String utteranceId) { }

            @Override public void onDone(String utteranceId) {
                fireFollowDone(utteranceId, 0); // TTS_DONE_FINISHED
            }

            @Override public void onError(String utteranceId) {
                fireFollowDone(utteranceId, 2); // TTS_DONE_FAILED
            }

            @Override public void onStop(String utteranceId, boolean interrupted) {
                fireFollowDone(utteranceId, 1); // TTS_DONE_ABORT
            }
        });
        if (rc != TextToSpeech.SUCCESS) {
            Log.w(TAG, "setOnUtteranceProgressListener failed rc=" + rc);
        }
    }

    /** 解析 utteranceId 里的页号并上报；无页号 / 非法一律静默（不伪造回执）。 */
    private void fireFollowDone(String utteranceId, int status) {
        if (utteranceId == null || !utteranceId.startsWith(FOLLOW_UTTERANCE_PREFIX)) {
            return;
        }
        final int pageNo;
        try {
            pageNo = Integer.parseInt(utteranceId.substring(FOLLOW_UTTERANCE_PREFIX.length()));
        } catch (NumberFormatException e) {
            return;
        }
        if (pageNo < 0) {
            return;
        }
        FollowDoneListener l = followDoneListener;
        if (l != null) {
            l.onFollowDone(pageNo, status);
        }
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
