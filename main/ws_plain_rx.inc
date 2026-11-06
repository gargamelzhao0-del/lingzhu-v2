#if defined(ESP_PLATFORM) && !LWIP_NETCONN_FULLDUPLEX
#error "Plain archive event RX requires lwIP full duplex socket support"
#endif

/* Dedicated plain archive socket: lwIP supports one reader and one writer
 * concurrently (IDF enables LWIP_NETCONN_FULLDUPLEX). Do not acquire the TLS
 * io_lock, and do not defer received ACKs behind tx_waiters. Socket lifetime
 * is pinned by this RX worker: owner_close interrupts, joins RX, then closes.
 * Connect/handshake and every TLS connection retain the original path. */
static esp_err_t recv_plain_event_until(nuwax_ws_client_t *client,
    uint8_t *data, size_t len, int64_t deadline_us)
{
    const int sock = client->sock;
    const uint32_t epoch = nuwax_ws_connection_epoch(client);
    if (sock < 0 || client->use_tls) return ESP_ERR_INVALID_STATE;
    size_t received = 0U;
    while (received < len) {
        if (!atomic_load_explicit(&client->connected, memory_order_acquire) ||
            close_requested_for_epoch(client, epoch) ||
            nuwax_ws_connection_epoch(client) != epoch) return ESP_ERR_INVALID_STATE;
        if (io_deadline_expired(deadline_us)) return ESP_ERR_TIMEOUT;
        errno = 0;
        client->plain_rx_reads++;
        const ssize_t got = recv(sock, data + received, len - received, 0);
        const int read_errno = errno;
        /* Preserve consumed bytes even if scheduling crossed the deadline. */
        if (got > 0) { received += (size_t)got; continue; }
        if (got == 0) return ESP_FAIL;
        if (read_errno == EINTR) continue;
        if (read_errno != EAGAIN && read_errno != EWOULDBLOCK) return ESP_FAIL;
        int64_t remaining = deadline_us - esp_timer_get_time();
        if (remaining <= 0) return ESP_ERR_TIMEOUT;
        if (remaining > 100000LL) remaining = 100000LL;
        struct timeval wait = {.tv_sec = 0, .tv_usec = (suseconds_t)remaining};
        fd_set reads;
        FD_ZERO(&reads); FD_SET(sock, &reads);
        client->plain_rx_waits++;
        errno = 0;
        const int ready = select(sock + 1, &reads, NULL, NULL, &wait);
        const int wait_errno = errno;
        if (ready == 0) client->plain_rx_timeouts++;
        else if (ready < 0 && wait_errno != EINTR && wait_errno != EAGAIN &&
                 wait_errno != EWOULDBLOCK) return ESP_FAIL;
        /* select wakes immediately for data/FIN. A bounded timeout observes
         * cancellation even when shutdown cannot acquire the TX I/O lock. */
    }
    return ESP_OK;
}
