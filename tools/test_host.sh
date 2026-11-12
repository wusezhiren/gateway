#!/usr/bin/env bash
set -euo pipefail
root="$(dirname "$(dirname "$(realpath "$0")")")"
build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"$root/components/meshproto/tests/stubs" -I"$root/components/meshproto/include" \
  "$root/components/meshproto/tests/meshproto_test.c" \
  "$root/components/meshproto/meshproto.c" -o "$build/meshproto_test"
"$build/meshproto_test"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"$root/gateway/main" "$root/gateway/tests/tts_resample_test.c" \
  "$root/gateway/main/tts_resample.c" -o "$build/tts_resample_test"
"$build/tts_resample_test"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"$root/gateway/main" "$root/gateway/tests/voice_vad_test.c" \
  "$root/gateway/main/voice_vad.c" -o "$build/voice_vad_test"
"$build/voice_vad_test"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"$root/gateway/main" "$root/gateway/tests/voice_session_test.c" \
  "$root/gateway/main/voice_session.c" -o "$build/voice_session_test"
"$build/voice_session_test"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"$root/gateway/main" "$root/gateway/tests/gw_set_policy_test.c" \
  "$root/gateway/main/gw_set_policy.c" -o "$build/gw_set_policy_test"
"$build/gw_set_policy_test"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"$root/node/main" "$root/node/tests/node_ota_policy_test.c" \
  "$root/node/main/node_ota_policy.c" -o "$build/node_ota_policy_test"
"$build/node_ota_policy_test"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"$root/gateway/main" "$root/gateway/tests/gw_ota_policy_test.c" \
  "$root/gateway/main/gw_ota_policy.c" -o "$build/gw_ota_policy_test"
"$build/gw_ota_policy_test"
