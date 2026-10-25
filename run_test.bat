@echo off
REM run_test.bat - correctness tests of dev\ (CPU vectors, GPU digests and scans against the
REM   CPU reference).  Options in run\test_args.txt (e.g. --gpus 0).  Log: logs\test.log
setlocal EnableExtensions
cd /d "%~dp0"
if not exist logs mkdir logs
set "PATH=%CD%\dev;C:\msys64\mingw64\bin;%PATH%"
set CUDA_DEVICE_ORDER=PCI_BUS_ID
set "ARGS="
if exist run\test_args.txt set "ARGS=--args-file run\test_args.txt"
echo [%date% %time%] test %ARGS%> logs\test.log
dev\rgrminer.exe --test %ARGS% >> logs\test.log 2>&1
echo exit=%ERRORLEVEL%>> logs\test.log
