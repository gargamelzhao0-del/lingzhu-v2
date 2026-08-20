#pragma once

#include <stdbool.h>
#include <stddef.h>

#define NUWAX_WIFI_SAVED_SSID_BYTES 33
#define NUWAX_WIFI_SAVED_PASSWORD_BYTES 65

static inline void nuwax_wifi_saved_copy(char *destination,
                                         size_t destination_size,
                                         const char *source)
{
    if (!destination || destination_size == 0) {
        return;
    }
    size_t index = 0;
    if (source) {
        while (index + 1 < destination_size && source[index] != '\0') {
            destination[index] = source[index];
            index++;
        }
    }
    destination[index] = '\0';
}

static inline bool nuwax_wifi_saved_network_append(
    char ssids[][NUWAX_WIFI_SAVED_SSID_BYTES],
    char passwords[][NUWAX_WIFI_SAVED_PASSWORD_BYTES],
    int capacity,
    int *count,
    const char *ssid,
    const char *password)
{
    if (!ssids || !passwords || !count || capacity <= 0 || *count < 0 ||
        *count >= capacity || !ssid || ssid[0] == '\0') {
        return false;
    }

    const int destination = *count;
    nuwax_wifi_saved_copy(ssids[destination],
                          NUWAX_WIFI_SAVED_SSID_BYTES, ssid);
    nuwax_wifi_saved_copy(passwords[destination],
                          NUWAX_WIFI_SAVED_PASSWORD_BYTES, password);
    *count = destination + 1;
    return true;
}
