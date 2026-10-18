@echo off
rem Builds build\dinput8.dll (the engine fix) and build\sampler.exe (the profiler).
rem Needs Visual Studio with the "Desktop development with C++" x86 tools.
rem Set VCVARS to your vcvars32.bat if Visual Studio is not in the default location.
setlocal
if not defined VCVARS set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
if not exist "%VCVARS%" (
    echo vcvars32.bat not found. Set VCVARS to its full path.
    exit /b 1
)
call "%VCVARS%" >nul 2>nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build

cl /nologo /O2 /W4 /MT /LD src\enginefix.c /Fobuild\ /link /DEF:src\dinput8.def /OUT:build\dinput8.dll /IMPLIB:build\dinput8.lib || exit /b 1
cl /nologo /O2 /W4 tools\profiler\sampler.c winmm.lib /Fobuild\ /Fe:build\sampler.exe || exit /b 1
echo Built build\dinput8.dll and build\sampler.exe
