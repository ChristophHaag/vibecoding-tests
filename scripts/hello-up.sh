#!/bin/bash
# hello-up.sh — launch hello_xr detached (own session) with a blocking stdin
# holder (hello_xr exits on stdin EOF) and wait for swapchain creation.
#
# Usage: bash scripts/hello-up.sh [logfile]
# Env:   HELLO_ARGS (default "-G Vulkan2").
# Exit non-zero if swapchains are not created within 90 s.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOG="${1:-/tmp/hello_xr.log}"
: > "$LOG"

export HELLO_ARGS="${HELLO_ARGS:--G Vulkan2}"
# Only one instance drives the session cleanly; kill stale ones first.
pkill -x hello_xr 2>/dev/null || true
pkill -f "hello-stdin-holde[r]" 2>/dev/null || true
sleep 1
setsid -f bash -c 'exec -a hello-stdin-holder sleep infinity | stdbuf -oL -eL hello_xr $HELLO_ARGS >"$0" 2>&1' \
	"$LOG" \
	< /dev/null > /dev/null 2>&1

for _ in $(seq 1 450); do
	if grep -q "swapchain for view 1" "$LOG" 2>/dev/null; then
		echo "hello_xr up (log: $LOG)"
		exit 0
	fi
	sleep 0.2
done
echo "hello-up: timed out waiting for swapchains (log: $LOG)" >&2
tail -5 "$LOG" >&2 || true
exit 1
