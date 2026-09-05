// ============================================================================
// WMS退货分拣HTTP服务 — 独立应用程序入口
//
// 单端口架构:
//   8191: 对外接收WMS推送及提供查询API
//
// 有效API接口（仅WMS系统调用）:
//   POST /api/DispatchSortingCommand/InsertWaveInfo          — WMS推送波次数据（含EPC编码-格口映射）
//   POST /api/DispatchSortingCommand/BindingLatticePort      — WMS绑定容器号到格口
//   POST /api/DispatchSortingCommand/InsertWaveIn            — WMS退货任务取消
//
// 日志系统:
//   hlog (log4cxx) → ./log/WCS/WCS.log       (WCS_INFO/WCS_WARN/WCS_ERROR)
//                   → ./log/HTTP/http.log     (HTTP_INFO/HTTP_WARN/HTTP_ERROR)
//                   → ./log/Run/run.log        (LOG_INFO/LOG_WARN/LOG_ERROR)
//                   → ./log/PLC/PLC.log        (PLC_INFO/PLC_WARN/PLC_ERROR)
//                   → ./log/LIFECYCLE/lifecycle.log (LIFE_LOG)
//                   → ./log/DataBase/DataBase.log (Data_INFO)
//
// 崩溃捕获（2026-09-04）:
//   程序异常退出（闪退）时自动在 exe 目录生成 crash_*.dmp（minidump）与 crash.log，
//   便于定位崩溃现场（配合 VS 打开 dmp 即可看到崩溃调用栈）。
// ============================================================================

#include <QApplication>
#include <QTextCodec>
#include <QDir>
// ★ 必须在 windows.h 之前包含 winsock2.h：
//   windows.h 会拉入旧版 winsock.h，而 MainWindow.h→HttpServer.h→HPSocket.h
//   使用的是 winsock2.h，两者同时包含会导致 sockaddr/fd_set 等类型重定义错误
#include <winsock2.h>
#include <windows.h>
#include <dbghelp.h>
#include <csignal>
#pragma comment(lib, "dbghelp.lib")

#include "MainWindow.h"
#include "ConfigManager.h"
#include "LogService.h"

// ──── 全局崩溃捕获（2026-09-04 新增，定位"空按停止闪退"等异常退出）────
// ★ 2026-09-04 修复"无法生成 crash 文件"：
//   原实现仅注册 SetUnhandledExceptionFilter（只能捕获 SEH 异常，如 0xC0000005 访问冲突），
//   但 Qt 的 qFatal（如 "QThread: Destroyed while thread is still running"）走的是 abort()→SIGABRT，
//   是 C 信号而非 SEH 异常，不会触发该 handler，因此既无 crash.log 也无 dmp。
//   现将"写 crash.log + 写 minidump"抽成公共函数，同时注册 SEH handler 与 signal handler。
static void WriteCrashReport(EXCEPTION_POINTERS* pException)
{
    // 1. 记录崩溃摘要到 crash.log（exe 同目录）
    HANDLE hLog = CreateFileA("crash.log", GENERIC_WRITE, FILE_SHARE_READ,
                              NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hLog != INVALID_HANDLE_VALUE)
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char buf2[512];
        int n2 = wsprintfA(buf2,
            "==== crash ====\r\n"
            "time    : %04u-%02u-%02u %02u:%02u:%02u\r\n"
            "code    : %08X\r\n"
            "address : %p\r\n"
            "thread  : %u\r\n\r\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
            pException ? (DWORD)pException->ExceptionRecord->ExceptionCode : 0,
            pException ? pException->ExceptionRecord->ExceptionAddress : nullptr,
            (DWORD)GetCurrentThreadId());
        SetFilePointer(hLog, 0, NULL, FILE_END);
        DWORD written = 0;
        WriteFile(hLog, buf2, (DWORD)strlen(buf2), &written, NULL);
        CloseHandle(hLog);
    }

    // 2. 生成 minidump（crash_YYYYMMDD_HHMMSS.dmp，exe 同目录）
    SYSTEMTIME st;
    GetLocalTime(&st);
    char path[MAX_PATH];
    wsprintfA(path, "crash_%04u%02u%02u_%02u%02u%02u.dmp",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    HANDLE hDump = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hDump != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId           = GetCurrentThreadId();
        mei.ExceptionPointers  = pException;
        mei.ClientPointers     = FALSE;
        // signal 回调（无 SEH 上下文）时 pException 为 nullptr，MiniDump 自动抓取当前线程上下文
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hDump,
                          MiniDumpNormal,
                          pException ? &mei : nullptr, nullptr, nullptr);
        CloseHandle(hDump);
    }
}

static LONG WINAPI AppCrashHandler(EXCEPTION_POINTERS* pException)
{
    WriteCrashReport(pException);
    return EXCEPTION_CONTINUE_SEARCH;  // ★ 交还系统正常终止（避免在崩溃点继续执行）
}

// ★ 捕获 abort()/终止类信号（Qt qFatal 最终走 SIGABRT，不经过 SEH）
static void CrashSignalHandler(int sig)
{
    WriteCrashReport(nullptr);
    signal(sig, SIG_DFL);  // 恢复默认处理
    raise(sig);            // 重新触发，让系统按默认方式终止
}

// ──── 日志系统说明（宏定义已移至 LogService.h 统一管理）────
// ============================================================================

int main(int argc, char* argv[])
{
    // ★ 2026-09-04 注册崩溃捕获：
    //   ① SEH 异常（访问冲突 0xC0000005 等）→ AppCrashHandler
    //   ② abort()/终止类信号（Qt qFatal 走 SIGABRT）→ CrashSignalHandler
    SetUnhandledExceptionFilter(AppCrashHandler);
    signal(SIGABRT, CrashSignalHandler);
    signal(SIGSEGV, CrashSignalHandler);
    signal(SIGFPE,  CrashSignalHandler);
    signal(SIGILL,  CrashSignalHandler);

    QApplication app(argc, argv);
    app.setApplicationName("WMS_HttpServer");
    app.setApplicationVersion("1.0.0");

    // ★ 统一本地编码为 UTF-8（与源码 /utf-8 编译保持一致）：
    //   否则 QString::toLocal8Bit()/fromLocal8Bit() 在中文系统走 GBK，
    //   而源码中文字面量是 UTF-8，两者混写进 hlog 日志文件会变成乱码。
    QTextCodec::setCodecForLocale(QTextCodec::codecForName("UTF-8"));

    // 创建必要目录
    QString exeDir = QApplication::applicationDirPath();
    QDir().mkpath(exeDir + "/config");
    QDir().mkpath(exeDir + "/log");
    QDir().mkpath(exeDir + "/log/WCS");
    QDir().mkpath(exeDir + "/log/HTTP");
    QDir().mkpath(exeDir + "/log/Run");
    QDir().mkpath(exeDir + "/log/PLC");         // PLC通信日志
    QDir().mkpath(exeDir + "/log/LIFECYCLE");   // EPC编码生命周期日志
    QDir().mkpath(exeDir + "/log/DataBase");    // 数据库操作日志
    
    QDir().mkpath(exeDir + "/log/EPC");         // EpcCache 日志
    QDir().mkpath(exeDir + "/log/RFID");        // RFID 推送客户端日志
    QDir().mkpath(exeDir + "/log/RFID");        // ★ 2026-09-04 RFID 推送原始报文日志
    QDir().mkpath(exeDir + "/data");            // 分拣数据库目录（UI 查询独立于服务，需提前创建）

    // ★ 2026-09-04 启动步骤日志（方便排查"程序当前在做什么"）
    LOG_INFO("[启动] 程序启动 exeDir=%s", exeDir.toLocal8Bit().constData());
    LOG_INFO("[启动] 目录检查完成（config/log/data）");

    ConfigManager::instance()->load();
    LOG_INFO("[启动] 配置加载完成");

    MainWindow mainWindow;
    mainWindow.show();
    LOG_INFO("[启动] 主窗口已显示，等待用户操作（点击\"开始启动\"后服务才监听端口）");

    return app.exec();
}
