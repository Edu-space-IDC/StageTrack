@echo off
setlocal
cd /d "%~dp0"

where cmake >nul 2>nul
if errorlevel 1 (
  echo ERROR: CMake no esta en PATH.
  echo Abre Developer PowerShell for VS 2022 o agrega CMake al PATH.
  pause
  exit /b 1
)

cmake -S . -B build
if errorlevel 1 goto :failed
echo.
echo (Si arriba dice "ASIO: activado" el programa usa ASIO; si dice WARNING, falta el SDK en third_party\asiosdk)
echo.

cmake --build build --config Release
if errorlevel 1 goto :failed

echo.
echo Compilacion terminada: build\StageTracks_artefacts\Release\StageTracks.exe
start "" "%~dp0build\StageTracks_artefacts\Release\StageTracks.exe"
exit /b 0

:failed
echo.
echo La compilacion fallo. Copia el error de arriba para revisarlo.
pause
exit /b 1
