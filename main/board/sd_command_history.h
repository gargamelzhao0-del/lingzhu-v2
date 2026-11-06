#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Native command evidence only. These APIs issue no I/O and never replace an
 * SDK result. Source/card pointers live only inside the synchronous SDK scope. */
bool nuwax_sd_command_history_init(void);
void nuwax_sd_command_history_arm(uint32_t generation);
bool nuwax_sd_command_history_begin(const void *card, const void *alias, const void *source,
    size_t sector, size_t count, bool write, bool independent_read, uint32_t generation,
    uint32_t operation, bool writer_owner);
void nuwax_sd_command_history_end(void);
bool nuwax_sd_command_history_active(void);
/* SDK hook gate also covers native erase/discard commands outside read/write
 * scopes. The callback still fences the bound card and records actor tags. */
bool nuwax_sd_command_history_observing(void);
void nuwax_sd_command_history_command(const void *card, uint32_t opcode, uint32_t argument,
    uint32_t response, int result, const void *buffer, size_t bytes);
void nuwax_sd_command_history_freeze(void);
void nuwax_sd_command_history_dump(uint64_t bad_lba, bool lba_known);
