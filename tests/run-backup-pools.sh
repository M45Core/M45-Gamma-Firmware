#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd -- "$(dirname -- "$0")/.." && pwd)"
idf_dir="${IDF_PATH:?Set IDF_PATH to your ESP-IDF checkout}"
test_bin="$(mktemp /tmp/m45-backup-test.XXXXXX)"
trap 'rm -f "$test_bin"' EXIT
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
    -I"$repo_dir/tests/stubs" -I"$repo_dir/main" -I"$idf_dir/components/json/cJSON" \
    "$repo_dir/tests/backup_pools_test.c" "$repo_dir/main/m45_backup_pools.c" \
    "$idf_dir/components/json/cJSON/cJSON.c" -lm -o "$test_bin"
"$test_bin"
