cmake_minimum_required(VERSION 3.22)
project(wallet_tg_miner C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# Release optimisation (AGP may otherwise inject -O2 / RelWithDebInfo).
set(CMAKE_C_FLAGS_RELEASE   "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

# The shared C++ core lives at the repository root: android/app/src/main/cpp -> 5 up.
get_filename_component(WTM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)

add_library(wallet_tg_miner SHARED
    ${WTM_ROOT}/wallet_tg_miner.cpp
    ${WTM_ROOT}/gpu_opencl.cpp
    ${WTM_ROOT}/monocypher.c
    jni_bridge.cpp
)

# In this build the CLI main() is replaced by the embeddable C API.
set_source_files_properties(${WTM_ROOT}/wallet_tg_miner.cpp
    PROPERTIES COMPILE_DEFINITIONS "WTM_LIB")

target_include_directories(wallet_tg_miner PRIVATE
    ${WTM_ROOT}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

find_library(log-lib log)
target_link_libraries(wallet_tg_miner ${log-lib} dl)
