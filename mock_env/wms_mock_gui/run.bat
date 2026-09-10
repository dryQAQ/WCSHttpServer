@echo off
chcp 65001 >nul
title WMS 报文 Mock 测试台
cd /d "%~dp0"
echo ============================================================
echo    WMS 报文 Mock 测试台
echo    优先使用免Python的单文件 EXE；无 EXE 时回退 python 源码
echo ============================================================
echo.
if exist "dist\wms_mock_gui.exe" (
    echo [运行 EXE 版，无需安装 Python]
    start "" "dist\wms_mock_gui.exe" %*
    echo EXE 已在后台运行：浏览器访问 http://127.0.0.1:8765
    echo 页面右上角「退出服务」可停止程序
    goto :eof
)
where python >nul 2>nul
if %errorlevel%==0 (
    python wms_mock_gui.py %*
) else (
    py -3 wms_mock_gui.py %*
)
echo.
pause
