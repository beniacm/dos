@echo off
set WATCOM=C:\WATCOM
set PATH=%WATCOM%\binnt64;%WATCOM%\binnt;%PATH%
set INCLUDE=%WATCOM%\h

echo Building RBLIT...
wcc386 -bt=dos -3r -ox -s -zq RBLIT.C
if errorlevel 1 goto :fail
wlink system pmodew name RBLIT file RBLIT option quiet
if errorlevel 1 goto :fail
echo Done: RBLIT.exe
goto :eof
:fail
echo BUILD FAILED
