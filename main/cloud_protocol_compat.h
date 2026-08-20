#pragma once

typedef enum {
    NUWAX_CLOUD_MESSAGE_NONE = 0,
    NUWAX_CLOUD_MESSAGE_HELLO_OK,
    NUWAX_CLOUD_MESSAGE_HELLO_REJECTED,
    NUWAX_CLOUD_MESSAGE_AUTH_REJECTED,
    NUWAX_CLOUD_MESSAGE_PROVISION_REQUIRED,
    NUWAX_CLOUD_MESSAGE_PONG,
    NUWAX_CLOUD_MESSAGE_DEVICE_COMMAND,
} nuwax_cloud_message_kind_t;

/* Classifies application-layer evidence that the connected cloud peer has
 * accepted the device session. The payload must contain exactly one complete
 * JSON object. expected_device_id is used whenever a hello response includes
 * device_id; a mismatched identity is never accepted. */
nuwax_cloud_message_kind_t nuwax_cloud_classify_message(
    const char *payload,
    int len,
    const char *expected_device_id);
