cmake_minimum_required(VERSION 3.10)
project(wallet_tg_miner LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(Threads REQUIRED)

add_executable(wallet_tg_miner
  wallet_tg_miner.cpp
  monocypher.c
)

target_link_libraries(wallet_tg_miner PRIVATE Threads::Threads)

if(WIN32)
  target_link_libraries(wallet_tg_miner PRIVATE advapi32)
endif()

# Optional: build with zig for a fully static, dependency-free binary.
#   cmake -B build -DCMAKE_C_COMPILER="zig cc" -DCMAKE_CXX_COMPILER="zig c++"
