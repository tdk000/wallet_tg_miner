@echo off
chcp 65001 >nul
cd /d "%~dp0"
title vanity_tg

:menu
cls
echo   vanity_tg - generator of beautiful WalletTg addresses
echo   ---------------------------------------------------
echo.
echo     1.  Quick search  (subwallet mode, fast)
echo     2.  Mnemonic search (phrase mode, slow)
echo     3.  Self-test
echo     0.  Exit
echo.
set "CH="
set /p CH=Choose [0-3]: 
if "%CH%"=="1" goto quick
if "%CH%"=="2" goto phrase
if "%CH%"=="3" goto test
if "%CH%"=="0" exit /b 0
goto menu

:quick
echo.
set "SUF="
set /p SUF=Address ending (e.g. TG): 
if "%SUF%"=="" goto menu
echo.
vanity_tg.exe --mode subwallet --suffix %SUF%
echo.
pause
goto menu

:phrase
echo.
set "SUF="
set /p SUF=Address ending (e.g. TG): 
if "%SUF%"=="" goto menu
echo.
vanity_tg.exe --mode phrase --suffix %SUF%
echo.
pause
goto menu

:test
echo.
vanity_tg.exe --selftest
echo.
pause
goto menu
