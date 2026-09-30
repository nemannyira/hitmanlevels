@echo off
rem Configure + build the x86 DLL with MSVC and Ninja.
rem Usage: build.bat [Release|Debug] [install]
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release

set VCVARS=C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvarsamd64_x86.bat
if not exist "%VCVARS%" set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvarsamd64_x86.bat
call "%VCVARS%" >nul || exit /b 1

cd /d "%~dp0"
cmake -S . -B build\%CONFIG% -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% || exit /b 1
cmake --build build\%CONFIG% || exit /b 1

if /i "%2"=="install" (
    copy /y build\%CONFIG%\dinput8.dll "C:\Program Files (x86)\Steam\steamapps\common\Hitman 2 Silent Assassin\dinput8.dll" || exit /b 1
    if not exist "C:\Program Files (x86)\Steam\steamapps\common\Hitman 2 Silent Assassin\H2Fix.ini" copy /y H2Fix.ini "C:\Program Files (x86)\Steam\steamapps\common\Hitman 2 Silent Assassin\H2Fix.ini"
)
