# SD archive development prototype

This directory is intentionally isolated from the ESP-IDF product. It is not
listed in `main/CMakeLists.txt`, has no Kconfig entry, and is not linked into
the firmware. It validates the storage invariants before an SD card is
available.

## What is implemented

- Fixed, versioned segment header containing archive/session/audio metadata.
- Exact binary frame preservation with a 32-bit length prefix per frame.
- CRC32 over frame bytes and an explicit complete/incomplete footer.
- `.open` write files and atomic `.open` -> `.ready` commit.
- Recovery of a power-loss `.open` file by truncating a partial final frame and
  promoting it to `.ready` with `complete=false`.
- Usage and ready-segment counting. The directory is scanned at initialization
  and after recovery; normal frame appends use cached counters and do not scan
  the card.
- Deletion only through an explicit `remove_ready()` call, which is intended to
  happen after a cloud archive commit ACK.
- Segment and total-byte limits. The prototype returns `NO_SPACE`; it does not
  silently delete unacknowledged data.
- Lifecycle protection prevents reinitializing an active store, and oversized
  frames are rejected without leaving a permanently active `.open` segment.
- Recovery ignores non-archive temporary files and quarantines malformed
  `archive_*.open` files under `quarantine/`.
- Initialization performs one recovery pass before the store can accept a new
  segment, so stale `.open` files do not silently consume the live quota.

## Build and run on the host

```text
cmake -S tools/sd-archive-dev -B tools/sd-archive-dev/build
cmake --build tools/sd-archive-dev/build
ctest --test-dir tools/sd-archive-dev/build --output-on-failure
```

With a compiler that is not available through CMake, the equivalent direct
build is:

```text
gcc -std=c11 -Wall -Wextra -Werror -Itools/sd-archive-dev/include \
  tools/sd-archive-dev/src/sd_archive_store.c \
  tools/sd-archive-dev/tests/test_sd_archive_store.c \
  -o tools/sd-archive-dev/sd_archive_store_test
tools/sd-archive-dev/sd_archive_store_test
```

## Deliberate non-goals

This prototype does not mount FATFS, capture microphone frames, own a FreeRTOS
task, upload data, decode music, or access the speaker. Those responsibilities
must be added only after the storage behavior and cloud archive contract are
approved.

## Required follow-up before device integration

1. Port the file adapter to FATFS and serialize all SD access through one
   owner task.
2. Add a bounded PSRAM queue between monitoring capture and SD writes; never
   call file I/O in the 20 ms capture loop.
3. Define a separate cloud archive API with archive ID, segment ID, offset,
   checksum, idempotent commit, and resume ACK. The existing live
   `listen`/NA-v1 stream cannot provide these guarantees.
4. Add quota policy, card-removal handling, metrics, power-loss tests, and
   long-duration hardware soak tests.
