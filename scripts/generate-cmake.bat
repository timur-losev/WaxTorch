@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..") do set "REPO_ROOT=%%~fI"

set "SOURCE_DIR=%REPO_ROOT%\cpp"
set "BUILD_DIR=%SOURCE_DIR%\build"
set "GENERATOR=Visual Studio 17 2022"
set "ARCH=x64"
set "CONFIG=Debug"
set "TOOLSET="
set "BUILD_TESTS=ON"
set "REQUIRE_PARITY_FIXTURES=OFF"
set "REQUIRE_SWIFT_FIXTURES=OFF"

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

if /I "%~1"=="--generator" (
  if "%~2"=="" goto missing_value
  set "GENERATOR=%~2"
  shift
  shift
  goto parse_args
)

if /I "%~1"=="--arch" (
  if "%~2"=="" goto missing_value
  set "ARCH=%~2"
  shift
  shift
  goto parse_args
)

if /I "%~1"=="--clangcl" (
  set "TOOLSET=ClangCL"
  shift
  goto parse_args
)

if /I "%~1"=="--build-tests" (
  if "%~2"=="" goto missing_value
  set "BUILD_TESTS=%~2"
  shift
  shift
  goto parse_args
)

if /I "%~1"=="--require-parity-fixtures" (
  if "%~2"=="" goto missing_value
  set "REQUIRE_PARITY_FIXTURES=%~2"
  shift
  shift
  goto parse_args
)

if /I "%~1"=="--require-swift-fixtures" (
  if "%~2"=="" goto missing_value
  set "REQUIRE_SWIFT_FIXTURES=%~2"
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
if not exist "%SOURCE_DIR%\CMakeLists.txt" (
  echo [ERROR] CMakeLists not found: "%SOURCE_DIR%\CMakeLists.txt"
  exit /b 1
)

echo [waxcpp] Configure CMake project
echo   source   : "%SOURCE_DIR%"
echo   build    : "%BUILD_DIR%"
echo   generator: "%GENERATOR%"
echo   arch     : "%ARCH%"
echo   config   : "%CONFIG%"
if defined TOOLSET (
  echo   toolset : "%TOOLSET%"
) else (
  echo   toolset : default
)
echo   tests    : "%BUILD_TESTS%"
echo   parity   : "%REQUIRE_PARITY_FIXTURES%"
echo   swift    : "%REQUIRE_SWIFT_FIXTURES%"

if defined TOOLSET (
  cmake -S "%SOURCE_DIR%" -B "%BUILD_DIR%" ^
    -G "%GENERATOR%" ^
    -A "%ARCH%" ^
    -T "%TOOLSET%" ^
    -DCMAKE_CONFIGURATION_TYPES=%CONFIG% ^
    -DWAXCPP_BUILD_TESTS=%BUILD_TESTS% ^
    -DWAXCPP_REQUIRE_PARITY_FIXTURES=%REQUIRE_PARITY_FIXTURES% ^
    -DWAXCPP_REQUIRE_SWIFT_FIXTURES=%REQUIRE_SWIFT_FIXTURES%
) else (
  cmake -S "%SOURCE_DIR%" -B "%BUILD_DIR%" ^
    -G "%GENERATOR%" ^
    -A "%ARCH%" ^
    -DCMAKE_CONFIGURATION_TYPES=%CONFIG% ^
    -DWAXCPP_BUILD_TESTS=%BUILD_TESTS% ^
    -DWAXCPP_REQUIRE_PARITY_FIXTURES=%REQUIRE_PARITY_FIXTURES% ^
    -DWAXCPP_REQUIRE_SWIFT_FIXTURES=%REQUIRE_SWIFT_FIXTURES%
)

if errorlevel 1 (
  echo [ERROR] CMake configuration failed.
  exit /b 1
)

echo [OK] CMake project generated.
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
echo   scripts\generate-cmake.bat [options]
echo.
echo Options:
echo   --build-dir PATH                  Build directory (default: cpp\build)
echo   --config Debug^|Release           VS configuration type (default: Debug)
echo   --generator "NAME"                CMake generator (default: Visual Studio 17 2022)
echo   --arch x64^|Win32^|ARM64          Target architecture (default: x64)
echo   --clangcl                         Use ClangCL toolset for Visual Studio
echo   --build-tests ON^|OFF             Build test targets (default: ON)
echo   --require-parity-fixtures ON^|OFF Require parity fixtures (default: OFF)
echo   --require-swift-fixtures ON^|OFF  Require Swift fixtures (default: OFF)
echo   --help                            Show this message
echo.
echo Examples:
echo   scripts\generate-cmake.bat
echo   scripts\generate-cmake.bat --config Release
echo   scripts\generate-cmake.bat --clangcl --build-dir cpp\build-clang
exit /b %USAGE_CODE%
