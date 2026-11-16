/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：公开升级清单的严格解析与版本比较。/ English: strict public release parsing and version comparison.
 */
#include "ota_online.h"
#include "cJSON.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 比较键只取 fork 的 -picoYY.MM.DD 日期后缀；前缀 X.Y.Z 仅做语法校验、不参与排序。
// 无 pico 后缀（纯 X.Y.Z 或历史 -rcN）视为最旧：parts 全 0，安全默认不触发幻影升级。
// / Only the fork's pico date drives ordering; the X.Y.Z prefix is syntax only.
// / No pico suffix means oldest (all-zero key): a stale manifest cannot trigger an upgrade.
static bool version_parts(const char *s, unsigned parts[4]) {
    if (!s || !*s || strlen(s) > 32) return false;
    memset(parts, 0, 4 * sizeof(*parts));
    for (unsigned i = 0; i < 3; ++i) {
        if (!isdigit((unsigned char)*s)) return false;
        char *end; unsigned long n = strtoul(s, &end, 10);
        if (n > 65535) return false;
        s = end;
        if (i < 2 && *s++ != '.') return false;
    }
    if (!*s) return true;                       // 纯 X.Y.Z：无 pico 键，最旧
    if (!strncmp(s, "-rc", 3)) {               // 历史 rc 语法仍合法，同样最旧
        s += 3;
        if (!isdigit((unsigned char)*s)) return false;
        char *end; unsigned long n = strtoul(s, &end, 10);
        return !*end && n <= 65535;
    }
    // fork 的 -picoYY.MM.DD 才是比较键：逐段数值比较，parts[3]=1 标记「有 pico 键」。
    // / The fork's pico date is the only ordering key; parts[3]=1 marks a real key.
    if (strncmp(s, "-pico", 5)) return false;
    s += 5;
    for (unsigned i = 0; i < 3; ++i) {
        if (!isdigit((unsigned char)*s)) return false;
        char *end; unsigned long n = strtoul(s, &end, 10);
        if (n > 65535) return false;
        parts[i] = (unsigned)n; s = end;
        if (i < 2 && *s++ != '.') return false;
    }
    if (*s) return false;
    parts[3] = 1;                              // 有 pico 键 > 任何无键版本
    return true;
}
int pico_version_compare(const char *a, const char *b) {
    unsigned left[4], right[4];
    if (!version_parts(a, left) || !version_parts(b, right)) return 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (left[i] < right[i]) return -1;
        if (left[i] > right[i]) return 1;
    }
    return 0;
}
bool pico_release_url_valid(const char *url) {
    // 升级源限定自家 Pages 单前缀，避免重定向泄漏或被替换为任意下载源；
    // 原厂两前缀已废——原厂镜像覆盖词典 / 生词本的风险从源头归零。
    // / Only our own Pages prefix; stock origins are gone so a stock image can never be fetched.
    static const char *const prefixes[] = {
        "https://cere-hy.github.io/pico-plus/firmware/"
    };
    if (!url || strlen(url) >= 256) return false;
    const char *name = NULL;
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        size_t n = strlen(prefixes[i]);
        if (!strncmp(url, prefixes[i], n)) { name = url + n; break; }
    }
    if (!name) return false;
    if (!*name || strstr(name, "..") || strchr(name, '/') || strchr(name, '\\')) return false;
    for (const char *p = name; *p; ++p)
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_' && *p != '.') return false;
    size_t n = strlen(name);
    return n > 4 && !strcmp(name + n - 4, ".bin");
}
static bool field(const cJSON *root, const char *key, char *out, size_t cap) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(v) || !v->valuestring || !*v->valuestring || strlen(v->valuestring) >= cap) return false;
    memcpy(out, v->valuestring, strlen(v->valuestring) + 1);
    return true;
}
esp_err_t pico_release_parse(const char *json, size_t length, pico_release_t *release) {
    if (!json || !release || !length || length > 8192 || memchr(json, 0, length)) return ESP_ERR_INVALID_ARG;
    memset(release, 0, sizeof(*release));
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, length, &end, false);
    if (!root || !cJSON_IsObject(root)) { cJSON_Delete(root); return ESP_ERR_INVALID_RESPONSE; }
    // 拒绝重复字段与尾随载荷。/ Reject duplicate fields and trailing payloads.
    bool valid = true;
    for (const cJSON *a = root->child; a; a = a->next)
        for (const cJSON *b = a->next; b; b = b->next)
            if (a->string && b->string && !strcmp(a->string, b->string)) valid = false;
    while (end && end < json + length && isspace((unsigned char)*end)) ++end;
    if (!end || end != json + length) valid = false;
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(root, "size");
    unsigned version[4], base[4];
    valid = valid && cJSON_IsNumber(schema) && schema->valuedouble == 1 &&
        field(root, "version", release->version, sizeof(release->version)) &&
        field(root, "project", release->project, sizeof(release->project)) &&
        field(root, "board", release->board, sizeof(release->board)) &&
        field(root, "layout", release->layout, sizeof(release->layout)) &&
        field(root, "minimum_base_version", release->minimum_base_version, sizeof(release->minimum_base_version)) &&
        field(root, "url", release->url, sizeof(release->url)) &&
        field(root, "sha256", release->sha256, sizeof(release->sha256)) &&
        field(root, "notes", release->notes, sizeof(release->notes)) &&
        cJSON_IsNumber(size) && size->valuedouble >= 288 && size->valuedouble <= 0x580000 &&
        size->valuedouble == (uint32_t)size->valuedouble &&
        version_parts(release->version, version) && version_parts(release->minimum_base_version, base) &&
        !strcmp(release->board, PICO_OTA_BOARD) && !strcmp(release->layout, PICO_OTA_LAYOUT) &&
        pico_release_url_valid(release->url) && strlen(release->sha256) == 64;
    if (valid) {
        for (unsigned i = 0; i < 64; ++i)
            if (!isxdigit((unsigned char)release->sha256[i])) valid = false;
        release->size = (uint32_t)size->valuedouble;
    }
    cJSON_Delete(root);
    if (!valid) { memset(release, 0, sizeof(*release)); return ESP_ERR_INVALID_RESPONSE; }
    return ESP_OK;
}
