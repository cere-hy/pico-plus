/* SPDX-License-Identifier: Apache-2.0
 * 宿主测试的日志桩：暗号只在真机串口有意义，宿主侧丢弃。
 * Log stub for host tests: the marker only matters on the device UART.
 */
#pragma once

#define ESP_LOGI(tag, fmt, ...) do { (void)(tag); } while (0)
#define ESP_LOGW(tag, fmt, ...) do { (void)(tag); } while (0)
#define ESP_LOGE(tag, fmt, ...) do { (void)(tag); } while (0)
