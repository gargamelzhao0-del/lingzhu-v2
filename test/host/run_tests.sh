#!/usr/bin/env bash
#
# Host unit-test runner for the pure-logic focus-menu modules.
#
# Compiles the modules under test (which depend only on standard C plus a
# tiny set of LVGL types) against the stub headers in test/host/stubs, links
# them with the test suites, runs the resulting binary and reports pass/fail.
#
# Requires only a host C compiler (gcc/clang). No ESP-IDF, no hardware.
#
# Usage:  bash test/host/run_tests.sh
set -u

# Resolve repo root from this script's location (test/host/..).
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SRC="$ROOT/main/nuwax_ui_v28"
OUT="$HERE/build"
mkdir -p "$OUT"

CC="${CC:-gcc}"
CFLAGS="-std=c11 -Wall -Wextra -Werror -g -O0"

# Include order: stubs FIRST (so "lvgl.h" / "nuwax_device_state.h" resolve to
# the stubs), then the real source tree for "menu/..." and "nuwax_focus_menu.h".
INCLUDES=(
  -I "$HERE/stubs"
  -I "$HERE"
  -I "$SRC"
)

# Modules under test (pure logic only).
MODULES=(
  "$SRC/menu/nuwax_focus_menu_style.c"
  "$SRC/menu/nuwax_focus_menu_data.c"
  "$SRC/menu/nuwax_focus_menu_wifi.c"
  "$SRC/menu/nuwax_focus_menu_status.c"
  "$SRC/menu/nuwax_focus_menu_wheel.c"
  "$SRC/menu/nuwax_focus_menu_surface_style.c"
)

# Test suites + runner.
TESTS=(
  "$HERE/test_main.c"
  "$HERE/test_style.c"
  "$HERE/test_data.c"
  "$HERE/test_wifi.c"
  "$HERE/test_status.c"
  "$HERE/test_wheel.c"
  "$HERE/test_surface_style.c"
)

BIN="$OUT/nuwax_host_tests"

echo "Compiling host tests with $CC ..."
if ! "$CC" $CFLAGS "${INCLUDES[@]}" "${MODULES[@]}" "${TESTS[@]}" -o "$BIN"; then
  echo "BUILD FAILED"
  exit 2
fi

echo "Running ..."
echo
"$BIN"
rc=$?
echo
if [ $rc -eq 0 ]; then
  echo "ALL TESTS PASSED"
else
  echo "TESTS FAILED (exit $rc)"
fi
exit $rc
