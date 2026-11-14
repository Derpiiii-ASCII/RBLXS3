@echo off
rem Double-click or run from cmd: build.bat [all|self|pkg|clean|shell]
rem Installs Docker Desktop automatically if it's missing.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
if errorlevel 1 pause
