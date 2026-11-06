#pragma once

/* A begin may contain 16 durable resume entries plus session_time. Keep a
 * bounded transport envelope large enough for maximum-width JSON numbers. */
#define NUWAX_SD_CONTROL_TEXT_MAX_BYTES 4096U
