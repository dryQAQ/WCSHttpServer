@echo off
REM ============================================================================
REM  tests\run_tests.bat -- build + run the regression tests for
REM    (1) RFID pushed EPC recognition   (rfidEpcTruncateLen: 'A' + (N-1) digits, default 24)
rem    (2) RFID raw push frame retention  (run.log / UI / rfid_raw table)
rem    (3) wave-record list batch queries (switch/refresh lag fix: N+1 -> 3 GROUP BY queries)
rem    (4) close-cut / restart-new-task / switch-back-resume contract
rem        (bindings archived on close, none loaded on start, restored when switching back)
rem        + terminal wave (finished/cancelled) MUST NOT be switchable back
rem        + power-loss path (no close-cut): bindings still active -> archived on next start
rem          -> switch back restores the SAME per-grid binding set (bound stay bound, unbound stay unbound)
rem        + landed-progress rebuild on switch-back (H7 detail / plan quota / landing dedup)
rem    (5) sorting-record query date filter (required range: EPC/SKU/grid/boxcode modes)
rem    (6) fullbox failed-by-order + per-wave H7 counters (one-key fullbox label / H7 dropdown)
rem    (7) binding panel policy (hide exception grid 66 + four-state background colors)
rem
rem  All tests drive the REAL product classes:
rem    test_epc_recognize.cpp          -> RfidPushClient::OnReceive (frame parse + EPC recognition + raw frame)
rem    test_rfid_raw_db.cpp            -> SortingDatabase       (rfid_raw table: insert / query / cleanup)
rem    test_wave_records_batch.cpp     -> SortingDatabase       (batch H7/H8 status + pending-exception counts)
rem    test_restart_resume_db.cpp      -> SortingDatabase       (archive on exit, new-task start, resume inputs)
rem    test_sorting_query_date.cpp     -> SortingDatabase       (date-range filters on all query modes)
rem    test_fullbox_failed_by_order.cpp-> SortingDatabase       (failed H7 by wave + per-wave H7 counters)
rem    test_binding_panel_policy.cpp   -> tests/BindingPanelPolicy.h (also used by MainWindow)
rem
rem  Outputs go to .build_check (local scratch, git-ignored) - never into x64\Release,
rem  so this can run while Visual Studio keeps its own incremental build.
rem
rem  Usage:  tests\run_tests.bat
rem ============================================================================
setlocal
call "D:\vs2019\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 ( echo [FAIL] vcvars64 failed & exit /b 1 )

set QT=D:\Qt\5.15.2\msvc2019_64
set ROOT=%~dp0..
set SRC=%ROOT%\WCS_httpServer
set OUT=%ROOT%\.build_check
if not exist "%OUT%" mkdir "%OUT%"
cd /d "%OUT%"

set INC=/I"%SRC%" /I"%ROOT%\tests" /I"%ROOT%\include" /I"%ROOT%\include\HPSocket" /I"%ROOT%\include\hlog" /I"%QT%\include" /I"%QT%\include\QtCore" /I"%QT%\include\QtGui" /I"%QT%\include\QtWidgets" /I"%QT%\include\QtSql"
set FLAGS=/nologo /std:c++17 /W3 /utf-8 /MD /EHsc /wd4996 /wd4267 /DUNICODE /D_UNICODE /D_WINDOWS /DWIN64 /DNDEBUG /DQT_NO_DEBUG /DQT_CORE_LIB /DQT_SQL_LIB /D_ENABLE_EXTENDED_ALIGNED_STORAGE
set PATH=%QT%\bin;%ROOT%\release_WcsHttpServer;%PATH%

echo ==== [1/8] EPC recognition + raw frame preserved (RfidPushClient) ====
"%QT%\bin\moc.exe" "%SRC%\RfidPushClient.h" -o "%OUT%\moc_RfidPushClient_test.cpp"
if errorlevel 1 goto :fail
cl %FLAGS% %INC% "%ROOT%\tests\test_epc_recognize.cpp" "%SRC%\RfidPushClient.cpp" "%OUT%\moc_RfidPushClient_test.cpp" /Fe"%OUT%\test_epc_recognize.exe" /link /LIBPATH:"%ROOT%\lib" /LIBPATH:"%QT%\lib" HPSocket_U.lib hlog.lib Qt5Core.lib
if errorlevel 1 goto :fail
"%OUT%\test_epc_recognize.exe"
if errorlevel 1 goto :fail

echo.
echo ==== [2/8] raw frame retention in SQLite (SortingDatabase rfid_raw) ====
"%QT%\bin\moc.exe" "%SRC%\ConfigManager.h" -o "%OUT%\moc_ConfigManager_test.cpp"
if errorlevel 1 goto :fail
cl %FLAGS% %INC% "%ROOT%\tests\test_rfid_raw_db.cpp" "%SRC%\SortingDatabase.cpp" "%SRC%\ConfigManager.cpp" "%OUT%\moc_ConfigManager_test.cpp" /Fe"%OUT%\test_rfid_raw_db.exe" /link /LIBPATH:"%ROOT%\lib" /LIBPATH:"%QT%\lib" hlog.lib Qt5Core.lib Qt5Sql.lib
if errorlevel 1 goto :fail
"%OUT%\test_rfid_raw_db.exe"
if errorlevel 1 goto :fail

echo.
echo ==== [3/8] wave-record list batch queries (switch/refresh lag fix) ====
cl %FLAGS% %INC% "%ROOT%\tests\test_wave_records_batch.cpp" "%SRC%\SortingDatabase.cpp" "%SRC%\ConfigManager.cpp" "%OUT%\moc_ConfigManager_test.cpp" /Fe"%OUT%\test_wave_records_batch.exe" /link /LIBPATH:"%ROOT%\lib" /LIBPATH:"%QT%\lib" hlog.lib Qt5Core.lib Qt5Sql.lib
if errorlevel 1 goto :fail
"%OUT%\test_wave_records_batch.exe"
if errorlevel 1 goto :fail

echo.
echo ==== [4/8] close-cut / new-task start / switch-back resume contract (+ terminal wave not resumable, power-loss path, landed-progress rebuild) ====
cl %FLAGS% %INC% "%ROOT%\tests\test_restart_resume_db.cpp" "%SRC%\SortingDatabase.cpp" "%SRC%\ConfigManager.cpp" "%OUT%\moc_ConfigManager_test.cpp" /Fe"%OUT%\test_restart_resume_db.exe" /link /LIBPATH:"%ROOT%\lib" /LIBPATH:"%QT%\lib" hlog.lib Qt5Core.lib Qt5Sql.lib
if errorlevel 1 goto :fail
"%OUT%\test_restart_resume_db.exe"
if errorlevel 1 goto :fail

echo.
echo ==== [5/8] sorting-record query date filter (required range, all query modes) ====
cl %FLAGS% %INC% "%ROOT%\tests\test_sorting_query_date.cpp" "%SRC%\SortingDatabase.cpp" "%SRC%\ConfigManager.cpp" "%OUT%\moc_ConfigManager_test.cpp" /Fe"%OUT%\test_sorting_query_date.exe" /link /LIBPATH:"%ROOT%\lib" /LIBPATH:"%QT%\lib" hlog.lib Qt5Core.lib Qt5Sql.lib
if errorlevel 1 goto :fail
"%OUT%\test_sorting_query_date.exe"
if errorlevel 1 goto :fail

echo.
echo ==== [6/8] failed H7 by wave + per-wave H7 counters (one-key fullbox label / H7 dropdown) ====
cl %FLAGS% %INC% "%ROOT%\tests\test_fullbox_failed_by_order.cpp" "%SRC%\SortingDatabase.cpp" "%SRC%\ConfigManager.cpp" "%OUT%\moc_ConfigManager_test.cpp" /Fe"%OUT%\test_fullbox_failed_by_order.exe" /link /LIBPATH:"%ROOT%\lib" /LIBPATH:"%QT%\lib" hlog.lib Qt5Core.lib Qt5Sql.lib
if errorlevel 1 goto :fail
"%OUT%\test_fullbox_failed_by_order.exe"
if errorlevel 1 goto :fail

echo.
echo ==== [7/8] binding panel policy (hide exception grid + four-state background colors) ====
cl %FLAGS% %INC% "%ROOT%\tests\test_binding_panel_policy.cpp" "%SRC%\ConfigManager.cpp" "%OUT%\moc_ConfigManager_test.cpp" /Fe"%OUT%\test_binding_panel_policy.exe" /link /LIBPATH:"%ROOT%\lib" /LIBPATH:"%QT%\lib" hlog.lib Qt5Core.lib Qt5Sql.lib
if errorlevel 1 goto :fail
"%OUT%\test_binding_panel_policy.exe"
if errorlevel 1 goto :fail

echo.
echo ==== [8/8] pending-wave queue dequeue timing (only 「开始接收任务」 may start queued waves; no wave skipped) ====
cl %FLAGS% %INC% "%ROOT%\tests\test_pending_queue_gate.cpp" /Fe"%OUT%\test_pending_queue_gate.exe" /link /LIBPATH:"%QT%\lib" Qt5Core.lib
if errorlevel 1 goto :fail
"%OUT%\test_pending_queue_gate.exe"
if errorlevel 1 goto :fail

echo.
echo [OK] all tests passed
endlocal
exit /b 0

:fail
echo.
echo [FAIL] tests FAILED (see messages above)
endlocal
exit /b 1
