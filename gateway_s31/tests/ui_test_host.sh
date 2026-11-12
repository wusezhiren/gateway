#!/usr/bin/env bash
set -euo pipefail
root="$(dirname "$(dirname "$(realpath "$0")")")"
build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"$root/tests/stubs" -I"$root/main" "$root/tests/ui_host_test.c" "$root/main/s31_ui.c" \
  -o "$build/ui_host_test"
"$build/ui_host_test"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"$root/main" "$root/tests/screen_policy_test.c" "$root/main/s31_screen_policy.c" \
  -o "$build/screen_policy_test"
"$build/screen_policy_test"
