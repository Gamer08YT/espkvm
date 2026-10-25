#!/bin/sh
# Build and run the settings tests on this machine - no device needed. The
# pin-conflict rule is a set comparison over small structs. The profiles run
# the real settings store and table over a fake NVS (stub/, fake_nvs.c); they
# need cJSON, which an ESP-IDF build of the firmware has fetched.
set -e
here=$(dirname "$0")
out=$(mktemp -d)
cc -std=c11 -Wall -Wextra -O2 -I "$here/.." \
   "$here/test_pin_conflict.c" "$here/../pin_conflict.c" -o "$out/test_pin_conflict"
"$out/test_pin_conflict"

cjson=${CJSON_DIR:-$here/../../../managed_components/espressif__cjson/cJSON}
if [ -f "$cjson/cJSON.c" ]; then
    cc -std=gnu11 -Wall -Wextra -O2 -I "$here/stub" -I "$here/.." -I "$here/../include" \
       -I "$here/../../kvm_board/include" -I "$cjson" \
       "$here/test_profiles.c" "$here/fake_nvs.c" "$here/../kvm_settings.c" \
       "$here/../kvm_settings_table.c" "$here/../kvm_profiles.c" "$here/../profile_match.c" \
       "$here/../pin_conflict.c" "$cjson/cJSON.c" -o "$out/test_profiles"
    "$out/test_profiles"
else
    echo "profiles: skipped, no cJSON at $cjson (build the firmware once, or set CJSON_DIR)"
fi
rm -rf "$out"
