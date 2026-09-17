@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Error: Visual Studio Installer not found at "%VSWHERE%".
    exit /b 2
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VSROOT=%%i"
)

if not defined VSROOT (
    echo Error: C++ build tools not found.
    exit /b 3
)

call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo Error: vcvars64.bat failed.
    exit /b 4
)

cl /nologo /std:c++17 /O2 /EHsc /W3 /DWIN32_LEAN_AND_MEAN makcu_hid_extractor.cpp /Fe:universal_usb_parser.exe /link /OPT:REF /OPT:ICF
if errorlevel 1 (
    echo Compilation failed.
    exit /b %errorlevel%
)

copy /y universal_usb_parser.exe makcu_hid_extractor.exe >nul
if exist makcu_hid_extractor.obj del makcu_hid_extractor.obj
echo Successfully built universal_usb_parser.exe
exit /b 0
