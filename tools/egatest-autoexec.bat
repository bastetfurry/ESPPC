@echo off
set prompt=$p$g
REM --- test machine: runs the video test straight from boot.
REM --- delete the QBASIC line below for a plain DOS prompt.
echo.
echo  ESPPC video adapter test disk
echo.
QBASIC /RUN A:\EGATEST.BAS
