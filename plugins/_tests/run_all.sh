#!/usr/bin/env bash
# Compila los 22 plugins LooperX (nativo + ARM32 + ARM64, skins, validación) y pasa todas las pruebas.
# Uso (desde la raíz del SDK):  bash plugins/_tests/run_all.sh [--no-build]
set -u
cd "$(dirname "$0")/../.."
export ZIG="${ZIG:-zig}"
export LOOPERX_EXPORT_ROOT=/tmp/lx_test_exports   # que las pruebas no escriban WAV en el proyecto
mkdir -p "$LOOPERX_EXPORT_ROOT" plugins/_tests/bin
FAILS=0
note() { printf '%-28s %s\n' "$1" "$2"; }

if [ "${1:-}" != "--no-build" ]; then
  for d in plugins/LooperX*/; do
    d=${d%/}
    if python3 tools/build.py "$d" > /tmp/lx_build.log 2>&1 && grep -qE '^0 errors' /tmp/lx_build.log; then
      note "$(basename "$d")" "build + validate OK ($(grep -E '^[0-9]+ errors' /tmp/lx_build.log))"
    else
      note "$(basename "$d")" "BUILD/VALIDATE FAIL"; tail -5 /tmp/lx_build.log; FAILS=$((FAILS+1))
    fi
  done
fi

echo
echo "== host_sim (el simulador del MPC del SDK) =="
for d in plugins/LooperX*/; do
  so=$(ls "$d"build/native/*.so 2>/dev/null | head -1)
  [ -z "$so" ] && continue
  args=""
  case "$d" in *Granular*) args="--notes 48,60,67";; esac
  r=$(.sdk-tools/host_sim "$so" test $args 2>&1 | grep -E '^Result')
  w=$(.sdk-tools/host_sim "$so" test $args 2>&1 | grep -E '^(WARN|FAIL)' | head -1)
  note "$(basename "$d")" "$r ${w}"
  echo "$r" | grep -q "failures)" && ! echo "$r" | grep -q " 0 failures" && FAILS=$((FAILS+1))
done

echo
echo "== pruebas propias =="
CXX="g++ -std=c++17 -O2 -g -Wall -Wextra -Iframework -Iplugins/_tests"
$CXX -o plugins/_tests/bin/looper_core_test plugins/_tests/looper_core_test.cpp -lpthread 2>&1 | grep -E "error" ; true
for t in looper_plugin_test link_test fx_test; do $CXX -o plugins/_tests/bin/$t plugins/_tests/$t.cpp -ldl -lpthread 2>&1 | grep -E "error"; done
for t in looper_core_test looper_plugin_test link_test fx_test; do
  out=$(./plugins/_tests/bin/$t 2>&1 | tail -1)
  note "$t" "$out"
  echo "$out" | grep -q "(0 fallos)" || FAILS=$((FAILS+1))
done

echo
if [ $FAILS -eq 0 ]; then echo "TODO OK"; else echo "FALLOS: $FAILS"; fi
exit $FAILS
