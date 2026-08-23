@echo off
rem Double-click to start the Linux server inside WSL from Windows.
rem Works from the repository root (binary in .\bin) or from inside bin\
rem (binary next to this script), and from a \\wsl.localhost\... path.
rem Optional: pass extra log_server options, e.g. run_server.bat --port 6000
setlocal
set "HERE=%~dp0"
if "%HERE:~-1%"=="\" set "HERE=%HERE:~0,-1%"
set "BIN=./bin/log_server"
if exist "%HERE%\log_server" set "BIN=./log_server"
echo Starting log_server in WSL from "%HERE%" ...  Press Ctrl+C to stop.
wsl.exe --cd "%HERE%" -- %BIN% --csv result.csv %*
echo.
echo log_server exited with code %ERRORLEVEL%.
pause
