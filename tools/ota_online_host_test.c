/* SPDX-License-Identifier: Apache-2.0
 * 中文：本地化 OTA 批 26.11.25 —— 在线链路只下载到 TF 文件、哈希闸、取消、门与诊断打点、
 *       全程零 esp_ota_* 调用（flash 未被碰），以及与屏幕推送的并发互斥。
 * English: localized OTA downloads to a TF file behind a hash gate; cancellation, memory
 *          gates, on-screen diagnostics, zero esp_ota_* calls and display/flash exclusion.
 */
#include "common.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>          // 真实 FILE/fopen/fwrite/fclose/remove 声明（先于桩宏引入）
#include <pthread.h>
#include <sched.h>

/* 把文件 I/O 重定向到内存桩：不真写盘，「断言在写盘前」。
   stdio.h 已引入，产品码内的 #include <stdio.h> 因 guard 不再展开，
   故产品码里的 fopen/fwrite/fclose/remove 调用全部落到这几个桩。
   落点常量 PICO_OTA_UPDATE_PATH 来自产品码 include 的 ota_update.h（不复制第二个家）。*/
static FILE *test_fopen(const char *path, const char *mode);
static size_t test_fwrite(const void *buf, size_t size, size_t nmemb, FILE *f);
static int test_fclose(FILE *f);
static int test_remove(const char *path);
#define fopen(p, m)        test_fopen(p, m)
#define fwrite(b, s, n, f) test_fwrite(b, s, n, f)
#define fclose(f)          test_fclose(f)
#define remove(p)          test_remove(p)
#include "../main/ota_online.c"
#undef fopen
#undef fwrite
#undef fclose
#undef remove

// R3：layout_valid() 关系校验仍用分区表桩（两槽在 / 等大 / otadata 在），不硬编码地址。
static const esp_partition_t running={0x10000,0x580000},inactive={0x590000,0x580000},metadata={0xb10000,0x2000};
static const esp_partition_t small_slot={0x590000,0x570000};   // 两槽不等大反例
static const esp_app_desc_t current={.version="0.3.9-pico26.11.19",.project_name="Read_Pico"};
static uint8_t firmware[4096],written[4096];   // written = 下载落盘的 TF 文件镜像
static char feed[1600];
static size_t written_size,limit;
static int slot_ops;                            // 任何 esp_ota_* 调用数，恒 0（零槽硬判据）
static int fail_task;
static bool bad_layout,missing_ota0,missing_ota1,missing_otadata,unequal_slots,cancel_read,again;
static size_t drop_at;
static int drops,open_failures,range_requests,http_opens;
static bool wrong_range,ignored_range,feed_down;
static int feed_requests;
static wifi_ps_type_t wifi_ps;
static int ps_disabled,ps_restored,offline_polls;
static bool cancel_reconnect;
static void (*pending)(void*);
static int64_t now;
static size_t internal_free=131072,internal_block=65536,external_free=1048576,external_block=524288;
static size_t failed_allocation;
static pthread_mutex_t flash_display_mutex;
static _Thread_local unsigned lock_depth;
static atomic_bool display_active;
// 文件 I/O 桩状态
static char file_token;
static int file_open_calls,file_write_calls,file_close_calls,remove_calls;
static bool fopen_fail,fwrite_fail,fclose_fail,file_is_open;
static char last_fopen_path[128],last_remove_path[128];

struct FakeHttp {const uint8_t *data;size_t size,at,offset;bool firmware,is_feed;int (*event_handler)(esp_http_client_event_t*);void *user_data;};
void *heap_caps_malloc(size_t n,int c){(void)c;return n==failed_allocation?NULL:malloc(n);}
void *heap_caps_calloc(size_t n,size_t s,int c){(void)c;return calloc(n,s);}
void heap_caps_free(void*p){free(p);}
size_t heap_caps_get_free_size(unsigned c){return c&MALLOC_CAP_INTERNAL?internal_free:external_free;}
size_t heap_caps_get_largest_free_block(unsigned c){return c&MALLOC_CAP_INTERNAL?internal_block:external_block;}
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void){
 pthread_mutexattr_t attr;assert(!pthread_mutexattr_init(&attr));
 assert(!pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE));
 assert(!pthread_mutex_init(&flash_display_mutex,&attr));assert(!pthread_mutexattr_destroy(&attr));return (void*)3;
}
int xSemaphoreTakeRecursive(SemaphoreHandle_t s,uint32_t wait){assert(s==(void*)3&&wait==portMAX_DELAY);assert(!pthread_mutex_lock(&flash_display_mutex));++lock_depth;return pdTRUE;}
int xSemaphoreGiveRecursive(SemaphoreHandle_t s){assert(s==(void*)3&&lock_depth);--lock_depth;assert(!pthread_mutex_unlock(&flash_display_mutex));return pdTRUE;}
SemaphoreHandle_t xSemaphoreCreateMutex(void){return (void*)1;}
SemaphoreHandle_t xSemaphoreCreateBinary(void){return (void*)2;}
static void run(void){assert(pending);void(*f)(void*)=pending;pending=NULL;f(NULL);}
int xSemaphoreTake(SemaphoreHandle_t s,uint32_t wait){(void)wait;if(s==(void*)2&&pending)run();return 1;}
int xSemaphoreGive(SemaphoreHandle_t s){(void)s;return 1;}
int xTaskCreate(void(*f)(void*),const char*n,unsigned stack,void*a,unsigned p,void*t){(void)n;(void)a;(void)p;(void)t;assert(stack==12288);if(fail_task)return 0;pending=f;return pdPASS;}
void vTaskDelete(void*p){(void)p;}
void vTaskDelay(unsigned ms){now+=(int64_t)ms*1000;}
int64_t esp_timer_get_time(void){return now+=1000;}
const esp_app_desc_t *esp_app_get_description(void){return &current;}
const esp_partition_t *esp_partition_find_first(int type,int sub,const char*label){(void)label;
 if(bad_layout)return NULL;
 if(type==ESP_PARTITION_TYPE_DATA)return missing_otadata?NULL:&metadata;
 if(sub==ESP_PARTITION_SUBTYPE_APP_OTA_0)return missing_ota0?NULL:&running;
 if(missing_ota1)return NULL;
 return unequal_slots?&small_slot:&inactive;}
// 零槽：在线链路删净 esp_ota_* 后，这些桩一旦被调用 slot_ops 即 >0，用例断言恒 0 兜住。
const esp_partition_t *esp_ota_get_running_partition(void){++slot_ops;return &running;}
const esp_partition_t *esp_ota_get_next_update_partition(void*p){(void)p;++slot_ops;return &inactive;}
esp_err_t esp_ota_begin(const esp_partition_t*p,size_t n,esp_ota_handle_t*h){(void)p;(void)n;(void)h;++slot_ops;return ESP_OK;}
esp_err_t esp_ota_write(esp_ota_handle_t h,const void*p,size_t n){(void)h;(void)p;(void)n;++slot_ops;return ESP_OK;}
esp_err_t esp_ota_abort(esp_ota_handle_t h){(void)h;++slot_ops;return ESP_OK;}
esp_err_t esp_ota_end(esp_ota_handle_t h){(void)h;++slot_ops;return ESP_OK;}
esp_err_t esp_ota_set_boot_partition(const esp_partition_t*p){(void)p;++slot_ops;return ESP_OK;}
void read_pico_transfer_get_status(read_pico_transfer_status_t*s){*s=(read_pico_transfer_status_t){.mode=1,.network_ready=offline_polls==0};}
esp_err_t read_pico_transfer_get_saved_wifi(char*s,bool*b){strcpy(s,"test");*b=true;return ESP_OK;}
void read_pico_transfer_stop(void){}
esp_err_t read_pico_transfer_start(const read_pico_transfer_cfg_t*c){(void)c;return ESP_OK;}
void read_pico_transfer_service_poll(void){if(offline_polls){--offline_polls;if(cancel_reconnect)atomic_store(&s_cancel,true);}}
static bool rf_blocked;
bool read_pico_transfer_rf_blocked(void){return rf_blocked;}
esp_err_t esp_wifi_get_ps(wifi_ps_type_t*p){*p=wifi_ps;return ESP_OK;}
esp_err_t esp_wifi_set_ps(wifi_ps_type_t p){wifi_ps=p;if(p==WIFI_PS_NONE)++ps_disabled;else ++ps_restored;return ESP_OK;}
esp_err_t read_pico_transfer_sync_time_online(uint32_t*p){*p=1791200000;return ESP_OK;}
int esp_crt_bundle_attach(void*p){(void)p;return ESP_OK;}
// R4：自家 Pages 单源守卫（运行时侧）——本批任何 HTTP 请求都不得离开自家前缀。
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t*c){assert(c->disable_auto_redirect&&c->crt_bundle_attach&&c->timeout_ms==15000);
 static const char prefix[]="https://cere-hy.github.io/pico-plus/";
 assert(!strncmp(c->url,prefix,sizeof(prefix)-1));
 struct FakeHttp*h=calloc(1,sizeof(*h));h->firmware=!strstr(c->url,"update.json");h->is_feed=!h->firmware;
 if(h->is_feed)++feed_requests;
 h->data=h->firmware?firmware:(const uint8_t*)feed;h->size=h->firmware?sizeof(firmware):strlen(feed);h->event_handler=c->event_handler;h->user_data=c->user_data;return h;}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t h,const char*k,const char*v){if(!strcmp(k,"Range")){unsigned long at=0;assert(sscanf(v,"bytes=%lu-",&at)==1);h->offset=at;h->at=ignored_range?0:at;++range_requests;}return ESP_OK;}
esp_err_t esp_http_client_open(esp_http_client_handle_t h,int n){(void)n;++http_opens;assert(wifi_ps==WIFI_PS_NONE);if(h->is_feed&&feed_down)return ESP_FAIL;if(h->firmware&&open_failures){--open_failures;return ESP_FAIL;}return ESP_OK;}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t h){if(h->offset&&!ignored_range){char value[96];snprintf(value,sizeof(value),"bytes %lu-%lu/%lu",(unsigned long)(h->offset+(wrong_range?1:0)),(unsigned long)h->size-1,(unsigned long)h->size);esp_http_client_event_t event={.event_id=HTTP_EVENT_ON_HEADER,.user_data=h->user_data,.header_key="Content-Range",.header_value=value};assert(h->event_handler(&event)==ESP_OK);}return h->size-(ignored_range?0:h->offset);}
int esp_http_client_get_status_code(esp_http_client_handle_t h){return h->offset&&!ignored_range?206:200;}
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t h,int ms){(void)h;(void)ms;return ESP_OK;}
int esp_http_client_read(esp_http_client_handle_t h,char*p,int cap){if(again){again=false;return -ESP_ERR_HTTP_EAGAIN;}if(h->firmware&&drops&&h->at>=drop_at){--drops;offline_polls=5;return -1;}size_t end=h->firmware?limit:h->size;if(h->at>=end)return 0;size_t n=end-h->at;if(n>37)n=37;if(n>(size_t)cap)n=cap;memcpy(p,h->data+h->at,n);h->at+=n;if(cancel_read&&h->firmware&&h->at>500)atomic_store(&s_cancel,true);return (int)n;}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t h){return h->at==h->size;}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t h){free(h);return ESP_OK;}
int psa_crypto_init(void){return PSA_SUCCESS;}
int psa_hash_setup(psa_hash_operation_t*h,int alg){assert(alg==PSA_ALG_SHA_256);h->bytes=0;return PSA_SUCCESS;}
int psa_hash_update(psa_hash_operation_t*h,const void*p,size_t n){(void)p;h->bytes+=n;return PSA_SUCCESS;}
int psa_hash_finish(psa_hash_operation_t*h,uint8_t*p,size_t cap,size_t*n){assert(cap==32&&h->bytes==4096);memset(p,0,32);*n=32;return PSA_SUCCESS;}
int psa_hash_abort(psa_hash_operation_t*h){(void)h;return PSA_SUCCESS;}
// 内存 FILE 桩：只接纳 "wb"，落点必须等于 PICO_OTA_UPDATE_PATH（安装器优先命中路径）。
static FILE *test_fopen(const char *path,const char *mode){
 ++file_open_calls;snprintf(last_fopen_path,sizeof(last_fopen_path),"%s",path);
 assert(!strcmp(mode,"wb")&&!strcmp(path,PICO_OTA_UPDATE_PATH));
 if(fopen_fail)return NULL;
 file_is_open=true;written_size=0;return (FILE*)&file_token;
}
static size_t test_fwrite(const void*buf,size_t size,size_t nmemb,FILE*f){
 (void)size;(void)f;++file_write_calls;assert(file_is_open);
 if(fwrite_fail)return 0;
 size_t bytes=size*nmemb;assert(written_size+bytes<=sizeof(written));
 memcpy(written+written_size,buf,bytes);written_size+=bytes;return nmemb;
}
static int test_fclose(FILE*f){(void)f;++file_close_calls;assert(file_is_open);file_is_open=false;return fclose_fail?-1:0;}
static int test_remove(const char*path){++remove_calls;snprintf(last_remove_path,sizeof(last_remove_path),"%s",path);return 0;}

// 桩固件固定 4096 B、sha 全 0，与 psa_hash_finish 桩 / feed 同源。守卫用固定历史版本串测机制，不随产品版本 bump。
static void fixture(void){
 pico_online_cancel_join();
 internal_free=131072;internal_block=65536;external_free=1048576;external_block=524288;failed_allocation=0;
 memset(firmware,0,sizeof(firmware));
 esp_image_header_t header={.magic=0xe9,.chip_id=9};memcpy(firmware,&header,sizeof(header));
 esp_app_desc_t desc={.magic_word=ESP_APP_DESC_MAGIC_WORD,.version="0.3.9-pico26.11.20",.project_name="Read_Pico"};memcpy(firmware+32,&desc,sizeof(desc));
 snprintf(feed,sizeof(feed),"{\"schema\":1,\"version\":\"0.3.9-pico26.11.20\",\"project\":\"Read_Pico\",\"board\":\"RDP-G01-W\",\"layout\":\"pico-dual-4m-v1\",\"minimum_base_version\":\"0.3.9-pico26.11.19\",\"url\":\"https://cere-hy.github.io/pico-plus/firmware/Read_Pico.bin\",\"sha256\":\"%064d\",\"notes\":\"test\",\"size\":4096}",0);
 limit=4096;bad_layout=missing_ota0=missing_ota1=missing_otadata=unequal_slots=cancel_read=again=feed_down=false;
 feed_requests=0;written_size=0;slot_ops=0;fail_task=0;
 file_open_calls=file_write_calls=file_close_calls=remove_calls=0;fopen_fail=fwrite_fail=fclose_fail=file_is_open=false;
 last_fopen_path[0]=last_remove_path[0]=0;
 drop_at=0;drops=open_failures=range_requests=http_opens=0;wrong_range=ignored_range=cancel_reconnect=false;
 offline_polls=0;wifi_ps=WIFI_PS_MIN_MODEM;ps_disabled=ps_restored=0;
}
static void check_download(void){wifi_ps=WIFI_PS_MIN_MODEM;ps_disabled=ps_restored=0;assert(pico_online_check()==ESP_OK);assert(pico_online_busy());run();assert(s_status->state==PICO_UPDATE_AVAILABLE&&!pico_online_busy());assert(wifi_ps==WIFI_PS_MIN_MODEM&&ps_disabled==1&&ps_restored==1);assert(pico_online_download()==ESP_OK);run();assert(wifi_ps==WIFI_PS_MIN_MODEM&&ps_disabled==2&&ps_restored==2);}
// 两个真实线程交错取 display 递归锁（锁壳保留：display.c 有 24 处刷屏串行化依赖）。
static void *display_thread(void *arg){(void)arg;for(unsigned i=0;i<1000;++i){
 bool held=pico_online_display_begin();assert(held&&lock_depth==1);
 bool nested=pico_online_display_begin();assert(nested&&lock_depth==2);
 atomic_store(&display_active,true);sched_yield();atomic_store(&display_active,false);
 pico_online_display_end(nested);pico_online_display_end(held);assert(!lock_depth);
}return NULL;}
static void *flash_thread(void *arg){(void)arg;for(unsigned i=0;i<1000;++i){
 bool held=pico_online_display_begin();assert(held);sched_yield();pico_online_display_end(held);
}return NULL;}

int main(void){
 // R5：kill switch (-D=1) 单编一次，验证「不建任务、不联网、不碰槽、不碰文件」。
 if(PICO_ONLINE_UPDATES_DISABLED){
  assert(pico_online_updates_disabled());
  assert(pico_online_check()==ESP_ERR_NOT_SUPPORTED&&!pending&&!pico_online_busy());
  assert(pico_online_download()==ESP_ERR_NOT_SUPPORTED&&!pending&&!pico_online_busy());
  assert(slot_ops==0&&file_open_calls==0&&remove_calls==0);
  pico_online_cancel_join();
  puts("PASS: online OTA stays disabled; no worker, no feed request, no file and no boot slot change");
  return 0;
 }
 _Static_assert(sizeof(esp_image_header_t)==24&&sizeof(esp_app_desc_t)==256,"real header sizes");
 // R4：唯一自家源连续失败 → 明确失败提示；零槽、零文件。
 fixture();feed_down=true;wifi_ps=WIFI_PS_MIN_MODEM;assert(pico_online_check()==ESP_OK);run();assert(s_status->state==PICO_UPDATE_FAILED&&feed_requests==2&&slot_ops==0&&file_open_calls==0);feed_down=false;
 // R3 关系校验：两槽在 / 等大 / otadata 在才通过；缺任一或不等大 → 失败。
 fixture();assert(pico_online_check()==ESP_OK);run();assert(s_status->state==PICO_UPDATE_AVAILABLE);
 fixture();missing_ota0=true;assert(pico_online_check()==ESP_OK);run();assert(s_status->state==PICO_UPDATE_FAILED&&slot_ops==0);
 fixture();missing_ota1=true;assert(pico_online_check()==ESP_OK);run();assert(s_status->state==PICO_UPDATE_FAILED&&slot_ops==0);
 fixture();missing_otadata=true;assert(pico_online_check()==ESP_OK);run();assert(s_status->state==PICO_UPDATE_FAILED&&slot_ops==0);
 fixture();unequal_slots=true;assert(pico_online_check()==ESP_OK);run();assert(s_status->state==PICO_UPDATE_FAILED&&slot_ops==0);
 // R6：compare==0 → LATEST，天然同版门，不下载不写文件不碰槽。
 fixture();char *same=strstr(feed,"0.3.9-pico26.11.20");assert(same);same[16]='1';same[17]='9';
 assert(pico_online_check()==ESP_OK);run();assert(s_status->state==PICO_UPDATE_LATEST&&file_open_calls==0&&slot_ops==0);
 // ① 成功下载：EAGAIN 重试 → READY + 落盘 4096B 逐字命中 + 零槽 + 落点=安装器入口 + 成功不清理。
 fixture();again=true;check_download();
 assert(s_status->state==PICO_UPDATE_READY);
 assert(written_size==4096&&!memcmp(firmware,written,4096));
 assert(slot_ops==0);
 assert(file_open_calls==1&&file_close_calls==1&&!file_is_open&&remove_calls==0);
 assert(!strcmp(last_fopen_path,PICO_OTA_UPDATE_PATH));
 assert(!strcmp(s_status->message,"下载完成，是否立即安装？"));
 // ② 断流 limit=1024：重试 3 次 Range → FAILED + 残留清理 + 零槽。
 fixture();limit=1024;check_download();
 assert(s_status->state==PICO_UPDATE_FAILED&&remove_calls==1&&slot_ops==0&&range_requests==3);
 assert(!strcmp(last_remove_path,PICO_OTA_UPDATE_PATH));range_requests=0;
 // ③ 续传 drops：HTTP 断流重连，文件只 fopen 一次、顺序追加，仍逐字完整。
 fixture();drop_at=100;drops=1;check_download();assert(s_status->state==PICO_UPDATE_READY&&written_size==4096&&!memcmp(firmware,written,4096)&&file_open_calls==1&&range_requests==1&&slot_ops==0);range_requests=0;
 fixture();drop_at=1000;drops=2;check_download();assert(s_status->state==PICO_UPDATE_READY&&written_size==4096&&!memcmp(firmware,written,4096)&&file_open_calls==1&&range_requests==2&&slot_ops==0);range_requests=0;
 // ④ 取消重连 / 读中途取消 → CANCELLED + 清理 + 零槽。
 fixture();drop_at=1000;drops=1;cancel_reconnect=true;check_download();assert(s_status->state==PICO_UPDATE_CANCELLED&&remove_calls==1&&slot_ops==0);cancel_reconnect=false;offline_polls=0;
 fixture();cancel_read=true;check_download();assert(s_status->state==PICO_UPDATE_CANCELLED&&remove_calls==1&&slot_ops==0);
 // ⑤ open 失败重试 / 错误 Range / 忽略 Range / 重试耗尽 → FAILED + 清理 + 零槽。
 fixture();open_failures=2;check_download();assert(s_status->state==PICO_UPDATE_READY&&written_size==4096&&slot_ops==0);
 fixture();drop_at=1000;drops=1;wrong_range=true;check_download();assert(s_status->state==PICO_UPDATE_FAILED&&remove_calls==1&&slot_ops==0);wrong_range=false;range_requests=0;
 fixture();drop_at=1000;drops=1;ignored_range=true;check_download();assert(s_status->state==PICO_UPDATE_FAILED&&remove_calls==1&&slot_ops==0);ignored_range=false;range_requests=0;
 fixture();drop_at=1000;drops=20;check_download();assert(s_status->state==PICO_UPDATE_FAILED&&remove_calls==1&&slot_ops==0&&range_requests==3);drops=range_requests=0;
 // ⑥ 哈希闸：清单 sha 不符 → FAILED + 清理 + 零槽（校验先于「可安装」状态）。
 fixture();char *sha=strstr(feed,"0000000000");assert(sha);sha[0]='1';check_download();assert(s_status->state==PICO_UPDATE_FAILED&&remove_calls==1&&slot_ops==0);
 // ⑦ 头/项目/镜像健康校验已移交 ota_update.c::pico_ota_inspect()：在线侧不再有头拒绝分支。
 //    桩 hash 是固定值、不模拟内容相关，故此例只证「在线侧不因头字节篡改而额外拒绝」。
 fixture();firmware[80]='X';check_download();assert(s_status->state==PICO_UPDATE_READY&&written_size==4096&&slot_ops==0);
 // ⑧ 无 TF 卡（fopen 失败）→ 明确文案、不写、不碰槽、不清理（文件从未建立）。
 fixture();fopen_fail=true;check_download();assert(s_status->state==PICO_UPDATE_FAILED&&!strcmp(s_status->message,"需要 TF 卡，请插入后重试")&&file_write_calls==0&&remove_calls==0&&slot_ops==0);
 // ⑨ fwrite / fclose 失败 → FAILED + 清理 + 零槽。
 fixture();fwrite_fail=true;check_download();assert(s_status->state==PICO_UPDATE_FAILED&&remove_calls==1&&slot_ops==0);
 fixture();fclose_fail=true;check_download();assert(s_status->state==PICO_UPDATE_FAILED&&remove_calls==1&&slot_ops==0);
 // ⑩ 分配失败（buffer 现放 SPIRAM）→ 门诊断 FAIL、不建文件、不碰槽。
 fixture();failed_allocation=4096;check_download();assert(s_status->state==PICO_UPDATE_FAILED&&file_open_calls==0&&remove_calls==0&&slot_ops==0);
 // ⑪ 门重校准：两门 16K 代入诊断 24 的点击瞬间 26K 应放行；旧 32K 门在同一水位拒绝。
 //    旧门断言必须放在低水位才成立：fixture 默认 128K 高水位下新旧门都过（先验对照，
 //    证明两门的差异只在低水位显形，而非「旧门恒拒」）。首跑曾在高水位断言旧门拒 → 假红，
 //    按「报绿前先证伪自己」修正，产品码一字未动。
 fixture();assert(memory_available(16384,16384,524288)&&memory_available(32768,16384,524288));
 fixture();internal_free=26624;
 assert(memory_available(16384,8192,262144)&&memory_available(16384,16384,524288));
 assert(!memory_available(32768,8192,262144)&&!memory_available(32768,16384,524288));
 // ⑫ 门 FAIL 诊断四数：把 23403.jpg 真机读数（内26K/块16K/PS4M/PS块4352K）喂进桩，
 //    验证数值前置、按序、96 字节内、前缀 <540px（截尾只可能删中文理由）。
 fixture();internal_free=27000;internal_block=16383;external_free=4u*1024u*1024u;external_block=4352u*1024u;
 assert(pico_online_check()==ESP_ERR_NO_MEM&&!pending&&!pico_online_busy()&&slot_ops==0);
 {
  const char *msg=s_status->message;
  const char *p1=strstr(msg,"内26K"),*p2=strstr(msg,"块15K"),*p3=strstr(msg,"PS4M"),*p4=strstr(msg,"PS块4352K");
  const char *reason=strstr(msg,"请退出传书后重试");
  assert(p1&&p2&&p3&&p4&&reason);
  assert(p1<p2&&p2<p3&&p3<p4&&p4<reason);
  assert(strlen(msg)<sizeof(s_status->message));
  size_t prefix=(size_t)(reason-msg);unsigned w=0;
  for(size_t i=0;i<prefix;++i){unsigned char c=(unsigned char)msg[i];if((c&0xc0)!=0x80)w+=c<128?10:20;}
  assert(w<540);
 }
 // ⑬ 门其余三维 / 第四维：任一不过 → 同步门 NO_MEM，不建任务、不碰槽。
 fixture();internal_free=16383;assert(pico_online_check()==ESP_ERR_NO_MEM&&!pending&&slot_ops==0);
 fixture();internal_block=16383;assert(pico_online_check()==ESP_ERR_NO_MEM&&!pending&&slot_ops==0);
 fixture();external_free=524287;assert(pico_online_check()==ESP_ERR_NO_MEM&&!pending&&slot_ops==0);
 fixture();external_block=32767;assert(pico_online_check()==ESP_ERR_NO_MEM&&!pending&&slot_ops==0);
 // ⑭ worker 异步门（online 起 WiFi 后）FAIL：不联网、不建文件、不碰槽。
 fixture();assert(pico_online_check()==ESP_OK);internal_free=16383;int opens=http_opens;run();assert(s_status->state==PICO_UPDATE_FAILED&&!pico_online_busy()&&http_opens==opens&&file_open_calls==0&&slot_ops==0&&wifi_ps==WIFI_PS_MIN_MODEM);
 // ⑮ 并发：display 递归锁在两线程交错下互斥、无死锁。
 fixture();pthread_t display_job,flash_job;
 assert(!pthread_create(&display_job,NULL,display_thread,NULL));assert(!pthread_create(&flash_job,NULL,flash_thread,NULL));
 assert(!pthread_join(display_job,NULL));assert(!pthread_join(flash_job,NULL));assert(!lock_depth);
 assert(!pthread_mutex_destroy(&flash_display_mutex));
 free(s_status);s_status=NULL;
 puts("PASS: localized OTA downloads to TF file behind hash gate; zero esp_ota_* calls (flash untouched), RF power-save restored, Range resume, cancellation and residual cleanup");
 puts("PASS: 16K download gates pass the 26K click-time reading while old 32K gates reject; 4 on-screen diagnostics stay numeric-first, ordered, <96B and <540px prefix");
 puts("PASS: no-TF-card, write/close/alloc failures, wrong/ignored ranges, single self-hosted feed, relational layout check, pico-date same-version LATEST, display lock exclusive on 2 threads");
}
