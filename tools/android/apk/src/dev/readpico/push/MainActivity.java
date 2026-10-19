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
 *   订阅 5f13 通知 -> 写 BEGIN 到 5f12 -> 等 READY -> DATA×N 写 5f11（WWR，
 *   逐包等 ATT 回执） -> 写 END 到 5f12 -> 等 DONE/ERR。
 *   写入方式实证修正记录（gen12）：固件 5f11/5f12 特性位一直是 F_WRITE（WWR），
 *   F_WRITE_NO_RSP 从未在固件出现；ATT 逐包回执是第一道防线，设备侧 RESUME
 *   是第二道防线。
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
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
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

import java.nio.charset.Charset;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;

public class MainActivity extends Activity {

    private static final Charset UTF8 = Charset.forName("UTF-8");
    private static final int REQ_PERMS = 1;
    private static final int REQ_ENABLE_BT = 2;
    private static final long SCAN_TIMEOUT_MS = 12000;
    private static final long CONNECT_TIMEOUT_MS = 20000;
    private static final long STEP_TIMEOUT_MS = 20000;
    private static final long FINAL_TIMEOUT_MS = 60000; // END 后设备要校验+落盘，放宽
    private static final int SESSION_ID = 1;

    private EditText input;
    private CheckBox wordCard;
    private TextView logView;
    private ScrollView logScroll;

    private BluetoothAdapter adapter;
    private BluetoothGatt gatt;

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

    /* ---- BLE 回调 ---- */

    private final BluetoothGattCallback gattCb = new BluetoothGattCallback() {
        @Override
        public void onConnectionStateChange(BluetoothGatt g, int status, int newState) {
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
            synchronized (lock) {
                byte[] v = c.getValue();
                lastStatus = (v == null) ? null : v.clone();
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
                synchronized (lock) {
                    if (foundAddr == null) {
                        foundAddr = result.getDevice().getAddress();
                        foundToken = token.intValue();
                        foundName = name;
                        scanDone = true;
                        lock.notifyAll();
                    }
                }
            }
        }

        @Override
        public void onScanFailed(int errorCode) {
            synchronized (lock) {
                if (foundAddr == null && failReason == null) {
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
        super.onCreate(savedInstanceState);

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
    }

    @Override
    protected void onDestroy() {
        closeQuietly();
        super.onDestroy();
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
        new Thread(new Runnable() {
            @Override
            public void run() {
                doPush(text, isWord);
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

            byte[] begin = PushProto.encodeBegin(body.length, PushProto.crc32(body),
                    SESSION_ID, foundToken, ptype, title);
            uiLog("发送 BEGIN（" + begin.length + " 字节，标题「" + title + "」）……");
            if (!writeChar(cCtrl, begin)) {
                return;
            }
            byte[] st = waitStatus(new int[] { PushProto.ST_READY, PushProto.ST_ERR }, STEP_TIMEOUT_MS);
            if (st == null) {
                return;
            }
            if (PushProto.statusOp(st) == PushProto.ST_ERR) {
                uiLog("设备拒绝：" + PushProto.errText(PushProto.statusBody(st)[0] & 0xFF));
                return;
            }
            long sid = PushProto.readySessionId(st);
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

    private boolean scanForDevice() {
        BluetoothLeScanner scanner = adapter.getBluetoothLeScanner();
        if (scanner == null) {
            failReason = "拿不到 BLE 扫描器（蓝牙未开？）";
            return false;
        }
        ScanSettings settings = new ScanSettings.Builder()
                .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build();
        uiLog("扫描 PicoPush-XXXX 广播（最多 12 秒）……");
        synchronized (lock) {
            scanDone = false;
            foundAddr = null;
        }
        scanner.startScan(null, settings, scanCb);
        long deadline = System.currentTimeMillis() + SCAN_TIMEOUT_MS;
        synchronized (lock) {
            while (!scanDone && failReason == null) {
                long rem = deadline - System.currentTimeMillis();
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
        if (foundAddr != null) {
            return true;
        }
        if (failReason == null) {
            failReason = "12 秒内未发现 PicoPush-XXXX 广播（请先在墨水屏开启「接收推送」）";
        }
        return false;
    }

    /* ---- 权限 ---- */

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

    private void addIfMissing(List<String> need, String perm) {
        if (checkSelfPermission(perm) != PackageManager.PERMISSION_GRANTED) {
            need.add(perm);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
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
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                logView.append(s + "\n");
                logScroll.post(new Runnable() {
                    @Override
                    public void run() {
                        logScroll.fullScroll(View.FOCUS_DOWN);
                    }
                });
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
