@echo off
setlocal

set "ROOT=D:\kaifa\newPcmProcess"
set "SCRIPT=%ROOT%\tools\generate_head2_pcm.ps1"
set "OUT=%ROOT%\generated\head2_15gb.pcm"
set "PROGRESS=%ROOT%\generated\head2_15gb.progress.json"
set "DONE=%ROOT%\generated\head2_15gb.done"
set "ERROR=%ROOT%\generated\head2_15gb.error.txt"
set "STDOUT=%ROOT%\generated\head2_15gb.stdout.log"
set "STDERR=%ROOT%\generated\head2_15gb.stderr.log"

powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%" ^
    -OutputPath "%OUT%" ^
    -ProgressPath "%PROGRESS%" ^
    -DonePath "%DONE%" ^
    -ErrorPath "%ERROR%" ^
    -TargetBytes 15000000000 ^
    > "%STDOUT%" 2> "%STDERR%"

exit /b %ERRORLEVEL%
