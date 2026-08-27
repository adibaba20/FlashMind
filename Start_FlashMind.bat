@echo off
title FlashMind Launcher
color 0B

set "ROOT=%~dp0"

echo.
echo ==========================================
echo          FLASHMIND - STARTING
echo ==========================================
echo.

echo [1/4] Cleaning old processes...

for /f "tokens=5" %%P in ('netstat -ano ^| findstr ":5036" ^| findstr "LISTENING"') do taskkill /PID %%P /F >nul 2>&1
for /f "tokens=5" %%P in ('netstat -ano ^| findstr ":5173" ^| findstr "LISTENING"') do taskkill /PID %%P /F >nul 2>&1

echo Done.
echo.

echo [2/4] Starting FlashMind API...

start "FlashMind API" cmd /k "cd /d "%ROOT%FlashMind.Api" && dotnet run"

echo [3/4] Starting React frontend...

start "FlashMind Frontend" cmd /k "cd /d "%ROOT%frontend" && npm run dev -- --host 127.0.0.1 --port 5173"

echo.
echo [4/4] Waiting for services...
timeout /t 6 /nobreak >nul

echo.
echo ==========================================
echo       FLASHMIND IS STARTING
echo ==========================================
echo.
echo API:      http://localhost:5036
echo Frontend: http://localhost:5173
echo.

start "" "http://localhost:5173"

echo Dashboard opened.
echo.
timeout /t 3 /nobreak >nul
exit