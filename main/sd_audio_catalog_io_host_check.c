#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <io.h>
#define NATIVE_SYNC _commit
#else
#define NATIVE_SYNC fsync
#endif
#include "freertos/task.h"
#include "board/sd_dma_bulk.h"
#include "sd_audio_read_recovery_host.h"
uint32_t esp_random(void) { static uint32_t state=1U;state=state*1664525U+1013904223U;return state; }
bool nuwax_sd_io_probe_begin(nuwax_sd_io_probe_t *probe) { (void)probe;return false; }
void nuwax_sd_io_probe_end(void) { assert(!"unowned probe end"); }
bool nuwax_sd_probe_trace_begin(void) { assert(!"unowned command trace"); return false; }
void nuwax_sd_probe_trace_finish(const char *scope, int result, uint64_t lba, uint32_t expected, uint32_t actual)
{ (void)scope; (void)result; (void)lba; (void)expected; (void)actual; assert(!"unowned command trace finish"); }
void vTaskDelay(TickType_t ticks) { (void)ticks; }
static char diagnostic[32768]; static size_t diagnostic_bytes;
static void catalog_host_log(const char *format, ...)
{
    va_list args; va_start(args, format);
    const int n = vsnprintf(diagnostic + diagnostic_bytes, sizeof(diagnostic) - diagnostic_bytes, format, args);
    va_end(args); assert(n >= 0 && (size_t)n + 2U < sizeof(diagnostic) - diagnostic_bytes);
    diagnostic_bytes += (size_t)n; diagnostic[diagnostic_bytes++] = '\n'; diagnostic[diagnostic_bytes] = 0;
}
typedef enum { NONE, READ_ONCE, WRITE_FAIL, SYNC_FAIL, CLOSE_FAIL, RENAME_FAIL, CLEANUP_FAIL, STAT_FAIL } fault_t;
static fault_t fault; static bool read_injected;
static size_t host_read(void *dst, size_t size, size_t count, FILE *file)
{
    const size_t n = fread(dst, size, count, file);
    if (fault == READ_ONCE && !read_injected && size == 1U && n == count && count >= 240U) {
        memset(dst, 0, n); read_injected = true;
    }
    return n;
}
static size_t host_write(const void *src, size_t size, size_t count, FILE *file)
{
    if (fault == WRITE_FAIL) { errno = EIO; return count ? count - 1U : 0U; }
    return fwrite(src, size, count, file);
}
static int host_sync(int fd) { if (fault == SYNC_FAIL) { errno = EIO; return -1; } return NATIVE_SYNC(fd); }
static int host_close(FILE *file) { const int n = fclose(file); if (fault == CLOSE_FAIL) { errno = EIO; return EOF; } return n; }
static int host_rename(const char *a, const char *b) { if (fault == RENAME_FAIL) { errno = EIO; return -1; } return rename(a, b); }
static int host_unlink(const char *path) { if (fault == CLEANUP_FAIL) { errno = EIO; return -1; } return unlink(path); }
static int host_fstat(int fd, struct stat *st) { if (fault == STAT_FAIL) { errno = EIO; return -1; } return fstat(fd, st); }
#define fread host_read
#define fwrite host_write
#define fclose host_close
#define rename host_rename
#define unlink host_unlink
#ifdef _WIN32
#define _commit host_sync
#else
#define fsync host_sync
#endif
#undef fstat
#define fstat host_fstat
#include "sd_audio_catalog.c"
#undef fread
#undef fwrite
#undef fclose
#undef rename
#undef unlink
#undef _commit
#undef fsync
#undef fstat

static void reset_fault(fault_t value) { fault = value; read_injected = false; diagnostic_bytes = 0U; diagnostic[0] = 0; }
static void marker(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb"); assert(file);
    assert(fwrite(text, 1U, strlen(text), file) == strlen(text) && !fclose(file));
}
static void unchanged(const char *path, const char *text)
{
    char actual[64] = {0}; FILE *file = fopen(path, "rb"); assert(file);
    assert(fread(actual, 1U, sizeof(actual) - 1U, file) == strlen(text) && !fclose(file));
    assert(!strcmp(actual, text));
}
static void no_probe_files(void)
{
    DIR *dir = opendir("catalog"); assert(dir); struct dirent *entry;
    while ((entry = readdir(dir))) assert(strncmp(entry->d_name, ".nuwax_meta_", 12U));
    assert(!closedir(dir));
}
int main(void)
{
    assert(sd_audio_catalog_init() == ESP_OK && metadata_io_qualified && metadata_probe_ticket);
    const uint32_t first_ticket = metadata_probe_ticket;
    assert(sd_audio_catalog_init() == ESP_OK && metadata_probe_ticket == first_ticket);
    assert(sd_audio_catalog_prepare_remount() == ESP_OK && !metadata_io_qualified);
    assert(sd_audio_catalog_init() == ESP_OK && metadata_io_qualified && metadata_probe_ticket > first_ticket);
    no_probe_files();
    assert(strstr(diagnostic, "SD metadata qualification revision=199") && strstr(diagnostic, "stage=sparse_index"));
    puts("PASS metadata admission: actual FILE dual seed, read/update, rename, sparse cross-sector index, cleanup and one qualification per mount");

    metadata_probe_ticket = 0U;
    marker("catalog/.nuwax_meta_00000001.creating", "KEEP_ORIGINAL");
    marker("catalog/.nuwax_meta_00000002.checked", "KEEP_DESTINATION");
    assert(sd_audio_catalog_qualify_storage() == ESP_OK && metadata_probe_ticket == 3U);
    unchanged("catalog/.nuwax_meta_00000001.creating", "KEEP_ORIGINAL");
    unchanged("catalog/.nuwax_meta_00000002.checked", "KEEP_DESTINATION");
    assert(!remove("catalog/.nuwax_meta_00000001.creating") && !remove("catalog/.nuwax_meta_00000002.checked"));
    marker("catalog/old_unconfirmed.ready", "UNCONFIRMED_AUDIO");
    marker("catalog/old_scene.session", "OLD_MANIFEST");
    assert(sd_audio_catalog_pin("existing_scene", 0U) == ESP_OK);
    const uint32_t pinned_ticket = metadata_probe_ticket;
    assert(sd_audio_catalog_qualify_storage() == ESP_ERR_NOT_FINISHED && metadata_probe_ticket == pinned_ticket);
    assert(sd_audio_catalog_prepare_remount() == ESP_ERR_NOT_FINISHED && metadata_io_qualified);
    sd_audio_catalog_unpin();
    puts("PASS scratch ownership: exclusive create and rename collision preserve both foreign files; active replay prevents qualification/remount");

    for (fault_t value = READ_ONCE; value <= STAT_FAIL; ++value) {
        reset_fault(value);
        const esp_err_t err = sd_audio_catalog_qualify_storage();
        assert(err != ESP_OK && !metadata_io_qualified);
        if (value == READ_ONCE) {
            assert(err == ESP_ERR_INVALID_CRC && read_injected);
            assert(strstr(diagnostic, "sample=original") && strstr(diagnostic, "slot=0 full=1 valid=0 gen=0"));
            assert(strstr(diagnostic, "first_selected=0") && strstr(diagnostic, "sample=retry"));
            assert(strstr(diagnostic, "slot=0 full=1 valid=1 gen=2") && strstr(diagnostic, "slot=1 full=1 valid=1 gen=1"));
            assert(strstr(diagnostic, "SD metadata descriptor revision=205") &&
                strstr(diagnostic,"slot0_valid=1 slot0_gen=2") && strstr(diagnostic,"slot1_valid=1 slot1_gen=1"));
        }
        const uint32_t failed_ticket = metadata_probe_ticket;
        reset_fault(NONE);
        if (value == CLEANUP_FAIL) {
            char path[128]; snprintf(path, sizeof(path), "catalog/.nuwax_meta_%08lx.checked", (unsigned long)failed_ticket);
            assert(!remove(path));
        }
        no_probe_files();
        unchanged("catalog/old_unconfirmed.ready", "UNCONFIRMED_AUDIO"); unchanged("catalog/old_scene.session", "OLD_MANIFEST");
        assert(sd_audio_catalog_qualify_storage() == ESP_OK && metadata_io_qualified);
    }
    puts("PASS metadata faults: transient read corruption, write, sync, close, rename, cleanup and fstat reject admission; good diagnostic retry cannot overturn first failure; saved data untouched");

    sd_audio_session_t expected = {.magic = CATALOG_MAGIC, .boot_id = 7U, .complete = 1U};
    strcpy(expected.session_id, "probe_scene"); memset(expected.owner, 0x5a, sizeof(expected.owner));
    assert(catalog_seed_session("gold.creating", &expected) == ESP_OK && expected.generation == 2U);
    FILE *file = fopen("gold.creating", "rb"); assert(file); catalog_pair_sample_t sample;
    catalog_sample_pair(file, 0U, sizeof(expected), offsetof(sd_audio_session_t, crc), CATALOG_MAGIC, &sample);
    assert(!fclose(file) && sample.file_bytes == (long)(2U * sizeof(expected)) && sample.read_bytes == 2U * sizeof(expected));
    assert(sample.slot[0].valid && sample.slot[0].generation == 2U && sample.slot[1].valid && sample.slot[1].generation == 1U);
    file = fopen("gold.creating", "r+b"); assert(file); uint8_t zero[sizeof(expected)] = {0};
    assert(fwrite(zero, 1U, sizeof(zero), file) == sizeof(zero) && !fclose(file));
    reset_fault(NONE); assert(catalog_verify_session_file("gold.creating", &expected) == ESP_ERR_INVALID_CRC);
    assert(strstr(diagnostic, "stage=creating") && strstr(diagnostic, "first_selected=1 expected_gen=2 selected_gen=1 diff_offset=0"));
    assert(strstr(diagnostic, "sample=original") && strstr(diagnostic, "expected_crc=") && strstr(diagnostic, "selected_crc="));
    assert(strstr(diagnostic, "slot=0 full=1 valid=0") && strstr(diagnostic, "slot=1 full=1 valid=1 gen=1"));
    assert(!strstr(diagnostic, "5a5a5a5a5a5a"));
    file = fopen("gold.creating", "r+b"); assert(file);
    assert(!ftruncate(fileno(file), (long)sizeof(expected) + 1L) && !fclose(file));
    file = fopen("gold.creating", "rb"); assert(file);
    catalog_sample_pair(file, 0U, sizeof(expected), offsetof(sd_audio_session_t, crc), CATALOG_MAGIC, &sample);
    assert(!fclose(file) && sample.file_bytes == (long)sizeof(expected) + 1L && sample.slot[0].full && !sample.slot[1].full);
    puts("PASS actual diagnostics: creating/published phase, first selected generation vs strict mismatch, both CRC slots and short read are distinguishable; owner bytes are not logged");
    assert(!remove("gold.creating"));
    return 0;
}
