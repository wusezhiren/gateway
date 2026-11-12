#!/usr/bin/env bash
set -euo pipefail
root="$(dirname "$(dirname "$(realpath "$0")")")"
json="$root/../../gateway/managed_components/espressif__cjson/cJSON"
build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"$root/include" "$root/tests/voice_core_test.c" \
  "$root/voice_core.c" -o "$build/voice_core_test"
"$build/voice_core_test"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"$root/include" -I"$json" "$root/tests/voice_mcp_json_test.c" \
  "$root/voice_core.c" "$root/voice_mcp_json.c" "$json/cJSON.c" -o "$build/voice_mcp_json_test"
"$build/voice_mcp_json_test"
