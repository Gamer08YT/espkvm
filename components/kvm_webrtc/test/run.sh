#!/bin/sh
# SRTP, STUN, the H.264 packetizer and the SDP reader, on this machine, with
# AddressSanitizer watching every read.
set -e
here=$(dirname "$0")
out=$(mktemp -d)
cc -std=gnu11 -Wall -Wextra -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$here/.." -I "$here" "$here/test_webrtc.c" "$here/host_crypto.c" \
    "$here/../srtp.c" "$here/../stun.c" "$here/../rtp_h264.c" "$here/../sdp.c" "$here/../sps_patch.c" \
    -o "$out/test_webrtc"
"$out/test_webrtc"
rm -rf "$out"
