/* 中文：调度测试替身。/ English: Scheduler test shim. */
#pragma once
#include "common.h"

#ifndef ESP_LOGW
#define ESP_LOGW(...) ((void)0)
#endif
#ifndef ESP_LOGI
#define ESP_LOGI(...) ((void)0)
#endif
#ifndef ESP_LOGE
#define ESP_LOGE(...) ((void)0)
#endif
