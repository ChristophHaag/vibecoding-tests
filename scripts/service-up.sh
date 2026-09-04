#!/bin/bash
# service-up.sh — start monado-service (remote config) detached in its own
# session and wait until it listens on the remote-driver port.
#
# Usage: bash scripts/service-up.sh [logfile]
# Env:   XRT_COMPOSITOR_COMPUTE (default 1), XRT_LOG (default warn).
# Exit non-zero if the service does not come up within 30 s.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOG="${1:-/tmp/monado-service.log}"

bash "$ROOT/scripts/service-down.sh" >/dev/null 2>&1 || true
rm -f /run/user/1000/monado.pid
# The old service can linger in teardown while still holding port 4242;
# wait until it is really gone before binding a new one.
for _ in $(seq 1 75); do
	if ! pgrep -x monado-service >/dev/null 2>&1; then
		break
	fi
	sleep 0.2
done
if pgrep -x monado-service >/dev/null 2>&1; then
	echo "service-up: old monado-service refuses to die" >&2
	exit 1
fi
: > "$LOG"

setsid -f env P_OVERRIDE_ACTIVE_CONFIG=remote \
	XRT_COMPOSITOR_FORCE_XCB=1 \
	XRT_NO_STDIN=1 \
	"XRT_COMPOSITOR_COMPUTE=${XRT_COMPOSITOR_COMPUTE:-1}" \
	"XRT_LOG=${XRT_LOG:-warn}" \
	stdbuf -oL -eL "$ROOT/monado/build/src/xrt/targets/service/monado-service" \
	> "$LOG" 2>&1 < /dev/null

for _ in $(seq 1 150); do
	if grep -q "Listening on port '4242'" "$LOG" 2>/dev/null; then
		echo "service up (log: $LOG)"
		exit 0
	fi
	sleep 0.2
done
echo "service-up: timed out waiting for port 4242 (log: $LOG)" >&2
tail -5 "$LOG" >&2 || true
exit 1
