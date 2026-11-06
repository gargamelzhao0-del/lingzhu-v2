#ifndef NUWAX_SOCKET_WAIT_HOST_TEST
#include "sdkconfig.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sys.h"
#endif
#include <stdatomic.h>
#include <stdint.h>
#include "sd_socket_wait.h"

#if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE
/* One serialized formal sender. A timeout signal is borrowed only while the
 * real SDK sys_arch_sem_wait is on the stack. The callback never accesses the
 * caller's metrics, task or socket; its state and completion semaphore live
 * for the runtime lifetime. No allocation/logging/I/O in the callback. */
static esp_timer_handle_t s_timer;
static StaticSemaphore_t s_done_storage;
static SemaphoreHandle_t s_done;
static atomic_uint s_ready;
static atomic_uintptr_t s_owner;
static nuwax_ws_send_metrics_t *s_metrics;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    bool armed, fired, callback_active;
    int64_t deadline_us;
    SemaphoreHandle_t sem;
} s_wait;

static void short_wait_expired(void *unused)
{
    (void)unused;
    SemaphoreHandle_t sem = NULL;
    portENTER_CRITICAL(&s_lock);
    /* A callback selected just before stop may run after a later wait arms
     * this same timer. Its new deadline prevents an early/ABA timeout. */
    if (s_wait.armed && !s_wait.callback_active &&
        esp_timer_get_time() >= s_wait.deadline_us) {
        s_wait.callback_active = true;
        s_wait.fired = true;
        sem = s_wait.sem;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!sem) return;
    xSemaphoreGive(sem);
    /* Complete all accesses to the borrowed semaphore BEFORE publication.
     * A new arm refuses callback_active until the old handler fully retires. */
    xSemaphoreGive(s_done);
    portENTER_CRITICAL(&s_lock);
    s_wait.callback_active = false;
    portEXIT_CRITICAL(&s_lock);
}

void nuwax_sd_socket_wait_prepare(void)
{
    unsigned vacant = 0U;
    if (!atomic_compare_exchange_strong_explicit(&s_ready, &vacant, 1U,
            memory_order_acq_rel, memory_order_acquire)) return;
    s_done = xSemaphoreCreateBinaryStatic(&s_done_storage);
    const esp_timer_create_args_t args = {
        .callback = short_wait_expired, .dispatch_method = ESP_TIMER_TASK,
        .name = "sd_socket_wait",
    };
    const bool ok = s_done && esp_timer_create(&args, &s_timer) == ESP_OK;
    /* A failed optional prepare never reserves an owner or blocks playback.
     * Retry on a later file; no per-WANT or callback allocation. */
    atomic_store_explicit(&s_ready, ok ? 2U : 0U, memory_order_release);
}

bool nuwax_sd_socket_wait_begin(nuwax_ws_send_metrics_t *metrics)
{
    if (!metrics || !metrics->shared_tls_archive ||
        atomic_load_explicit(&s_ready, memory_order_acquire) != 2U) {
        if (metrics) metrics->precise_wait_fallbacks++;
        return false;
    }
    uintptr_t vacant = 0U;
    if (!atomic_compare_exchange_strong_explicit(&s_owner, &vacant,
            (uintptr_t)xTaskGetCurrentTaskHandle(), memory_order_acq_rel, memory_order_acquire)) {
        metrics->precise_wait_fallbacks++;
        return false;
    }
    s_metrics = metrics;
    return true;
}

void nuwax_sd_socket_wait_end(void)
{
    if (atomic_load_explicit(&s_owner, memory_order_acquire) !=
            (uintptr_t)xTaskGetCurrentTaskHandle()) return;
    s_metrics = NULL;
    atomic_store_explicit(&s_owner, 0U, memory_order_release);
}

static bool finish_short_wait(void)
{
    portENTER_CRITICAL(&s_lock);
    s_wait.armed = false;
    const bool joining = s_wait.callback_active;
    portEXIT_CRITICAL(&s_lock);
    (void)esp_timer_stop(s_timer);
    if (joining) {
        /* The callback has no waits/network/FS calls. Never let its last
         * Give race a caller freeing/reusing the borrowed semaphore. */
        (void)xSemaphoreTake(s_done, portMAX_DELAY);
    }
    portENTER_CRITICAL(&s_lock);
    const bool expired = s_wait.fired;
    SemaphoreHandle_t sem = s_wait.sem;
    s_wait.sem = NULL;
    portEXIT_CRITICAL(&s_lock);
    /* A real ACK or the RTOS timeout can win just BEFORE the callback Give.
     * Remove a late timer token even if lwIP's sem_signalled flag is false.
     * If we also consume a real signal, returning SYS_ARCH_TIMEOUT below
     * makes select run its existing per-thread semaphore cleanup. */
    if (expired && sem) (void)xSemaphoreTake(sem, 0U);
    return expired;
}

u32_t __real_sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout);
u32_t __wrap_sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout)
{
    if (!timeout || timeout > 16U || !sem || !*sem ||
        atomic_load_explicit(&s_owner, memory_order_acquire) !=
            (uintptr_t)xTaskGetCurrentTaskHandle() || !s_metrics)
        return __real_sys_arch_sem_wait(sem, timeout);
    if (xSemaphoreTake(*sem, 0U) == pdTRUE) return 0U;
    portENTER_CRITICAL(&s_lock);
    const bool busy = s_wait.callback_active || s_wait.armed;
    portEXIT_CRITICAL(&s_lock);
    if (busy) {
        s_metrics->precise_wait_fallbacks++;
        return __real_sys_arch_sem_wait(sem, timeout);
    }
    (void)xSemaphoreTake(s_done, 0U); /* Retired callback completion only. */
    portENTER_CRITICAL(&s_lock);
    s_wait.sem = *sem;
    s_wait.deadline_us = esp_timer_get_time() + (int64_t)timeout * 1000;
    s_wait.fired = false;
    s_wait.armed = true;
    portEXIT_CRITICAL(&s_lock);
    const esp_err_t started = esp_timer_start_once(s_timer, (uint64_t)timeout * 1000U);
    if (started != ESP_OK) {
        const bool expired = finish_short_wait();
        s_metrics->precise_wait_fallbacks++;
        if (expired) { s_metrics->precise_wait_timeouts++; return SYS_ARCH_TIMEOUT; }
        return __real_sys_arch_sem_wait(sem, timeout);
    }
    s_metrics->precise_wait_calls++;
    const u32_t result = __real_sys_arch_sem_wait(sem, timeout);
    const bool expired = finish_short_wait();
    if (expired) {
        s_metrics->precise_wait_timeouts++;
        /* Crucial: lwIP select's existing timeout cleanup drains a racing
         * real socket signal. Returning success here could leave a thread-
         * local semaphore token for an unrelated future socket operation. */
        return SYS_ARCH_TIMEOUT;
    }
    return result;
}
#else
void nuwax_sd_socket_wait_prepare(void) {}
bool nuwax_sd_socket_wait_begin(nuwax_ws_send_metrics_t *metrics) { (void)metrics; return false; }
void nuwax_sd_socket_wait_end(void) {}
#endif
