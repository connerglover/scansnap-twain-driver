@echo off
rem Build with MSVC. The data source itself must be x86 to match NAPS2's
rem TWAIN worker; the helper tools are also x86 for simplicity.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
cd /d "%~dp0.."
if not exist build\obj mkdir build\obj
pushd build\obj

set INC=/I ..\..\third_party\twain /I ..\..\src
set DEFS=/D_CRT_SECURE_NO_WARNINGS
set SRC=..\..\src\ds.c ..\..\src\session.c ..\..\src\proto.c ..\..\src\usb.c ..\..\src\image.c ..\..\src\misc.c

cl /nologo /W3 /O2 /MT %DEFS% %INC% %SRC% /Fe..\ScanSnap-iX1300.ds /LD /link /DEF:..\..\src\entry.def winusb.lib setupapi.lib user32.lib || (popd & exit /b 1)

cl /nologo /W3 /O2 /MT %DEFS% %INC% ..\..\tools\selftest.c ..\..\src\session.c ..\..\src\proto.c ..\..\src\usb.c ..\..\src\image.c ..\..\src\misc.c /Fe..\selftest.exe /link winusb.lib setupapi.lib || (popd & exit /b 1)

cl /nologo /W3 /O2 /MT %DEFS% %INC% ..\..\tools\probe.c ..\..\src\session.c ..\..\src\proto.c ..\..\src\usb.c ..\..\src\image.c ..\..\src\misc.c /Fe..\probe.exe /link winusb.lib setupapi.lib || (popd & exit /b 1)

cl /nologo /W3 /O2 /MT %DEFS% %INC% ..\..\tools\mockscan.c ..\..\src\session.c ..\..\src\proto.c ..\..\src\usb.c ..\..\src\image.c ..\..\src\misc.c /Fe..\mockscan.exe /link winusb.lib setupapi.lib || (popd & exit /b 1)

cl /nologo /W3 /O2 /MT %DEFS% %INC% ..\..\tools\twaintest.c /Fe..\twaintest.exe /link user32.lib || (popd & exit /b 1)

cl /nologo /W3 /O2 /MT %DEFS% %INC% ..\..\tools\dsmtest.c /Fe..\dsmtest.exe /link user32.lib || (popd & exit /b 1)

popd
echo.
echo Built build\ScanSnap-iX1300.ds, build\selftest.exe, build\probe.exe
