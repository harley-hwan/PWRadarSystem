@echo off
REM ==========================================================================
REM  PWRadarSystem - Windows build
REM  ------------------------------------------------------------------------
REM  Generates PWRadarSystem.sln with Visual Studio's own CMake generator,
REM  builds it, and runs the numerical acceptance suite.
REM
REM      build.bat                Release x64 - builds and self tests only
REM      build.bat Debug          Debug x64
REM      build.bat Release run    build, self test, THEN LAUNCH the console
REM      build.bat clean          delete the build directory
REM
REM  Note the third line: without "run" this script never opens a window.  It
REM  builds and checks, and that is all.
REM
REM  After a build you can work in the IDE:  open build\PWRadarSystem.sln
REM  That solution is generated, so it is guaranteed to load correctly.
REM
REM  Requires: Visual Studio 2022 with the "Desktop development with C++"
REM  workload.  CMake is found on PATH, or in the VS installation, or in the
REM  standalone install location - no configuration needed.
REM ==========================================================================
setlocal

set "ROOT=%~dp0"
set "BUILDDIR=%ROOT%build"
set "CFG=%~1"
if "%CFG%"=="" set "CFG=Release"

if /i "%CFG%"=="clean" goto do_clean
if /i "%CFG%"=="Debug"   goto cfg_ok
if /i "%CFG%"=="Release" goto cfg_ok
echo [ERROR] Unknown configuration "%CFG%".  Use Debug, Release or clean.
exit /b 2

:cfg_ok
call :find_cmake
if not defined CMAKE_EXE goto no_cmake

echo ======================================================================
echo  PWRadarSystem  -  %CFG% ^| x64
echo  cmake: %CMAKE_EXE%
echo ======================================================================
echo.

"%CMAKE_EXE%" -S "%ROOT%." -B "%BUILDDIR%" -G "Visual Studio 17 2022" -A x64
if errorlevel 1 goto fail_configure

"%CMAKE_EXE%" --build "%BUILDDIR%" --config %CFG%
if errorlevel 1 goto fail_build

set "EXE=%BUILDDIR%\%CFG%\PWRadarUI.exe"
if not exist "%EXE%" goto fail_missing

echo.
echo ---------------------------------------------------------------------
echo  Numerical acceptance suite (quick set)
echo ---------------------------------------------------------------------
REM  The quick set is every case that does not have to integrate over
REM  thousands of coherent intervals - about a second, so a build never sits
REM  silent long enough to look like it has hung.  The four statistical cases
REM  it leaves out (rotating dwell and track life cycle, Pd versus theory,
REM  K-distributed clutter) are the gate, and run under "--selftest" or ctest.
"%EXE%" --selftest quick
if errorlevel 1 goto fail_selftest

echo.
echo ======================================================================
echo  BUILD OK
echo    executable : %EXE%
echo    library    : %BUILDDIR%\%CFG%\PWRadarCore.dll
echo    solution   : %BUILDDIR%\PWRadarSystem.sln
echo ----------------------------------------------------------------------
echo  This script does NOT open the console window.  To launch it:
echo      build.bat %CFG% run
echo    or run the executable above directly.
echo  Full acceptance gate (adds the four statistical cases):
echo      "%EXE%" --selftest
echo ======================================================================

if /i "%~2"=="run" (
    echo.
    echo  launching the console ...
    start "" "%EXE%"
) else (
    echo.
    echo  [note] no window was opened - pass "run" if you wanted one.
)
endlocal
exit /b 0

REM ---------------------------------------------------------------------------
:find_cmake
set "CMAKE_EXE="
REM 1. on PATH
for /f "delims=" %%p in ('where cmake 2^>nul') do if not defined CMAKE_EXE set "CMAKE_EXE=%%p"
if defined CMAKE_EXE goto :eof
REM 2. bundled with Visual Studio 2022 (every edition)
call :try_vs "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise"
call :try_vs "%ProgramFiles%\Microsoft Visual Studio\2022\Professional"
call :try_vs "%ProgramFiles%\Microsoft Visual Studio\2022\Community"
call :try_vs "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools"
call :try_vs "%ProgramFiles%\Microsoft Visual Studio\2022\Preview"
if defined CMAKE_EXE goto :eof
REM 3. standalone CMake install
if exist "%ProgramFiles%\CMake\bin\cmake.exe" set "CMAKE_EXE=%ProgramFiles%\CMake\bin\cmake.exe"
goto :eof

:try_vs
if defined CMAKE_EXE goto :eof
set "VSCMAKE=%~1\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if exist "%VSCMAKE%" set "CMAKE_EXE=%VSCMAKE%"
goto :eof

REM ---------------------------------------------------------------------------
:do_clean
if exist "%BUILDDIR%" rmdir /s /q "%BUILDDIR%"
echo [OK] removed "%BUILDDIR%"
endlocal
exit /b 0

:no_cmake
echo [ERROR] cmake.exe was not found.
echo.
echo         Install Visual Studio 2022 with the workload
echo             "Desktop development with C++"
echo         which bundles CMake, or install CMake from cmake.org and make
echo         sure it is on PATH.
endlocal
exit /b 1

:fail_configure
echo.
echo [FAILED] CMake configure step.  Most likely cause: the Visual Studio 2022
echo          C++ toolset is not installed.  Open the Visual Studio Installer
echo          and add "Desktop development with C++".
endlocal
exit /b 1

:fail_build
echo.
echo [FAILED] compile or link step.  See the errors above.
endlocal
exit /b 1

:fail_missing
echo.
echo [FAILED] the build reported success but "%EXE%" is missing.
endlocal
exit /b 1

:fail_selftest
echo.
echo [FAILED] the numerical acceptance suite reported failures.
endlocal
exit /b 1
