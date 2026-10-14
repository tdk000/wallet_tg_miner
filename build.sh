#!/bin/sh
# Build wallet_tg_miner on Linux / macOS.
set -e

CXX="${CXX:-g++}"
SRC="wallet_tg_miner.cpp monocypher.c"
OUT="wallet_tg_miner"

echo "Using: $CXX"
$CXX -std=c++17 -O3 -pthread $SRC -o "$OUT"

echo "Built ./$OUT"
echo "Run it:  ./$OUT --selftest"
