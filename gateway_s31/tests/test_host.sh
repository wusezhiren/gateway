#!/usr/bin/env bash
set -euo pipefail
root="$(dirname "$(dirname "$(realpath "$0")")")"
build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT
cc -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  "$root/tests/msgbus_host_test.c" -o "$build/msgbus_host_test"
"$build/msgbus_host_test"
