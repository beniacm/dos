@echo off
set WATCOM=C:\WATCOM
set PATH=%WATCOM%\binnt64;%WATCOM%\binnt;%PATH%
set INCLUDE=%WATCOM%\h

echo Building RADEON (shared HW layer + demo)...
wcc386 -bt=dos -5r -ox -s -zq RADEONHW.C
if errorlevel 1 goto :fail
wcc386 -bt=dos -5r -ox -s -zq RADEON.C
if errorlevel 1 goto :fail
wlink system pmodew name RADEON file { RADEON RADEONHW } option quiet
if errorlevel 1 goto :fail
echo Done: RADEON.exe
goto :eof
:fail
echo BUILD FAILED
