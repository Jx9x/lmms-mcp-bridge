@echo off
rem Launch LMMS with the agentbridge plugin loaded.
rem Pass-through args are forwarded to start-lmms-with-bridge.ps1, e.g.:
rem   start-lmms-with-bridge.bat -Project "C:\path\to\project.mmp"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\start-lmms-with-bridge.ps1" %*
set "rc=%errorlevel%"
if %rc% neq 0 pause
exit /b %rc%
