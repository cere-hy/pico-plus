/* SPDX-License-Identifier: Apache-2.0
 * 中文：升级元数据边界、pico 日期版本比较与自家 Pages 单前缀白名单。
 * English: Release bounds, pico-date version ordering and the single self-hosted prefix. */
#include "ota_online.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
// 与 26.11.20 判据定稿 update.json 模板同源；url 落在自家单前缀内、name 无斜杠。
static const char *valid =
    "{\"schema\":1,\"version\":\"0.3.9-pico26.11.20\",\"project\":\"Read_Pico\","
    "\"board\":\"RDP-G01-W\",\"layout\":\"pico-dual-4m-v1\","
    "\"minimum_base_version\":\"0.3.9-pico26.11.19\","
    "\"url\":\"https://cere-hy.github.io/pico-plus/firmware/Read_Pico.bin\","
    "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
    "\"notes\":\"Test release\",\"size\":4217088}";
int main(void) {
 pico_release_t release;
 assert(pico_release_parse(valid,strlen(valid),&release)==ESP_OK);
 assert(release.size==4217088 && !strcmp(release.version,"0.3.9-pico26.11.20"));

 // R1：pico 日期后缀逐段数值比较；前缀 X.Y.Z 不参与排序。
 assert(pico_version_compare("0.3.9-pico26.11.20","0.3.9-pico26.11.19")>0);
 assert(pico_version_compare("0.3.9-pico26.11.19","0.3.9-pico26.11.20")<0);
 assert(pico_version_compare("0.3.9-pico27.01.01","0.3.9-pico26.12.31")>0);
 assert(pico_version_compare("0.3.9-pico26.12.01","0.3.9-pico26.11.30")>0);
 assert(pico_version_compare("9.9.9-pico26.11.20","0.0.1-pico26.11.20")==0);
 assert(pico_version_compare("0.3.9-pico26.11.20","0.3.9-pico26.11.20")==0);  // D2 同版
 // 无 pico 后缀（纯 X.Y.Z / 历史 -rcN）视为最旧，且彼此相等 → 不触发幻影升级。
 assert(pico_version_compare("0.3.9-pico26.11.20","0.3.9")>0);
 assert(pico_version_compare("0.3.9-pico26.11.20","0.3.3-rc79")>0);
 assert(pico_version_compare("0.3.3","0.4.0-rc1")==0);
 assert(pico_version_compare("0.3.9","0.3.9-pico26.11.19")<0);
 assert(pico_version_compare("garbage","0.3.9-pico26.11.20")==0);
 // 门修正批编排：24 孪生靶子 > 23 正式（D1 发现新版本）；收尾翻清单指 23 后不弹升级；D2 同版。
 assert(pico_version_compare("0.3.9-pico26.11.24","0.3.9-pico26.11.23")>0);
 assert(pico_version_compare("0.3.9-pico26.11.23","0.3.9-pico26.11.24")<0);
 assert(pico_version_compare("0.3.9-pico26.11.23","0.3.9-pico26.11.23")==0);
 // 本地化 OTA 批 25/26：设备刷 25 → 清单指 26（D1 发现新版本）；
 // 收尾翻清单指 25 而设备留 26 时 compare(25,26)<0 → 不弹幻影升级；D2 同版。
 assert(pico_version_compare("0.3.9-pico26.11.26","0.3.9-pico26.11.25")>0);
 assert(pico_version_compare("0.3.9-pico26.11.25","0.3.9-pico26.11.26")<0);
 assert(pico_version_compare("0.3.9-pico26.11.25","0.3.9-pico26.11.25")==0);

 // R4：白名单收缩为自家单前缀；原厂两前缀一律拒绝；name 禁斜杠 / 查询 / .. / 非 .bin。
 assert(pico_release_url_valid("https://cere-hy.github.io/pico-plus/firmware/Read_Pico.bin"));
 assert(pico_release_url_valid("https://cere-hy.github.io/pico-plus/firmware/Pico-update.bin"));
 assert(!pico_release_url_valid("https://kiikoread.com/Pico-update-0.3.3-rc90.bin"));
 assert(!pico_release_url_valid("https://wegooo-cell.github.io/read-pico-reader/a.bin"));
 assert(!pico_release_url_valid("https://kiikoread.com.evil/a.bin"));
 assert(!pico_release_url_valid("https://kiikoread.com@evil/a.bin"));
 assert(!pico_release_url_valid("https://kiikoread.com:8443/a.bin"));
 assert(!pico_release_url_valid("https://cere-hy.github.io/pico-plus/firmware/../a.bin"));
 assert(!pico_release_url_valid("https://cere-hy.github.io/pico-plus/firmware/sub/a.bin"));
 assert(!pico_release_url_valid("https://cere-hy.github.io/pico-plus/firmware/a.bin?url=evil"));
 assert(!pico_release_url_valid("http://cere-hy.github.io/pico-plus/firmware/a.bin"));
 assert(!pico_release_url_valid("https://cere-hy.github.io.evil/pico-plus/firmware/a.bin"));
 assert(!pico_release_url_valid("https://cere-hy.github.io/pico-plus/firmware/a.txt"));
 assert(!pico_release_url_valid("https://cere-hy.github.io/pico-plus/firmware/.bin"));

 // 字段清单 / 长度 / 重复字段 / 尾随载荷 / sha256 hex / board 严格校验保留。
 assert(pico_release_parse(valid,strlen(valid)+1,&release)!=ESP_OK);
 assert(pico_release_parse("{}",2,&release)!=ESP_OK);
 char altered[2048]; char *p;
 snprintf(altered,sizeof(altered),"%s junk",valid);
 assert(pico_release_parse(altered,strlen(altered),&release)!=ESP_OK);
 snprintf(altered,sizeof(altered),"{\"schema\":1,%s",valid+1);
 assert(pico_release_parse(altered,strlen(altered),&release)!=ESP_OK);
 // R2：静态上限 0x580000 边界通过、越界拒绝。
 strcpy(altered,valid); p=strstr(altered,"4217088"); assert(p); memcpy(p,"5767168",7);
 assert(pico_release_parse(altered,strlen(altered),&release)==ESP_OK && release.size==5767168);
 strcpy(altered,valid); p=strstr(altered,"4217088"); assert(p); memcpy(p,"5767169",7);
 assert(pico_release_parse(altered,strlen(altered),&release)!=ESP_OK);
 strcpy(altered,valid); p=strstr(altered,"4217088"); assert(p); memcpy(p,"9999999",7);
 assert(pico_release_parse(altered,strlen(altered),&release)!=ESP_OK);
 strcpy(altered,valid); p=strstr(altered,"RDP-G01-W"); assert(p); p[0]='X';
 assert(pico_release_parse(altered,strlen(altered),&release)!=ESP_OK);
 strcpy(altered,valid); p=strstr(altered,"0123456789abcdef"); assert(p); p[0]='z';
 assert(pico_release_parse(altered,strlen(altered),&release)!=ESP_OK);
 puts("PASS: pico-date compare, no-suffix oldest, same-version, self-hosted single prefix, 0x580000 size bound, schema/hash/board, duplicate/trailing payload");
}
