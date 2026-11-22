#!/usr/bin/env bash
set -u
D=${1:-30}
shift $(( $# > 0 ? 1 : 0 ))
EXTRA=("$@")
EXE=${CAMERAVFX_EXE:-./build/Release/CameraVFX.exe}
mkdir -p out/bench
run() {
  name=$1; shift
  echo "== $name: $* ${EXTRA[*]}"
  timeout $((D + 40)) "$EXE" --no-config --no-audio --ui plain "$@" "${EXTRA[@]}" --duration "$D" --warmup 10 --report 1000 \
    --csv "out/bench/$name.csv" > "out/bench/$name.log" 2>&1
  tail -n 40 "out/bench/$name.log" | grep -v "^\s*$" > "out/bench/$name.summary"
}
run pass_vrr      --mode passthrough --passthrough-sync 0
run pass_vsync    --mode passthrough --passthrough-sync 1
run fg_vrr_k2     --mode vrr --fg --fg-multiplier 2 --fg-quality low
run fg_med_vrr_k2 --mode vrr --fg --fg-multiplier 2 --fg-quality medium
run full_vrr_k2   --mode vrr --fg --fg-multiplier 2 --sr --hdr
run full_vrr_k3   --mode vrr --fg --fg-multiplier 3 --sr --hdr
run full_fixed    --mode fixed --fg --sr --hdr
echo done
