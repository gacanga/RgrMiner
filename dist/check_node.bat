@echo off
REM RgrMiner - checks the link with your node with the settings of rgrminer.conf:
REM   1) getblocktemplate in "proposal" mode: the node checks a full block built by RgrMiner
REM      (everything except the proof of work) - expected answer: null
REM   2) recomputes the proof of work of the last 5 blocks of the chain
setlocal EnableExtensions
title RgrMiner - check node
cd /d "%~dp0"
"%~dp0rgrminer.exe" --config "%~dp0rgrminer.conf" --propose
"%~dp0rgrminer.exe" --config "%~dp0rgrminer.conf" --check-chain 5
pause
