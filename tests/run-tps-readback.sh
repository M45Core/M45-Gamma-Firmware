#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd -- "$(dirname -- "$0")/.." && pwd)"
test_bin="$(mktemp /tmp/m45-tps-readback.XXXXXX)"
trap 'rm -f "$test_bin"' EXIT
cc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=address,undefined \
    -I"$repo_dir/tests/hardware_read_stubs" -I"$repo_dir/tests/diagnostic_stubs" -I"$repo_dir/main" \
    "$repo_dir/tests/tps_readback_test.c" -lm -o "$test_bin"
"$test_bin"
