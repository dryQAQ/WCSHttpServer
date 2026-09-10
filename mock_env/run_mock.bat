@echo off
rem ================================================================
rem  WCS narrow-band sorting Mock console launcher (pure ASCII on
rem  purpose, so it works on any client console codepage/line ending).
rem
rem  Usage:
rem    run_mock.bat
rem        Start the mock console using the current http_server.xml.
rem    run_mock.bat wms=9191 plc=2500 rfid=3010 gw=9099 query=9100
rem        Expected ports for THIS session (reference only; the GUI
rem        "Config" page compares them against the config file).
rem    run_mock.bat check [wms=.. plc=.. rfid=.. gw=.. query=..]
rem        Print the expected-vs-current port table without GUI.
rem
rem  Port keys -> config nodes (see GUI Config page for details):
rem    wms=   <wmsListenPort>            (WMS->WCS push, WCS listens)
rem    plc=   <plcListenPort>            (PLC channel, WCS listens)
rem    rfid=  <rfidPushServerPort>       (RFID push server, mock listens)
rem    gw=    port inside feedbackTestUrl / feedbackEndTestUrl
rem    query= port inside rfidQueryUrl
rem    wcs=   IP address of the WCS machine. Used by the mock "PLC
rem           client" and "WMS push" channels (default 127.0.0.1).
rem           Set it when the mock console and WCS are on different
rem           machines, or WCS only listens on its LAN IP.
rem    bind=  IP the mock console binds its listeners to (RFID push
rem           server / S7 / H7H8 gateway / RFID query). Default
rem           127.0.0.1; use 0.0.0.0 when WCS connects from another
rem           machine.
rem  Channel switches (add any of these words):
rem    noplc   do NOT start the mock PLC client. Use this when the
rem            REAL PLC is connected to WCS and must be the only
rem            device that receives/answers PLC commands.
rem    nos7    do NOT start the mock S7 lock server
rem    nogw    do NOT start the mock H7/H8 gateway
rem    noquery do NOT start the mock RFID query server
rem    norfid  do NOT start the mock RFID push server
rem    S7 lock port is fixed to 102 (snap7) on both sides.
rem
rem  This script NEVER edits any config file. Both WCS and the mock
rem  console read release_WcsHttpServer\config\http_server.xml.
rem  To change ports (restart WCS_httpServer.exe afterwards):
rem    a) edit mock_env\config_mock\http_server.xml, then press the
rem       "Enable Mock Config" button on the GUI Config page; or
rem    b) edit the live http_server.xml directly (back it up first).
rem ================================================================
cd /d "%~dp0"

rem ---- locate a working Python (python.exe, then py launcher) ----
set "PY_CMD="
where python >nul 2>&1
if not errorlevel 1 set "PY_CMD=python"
if defined PY_CMD goto :found
where py >nul 2>&1
if not errorlevel 1 set "PY_CMD=py -3"
if defined PY_CMD goto :found
echo.
echo [ERROR] Python was not found on this machine.
echo   Install Python 3 from https://www.python.org/downloads/ and
echo   tick "Add python.exe to PATH" during setup, then run again.
echo   The Microsoft Store "python" stub is NOT enough.
echo   Or run this instead:  py -3 mock_app.py
pause
exit /b 1

:found
%PY_CMD% mock_app.py %*
if errorlevel 1 goto :fail
exit /b 0

:fail
echo.
echo [HINT] Python launch failed. In a "cmd" window run:
echo   %PY_CMD% --version
echo   It should print something like "Python 3.12.x". If not, install
echo   real Python from https://www.python.org/downloads/ and tick
echo   "Add python.exe to PATH", then close and reopen this window.
pause
exit /b 1
