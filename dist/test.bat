@echo off
REM RgrMiner - correctness tests: reference vectors on the CPU, then every GPU against the
REM CPU reference (digests and nonce scans). Stop mining first.
setlocal EnableExtensions
title RgrMiner - test
cd /d "%~dp0"
set CUDA_DEVICE_ORDER=PCI_BUS_ID
"%~dp0rgrminer.exe" --test
echo.
echo Test exit code %ERRORLEVEL% (0 = all OK).
pause
