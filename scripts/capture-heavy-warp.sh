#!/bin/bash
# capture-heavy-warp.sh — capture heavily-warped compositor frames with a
# reproducible head-motion program, plus a settled fresh reference.
#
# Background: the compositor only presents on client frame commits, so a
# SIGSTOP-frozen client yields a frozen mirror no matter how the head moves
# (verified: 30 s dwell, 0.3 m + 20-degree motion, zero pixel change) and
# freezing additionally risks Monado's known frame-pacing recovery stall.
# Heavy warp therefore needs a stale-but-alive client: the playground's
# PLAYGROUND_FRAME_MS hook (e.g. 100..1000) makes every submitted frame
# lag its warp pose by up to the frame interval.
#
# Usage:
#   bash scripts/capture-heavy-warp.sh <x0> <dx> <n> <step-wait> <out-prefix> [settle=25]
#   Moves the head from x0 in n steps of dx (metres, y=1.7 z=0, identity
#   rotation), waiting step-wait seconds after each send and screenshotting
#   immediately (the frames most heavily loaded are the mid-motion ones).
#   Then holds the final pose for `settle` seconds and captures the fresh
#   reference twice (checks bit-stability).
#   Screenshots: <out-prefix>_motNN.png, <out-prefix>_fresh.png[_2].
#
# Prints md5sums and status lines only (no image interpretation); pair with
# scripts/analyze-warp.py for numeric artifact metrics.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
X0="$1"; DX="$2"; N="$3"; WAIT="$4"; OUT="$5"; SETTLE="${6:-25}"

shot() { import -window "Monado" "$1"; }
send_head() { printf 'head %s 1.7 0 0 0 0 1\nsend\nquit\n' "$1" \
	| "$ROOT/remote-driver-client/build/monado-remote-client" > /dev/null 2>&1; }

i=1
while [ "$i" -le "$N" ]; do
	X="$(python3 -c "print(round($X0 + $i * $DX, 4))")"
	send_head "$X"
	sleep "$WAIT"
	shot "$(printf '%s_mot%02d.png' "$OUT" "$i")"
	echo "mot$(printf '%02d' "$i") head_x=$X: $(md5sum < "$(printf '%s_mot%02d.png' "$OUT" "$i")" | cut -c1-8)"
	i=$((i + 1))
done

echo "holding final pose ${SETTLE}s for fresh settle..."
sleep "$SETTLE"
shot "${OUT}_fresh.png"
sleep 4
shot "${OUT}_fresh2.png"
echo "fresh:  $(md5sum < "${OUT}_fresh.png" | cut -c1-8)"
echo "fresh2: $(md5sum < "${OUT}_fresh2.png" | cut -c1-8)"
if cmp -s "${OUT}_fresh.png" "${OUT}_fresh2.png"; then
	echo "fresh frames: STABLE (bit-identical)"
else
	echo "fresh frames: STILL SETTLING (differ)"
fi
