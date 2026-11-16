/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：ReadPico 墨水屏的安卓推送 App（单 Activity，纯框架 API，无第三方依赖）。
 * 界面：一个输入框（启动自动读剪贴板）+「读取剪贴板」+「发送到墨水屏」按钮 +
 * 日志区 + 单词卡模式开关。协议层在 PushProto.java（与 push_proto.h 逐字段对表，
 * 宿主测试 25/25 通过）。
 *
 * 推送流程（与 push_pico.py / ReadPicoPush.bsh 一致）：
 *   扫描 PicoPush-XXXX（12s） -> 连接 -> 协商 MTU 247 -> 发现服务 ->
 *   订阅 5f13 通知 -> 写 BEGIN 到 5f12 -> 等 READY/RESUME -> DATA×N 写 5f11（WWR，
 *   逐包等 ATT 回执） -> 写 END 到 5f12 -> 等 DONE/ERR。
 *   写入方式实证修正记录（gen12）：固件 5f11/5f12 特性位一直是 F_WRITE（WWR），
 *   F_WRITE_NO_RSP 从未在固件出现；ATT 逐包回执是第一道防线，设备侧 RESUME
 *   是第二道防线。
 *   gen14 加固：MTU 协商失败（<32）明确失败重试，不闷头走 Prepare Write 长写
 *   （固件 prep 队列已加深到 16 治 rc=6，但各安卓栈长写行为参差）；RESUME 帧
 *   进 STATUS 候选（offset 0 按 READY 继续，非 0 明确提示重新完整发送）。
 *   gen14 hardening: a failed MTU exchange (<32) fails explicitly for a retry
 *   instead of silently entering a Prepare Write long write (the firmware prep
 *   queue is now 16 deep to fix rc=6, but Android long-write behaviour varies);
 *   RESUME joins the STATUS candidates (offset 0 continues as READY, non-zero
 *   prompts a full resend).
 *
 * English: minimal single-activity push client for the ReadPico e-ink device.
 * Pure framework APIs; protocol codec lives in PushProto.java.
 */
package dev.readpico.push;

import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.BroadcastReceiver;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.location.LocationManager;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.provider.Settings;
import android.text.method.ScrollingMovementMethod;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import android.app.AlertDialog;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.Charset;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

public class MainActivity extends Activity {

    private static final Charset UTF8 = Charset.forName("UTF-8");
    // gen22 一键推流快捷方式：长按桌面图标 →「一键推流」→ 读剪贴板并直接推送，
    // 不建界面（透明 Activity），只弹 Toast。
    // gen22 one-tap push shortcut: long-press the launcher icon -> "一键推流" ->
    // read the clipboard and push straight away, no layout (translucent), Toast only.
    public static final String ACTION_PUSH_CLIPBOARD = "dev.readpico.push.ACTION_PUSH_CLIPBOARD";
    private static boolean sShortcutMode = false;
    private static final int REQ_PERMS = 1;
    private static final int REQ_ENABLE_BT = 2;
    private static final int REQ_IMPORT = 3;    // SAF 打开模板库 JSON
    private static final int REQ_EXPORT = 4;    // SAF 创建模板库 JSON
    private static final int REQ_NOTIF = 5;     // M1.3：POST_NOTIFICATIONS（拒绝不阻塞）
    private static final long SCAN_TIMEOUT_MS = 12000;
    private static final long CONNECT_TIMEOUT_MS = 20000;
    private static final long STEP_TIMEOUT_MS = 20000;
    private static final long FINAL_TIMEOUT_MS = 60000; // END 后设备要校验+落盘，放宽
    // gen15：session_id 必须每次推送都不同。设备端 CONNECTED+BEGIN 有一条
    // 「同 sid 且 has_part → 直接 RESUME」的分支；sid 恒定（原为 1）时，只要有一次
    // 推送失败留下 .part，此后**每一次**推送都会命中该分支被判成续传，而本客户端
    // 明确不支持续传（gen14 起遇到非 0 offset 就提示重发并放弃）→ 永久卡死。
    // gen14 真机铁证：首推 CRC 失败后，连续 4 次重连都只有「一次 notify 随即断开」。
    // gen15: the session id must differ per push. The device's CONNECTED+BEGIN path
    // resumes whenever the same sid arrives while a partial is on disk; with a
    // constant sid (previously 1), a single failed push poisons every later push
    // into a resume this client cannot serve (since gen14 it bails out on any
    // non-zero offset) - a permanent deadlock. gen14 hardware evidence: after the
    // first CRC failure, four reconnects each ended after a single notify.
    private static int nextSessionId() {
        return ((int) (System.nanoTime() >>> 1)) | 1; // 非零、逐次不同 / non-zero, distinct
    }

    private EditText input;
    private CheckBox wordCard;
    private TextView logView;
    private ScrollView logScroll;
    // gen23：定位服务状态横幅（红=缺权限/GPS 未开，绿=就绪）。
    // gen23: location-services status banner (red = missing, green = ready).
    private TextView locBanner;

    private BluetoothAdapter adapter;
    private BluetoothGatt gatt;

    /* ---- gen26 M1：AI 查词应答链 ----
     * 设备点词 -> STATUS 0x17 -> APK 调 LLM -> CTRL 0x07 回传 -> 设备出释义卡。
     * 要收到 0x17 就必须**保持连接**（现有推送流程是推完即断），所以监听是
     * 一条独立的显式路径：不改 doPush 一行，老行为零退化。
     * To receive 0x17 the link must stay up (the existing push flow disconnects
     * when done), so listening is an explicit, separate path: doPush is
     * untouched and the old behavior cannot regress.
     */
    private TextView aiStatus;
    private Button btnListen;
    private volatile boolean listenMode = false;
    private volatile long devCaps = -1L;              // 设备能力位图（0x18 上报）
    private volatile BluetoothGattCharacteristic cCtrlAi = null; // 回 0x07 用的 CTRL 特征
    /** BLE 写串行锁：GATT 同时只容一个操作，AI 回帧与推送不得并发。 */
    private final Object writeLock = new Object();
    // M1.2 ①：导出模板库时的暂存 JSON 串（onActivityResult 里写盘用）。
    // M1.2 item-1: the staged JSON while exporting the template library (written in
    // onActivityResult).
    private String pendingExportJson = null;
    // M1.2.1：AI 设置页的激活模板编辑框引用。模板库任何操作后必须回填它，
    // 保证「主编辑框内容 == 存储里的激活模板」——单一真相源，不做内存双份。
    // M1.2.1: handle to the active-template editor in the AI settings page. After
    // any template-library op it MUST be refreshed so the editor == the stored
    // active template (single source of truth, no dual in-memory copy).
    private EditText mActiveTplEditor = null;

    /* ---- M1.3（v1.4.3）：息屏保活 + 事件驱动重试 ----
     * 真机账：手机息屏后查词 DNS 失败。BLE notify 照常到达（不走网络栈），但
     * LLM 查询的后台网络被 Doze + 国产 ROM 挂起，解锁即恢复。三件对策：
     * ① 前台服务（ListenKeepAliveService）：开监听即启、停监听/退出即停，
     *    connectedDevice 类型；多数 ROM 下恢复后台网络，BLE 稳定性同步受益；
     * ② 电池白名单引导（requestBatteryWhitelist）：只引导不代决，状态行
     *    「保活：完整/受限」如实组合 FGS 与白名单；
     * ③ 事件驱动重试（AiKeepAlive 纯逻辑）：失败后 3s 快速重试一次防 DNS
     *    抖动，之后只挂 onAvailable / 亮屏，14s 窗口 cap 贴设备 15s 超时。
     * Screen-off DNS failures: BLE notify still arrives, but the LLM query's
     * background network is suspended by Doze/ROM policy. Three countermeasures:
     * FGS, a guided battery-whitelist request, and event-driven retry.
     */
    /** 本 APK 侧对 FGS 运行状态的最佳努力记录（start 置位 / stop 清位）。 */
    private volatile boolean mFgsRunning = false;
    /** 快速重试专用主线程 Handler（只排 3s 一次性，不轮询）。 */
    private final Handler mQuickRetryHandler = new Handler(Looper.getMainLooper());
    /** 当前点词的重试计划；null = 无未决查询。新点词接管时旧计划 markFinished。 */
    private volatile AiKeepAlive mAiRetry = null;
    /** 挂起中的快速重试 Runnable（事件抢先时撤回，防双发）。 */
    private Runnable mQuickRetryRunnable = null;
    /** 网络可用事件回调（M1.3 注册，查询终态/停监听即注销）。 */
    private ConnectivityManager.NetworkCallback mNetworkCallback = null;
    /** 亮屏/解锁事件接收器（同上）。 */
    private BroadcastReceiver mKeepAliveReceiver = null;

    /* ---- gen26.5 M2 Step10：TTS 听书（0x19 页文本 → 本地朗读）----
     * 设备侧「听书」面板开启跟随后，每翻页推一页 0x19；本侧重组回整页交
     * TextToSpeech 朗读。跟随模式 = 打断当前读新页，无本地队列——设备是单一
     * 真相源，APK 不缓存未读书页。FGS 独立于监听保活（两段会话可同时存在：
     * 点词监听 + 听书朗读，互不抢占）；10 分钟无新页即停保活（读停了不空耗，
     * 当前页若仍在读不打断），断连即停 + 重组器复位（半页残留不留）。
     * With the device's TTS panel in follow mode, every page turn pushes one
     * 0x19 page; this side folds it back and reads it aloud. Follow mode =
     * interrupt and read the new page, no local queue (the device is the
     * single source of truth). The FGS is independent of the listening
     * keep-alive (both sessions may coexist); 10 minutes without a new page
     * stops the keep-alive (an in-flight long page is never cut), and a
     * disconnect stops it and resets the reassembler.
     */
    private TextView ttsStatus;
    private Button btnTtsSettings;
    private Button btnTtsStop;
    /** 0x19 页文本重组器（只由 BLE 回调线程 touch：accept/reset 同线程，无锁）。 */
    private final TtsReassembler ttsReassembler = new TtsReassembler();
    /** M3：0x1A 情景上下文重组器（同样只由 BLE 回调线程 touch）。 */
    private final SceneReassembler sceneReassembler = new SceneReassembler();
    /** 朗读引擎封装（异步 init + 档位持久化 + 跟随/查词两个入口）。 */
    private final TtsSpeaker ttsSpeaker = new TtsSpeaker();
    /** 最近一页 0x19 收齐的时刻；0 = 本会话还没收到过页文本。 */
    private volatile long ttsLastPageMs = 0;
    /** 本会话已收齐并交朗读的页数（状态行展示）。 */
    private volatile int ttsPagesRead = 0;

    /* ---- gen30 伴读案（R0 冻结件 1）：STATUS 0x1B/0x1C 对账态 ----
     * 对账字段由 BLE 回调线程写、后台/UI 线程读（volatile）；摘要请求与回帧
     * 一律切后台线程、状态行切 UI 线程。伴读态以「收到过 0x1B/0x1C」为界：
     * 纯听书（只有 0x19、无 0x1C）不触发摘要请求，既有听书链零侵入。
     * gen30 read-along reconciliation; the plain read-aloud path (0x19 with no
     * 0x1C) never starts a summary request, so the existing chain is untouched.
     * ---- */
    /** 当前模式号（0 外语原著 / 1 小说网文 / 2 社科非虚构 / 3 古文诗词）。 */
    private volatile int companionMode = PushProto.RIDE_MODE_FOREIGN;
    /** 最近一次 PAGE_BEGIN 的页号；-1 = 本会话还没收到过对账帧。 */
    private volatile int companionPageNo = -1;
    /** 是否处于伴读态（收到过 0x1B 或 0x1C）。 */
    private volatile boolean companionActive = false;
    /** 0x1B 发出至新 PAGE_BEGIN 到达的在途窗：摘要请求挂起不串页（R0 §一）。 */
    private volatile boolean companionModeSwitchInFlight = false;
    /** 最近一次 0x19 组装完成的当前页正文（日志/朗读用；绝不作为回执比对基准——
     *  它会被翻页覆写，14:26 裁定 A4 实现要点）。 */
    private volatile String companionLastPage = null;
    /** 同页同模式同正文去重键（pageNo:mode:正文指纹，见 CompanionReqGuard）；
     *  旧摘要丢弃时清空。A3：含正文维度，同页异文不误判重复。 */
    private volatile String companionReqKey = null;
    /** 回执内容比对基准：每次真正发起请求时登记「该请求的正文快照」（局部引用的副本）。
     *  A4 用它作「当前登记的最新请求」，而非回执时现读已被覆写的 companionLastPage。 */
    private volatile String companionReqSnapshot = null;
    /**
     * 设备侧听书开关（0x1D 朗读使能位）。修复轮解耦的核心状态：
     * 页文本到达时摘要请求恒触发，只有本值为真才朗读。
     * <p>默认 {@code true}：与旧固件兼容——旧固件不发 0x1D，非伴读态又只在开关
     * 打开时才推流，所以默认开不会造成「该静音却出声」。收到 0x1D 后以设备为准。
     * <p>Device-side listening switch (0x1D enable bit). Summaries are always
     * requested; playback happens only when this is true. Defaults to true for
     * old-firmware compatibility (old firmware never sends 0x1D, and off-companion
     * pushes only while the switch is on, so the default cannot cause unwanted
     * audio). Once 0x1D arrives the device is authoritative.
     */
    private volatile boolean ttsEnableFromDevice = true;

    /** TTS 保活 FGS 运行态（最佳努力记录，与 ListenKeepAliveService 分开记）。 */
    private volatile boolean mTtsFgsRunning = false;
    /** 10 分钟无新页 → 停 TTS 保活。也作为「跟随朗读进行中」的判定窗。 */
    private static final long TTS_IDLE_STOP_MS = 10 * 60 * 1000;
    /** TTS 空闲停 FGS 的定时（UI 线程；每来新页重排）。 */
    private final Handler mTtsIdleHandler = new Handler(Looper.getMainLooper());
    private Runnable mTtsIdleStop = null;

    private static final String PREF = "ai";
    private static final String PREF_KEY = "api_key";
    private static final String PREF_BASE = "base_url";
    private static final String PREF_MODEL = "model";
    // M1.2 ①：提示模板库（命名模板列表，JSON 数组）与当前激活模板名。
    // M1.2 item-1: the prompt-template library (named templates, JSON array) and
    // the name of the currently active template.
    private static final String PREF_TPL_LIB = "tpl_lib";
    private static final String PREF_TPL_ACTIVE = "tpl_active";
    private static final String DEFAULT_TPL_NAME = "默认";
    // 模板导出/导入的 JSON 文件 MIME 与后缀。
    // MIME / extension for the template-library JSON file.
    private static final String TPL_MIME = "application/json";
    private static final String TPL_SUFFIX = ".templates.json";

    /* ---- 同步门：worker 线程等待 BLE 回调 ---- */
    private final Object lock = new Object();
    private int connState = -1;
    private int mtuValue = 23;
    private boolean mtuDone = false;
    private boolean svcDone = false;
    private int svcStatus = -1;
    private int writeStatus = -1;
    private boolean dscDone = false;
    private byte[] lastStatus = null;
    private boolean scanDone = false;
    private volatile String foundAddr = null;
    private volatile int foundToken = -1;
    private volatile String foundName = null;
    private String failReason = null;
    private volatile int scanFailedCode = -1; // 最近一次 onScanFailed 的 code，-1 = 无

    /* ---- gen22 防串台：候选设备列表（不再「扫到第一个就连」） ---- */
    // gen22 anti-crosstalk: candidate list (no longer "connect to the first hit").
    private static final long MULTI_SCAN_GRACE_MS = 2500; // 首个设备后继续收多久 / extra window after the first hit
    private static final class Found {
        final String addr;
        final String name;
        final int token;
        Found(String addr, String name, int token) { this.addr = addr; this.name = name; this.token = token; }
    }
    private final List<Found> candidates = new ArrayList<Found>();
    private volatile long firstFoundMs = 0;

    /* ---- BLE 回调 ---- */

    private final BluetoothGattCallback gattCb = new BluetoothGattCallback() {
        @Override
        public void onConnectionStateChange(BluetoothGatt g, int status, int newState) {
            if (newState == BluetoothGatt.STATE_DISCONNECTED) {
                // gen26.5 Step10：断连即停 TTS 保活 + 复位重组器（半页残留不留）。
                // 当前页若仍在本地朗读不打断——BLE 断了不影响 TextToSpeech 把
                // 这一页读完；设备侧重连后页变哨兵自然续推（推送失败时哨兵
                // 本就没更新，重连即补推）。
                // Disconnect: stop the TTS keep-alive and reset the reassembler
                // (no half-page residue). An in-flight local utterance is NOT
                // cut (a BLE loss never stops TextToSpeech mid-page), and the
                // device resumes on the next page change (a push that failed
                // for want of a link left its sentinel untouched, so a
                // reconnect re-pushes the current page).
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        stopTtsKeepAlive();
                        ttsReassembler.reset();
                        // gen30：断链即清伴读对账态；重连后首个 PAGE_BEGIN 自然同步
                        // （R0 §一「未连接时仅 NVS 记录，连接后首个 PAGE_BEGIN 同步」）。
                        companionActive = false;
                        companionPageNo = -1;
                        companionModeSwitchInFlight = false;
                        companionLastPage = null;
                        companionReqKey = null;
                        companionReqSnapshot = null;
                        refreshTtsStatus();
                    }
                });
            }
            synchronized (lock) {
                if (status != BluetoothGatt.GATT_SUCCESS
                        && newState == BluetoothGatt.STATE_DISCONNECTED) {
                    failReason = "连接中断（GATT status=" + status + "）";
                }
                connState = newState;
                lock.notifyAll();
            }
        }

        @Override
        public void onMtuChanged(BluetoothGatt g, int mtu, int status) {
            synchronized (lock) {
                if (status == BluetoothGatt.GATT_SUCCESS) {
                    mtuValue = mtu;
                }
                mtuDone = true;
                lock.notifyAll();
            }
        }

        @Override
        public void onServicesDiscovered(BluetoothGatt g, int status) {
            synchronized (lock) {
                svcStatus = status;
                svcDone = true;
                lock.notifyAll();
            }
        }

        @Override
        public void onCharacteristicWrite(BluetoothGatt g, BluetoothGattCharacteristic c, int status) {
            synchronized (lock) {
                writeStatus = status;
                lock.notifyAll();
            }
        }

        @Override
        public void onDescriptorWrite(BluetoothGatt g, BluetoothGattDescriptor d, int status) {
            synchronized (lock) {
                dscDone = true;
                lock.notifyAll();
            }
        }

        @Override
        public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c) {
            if (!PushProto.STATUS_UUID.equalsIgnoreCase(c.getUuid().toString())) {
                return;
            }
            byte[] v = c.getValue();
            byte[] copy = (v == null) ? null : v.clone();
            // gen26 M1：AI 帧在推送流程之外单独分发（0x18 记能力位 / 0x17 触发查词）。
            // 必须先分发再入 lastStatus——waitStatus 只认推送 opcode，不会吃掉 AI 帧，
            // 所以这里也无需担心抢走推送流程要等的帧。
            // The AI frames are dispatched outside the push flow; waitStatus only
            // accepts push opcodes, so this can never steal a frame the push is
            // waiting for.
            if (copy != null) {
                dispatchAiStatus(copy);
            }
            synchronized (lock) {
                lastStatus = copy;
                lock.notifyAll();
            }
        }
    };

    /* ---- 扫描回调：按广播名前缀 PicoPush- 过滤 ---- */

    private final ScanCallback scanCb = new ScanCallback() {
        @Override
        public void onScanResult(int callbackType, ScanResult result) {
            String name = null;
            if (result.getScanRecord() != null) {
                name = result.getScanRecord().getDeviceName();
            }
            if (name == null && result.getDevice() != null) {
                name = result.getDevice().getName();
            }
            Integer token = PushProto.tokenFromAdvName(name);
            if (token != null && result.getDevice() != null) {
                // gen22 防串台：不再「第一个命中就收工」。全部收集起来（按地址去重），
                // 由 scanForDevice 在宽限期后统一挑选——多台并存时交给用户点选，
                // 避免把私密长文推到别人的墨水屏上。
                // gen22 anti-crosstalk: no longer "first hit wins". Collect every match
                // (de-duplicated by address); scanForDevice picks after a grace window -
                // with several panels around, the user chooses, so a private long text
                // never lands on someone else's screen.
                synchronized (lock) {
                    String addr = result.getDevice().getAddress();
                    boolean dup = false;
                    for (int i = 0; i < candidates.size(); i++) {
                        if (candidates.get(i).addr.equals(addr)) { dup = true; break; }
                    }
                    if (!dup) {
                        candidates.add(new Found(addr, name, token.intValue()));
                        if (firstFoundMs == 0) firstFoundMs = System.currentTimeMillis();
                        lock.notifyAll();
                    }
                }
            }
        }

        @Override
        public void onScanFailed(int errorCode) {
            synchronized (lock) {
                scanFailedCode = errorCode;
                // SCAN_FAILED_ALREADY_STARTED(1) 可恢复：上一轮扫描还没停净。
                // 唤醒等待循环（不置 failReason），由 scanForDevice 的重试逻辑
                // 停一次再起——否则会白等满 12 秒。
                // SCAN_FAILED_ALREADY_STARTED (1) is recoverable: the previous
                // scan has not fully stopped. Wake the waiter (without setting
                // failReason) so the retry loop in scanForDevice can stop and
                // restart - otherwise we would idle out the full 12 seconds.
                if (errorCode == 1) {
                    if (candidates.isEmpty()) {
                        scanDone = true;
                        lock.notifyAll();
                    }
                    return;
                }
                if (candidates.isEmpty() && failReason == null) {
                    failReason = "扫描启动失败（code=" + errorCode + "）";
                    scanDone = true;
                    lock.notifyAll();
                }
            }
        }
    };

    /* ---- Activity 生命周期 ---- */

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // 26.11.3-C：把未知 flags 位的喊话口挂上 android.util.Log。重组器本身零
        // android 依赖（宿主可测），所以它不直接 Log——这一行是它和日志之间的桥。
        // Bridge the reassembler's unknown-flag shout to android.util.Log (the class
        // itself stays android-free so it can run on the host).
        SceneReassembler.flagSink = new SceneReassembler.FlagSink() {
            @Override
            public void onUnknownFlags(int flags) {
                android.util.Log.w("PicoPush", "scene: unknown flag bits 0x"
                        + Integer.toHexString(flags) + " (this build ignores them)");
            }
        };
        // setTheme 必须在 super.onCreate 之前：快捷方式用透明无界面主题，
        // 用户看不到任何窗口，只有 Toast。/ setTheme must precede super.onCreate:
        // the shortcut uses a translucent no-display theme so only a Toast shows.
        sShortcutMode = ACTION_PUSH_CLIPBOARD.equals(getIntent().getAction());
        if (sShortcutMode) {
            // 对话框主题：快捷方式必须弹出一个**可见的小窗口**才行。
            // 真机实测（荣耀 / Android 16）：无界面的透明 Activity 会被系统判为后台，
            // 读剪贴板被隐私权限拦截（弹「允许读取剪贴板」后选「仅使用期间允许」仍无效）。
            // 所以改为可见对话框 + 用户在窗口里点「发送」——有前台交互，系统放行。
            // Dialog theme: the shortcut MUST show a visible small window. On real
            // hardware (Honor / Android 16) a faceless translucent activity is treated
            // as background and clipboard reads get blocked by the privacy guard (even
            // after granting "only while in use"). A visible dialog plus an explicit tap
            // on 发送 is foreground interaction, which the system allows.
            setTheme(android.R.style.Theme_DeviceDefault_Dialog_MinWidth);
        }
        super.onCreate(savedInstanceState);
        if (sShortcutMode) {
            runShortcutPush();
            return;
        }

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(16);
        root.setPadding(pad, pad, pad, pad);

        TextView head = new TextView(this);
        head.setText("ReadPico 推送");
        head.setTextSize(20);
        head.setPadding(0, 0, 0, dp(8));
        root.addView(head, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        // gen23：定位服务检测横幅，紧跟标题之下。/ gen23: location banner right under the title.
        locBanner = new TextView(this);
        locBanner.setTextSize(13);
        locBanner.setPadding(dp(10), dp(8), dp(10), dp(8));
        locBanner.setVisibility(View.GONE); // onCreate 末尾按状态刷新显示
        root.addView(locBanner, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        input = new EditText(this);
        input.setHint("在此粘贴或输入要推送到墨水屏的文字……");
        input.setGravity(Gravity.TOP | Gravity.START);
        input.setTextSize(16);
        input.setBackgroundColor(0xFFF3F4F6);
        input.setPadding(dp(10), dp(10), dp(10), dp(10));
        root.addView(input, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

        wordCard = new CheckBox(this);
        wordCard.setText("单词卡模式（正文忽略，title 即单词）");
        wordCard.setPadding(0, dp(6), 0, dp(6));
        root.addView(wordCard, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);

        Button btnClip = new Button(this);
        btnClip.setText("读取剪贴板");
        btnClip.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                loadClipboard();
                uiLog("已从剪贴板读取（" + input.getText().length() + " 字符）");
            }
        });
        row.addView(btnClip, new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));

        Button btnSend = new Button(this);
        btnSend.setText("发送到墨水屏");
        btnSend.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                onSendClicked();
            }
        });
        row.addView(btnSend, new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        root.addView(row, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        // ---- gen26 M1：AI 查词应答（设置入口 + 常驻监听开关）----
        // 监听是显式开启的独立路径：不开就与老版本行为完全一致。
        // Listening is an explicit, separate path: off means byte-identical old behavior.
        aiStatus = new TextView(this);
        aiStatus.setTextSize(12);
        aiStatus.setPadding(0, dp(6), 0, dp(2));
        root.addView(aiStatus, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        LinearLayout rowAi = new LinearLayout(this);
        rowAi.setOrientation(LinearLayout.HORIZONTAL);

        Button btnAi = new Button(this);
        btnAi.setText("AI 设置");
        btnAi.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showAiSettings();
            }
        });
        rowAi.addView(btnAi, new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));

        btnListen = new Button(this);
        btnListen.setText("连接并监听点词");
        btnListen.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (listenMode) {
                    stopListen();
                } else {
                    startListen();
                }
            }
        });
        rowAi.addView(btnListen, new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        root.addView(rowAi, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        // ---- gen26.5 M2 Step10：TTS 听书（状态行 + 朗读设置）----
        // 设备侧工具条第 7 项「听书」打开跟随后，每翻页推一页 0x19 由本 App
        // 朗读；语速/音调档位与系统引擎入口在「朗读设置」。状态行只反映事实
        // （未开始 / 朗读中·页数·引擎态 / 已待机），不承诺推不到的声音。
        // With the device's 7th tool entry (听书) in follow mode, every page
        // turn arrives as 0x19 and is read aloud here; rate/pitch steps and
        // the system engine entry live in 朗读设置. The status row only
        // reports facts (idle / reading·pages·engine / standby).
        ttsStatus = new TextView(this);
        ttsStatus.setTextSize(12);
        ttsStatus.setPadding(0, dp(6), 0, dp(2));
        root.addView(ttsStatus, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        LinearLayout rowTts = new LinearLayout(this);
        rowTts.setOrientation(LinearLayout.HORIZONTAL);
        btnTtsSettings = new Button(this);
        btnTtsSettings.setText("朗读设置");
        btnTtsSettings.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showTtsSettings();
            }
        });
        rowTts.addView(btnTtsSettings, new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        // gen26.6 D2B：听书状态行加「停止朗读」按钮——手动即时停掉当前发声
        // （设备侧关开关经 D2A 推空页也能停，这里给用户一个本地兜底入口）。
        // gen26.6 D2B: a 停止朗读 (stop reading) button on the TTS status row -
        // a local, immediate stop of the current utterance (the device's D2A
        // empty-page stop also works; this is the user's local fallback entry).
        btnTtsStop = new Button(this);
        btnTtsStop.setText("停止朗读");
        btnTtsStop.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                ttsSpeaker.stopSpeaking();
                stopTtsKeepAlive();
                refreshTtsStatus();
                uiLog("已手动停止朗读");
            }
        });
        rowTts.addView(btnTtsStop, new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        root.addView(rowTts, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        // 步 5：更新检测入口（22:11 裁决 §四 最小集，UI 落点代码端自定，主界面独立一行）。
        // 手动触发；非阻塞卡片提示；仅引导跳转，不做应用内下载/静默安装。
        // Step5 update-check entry (verdict section 4): manual trigger, non-blocking
        // card, guide-to-browser only, no in-app download / silent install.
        LinearLayout rowUpdate = new LinearLayout(this);
        rowUpdate.setOrientation(LinearLayout.HORIZONTAL);
        Button btnUpdate = new Button(this);
        btnUpdate.setText("检查更新");
        btnUpdate.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                checkUpdate();
            }
        });
        rowUpdate.addView(btnUpdate, new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        root.addView(rowUpdate, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        logScroll = new ScrollView(this);
        logView = new TextView(this);
        logView.setTextSize(12);
        logView.setPadding(0, dp(8), 0, 0);
        logView.setMovementMethod(new ScrollingMovementMethod());
        logScroll.addView(logView, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        root.addView(logScroll, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

        setContentView(root);

        BluetoothManager bm = (BluetoothManager) getSystemService(Context.BLUETOOTH_SERVICE);
        adapter = (bm == null) ? null : bm.getAdapter();
        if (adapter == null) {
            uiLog("本机没有蓝牙硬件");
        }

        loadClipboard();
        uiLog("就绪。在墨水屏上开启「接收推送」后，点「发送到墨水屏」。");
        refreshLocationBanner(); // gen23：进入 App 检测一次定位服务/权限
        refreshAiStatus();       // gen26 M1：AI 状态行（key 是否配置 / 是否在监听）
        // gen26.5 Step10：异步起朗读引擎（onInit 成功前到达的页暂存，init 成功
        // 即补读——「开书即推首推」与「引擎刚初始化」竞态不丢页）；随后刷状态行。
        // Bring up the read-aloud engine asynchronously (a page that arrives
        // before onInit is stashed and read once init succeeds, so the
        // device's first push never races engine startup).
        // gen30：先装播完回调再异步 init（onInit 成功时补装 utterance listener）。
        ttsSpeaker.setFollowDoneListener(new TtsSpeaker.FollowDoneListener() {
            @Override public void onFollowDone(int pageNo, int status) {
                sendCompanionTtsDone(pageNo, status);
            }
        });
        ttsSpeaker.init(this);
        refreshTtsStatus();      // gen26.5 Step10：TTS 状态行
    }

    @Override
    protected void onResume() {
        super.onResume();
        // gen23：从系统设置（开 GPS / 授权限）返回时刷新一次横幅；事件触发，非轮询。
        // gen23: refresh the banner when returning from system settings; event-driven, not polling.
        if (!sShortcutMode) {
            refreshLocationBanner();
        }
        // M1.3：从电池白名单确认页返回时，保活档位可能已变——如实刷新状态行。
        // M1.3: returning from the whitelist page may change the tier - refresh.
        refreshAiStatus();
    }

    @Override
    protected void onDestroy() {
        listenMode = false; // gen26 M1：退出即停监听，不留悬挂连接
        // M1.3：退出即停保活 FGS + 注销事件 + 撤销未决重试（监听都没了，重试无意义）。
        // M1.3: stop the keep-alive FGS, drop event hooks and cancel pending retries.
        cancelAiRetry();
        mFgsRunning = false;
        ListenKeepAliveService.stop(this);
        // gen26.5 Step10：退出即停 TTS 保活 + 关引擎 + 复位重组器（半页不留）。
        // Exit: stop the TTS keep-alive, shut the engine, reset the reassembler.
        stopTtsKeepAlive();
        ttsReassembler.reset();
        ttsSpeaker.shutdown();   // 内部清 followDoneListener，防回调打进已死 Activity
        // gen30：退出即清伴读对账态（重连后首个 PAGE_BEGIN 自然同步，R0 §一）。
        companionActive = false;
        companionPageNo = -1;
        companionModeSwitchInFlight = false;
        companionLastPage = null;
        companionReqKey = null;
        companionReqSnapshot = null;
        closeQuietly();
        super.onDestroy();
    }

    /* ---- gen22 一键推流（桌面快捷方式） ---- */

    // 长按桌面图标点「一键推流」后走这里：无界面，读剪贴板 → 扫描连接 → 推送 → Toast 收尾。
    // Android 10+ 禁止后台读剪贴板，所以必须由快捷方式拉起这个（透明的）Activity——
    // 它处于前台，读剪贴板才合法；纯后台 Service 拿不到剪贴板内容。
    // Entry for the launcher shortcut: no layout; read clipboard -> scan/connect ->
    // push -> finish with a Toast. Android 10+ forbids background clipboard access,
    // so the shortcut must raise this (translucent) Activity - it is foreground, which
    // is what makes reading the clipboard legal.
    private void runShortcutPush() {
        // 只检查、不弹系统授权框：NoDisplay 的 Activity 上权限弹窗显示不出来，
        // 弹了就会卡在这里。缺权限就提示用户先正常打开一次 App。
        // Check only, never raise the system permission dialog: it cannot render on a
        // NoDisplay activity and would hang here. Missing permissions -> ask the user
        // to open the app normally once.
        if (!hasBluetoothPermissions()) {
            toast("请先打开 PicoPush 一次并授予蓝牙权限");
            finish();
            return;
        }
        BluetoothManager bm = (BluetoothManager) getSystemService(Context.BLUETOOTH_SERVICE);
        adapter = (bm == null) ? null : bm.getAdapter();
        if (adapter == null) {
            toast("本机没有蓝牙硬件");
            finish();
            return;
        }
        if (!adapter.isEnabled()) {
            toast("请先开启手机蓝牙");
            finish();
            return;
        }
        // gen23：老系统位置服务未开 → 扫描必然空结果，提前提示（与主界面同因）。
        // gen23: legacy systems with location services off scan into the void - warn early.
        if (Build.VERSION.SDK_INT < 31 && !isLocationServiceOn()) {
            toast("请先打开手机定位服务（GPS）才能扫描墨水屏");
            finish();
            return;
        }
        showShortcutDialog();
    }

    // 极简确认弹窗：显示读到的字数预览，用户点「发送」才真正推送。
    // **关键**：剪贴板在用户点「发送」的那一刻再读一次——那是明确的用户手势，
    // 系统一定放行（弹窗打开时先读一次只是为了给预览，读不到也不阻塞）。
    // Minimal confirm dialog: show a preview and push only after the user taps 发送.
    // Key point: the clipboard is read AGAIN inside the tap handler - an explicit user
    // gesture the system always allows. The read at dialog-open time is only for the
    // preview and failing it must not block.
    private void showShortcutDialog() {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(20);
        box.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(this);
        title.setText("一键推流");
        title.setTextSize(18);
        title.setPadding(0, 0, 0, dp(10));
        box.addView(title, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        TextView info = new TextView(this);
        info.setTextSize(14);
        info.setPadding(0, 0, 0, dp(14));
        String preview = readClipboardText();
        if (preview == null || preview.trim().isEmpty()) {
            info.setText("剪贴板暂时读不到内容。\n点「发送」时会在你的点击下重新读取。");
        } else {
            String head = preview.length() > 40 ? preview.substring(0, 40) + "…" : preview;
            info.setText("已读取剪贴板（" + preview.length() + " 个字符）\n\n" + head);
        }
        box.addView(info, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        Button send = new Button(this);
        send.setText("发送");
        send.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                String text = readClipboardText();   // 用户手势内读取 / read inside the gesture
                if (text == null || text.trim().isEmpty()) {
                    toast("剪贴板是空的，先复制要推送的文字");
                    return;
                }
                final String body = text;
                toast("正在推送到墨水屏……");
                new Thread(new Runnable() {
                    @Override
                    public void run() {
                        try {
                            doPush(body, false);
                            toast("推送完成");
                        } catch (Throwable t) {
                            toast("推送失败：" + t.getMessage());
                        }
                        runOnUiThread(new Runnable() {
                            @Override
                            public void run() {
                                finish();   // finish 必须回 UI 线程 / finish on the UI thread
                            }
                        });
                    }
                }).start();
            }
        });
        box.addView(send, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        Button cancel = new Button(this);
        cancel.setText("取消");
        cancel.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                finish();
            }
        });
        box.addView(cancel, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        setContentView(box);
    }

    // 直接读剪贴板文本（不写入 input 控件，快捷模式没有界面）。
    // Read the clipboard directly (no EditText to fill in shortcut mode).
    private String readClipboardText() {
        try {
            ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
            if (cm == null || !cm.hasPrimaryClip()) return null;
            ClipData data = cm.getPrimaryClip();
            if (data == null || data.getItemCount() == 0) return null;
            CharSequence cs = data.getItemAt(0).getText();
            return cs == null ? null : cs.toString();
        } catch (Throwable t) {
            return null;
        }
    }

    /* ---- 发送入口 ---- */

    private void onSendClicked() {
        final String text = (input.getText() == null) ? "" : input.getText().toString();
        final boolean isWord = wordCard.isChecked();
        if (text.trim().isEmpty()) {
            toast(isWord ? "单词卡需要输入单词" : "正文为空");
            return;
        }
        if (adapter == null) {
            toast("本机没有蓝牙硬件");
            return;
        }
        if (!adapter.isEnabled()) {
            startActivityForResult(new Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE), REQ_ENABLE_BT);
            uiLog("请先打开蓝牙");
            return;
        }
        if (!ensurePermissions()) {
            return;
        }
        refreshLocationBanner(); // gen23：发送前再检测一次
        // gen23：老系统（8~11）上位置服务总开关不开，BLE 扫描会静默返回空——
        // 三星 Android 10 真机实证「一直未发现设备」。这里提前拦一下并说明原因。
        // gen23: on legacy systems (8-11) a scan silently returns nothing while the
        // location master switch is off (proven on Samsung Android 10: endless
        // "no device found"). Intercept early with an explanation.
        if (Build.VERSION.SDK_INT < 31 && !isLocationServiceOn()) {
            uiLog("位置服务（GPS）未开启：系统会禁止蓝牙扫描返回结果");
            toast("请先打开手机定位服务（GPS）");
            return;
        }
        new Thread(new Runnable() {
            @Override
            public void run() {
                // M1.1（B②）：外包一层监听守卫——推送前干净停监听、推完自动恢复。
                // doPush 本体一行不动（零侵入承诺延续）。/ M1.1 (B2): wrap with the
                // listen guard - stop listening cleanly before the push, restore it
                // after. doPush's body stays untouched (the zero-intrusion promise).
                pushWithListenGuard(text, isWord);
            }
        }).start();
    }

    /* ---- 推送主流程（worker 线程） ---- */

    private void doPush(String text, boolean wordCard) {
        try {
            resetState();
            byte[] body = wordCard ? new byte[0] : text.getBytes(UTF8);
            if (body.length > PushProto.TOTAL_MAX) {
                uiLog("正文 " + body.length + " 字节超过 512KB 上限");
                return;
            }
            String title = wordCard ? text.trim() : firstLine(text);
            int ptype = wordCard ? PushProto.TYPE_WORD : PushProto.TYPE_TEXT;

            if (!scanForDevice()) {
                uiLog(failReason == null ? "未发现设备" : failReason);
                return;
            }
            uiLog("发现 " + foundName + "（token=0x"
                    + String.format("%04X", foundToken & 0xFFFF) + "）");

            BluetoothDevice dev = adapter.getRemoteDevice(foundAddr);
            uiLog("连接中……");
            gatt = dev.connectGatt(this, false, gattCb, BluetoothDevice.TRANSPORT_LE);
            if (gatt == null) {
                uiLog("connectGatt 返回 null");
                return;
            }
            if (!waitFlag(STATE_CONNECTED_WAIT, CONNECT_TIMEOUT_MS)) {
                return;
            }
            uiLog("已连接，协商 MTU……");
            gatt.requestMtu(247);
            waitFlag(MTU_WAIT, 8000); // 失败不致命：退回默认 23，分片变小
            int chunk = PushProto.chunkForMtu(mtuValue);
            uiLog("MTU=" + mtuValue + "，每包 " + chunk + " 字节");
            // gen14：MTU 协商失败（有效 23）时 83 字节 BEGIN 要走 Prepare Write 长写。
            // 固件侧 prep 队列已加深到 16（gen14 修 rc=6），但各安卓栈对长写的处理
            // 参差（gen13 真机：长写第 5 包设备侧 ENOMEM，客户端表现还是白等），明确
            // 失败让用户重试比闷头走长写可靠。
            // gen14: with the MTU exchange failed (effective 23) the 83 B BEGIN
            // needs a Prepare Write long write. The firmware prep queue is now 16
            // deep (gen14 fixes rc=6), but Android stacks differ in long-write
            // handling (gen13 on hardware: the 5th packet hit device-side ENOMEM
            // while the client just sat there) - an explicit failure with a retry
            // hint beats a silent long write.
            if (mtuValue < 32) {
                uiLog("MTU 协商失败（" + mtuValue + "），本次无法发送，请重试");
                return;
            }

            uiLog("发现 GATT 服务……");
            gatt.discoverServices();
            if (!waitFlag(SVC_WAIT, 10000)) {
                return;
            }
            if (svcStatus != BluetoothGatt.GATT_SUCCESS) {
                uiLog("服务发现失败（status=" + svcStatus + "）");
                return;
            }
            BluetoothGattService svc = gatt.getService(UUID.fromString(PushProto.SERVICE_UUID));
            if (svc == null) {
                uiLog("设备上没有推送服务（" + PushProto.SERVICE_UUID + "）");
                return;
            }
            BluetoothGattCharacteristic cData = svc.getCharacteristic(UUID.fromString(PushProto.DATA_UUID));
            BluetoothGattCharacteristic cCtrl = svc.getCharacteristic(UUID.fromString(PushProto.CTRL_UUID));
            BluetoothGattCharacteristic cStatus = svc.getCharacteristic(UUID.fromString(PushProto.STATUS_UUID));
            if (cData == null || cCtrl == null || cStatus == null) {
                uiLog("推送特征缺失（5f11/5f12/5f13）");
                return;
            }

            uiLog("订阅设备通知（5f13）……");
            gatt.setCharacteristicNotification(cStatus, true);
            BluetoothGattDescriptor cccd = cStatus.getDescriptor(UUID.fromString(PushProto.CCCD_UUID));
            if (cccd == null) {
                uiLog("找不到 CCCD 描述符");
                return;
            }
            cccd.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
            if (!gatt.writeDescriptor(cccd) || !waitFlag(DSC_WAIT, 10000)) {
                return;
            }

            // gen22：先把真值时间带给设备——PMU 没有外部晶振、深睡靠内部 RC 走时，
            // 一天能快几分钟。独立操作码，设备未武装也能收；失败不阻断推送。
            // gen22: hand the device the true time first - the PMU has no crystal and
            // its internal RC gains minutes a day in sleep. Separate opcode, accepted
            // even when unarmed; a failure here must not block the push.
            byte[] setTime = PushProto.encodeSetTime(System.currentTimeMillis() / 1000L);
            uiLog("校准设备时钟……");
            writeChar(cCtrl, setTime);

            final int pushSid = nextSessionId();
            byte[] begin = PushProto.encodeBegin(body.length, PushProto.crc32(body),
                    pushSid, foundToken, ptype, title);
            uiLog("发送 BEGIN（" + begin.length + " 字节，标题「" + title + "」）……");
            if (!writeChar(cCtrl, begin)) {
                return;
            }
            // gen14：RESUME 进候选。设备有半成品簿记（上一轮断开前收过包）时会先答
            // RESUME 而不是 READY；gen13 只等 READY/ERR，RESUME 被当非目标帧丢弃，
            // 白等 20 秒超时。/ gen14: RESUME joins the candidates. A device with
            // partial bookkeeping answers RESUME before READY; gen13 waited for
            // READY/ERR only, dropped RESUME as a non-target frame and idled out
            // the full 20 s timeout.
            byte[] st = waitStatus(new int[] { PushProto.ST_READY, PushProto.ST_RESUME,
                    PushProto.ST_ERR }, STEP_TIMEOUT_MS);
            if (st == null) {
                return;
            }
            if (PushProto.statusOp(st) == PushProto.ST_ERR) {
                uiLog("设备拒绝：" + PushProto.errText(PushProto.statusBody(st)[0] & 0xFF));
                return;
            }
            if (PushProto.statusOp(st) == PushProto.ST_RESUME) {
                // RESUME body：offset(u32 LE) + next_seq(u16 LE)。offset 0 = 设备没有
                // 可续的字节，等价于 READY（同 sid 重发 BEGIN 的续传路径）；非 0 = 要求
                // 从中间续传，本版本客户端不做断点续发，明确告知重新完整发送。
                // RESUME body: offset(u32 LE) + next_seq(u16 LE). offset 0 means
                // nothing to resume (same as READY); non-zero asks for a mid-stream
                // resume this client version does not implement - say so.
                final long off = PushProto.resumeOffset(st);
                final long nseq = PushProto.resumeNextSeq(st);
                if (off != 0 || nseq != 0) {
                    uiLog("设备要求从 " + off + " 字节续传（seq " + nseq + "），"
                            + "请重新完整发送");
                    return;
                }
                uiLog("设备有旧会话簿记，按重发处理……");
            }
            final long sid = PushProto.statusOp(st) == PushProto.ST_READY
                    ? PushProto.readySessionId(st) : -1L;
            uiLog("设备就绪（session=" + (sid < 0 ? "?" : Long.toHexString(sid))
                    + "），正文 " + body.length + " 字节，开始传输……");

            int seq = 0;
            if (body.length == 0) {
                // 空正文（单词卡/URL）：仍发一包 seq=0 长度 0，对齐有正文路径
                if (!writeChar(cData, PushProto.encodeData(0, new byte[0]))) {
                    return;
                }
            } else {
                int off = 0;
                while (off < body.length) {
                    int n = Math.min(chunk, body.length - off);
                    byte[] piece = new byte[n];
                    System.arraycopy(body, off, piece, 0, n);
                    if (!writeChar(cData, PushProto.encodeData(seq, piece))) {
                        return;
                    }
                    off += n;
                    seq++;
                    if ((seq & 7) == 0) {
                        uiLog("已传 " + off + " / " + body.length + " 字节");
                    }
                }
            }

            uiLog("数据发送完毕（" + seq + " 包），发送 END 等待设备校验……");
            if (!writeChar(cCtrl, PushProto.encodeCtrl(PushProto.OP_END))) {
                return;
            }
            st = waitStatus(new int[] { PushProto.ST_DONE, PushProto.ST_ERR }, FINAL_TIMEOUT_MS);
            if (st == null) {
                return;
            }
            if (PushProto.statusOp(st) == PushProto.ST_ERR) {
                uiLog("设备拒绝：" + PushProto.errText(PushProto.statusBody(st)[0] & 0xFF));
                return;
            }
            uiLog("推送完成：设备已校验 CRC32 并开书 ✓");
            toast("推送完成 ✓");
        } finally {
            closeQuietly();
        }
    }

    /* ---- M1.1（B②）：推送的监听守卫 ---- */

    /**
     * 推送前后对监听的一层外包：doPush 本体一行不动。
     *
     * 背景（M1 联测反馈一）：doPush 的 resetState() 不动 listenMode/GATT，
     * finally 的 closeQuietly() 拆了监听连接但 UI 仍显示「监听中」——假状态；
     * 且监听会话占着 GATT，推送要么连不上、要么把监听链路拆坏后不恢复。
     *
     * 本守卫：推送前若在监听，先 stopListen() 干净停掉（清 listenMode + 断 GATT，
     * 按钮回「连接并监听点词」）；推完自动恢复——重走 startListen() 同路径
     * （扫描 + 连接 + 订阅）。恢复失败由 startListen 内部如实 uiLog（如
     * 「未发现设备」），listenMode 保持 false、按钮可手动重试——**不无限重试**。
     * Background (M1 live-test finding 1): doPush's resetState() leaves
     * listenMode/GATT alone, so the finally closeQuietly() tears down the
     * listening link while the UI still shows "listening" - a phantom state;
     * worse, the listening session occupies the GATT client, so the push either
     * cannot connect or wrecks the listen link without restoring it. This guard
     * stops listening cleanly before the push (clears listenMode, drops GATT,
     * the button returns to "connect and listen") and restores it after by
     * re-running startListen()'s own path (scan + connect + subscribe). A
     * failed restore is logged honestly by startListen itself, listenMode stays
     * false and the button offers a manual retry - **never an endless retry**.
     */
    private void pushWithListenGuard(String text, boolean wordCard) {
        final boolean wasListening = listenMode;
        if (wasListening) {
            uiLog("推送前先停监听（推完自动恢复）……");
            stopListen();
        }
        try {
            doPush(text, wordCard);
        } finally {
            if (wasListening) {
                uiLog("推送结束，正在恢复监听……");
                startListen();
            }
        }
    }

    /* ---- 步 5：更新检测（22:11 裁决 §四 最小集） ---- */

    /**
     * 「检查更新」按钮入口：手动触发。网络在工作线程，结果与弹窗回主线程。
     * 仅检测＋提示＋跳转引导；不下载、不静默安装、不写入本地文件。
     * Step5 manual update check (verdict section 4): detect + notify + guide only.
     */
    private void checkUpdate() {
        uiLog("正在检查更新（版本源：" + UpdateChecker.FEED_URL + "）……");
        new Thread(new Runnable() {
            @Override
            public void run() {
                boolean fetchOk = false;
                String body = null;
                java.net.HttpURLConnection conn = null;
                try {
                    java.net.URL url = new java.net.URL(UpdateChecker.FEED_URL);
                    conn = (java.net.HttpURLConnection) url.openConnection();
                    conn.setRequestMethod("GET");
                    conn.setConnectTimeout(8000);
                    conn.setReadTimeout(8000);
                    conn.setInstanceFollowRedirects(true);
                    int code = conn.getResponseCode();
                    if (code >= 200 && code < 300) {
                        body = readLimited(conn.getInputStream());
                        fetchOk = body != null && !body.trim().isEmpty();
                    }
                } catch (Exception e) {
                    fetchOk = false; // 网络不可用/超时：明确走「检查失败」，绝不误报最新
                } finally {
                    if (conn != null) {
                        conn.disconnect();
                    }
                }
                final int verdict = UpdateChecker.decide(body, fetchOk,
                        UpdateChecker.CURRENT_VERSION_CODE);
                final String latestName = (verdict == UpdateChecker.UPDATE_AVAILABLE)
                        ? UpdateChecker.extractStringField(body, "versionName") : null;
                final String jumpUrl = (verdict == UpdateChecker.UPDATE_AVAILABLE)
                        ? UpdateChecker.extractStringField(body, "url") : null;
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        showUpdateResult(verdict, latestName, jumpUrl);
                    }
                });
            }
        }).start();
    }

    /** 读上限 64KB，防异常响应占满内存；零第三方依赖。 */
    private static String readLimited(InputStream in) throws java.io.IOException {
        ByteArrayOutputStream bos = new ByteArrayOutputStream();
        byte[] buf = new byte[2048];
        int n, total = 0;
        while ((n = in.read(buf)) > 0) {
            total += n;
            if (total > 64 * 1024) {
                break;
            }
            bos.write(buf, 0, n);
        }
        return new String(bos.toByteArray(), UTF8);
    }

    /**
     * 三态结果上屏：卡片非阻塞、仅引导不装。失败态明确「检查失败」，
     * 绝不显示「已是最新」——裁决 §四 失败口径，防「伪闭环按钮」。
     */
    private void showUpdateResult(int verdict, String latestName, String jumpUrl) {
        if (verdict == UpdateChecker.CHECK_FAILED) {
            uiLog("检查失败：版本源不可达或返回数据异常（未判定为最新）");
            new AlertDialog.Builder(MainActivity.this)
                    .setTitle("检查失败")
                    .setMessage("无法获取版本信息：网络不可用或版本源暂不可达。\n"
                            + "本次未判定是否为最新版本，请稍后重试。\n\n"
                            + "当前版本 v" + UpdateChecker.CURRENT_VERSION_NAME
                            + "（versionCode " + UpdateChecker.CURRENT_VERSION_CODE + "）")
                    .setPositiveButton("知道了", null)
                    .show();
            return;
        }
        if (verdict == UpdateChecker.UP_TO_DATE) {
            uiLog("已是最新版本 v" + UpdateChecker.CURRENT_VERSION_NAME);
            new AlertDialog.Builder(MainActivity.this)
                    .setTitle("已是最新版本")
                    .setMessage("当前版本 v" + UpdateChecker.CURRENT_VERSION_NAME
                            + "（versionCode " + UpdateChecker.CURRENT_VERSION_CODE
                            + "）已是最新。")
                    .setPositiveButton("知道了", null)
                    .show();
            return;
        }
        // UPDATE_AVAILABLE：仅提示版本号/当前号 + 跳转引导，不下载、不安装
        final String target = (jumpUrl != null && jumpUrl.startsWith("http"))
                ? jumpUrl : "https://github.com/cere-hy/pico-plus/releases";
        uiLog("发现新版本 v" + (latestName == null ? "?" : latestName)
                + "（当前 v" + UpdateChecker.CURRENT_VERSION_NAME + "）");
        new AlertDialog.Builder(MainActivity.this)
                .setTitle("发现新版本")
                .setMessage("新版本：v" + (latestName == null ? "（见下载页）" : latestName)
                        + "\n当前版本：v" + UpdateChecker.CURRENT_VERSION_NAME
                        + "（versionCode " + UpdateChecker.CURRENT_VERSION_CODE + "）\n\n"
                        + "本应用不下载、不自动安装，点击下方按钮在浏览器打开下载页。")
                .setPositiveButton("前往下载页", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        try {
                            startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(target)));
                        } catch (Exception e) {
                            uiLog("打开下载页失败：" + e.getMessage());
                        }
                    }
                })
                .setNegativeButton("稍后", null)
                .show();
    }

    /* ---- 等待原语（worker 线程） ---- */

    private static final int STATE_CONNECTED_WAIT = 1;
    private static final int MTU_WAIT = 2;
    private static final int SVC_WAIT = 3;
    private static final int DSC_WAIT = 4;
    private static final int WRITE_WAIT = 5;

    /** 等一个回调条件；超时或 failReason 时记录原因并返回 false。 */
    private boolean waitFlag(int what, long timeoutMs) {
        long deadline = System.currentTimeMillis() + timeoutMs;
        synchronized (lock) {
            while (true) {
                if (failReason != null) {
                    uiLog(failReason);
                    return false;
                }
                if (what == STATE_CONNECTED_WAIT && connState == BluetoothGatt.STATE_CONNECTED) {
                    return true;
                }
                if (what == MTU_WAIT && mtuDone) {
                    return true;
                }
                if (what == SVC_WAIT && svcDone) {
                    return true;
                }
                if (what == DSC_WAIT && dscDone) {
                    return true;
                }
                if (what == WRITE_WAIT && writeStatus != -1) {
                    return true;
                }
                long rem = deadline - System.currentTimeMillis();
                if (rem <= 0) {
                    uiLog("等待设备回应超时（" + whatName(what) + "）");
                    return false;
                }
                try {
                    lock.wait(Math.min(rem, 250));
                } catch (InterruptedException e) {
                    return false;
                }
            }
        }
    }

    private static String whatName(int what) {
        switch (what) {
            case STATE_CONNECTED_WAIT: return "连接";
            case MTU_WAIT: return "MTU 协商";
            case SVC_WAIT: return "服务发现";
            case DSC_WAIT: return "描述符写入";
            case WRITE_WAIT: return "特征写入";
            default: return "步骤 " + what;
        }
    }

    /** 等一条 opcode 属于 wanted 的 STATUS 通知；其他 opcode（PROGRESS/BATT 等）丢弃继续等。 */
    private byte[] waitStatus(int[] wanted, long timeoutMs) {
        long deadline = System.currentTimeMillis() + timeoutMs;
        synchronized (lock) {
            while (true) {
                if (failReason != null) {
                    uiLog(failReason);
                    return null;
                }
                byte[] f = lastStatus;
                if (f != null) {
                    lastStatus = null;
                    int op = PushProto.statusOp(f);
                    for (int i = 0; i < wanted.length; i++) {
                        if (op == wanted[i]) {
                            return f;
                        }
                    }
                    continue; // 非目标帧：丢弃，继续等
                }
                long rem = deadline - System.currentTimeMillis();
                if (rem <= 0) {
                    uiLog("等待设备 STATUS 超时");
                    return null;
                }
                try {
                    lock.wait(Math.min(rem, 250));
                } catch (InterruptedException e) {
                    return null;
                }
            }
        }
    }

    /** WWR 写（WRITE_TYPE_DEFAULT）：逐包等 ATT 回执。 */
    private boolean writeChar(BluetoothGattCharacteristic c, byte[] value) {
        synchronized (lock) {
            writeStatus = -1;
        }
        c.setValue(value);
        c.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
        if (!gatt.writeCharacteristic(c)) {
            uiLog("写入调用失败");
            return false;
        }
        if (!waitFlag(WRITE_WAIT, STEP_TIMEOUT_MS)) {
            return false;
        }
        int ws;
        synchronized (lock) {
            ws = writeStatus;
            writeStatus = -1;
        }
        if (ws != BluetoothGatt.GATT_SUCCESS) {
            uiLog("设备未接受写入（GATT status=" + ws + "）");
            return false;
        }
        return true;
    }

    /* ---- 扫描 ---- */

    // gen22 防串台：只有一台才自动连；多台时必须让用户按墨水屏上显示的代号点选。
    // 名字本身就是 MAC 后四位（PicoPush-1A2B），屏幕与列表一一对应，不会认错。
    // gen22 anti-crosstalk: auto-connect only when there is exactly one panel; with
    // several, the user must pick by the code shown on the e-ink screen. The name is
    // the MAC's last four hex digits (PicoPush-1A2B), so screen and list match exactly.
    private boolean chooseDevice(final List<Found> list) {
        if (list.isEmpty()) return false;
        if (list.size() == 1) {
            Found only = list.get(0);
            synchronized (lock) {
                foundAddr = only.addr;
                foundToken = only.token;
                foundName = only.name;
            }
            return true;
        }
        final String[] labels = new String[list.size()];
        for (int i = 0; i < list.size(); i++) {
            Found f = list.get(i);
            labels[i] = f.name + "   [" + f.addr + "]";
        }
        final int[] pickedIndex = new int[] { -1 };
        final CountDownLatch picked = new CountDownLatch(1);
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                new AlertDialog.Builder(MainActivity.this)
                        .setTitle("选择要推送的墨水屏（代号见设备屏幕）")
                        .setItems(labels, new DialogInterface.OnClickListener() {
                            @Override
                            public void onClick(DialogInterface dialog, int which) {
                                pickedIndex[0] = which;
                                picked.countDown();
                            }
                        })
                        .setOnCancelListener(new DialogInterface.OnCancelListener() {
                            @Override
                            public void onCancel(DialogInterface dialog) {
                                picked.countDown();
                            }
                        })
                        .show();
            }
        });
        try {
            picked.await(90, TimeUnit.SECONDS);
        } catch (InterruptedException e) {
            /* 用户不等了 / give up waiting */
        }
        int index = pickedIndex[0];
        if (index < 0 || index >= list.size()) {
            failReason = "未选择设备（发现 " + list.size() + " 台，请选一台）";
            return false;
        }
        Found chosen = list.get(index);
        synchronized (lock) {
            foundAddr = chosen.addr;
            foundToken = chosen.token;
            foundName = chosen.name;
        }
        return true;
    }

    private boolean scanForDevice() {
        BluetoothLeScanner scanner = adapter.getBluetoothLeScanner();
        if (scanner == null) {
            failReason = "拿不到 BLE 扫描器（蓝牙未开？）";
            return false;
        }
        ScanSettings settings = new ScanSettings.Builder()
                .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build();
        // 上一轮扫描停净是异步的：紧接着再 startScan 会收
        // SCAN_FAILED_ALREADY_STARTED(1)。真机日志见过连续两发 code=1——
        // 不是权限问题，是没停净。最多重试 3 轮，每轮先停再等 600ms。
        // The previous scan stops asynchronously: an immediate restart collects
        // SCAN_FAILED_ALREADY_STARTED (1). Real logs showed two code=1s back to
        // back - not a permission problem, just an unstopped scan. Up to three
        // attempts, each stopping first and waiting 600 ms.
        for (int attempt = 1; attempt <= 3; attempt++) {
            uiLog(attempt == 1
                    ? "扫描 PicoPush-XXXX 广播（最多 12 秒）……"
                    : "重试扫描（第 " + attempt + " 次，先停上一轮）……");
            synchronized (lock) {
                scanDone = false;
                foundAddr = null;
                failReason = null;
                scanFailedCode = -1;
                candidates.clear();      // gen22：每轮重新收集 / recollect each round
                firstFoundMs = 0;
            }
            scanner.stopScan(scanCb); // 幂等：没在扫也是安全调用 / idempotent
            sleepQuiet(200);
            scanner.startScan(null, settings, scanCb);
            long deadline = System.currentTimeMillis() + SCAN_TIMEOUT_MS;
            synchronized (lock) {
                while (!scanDone && failReason == null) {
                    // gen22：扫到第一台之后再多收 2.5 秒，看旁边还有没有别的墨水屏；
                    // 只有一台时几乎立刻进入连接（保持原有手感）。
                    // gen22: after the first hit keep listening 2.5 s for neighbours;
                    // with a single panel we still proceed almost immediately.
                    long effective = deadline;
                    if (firstFoundMs > 0) {
                        effective = Math.min(deadline, firstFoundMs + MULTI_SCAN_GRACE_MS);
                    }
                    long rem = effective - System.currentTimeMillis();
                    if (rem <= 0) {
                        break;
                    }
                    try {
                        lock.wait(Math.min(rem, 250));
                    } catch (InterruptedException e) {
                        break;
                    }
                }
            }
            scanner.stopScan(scanCb);
            List<Found> snapshot;
            synchronized (lock) {
                snapshot = new ArrayList<Found>(candidates);
            }
            if (!snapshot.isEmpty()) {
                return chooseDevice(snapshot);
            }
            if (failReason != null) {
                return false; // 硬失败（非 code=1），onScanFailed 已写明原因
            }
            if (scanFailedCode == 1 && attempt < 3) {
                continue; // 可恢复：停一次等一会儿再来 / recoverable: stop, wait, retry
            }
            if (scanFailedCode == 1) {
                failReason = "扫描反复启动失败（code=1），请退出应用重进";
                return false;
            }
            failReason = "12 秒内未发现 PicoPush-XXXX 广播（请先在墨水屏开启「接收推送」）";
            return false;
        }
        failReason = "扫描反复启动失败，请退出应用重进";
        return false;
    }

    private static void sleepQuiet(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    /* ---- 权限 ---- */

    // gen23：位置服务总开关。/ gen23: location services master switch.
    // Android 8~11 的 BLE 扫描不仅需要 ACCESS_FINE_LOCATION，还需要位置服务开着，
    // 否则 onScanResult 一个都不回调（三星 Android 10 真机实证）。12+ 起
    // BLUETOOTH_SCAN 带 neverForLocation，不再要求位置服务。
    // On Android 8-11 a BLE scan needs ACCESS_FINE_LOCATION AND location services
    // enabled, otherwise onScanResult never fires (proven on Samsung Android 10).
    // On 12+ BLUETOOTH_SCAN carries neverForLocation and location is not required.
    private boolean isLocationServiceOn() {
        try {
            LocationManager lm = (LocationManager) getSystemService(Context.LOCATION_SERVICE);
            if (lm == null) {
                return false;
            }
            if (Build.VERSION.SDK_INT >= 28) {
                return lm.isLocationEnabled();
            }
            return lm.isProviderEnabled(LocationManager.GPS_PROVIDER)
                    || lm.isProviderEnabled(LocationManager.NETWORK_PROVIDER);
        } catch (Throwable t) {
            return false;
        }
    }

    // gen23：刷新顶部横幅。红 = 蓝牙/权限/位置服务有缺失（附具体缺项）；
    // 绿 = 定位服务已就绪。事件触发（进入 App / onResume / 发送前），不轮询。
    // gen23: refresh the top banner. Red = something is missing (with details);
    // green = ready. Event-driven (app entry / onResume / before send), never polled.
    private void refreshLocationBanner() {
        if (locBanner == null) {
            return;
        }
        boolean btOk = adapter != null && adapter.isEnabled();
        boolean permOk;
        boolean svcOk;
        if (Build.VERSION.SDK_INT >= 31) {
            permOk = hasBluetoothPermissions();
            svcOk = true; // neverForLocation：12+ 不要求位置服务
        } else {
            permOk = checkSelfPermission("android.permission.ACCESS_FINE_LOCATION")
                    == PackageManager.PERMISSION_GRANTED;
            svcOk = isLocationServiceOn();
        }
        if (btOk && permOk && svcOk) {
            locBanner.setText("✅ 定位服务已就绪");
            locBanner.setTextColor(0xFF166B3A);
            locBanner.setBackgroundColor(0xFFE8F5EC);
        } else {
            StringBuilder why = new StringBuilder();
            if (!btOk) {
                why.append("蓝牙未开 ");
            }
            if (!permOk) {
                why.append("定位权限未授予 ");
            }
            if (!svcOk) {
                why.append("位置服务(GPS)未开 ");
            }
            locBanner.setText("⚠️ 提示：安卓系统需要开启手机定位服务（GPS）及授予定位权限，"
                    + "才能正常扫描墨水屏。（当前：" + why.toString().trim() + "）");
            locBanner.setTextColor(0xFF9A1B1B);
            locBanner.setBackgroundColor(0xFFFDE8E8);
        }
        locBanner.setVisibility(View.VISIBLE);
    }

    private boolean ensurePermissions() {
        List<String> need = new ArrayList<String>();
        if (Build.VERSION.SDK_INT >= 31) {
            addIfMissing(need, "android.permission.BLUETOOTH_SCAN");
            addIfMissing(need, "android.permission.BLUETOOTH_CONNECT");
        } else {
            addIfMissing(need, "android.permission.ACCESS_FINE_LOCATION");
        }
        if (need.isEmpty()) {
            return true;
        }
        requestPermissions(need.toArray(new String[need.size()]), REQ_PERMS);
        uiLog("请先在系统弹窗中授予蓝牙权限，然后再点一次发送");
        return false;
    }

    // 快捷方式用：只判断权限是否已有，不触发请求。/ Shortcut path: report only, never request.
    private boolean hasBluetoothPermissions() {
        if (Build.VERSION.SDK_INT >= 31) {
            return checkSelfPermission("android.permission.BLUETOOTH_SCAN") == PackageManager.PERMISSION_GRANTED
                    && checkSelfPermission("android.permission.BLUETOOTH_CONNECT") == PackageManager.PERMISSION_GRANTED;
        }
        return checkSelfPermission("android.permission.ACCESS_FINE_LOCATION") == PackageManager.PERMISSION_GRANTED;
    }

    private void addIfMissing(List<String> need, String perm) {
        if (checkSelfPermission(perm) != PackageManager.PERMISSION_GRANTED) {
            need.add(perm);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        if (requestCode == REQ_NOTIF) {
            // M1.3：通知权限拒绝不阻塞保活——FGS 照常运行，仅通知栏不可见。
            // M1.3: a denied notification permission never blocks the keep-alive.
            boolean granted = grantResults.length > 0
                    && grantResults[0] == PackageManager.PERMISSION_GRANTED;
            if (!granted) {
                uiLog("通知权限被拒：保活照常运行，仅通知栏不可见（看状态行即可）");
            }
            return;
        }
        if (requestCode != REQ_PERMS) {
            return;
        }
        boolean all = grantResults.length > 0;
        for (int i = 0; i < grantResults.length; i++) {
            if (grantResults[i] != PackageManager.PERMISSION_GRANTED) {
                all = false;
            }
        }
        uiLog(all ? "权限已授予，请再次点击发送" : "权限被拒绝，无法推送（可在系统设置里手动授予）");
    }

    /* ---- 小工具 ---- */

    private void resetState() {
        synchronized (lock) {
            connState = -1;
            mtuValue = 23;
            mtuDone = false;
            svcDone = false;
            svcStatus = -1;
            writeStatus = -1;
            dscDone = false;
            lastStatus = null;
            scanDone = false;
            failReason = null;
        }
        foundAddr = null;
        foundToken = -1;
        foundName = null;
    }

    private void closeQuietly() {
        BluetoothGatt g = gatt;
        gatt = null;
        if (g != null) {
            try {
                g.disconnect();
            } catch (Exception ignored) {
            }
            try {
                g.close();
            } catch (Exception ignored) {
            }
        }
    }

    /* ================= gen26 M1：AI 查词应答链 =================
     * 设备点词 -> STATUS 0x17 -> 本 APK 调 LLM -> CTRL 0x07 回传 -> 设备出释义卡。
     * 与推送流程完全并列：doPush 一行未改，不开监听即与老版本行为一致。
     * key/prompt/模型全在 APK 侧（可热改），设备零改动。
     */

    /** 读本机保存的 AI 配置（key 只存 SharedPreferences，绝不进仓库）。 */
    private AiClient.Config aiConfig() {
        AiClient.Config cfg = new AiClient.Config();
        android.content.SharedPreferences sp = getSharedPreferences(PREF, MODE_PRIVATE);
        cfg.apiKey = sp.getString(PREF_KEY, "");
        String base = sp.getString(PREF_BASE, "");
        cfg.baseUrl = base.isEmpty() ? AiClient.DEFAULT_BASE_URL : base;
        String model = sp.getString(PREF_MODEL, "");
        cfg.model = model.isEmpty() ? AiClient.DEFAULT_MODEL : model;
        // M1.2 ①：激活模板注入 user 角色；系统指令（230 字硬约束，gen26.6.1 放宽）
        // 仍由 AiClient 恒定钉在 system 角色，模板永远碰不到。
        // M1.2 item-1: inject the active template into the user role; the system
        // instruction (230-char hard constraint, relaxed in gen26.6.1) stays pinned in the system role by
        // AiClient and is never reachable from the template.
        cfg.promptTemplate = getActiveTemplateContent();
        return cfg;
    }

    private void saveAiConfig(String key, String base, String model) {
        getSharedPreferences(PREF, MODE_PRIVATE).edit()
                .putString(PREF_KEY, key)
                .putString(PREF_BASE, base)
                .putString(PREF_MODEL, model)
                .apply();
    }

    /** 刷新 AI 状态行与监听按钮文案（可在任意线程调用）。 */
    private void refreshAiStatus() {
        if (aiStatus == null) {
            return;
        }
        AiClient.Config cfg = aiConfig();
        StringBuilder sb = new StringBuilder("AI 释义：");
        sb.append(cfg.apiKey.isEmpty() ? "未配置 key" : "key 已配置");
        sb.append(" · ").append(cfg.model);
        if (listenMode) {
            sb.append(" · 监听中");
            if (devCaps >= 0) {
                sb.append(((devCaps & PushProto.CAP_AI_QUERY) != 0) ? " · 设备支持" : " · 设备不支持");
            }
            // M1.3：保活档位 = FGS 运行中 × 电池白名单，如实组合（单一真相源
            // AiKeepAlive.keepAliveState，宿测 20 断言覆盖四态）。
            // M1.3: keep-alive tier = FGS running AND battery whitelisted,
            // computed by the single source of truth AiKeepAlive.keepAliveState.
            AiKeepAlive.KeepAlive ka = AiKeepAlive.keepAliveState(
                    mFgsRunning, batteryWhitelisted());
            sb.append(ka == AiKeepAlive.KeepAlive.FULL ? " · 保活：完整" : " · 保活：受限");
        } else {
            sb.append(" · 未监听");
        }
        final String text = sb.toString();
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                aiStatus.setText(text);
                if (btnListen != null) {
                    btnListen.setText(listenMode ? "停止监听" : "连接并监听点词");
                }
            }
        });
    }

    /** AI 设置对话框：key / base_url / model 三项 + M1.2 ① 提示模板编辑。 */
    private void showAiSettings() {
        AiClient.Config cfg = aiConfig();
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        int p = dp(16);
        box.setPadding(p, p, p, p);

        final EditText edKey = new EditText(this);
        edKey.setHint("API key（只存本机，不进代码仓库）");
        edKey.setText(cfg.apiKey);
        edKey.setSingleLine(true);
        box.addView(edKey);

        final EditText edBase = new EditText(this);
        edBase.setHint("base_url（默认 " + AiClient.DEFAULT_BASE_URL + "）");
        edBase.setText(cfg.baseUrl);
        edBase.setSingleLine(true);
        box.addView(edBase);

        final EditText edModel = new EditText(this);
        edModel.setHint("model（默认 " + AiClient.DEFAULT_MODEL + "）");
        edModel.setText(cfg.model);
        edModel.setSingleLine(true);
        box.addView(edModel);

        // ---- M1.2 ① 提示模板：多行编辑，{word} 占位符，恢复默认 ----
        // M1.2 item-1 prompt template: multi-line, {word} placeholder, reset-to-default.
        TextView tplHint = new TextView(this);
        // gen26.6.4：文案从预算常量派生，不再写死数字（180→230→360 已漂三次）。
        // 单一来源 AiClient.SYSTEM_CHAR_LIMIT，改预算即改文案，防第四次漂移。
        tplHint.setText("提示模板（可用 {word} 占位符，系统 " + AiClient.SYSTEM_CHAR_LIMIT
                + " 字约束始终生效且不可被覆盖）");
        tplHint.setTextSize(12);
        tplHint.setPadding(0, dp(8), 0, dp(4));
        box.addView(tplHint);

        final EditText edTpl = new EditText(this);
        edTpl.setHint("word: {word}");
        edTpl.setText(getActiveTemplateContent());
        edTpl.setSingleLine(false);
        edTpl.setMaxLines(4);
        edTpl.setGravity(android.view.Gravity.TOP);
        box.addView(edTpl);
        // M1.2.1：登记为激活模板编辑框，供模板库操作后回填对齐。
        // M1.2.1: register as the active-template editor for post-op refresh.
        mActiveTplEditor = edTpl;

        Button btnTplLib = new Button(this);
        btnTplLib.setText("管理模板库（切换 / 新建 / 导出 / 导入 / 分享）");
        btnTplLib.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                // 先把当前编辑暂存进库，再打开管理页，避免来回丢改。
                // Stash the in-progress edit into the library first, then open the
                // manager, so we don't lose the edit when bouncing between dialogs.
                setActiveTemplateContent(edTpl.getText().toString());
                showTemplateManager();
            }
        });
        box.addView(btnTplLib);

        // ---- M1.3：电池白名单引导（息屏保活第二环）----
        // 只引导、不代决：受限时给一键入口，去系统页由用户亲手放行；白名单
        // 动作拿不到返回值，onResume 的 refreshAiStatus 会把新状态如实刷出来。
        // M1.3 battery-whitelist guide: a one-tap entry when restricted; the
        // user grants it in the system page (no return value), and onResume's
        // refreshAiStatus surfaces the new state honestly.
        final AiKeepAlive.KeepAlive kaNow = AiKeepAlive.keepAliveState(
                mFgsRunning, batteryWhitelisted());
        final TextView kaHint = new TextView(this);
        kaHint.setTextSize(12);
        kaHint.setPadding(0, dp(8), 0, dp(4));
        kaHint.setText(kaNow == AiKeepAlive.KeepAlive.FULL
                ? "保活：完整（前台服务 + 电池白名单均已就位，息屏查词不断网）"
                : "保活：受限（息屏久置后后台网络可能被系统挂起，建议补全白名单）");
        box.addView(kaHint);

        if (kaNow != AiKeepAlive.KeepAlive.FULL && !batteryWhitelisted()) {
            Button btnWhitelist = new Button(this);
            btnWhitelist.setText("加入电池白名单（系统页确认）");
            btnWhitelist.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    requestBatteryWhitelist();
                }
            });
            box.addView(btnWhitelist);
        }

        new android.app.AlertDialog.Builder(this)
                .setTitle("AI 释义设置（默认 DeepSeek）")
                .setView(box)
                .setOnDismissListener(new android.content.DialogInterface.OnDismissListener() {
                    @Override
                    public void onDismiss(android.content.DialogInterface d) {
                        // 对话框关：释放编辑框引用，避免模板库操作写进已销毁的视图。
                        // Dialog gone: drop the editor handle so later ops don't
                        // touch a dead view.
                        if (mActiveTplEditor == edTpl) {
                            mActiveTplEditor = null;
                        }
                    }
                })
                .setNegativeButton("取消", null)
                .setNeutralButton("恢复默认", new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        // M1.2.1：恢复默认必须**立即落库**——旧版只改编辑框文本、对话框
                        // 随即关闭，不点「保存」等于没生效（真机实测 bug）。写入存储后
                        // 回填编辑框，UI 与存储即刻一致。
                        // M1.2.1: reset-to-default MUST persist immediately - the old
                        // version only changed the box text, then the dialog closed, so
                        // without tapping 保存 it never took effect (real-device bug).
                        // Persist, then refresh the editor so UI == storage at once.
                        setActiveTemplateContent(AiClient.DEFAULT_TEMPLATE);
                        refreshActiveTemplateEditor();
                        refreshAiStatus();
                        uiLog("提示模板已恢复默认（word: {word}）");
                    }
                })
                .setPositiveButton("保存", new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        saveAiConfig(edKey.getText().toString().trim(),
                                edBase.getText().toString().trim(),
                                edModel.getText().toString().trim());
                        // 把编辑后的模板写回当前激活的命名模板。
                        // Persist the edited template back into the active named template.
                        setActiveTemplateContent(edTpl.getText().toString());
                        uiLog("AI 设置已保存到本机（key 不进仓库）");
                        refreshAiStatus();
                    }
                })
                .show();
    }

    /**
     * M1.3：引导式电池白名单请求——跳系统「忽略电池优化」确认页，由用户亲手放行。
     * 该动作无返回值（用户可能拒绝），onResume 的 refreshAiStatus 会把结果如实
     * 刷到状态行；个别 ROM 没有此 action 时退到应用详情页，再不行就文字指引。
     * Guided battery-whitelist request: the system confirm page, granted by the
     * user's own hand. No return value (may be declined); onResume refreshes
     * the status row honestly. Fallback chain: details page -> text hint.
     */
    private void requestBatteryWhitelist() {
        try {
            Intent i = new Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS);
            i.setData(Uri.parse("package:" + getPackageName()));
            startActivity(i);
            uiLog("已打开电池优化确认页（请选「允许」；回来状态行会自动刷新）");
        } catch (Throwable t) {
            try {
                startActivity(new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                        Uri.parse("package:" + getPackageName())));
                uiLog("已打开应用详情页（请进电池 → 选「无限制」）");
            } catch (Throwable t2) {
                toast("请手动设置：系统设置 → 电池 → 本应用 → 无限制");
            }
        }
    }

    /** 扫描 + 连接 + 订阅，然后**保持连接**等设备的点词通知。 */
    private void startListen() {
        if (aiConfig().apiKey.trim().isEmpty()) {
            uiLog("请先点「AI 设置」填写 API key");
            return;
        }
        // M1.3：Android 13+ 通知权限引导——拒绝不影响 FGS 运行，仅通知不可见
        // （状态行「保活：完整/受限」才是可见反馈）。调用点在按钮路径，合法。
        // M1.3: guide the POST_NOTIFICATIONS grant on Android 13+; a denial
        // never stops the FGS - the notification just stays invisible and the
        // status row remains the visible feedback. Called from a button, legal.
        if (Build.VERSION.SDK_INT >= 33
                && checkSelfPermission("android.permission.POST_NOTIFICATIONS")
                != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{"android.permission.POST_NOTIFICATIONS"},
                    REQ_NOTIF);
        }
        // M1.3：开监听即启前台服务（connectedDevice 类型）——息屏后台网络保活主修。
        // 调用点在按钮点击路径（前台），不受后台启动 FGS 限制；激进 ROM 拒启时
        // 服务内部降级不崩，状态行如实显示「保活：受限」。
        // M1.3: bring up the keep-alive FGS the moment listening starts (the call
        // site is a foreground button click, so background-FGS limits do not
        // apply); a refusing ROM degrades inside the service, status row honest.
        mFgsRunning = true;
        ListenKeepAliveService.start(this);
        new Thread(new Runnable() {
            @Override
            public void run() {
                // M1.3：链路没建成就不虚挂保活——停 FGS，状态行回落「未监听」。
                // M1.3: a failed link must not keep a phantom FGS - stop it and
                // let the status row fall back to "not listening".
                if (!listenThreadBody()) {
                    mFgsRunning = false;
                    ListenKeepAliveService.stop(MainActivity.this);
                }
            }
        }).start();
    }

    /**
     * startListen 的 worker 线程体（M1.3 抽出）：扫描 → 连接 → MTU → 服务发现 →
     * 订阅 → 能力声明。true = 链路建成进入常驻监听；false = 任一步失败（原因已
     * uiLog），调用方负责停 FGS。
     */
    private boolean listenThreadBody() {
        resetState();
        uiLog("扫描设备……");
        if (!scanForDevice()) {
            uiLog(failReason == null ? "未发现设备" : failReason);
            return false;
        }
        uiLog("发现 " + foundName + "，连接中……");
        BluetoothDevice dev = adapter.getRemoteDevice(foundAddr);
        gatt = dev.connectGatt(MainActivity.this, false, gattCb,
                BluetoothDevice.TRANSPORT_LE);
        if (gatt == null) {
            uiLog("connectGatt 返回 null");
            return false;
        }
        if (!waitFlag(STATE_CONNECTED_WAIT, CONNECT_TIMEOUT_MS)) {
            return false;
        }
        gatt.requestMtu(247);
        waitFlag(MTU_WAIT, 8000); // 失败不致命 / non-fatal
        gatt.discoverServices();
        if (!waitFlag(SVC_WAIT, 10000)) {
            return false;
        }
        if (svcStatus != BluetoothGatt.GATT_SUCCESS) {
            uiLog("服务发现失败（status=" + svcStatus + "）");
            return false;
        }
        BluetoothGattService svc = gatt.getService(UUID.fromString(PushProto.SERVICE_UUID));
        if (svc == null) {
            uiLog("设备上没有推送服务");
            return false;
        }
        BluetoothGattCharacteristic cStatus =
                svc.getCharacteristic(UUID.fromString(PushProto.STATUS_UUID));
        BluetoothGattCharacteristic cCtrl =
                svc.getCharacteristic(UUID.fromString(PushProto.CTRL_UUID));
        if (cStatus == null || cCtrl == null) {
            uiLog("推送特征缺失（5f12/5f13）");
            return false;
        }
        cCtrlAi = cCtrl;
        gatt.setCharacteristicNotification(cStatus, true);
        BluetoothGattDescriptor cccd =
                cStatus.getDescriptor(UUID.fromString(PushProto.CCCD_UUID));
        if (cccd == null) {
            uiLog("找不到 CCCD 描述符");
            return false;
        }
        cccd.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
        if (!gatt.writeDescriptor(cccd) || !waitFlag(DSC_WAIT, 10000)) {
            uiLog("订阅失败");
            return false;
        }
        // 步5缺陷2：校时与「发送文本」解耦——纯监听/待机链路一旦订阅建成，
        // 立即主动下发一次标准时间（0x05 SET_TIME），不再依赖用户发文本。
        // 设备侧 0x05 是独立操作码、未武装也能收，由 time_service_poll 消费
        // 并同步 PMU/漂移账本；写失败不阻断监听（与 pushThreadBody 口径一致）。
        // Step5 D2: decouple time sync from text sending. As soon as the
        // listen-only subscription is up, push one SET_TIME frame; no text needed.
        // Firmware accepts 0x05 even when unarmed; a write failure is non-fatal.
        byte[] listenSetTime = PushProto.encodeSetTime(
                System.currentTimeMillis() / 1000L);
        uiLog("校准设备时钟（连接即校时）……");
        writeChar(cCtrl, listenSetTime);
        // gen26.5 能力使能（0x09）：订阅建立的同一时刻声明「本 APK 支持
        // 74B 上下文档」。设备默认发 66B legacy，收到本帧并置起
        // CAP_AI_CONTEXT 后才改发 74B；老固件不认 0x09 → 回 ERR(2)，
        // 双方行为都不变（本 APK 的旧解码路径照读 66B）。写失败也不致命。
        // gen26.5 capability enable (0x09): declare support for the 74B
        // contextual frame at the same moment the subscription lands. The
        // device emits 66B legacy by default and only upgrades after this
        // frame sets CAP_AI_CONTEXT; old firmware rejects 0x09 with ERR(2)
        // and both sides carry on (our legacy decode path still reads 66B).
        // M4-A：补声明 CAP_AI_MULTI（0x08 释义分帧）。注意它与「双指手势」无关——
        // 双指是纯设备侧闭环，不需要 APK 声明任何能力位；这里只是让 v1.6.x 的
        // 声明与它的实际能力对齐（设备侧目前只记录该位、不消费它，行为零变化）。
        // M4-A: also declare CAP_AI_MULTI (0x08 segmented definitions). Note it has
        // nothing to do with two-finger gestures - those are device-side only and
        // need no APK capability. This merely aligns v1.6.x's declaration with what
        // it can actually do (the firmware records the bit but does not act on it,
        // so behavior is unchanged).
        final byte[] capsEnable = PushProto.encodeAiCapsEnable(
                PushProto.CAP_AI_QUERY | PushProto.CAP_AI_CONTEXT | PushProto.CAP_AI_MULTI
                        | PushProto.CAP_AI_SCENE
                        | PushProto.CAP_AI_SCENE_VIEW);
        uiLog("声明 APK 能力（0x09，bits=0x"
                + Long.toHexString(PushProto.CAP_AI_QUERY | PushProto.CAP_AI_CONTEXT
                        | PushProto.CAP_AI_MULTI | PushProto.CAP_AI_SCENE
                        | PushProto.CAP_AI_SCENE_VIEW) + "）……");
        if (!writeChar(cCtrl, capsEnable)) {
            uiLog("能力声明未送达，按旧帧（66B）工作");
        }
        listenMode = true;
        uiLog("已连接并保持监听——在墨水屏上点英文词，释义会自动回传（约 2-5 秒出卡）。");
        refreshAiStatus();
        return true;
    }

    private void stopListen() {
        listenMode = false;
        cCtrlAi = null;
        devCaps = -1L;
        // M1.3：停监听即停保活 FGS + 撤销未决重试（连接都没了，重试无意义）。
        // M1.3: stop the FGS and cancel pending retries - no link, no retry.
        cancelAiRetry();
        mFgsRunning = false;
        ListenKeepAliveService.stop(this);
        closeQuietly();
        uiLog("已停止监听并断开");
        refreshAiStatus();
    }

    /** 分发设备来的状态帧：0x18 记能力位，0x17 触发查词，0x19 听书页文本。 */
    private void dispatchAiStatus(byte[] frame) {
        int op = frame.length > 0 ? (frame[0] & 0xFF) : 0;
        if (op == PushProto.ST_CAPS) {
            long bits = PushProto.capsBits(frame);
            if (bits >= 0) {
                devCaps = bits;
                boolean ai = (bits & PushProto.CAP_AI_QUERY) != 0;
                uiLog("设备能力上报：AI 查词" + (ai ? "已支持" : "不支持")
                        + "（bits=0x" + Long.toHexString(bits) + "）");
                refreshAiStatus();
            }
            return;
        }
        if (op == PushProto.ST_AI_QUERY) {
            if (!listenMode) {
                return; // 没开监听就不应答（老行为：推送流程不处理 AI 帧）
            }
            // gen26.5：优先按上下文档（74B）解，解不出再退回旧帧（66B）——同一份宽容
            // 顺序，新旧固件 × 新旧 APK 任意组合都拿得到词；拿不到上下文就当 0（
            // Step6 起用它把「这一本、这一处」写进提示词）。
            // Prefer the contextual frame (74B), fall back to legacy (66B): any
            // firmware x APK combination still yields the word; missing context
            // reads as 0 (Step6 feeds it into the prompt).
            String word = PushProto.aiQueryStatusCtxWord(frame);
            long bookId = 0L;
            long pos = 0L;
            if (word != null) {
                bookId = PushProto.aiQueryStatusCtxBookId(frame);
                pos = PushProto.aiQueryStatusCtxPos(frame);
            } else {
                word = PushProto.aiQueryWord(frame); // 旧 66B 帧 / legacy 66B frame
            }
            if (word == null || word.isEmpty()) {
                return;
            }
            onAiQuery(word, bookId, pos);
        }
        if (op == PushProto.ST_TTS_TEXT) {
            dispatchTtsText(frame);
        }
        // gen30 伴读案（R0 冻结件 1）：0x1B 模式广播 / 0x1C 页号对账前置帧。
        // 与 0x19 同走 STATUS 特征、按首字节 op 分发；不受 listenMode 门控——
        // 点词监听与伴读是两段独立会话（R0 §一）。
        // gen30: MODE_SET / PAGE_BEGIN share the STATUS characteristic with 0x19.
        if (op == PushProto.ST_MODE_SET) {
            onCompanionModeSet(frame);
        }
        if (op == PushProto.ST_PAGE_BEGIN) {
            onCompanionPageBegin(frame);
        }
        // 修复轮解耦：0x1D 朗读使能位。设备把「听书开关」作为独立事实传下来，
        // 手机据此只控制声音——摘要请求照旧由页文本无条件触发。
        // Repair-round decoupling: 0x1D carries the listening switch as its own
        // fact; the phone gates playback on it while summaries stay unconditional.
        if (op == PushProto.ST_TTS_ENABLE) {
            onTtsEnable(frame);
        }
        // M3：0x1A 情景请求。M3-A 只验链路——重组完整后打一条日志（上下文字节数、
        // 词偏移、截断/回退标志），情景提示词与视图在 M3-B 接。
        // M3: 0x1A scene request. M3-A verifies the link only - a completed request
        // logs its context bytes, word offset and flags; the prompt and view land
        // in M3-B.
        if (op == PushProto.ST_AI_SCENE) {
            SceneReassembler.Scene scene = sceneReassembler.accept(frame);
            if (scene != null) {
                int bytes = scene.context.getBytes(StandardCharsets.UTF_8).length;
                uiLog("收到情景上下文：" + scene.word + "（" + bytes + " 字节，偏移 "
                        + scene.wordOffset + (scene.excerpt() ? "，整段" : "")
                        + (scene.truncated() ? "，已截断" : "")
                        + (scene.sentenceFallback() ? "，句界回退" : "") + "）");
                // M3-B：链路验完就干活——四节分析请求，结果走 0x08 回帧（word 原样
                // 回填，设备靠它认领）。失败不回帧：设备 16s 期限自己降级，
                // 不把「抱歉」之类的占位文本塞进卡面。
                // M3-B: the link is verified, now do the work - request the four
                // sections and answer over 0x08 with the word echoed back (that is
                // how the device claims it). No frame on failure: the device's 16s
                // deadline degrades the card by itself, and placeholder apologies
                // never reach the body.
                // 26.11.3-C：bit2 = 整段选文。v1.6.2 没有这个分支，把段落当「单词 +
                // 背景」解析了五张词卡（设备侧一直在发 0x04）。分派这一行就是修复本身。
                // bit2 = whole passage. v1.6.2 had no such branch and answered word
                // cards for passages the device had flagged 0x04 all along.
                if (scene.excerpt()) {
                    onExcerptRequest(scene.word, scene.context);
                } else {
                    onSceneRequest(scene.word, scene.context);
                }
            }
        }
    }

    /* ================= gen30 伴读案 · APK 侧接线（R0 冻结件 1/2）=================
     * 设备是单一真相源：0x1B 广播模式、0x1C 前置页号、0x19 流推页文本；APK
     * 只对账 + 回两帧（CTRL 0x0A SUMMARY / 0x0B TTS_DONE），不缓存多页。
     * 失败 / 超时 / 空回包回「空文本」0x0A 帧（dlen=0）：设备侧右栏三态由
     * 「摘要生成中…」切到「本次未生成」。设备侧实测无 16s 自动降级，不回帧
     * 会永久停在占位态——此处按设备侧实测接线（详见回执工程事实报备）。
     * ==================================================================== */

    /** 四模式编号 → 中文模式名（0x1B 回显；与 push_proto.h 注释逐字一致）。 */
    private static String companionModeName(int mode) {
        switch (mode) {
            case PushProto.RIDE_MODE_FOREIGN: return "外语原著";
            case PushProto.RIDE_MODE_NOVEL:   return "小说网文";
            case PushProto.RIDE_MODE_SOCIAL:  return "社科非虚构";
            case PushProto.RIDE_MODE_CLASSIC: return "古文诗词";
            default: return "未知模式(" + mode + ")";
        }
    }

    /** gen30 0x1B MODE_SET：回显模式名 + 置在途窗 + 旧摘要丢弃（等新 PAGE_BEGIN）。 */
    private void onCompanionModeSet(byte[] frame) {
        final int mode;
        final int source;
        try {
            mode = PushProto.modeSetMode(frame);
            source = PushProto.modeSetSource(frame);
        } catch (IllegalArgumentException e) {
            return; // 畸形 5B 裸帧静默丢弃，不打断既有 STATUS 分发
        }
        companionMode = mode;
        companionActive = true;
        companionModeSwitchInFlight = true; // 0x1B → 新 PAGE_BEGIN 之间挂起摘要请求
        companionReqKey = null;             // 旧摘要作废
        companionReqSnapshot = null;        // A4：模式切换使在途请求的登记作废
        uiLog("伴读模式切换：" + companionModeName(mode)
                + "（source=" + source + "，等页号对账后按新模式重取摘要）");
        refreshCompanionMode();
    }

    /** gen30 0x1C PAGE_BEGIN：页号对账 + 丢弃在途半页 + 结束在途窗。 */
    private void onCompanionPageBegin(byte[] frame) {
        final int pageNo;
        final int mode;
        try {
            pageNo = PushProto.pageBeginPageNo(frame);
            mode = PushProto.pageBeginMode(frame);
        } catch (IllegalArgumentException e) {
            return;
        }
        // mode 是快照冗余：与 0x1B 不一致时按 PAGE_BEGIN 覆盖并记日志（R0 §一）。
        if (mode != companionMode) {
            uiLog("伴读模式以 PAGE_BEGIN 为准覆盖：" + companionModeName(companionMode)
                    + " → " + companionModeName(mode));
            companionMode = mode;
        }
        companionPageNo = pageNo;
        companionActive = true;
        companionModeSwitchInFlight = false; // 在途窗结束，摘要请求解锁
        companionReqKey = null;
        companionReqSnapshot = null;        // A4：翻页使在途请求的登记作废
        // PAGE_BEGIN 是每页 0x19 流的前置帧：丢弃在途半页，等下一页 seq==0
        // 自然收敛（R0 §一「复用 seq==0 语义」）。accept/reset 同在 BLE 回调线程。
        ttsReassembler.reset();
        uiLog("伴读页号对账：page=" + pageNo + " · " + companionModeName(mode));
        refreshCompanionMode();
        /* A1（14:26 裁定）：0x1C 只做页号对账 + 在途状态重置，绝不发起摘要请求。
           原「cached 重请求」用上一页正文配新页号发请求、去重键又吞掉 0x19 的
           正确请求，正是 P1「张冠李戴」的构造性根因；模式切换兜底已由设备侧
           s_tts_last_chapter/page=SIZE_MAX 强制重推 0x1C+0x19 覆盖，此分支整段删除。
           摘要唯一发令点是 0x19 正文组装完成（dispatchTtsText）。
           取证日志②：现场应恒见「cached触发=否」，且 cached前20字仍指向上一页
           ——证明错配源仍在、但已不再据此发请求（P1 实锤回验）。 */
        uiLog("取证 0x1C page=" + pageNo + " cached触发=否（A1）cached前20字="
                + CompanionReqGuard.head(companionLastPage, 20));
    }

    /**
     * 0x1D 朗读使能落点（修复轮解耦）。
     * <p>设备把听书开关当独立事实发下来，手机只在这里更新一个布尔：此后页文本
     * 到达仍无条件请求摘要（右栏恒更新），但只有该位为真才朗读。开关关闭时设备
     * 仍推文本，所以摘要不会因关听书而停——这正是现场异议要的「只看不听」。
     * <p>Link 断开时不在本地改这一位：设备在重连时会同步一次（见固件侧
     * tts_follow_poll 的 up 边沿），本地乱猜只会制造「界面与设备不一致」。
     */
    private void onTtsEnable(byte[] frame) {
        boolean[] on = new boolean[1];
        if (!PushProto.isTtsEnable(frame, on)) {
            return;
        }
        if (ttsEnableFromDevice == on[0]) {
            return; // 无变化不刷 UI
        }
        ttsEnableFromDevice = on[0];
        uiLog(on[0] ? "设备已开启听书：后续页文本将朗读（摘要链不受影响）"
                : "设备已关闭听书：仅更新右栏摘要，不再朗读");
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                refreshTtsStatus();
            }
        });
    }

    /** 0x1B 回显落点：复用既有 TTS 状态行，不新增 UI 元素（控射程）。 */
    private void refreshCompanionMode() {
        refreshTtsStatus();
    }

    /**
     * gen30 0x0A SUMMARY：后台线程调 DeepSeek，成功后回带文本的裸帧；
     * 失败 / 超时 / 空回包回空文本帧（设备侧落「本次未生成」占位）。
     * <p>步 4 A 组（14:26 裁定）：
     * <ul>
     *   <li><b>A1</b> 入口准入闸 mayIssueFrom(source)：唯一合法发令点是 0x19 正文组装
     *       完成；0x1C-cached 恒拒，作 cached 分支绝迹的回归护栏；</li>
     *   <li><b>A3</b> 去重键改为 pageNo:mode:正文指纹（CompanionReqGuard.reqKey），
     *       同页异文不误判重复、同页同文仍去重；</li>
     *   <li><b>A4</b> 回执在既有页号/模式复核之外，再比对「发起时捕获的正文快照」
     *       与「当前登记的最新请求快照」，正文已变 / 断链复位一律丢弃迟到回执。</li>
     * </ul>
     * 禁令照登：加严页号丢弃条件不得做、只加延迟等待不得做——对账三层本就正确，
     * 页号不匹配丢弃是既有逻辑，绝不因本次故障去加严它。
     */
    private void requestCompanionSummary(final String pageText, final int pageNo,
                                          final int mode, final int source) {
        // A1：发令来源准入闸（0x1C 绝迹护栏，纯逻辑、宿主可测）。
        if (!CompanionReqGuard.mayIssueFrom(source)) {
            uiLog("取证 摘要请求被准入闸拒绝 来源="
                    + CompanionReqGuard.sourceLabel(source) + "（A1：仅 0x19 放令）");
            return;
        }
        if (!companionActive || companionModeSwitchInFlight || pageText == null
                || pageText.isEmpty()) {
            return;
        }
        if (pageNo < 0 || pageNo != companionPageNo) {
            return; // 既有页号对账：页号已翻走不发过期请求（非「加严」，原样保留）
        }
        // A3：去重键含正文指纹。
        final String key = CompanionReqGuard.reqKey(pageNo, mode, pageText);
        if (key.equals(companionReqKey)) {
            return; // 同页同模式同正文已请求，去重防浪费
        }
        companionReqKey = key;
        // A4 基准：发起请求时捕获的正文快照（局部 final 引用，不随 companionLastPage 覆写）。
        // 登记为「当前有效请求」；回执时与之比对，而非现读已被覆写的 companionLastPage。
        final String snapshot = pageText;
        companionReqSnapshot = snapshot;
        // 取证日志①：请求发起的页号 / 正文前 20 字 / 字数 / 来源。现场应恒见来源=0x19。
        uiLog("取证 请求发起 page=" + pageNo
                + " 正文前20字=" + CompanionReqGuard.head(pageText, 20)
                + " 正文字数=" + pageText.length()
                + " 来源=" + CompanionReqGuard.sourceLabel(source));
        uiLog("伴读摘要生成中…（page=" + pageNo + " · " + companionModeName(mode) + "）");
        new Thread(new Runnable() {
            @Override public void run() {
                // Android 主线程禁网：整个 LLM 往返在后台线程（同 onAiQuery 范式）。
                AiClient.Result r = AiClient.summarize(aiConfig(), snapshot, mode);
                final String text = r.ok ? r.text : "";
                runOnUiThread(new Runnable() {
                    @Override public void run() {
                        // 既有迟到回执：页号或模式已变一律丢弃，不覆盖新页右栏。
                        if (pageNo != companionPageNo || mode != companionMode) {
                            return;
                        }
                        // A4 纵深防线：正文已变（翻页 / 同页异文重推 / 断链复位）→
                        // 登记快照已被顶掉或清空，本次迟到回执绝不发帧。
                        if (!CompanionReqGuard.receiptCurrent(snapshot, companionReqSnapshot)) {
                            uiLog("取证 回执丢弃 正文已变 page=" + pageNo
                                    + "（A4 内容比对不命中）");
                            return;
                        }
                        sendCompanionSummary(text, pageNo, mode);
                    }
                });
            }
        }).start();
    }

    /** 回 CTRL 0x0A SUMMARY 裸帧（方案 B，单帧 241B；二轮按 233B 槽分片）。
     *  设备端只在「idx 从 0 连续且收齐 total」时整段交付，因此这里逐帧写、帧间
     *  留 20 ms 让 notify 排空；任一片写失败即整段作废（设备侧会丢弃残片），
     *  下一轮翻页重新请求，不会拼出半句冒充完整结论。 */
    private void sendCompanionSummary(final String text, final int pageNo, final int mode) {
        final BluetoothGattCharacteristic c = cCtrlAi;
        if (c == null) {
            uiLog("未持有 CTRL 特征，无法回传伴读摘要");
            return;
        }
        final java.util.List<byte[]> frames;
        try {
            frames = PushProto.encodeSummaryShards(text, pageNo, mode);
        } catch (IllegalArgumentException e) {
            uiLog("伴读摘要帧参数越界，已丢弃：" + e.getMessage());
            return;
        }
        final int shards = frames.size();
        new Thread(new Runnable() {
            @Override public void run() {
                synchronized (writeLock) {
                    boolean ok = true;
                    for (int i = 0; i < shards; i++) {
                        if (!writeChar(c, frames.get(i))) { ok = false; break; }
                        if (i + 1 < shards) {
                            try { Thread.sleep(20); } catch (InterruptedException ignored) { }
                        }
                    }
                    final boolean sent = ok;
                    runOnUiThread(new Runnable() {
                        @Override public void run() {
                            uiLog(sent
                                    ? (text.isEmpty()
                                        ? "伴读摘要已回传（空文本 → 设备显示「本次未生成」）"
                                        : "伴读摘要已回传（" + text.length() + " 字 · "
                                            + shards + " 帧）")
                                    : "伴读摘要回传失败（第 " + shards + " 片写入被拒，整段作废）");
                        }
                    });
                }
            }
        }).start();
    }

    /** gen30 0x0B TTS_DONE：播完一律回，设备端按 NVS 设置自治消费（R0 §四）。 */
    private void sendCompanionTtsDone(final int pageNo, final int status) {
        final BluetoothGattCharacteristic c = cCtrlAi;
        if (c == null) {
            return;
        }
        final byte[] frame;
        try {
            frame = PushProto.encodeTtsDone(pageNo, status);
        } catch (IllegalArgumentException e) {
            return;
        }
        new Thread(new Runnable() {
            @Override public void run() {
                synchronized (writeLock) {
                    writeChar(c, frame); // 静默账务：不刷新 UI、不弹错误
                }
            }
        }).start();
    }

    /**
     * gen26.5 Step10：0x19 页文本 → 重组 → 朗读。BLE 回调线程只做纯逻辑重组
     * （快、无锁、不碰 UI），朗读/FGS/状态一律回 UI 线程。半页在途或本页作废
     * （accept 返回 null）静默等待；空页（""）= 静默——设备侧本不推空页，
     * 此处是防御性同语义（这页没字可读，不切断上一页的剩余部分也无页可读）。
     * Fold 0x19 page text and read it aloud: the BLE callback thread only runs
     * the pure reassembler (fast, lock-free, no UI); speaking, the FGS and the
     * status row all hop to the UI thread. A half-page or a dropped page
     * (accept == null) waits in silence; an empty page ("") reads as silence
     * (the defensive twin of the device rule - nothing to read on this page).
     */
    private void dispatchTtsText(byte[] frame) {
        final String page = ttsReassembler.accept(frame);
        if (page == null) {
            return; // 半页在途/缺帧作废：等下一页 seq==0 自然收敛
        }
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                ttsLastPageMs = System.currentTimeMillis();
                ++ttsPagesRead;
                // gen30 伴读案：伴读态把 PAGE_BEGIN 对账页号绑进本次朗读（播完
                // 回 TTS_DONE 用），并按该页号请求摘要；纯听书仍走无页号旧入口，
                // 状态、回帧、请求三者全不变（R0 §一）。
                final boolean comp = companionActive && companionPageNo >= 0
                        && !companionModeSwitchInFlight;
                final int compPageNo = comp ? companionPageNo : -1;
                final int compMode = companionMode;
                if (comp) {
                    // A1 后 0x19 是摘要唯一发令点；显式传来源给准入闸与取证日志。
                    companionLastPage = page;
                    requestCompanionSummary(page, compPageNo, compMode,
                            CompanionReqGuard.SRC_PAGE_TEXT);
                }
                // 修复轮解耦：摘要已在上面无条件请求（comp 分支），朗读在这里才被
                // 0x1D 使能位门控——设备关掉听书后仍推页文本（摘要恒通、右栏照常
                // 更新），只有声音停。空页（""）永远走停止分支，与使能位无关：
                // 那是设备明确的「立即静默」语义，不能被 gating 掉。
                // Repair-round decoupling: the summary is requested above
                // unconditionally; playback is gated on the 0x1D enable bit. An
                // empty page always reaches speakFollow - it is the device's
                // explicit "silence now" signal and must never be gated away.
                if (page.isEmpty() || ttsEnableFromDevice) {
                    // 跟随模式：打断当前读新页（"" = 静默）。/ Follow: interrupt
                    // and read the new page ("" = silence).
                    ttsSpeaker.speakFollow(page, compPageNo);
                }
                // 来页即保活：点词监听与听书朗读是两段独立会话，两个 FGS 互不
                // 抢占；激进 ROM 拒启时服务内降级不崩，状态行如实显示。
                // A page arrives -> keep-alive. The listening and read-aloud
                // sessions are independent and their FGS never preempt each
                // other; a refusing ROM degrades inside the service.
                if (!mTtsFgsRunning) {
                    mTtsFgsRunning = true;
                    TtsKeepAliveService.start(MainActivity.this);
                }
                scheduleTtsIdleStop();
                refreshTtsStatus();
            }
        });
    }

    /** 重排 10 分钟空闲停 FGS（每页到达重排；只停保活，不打断可能仍在读的长页）。 */
    private void scheduleTtsIdleStop() {
        if (mTtsIdleStop != null) {
            mTtsIdleHandler.removeCallbacks(mTtsIdleStop);
        }
        mTtsIdleStop = new Runnable() {
            @Override
            public void run() {
                mTtsIdleStop = null;
                mTtsFgsRunning = false;
                TtsKeepAliveService.stop(MainActivity.this);
                refreshTtsStatus(); // 「已待机」如实显示
            }
        };
        mTtsIdleHandler.postDelayed(mTtsIdleStop, TTS_IDLE_STOP_MS);
    }

    /** 停止 TTS 保活 + 撤销空闲定时（断开/退出时调，幂等）。 */
    private void stopTtsKeepAlive() {
        if (mTtsIdleStop != null) {
            mTtsIdleHandler.removeCallbacks(mTtsIdleStop);
            mTtsIdleStop = null;
        }
        mTtsFgsRunning = false;
        TtsKeepAliveService.stop(this);
    }

    /** 跟随朗读是否进行中（最近一页在 10 分钟空闲窗内）——查词发音联动判据。 */
    private boolean ttsActive() {
        final long last = ttsLastPageMs;
        return last != 0 && System.currentTimeMillis() - last < TTS_IDLE_STOP_MS;
    }

    /** TTS 状态行：未开始 / 朗读中（页数 + 引擎态）/ 已待机。只反映事实。 */
    private void refreshTtsStatus() {
        if (ttsStatus == null) {
            return;
        }
        StringBuilder sb = new StringBuilder("听书：");
        if (ttsLastPageMs == 0) {
            sb.append("未开始（在墨水屏工具条开「听书」并打开跟随开关）");
        } else if (ttsActive()) {
            sb.append("朗读中 · 已读 ").append(ttsPagesRead).append(" 页");
            sb.append(ttsSpeaker.ready() ? " · 引擎就绪" : " · 引擎不可用");
        } else {
            sb.append("已待机（10 分钟无新页，翻页自动续读）");
        }
        // gen30 0x1B 回显：伴读态在既有状态行末尾追加当前模式名（零新增 UI 元素）。
        if (companionActive) {
            sb.append(" · 伴读：").append(companionModeName(companionMode));
        }
        final String text = sb.toString();
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                ttsStatus.setText(text);
            }
        });
    }

    /**
     * gen26.5 Step10：朗读设置——语速/音调离散档循环 + 系统 TTS 引擎入口 +
     * 引擎就绪态。引擎选择走系统（本 App 不私存引擎包名：装了什么引擎是用户
     * 的决定）；档位存 SharedPreferences，init 成功时刷进引擎。
     * Read-aloud settings: discrete rate/pitch step cycling, the system TTS
     * engine entry and engine readiness. Engine choice belongs to the system;
     * the steps persist in SharedPreferences and land in the engine on init.
     */
    private void showTtsSettings() {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        int p = dp(16);
        box.setPadding(p, p, p, p);

        TextView stEngine = new TextView(this);
        stEngine.setTextSize(13);
        stEngine.setText(ttsSpeaker.ready()
                ? "朗读引擎：就绪（档位下面切换，引擎本身在系统设置里选）"
                : "朗读引擎：未就绪（系统没有可用的 TTS 引擎，或仍在初始化）");
        box.addView(stEngine);

        final TextView stRate = new TextView(this);
        stRate.setTextSize(15);
        stRate.setPadding(0, dp(8), 0, 0);
        stRate.setText("语速：" + rateLabel(ttsSpeaker.rateIdx(this))
                + "（点一下换一档，共 " + TtsSpeaker.RATE_STEPS.length + " 档）");
        box.addView(stRate);

        final TextView stPitch = new TextView(this);
        stPitch.setTextSize(15);
        stPitch.setText("音调：" + pitchLabel(ttsSpeaker.pitchIdx(this))
                + "（点一下换一档，共 " + TtsSpeaker.PITCH_STEPS.length + " 档）");
        box.addView(stPitch);

        Button btnRate = new Button(this);
        btnRate.setText("切换语速");
        btnRate.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                int i = ttsSpeaker.cycleRate(MainActivity.this);
                stRate.setText("语速：" + rateLabel(i)
                        + "（点一下换一档，共 " + TtsSpeaker.RATE_STEPS.length + " 档）");
            }
        });
        box.addView(btnRate);

        Button btnPitch = new Button(this);
        btnPitch.setText("切换音调");
        btnPitch.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                int i = ttsSpeaker.cyclePitch(MainActivity.this);
                stPitch.setText("音调：" + pitchLabel(i)
                        + "（点一下换一档，共 " + TtsSpeaker.PITCH_STEPS.length + " 档）");
            }
        });
        box.addView(btnPitch);

        Button btnEngine = new Button(this);
        btnEngine.setText("系统 TTS 设置（选引擎 / 装语音数据）");
        btnEngine.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                TtsSpeaker.openSystemTtsSettings(MainActivity.this);
            }
        });
        box.addView(btnEngine);

        new android.app.AlertDialog.Builder(this)
                .setTitle("朗读设置")
                .setView(box)
                .setNegativeButton("完成", null)
                .show();
    }

    private static String rateLabel(int idx) {
        return TtsSpeaker.RATE_STEPS[idx] + "×";
    }

    private static String pitchLabel(int idx) {
        return TtsSpeaker.PITCH_STEPS[idx] + "×";
    }

    /** 收到点词：后台调 LLM，拿到释义后回 0x07。 */
    private void onAiQuery(final String word, final long bookId, final long pos) {
        uiLog("墨水屏点词「" + word + "」，查询中……"
                + (bookId != 0L ? "（上下文：书 0x" + Long.toHexString(bookId)
                        + " @ " + pos + "B）" : ""));
        // M1.3：新点词接管——旧计划立即作废（其迟到回调被 isFinished 挡回），
        // 未决快速重试由 cancelAiRetry 撤回，防止两次点词互相抢发。
        // M1.3: a new word takes over; the old plan is marked finished so its
        // late callbacks are dropped and its pending quick retry is withdrawn.
        cancelAiRetry();
        final AiKeepAlive plan = new AiKeepAlive(word, bookId, pos,
                System.currentTimeMillis());
        mAiRetry = plan;
        runAiQueryAttempt(plan);
    }

    /**
     * M3-B：情景分析请求。与点词释义**各走各的**：同一个 AiClient 通道、不同的
     * system 提示与清洗，结果同样用 sendAiResult() 走 0x08 分帧（word 作关联键）。
     * 不做重试：情景是用户当场发起的上下文查询，16s 内没等到就让他重按一次，
     * 比在后台补一条已经过期的分析更有意义。
     * M3-B scene analysis. Independent of the tap-to-define flow: same AiClient
     * transport, different system prompt and cleanup, and the result goes out over
     * 0x08 via sendAiResult() with the word as the correlation key. No retry: a
     * scene request is a right-now question, and re-tapping beats delivering a
     * stale analysis minutes later.
     */
    private void onSceneRequest(final String word, final String context) {
        uiLog("情景请求：" + word + "（" + context.length() + " 字）");
        new Thread(new Runnable() {
            @Override
            public void run() {
                final AiClient.Result r = AiClient.scene(aiConfig(), word, context);
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        if (!r.ok) {
                            uiLog("情景失败：" + r.error + "（不回帧）");
                            return;
                        }
                        uiLog("情景：" + word + " 共 " + r.text.length() + " 字");
                        sendAiResult(word, r.text);
                    }
                });
            }
        }).start();
    }

    /**
     * 26.11.3-C：整段选文的四节解析（bit2）。与 onSceneRequest 并列、不合并：
     * 问的对象不同（段 vs 词），失败日志的措辞不同，回帧时回填的 word 也不同
     * （段落模式回填的 word 只是设备认领用的锚点，不是卡面标题）。
     *
     * Four-section passage analysis (bit2). Sibling to onSceneRequest, not merged:
     * different subject (passage vs word), different failure wording, and the word
     * echoed back is only the device's claim anchor, not a card title.
     */
    private void onExcerptRequest(final String word, final String context) {
        uiLog("选段请求：" + context.length() + " 字（锚点 " + word + "）");
        new Thread(new Runnable() {
            @Override
            public void run() {
                final AiClient.Result r = AiClient.excerpt(aiConfig(), context);
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        if (!r.ok) {
                            uiLog("选段失败：" + r.error + "（不回帧）");
                            return;
                        }
                        // 26.11.3-C2：产出比判据的**机器可读**行。审计定的判据是
                        // 「def 字节 / 输入字节 ≥ 3×（英文）/ ≥ 2×（中文）」，所以这里
                        // 必须同时打出字节数——只打字符数时，中英混排的比值会失真，
                        // 而这次事故正是「字」这个单位把 883→305 的萎缩减成了 0.35×。
                        // 26.11.3-C2: the machine-readable line behind the output-ratio
                        // criterion (>= 3x for English, >= 2x for Chinese, in BYTES).
                        // Characters alone distort a mixed CJK/Latin ratio - and "chars"
                        // is exactly the unit that hid 883 -> 305 as 0.35x.
                        final int inChars = context.length();
                        final int inBytes = context.getBytes(UTF8).length;
                        final int outChars = r.text.length();
                        final int outBytes = r.text.getBytes(UTF8).length;
                        uiLog("选段：" + inChars + " 字 / " + inBytes + " B → "
                                + outChars + " 字 / " + outBytes + " B"
                                + "（truncated=" + (r.truncated ? 1 : 0) + "）");
                        if (r.truncated) {
                            // 撞帽必须喊出来：省略号不解释自己，日志必须解释。
                            // Hitting the cap must be said out loud - an ellipsis does
                            // not explain itself, the log has to.
                            uiLog("选段被截断：输出超过 " + AiClient.EXCERPT_DEF_MAX_BYTES
                                    + " B 上限（复测卡「输出顶到上限」档照此判读）");
                        }
                        sendAiResult(word, r.text);
                    }
                });
            }
        }).start();
    }

    /** M1.3：一次查询尝试（首发与重试共用）。结果回 UI 线程统一编排。 */
    private void runAiQueryAttempt(final AiKeepAlive plan) {
        new Thread(new Runnable() {
            @Override
            public void run() {
                final AiClient.Result r = AiClient.define(aiConfig(), plan.word());
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        // 已被取代/取消（新点词、停监听、退出）：迟到结果不动作。
                        // Superseded/cancelled (new word, stop, exit): drop it.
                        if (plan.isFinished() || mAiRetry != plan) {
                            return;
                        }
                        if (!r.ok) {
                            // 失败不回帧：设备不会弹出空卡（诚实优于假成功）。
                            // 但 M1.3 不再就此罢手：息屏挂起期间网络恢复时补得上。
                            // No frame on failure (the device never shows an empty
                            // card); M1.3 adds event-driven retry so a network
                            // recovery can still land the 0x07 inside the window.
                            uiLog("释义失败：" + r.error + "（不回帧，安排重试）");
                            scheduleAiRetry(plan);
                            refreshAiStatus();
                            return;
                        }
                        plan.markFinished();
                        uiLog("释义：" + r.text);
                        sendAiResult(plan.word(), r.text);
                        // gen26.5 Step10「AI 联动」：跟随朗读进行中 → 顺带发音
                        // （打断当前页读「词 + 释义」——用户的注意力在词上；
                        // 下一页推送自然续上跟随，无需本地补读）。
                        // AI linkage: while following, pronounce the word plus
                        // its definition (the current page is cut on purpose;
                        // the next page push resumes the follow - no local
                        // catch-up needed).
                        if (ttsActive()) {
                            ttsSpeaker.speakWord(plan.word(), r.text);
                        }
                        refreshAiStatus();
                    }
                });
            }
        }).start();
    }

    /**
     * M1.3：失败后的重试编排（UI 线程）。3s 快速重试一次（防瞬时 DNS 抖动），
     * 之后只等事件（网络可用 / 亮屏），14s 窗口 cap 贴设备 15s 超时。快速重试
     * 与事件互斥：事件抢先时撤回挂起的快速重试，不双发。
     * Retry orchestration after a failure: one 3s quick retry, then events only,
     * capped at a 14s window. The quick retry and events are mutually exclusive.
     */
    private void scheduleAiRetry(final AiKeepAlive plan) {
        if (plan.isFinished() || mAiRetry != plan) {
            return; // 已终态或被新查询接管
        }
        final long now = System.currentTimeMillis();
        if (plan.windowExpired(now)) {
            // 超窗放弃：迟到释义走设备侧预取缓存门 A 语义，下次点同词秒出。
            // Window exhausted: a late 0x07 lands in the device prefetch cache
            // (gate A) and the next tap on the same word answers instantly.
            plan.markFinished();
            unregisterKeepAliveEvents();
            uiLog("重试窗口已过（" + (AiKeepAlive.RETRY_WINDOW_MS / 1000)
                    + "s），本次点词放弃——下次点同一词会命中预取缓存秒出");
            refreshAiStatus();
            return;
        }
        if (!plan.quickRetryUsed()) {
            // 安排一次性快速重试（距查询发起 3s；查询本身耗时时余量自然缩短）。
            // Schedule the one-shot quick retry at start+3s; a slow query just
            // shortens the remaining delay.
            plan.consumeQuickRetry();
            final long delay = Math.max(
                    0, plan.startMs() + AiKeepAlive.QUICK_RETRY_DELAY_MS - now);
            uiLog("网络疑似被挂起，" + (AiKeepAlive.QUICK_RETRY_DELAY_MS / 1000)
                    + " 秒后快速重试一次，之后只等网络恢复/亮屏事件……");
            mQuickRetryRunnable = new Runnable() {
                @Override
                public void run() {
                    mQuickRetryRunnable = null;
                    if (plan.isFinished() || mAiRetry != plan) {
                        return;
                    }
                    final long t = System.currentTimeMillis();
                    if (plan.windowExpired(t)) {
                        plan.markFinished();
                        unregisterKeepAliveEvents();
                        uiLog("重试窗口已过（" + (AiKeepAlive.RETRY_WINDOW_MS / 1000)
                                + "s），本次点词放弃——下次点同一词会命中预取缓存秒出");
                        refreshAiStatus();
                        return;
                    }
                    runAiQueryAttempt(plan); // 快速重试这一次
                }
            };
            mQuickRetryHandler.postDelayed(mQuickRetryRunnable, delay);
            registerKeepAliveEvents(); // 事件也挂上：快速重试后再失败只等事件
        } else if (plan.eventRetryDue(now)) {
            // 额度已用（快速重试已发或事件抢先）：只挂事件等恢复。
            // Quota spent: wait for events only.
            registerKeepAliveEvents();
        }
        // 其余组合（未用额度但事件也……）不会出现：上面两支穷尽。
    }

    /** M1.3：网络可用 / 亮屏 / 解锁到达——窗口内补发一次查询。 */
    private void onKeepAliveEvent(String why) {
        final AiKeepAlive plan = mAiRetry;
        if (plan == null || plan.isFinished()) {
            unregisterKeepAliveEvents(); // 无事可做：撤掉监听，不空转
            return;
        }
        final long now = System.currentTimeMillis();
        if (plan.windowExpired(now)) {
            plan.markFinished();
            unregisterKeepAliveEvents();
            uiLog("重试窗口已过（" + (AiKeepAlive.RETRY_WINDOW_MS / 1000)
                    + "s），本次点词放弃——下次点同一词会命中预取缓存秒出");
            refreshAiStatus();
            return;
        }
        if (plan.eventRetryDue(now)) {
            // 事件抢先：撤回挂起的快速重试并消耗其额度——两条路不双发。
            // The event preempts the quick retry: withdraw it and consume its
            // quota so exactly one retry goes out.
            plan.consumeQuickRetry();
            if (mQuickRetryRunnable != null) {
                mQuickRetryHandler.removeCallbacks(mQuickRetryRunnable);
                mQuickRetryRunnable = null;
            }
            uiLog("网络恢复（" + why + "），重试查询……");
            runAiQueryAttempt(plan);
        }
    }

    /** M1.3：撤销未决重试计划 + 挂起的快速重试 + 事件监听（幂等）。 */
    private void cancelAiRetry() {
        final AiKeepAlive plan = mAiRetry;
        if (plan != null) {
            plan.markFinished();
        }
        mAiRetry = null;
        if (mQuickRetryRunnable != null) {
            mQuickRetryHandler.removeCallbacks(mQuickRetryRunnable);
            mQuickRetryRunnable = null;
        }
        unregisterKeepAliveEvents();
    }

    /** M1.3：挂网络可用 + 亮屏/解锁事件（幂等，查完即撤）。 */
    private void registerKeepAliveEvents() {
        if (mNetworkCallback == null) {
            try {
                ConnectivityManager cm = (ConnectivityManager)
                        getSystemService(Context.CONNECTIVITY_SERVICE);
                if (cm != null) {
                    NetworkRequest req = new NetworkRequest.Builder()
                            .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                            .build();
                    mNetworkCallback = new ConnectivityManager.NetworkCallback() {
                        @Override
                        public void onAvailable(Network network) {
                            runOnUiThread(new Runnable() {
                                @Override
                                public void run() {
                                    onKeepAliveEvent("网络可用");
                                }
                            });
                        }
                    };
                    cm.registerNetworkCallback(req, mNetworkCallback);
                }
            } catch (Throwable ignored) {
                // 注册失败不崩：快速重试仍在，白名单/FGS 主修不受影响。
                // A failed registration never crashes; the quick retry and the
                // FGS/whitelist primary fixes are unaffected.
                mNetworkCallback = null;
            }
        }
        if (mKeepAliveReceiver == null) {
            mKeepAliveReceiver = new BroadcastReceiver() {
                @Override
                public void onReceive(Context c, Intent i) {
                    final String action = i == null ? null : i.getAction();
                    onKeepAliveEvent(Intent.ACTION_USER_PRESENT.equals(action)
                            ? "解锁亮屏" : "亮屏");
                }
            };
            try {
                IntentFilter f = new IntentFilter(Intent.ACTION_SCREEN_ON);
                f.addAction(Intent.ACTION_USER_PRESENT);
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    registerReceiver(mKeepAliveReceiver, f, Context.RECEIVER_NOT_EXPORTED);
                } else {
                    registerReceiver(mKeepAliveReceiver, f);
                }
            } catch (Throwable ignored) {
                mKeepAliveReceiver = null;
            }
        }
    }

    /** M1.3：注销网络/亮屏事件（幂等）。 */
    private void unregisterKeepAliveEvents() {
        if (mNetworkCallback != null) {
            try {
                ConnectivityManager cm = (ConnectivityManager)
                        getSystemService(Context.CONNECTIVITY_SERVICE);
                if (cm != null) {
                    cm.unregisterNetworkCallback(mNetworkCallback);
                }
            } catch (Throwable ignored) {
                // 重复注销/已回收：无操作即目标态。
            }
            mNetworkCallback = null;
        }
        if (mKeepAliveReceiver != null) {
            try {
                unregisterReceiver(mKeepAliveReceiver);
            } catch (Throwable ignored) {
                // 同上。
            }
            mKeepAliveReceiver = null;
        }
    }

    /** M1.3：电池优化白名单状态（状态行「保活」组合判定的一半）。 */
    private boolean batteryWhitelisted() {
        try {
            PowerManager pm = (PowerManager) getSystemService(Context.POWER_SERVICE);
            return pm != null && pm.isIgnoringBatteryOptimizations(getPackageName());
        } catch (Throwable t) {
            return false; // 取不到就当未授权：状态行显示「受限」，引导用户去开
        }
    }

    /** 按 UTF-8 字节切段，不切断多字节序列。/ chunk by UTF-8 bytes, never mid-sequence. */
    private static List<byte[]> utf8Chunks(String s, int size) {
        byte[] raw = s.getBytes(UTF8);
        List<byte[]> out = new ArrayList<byte[]>();
        int i = 0;
        while (i < raw.length) {
            int j = Math.min(i + size, raw.length);
            while (j > i && j < raw.length && (raw[j] & 0xC0) == 0x80) {
                j--; // 0x80..0xBF = 续字节 / continuation byte
            }
            out.add(Arrays.copyOfRange(raw, i, j));
            i = j;
        }
        return out;
    }

    /**
     * 写 CTRL 回传释义：短释义走 0x07 单帧；**长释义且设备上报 CAP_AI_MULTI** 时
     * 走 0x08 分帧（每帧带同一单词作关连键，seq 从 0 起，末帧收齐才出卡）。
     * BLE 写串行化，且必须离主线程。
     * Short definitions go out as one 0x07 frame; a long one, when the device
     * reports CAP_AI_MULTI, goes as 0x08 segments (every frame carries the word as
     * the correlation key, seq starts at 0, the card surfaces on the last frame).
     */
    private void sendAiResult(String word, String def) {
        final BluetoothGattCharacteristic c = cCtrlAi;
        if (c == null) {
            uiLog("未持有 CTRL 特征，无法回传释义");
            return;
        }
        final byte[] defBytes = def.getBytes(UTF8);
        final boolean multi = devCaps >= 0 && (devCaps & PushProto.CAP_AI_MULTI) != 0;
        final List<byte[]> frames = new ArrayList<byte[]>();
        final boolean segmented = multi && defBytes.length > PushProto.AI_DEF_MAX - 1;
        if (segmented) {
            List<byte[]> chunks = utf8Chunks(def, PushProto.AI_SEG_MAX - 1);
            for (int i = 0; i < chunks.size(); i++) {
                byte[] f = PushProto.encodeAiResultSeg(word, new String(chunks.get(i), UTF8),
                        i, chunks.size());
                if (f == null) { // 不产非法帧：整条退回单帧（可能被截断，但不发坏帧）
                    frames.clear();
                    break;
                }
                frames.add(f);
            }
        }
        if (frames.isEmpty()) {
            frames.add(PushProto.encodeAiResult(word, def)); // 单帧路（0x07）
        }
        final boolean seg = frames.size() > 1;
        final byte[][] out = frames.toArray(new byte[0][]);
        new Thread(new Runnable() {
            @Override
            public void run() {
                synchronized (writeLock) {
                    boolean ok = true;
                    for (int i = 0; i < out.length; i++) {
                        ok = writeChar(c, out[i]) && ok;
                    }
                    final boolean done = ok;
                    final int n = out.length;
                    runOnUiThread(new Runnable() {
                        @Override
                        public void run() {
                            uiLog(done
                                    ? ("释义已回传设备（" + (seg ? "0x08 分帧 " + n + " 帧" : "0x07 单帧")
                                            + "）")
                                    : "释义回传失败（设备未接受写入）");
                        }
                    });
                }
            }
        }).start();
    }

    /* ================= M1.2 ① 提示模板库 =================
     * 命名模板列表存 SharedPreferences（JSON 数组），激活模板按名字引用。
     * 系统指令（230 字硬约束，gen26.6.1 放宽）永远独立于模板、恒定钉在 system 角色——模板只能
     * 改 user 角色的内容/风格，永远碰不到系统约束。这就是「分层铁律」。
     * Named templates live in SharedPreferences (JSON array); the active one is
     * referenced by name. The system instruction (230-char hard constraint, relaxed in gen26.6.1) is
     * always pinned in the system role, independent of templates - the "layering
     * rule": a template edits only the user-role content/style.
     */

    /** 命名模板。/ A named prompt template. */
    private static final class Tpl {
        String name;
        String content;
        Tpl(String name, String content) {
            this.name = name;
            this.content = content;
        }
    }

    /** 首启或升级时播种默认库（仅含「默认」一条）与默认激活名。 */
    private void ensureLibrary() {
        android.content.SharedPreferences sp = getSharedPreferences(PREF, MODE_PRIVATE);
        if (!sp.contains(PREF_TPL_LIB)) {
            List<Tpl> seed = new ArrayList<Tpl>();
            seed.add(new Tpl(DEFAULT_TPL_NAME, AiClient.DEFAULT_TEMPLATE));
            saveLibrary(seed);
        }
        if (!sp.contains(PREF_TPL_ACTIVE)) {
            sp.edit().putString(PREF_TPL_ACTIVE, DEFAULT_TPL_NAME).apply();
        }
    }

    private List<Tpl> loadLibrary() {
        ensureLibrary();
        android.content.SharedPreferences sp = getSharedPreferences(PREF, MODE_PRIVATE);
        List<Tpl> out = new ArrayList<Tpl>();
        String raw = sp.getString(PREF_TPL_LIB, "");
        if (raw == null || raw.isEmpty()) {
            out.add(new Tpl(DEFAULT_TPL_NAME, AiClient.DEFAULT_TEMPLATE));
            return out;
        }
        try {
            JSONArray arr = new JSONArray(raw);
            for (int i = 0; i < arr.length(); i++) {
                JSONObject o = arr.getJSONObject(i);
                String name = o.optString("name", DEFAULT_TPL_NAME);
                String content = o.optString("content", AiClient.DEFAULT_TEMPLATE);
                out.add(new Tpl(name, content));
            }
        } catch (JSONException e) {
            // 损坏就回退单条默认，不崩。
            out.clear();
            out.add(new Tpl(DEFAULT_TPL_NAME, AiClient.DEFAULT_TEMPLATE));
        }
        if (out.isEmpty()) {
            out.add(new Tpl(DEFAULT_TPL_NAME, AiClient.DEFAULT_TEMPLATE));
        }
        return out;
    }

    private void saveLibrary(List<Tpl> lib) {
        JSONArray arr = new JSONArray();
        for (Tpl t : lib) {
            JSONObject o = new JSONObject();
            try {
                o.put("name", t.name == null ? "" : t.name);
                o.put("content", t.content == null ? "" : t.content);
            } catch (JSONException ignored) {
            }
            arr.put(o);
        }
        getSharedPreferences(PREF, MODE_PRIVATE).edit().putString(PREF_TPL_LIB, arr.toString()).apply();
    }

    private String getActiveName() {
        ensureLibrary();
        return getSharedPreferences(PREF, MODE_PRIVATE).getString(PREF_TPL_ACTIVE, DEFAULT_TPL_NAME);
    }

    private void setActiveName(String name) {
        getSharedPreferences(PREF, MODE_PRIVATE).edit().putString(PREF_TPL_ACTIVE, name).apply();
    }

    /** M1.2.1：把存储里的激活模板内容回填到设置页编辑框（UI 与存储对齐）。 */
    private void refreshActiveTemplateEditor() {
        final EditText ed = mActiveTplEditor;
        if (ed != null) {
            ed.setText(getActiveTemplateContent());
        }
    }

    /** 当前激活模板的内容（供 aiConfig 注入 user 角色）。 */
    private String getActiveTemplateContent() {
        String active = getActiveName();
        for (Tpl t : loadLibrary()) {
            if (t.name != null && t.name.equals(active)) {
                return t.content;
            }
        }
        return AiClient.DEFAULT_TEMPLATE;
    }

    /** 把内容写回当前激活的命名模板（按 active 名定位）。 */
    private void setActiveTemplateContent(String content) {
        String active = getActiveName();
        List<Tpl> lib = loadLibrary();
        boolean found = false;
        for (Tpl t : lib) {
            if (t.name != null && t.name.equals(active)) {
                t.content = content;
                found = true;
                break;
            }
        }
        if (!found) {
            // 激活名在库里失踪（理论不该发生）：补一条。
            lib.add(new Tpl(active, content));
        }
        saveLibrary(lib);
    }

    /** 模板库管理页：一键切换 + 新建 + 导入 + 逐项编辑/删除/分享/导出。 */
    private void showTemplateManager() {
        final List<Tpl> lib = loadLibrary();
        String active = getActiveName();
        CharSequence[] items = new CharSequence[lib.size()];
        for (int i = 0; i < lib.size(); i++) {
            items[i] = (lib.get(i).name != null && lib.get(i).name.equals(active) ? "✔ " : "   ")
                    + lib.get(i).name;
        }
        AlertDialog.Builder b = new AlertDialog.Builder(this).setTitle("提示模板库（✔ = 当前）");
        if (lib.isEmpty()) {
            b.setMessage("（空）");
        } else {
            b.setItems(items, new DialogInterface.OnClickListener() {
                @Override
                public void onClick(DialogInterface d, int which) {
                    showTemplateActions(lib.get(which));
                }
            });
        }
        b.setPositiveButton("新建模板", new DialogInterface.OnClickListener() {
            @Override
            public void onClick(DialogInterface d, int which) {
                showTemplateEdit(new Tpl("", AiClient.DEFAULT_TEMPLATE), true);
            }
        });
        b.setNeutralButton("导入文件", new DialogInterface.OnClickListener() {
            @Override
            public void onClick(DialogInterface d, int which) {
                Intent it = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                it.addCategory(Intent.CATEGORY_OPENABLE);
                it.setType(TPL_MIME);
                startActivityForResult(it, REQ_IMPORT);
            }
        });
        b.setNegativeButton("关闭", null);
        b.show();
    }

    /** 单条模板的操作菜单：切换 / 编辑 / 删除 / 分享 / 导出全部。 */
    private void showTemplateActions(final Tpl tpl) {
        final CharSequence[] acts = {"切换为当前", "编辑", "删除", "分享", "导出全部"};
        new AlertDialog.Builder(this)
                .setTitle(tpl.name)
                .setItems(acts, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int which) {
                        switch (which) {
                            case 0: // 切换为当前
                                setActiveName(tpl.name);
                                // M1.2.1：回填设置页编辑框到新激活模板内容——旧版只改激活名、
                                // 编辑框仍显示旧模板，用户一保存就把旧内容覆盖到新模板（擦除）。
                                // M1.2.1: refresh the settings editor to the new active
                                // template - the old version only switched the active name
                                // while the editor kept the old content, so a later 保存
                                // overwrote the new template with the stale text (erasure).
                                refreshActiveTemplateEditor();
                                refreshAiStatus();
                                uiLog("已切换提示模板：" + tpl.name);
                                break;
                            case 1: // 编辑
                                showTemplateEdit(tpl, false);
                                break;
                            case 2: // 删除
                                deleteTemplate(tpl);
                                break;
                            case 3: // 分享
                                shareTemplate(tpl);
                                break;
                            case 4: // 导出全部
                                exportLibrary();
                                break;
                            default:
                                break;
                        }
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private void deleteTemplate(final Tpl tpl) {
        final List<Tpl> lib = loadLibrary();
        if (lib.size() <= 1) {
            uiLog("至少保留一条模板，无法删除");
            return;
        }
        new AlertDialog.Builder(this)
                .setTitle("删除模板「" + tpl.name + "」？")
                .setMessage("删除后不可恢复。")
                .setPositiveButton("删除", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int which) {
                        // M1.2.1：按名字删——旧版用 lib.remove(tpl) 按对象身份删，而 tpl
                        // 来自另一份 loadLibrary() 快照、Tpl 未覆写 equals，identity 不等
                        // → 删除静默无效（真机实测 bug）。这里按 name 精确移除。
                        // M1.2.1: delete by name - the old lib.remove(tpl) relied on
                        // object identity; tpl comes from a different loadLibrary()
                        // snapshot and Tpl has no equals(), so removal was a silent
                        // no-op (real-device bug). Remove precisely by name here.
                        boolean removed = false;
                        for (int i = lib.size() - 1; i >= 0; i--) {
                            if (lib.get(i).name != null && lib.get(i).name.equals(tpl.name)) {
                                lib.remove(i);
                                removed = true;
                            }
                        }
                        if (!removed) {
                            uiLog("删除失败：模板不在库中");
                            return;
                        }
                        saveLibrary(lib);
                        if (tpl.name != null && tpl.name.equals(getActiveName())) {
                            setActiveName(lib.get(0).name);
                        }
                        refreshActiveTemplateEditor(); // M1.2.1：删的是激活模板时回填到新激活
                        refreshAiStatus();
                        uiLog("已删除模板：" + tpl.name);
                        showTemplateManager(); // 刷新列表
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    /** 新建 / 编辑模板对话框。 */
    private void showTemplateEdit(final Tpl tpl, final boolean isNew) {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        int p = dp(16);
        box.setPadding(p, p, p, p);

        final EditText edName = new EditText(this);
        edName.setHint("模板名（如：默认 / 学术 / 口语）");
        edName.setText(isNew ? "" : (tpl.name == null ? "" : tpl.name));
        edName.setSingleLine(true);
        box.addView(edName);

        final EditText edContent = new EditText(this);
        edContent.setHint("模板内容，用 {word} 占位实际单词");
        edContent.setText(tpl.content);
        edContent.setSingleLine(false);
        edContent.setMaxLines(4);
        edContent.setGravity(android.view.Gravity.TOP);
        box.addView(edContent);

        new AlertDialog.Builder(this)
                .setTitle(isNew ? "新建模板" : ("编辑模板「" + tpl.name + "」"))
                .setView(box)
                .setNegativeButton("取消", null)
                .setPositiveButton("保存", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int which) {
                        String name = edName.getText().toString().trim();
                        String content = edContent.getText().toString();
                        if (name.isEmpty()) {
                            uiLog("模板名不能为空");
                            return;
                        }
                        List<Tpl> lib = loadLibrary();
                        // 移除同名（含改名后的目标名）旧条目，再追加新条目。
                        // Drop any same-name (including the rename target) entry, then append.
                        for (int i = lib.size() - 1; i >= 0; i--) {
                            String n = lib.get(i).name;
                            if (n != null && n.equals(name)) {
                                lib.remove(i);
                            }
                        }
                        lib.add(new Tpl(name, content));
                        saveLibrary(lib);
                        setActiveName(name); // 新建/编辑后即设为当前
                        refreshActiveTemplateEditor(); // M1.2.1：回填设置页编辑框到新模板
                        refreshAiStatus();
                        uiLog((isNew ? "已新建并启用模板：" : "已保存模板：") + name);
                        showTemplateManager(); // 刷新列表
                    }
                })
                .show();
    }

    /** 导出整库为 JSON 文件（SAF 选路径，真正写盘在 onActivityResult）。 */
    private void exportLibrary() {
        List<Tpl> lib = loadLibrary();
        JSONArray arr = new JSONArray();
        for (Tpl t : lib) {
            JSONObject o = new JSONObject();
            try {
                o.put("name", t.name == null ? "" : t.name);
                o.put("content", t.content == null ? "" : t.content);
            } catch (JSONException ignored) {
            }
            arr.put(o);
        }
        pendingExportJson = arr.toString();
        Intent it = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        it.addCategory(Intent.CATEGORY_OPENABLE);
        it.setType(TPL_MIME);
        it.putExtra(Intent.EXTRA_TITLE, "readpico" + TPL_SUFFIX);
        startActivityForResult(it, REQ_EXPORT);
    }

    /** 分享单条模板（纯文本，便于直接发给别人用）。 */
    private void shareTemplate(Tpl tpl) {
        Intent it = new Intent(Intent.ACTION_SEND);
        it.setType("text/plain");
        it.putExtra(Intent.EXTRA_SUBJECT, "ReadPico 提示模板：" + (tpl.name == null ? "" : tpl.name));
        it.putExtra(Intent.EXTRA_TEXT, tpl.content == null ? "" : tpl.content);
        startActivity(Intent.createChooser(it, "分享提示模板"));
    }

    private String readUri(android.net.Uri uri) {
        if (uri == null) {
            return "";
        }
        InputStream in = null;
        try {
            in = getContentResolver().openInputStream(uri);
            if (in == null) {
                return "";
            }
            ByteArrayOutputStream bos = new ByteArrayOutputStream();
            byte[] buf = new byte[4096];
            int n;
            while ((n = in.read(buf)) > 0) {
                bos.write(buf, 0, n);
            }
            return new String(bos.toByteArray(), UTF8);
        } catch (Exception e) {
            return "";
        } finally {
            if (in != null) {
                try {
                    in.close();
                } catch (Exception ignored) {
                }
            }
        }
    }

    private boolean writeUri(android.net.Uri uri, String text) {
        if (uri == null) {
            return false;
        }
        OutputStream os = null;
        try {
            os = getContentResolver().openOutputStream(uri);
            if (os == null) {
                return false;
            }
            os.write(text.getBytes(UTF8));
            return true;
        } catch (Exception e) {
            return false;
        } finally {
            if (os != null) {
                try {
                    os.close();
                } catch (Exception ignored) {
                }
            }
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQ_EXPORT) {
            if (resultCode == Activity.RESULT_OK && data != null && pendingExportJson != null
                    && data.getData() != null) {
                boolean ok = writeUri(data.getData(), pendingExportJson);
                uiLog(ok ? "模板库已导出" : "模板库导出失败");
            }
            pendingExportJson = null;
        } else if (requestCode == REQ_IMPORT) {
            if (resultCode == Activity.RESULT_OK && data != null && data.getData() != null) {
                String raw = readUri(data.getData());
                if (importLibraryJson(raw)) {
                    uiLog("模板库已导入");
                    refreshActiveTemplateEditor(); // M1.2.1：导入后回填编辑框到新激活模板
                    showTemplateManager(); // 刷新列表
                    refreshAiStatus();
                } else {
                    uiLog("模板库导入失败（文件格式不对）");
                }
            }
        }
        // REQ_ENABLE_BT / REQ_PERMS 等其它请求不在此处理。
    }

    /** 解析导入的 JSON 并替换整库；返回是否成功。 */
    private boolean importLibraryJson(String raw) {
        if (raw == null || raw.isEmpty()) {
            return false;
        }
        try {
            JSONArray arr = new JSONArray(raw);
            List<Tpl> lib = new ArrayList<Tpl>();
            for (int i = 0; i < arr.length(); i++) {
                JSONObject o = arr.getJSONObject(i);
                String name = o.optString("name", DEFAULT_TPL_NAME);
                String content = o.optString("content", AiClient.DEFAULT_TEMPLATE);
                // 唯一性规范化（M1.2.1 审计尾巴）：按 name 删除依赖「模板名唯一」，
                // 文件内重名会让删除连删多条。与保存路径的 upsert 语义一致——同名
                // 保留后出现的条目。/ Uniqueness normalization (M1.2.1 audit tail):
                // name-based delete assumes unique names; duplicate entries in a
                // file would make one delete wipe several. Same upsert stance as
                // the save path - the later entry wins.
                for (int j = lib.size() - 1; j >= 0; j--) {
                    if (lib.get(j).name != null && lib.get(j).name.equals(name)) {
                        lib.remove(j);
                    }
                }
                lib.add(new Tpl(name, content));
            }
            if (lib.isEmpty()) {
                return false;
            }
            saveLibrary(lib);
            // 激活名若不在新库里，退到第一条。
            String active = getActiveName();
            boolean has = false;
            for (Tpl t : lib) {
                if (t.name != null && t.name.equals(active)) {
                    has = true;
                    break;
                }
            }
            if (!has) {
                setActiveName(lib.get(0).name);
            }
            return true;
        } catch (JSONException e) {
            return false;
        }
    }

    private void loadClipboard() {
        try {
            ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
            if (cm != null && cm.hasPrimaryClip()
                    && cm.getPrimaryClip().getItemCount() > 0) {
                CharSequence cs = cm.getPrimaryClip().getItemAt(0).coerceToText(this);
                if (cs != null && cs.length() > 0) {
                    input.setText(cs);
                    input.setSelection(cs.length());
                }
            }
        } catch (Exception ignored) {
        }
    }

    /** 标题：取首行并截到 40 字符（协议层另有 63 字节硬截断）。 */
    private static String firstLine(String s) {
        if (s == null) {
            return "";
        }
        String t = s.trim();
        int nl = t.indexOf('\n');
        if (nl >= 0) {
            t = t.substring(0, nl).trim();
        }
        int cr = t.indexOf('\r');
        if (cr >= 0) {
            t = t.substring(0, cr).trim();
        }
        if (t.length() > 40) {
            t = t.substring(0, 40);
        }
        return t;
    }

    private int dp(int v) {
        return (int) (v * getResources().getDisplayMetrics().density + 0.5f);
    }

    private void uiLog(final String s) {
        // gen22 快捷方式（shortcut）模式下不建界面，logView/logScroll 为 null：
        // 日志只进 logcat，不能碰 UI，否则一键推流会空指针崩掉。
        // gen22: in shortcut mode there is no layout, so logView/logScroll are null -
        // send the line to logcat only; touching the UI would crash one-tap push.
        if (sShortcutMode) {
            android.util.Log.d("PicoPush", s);
            return;
        }
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (logView == null) {
                    android.util.Log.d("PicoPush", s);
                    return;
                }
                logView.append(s + "\n");
                if (logScroll != null) {
                    logScroll.post(new Runnable() {
                        @Override
                        public void run() {
                            logScroll.fullScroll(View.FOCUS_DOWN);
                        }
                    });
                }
            }
        });
    }

    private void toast(final String s) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                Toast.makeText(MainActivity.this, s, Toast.LENGTH_LONG).show();
            }
        });
    }
}
