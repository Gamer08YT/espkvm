#!/bin/sh
# Build and run the region tests on this machine - no device needed.
set -e
here=$(dirname "$0")
out=$(mktemp -d)
cc -std=c11 -Wall -Wextra -O2 -I "$here/../include" \
   "$here/test_regions.c" "$here/../region_core.c" -o "$out/test_regions"
"$out/test_regions"
rm -rf "$out"
