#!/bin/bash
# service-down.sh — stop monado-service and all test clients plus their
# stdin-holder sleeps. Safe to run when nothing is running.
set -u
pkill -x monado-service 2>/dev/null || true
pkill -x hello_xr 2>/dev/null || true
# NB: the comm name is truncated to 15 chars, so -x cannot match
# "openxr-playground" (16); match the build-tree path instead.
pkill -f "openxr-simple-playground/build/openxr-playgroun[d]" 2>/dev/null || true
# Stale wrappers from older manual runs (their clients are long dead).
pkill -f "hello_xr-e2[e]|run_playgroun[d]" 2>/dev/null || true
pkill -f "pg-stdin-holde[r]" 2>/dev/null || true
pkill -f "hello-stdin-holde[r]" 2>/dev/null || true
sleep 3
# SIGTERM can take a while (full compositor teardown); escalate.
pkill -9 -x monado-service 2>/dev/null || true
pkill -9 -x hello_xr 2>/dev/null || true
pkill -9 -f "openxr-simple-playground/build/openxr-playgroun[d]" 2>/dev/null || true
pkill -9 -f "pg-stdin-holde[r]" 2>/dev/null || true
pkill -9 -f "hello-stdin-holde[r]" 2>/dev/null || true
sleep 1
rm -f /run/user/1000/monado.pid
leftover=$(pgrep -c -x monado-service 2>/dev/null || true)
# Bracket trick: pgrep -f would otherwise match its own command line.
leftover_apps=$(pgrep -c -f "[o]penxr-playground|[h]ello_xr" 2>/dev/null || true)
echo "service-down: monado-service=${leftover:-0} test-apps=${leftover_apps:-0}"
