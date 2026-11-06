#include "cloud_time.h"
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
static portMUX_TYPE s_time_lock = portMUX_INITIALIZER_UNLOCKED;
#define TIME_LOCK() portENTER_CRITICAL(&s_time_lock)
#define TIME_UNLOCK() portEXIT_CRITICAL(&s_time_lock)
#else
#define TIME_LOCK() ((void)0)
#define TIME_UNLOCK() ((void)0)
#endif

/* Deliberately starts unknown after reset. Persisted old-boot anchors belong
 * to old SD scenes; they cannot determine how long power was absent. */
static nuwax_cloud_time_anchor_t s_anchor;

bool nuwax_cloud_time_observe(uint64_t server_ms, uint64_t received_uptime_ms)
{
    if (!nuwax_cloud_time_valid(server_ms)) return false;
    TIME_LOCK();
    const bool accept = !s_anchor.utc_ms || received_uptime_ms >= s_anchor.uptime_ms;
    if (accept) s_anchor = (nuwax_cloud_time_anchor_t){server_ms, received_uptime_ms};
    TIME_UNLOCK();
    return accept;
}
bool nuwax_cloud_time_snapshot(nuwax_cloud_time_anchor_t *anchor)
{
    if (!anchor) return false;
    TIME_LOCK();
    *anchor = s_anchor;
    TIME_UNLOCK();
    return nuwax_cloud_time_valid(anchor->utc_ms);
}
