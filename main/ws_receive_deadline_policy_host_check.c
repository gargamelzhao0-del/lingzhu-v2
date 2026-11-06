#include <stdio.h>
#include <stdlib.h>

#include "ws_receive_deadline_policy.h"

static void expect_action(const char *name,
                          nuwax_ws_receive_timeout_action_t actual,
                          nuwax_ws_receive_timeout_action_t expected) {
    if (actual != expected) {
        fprintf(stderr, "%s: expected=%d actual=%d\n", name,
                (int)expected, (int)actual);
        exit(1);
    }
}

int main(void) {
    const int64_t heartbeat_us = 25000000LL;
    const int64_t idle_us = 60000000LL;

    expect_action("lock timeout before heartbeat",
                  nuwax_ws_receive_timeout_action(24686000LL,
                                                  heartbeat_us,
                                                  idle_us),
                  NUWAX_WS_RECEIVE_TIMEOUT_RETRY);
    expect_action("heartbeat deadline",
                  nuwax_ws_receive_timeout_action(heartbeat_us,
                                                  heartbeat_us,
                                                  idle_us),
                  NUWAX_WS_RECEIVE_TIMEOUT_SEND_HEARTBEAT);
    expect_action("between heartbeat and idle",
                  nuwax_ws_receive_timeout_action(45000000LL,
                                                  heartbeat_us,
                                                  idle_us),
                  NUWAX_WS_RECEIVE_TIMEOUT_SEND_HEARTBEAT);
    expect_action("idle deadline",
                  nuwax_ws_receive_timeout_action(idle_us,
                                                  heartbeat_us,
                                                  idle_us),
                  NUWAX_WS_RECEIVE_TIMEOUT_DISCONNECT_IDLE);
    expect_action("idle wins when both elapsed",
                  nuwax_ws_receive_timeout_action(70000000LL,
                                                  65000000LL,
                                                  idle_us),
                  NUWAX_WS_RECEIVE_TIMEOUT_DISCONNECT_IDLE);

    puts("ws receive deadline policy: PASS");
    return 0;
}
