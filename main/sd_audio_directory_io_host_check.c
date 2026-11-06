#include <stdbool.h>
#include <errno.h>
#include <dirent.h>
static bool directory_fault;
static unsigned fault_hits;
static struct dirent *checked_readdir(DIR *dir)
{
    if (directory_fault) { ++fault_hits; errno = EIO; return NULL; }
    return readdir(dir);
}
#define readdir checked_readdir
#define main existing_repair_cases
#include "sd_audio_catalog_repair_host_check.c"
#undef main
#undef readdir
int main(void)
{
    enter_case("pending_io");
    sd_audio_session_t session = make("meeting_pending", 0U, 3U, false);
    bool pending = false;
    assert(sd_audio_catalog_has_pending(&pending) == ESP_OK && pending);
    directory_fault = true; pending = true;
    assert(sd_audio_catalog_has_pending(&pending) == ESP_FAIL && fault_hits > 0U);
    assert(sd_audio_catalog_recover(8U) == ESP_FAIL);
    directory_fault = false;
    assert(sd_audio_catalog_has_pending(&pending) == ESP_OK && pending);
    assert(session.segment_count == 3U);
    puts("PASS directory I/O: readdir EIO is an error, never successful empty/pending=false recovery");
    assert(chdir("..") == 0);
    enter_case("paged_io"); session = make("monitor_directory", 1U, 3U, true);
    poison(&session, 0U, true);
    uint32_t first = 99U;
    assert(first_existing_index(&session, &first) == ESP_OK && first == 0U);
    directory_fault = true;
    assert(first_existing_index(&session, &first) == ESP_FAIL && first != session.segment_count);
    bool repaired = true;
    assert(repair_pending_session(&session, false, &repaired) == ESP_FAIL && !repaired && session.done);
    directory_fault = false;
    assert(repair_pending_session(&session, false, &repaired) == ESP_OK && repaired && !session.done);
    puts("PASS directory I/O: paged scan error cannot prune a skipped prefix or authorize done; healthy retry restores the same files");
    return 0;
}
