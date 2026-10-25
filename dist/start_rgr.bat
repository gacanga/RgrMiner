@echo off
REM RgrMiner - mines RIGOR (ORU) solo on your local node with the settings of rgrminer.conf.
REM Edit rgrminer.conf first: your rg1q address and the path of the node rpc-auth.conf.
REM Restarts the miner 10 s after any exit; close this window to stop.
setlocal EnableExtensions
title RgrMiner - miner
cd /d "%~dp0"
findstr /c:"YOUR_RIGOR_" "%~dp0rgrminer.conf" >nul
if not errorlevel 1 (
    echo.
    echo  Edit rgrminer.conf: set your rg1q address and the path of rpc-auth.conf.
    echo  Modifiez rgrminer.conf : mettez votre adresse rg1q et le chemin de rpc-auth.conf.
    echo.
    pause
    exit /b 1
)
set CUDA_DEVICE_ORDER=PCI_BUS_ID
:loop
"%~dp0rgrminer.exe" --config "%~dp0rgrminer.conf" --log "%~dp0rgrminer.log"
echo RgrMiner exited (code %ERRORLEVEL%), restarting in 10 s - close this window to stop.
timeout /t 10 /nobreak >nul
goto loop
