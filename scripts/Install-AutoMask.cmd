@echo off
REM Double-clickable installer: builds Release, then installs.
REM
REM -Force is deliberately NOT passed. It would close DaVinci Resolve for you,
REM which is a rude thing to do to someone who double-clicked a file. If the copy
REM fails because Resolve has an older build locked, the script says so and you
REM can close it yourself.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Build
pause

