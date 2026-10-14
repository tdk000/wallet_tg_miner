@echo off
chcp 65001 >nul
cd /d "%~dp0"
title wallet_tg_miner

:menu
cls
echo   wallet_tg_miner - generator of beautiful WalletTg addresses
echo   ---------------------------------------------------
echo.
echo     1.  Quick search  (subwallet mode, fast)
echo     2.  Mnemonic search (phrase mode, slow)
echo     3.  Mnemonic search (phrase mode, GPU if available)
echo     4.  GPU info / self-test of the GPU path
echo     5.  Self-test
echo     0.  Exit
echo.
set "CH="
set /p CH=Choose [0-5]: 
if "%CH%"=="1" goto quick
if "%CH%"=="2" goto phrase
if "%CH%"=="3" goto gpuphrase
if "%CH%"=="4" goto gpucheck
if "%CH%"=="5" goto test
if "%CH%"=="0" exit /b 0
goto menu

:quick
echo.
set "SUF="
set /p SUF=Address ending (e.g. TG): 
if "%SUF%"=="" goto menu
echo.
wallet_tg_miner.exe --mode subwallet --suffix %SUF%
echo.
pause
goto menu

:phrase
echo.
set "SUF="
set /p SUF=Address ending (e.g. TG): 
if "%SUF%"=="" goto menu
echo.
wallet_tg_miner.exe --mode phrase --suffix %SUF%
echo.
pause
goto menu

:gpuphrase
echo.
set "SUF="
set /p SUF=Address ending (e.g. TG): 
if "%SUF%"=="" goto menu
echo.
wallet_tg_miner.exe --gpu --suffix %SUF%
echo.
pause
goto menu

:gpucheck
echo.
wallet_tg_miner.exe --gpu-info
echo.
wallet_tg_miner.exe --gpu-test
echo.
pause
goto menu

:test
echo.
wallet_tg_miner.exe --selftest
echo.
pause
goto menu
