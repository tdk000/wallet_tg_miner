#!/bin/sh
# ===========================================================================
#  test-linux.sh - build wallet_tg_miner on Linux and verify the build.
#
#  Usage:
#      sh test-linux.sh               build + self-test + benchmark
#      sh test-linux.sh --search      the same, plus one real address search
#      CXX=clang++ sh test-linux.sh   use another C++17 compiler
#      OUT=wtm sh test-linux.sh       different output binary name
#
#  Exit code 0 means: the compiler produced a working binary, every built-in
#  test vector matched, and (with --search) an address was actually found.
#
#  Nothing is downloaded and nothing is written outside the source directory.
# ===========================================================================
set -u

CXX="${CXX:-g++}"
OUT="${OUT:-wallet_tg_miner}"
SRC="wallet_tg_miner.cpp gpu_opencl.cpp monocypher.c"
WANT_SEARCH=0
[ "${1:-}" = "--search" ] && WANT_SEARCH=1

fail() {
    printf '\nFAIL: %s\n' "$1" >&2
    exit 1
}

if ! command -v "$CXX" >/dev/null 2>&1; then
    printf 'error: %s not found.\n' "$CXX" >&2
    printf '       Install a C++17 compiler first, e.g.:  sudo apt install g++\n' >&2
    exit 1
fi

printf '== compiler ==\n'
"$CXX" --version | head -1
printf '== machine  ==\n'
uname -srm
printf '== cores    ==\n'
(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo '?')

# -ldl is only required for dlopen() on glibc < 2.34; harmless after that.
printf '\n== build ==\n'
# shellcheck disable=SC2086  # SRC is intentionally split into arguments
"$CXX" -std=c++17 -O3 -pthread $SRC -o "$OUT" -ldl || fail "compilation failed"
printf 'built ./%s\n' "$OUT"

printf '\n== self-test (reference vectors: SHA-2, PBKDF2, SLIP-0010, BIP39, TON) ==\n'
selftest_out=$("./$OUT" --selftest) || fail "self-test exited with an error"
printf '%s\n' "$selftest_out"
printf '%s\n' "$selftest_out" | grep -q 'all self-tests passed' \
    || fail "self-test did not report success"

printf '\n== benchmark ==\n'
"./$OUT" --bench || fail "benchmark exited with an error"

printf '\n== OpenCL (optional; CPU fallback is expected when absent) ==\n'
"./$OUT" --gpu-info || printf '(no OpenCL runtime: --gpu would fall back to the CPU)\n'

if [ "$WANT_SEARCH" = 1 ]; then
    printf '\n== search: phrase mode, suffix "AA" (importable into Telegram) ==\n'
    printf '   expected ~4.1e3 attempts; may take a minute or two on a slow CPU\n\n'
    if command -v timeout >/dev/null 2>&1; then
        timeout 900 "./$OUT" --mode phrase --suffix AA --quiet || fail "search failed"
    else
        "./$OUT" --mode phrase --suffix AA --quiet || fail "search failed"
    fi
fi

printf '\nOK: build and self-test passed on this machine.\n'
