@echo off
REM ============================================================================
REM  tests\run_tests.bat -- build + run the regression tests for
REM    (1) RFID pushed EPC recognition (rfidEpcTruncateLen: 'A' + (N-1) digits, default 24)
REM    (2) RFID raw push frame retention (run.log / UI / rfid_raw table)
REM    (3) wave-record list batch queries (switch/refresh lag fix: N+1 -> 3 GROUP BY queries)
REM    (4) close-cut / restart-new-task / switch-back-resume contract
REM        (bindings archived on close, none loaded on start, restored when switching back)
REM        + terminal wave (finished/cancelled) MUST NOT be switchable back
REM        + power-loss path (no close-cut): bindings still active -> archived on next start
REM          -> switch back restores the SAME per-grid binding set (bound stay bound, unbound stay unbound)
REM        + landed-progress rebuild on switch-back (H7 detail / plan quota / landing dedup)
REM    (5) sorting-record query date filter (required range: EPC/SKU/grid/boxcode modes)
REM    (6) fullbox failed-by-order + per-wave H7 counters (one-key fullbox label / H7 dropdown)
REM    (7) binding panel policy (hide exception grid 66 + four-state background colors)
REM    (8) pending-wave queue dequeue timing (only "start receive task" may start queued waves)
REM    (9) wave-map retention gate (log/WAVE_MAP per-SKU lines must stay bounded)
REM   (10) plan-alloc blocked-grid mask (2026-09-20 field issue #4 no_bind: a grid that is
REM        unlocked but has no bound container must never be claimed; its quota stays intact;
REM        nullptr mask = byte-for-byte legacy behaviour)
REM   (11) plan-alloc hard ceiling + sorting-attribute isolation (2026-09-21: per (SKU,grid)
REM        landed MUST be <= H4 plan; moveGap disabled; classification/shipping capped separately)
REM   (12) outbox response retention for the wave panel (entries -> view -> double click):
REM        response trace (HTTP status / body / note / time) round-trips unchanged and is NEVER
REM        truncated (>1MB body compared char by char); legacy DBs auto-migrate resp_* columns;
REM        entry counters = H7 count + H8 count, isolated per wave; full per-wave fetch (no LIMIT)
REM   (13) 2026-09-22 field request #2 + end-order rule:
REM        lock (=full box) / pre-end flush MUST snapshot the box code and clear the active
REM        binding immediately (no waiting for the H7 receipt) so the dispatch gate sees
REM        "no container"; the H7 message + box code are RETAINED through pending -> failed
REM        (auto retry, retry-exhausted -> manual resend); sendEndToWms has exactly ONE call
REM        site (emitEndReportNow) so the H8 end report is always the LAST message; when any
REM        H7 is still failing the H8 is deferred and the operator decides with the grid list.
REM   (14) 2026-09-25 count-basis unification (field: "history list always shows a few
REM        pieces LESS than the wave panel"):
REM        both the wave panel "sorted pieces" label and the "wave data history" list
REM        "sorted" column MUST be the SAME number = COUNT(DISTINCT barcode) of
REM        sorting_records, i.e. deduplicated PHYSICAL pieces that were really written as
REM        landing detail (same basis as the per-grid H7 detail / box content / WMS upload).
REM        The panel used to read the wave-snapshot sumLocation field (= WaveManager::sorted(),
REM        a PER-FEEDBACK count that also includes duplicate feedbacks and the four
REM        "counted but no landing detail" cases: no_bind / no_match / wrong grid /
REM        over-plan piece) -> the panel was always a few pieces higher than the history
REM        list. Also pins: the EPC-only key of the landing-detail set (so one piece landing
REM        in two grids still counts once) and both write points of that set.
REM
REM  All tests drive the REAL product classes:
REM    test_epc_recognize.cpp          -> RfidPushClient::OnReceive (frame parse + EPC recognition + raw frame)
REM    test_rfid_raw_db.cpp            -> SortingDatabase       (rfid_raw table: insert / query / cleanup)
REM    test_wave_records_batch.cpp     -> SortingDatabase       (batch H7/H8 status + pending-exception counts)
REM    test_restart_resume_db.cpp      -> SortingDatabase       (archive on exit, new-task start, resume inputs)
REM    test_sorting_query_date.cpp     -> SortingDatabase       (date-range filters on all query modes)
REM    test_fullbox_failed_by_order.cpp-> SortingDatabase       (failed H7 by wave + per-wave H7 counters)
REM    test_outbox_response_db.cpp     -> SortingDatabase       (outbox resp_* columns + per-wave entry counts)
REM    test_fullbox_unbind_on_lock.cpp -> SortingDatabase + product source contracts (unbind on lock /
REM                                       box snapshot / single H8 exit / H7 retention)
REM    test_binding_panel_policy.cpp   -> tests/BindingPanelPolicy.h (also used by MainWindow)
REM    test_pending_queue_gate.cpp     -> tests/PendingWaveQueuePolicy.h
REM    test_wave_map_policy.cpp        -> tests/WaveMapLogPolicy.h
REM    test_plan_alloc_blocked_grid.cpp-> WCS_httpServer/PlanAllocTable.h (claim mask)
REM    test_sorted_detail_count.cpp    -> SortingDatabase + product source contracts
REM                                       (wave panel "sorted pieces" == history list "sorted")
REM
REM  Outputs go to .build_check (local scratch, git-ignored) - never into x64\Release,
REM  so this can run while Visual Studio keeps its own incremental build.
REM
REM  NOTE: keep this file ASCII-only -- cmd.exe parses UTF-8 Chinese bytes as command
REM        separators when the console code page is not 65001 (fragmented lines -> errors).
REM
REM  Usage:  tests\run_tests.bat
REM ============================================================================
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

echo ==== [1/14] EPC recognition + raw frame preserved (RfidPushClient) ====
"%QT%\bin\moc.exe" "%SRC%\RfidPushClient.h" -o "%OUT%\moc_RfidPushClient_test.cpp"
if errorlevel 1 goto :fail
cl %FLAGS% %INC% "%ROOT%\tests\test_epc_recognize.cpp" "%SRC%\RfidPushClient.cpp" "%OUT%\moc_RfidPushClient_test.cpp" /Fe"%OUT%\test_epc_recognize.exe" /link /LIBPATH:"%ROOT%\lib" /LIBPATH:"%QT%\lib" HPSocket_U.lib hlog.lib Qt5Core.lib
if errorlevel 1 goto :fail
"%OUT%\test_epc_recognize.exe"
if errorlevel 1 goto :fail

echo.
echo ==== [2/14] raw frame retention in SQLite (SortingDatabase rfid_raw) ====
"%QT%\bin\moc.exe" "%SRC%\ConfigManager.h" -o "%OUT%\moc_ConfigManager_test.cpp"
if errorlevel 1 goto :fail
cl %FLAGS% %INC% "%ROOT%\tests\test_rfid_raw_db.cpp" "%SRC%\SortingDatabase.cpp" "%SRC%\ConfigManager.cpp" "%OUT%\moc_ConfigManager_test.cpp" /Fe"%OUT%\test_rfid_raw_db.exe" /link /LIBPATH:"%ROOT%\lib" /LIBPATH:"%QT%\lib" hlog.lib Qt5Core.lib Qt5Sql.lib
if errorlevel 1 goto :fail
"%OUT%\test_rfid_raw_db.exe"
if errorlevel 1 goto :fail

echo.
echo ==== [3/14] wave-record list batch queries (switch/refresh lag fix) ====
cl %FLAGS% %INC% "%ROOT%\tests\test_wave_records_batch.cpp" "%SRC%\SortingDatabase.cpp" "%SRC%\ConfigManager.cpp" "%OUT%\moc_ConfigManager_test.cpp" /Fe"%OUT%\test_wave_records_batch.exe" /link /LIBPATH:"%ROOT%\lib" /LIBPATH:"%QT%\lib" hlog.lib Qt5Core.lib Qt5Sql.lib
if errorlevel 1 goto :fail
"%OUT%\test_wave_records_batch.exe"
if errorlevel 1 goto :fail

echo.
echo ==== [4/14] close-cut / new-task start / switch-back resume contract ====
cl %FLAGS% %INC% "%ROOT%\tests\test_restart_resume_db.cpp" "%SRC%\SortingDatabase.cpp" "%SRC%\ConfigManager.cpp" "%OUT%\moc_ConfigManager_test.cpp" /Fe"%OUT%\test_restart_resume_db.exe" /link /LIBPATH:"%ROOT%\lib" /LIBPATH:"%QT%\lib" hlog.lib Qt5Core.lib Qt5Sql.lib
if errorlevel 1 goto :fail
"%OUT%\test_restart_resume_db.exe"
if errorlevel 1 goto :fail

echo.
echo ==== [5/14] sorting-record query date filter (required range, all query modes) ====
cl %FLAGS% %INC% "%ROOT%\tests\test_sorting_query_date.cpp" "%SRC%\SortingDatabase.cpp" "%SRC%\ConfigManager.cpp" "%OUT%\moc_ConfigManager_test.cpp" /Fe"%OUT%\test_sorting_query_date.exe" /link /LIBPATH:"%ROOT%\lib" /LIBPATH:"%QT%\lib" hlog.lib Qt5Core.lib Qt5Sql.lib
if errorlevel 1 goto :fail
"%OUT%\test_sorting_query_date.exe"
if errorlevel 1 goto :fail

echo.
echo ==== [6/14] failed H7 by wave + per-wave H7 counters ====
cl %FLAGS% %INC% "%ROOT%\tests\test_fullbox_failed_by_order.cpp" "%SRC%\SortingDatabase.cpp" "%SRC%\ConfigManager.cpp" "%OUT%\moc_ConfigManager_test.cpp" /Fe"%OUT%\test_fullbox_failed_by_order.exe" /link /LIBPATH:"%ROOT%\lib" /LIBPATH:"%QT%\lib" hlog.lib Qt5Core.lib Qt5Sql.lib
if errorlevel 1 goto :fail
"%OUT%\test_fullbox_failed_by_order.exe"
if errorlevel 1 goto :fail

echo.
echo ==== [7/14] binding panel policy (hide exception grid + four-state colors) ====
cl %FLAGS% %INC% "%ROOT%\tests\test_binding_panel_policy.cpp" "%SRC%\ConfigManager.cpp" "%OUT%\moc_ConfigManager_test.cpp" /Fe"%OUT%\test_binding_panel_policy.exe" /link /LIBPATH:"%ROOT%\lib" /LIBPATH:"%QT%\lib" hlog.lib Qt5Core.lib Qt5Sql.lib
if errorlevel 1 goto :fail
"%OUT%\test_binding_panel_policy.exe"
if errorlevel 1 goto :fail

echo.
echo ==== [8/14] pending-wave queue dequeue timing ====
cl %FLAGS% %INC% "%ROOT%\tests\test_pending_queue_gate.cpp" /Fe"%OUT%\test_pending_queue_gate.exe" /link /LIBPATH:"%QT%\lib" Qt5Core.lib
if errorlevel 1 goto :fail
"%OUT%\test_pending_queue_gate.exe"
if errorlevel 1 goto :fail

echo.
echo ==== [9/14] wave-map retention gate (WAVE_MAP per-SKU lines stay bounded) ====
cl %FLAGS% %INC% "%ROOT%\tests\test_wave_map_policy.cpp" /Fe"%OUT%\test_wave_map_policy.exe"
if errorlevel 1 goto :fail
"%OUT%\test_wave_map_policy.exe"
if errorlevel 1 goto :fail

echo.
echo ==== [10/14] plan-alloc blocked-grid mask (unbound grid keeps its quota; nullptr = zero regression) ====
cl %FLAGS% %INC% "%ROOT%\tests\test_plan_alloc_blocked_grid.cpp" /Fe"%OUT%\test_plan_alloc_blocked_grid.exe" /link /LIBPATH:"%QT%\lib" Qt5Core.lib
if errorlevel 1 goto :fail
"%OUT%\test_plan_alloc_blocked_grid.exe"
if errorlevel 1 goto :fail

echo.
echo ==== [11/14] plan-alloc hard ceiling + attribute isolation ====
cl %FLAGS% %INC% "%ROOT%\tests\test_plan_alloc_hard_ceiling.cpp" /Fe"%OUT%\test_plan_alloc_hard_ceiling.exe" /link /LIBPATH:"%QT%\lib" Qt5Core.lib
if errorlevel 1 goto :fail
"%OUT%\test_plan_alloc_hard_ceiling.exe"
if errorlevel 1 goto :fail

echo.
echo ==== [12/14] outbox response retention (panel entries -> view -> double click; no truncation) ====
cl %FLAGS% %INC% "%ROOT%\tests\test_outbox_response_db.cpp" "%SRC%\SortingDatabase.cpp" "%SRC%\ConfigManager.cpp" "%OUT%\moc_ConfigManager_test.cpp" /Fe"%OUT%\test_outbox_response_db.exe" /link /LIBPATH:"%ROOT%\lib" /LIBPATH:"%QT%\lib" hlog.lib Qt5Core.lib Qt5Sql.lib
if errorlevel 1 goto :fail
"%OUT%\test_outbox_response_db.exe"
if errorlevel 1 goto :fail

echo.
echo ==== [13/14] unbind-on-lock + H7 retention + single H8 exit ====
cl %FLAGS% %INC% "%ROOT%\tests\test_fullbox_unbind_on_lock.cpp" "%SRC%\SortingDatabase.cpp" "%SRC%\ConfigManager.cpp" "%OUT%\moc_ConfigManager_test.cpp" /Fe"%OUT%\test_fullbox_unbind_on_lock.exe" /link /LIBPATH:"%ROOT%\lib" /LIBPATH:"%QT%\lib" hlog.lib Qt5Core.lib Qt5Sql.lib
if errorlevel 1 goto :fail
"%OUT%\test_fullbox_unbind_on_lock.exe"
if errorlevel 1 goto :fail

echo.
echo ==== [14/14] panel "sorted pieces" == history list "sorted" (one count basis) ====
cl %FLAGS% %INC% "%ROOT%\tests\test_sorted_detail_count.cpp" "%SRC%\SortingDatabase.cpp" "%SRC%\ConfigManager.cpp" "%OUT%\moc_ConfigManager_test.cpp" /Fe"%OUT%\test_sorted_detail_count.exe" /link /LIBPATH:"%ROOT%\lib" /LIBPATH:"%QT%\lib" hlog.lib Qt5Core.lib Qt5Sql.lib
if errorlevel 1 goto :fail
"%OUT%\test_sorted_detail_count.exe"
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
