/* Tail tasks may be scheduled in any order. Publish FIFO ownership before
 * launching a worker; only the head may attach/drain/close an archive. */
typedef struct mirror_close_node {
    nuwax_offline_audio_mirror_t mirror;
    uint32_t deadline_ms;
    struct mirror_close_node *next;
} mirror_close_node_t;
static portMUX_TYPE s_mirror_close_lock = portMUX_INITIALIZER_UNLOCKED;
static mirror_close_node_t *s_mirror_close_head, *s_mirror_close_tail;

static void mirror_close_enqueue(mirror_close_node_t *node)
{
    node->next = NULL;
    portENTER_CRITICAL(&s_mirror_close_lock);
    if (s_mirror_close_tail) s_mirror_close_tail->next = node;
    else s_mirror_close_head = node;
    s_mirror_close_tail = node;
    atomic_fetch_add_explicit(&s_mirror_closers, 1U, memory_order_release);
    portEXIT_CRITICAL(&s_mirror_close_lock);
}

static bool mirror_close_is_head(mirror_close_node_t *node)
{
    portENTER_CRITICAL(&s_mirror_close_lock);
    const bool head = s_mirror_close_head == node;
    portEXIT_CRITICAL(&s_mirror_close_lock);
    return head;
}

static void mirror_close_remove(mirror_close_node_t *node)
{
    portENTER_CRITICAL(&s_mirror_close_lock);
    mirror_close_node_t *previous = NULL, *cursor = s_mirror_close_head;
    while (cursor && cursor != node) { previous = cursor; cursor = cursor->next; }
    if (cursor) {
        if (previous) previous->next = node->next;
        else s_mirror_close_head = node->next;
        if (s_mirror_close_tail == node) s_mirror_close_tail = previous;
        atomic_fetch_sub_explicit(&s_mirror_closers, 1U, memory_order_release);
    }
    portEXIT_CRITICAL(&s_mirror_close_lock);
}
