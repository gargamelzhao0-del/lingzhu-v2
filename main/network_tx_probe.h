#pragma once

/* Test-build cumulative diagnostics. Called outside the TCP/IP task; never
 * logs or allocates from a networking callback. TCP samples are observational:
 * sample/queue ages and local_port identify stale/different-connection data;
 * observed_conn is not the WebSocket epoch and RTO samples are not a count of
 * retransmissions. Queue payload counts are sampled at most every 100 ms,
 * plus a new connection or entry into RTO, with a bounded traversal. Observed
 * ACK bytes are TCP sequence-space advances, not durable audio acknowledgments. */
void nuwax_network_tx_probe_log(void);

/* Call from ordinary task context before the first test connection. Creates
 * a single lifetime timer; allocation failure preserves polling recovery and
 * permits another attempt after one second on connection/upload activity. */
void nuwax_network_tx_probe_init(void);

/* Test gateway only: nonblocking, coalesced request to retry a rejected TCP
 * output on the TCP/IP task. Call while sending/waiting for durable ACKs. */
void nuwax_network_tx_retry_poll(void);
