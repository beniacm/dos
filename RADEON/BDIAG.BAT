@echo off
if "%WATCOM%"=="" set WATCOM=C:\WATCOM
if not exist %WATCOM%\BINW\WCC386.EXE if exist W:\BINW\WCC386.EXE set WATCOM=W:
if not exist %WATCOM%\BINW\WCC386.EXE goto no_watcom
set PATH=%WATCOM%\BINW;%PATH%
set INCLUDE=%WATCOM%\H
echo Building RDIAG...
wcc386 -bt=dos -3r -ox -s -zq RDIAG.C
if errorlevel 1 goto fail
wlink system pmodew name RDIAG file RDIAG option quiet
if errorlevel 1 goto fail
echo Done: RDIAG.exe
goto end
:fail
echo.
echo *** BUILD FAILED ***
:end
goto done
:no_watcom
echo.
echo *** OpenWatcom DOS tools not found ***
echo Set WATCOM or mount /opt/watcom as W: in DOSBox.
:done
