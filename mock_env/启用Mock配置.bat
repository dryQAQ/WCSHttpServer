@echo off
rem Enable Mock config for local WCS test (backup + swap), then restart WCS manually.
chcp 65001 >nul
set PYTHONIOENCODING=gbk
cd /d "%~dp0"
python mock_swap_config.py --enable
if errorlevel 1 (
  echo.
  echo [FAILED] see messages above.
  pause
  exit /b 1
)
echo.
echo Done. Start WCS_httpServer.exe, then run run_mock.bat.
pause
