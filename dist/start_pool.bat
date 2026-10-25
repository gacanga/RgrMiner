@echo off
REM RgrMiner - mines RIGOR (ORU) on the Gozero pool with the settings of rgrminer_pool.conf.
REM Edit rgrminer_pool.conf first: replace YOUR_RIGOR_ADDRESS with your rg1q address.
REM Restarts the miner 10 s after any exit; close this window to stop.
setlocal EnableExtensions
title RgrMiner - pool
cd /d "%~dp0"
findstr /c:"YOUR_RIGOR_ADDRESS" "%~dp0rgrminer_pool.conf" >nul
if not errorlevel 1 (
    echo.
    echo  Edit rgrminer_pool.conf: replace YOUR_RIGOR_ADDRESS with your rg1q address.
    echo  Modifiez rgrminer_pool.conf : remplacez YOUR_RIGOR_ADDRESS par votre adresse rg1q.
    echo.
    pause
    exit /b 1
)
set CUDA_DEVICE_ORDER=PCI_BUS_ID
:loop
"%~dp0rgrminer.exe" --config "%~dp0rgrminer_pool.conf" --log "%~dp0rgrminer_pool.log"
echo RgrMiner exited (code %ERRORLEVEL%), restarting in 10 s - close this window to stop.
timeout /t 10 /nobreak >nul
goto loop
