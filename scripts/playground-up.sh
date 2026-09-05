#!/bin/bash
# playground-up.sh — launch openxr-playground detached (own session) with a
# blocking stdin holder (the app stalls early on EOF stdin) and wait for the
# session to reach FOCUSED.
#
# Usage: bash scripts/playground-up.sh [logfile]
# Env:   PLAYGROUND_NO_QUAD (default 1: single projection layer, so the
#          compositor takes the single-layer fast path incl. openwarp;
#          unset it entirely to submit the quad too — the compute
#          openwarp path composites it via its quad overlay pass),
#        PLAYGROUND_FRAME_MS (default 50: per-frame pacing; the test hook
#          replaces the stock sleep(1.5)).
# Exit non-zero if FOCUSED is not reached within 90 s.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOG="${1:-/tmp/playground.log}"
: > "$LOG"

export PGQ="${PLAYGROUND_NO_QUAD:-1}" PGMS="${PLAYGROUND_FRAME_MS:-50}"
# Only one instance can hold the session; kill stale ones first (the comm
# name is truncated to 15 chars so -x cannot match "openxr-playground").
pkill -f "openxr-simple-playground/build/openxr-playgroun[d]" 2>/dev/null || true
pkill -f "pg-stdin-holde[r]" 2>/dev/null || true
sleep 1
setsid -f bash -c 'exec -a pg-stdin-holder sleep infinity | PLAYGROUND_NO_QUAD="${PGQ:-1}" PLAYGROUND_FRAME_MS="${PGMS:-50}" stdbuf -oL -eL "$0" >"$1" 2>&1' \
	"$ROOT/openxr-simple-playground/build/openxr-playground" "$LOG" \
	< /dev/null > /dev/null 2>&1

for _ in $(seq 1 450); do
	if grep -q "state changed from 4 to 5" "$LOG" 2>/dev/null; then
		echo "playground FOCUSED (log: $LOG)"
		exit 0
	fi
	if grep -q "failed to end frame" "$LOG" 2>/dev/null; then
		echo "playground-up: xrEndFrame failed (log: $LOG)" >&2
		exit 1
	fi
	sleep 0.2
done
echo "playground-up: timed out waiting for FOCUSED (log: $LOG)" >&2
tail -5 "$LOG" >&2 || true
exit 1
