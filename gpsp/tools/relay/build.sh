#!/usr/bin/env bash
# tools/relay/build.sh OUT_DIR -- desktop (Linux/WSL) builds of the online-play
# pieces (docs/ONLINE-TUNNEL.md): the GBR1 relay server, the netdrv test
# client, and the in-process NAT-model test.  Then: OUT_DIR/test_relay_nat.
set -euo pipefail
OUT="${1:-/tmp/gbrelay}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ND="$HERE/../../netdrv"
mkdir -p "$OUT"
CF=(-std=gnu11 -O1 -g -Wall -Wextra -I"$ND")
SAN=(-fsanitize=address,undefined)
gcc "${CF[@]}" "${SAN[@]}" -o "$OUT/test_relay_nat" "$HERE/test_relay_nat.c" \
    "$ND/relay_core.c" "$ND/transport_relay.c" "$ND/netdrv.c"
gcc "${CF[@]}" -o "$OUT/gbadhoc-relay" "$HERE/relay_server.c" "$ND/relay_core.c"
gcc "${CF[@]}" -o "$OUT/relay_client" "$HERE/relay_client.c" \
    "$ND/transport_relay.c" "$ND/netdrv.c"
echo "built $OUT/{test_relay_nat,gbadhoc-relay,relay_client}"
