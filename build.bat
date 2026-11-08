@echo off
rem Compila Dinamic Zaur para 64 bits y 32 bits con Visual Studio (Build Tools 2019 o posterior).
rem Uso:  build.bat         -> las dos versiones
rem       build.bat x64     -> solo 64 bits
rem       build.bat x86     -> solo 32 bits
setlocal
cd /d "%~dp0"
set ARCHS=%1
if "%ARCHS%"=="" set ARCHS=x64 x86

set VSROOT=
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set VSROOT=%%i
if "%VSROOT%"=="" set VSROOT=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools
if not exist "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" (
  echo No se encontro Visual Studio con las herramientas de C++.
  exit /b 1
)

if not exist dist mkdir dist
set FAILED=0
for %%a in (%ARCHS%) do call :build %%a
endlocal & exit /b %FAILED%

:build
setlocal
echo.
echo ===== %1 =====
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" %1 >nul 2>nul
if not exist build\%1 mkdir build\%1
rc /nologo /fo build\%1\app.res res\app.rc
if errorlevel 1 (endlocal & set FAILED=1 & exit /b 1)
cl /nologo /std:c++17 /O1 /GL /EHsc /W3 /MT /utf-8 /bigobj /MP /DUNICODE /D_UNICODE /DNDEBUG /Fobuild\%1\ src\*.cpp build\%1\app.res /Fe:dist\DinamicZaur-%1.exe ^
   /link /LTCG /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:res\app.manifest /MANIFESTUAC:"level='asInvoker'"
if errorlevel 1 (endlocal & set FAILED=1 & exit /b 1)
endlocal
exit /b 0
