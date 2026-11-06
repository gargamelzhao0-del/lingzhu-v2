#pragma once
#include <stdbool.h>
#include "esp_err.h"
typedef struct {
    char request_id[80];
    char path[160];
    char format[8];
    bool stop;
} nuwax_music_test_command_t;
esp_err_t nuwax_music_test_control_init(void);
bool nuwax_music_test_control_take(nuwax_music_test_command_t *out);
void nuwax_music_test_control_result(const char *id, bool accepted, const char *reason);
