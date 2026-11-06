#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Read-only structural check for one FAT directory. The caller owns the mount
 * lifetime. Paths use FatFs drive syntax, not the VFS mount prefix. */
esp_err_t nuwax_sd_directory_validate(void *card, const char *fat_path, bool allow_missing);
