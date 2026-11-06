/* Compile the production worker unchanged, with a threaded host RTOS shim. */
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include "conversation/control_ack_tx.c"
#include "conversation/session_dispatch.h"

static atomic_llong now_us = 1000000;
static atomic_uint epoch = 1U;
static atomic_uint received, sent, send_entered;
static atomic_bool hold_receive = true, stop_worker, delay_send, fail_send;
static unsigned allocation_count, free_count, delete_count, task_count;
static bool fail_allocate, fail_queue, fail_task;
static pthread_t worker;
static TaskFunction_t worker_function;
static void *worker_arg;
static pthread_mutex_t sent_lock = PTHREAD_MUTEX_INITIALIZER;
static control_ack_job_t last_sent;
static unsigned emotion_calls;
static esp_err_t last_admission;

static void sleep_ms(unsigned milliseconds) {
    struct timespec ts = {milliseconds / 1000U,
                          (long)(milliseconds % 1000U) * 1000000L};
    nanosleep(&ts, NULL);
}

static int64_t wall_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

int64_t esp_timer_get_time(void) { return atomic_load(&now_us); }

uint32_t nuwax_ws_connection_epoch(const nuwax_ws_client_t *ws) {
    assert(ws);
    return atomic_load(&epoch);
}

void *heap_caps_malloc(size_t bytes, uint32_t caps) {
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (fail_allocate) return NULL;
    ++allocation_count;
    return malloc(bytes);
}

void heap_caps_free(void *memory) {
    assert(memory);
    ++free_count;
    free(memory);
}

QueueHandle_t xQueueCreateStatic(UBaseType_t length, UBaseType_t item_bytes,
                                uint8_t *storage, StaticQueue_t *control) {
    if (fail_queue) return NULL;
    memset(control, 0, sizeof(*control));
    assert(pthread_mutex_init(&control->lock, NULL) == 0);
    assert(pthread_cond_init(&control->ready, NULL) == 0);
    control->storage = storage;
    control->capacity = length;
    control->item_bytes = item_bytes;
    return control;
}

void vQueueDelete(QueueHandle_t queue) {
    ++delete_count;
    assert(pthread_mutex_destroy(&queue->lock) == 0);
    assert(pthread_cond_destroy(&queue->ready) == 0);
}

BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait) {
    assert(wait == 0U);
    pthread_mutex_lock(&queue->lock);
    const bool full = queue->count == queue->capacity;
    if (!full) {
        memcpy(queue->storage + queue->tail * queue->item_bytes,
               item, queue->item_bytes);
        queue->tail = (queue->tail + 1U) % queue->capacity;
        ++queue->count;
        pthread_cond_broadcast(&queue->ready);
    }
    pthread_mutex_unlock(&queue->lock);
    return full ? 0 : pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait) {
    assert(wait == portMAX_DELAY);
    pthread_mutex_lock(&queue->lock);
    while ((queue->count == 0U || atomic_load(&hold_receive)) &&
           !atomic_load(&stop_worker)) {
        pthread_cond_wait(&queue->ready, &queue->lock);
    }
    if (atomic_load(&stop_worker)) {
        pthread_mutex_unlock(&queue->lock);
        pthread_exit(NULL);
    }
    memcpy(item, queue->storage + queue->head * queue->item_bytes,
           queue->item_bytes);
    queue->head = (queue->head + 1U) % queue->capacity;
    --queue->count;
    atomic_fetch_add(&received, 1U);
    pthread_mutex_unlock(&queue->lock);
    return pdTRUE;
}

static void *worker_entry(void *unused) {
    (void)unused;
    worker_function(worker_arg);
    return NULL;
}

BaseType_t xTaskCreateWithCaps(TaskFunction_t function, const char *name,
                              uint32_t stack, void *arg, UBaseType_t priority,
                              TaskHandle_t *handle, UBaseType_t caps) {
    assert(strcmp(name, "control_ack") == 0);
    assert(stack >= 8192U && priority == 4U);
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (fail_task) return 0;
    ++task_count;
    worker_function = function;
    worker_arg = arg;
    *handle = &worker;
    assert(pthread_create(&worker, NULL, worker_entry, NULL) == 0);
    return pdPASS;
}

esp_err_t nuwax_session_send_command_ack_until_if_epoch(
    nuwax_ws_client_t *ws, const char *request, const char *command, bool ok,
    const char *message, int64_t deadline, uint32_t expected_epoch) {
    assert(pthread_equal(pthread_self(), worker));
    assert(deadline > esp_timer_get_time());
    assert(expected_epoch == atomic_load(&epoch));
    atomic_fetch_add(&send_entered, 1U);
    if (atomic_exchange(&delay_send, false)) {
        sleep_ms(500U);
        atomic_store(&now_us, deadline);
        return ESP_ERR_TIMEOUT;
    }
    if (atomic_exchange(&fail_send, false)) return ESP_FAIL;
    pthread_mutex_lock(&sent_lock);
    memset(&last_sent, 0, sizeof(last_sent));
    last_sent.ws = ws;
    last_sent.epoch = expected_epoch;
    last_sent.deadline_us = deadline;
    last_sent.ok = ok;
    snprintf(last_sent.request_id, sizeof(last_sent.request_id), "%s", request);
    snprintf(last_sent.command, sizeof(last_sent.command), "%s", command);
    snprintf(last_sent.message, sizeof(last_sent.message), "%s", message);
    pthread_mutex_unlock(&sent_lock);
    atomic_fetch_add(&sent, 1U);
    return ESP_OK;
}

static void release_receive(void) {
    pthread_mutex_lock(&s_queue->lock);
    atomic_store(&hold_receive, false);
    pthread_cond_broadcast(&s_queue->ready);
    pthread_mutex_unlock(&s_queue->lock);
}

static void await_counter(atomic_uint *counter, unsigned target) {
    const int64_t until = wall_us() + 3000000LL;
    while (atomic_load(counter) < target && wall_us() < until) sleep_ms(1U);
    assert(atomic_load(counter) == target);
}

static nuwax_session_command_t command_for(const char *request) {
    nuwax_session_command_t command = {.has_command = true};
    snprintf(command.request_id, sizeof(command.request_id), "%s", request);
    snprintf(command.command, sizeof(command.command), "%s", "set_emotion");
    snprintf(command.emotion, sizeof(command.emotion), "%s", "focused");
    return command;
}

static bool set_emotion(void *ctx, const char *emotion, bool has_duration,
                        uint32_t duration) {
    (void)ctx;
    (void)duration;
    assert(strcmp(emotion, "focused") == 0 && !has_duration);
    ++emotion_calls;
    return true;
}

static esp_err_t enqueue_hook(nuwax_ws_client_t *ws,
                              const nuwax_session_command_t *command,
                              bool ok, const char *message, uint32_t ws_epoch) {
    last_admission = nuwax_control_ack_tx_enqueue(ws, command, ok, message, ws_epoch);
    return last_admission;
}

static void test_init_failures(void) {
    fail_allocate = true;
    assert(nuwax_control_ack_tx_init() == ESP_ERR_NO_MEM);
    assert(!s_task && !s_queue && !s_queue_storage);
    fail_allocate = false;
    fail_queue = true;
    assert(nuwax_control_ack_tx_init() == ESP_ERR_NO_MEM);
    assert(allocation_count == free_count && delete_count == 0U);
    fail_queue = false;
    fail_task = true;
    assert(nuwax_control_ack_tx_init() == ESP_ERR_NO_MEM);
    assert(!s_task && !s_queue && !s_queue_storage);
    assert(allocation_count == free_count && delete_count == 1U);
    fail_task = false;
    assert(nuwax_control_ack_tx_init() == ESP_OK);
    assert(nuwax_control_ack_tx_init() == ESP_OK);
    assert(task_count == 1U && allocation_count == free_count + 1U);
}

int main(void) {
    nuwax_ws_client_t ws = {0};
    nuwax_session_command_t command = command_for("initial");
    assert(nuwax_control_ack_tx_enqueue(&ws, &command, true, NULL, 1U) ==
           ESP_ERR_INVALID_STATE);
    test_init_failures();
    assert(nuwax_control_ack_tx_enqueue(NULL, &command, true, NULL, 1U) ==
           ESP_ERR_INVALID_ARG);
    assert(nuwax_control_ack_tx_enqueue(&ws, NULL, true, NULL, 1U) ==
           ESP_ERR_INVALID_ARG);
    assert(nuwax_control_ack_tx_enqueue(&ws, &command, true, NULL, 0U) ==
           ESP_ERR_INVALID_ARG);
    char too_long[257];
    memset(too_long, 'x', sizeof(too_long) - 1U);
    too_long[sizeof(too_long) - 1U] = '\0';
    assert(nuwax_control_ack_tx_enqueue(&ws, &command, true, too_long, 1U) ==
           ESP_ERR_INVALID_SIZE);

    char message[] = "immutable";
    const int64_t admitted_at = esp_timer_get_time();
    assert(nuwax_control_ack_tx_enqueue(&ws, &command, true, message, 1U) == ESP_OK);
    memset(&command, 0, sizeof(command));
    memset(message, '?', sizeof(message) - 1U);
    release_receive();
    await_counter(&sent, 1U);
    pthread_mutex_lock(&sent_lock);
    assert(strcmp(last_sent.request_id, "initial") == 0);
    assert(strcmp(last_sent.command, "set_emotion") == 0);
    assert(strcmp(last_sent.message, "immutable") == 0);
    assert(last_sent.ok && last_sent.epoch == 1U);
    assert(last_sent.deadline_us == admitted_at + 500000LL);
    pthread_mutex_unlock(&sent_lock);

    atomic_store(&hold_receive, true);
    command = command_for("expired");
    assert(nuwax_control_ack_tx_enqueue(&ws, &command, true, NULL, 1U) == ESP_OK);
    atomic_fetch_add(&now_us, 500000LL);
    command = command_for("old-epoch");
    assert(nuwax_control_ack_tx_enqueue(&ws, &command, true, NULL, 1U) == ESP_OK);
    atomic_store(&epoch, 2U);
    release_receive();
    await_counter(&received, 3U);
    sleep_ms(10U);
    assert(atomic_load(&send_entered) == 1U);

    /* The send consumes 500 ms in a real background thread. The capture-side
     * producer still admits eight jobs and rejects the ninth immediately. */
    atomic_store(&delay_send, true);
    command = command_for("slow");
    assert(nuwax_control_ack_tx_enqueue(&ws, &command, true, NULL, 2U) == ESP_OK);
    await_counter(&send_entered, 2U);
    const int64_t producer_start = wall_us();
    for (unsigned i = 0U; i < ACK_QUEUE_DEPTH; ++i) {
        assert(nuwax_control_ack_tx_enqueue(&ws, &command, true, NULL, 2U) == ESP_OK);
    }
    assert(nuwax_control_ack_tx_enqueue(&ws, &command, true, NULL, 2U) == ESP_ERR_TIMEOUT);
    assert(wall_us() - producer_start < 100000LL);
    assert(atomic_load(&send_entered) == 2U);
    await_counter(&received, 12U);
    sleep_ms(10U);
    assert(atomic_load(&send_entered) == 2U);

    /* Exercise the real dispatch ledger with a deferred sender: side effects
     * happen once, including when queue admission fails and cloud retries. */
    nuwax_session_dispatch_set_ack_sender(enqueue_hook);
    nuwax_session_dispatch_callbacks_t callbacks = {.set_emotion = set_emotion};
    atomic_store(&hold_receive, true);
    for (unsigned i = 0U; i < ACK_QUEUE_DEPTH; ++i) {
        assert(nuwax_control_ack_tx_enqueue(&ws, &command, true, NULL, 2U) == ESP_OK);
    }
    command = command_for("ledger-full");
    nuwax_session_dispatch_command(&ws, &command, &callbacks);
    assert(last_admission == ESP_ERR_TIMEOUT && emotion_calls == 1U);
    nuwax_session_dispatch_command(&ws, &command, &callbacks);
    assert(last_admission == ESP_ERR_TIMEOUT && emotion_calls == 1U);
    atomic_fetch_add(&now_us, 500000LL);
    release_receive();
    await_counter(&received, 20U);
    nuwax_session_dispatch_command(&ws, &command, &callbacks);
    assert(last_admission == ESP_OK && emotion_calls == 1U);
    await_counter(&sent, 2U);
    pthread_mutex_lock(&sent_lock);
    assert(last_sent.ok && strcmp(last_sent.request_id, "ledger-full") == 0);
    pthread_mutex_unlock(&sent_lock);

    atomic_store(&fail_send, true);
    assert(nuwax_control_ack_tx_enqueue(&ws, &command, true, NULL, 2U) == ESP_OK);
    await_counter(&send_entered, 4U);
    assert(nuwax_control_ack_tx_enqueue(&ws, &command, true, NULL, 2U) == ESP_OK);
    await_counter(&sent, 3U);

    atomic_store(&stop_worker, true);
    release_receive();
    assert(pthread_join(worker, NULL) == 0);
    vQueueDelete(s_queue);
    heap_caps_free(s_queue_storage);
    assert(allocation_count == free_count);
    puts("control ACK worker + dispatch ledger host check: PASS");
    return 0;
}
