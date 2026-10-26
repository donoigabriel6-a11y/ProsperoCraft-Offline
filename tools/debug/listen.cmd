@echo off
REM runs listen.ps1 with the execution policy bypassed; extra arguments are passed through (e.g. listen.cmd -Include "perf:" or listen.cmd -Kill)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0listen.ps1" %*
