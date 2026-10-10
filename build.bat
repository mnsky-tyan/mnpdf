@echo off
rem The well-known install path is tried first because it needs no lookup; the
rem fallback asks vswhere, so a different edition, a fresh runner or a new
rem install does not need this file edited to build.
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
if errorlevel 1 (
  echo BUILD FAILED: VsDevCmd.bat could not set up the VC environment
  exit /b 1
)
rem quoted, and a failure stops the build: without both, a checkout path with a
rem space leaves the shell in the WRONG directory and rc/cl then compile nothing
rem (or the wrong tree) while still reporting success below.
cd /d "%~dp0" || exit /b 1
if not exist build mkdir build
rc /nologo /fo build\mnpdf.res mnpdf.rc
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /EHsc /W4 /DUNICODE /utf-8 src\main.cpp /I . /I third_party\pdfium\include /Fo:build\ ^
  /link build\mnpdf.res third_party\pdfium\lib\pdfium.dll.lib /OUT:build\mnpdf.exe
if errorlevel 1 exit /b 1
rem the copy needs its own check: without it the script prints BUILD OK and exits
rem 0 even when pdfium.dll never reached build\ (a missing source, an AV lock, a
rem read-only build dir), because the batch's exit code is the errorlevel of its
rem LAST command and a bare `echo` clears it. Every suite would then fail at app
rem launch with an error that points nowhere near the real cause.
copy /y third_party\pdfium\bin\pdfium.dll build\ >nul
if errorlevel 1 (
  echo BUILD FAILED: pdfium.dll was not copied into build\
  exit /b 1
)
if not exist build\pdfium.dll (
  echo BUILD FAILED: build\pdfium.dll is missing after the copy
  exit /b 1
)
echo BUILD OK
