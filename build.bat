@echo off
rem Configure + build every game's editor DLL and the tools (x86, MSVC, Ninja) into build\<config>.
rem Usage: build.bat [Release|Debug] [install]   (install: Hitman 2's dinput8.dll into its Steam folder)
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release
set VS2019=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvarsamd64_x86.bat

set VCVARS=C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvarsamd64_x86.bat
if not exist "%VCVARS%" set VCVARS=%VS2019%
call "%VCVARS%" >nul || exit /b 1
set BUILD=build\%CONFIG%
rem a Visual Studio being updated can lack its compiler: the 2019 Build Tools
rem then, in a folder of their own (a CMake cache keeps its compiler)
if not exist "%VCToolsInstallDir%bin\Hostx64\x86\cl.exe" (call "%VS2019%" >nul & set BUILD=build\%CONFIG%19)
where cl >nul 2>nul || (echo no C++ compiler found & exit /b 1)

cd /d "%~dp0"
cmake -S . -B %BUILD% -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% || exit /b 1
cmake --build %BUILD% || exit /b 1
if /i not "%BUILD%"=="build\%CONFIG%" copy /y %BUILD%\dinput8.dll build\%CONFIG%\dinput8.dll >nul

if /i "%2"=="install" (
    copy /y build\%CONFIG%\dinput8.dll "C:\Program Files (x86)\Steam\steamapps\common\Hitman 2 Silent Assassin\dinput8.dll" || exit /b 1
    rem (its .pdb next to it: crashlog.cpp names the DLL's frames from it, a stale one gives wrong names)
    if exist %BUILD%\dinput8.pdb copy /y %BUILD%\dinput8.pdb "C:\Program Files (x86)\Steam\steamapps\common\Hitman 2 Silent Assassin\dinput8.pdb" >nul
    if not exist "C:\Program Files (x86)\Steam\steamapps\common\Hitman 2 Silent Assassin\H2Fix.ini" copy /y h2\H2Fix.ini "C:\Program Files (x86)\Steam\steamapps\common\Hitman 2 Silent Assassin\H2Fix.ini"
)
