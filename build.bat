@echo off
rem Build wallet_tg_miner.exe on Windows.
rem Prefers zig (portable, no install), then falls back to MSVC cl.
setlocal

where zig >nul 2>nul
if %errorlevel%==0 (
  echo Using: zig
  zig cc -O3 -c monocypher.c -o monocypher.o || goto :fail
  zig c++ -std=c++17 -O3 -c gpu_opencl.cpp -o gpu_opencl.o || goto :fail
  zig c++ -std=c++17 -O3 -c wallet_tg_miner.cpp -o wallet_tg_miner.o || goto :fail
  zig c++ -O3 wallet_tg_miner.o gpu_opencl.o monocypher.o -o wallet_tg_miner.exe -ladvapi32 || goto :fail
  goto :ok
)

where cl >nul 2>nul
if %errorlevel%==0 (
  echo Using: MSVC cl
  cl /nologo /std:c++17 /O2 /EHsc wallet_tg_miner.cpp gpu_opencl.cpp monocypher.c /Fe:wallet_tg_miner.exe advapi32.lib || goto :fail
  goto :ok
)

echo No compiler found. Install zig (https://ziglang.org) or Visual Studio Build Tools.
exit /b 1

:ok
echo.
echo Built wallet_tg_miner.exe
echo Run it:  wallet_tg_miner.exe --selftest
exit /b 0

:fail
echo Build failed.
exit /b 1
