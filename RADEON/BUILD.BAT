@echo off
echo Building RADEON demo with OpenWatcom...
wcl386 -bt=dos -l=dos4g -ox -fp3 -zq RADEON.C
if errorlevel 1 goto fail
echo.
echo Build OK - run RADEON.EXE  (requires Radeon X1300 hardware)
goto end
:fail
echo.
echo *** BUILD FAILED ***
:end
