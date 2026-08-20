#include "sdkconfig.h"

#ifdef CONFIG_LV_USE_CUSTOM_MALLOC

#include "lvgl.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "lvgl_psram_mem";

typedef struct {
    size_t size;
    bool psram;
} lvgl_alloc_header_t;

#define LVGL_ALLOC_HEADER_BYTES sizeof(lvgl_alloc_header_t)
#define LVGL_INTERNAL_FALLBACK_LIMIT_BYTES 128

static size_t s_cur_used;
static size_t s_cur_used_internal;
static size_t s_max_used;
static size_t s_alloc_count;
static size_t s_internal_alloc_count;

void lv_free_core(void *p);
void nuwax_lvgl_mem_internal_usage(size_t *out_internal_bytes, size_t *out_internal_count);

static void *lvgl_heap_alloc(size_t size) {
    if (size == 0 || size > SIZE_MAX - LVGL_ALLOC_HEADER_BYTES) {
        return NULL;
    }

    const size_t total = size + LVGL_ALLOC_HEADER_BYTES;
    bool psram = true;

    /* P3-2 (downgraded to diagnostic-only): the watermark-guarded
     * internal-first borrow was structurally dead while the wake word model
     * stays resident — once the UI is up, internal_largest never exceeds the
     * reserve threshold, so this branch never fired (confirmed on-device:
     * steady internal_largest ~12-18KB << reserve+size). Keeping it would be a
     * misleading "optimization" that does nothing. We revert to pure
     * psram-first here. The borrow counters below remain so that if a future
     * P3-1 wake-word slimming frees internal SRAM, re-enabling the branch can
     * be measured directly in the UI perf log via nuwax_lvgl_mem_internal_usage().
     * See docs/6.3阶段记录-P3-2-分配器internal优先.md. */
    lvgl_alloc_header_t *header =
        (lvgl_alloc_header_t *)heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!header && size <= LVGL_INTERNAL_FALLBACK_LIMIT_BYTES) {
        header = (lvgl_alloc_header_t *)heap_caps_malloc(total, MALLOC_CAP_8BIT);
        psram = false;
    }
    if (!header) {
        ESP_LOGW(TAG,
                 "alloc failed size=%u internal=%u internal_largest=%u psram=%u psram_largest=%u",
                 (unsigned)size,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        return NULL;
    }

    header->size = size;
    header->psram = psram;
    s_cur_used += size;
    if (!psram) {
        s_cur_used_internal += size;
        s_internal_alloc_count++;
    }
    if (s_cur_used > s_max_used) {
        s_max_used = s_cur_used;
    }
    s_alloc_count++;
    return (uint8_t *)header + LVGL_ALLOC_HEADER_BYTES;
}

static lvgl_alloc_header_t *lvgl_heap_header(void *p) {
    return (lvgl_alloc_header_t *)((uint8_t *)p - LVGL_ALLOC_HEADER_BYTES);
}

void lv_mem_init(void) {
    s_cur_used = 0;
    s_cur_used_internal = 0;
    s_max_used = 0;
    s_alloc_count = 0;
    s_internal_alloc_count = 0;
    ESP_LOGI(TAG,
             "custom LVGL allocator active psram-first internal_fallback<=%u internal=%u psram=%u",
             LVGL_INTERNAL_FALLBACK_LIMIT_BYTES,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

void lv_mem_deinit(void) {
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) {
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool) {
    LV_UNUSED(pool);
}

void *lv_malloc_core(size_t size) {
    return lvgl_heap_alloc(size);
}

void *lv_realloc_core(void *p, size_t new_size) {
    if (!p) {
        return lvgl_heap_alloc(new_size);
    }
    if (new_size == 0) {
        lv_free_core(p);
        return NULL;
    }

    lvgl_alloc_header_t *old_header = lvgl_heap_header(p);
    const size_t old_size = old_header->size;
    void *next = lvgl_heap_alloc(new_size);
    if (!next) {
        return NULL;
    }
    memcpy(next, p, old_size < new_size ? old_size : new_size);
    lv_free_core(p);
    return next;
}

void lv_free_core(void *p) {
    if (!p) {
        return;
    }
    lvgl_alloc_header_t *header = lvgl_heap_header(p);
    if (header->size <= s_cur_used) {
        s_cur_used -= header->size;
    } else {
        s_cur_used = 0;
    }
    if (!header->psram) {
        if (header->size <= s_cur_used_internal) {
            s_cur_used_internal -= header->size;
        } else {
            s_cur_used_internal = 0;
        }
        if (s_internal_alloc_count > 0) {
            s_internal_alloc_count--;
        }
    }
    if (s_alloc_count > 0) {
        s_alloc_count--;
    }
    heap_caps_free(header);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) {
    if (!mon_p) {
        return;
    }
    const size_t free_size = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t free_biggest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t total_size = free_size + s_cur_used;
    memset(mon_p, 0, sizeof(*mon_p));
    mon_p->total_size = total_size;
    mon_p->free_cnt = 1;
    mon_p->free_size = free_size;
    mon_p->free_biggest_size = free_biggest;
    mon_p->used_cnt = s_alloc_count;
    mon_p->max_used = s_max_used;
    if (total_size > 0) {
        mon_p->used_pct = (uint8_t)((s_cur_used * 100U) / total_size);
    }
    if (free_size > 0) {
        mon_p->frag_pct = (uint8_t)(100U - ((free_biggest * 100U) / free_size));
    }
}

lv_result_t lv_mem_test_core(void) {
    return LV_RESULT_OK;
}

/* Diagnostic hook for future UI perf logs. */
void nuwax_lvgl_mem_internal_usage(size_t *out_internal_bytes, size_t *out_internal_count) {
    if (out_internal_bytes) {
        *out_internal_bytes = s_cur_used_internal;
    }
    if (out_internal_count) {
        *out_internal_count = s_internal_alloc_count;
    }
}

#endif
