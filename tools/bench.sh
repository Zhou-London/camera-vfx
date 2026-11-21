#!/usr/bin/env bash
# Steady-state latency runs on the real capture device. Usage: tools/bench.sh [seconds]
set -u
D=${1:-30}
mkdir -p out/bench
run() { name=$1; shift; echo "== $name: $*"; timeout $((D + 40)) ./build/camss.exe "$@" --duration "$D" --warmup 10 --report 1000 --csv "out/bench/$name.csv" > "out/bench/$name.log" 2>&1; tail -n 40 "out/bench/$name.log" | grep -v "^\s*$" > "out/bench/$name.summary"; }
run pass_vrr      --mode passthrough --sync 0
run pass_vsync    --mode passthrough --sync 1
run fg_vrr_k2     --mode vrr --k 2 --interp vfx --vfg-mode 0
run fg_med_vrr_k2 --mode vrr --k 2 --interp vfx --vfg-mode 1
run full_vrr_k2   --mode vrr --k 2 --interp vfx --sr vfx --hdr vfx
run full_vrr_k3   --mode vrr --k 3 --interp vfx --sr vfx --hdr vfx
run full_fixed    --mode fixed --interp vfx --sr vfx --hdr vfx
run rtx_vrr_k2    --mode vrr --k 2 --interp vfx --sr rtx --hdr rtx
echo done
