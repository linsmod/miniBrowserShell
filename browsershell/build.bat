@echo off
setlocal
cd /d "%~dp0"

where cmake >nul 2>nul
if errorlevel 1 (
  echo [build] cmake not found in PATH
  exit /b 1
)

cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64
if errorlevel 1 exit /b 1

cmake --build build-msvc --config Release
if errorlevel 1 exit /b 1

echo.
echo [build] done: %~dp0build-msvc\Release\minishell.exe
