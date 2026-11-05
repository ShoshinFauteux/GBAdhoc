#!/usr/bin/env bash
# ppsspp_online.sh RELAY_BIN RUNNER OUT TIMEOUT_S JOB_A [JOB_B]
#
# Online play in PPSSPP, nothing on the real network: a private user/net/pid/
# mount namespace (only loopback), gbadhoc-relay on 127.0.0.1, then the Lab's
# PPSSPP runner (RUNNER = backend/suites/wsl/run_ppsspp.sh) inside the SAME
# namespace, so the emulated consoles' sceNetInet sockets reach the relay.
# Stage the job dirs with:  python -m suites run <case> --eboot DIR --dry
#   --keys online_relay=127.0.0.1:27420,online_room=XXXX
# The relay log lands beside OUT (OUT.relay.log).
# RELAY_ARGS: extra relay arguments, e.g. a desktop host (GBAdhoc XMB's mode):
#   RELAY_ARGS="--home QWER --public 127.0.0.1"  (the log prints the invite code;
#   stage the jobs with online_room=<that invite code>: no room code, no directory)
set -uo pipefail
if [ "${LAB_SUITES_NS:-}" != 1 ]; then
  exec unshare -rmnpf --mount-proc --propagation private env LAB_SUITES_NS=1 bash "$0" "$@"
fi
RELAY="$1"; RUNNER="$2"; OUT="$3"; shift 3
ip link set lo up
# shellcheck disable=SC2086
"$RELAY" --bind 127.0.0.1 ${RELAY_ARGS:-} > "$OUT.relay.log" 2>&1 &
RP=$!
sleep 0.5
bash "$RUNNER" "$OUT" "$@"
rc=$?
kill "$RP" 2>/dev/null
exit $rc
