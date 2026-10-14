#!/bin/sh
# Build wallet_tg_miner on Linux / macOS.
set -e

CXX="${CXX:-g++}"
SRC="wallet_tg_miner.cpp gpu_opencl.cpp monocypher.c"
OUT="wallet_tg_miner"

# Linux needs -ldl for dlopen (no-op on glibc >= 2.34); macOS has it in libSystem.
case "$(uname -s)" in
  Linux) LDLIBS="-ldl" ;;
  *)     LDLIBS="" ;;
esac

echo "Using: $CXX"
$CXX -std=c++17 -O3 -pthread $SRC -o "$OUT" $LDLIBS

echo "Built ./$OUT"
echo "Run it:  ./$OUT --selftest"
