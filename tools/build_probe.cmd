@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
cd /d "%~dp0.."
cl /nologo /W3 /O2 /MT /I third_party\twain tools\probe.c /Fobuild\ /Febuild\probe.exe user32.lib
