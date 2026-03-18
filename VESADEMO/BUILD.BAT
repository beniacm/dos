@echo off
echo Building VESADEMO with OpenWatcom...
wcl386 -bt=dos -l=dos4g -ox -zq VESADEMO.C
if errorlevel 1 goto fail
echo.
echo Build OK - run VESADEMO.EXE
goto end
:fail
echo.
echo *** BUILD FAILED ***
:end
