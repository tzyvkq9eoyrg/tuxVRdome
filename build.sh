#!/usr/bin/env bash
# Builds tuxvrdome into ./build and runs the automatic tests.
# The full compiler output is kept in build/last_build.log.
set -uo pipefail
cd "$(dirname "$0")"

if [ ! -f build/CMakeCache.txt ]; then
  cmake -S . -B build || exit 1
fi

cmake --build build -j > build/last_build.log 2>&1
status=$?
grep -E "error|warning" build/last_build.log

if [ $status -ne 0 ]; then
  echo "BUILD FAILED (full log: build/last_build.log)"
  exit 1
fi
echo "BUILD OK"

# The automatic checks (format detection, YouTube rules, links).
if ! ./build/tuxvrdome_tests >> build/last_build.log 2>&1; then
  grep FAILED build/last_build.log
  echo "TESTS FAILED (details: build/last_build.log)"
  exit 1
fi
echo "TESTS OK  –  start with: ./build/tuxvrdome [video]"
