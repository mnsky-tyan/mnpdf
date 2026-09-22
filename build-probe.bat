@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat" -arch=x64 -no_logo
cd /d %~dp0
cl /nologo /O2 /MT /EHsc /W4 /DUNICODE /utf-8 probe-render.cpp /I third_party\pdfium\include /Fo:build\ ^
  /link third_party\pdfium\lib\pdfium.dll.lib /OUT:build\probe-render.exe
if errorlevel 1 exit /b 1
copy /y third_party\pdfium\bin\pdfium.dll build\ >nul
echo PROBE BUILD OK
