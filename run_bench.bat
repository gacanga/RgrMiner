@echo off
REM run_bench.bat - hashrate benchmark of dev\ (options in run\bench_args.txt, e.g. --gpus 0 --bench 60)
REM   Log: logs\bench.log, history in logs\bench_history.log
setlocal EnableExtensions
cd /d "%~dp0"
if not exist logs mkdir logs
set "PATH=%CD%\dev;C:\msys64\mingw64\bin;%PATH%"
set CUDA_DEVICE_ORDER=PCI_BUS_ID
set "ARGS=--bench 30"
if exist run\bench_args.txt set "ARGS=--args-file run\bench_args.txt"
echo [%date% %time%] bench %ARGS%> logs\bench.log
type run\nvcc_extra.txt >> logs\bench.log 2>nul
dev\rgrminer.exe %ARGS% >> logs\bench.log 2>&1
echo exit=%ERRORLEVEL%>> logs\bench.log
type logs\bench.log >> logs\bench_history.log
