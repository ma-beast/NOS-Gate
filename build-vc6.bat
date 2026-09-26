@echo off
echo NOS-Gate 0.5.1 Final for Visual C++ 6.0
echo.
if exist build-vc6.log del build-vc6.log
echo NOS-Gate 0.5.1 Final log > build-vc6.log
if not exist "C:\Program Files\Microsoft Visual Studio\VC98\Bin\CL.EXE" goto no_vc
if not exist "C:\Program Files\Microsoft Visual Studio\VC98\Bin\NMAKE.EXE" goto no_nmake
if not exist build\NUL mkdir build
if not exist MSPDB60.DLL copy "C:\Program Files\Microsoft Visual Studio\Common\MSDev98\Bin\MSPDB60.DLL" MSPDB60.DLL
if not exist MSPDB60.DLL goto no_pdb
if exist build\http_win32.obj del build\http_win32.obj
if exist build\main.obj del build\main.obj
if exist build\NOS-Gate.res del build\NOS-Gate.res
if exist build\NOS-Gate.exe del build\NOS-Gate.exe

if exist vendor\BearSSL\build\bearssls.lib goto bearssl_cached

echo Building BearSSL TLS library...
echo Building BearSSL TLS library... >> build-vc6.log
if not exist vendor\BearSSL\MSPDB60.DLL copy MSPDB60.DLL vendor\BearSSL\MSPDB60.DLL
if not exist vendor\BearSSL\build\NUL mkdir vendor\BearSSL\build
cd vendor\BearSSL
"C:\Program Files\Microsoft Visual Studio\VC98\Bin\NMAKE.EXE" /nologo /f Makefile CONF=NOSWebVC6 lib >> ..\..\build-vc6.log
cd ..\..
goto bearssl_ready

:bearssl_cached
echo Using existing BearSSL TLS library...
echo Using existing BearSSL TLS library... >> build-vc6.log

:bearssl_ready
if not exist vendor\BearSSL\build\bearssls.lib goto error

echo Compiling HTTP and TLS layer...
"C:\Program Files\Microsoft Visual Studio\VC98\Bin\CL.EXE" /nologo /O1 /W3 /DWIN32 /D_WIN32_WINNT=0x0400 /Dinline=__inline /Isrc\win32 /Ivendor\BearSSL\inc /I"C:\Program Files\Microsoft Visual Studio\VC98\Include" /c src\win32\http_win32.c /Fobuild\http_win32.obj >> build-vc6.log
if not exist build\http_win32.obj goto error

echo Compiling local gateway...
"C:\Program Files\Microsoft Visual Studio\VC98\Bin\CL.EXE" /nologo /O1 /W3 /DWIN32 /D_WIN32_WINNT=0x0400 /Isrc\win32 /I"C:\Program Files\Microsoft Visual Studio\VC98\Include" /c src\win32\main.c /Fobuild\main.obj >> build-vc6.log
if not exist build\main.obj goto error

echo Compiling application icon...
RC.EXE /fo build\NOS-Gate.res src\win32\NOS-Gate.rc >> build-vc6.log 2>&1
if not exist build\NOS-Gate.res goto error

echo Linking NOS-Gate.exe...
"C:\Program Files\Microsoft Visual Studio\VC98\Bin\LINK.EXE" /nologo /SUBSYSTEM:WINDOWS,4.0 /LIBPATH:"C:\Program Files\Microsoft Visual Studio\VC98\Lib" /OUT:build\NOS-Gate.exe build\main.obj build\http_win32.obj build\NOS-Gate.res vendor\BearSSL\build\bearssls.lib kernel32.lib user32.lib shell32.lib wsock32.lib advapi32.lib >> build-vc6.log
if not exist build\NOS-Gate.exe goto error
echo.
echo Built: build\NOS-Gate.exe [0.5.1 Final]
echo Built: build\NOS-Gate.exe [0.5.1 Final] >> build-vc6.log
goto end
:no_pdb
echo ERROR: MSPDB60.DLL was not found.
goto end
:no_vc
echo ERROR: CL.EXE was not found.
goto end
:no_nmake
echo ERROR: NMAKE.EXE was not found.
goto end
:error
echo.
echo Build failed.
echo Build failed. >> build-vc6.log
type build-vc6.log
:end
echo.
pause
