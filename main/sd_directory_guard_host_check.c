/* Compile the actual guard with deterministic FatFs/SD failures. No device I/O. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "esp_err.h"
#define CONFIG_NUWAX_SD_DIRECTORY_GUARD_ENABLE 1
#define FF_FS_REENTRANT 1
#define FF_MAX_SS 4096
#define FF_MIN_SS 512
#define FS_FAT12 1
#define FS_FAT16 2
#define FS_FAT32 3
#define AM_DIR 0x10
#define MALLOC_CAP_DMA 1
#define MALLOC_CAP_INTERNAL 2
#define MALLOC_CAP_8BIT 4
typedef enum {FR_OK,FR_DISK_ERR,FR_INT_ERR,FR_NO_FILE,FR_NO_PATH,FR_TIMEOUT,FR_NOT_ENOUGH_CORE} FRESULT;
typedef struct {unsigned fs_type,ldrv,id,csize,ssize,n_fatent;uint64_t database,dirbase;} FATFS;
typedef struct {struct {FATFS*fs;unsigned id,sclust;}obj;} FF_DIR;
typedef struct {unsigned fattrib;} FILINFO;
typedef struct {struct {unsigned sector_size;size_t capacity;}csd;} sdmmc_card_t;
static FATFS fs,other;
static sdmmc_card_t card;
static uint8_t first_sector[512];
static unsigned locked,closed,allocations,reads;
static size_t internal_free;
static bool fail_alloc,alloc_pressure,fail_lock,regular_file,wrong_id,other_fs;
static FRESULT stat_error,child_error,parent_error;
static esp_err_t read_error;
static unsigned parent_cluster;
static void *heap_caps_malloc(size_t n,unsigned caps){
 assert(locked&&n==512&&caps==(MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL));if(fail_alloc)return NULL;
 void*p=malloc(n);assert(p);++allocations;internal_free-=n;if(alloc_pressure)internal_free=8192;return p;}
static size_t heap_caps_get_free_size(unsigned caps){assert(caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));return internal_free;}
static void heap_caps_free(void*p){if(p){assert(locked&&allocations);--allocations;free(p);}}
static FRESULT f_stat(const char*p,FILINFO*i){assert(!locked&&strstr(p,"child"));i->fattrib=regular_file?0:AM_DIR;return stat_error;}
static FRESULT f_opendir(FF_DIR*d,const char*p){
 assert(!locked);bool child=strstr(p,"child")!=NULL;FRESULT err=child?child_error:parent_error;
 if(err!=FR_OK)return err;d->obj.fs=!child&&other_fs?&other:&fs;d->obj.id=wrong_id?8:7;d->obj.sclust=child?37:parent_cluster;return FR_OK;}
static FRESULT f_closedir(FF_DIR*d){assert(!locked&&d->obj.fs);++closed;d->obj.fs=NULL;return FR_OK;}
static int ff_mutex_take(unsigned v){assert(!locked&&v==0);if(fail_lock)return 0;locked=1;return 1;}
static void ff_mutex_give(unsigned v){assert(locked&&v==0);locked=0;}
static esp_err_t sdmmc_read_sectors(sdmmc_card_t*c,void*p,size_t lba,size_t count){
 assert(locked&&c==&card&&lba==100+(37-2)*64&&count==1);++reads;memcpy(p,first_sector,512);errno=EDOM;return read_error;}
#include "sd_directory_guard_actual.inc"
static void put_cluster(uint8_t*p,uint32_t n){p[26]=(uint8_t)n;p[27]=(uint8_t)(n>>8);p[20]=(uint8_t)(n>>16);p[21]=(uint8_t)(n>>24);}
static void reset(unsigned type,unsigned parent){
 assert(!locked&&!allocations);closed=reads=0;internal_free=65536;
 fail_alloc=alloc_pressure=fail_lock=regular_file=wrong_id=other_fs=false;
 stat_error=child_error=parent_error=FR_OK;read_error=ESP_OK;parent_cluster=parent;
 fs=(FATFS){.fs_type=type,.id=7,.csize=64,.ssize=512,.n_fatent=10000,.database=100,.dirbase=2};other=fs;
 card.csd.sector_size=512;card.csd.capacity=1000000;
 memset(first_sector,0,512);memcpy(first_sector,".          ",11);first_sector[11]=AM_DIR;put_cluster(first_sector,37);
 memcpy(first_sector+32,"..         ",11);first_sector[43]=AM_DIR;put_cluster(first_sector+32,parent);}
static esp_err_t check(bool missing){errno=ERANGE;esp_err_t e=nuwax_sd_directory_validate(&card,"0:/parent/child",missing);assert(errno==ERANGE&&!locked&&!allocations);return e;}
int main(void){
 for(unsigned type=1;type<=3;type++)for(unsigned parent=0;parent<=41;parent+=41){reset(type,parent);assert(check(false)==ESP_OK&&reads==1&&closed==2);}
 reset(FS_FAT32,2);put_cluster(first_sector+32,0);assert(check(false)==ESP_OK);
 reset(FS_FAT32,0);put_cluster(first_sector+32,2);assert(check(false)==ESP_OK);
 reset(FS_FAT32,41);put_cluster(first_sector+32,0);assert(check(false)==ESP_ERR_INVALID_CRC);
 for(unsigned n=0;n<64;n++){reset(FS_FAT32,41);first_sector[n]^=1;
  bool checked=n<11||(n>=20&&n<22)||(n>=26&&n<32)||(n>=32&&n<43)||(n>=52&&n<54)||(n>=58);
  if(checked)assert(check(false)==ESP_ERR_INVALID_CRC);}
 reset(FS_FAT32,41);first_sector[0]=0xe5;assert(check(false)==ESP_ERR_INVALID_CRC);
 reset(FS_FAT32,41);first_sector[11]|=8;assert(check(false)==ESP_ERR_INVALID_CRC);
 reset(FS_FAT32,41);first_sector[43]|=8;assert(check(false)==ESP_ERR_INVALID_CRC);
 reset(FS_FAT32,41);first_sector[11]=0;assert(check(false)==ESP_ERR_INVALID_CRC);
 reset(FS_FAT32,41);first_sector[43]=0;assert(check(false)==ESP_ERR_INVALID_CRC);
 for(unsigned e=FR_NO_FILE;e<=FR_NO_PATH;e++){reset(3,0);stat_error=e;assert(check(true)==ESP_OK&&!reads&&!closed);assert(check(false)==ESP_ERR_NOT_FOUND);}
 reset(3,0);regular_file=true;assert(check(true)==ESP_ERR_INVALID_CRC&&!reads);
 reset(3,0);stat_error=FR_DISK_ERR;assert(check(true)==ESP_FAIL&&!reads);
 reset(3,0);child_error=FR_DISK_ERR;assert(check(false)==ESP_FAIL&&!closed);
 reset(3,0);parent_error=FR_DISK_ERR;assert(check(false)==ESP_FAIL&&closed==1);
 reset(3,0);wrong_id=true;assert(check(false)==ESP_ERR_INVALID_STATE&&!reads&&closed==2);
 reset(3,0);other_fs=true;assert(check(false)==ESP_ERR_INVALID_STATE&&!reads);
 reset(3,0);fail_lock=true;assert(check(false)==ESP_ERR_TIMEOUT&&!reads);
 reset(3,0);fail_alloc=true;assert(check(false)==ESP_ERR_NO_MEM&&!reads);
 reset(3,0);internal_free=16384+511;assert(check(false)==ESP_ERR_NO_MEM&&!reads);
 reset(3,0);alloc_pressure=true;assert(check(false)==ESP_ERR_NO_MEM&&!reads);
 reset(3,0);read_error=ESP_FAIL;assert(check(false)==ESP_FAIL&&reads==1);
 reset(3,0);fs.fs_type=4;assert(check(false)==ESP_ERR_INVALID_CRC&&!reads);
 reset(3,0);fs.ssize=4096;assert(check(false)==ESP_ERR_INVALID_CRC&&!reads);
 reset(3,0);card.csd.capacity=2000;assert(check(false)==ESP_ERR_INVALID_CRC&&!reads);
 reset(3,0);assert(nuwax_sd_directory_validate(&card,"0:/",true)==ESP_ERR_INVALID_ARG);
 assert(nuwax_sd_directory_validate(NULL,"0:/child",false)==ESP_ERR_INVALID_ARG);
 puts("PASS actual directory guard: FAT12/16/32, root/nested parent, dot/cluster/size damage, missing vs regular file, locked native read, timeout/ENOMEM, lifetime, errno, cleanup; zero writes");
 return 0;
}
