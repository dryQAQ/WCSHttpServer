@echo off
cd /d "%~dp0"
move /Y "sorting_records.db" "sorting_records.db.bak"
move /Y "sorting_records.db-wal" "sorting_records.db-wal.bak"
move /Y "sorting_records.db-shm" "sorting_records.db-shm.bak"
echo Done.
pause