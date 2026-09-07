@echo off
rem Restore production config from latest backup, then restart WCS manually.
chcp 65001 >nul
set PYTHONIOENCODING=gbk
cd /d "%~dp0"
python mock_swap_config.py --restore
if errorlevel 1 (
  echo.
  echo [FAILED] see messages above.
  pause
  exit /b 1
)
echo.
echo Done. Restart WCS_httpServer.exe to load production config.
pause
