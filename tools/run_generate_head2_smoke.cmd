@echo off
setlocal

set "ROOT=D:\kaifa\newPcmProcess"
set "SCRIPT=%ROOT%\tools\generate_head2_pcm.ps1"
set "OUT=%ROOT%\generated\head2_cmd_smoke.pcm"
set "PROGRESS=%ROOT%\generated\head2_cmd_smoke.progress.json"
set "DONE=%ROOT%\generated\head2_cmd_smoke.done"
set "ERROR=%ROOT%\generated\head2_cmd_smoke.error.txt"
set "STDOUT=%ROOT%\generated\head2_cmd_smoke.stdout.log"
set "STDERR=%ROOT%\generated\head2_cmd_smoke.stderr.log"

powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%" ^
    -OutputPath "%OUT%" ^
    -ProgressPath "%PROGRESS%" ^
    -DonePath "%DONE%" ^
    -ErrorPath "%ERROR%" ^
    -TargetBytes 1536000 ^
    > "%STDOUT%" 2> "%STDERR%"

exit /b %ERRORLEVEL%
