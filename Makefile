# Makefile for wallet_tg_miner  (Linux / macOS / MinGW / MSYS2)
#
#   make            build ./wallet_tg_miner
#   make clean      remove binaries / objects
#
# For MSVC use build.bat instead.

CXX      ?= c++
CXXFLAGS ?= -O3 -std=c++17 -Wall -Wextra
LDLIBS   ?= -pthread

# Windows (MinGW / MSYS2) needs advapi32 for RtlGenRandom and a .exe suffix.
ifeq ($(OS),Windows_NT)
  LDLIBS += -ladvapi32
  BIN := wallet_tg_miner.exe
else
  # Linux needs -ldl for dlopen; macOS has it in libSystem.
  ifeq ($(shell uname -s),Linux)
    LDLIBS += -ldl
  endif
  BIN := wallet_tg_miner
endif

HDRS := monocypher.h bip39_english.h ocl.h gpu_opencl.h gpu_kernels.h

all: $(BIN)

$(BIN): wallet_tg_miner.cpp gpu_opencl.cpp monocypher.c $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ wallet_tg_miner.cpp gpu_opencl.cpp monocypher.c $(LDLIBS)

clean:
	rm -f wallet_tg_miner wallet_tg_miner.exe *.o

.PHONY: all clean
