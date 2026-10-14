#!/bin/sh
# Build vanity_tg on Linux / macOS.
set -e

CXX="${CXX:-g++}"
SRC="vanity_tg.cpp monocypher.c"
OUT="vanity_tg"

echo "Using: $CXX"
$CXX -std=c++17 -O3 -pthread $SRC -o "$OUT"

echo "Built ./$OUT"
echo "Run it:  ./$OUT --selftest"
