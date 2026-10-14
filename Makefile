# Makefile for vanity_tg  (Linux / macOS / MinGW / MSYS2)
#
#   make            build ./vanity_tg
#   make clean      remove binaries / objects
#
# For MSVC use build.bat instead.

CXX      ?= c++
CXXFLAGS ?= -O3 -std=c++17 -Wall -Wextra
LDLIBS   ?= -pthread

# Windows (MinGW / MSYS2) needs advapi32 for RtlGenRandom and a .exe suffix.
ifeq ($(OS),Windows_NT)
  LDLIBS += -ladvapi32
  BIN := vanity_tg.exe
else
  BIN := vanity_tg
endif

HDRS := monocypher.h bip39_english.h

all: $(BIN)

$(BIN): vanity_tg.cpp monocypher.c $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ vanity_tg.cpp monocypher.c $(LDLIBS)

clean:
	rm -f vanity_tg vanity_tg.exe *.o

.PHONY: all clean
