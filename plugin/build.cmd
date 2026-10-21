@echo off
rem Usage: build.cmd [Release|RelWithDebInfo|Debug]
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
set VCPKG_ROOT=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\vcpkg
cmake -S . -B build\%CONFIG% -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% ^
	"-DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" ^
	-DVCPKG_TARGET_TRIPLET=x64-windows-static-md "-DVCPKG_INSTALLED_DIR=%~dp0build\vcpkg_installed" || exit /b 1
cmake --build build\%CONFIG% || exit /b 1
