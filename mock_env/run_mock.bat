@echo off
rem Start the WCS local mock console (Python + tkinter, no 3rd-party deps).
chcp 65001 >nul
set PYTHONIOENCODING=gbk
cd /d "%~dp0"
python mock_app.py
if errorlevel 1 pause
