#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <string.h>
#include <errno.h>
#include "board/sd_command_history.h"
#define CONFIG_NUWAX_SD_COMMAND_HISTORY_ENABLE 1
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define SD_OCR_SDHC_CAP (1U<<30)
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) assert(!(*(p))++)
#define portEXIT_CRITICAL(p) assert((*(p))--==1)
static uintptr_t task=1;
static uintptr_t xTaskGetCurrentTaskHandle(void){return task;}
typedef struct {struct {size_t sector_size,capacity;}csd;uint32_t ocr;} sdmmc_card_t;
static unsigned allocations,log_lines;static bool alloc_fail;
static char logs[16384];
static void *heap_caps_calloc(size_t n,size_t b,unsigned caps){assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));if(alloc_fail)return NULL;++allocations;return calloc(n,b);}
static void log_line(const char*tag,const char*fmt,...){(void)tag;size_t at=strlen(logs);va_list ap;va_start(ap,fmt);int n=vsnprintf(logs+at,sizeof(logs)-at,fmt,ap);va_end(ap);assert(n>0&&(size_t)n+at+2<sizeof(logs));strcat(logs,"\n");++log_lines;}
#define ESP_LOGW(...) log_line(__VA_ARGS__)
#define ESP_LOGE(...) log_line(__VA_ARGS__)
static const char *esp_err_to_name(int e){return e?"ERR":"ESP_OK";}
static uint32_t esp_rom_crc32_le(uint32_t c,const void*v,size_t n){const uint8_t*p=v;c=~c;while(n--){c^=*p++;for(unsigned b=0;b<8;b++)c=c>>1^((c&1)?0xedb88320U:0);}return ~c;}
#include "sd_command_history_actual.inc"
static sdmmc_card_t card={.csd={512,10000},.ocr=SD_OCR_SDHC_CAP},alias,foreign;
static uint8_t source[1024],bounce[512];
static bool begin(bool write,bool read){return nuwax_sd_command_history_begin(&card,&alias,source,100,2,write,read,s_history->generation,21,true);}
static void command(unsigned op,unsigned arg,int error,const void*p,size_t n){errno=EACCES;nuwax_sd_command_history_command(&card,op,arg,0x900,error,p,n);assert(errno==EACCES);}
int main(void){
 alloc_fail=true;assert(!nuwax_sd_command_history_init()&&!s_history);alloc_fail=false;
 assert(nuwax_sd_command_history_init()&&allocations==1);unsigned start_logs=log_lines;
 for(unsigned n=0;n<sizeof(source);n++)source[n]=(uint8_t)(n*31+5);memcpy(bounce,source+512,512);
 nuwax_sd_command_history_arm(1);assert(!nuwax_sd_command_history_active());
 assert(begin(false,false));command(17,101,0,bounce,512);nuwax_sd_command_history_end();
 assert(s_history->count==1&&!s_history->events[0].source_valid);
 nuwax_sd_command_history_arm(1);assert(begin(true,false));assert(!begin(true,false)&&s_history->conflicts==1);
 task=2;command(25,100,0,source,1024);nuwax_sd_command_history_end();assert(!s_history->count);task=1;
 nuwax_sd_command_history_command(&foreign,25,100,0x900,0,source,1024);assert(!s_history->count);
 command(24,101,0,bounce,512);command(13,1,0,NULL,0);command(13,1,-1,NULL,0);
 command_event_t *e=&s_history->events[0];assert(e->lba==101&&e->operation==21&&e->source_valid&&e->range_valid);
 assert(e->source_crc==esp_rom_crc32_le(0,source+512,512)&&e->data_crc==e->source_crc&&e->polls==2&&e->last_error==-1);
 command(17,101,0,bounce,512);command(13,1,-7,NULL,0);assert(s_history->count==1&&e->polls==2&&e->last_error==-1);
 nuwax_sd_command_history_end();assert(!nuwax_sd_command_history_active()&&!s_history->scope.source&&!s_history->scope.card);
 assert(begin(false,true));command(17,101,0,bounce,512);command(13,1,0,NULL,0);nuwax_sd_command_history_end();
 assert(s_history->count==2&&!s_history->events[1].source_valid&&s_history->events[1].polls==1);
 assert(log_lines==start_logs);nuwax_sd_command_history_freeze();assert(!begin(true,false));
 errno=ERANGE;nuwax_sd_command_history_dump(101,true);assert(errno==ERANGE&&log_lines-start_logs==3);
 nuwax_sd_command_history_dump(101,true);assert(log_lines-start_logs==3);assert(strstr(logs,"write_seq=0 read_seq=1"));
 nuwax_sd_command_history_arm(2);assert(!s_history->frozen&&begin(true,false));
 command(25,99,0,(const void*)1,1024);assert(!s_history->events[0].range_valid&&!s_history->events[0].source_valid);
 command(25,101,0,(const void*)1,1024);assert(!s_history->events[1].range_valid);
 command(25,100,0,(const void*)1,513);assert(!s_history->events[2].range_valid);nuwax_sd_command_history_end();
 card.ocr=0;nuwax_sd_command_history_arm(3);assert(begin(true,false));
 command(24,101*512,0,bounce,512);assert(s_history->events[0].lba==101&&s_history->events[0].source_valid);
 command(24,101*512+1,0,(const void*)1,512);assert(!s_history->events[1].range_valid);nuwax_sd_command_history_end();card.ocr=SD_OCR_SDHC_CAP;
 nuwax_sd_command_history_arm(4);
 for(unsigned n=0;n<600;n++){assert(begin(true,false));command(24,101,0,bounce,512);nuwax_sd_command_history_end();}
 nuwax_sd_command_history_freeze();start_logs=log_lines;errno=EDOM;
 nuwax_sd_command_history_dump(101,true);assert(errno==EDOM&&log_lines-start_logs<=13&&strstr(logs,"retained=256"));
 assert(!memcmp(bounce,source+512,512)&&allocations==1);
 nuwax_sd_command_history_arm(5);assert(begin(true,false));
 nuwax_sd_command_history_command(&alias,24,101,0x900,0,bounce,512);nuwax_sd_command_history_end();
 assert(s_history->count==1&&s_history->events[0].source_valid);
 nuwax_sd_command_history_arm(6);assert(begin(true,true));
 command(24,101,0,bounce,512);command(17,101,0,bounce,512);nuwax_sd_command_history_end();
 assert(s_history->count==1); /* A metadata write during a read phase is still a write scope. */
 puts("PASS actual native command history: no added I/O, no per-command logs, source/range CRC, native byte/block addressing, alias/task isolation, CMD13 association, bounded ring/dump, freeze before repair, errno, unchanged audio");
 nuwax_sd_command_history_arm(7);assert(begin(false,false));nuwax_sd_command_history_end();
 task=2;assert(nuwax_sd_command_history_observing()&&!nuwax_sd_command_history_active());
 command(32,100,0,NULL,0);command(33,110,0,NULL,0);command(38,0,0,NULL,0);command(13,1,0,NULL,0);
 assert(s_history->erase_valid[0]&&s_history->erase_valid[1]&&s_history->erase_valid[2]);
 assert(!s_history->erase[2].writer_owner&&s_history->erase[2].polls==1);
 unsigned before=s_history->count;nuwax_sd_command_history_command(&foreign,38,0,0x900,0,NULL,0);assert(s_history->count==before);
 task=1;nuwax_sd_command_history_freeze();assert(!nuwax_sd_command_history_observing());
 puts("PASS ordinary cache-fill reads and foreign-owner native erase/discard are retained; unrelated cards and frozen scopes excluded");
 free(s_history);s_history=NULL;return 0;
}
