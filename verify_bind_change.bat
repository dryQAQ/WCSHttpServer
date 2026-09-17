@echo off
REM ============================================================================
REM  verify_bind_change.bat -- syntax/semantic compile check only (/Zs, no .obj)
REM  Purpose: verify changed .cpp files compile while Visual Studio is open,
REM           without writing into x64\Release (avoids fighting VS incremental build).
REM  Usage: verify_bind_change.bat [file1.cpp file2.cpp ...]
REM         default = HttpServer.cpp MainWindow.cpp
REM ============================================================================
setlocal

call "D:\vs2019\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 ( echo [FAIL] vcvars64 failed & exit /b 1 )

set SRC=D:\WCS\WCSApp\WCS_httpServer\WCS_httpServer
set X64=%SRC%\x64\Release

set FILES=%*
if "%FILES%"=="" set FILES=HttpServer.cpp MainWindow.cpp

set FAILED=0
for %%F in (%FILES%) do (
  echo === checking %%F ===
  cl.exe /nologo /Zs /c ^
    /I"%X64%\uic" /I"%X64%\moc" ^
    /I"D:\WCS\WCSApp\WCS_httpServer\include\hlog" ^
    /I"D:\WCS\WCSApp\WCS_httpServer\include\HPSocket" ^
    /I"%X64%\qmake\temp" ^
    /I"D:\Qt\5.15.2\msvc2019_64\include" ^
    /I"D:\Qt\5.15.2\msvc2019_64\include\QtWidgets" ^
    /I"D:\Qt\5.15.2\msvc2019_64\include\QtGui" ^
    /I"D:\Qt\5.15.2\msvc2019_64\include\QtANGLE" ^
    /I"D:\Qt\5.15.2\msvc2019_64\include\QtSql" ^
    /I"D:\Qt\5.15.2\msvc2019_64\include\QtNetwork" ^
    /I"D:\Qt\5.15.2\msvc2019_64\include\QtCore" ^
    /I"D:\Qt\5.15.2\msvc2019_64\mkspecs\win32-msvc" ^
    /W1 /WX- /diagnostics:column /O2 ^
    /D _WINDOWS /D UNICODE /D _UNICODE /D WIN32 /D _ENABLE_EXTENDED_ALIGNED_STORAGE ^
    /D WIN64 /D NDEBUG /D QT_NO_DEBUG /D QT_WIDGETS_LIB /D QT_GUI_LIB /D QT_SQL_LIB ^
    /D QT_NETWORK_LIB /D QT_CORE_LIB ^
    /Gm- /EHsc /MD /GS /fp:precise /Zc:wchar_t /Zc:forScope /Zc:inline /std:c++17 ^
    /Gd /TP /FC /utf-8 ^
    -Zc:rvalueCast -Zc:inline -Zc:strictStrings -Zc:throwingNew -Zc:referenceBinding -Zc:__cplusplus ^
    "%SRC%\%%F"
  if errorlevel 1 set FAILED=1
)

if %FAILED%==1 ( echo. & echo [FAIL] compile check FAILED & exit /b 1 )
echo. & echo [OK] all checked files compile with no errors
endlocal
