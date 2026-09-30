@echo off
rem The exact path is tried first because that is what this machine has; the
rem fallback asks vswhere, so a runner or a new install does not need this file
rem edited to build.
set "VSDEV="
if exist "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat" (
  set "VSDEV=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat"
) else (
  for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDEV=%%i\Common7\Tools\VsDevCmd.bat"
)
if not defined VSDEV (
  echo BUILD FAILED: no Visual Studio with the VC tools found
  exit /b 1
)
call "%VSDEV%" -arch=x64 -no_logo
cd /d %~dp0
if not exist build mkdir build
rc /nologo /fo build\mnpdf.res mnpdf.rc
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /EHsc /W4 /DUNICODE /utf-8 src\main.cpp /I . /I third_party\pdfium\include /Fo:build\ ^
  /link build\mnpdf.res third_party\pdfium\lib\pdfium.dll.lib /OUT:build\mnpdf.exe
if errorlevel 1 exit /b 1
copy /y third_party\pdfium\bin\pdfium.dll build\ >nul
echo BUILD OK
