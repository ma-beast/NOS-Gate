@echo off
echo NOS-Gate full rebuild: BearSSL and gateway
echo.
if not exist "C:\Program Files\Microsoft Visual Studio\VC98\Bin\NMAKE.EXE" goto no_nmake
if not exist MSPDB60.DLL copy "C:\Program Files\Microsoft Visual Studio\Common\MSDev98\Bin\MSPDB60.DLL" MSPDB60.DLL
if not exist vendor\BearSSL\MSPDB60.DLL copy MSPDB60.DLL vendor\BearSSL\MSPDB60.DLL
cd vendor\BearSSL
"C:\Program Files\Microsoft Visual Studio\VC98\Bin\NMAKE.EXE" /nologo /f Makefile CONF=NOSWebVC6 clean
cd ..\..
call build-vc6.bat
goto end
:no_nmake
echo ERROR: NMAKE.EXE was not found.
:end
