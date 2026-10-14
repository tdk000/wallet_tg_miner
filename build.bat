@echo off
rem Build vanity_tg.exe on Windows.
rem Prefers zig (portable, no install), then falls back to MSVC cl.
setlocal

where zig >nul 2>nul
if %errorlevel%==0 (
  echo Using: zig
  zig cc -O3 -c monocypher.c -o monocypher.o || goto :fail
  zig c++ -std=c++17 -O3 -c vanity_tg.cpp -o vanity_tg.o || goto :fail
  zig c++ -O3 vanity_tg.o monocypher.o -o vanity_tg.exe -ladvapi32 || goto :fail
  goto :ok
)

where cl >nul 2>nul
if %errorlevel%==0 (
  echo Using: MSVC cl
  cl /nologo /std:c++17 /O2 /EHsc vanity_tg.cpp monocypher.c /Fe:vanity_tg.exe advapi32.lib || goto :fail
  goto :ok
)

echo No compiler found. Install zig (https://ziglang.org) or Visual Studio Build Tools.
exit /b 1

:ok
echo.
echo Built vanity_tg.exe
echo Run it:  vanity_tg.exe --selftest
exit /b 0

:fail
echo Build failed.
exit /b 1
