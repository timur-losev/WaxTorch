@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..") do set "REPO_ROOT=%%~fI"

set "BUILD_DIR=%REPO_ROOT%\cpp\build"
set "CONFIG=Debug"
set "TARGET="
set "JOBS=%NUMBER_OF_PROCESSORS%"
if not defined JOBS set "JOBS=8"

:parse_args
if "%~1"=="" goto run

if /I "%~1"=="--help" goto usage_ok
if /I "%~1"=="-h" goto usage_ok

if /I "%~1"=="--build-dir" (
  if "%~2"=="" goto missing_value
  set "BUILD_DIR=%~2"
  shift
  shift
  goto parse_args
)

if /I "%~1"=="--config" (
  if "%~2"=="" goto missing_value
  set "CONFIG=%~2"
  shift
  shift
  goto parse_args
)

if /I "%~1"=="--target" (
  if "%~2"=="" goto missing_value
  set "TARGET=%~2"
  shift
  shift
  goto parse_args
)

if /I "%~1"=="--jobs" (
  if "%~2"=="" goto missing_value
  set "JOBS=%~2"
  shift
  shift
  goto parse_args
)

echo [ERROR] Unknown argument: %~1
echo.
goto usage_err

:missing_value
echo [ERROR] Missing value for argument: %~1
echo.
goto usage_err

:run
if not exist "%BUILD_DIR%\CMakeCache.txt" (
  echo [ERROR] CMake cache not found: "%BUILD_DIR%\CMakeCache.txt"
  echo         Run configure first: scripts\generate-cmake.bat
  exit /b 1
)

echo [waxcpp] Build
echo   build : "%BUILD_DIR%"
echo   config: "%CONFIG%"
echo   jobs  : "%JOBS%"
if defined TARGET (
  echo   target: "%TARGET%"
) else (
  echo   target: all
)

if defined TARGET (
  cmake --build "%BUILD_DIR%" --config "%CONFIG%" --target "%TARGET%" --parallel "%JOBS%"
) else (
  cmake --build "%BUILD_DIR%" --config "%CONFIG%" --parallel "%JOBS%"
)

if errorlevel 1 (
  echo [ERROR] Build failed.
  exit /b 1
)

echo [OK] Build completed.
exit /b 0

:usage_ok
set "USAGE_CODE=0"
goto usage

:usage_err
set "USAGE_CODE=1"
goto usage

:usage
if not defined USAGE_CODE set "USAGE_CODE=1"
echo Usage:
echo   scripts\build.bat [options]
echo.
echo Options:
echo   --build-dir PATH      Build directory (default: cpp\build)
echo   --config Debug^|Release
echo                          Configuration (default: Debug)
echo   --target NAME         Build specific target (optional)
echo   --jobs N              Parallel build jobs (default: %%NUMBER_OF_PROCESSORS%%)
echo   --help                Show this message
echo.
echo Examples:
echo   scripts\build.bat
echo   scripts\build.bat --config Release
echo   scripts\build.bat --target waxcpp_rag_server --config Debug
echo   scripts\build.bat --target run_rag_server --jobs 16
exit /b %USAGE_CODE%
