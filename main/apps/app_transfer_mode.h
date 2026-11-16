#pragma once

// The file manager uses this before opening Transfer to enter USB storage directly.
void app_transfer_request_usb_start(void);

// File manager opens the selected transfer mode directly, without the method picker.
void app_transfer_request_wifi_upload(void);
void app_transfer_request_hotspot_start(void);

// Settings uses this before opening Transfer to enter WiFi provisioning directly.
void app_transfer_request_wifi_setup(void);

// 文件管理打开 WiFi/热点方式选择，选择前不启动传输。
// File manager opens a WiFi/hotspot chooser before starting either service.
void app_transfer_request_method_picker(void);

// 二期 M1：打开传书页并直接进入手机 BLE 推送武装态。
// Phase 2 M1: open the transfer page straight into the phone BLE push arm.
void app_transfer_request_push(void);

// 26.9.30 commit 2：提交段执行体。app_loop 每帧在 push_sink_poll() 之后取走请求
// 并调到这里；四拍（rename/remove）只在主循环帧间跑，绝不上 BLE 回调栈。
// The commit executor, called by app_loop right after push_sink_poll(); the four
// beats run between frames and never on the BLE callback stack.
void transfer_pump_commit(void);
