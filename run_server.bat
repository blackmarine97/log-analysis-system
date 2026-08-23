@echo off
rem Double-click to start the Linux server inside WSL from Windows.
rem Works from a \\wsl.localhost\... path or a Windows-side clone; WSL
rem translates the Windows path itself (no wslpath round-trip needed).
rem Optional: pass extra log_server options, e.g. run_server.bat --port 6000
setlocal
set "HERE=%~dp0"
if "%HERE:~-1%"=="\" set "HERE=%HERE:~0,-1%"
echo Starting log_server in WSL from "%HERE%" ...  Press Ctrl+C to stop.
wsl.exe --cd "%HERE%" -- ./bin/log_server --csv result.csv %*
echo.
echo log_server exited with code %ERRORLEVEL%.
pause
