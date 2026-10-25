@echo off
REM RgrMiner - hashrate benchmark of every GPU for 60 s (stop mining first).
setlocal EnableExtensions
title RgrMiner - bench
cd /d "%~dp0"
set CUDA_DEVICE_ORDER=PCI_BUS_ID
"%~dp0rgrminer.exe" --bench 60
pause
