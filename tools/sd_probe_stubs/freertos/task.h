/* SPDX-License-Identifier: Apache-2.0
 * SD 探测主机测试任务接口。/ Host SD-probe task stub.
 */
#pragma once

#include "freertos/FreeRTOS.h"

BaseType_t xTaskCreate(void (*fn)(void*), const char* name, int stack,
                       void* arg, int priority, void* handle);
void vTaskDelay(int ticks);
// 26.9.20 S9(b)：挂载后的空间统计在后台跑到一半就把自己降到最低优先级。
void vTaskPrioritySet(void* handle, int priority);
void vTaskDelete(void* handle);
// 26.9.21 S14(b)：分片扫完一片就让一拍，界面才有机会插进来读目录。
// 26.9.21 S14(b): yield after every FAT slice so the UI can read a directory in between.
void taskYIELD(void);
// 26.9.23 S24：512 B 缓冲搬上栈后，余量必须打印出来（<256 判方案自败）。
// 26.9.23 S24: with the 512 B buffer on the stack the high-water mark must be printed
// (a margin below 256 fails the plan by its own terms).
typedef unsigned UBaseType_t;
UBaseType_t uxTaskGetStackHighWaterMark(void* handle);
