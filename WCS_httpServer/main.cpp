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
#include <QMessageBox>   // ★ 2026-09-06 单实例提示
#include <QtGlobal>      // ★ 2026-09-07 Qt 消息钩子（QtMsgType/QMessageLogContext）
#include <QPalette>      // ★ 2026-09-13 UI主题（深色背景）
#include <QColor>
#include <QStyleFactory>
#include <atomic>
#include <string>
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

// ★ 2026-09-07 Qt 消息钩子：qFatal/qCritical/qWarning 文本落 run.log；
//   崩溃前最后一条 Qt 消息（qFatal 文本即 abort 根因）随 crash.log 落盘
static char g_lastQtLevel[16]  = {0};
static char g_lastQtMsg[1024]  = {0};

static void QtMsgHook(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    const char* lvl = (type == QtFatalMsg)   ? "FATAL"
                    : (type == QtCriticalMsg) ? "CRITICAL"
                    : (type == QtWarningMsg)  ? "WARNING" : "INFO";
    QByteArray mb = msg.toLocal8Bit();
    LOG_INFO("[Qt%s] %s (%s:%d)", lvl, mb.constData(),
             ctx.file ? ctx.file : "", ctx.line);
    if (type == QtFatalMsg || type == QtCriticalMsg || type == QtWarningMsg)
    {
        strcpy_s(g_lastQtLevel, lvl);
        size_t n = (size_t)mb.size();
        if (n >= sizeof(g_lastQtMsg)) n = sizeof(g_lastQtMsg) - 1;
        memcpy(g_lastQtMsg, mb.constData(), n);
        g_lastQtMsg[n] = '\0';
    }
}

// ──── 全局崩溃捕获（2026-09-04 新增，定位"空按停止闪退"等异常退出）────
// ★ 2026-09-04 修复"无法生成 crash 文件"：
//   原实现仅注册 SetUnhandledExceptionFilter（只能捕获 SEH 异常，如 0xC0000005 访问冲突），
//   但 Qt 的 qFatal（如 "QThread: Destroyed while thread is still running"）走的是 abort()→SIGABRT，
//   是 C 信号而非 SEH 异常，不会触发该 handler，因此既无 crash.log 也无 dmp。
//   现将"写 crash.log + 写 minidump"抽成公共函数，同时注册 SEH handler 与 signal handler。
static void WriteCrashReport(EXCEPTION_POINTERS* pException)
{
    // 1. 记录崩溃摘要 + 栈回溯到 crash.log（exe 同目录）
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

        // ★ 2026-09-06 栈回溯：返回地址列表（无需符号/调试器，配合 dmp 定位崩溃函数）
        char buf3[2048];
        int off = 0;
        {
            void* frames[32] = { 0 };
            USHORT count = CaptureStackBackTrace(0, 32, frames, nullptr);
            off = wsprintfA(buf3, "stack   : %u frames\r\n", count);
            for (USHORT i = 0; i < count && off < (int)sizeof(buf3) - 48; ++i)
                off += wsprintfA(buf3 + off, "  [%02u] %p\r\n", i, frames[i]);
        }
        WriteFile(hLog, buf3, (DWORD)off, &written, NULL);

        // ★ 2026-09-07 Qt 最后消息（qFatal 文本即 abort 根因）
        if (g_lastQtMsg[0])
        {
            char qtBuf[1200];
            int nq = wsprintfA(qtBuf, "\r\nqt-last: [%s] %s\r\n", g_lastQtLevel, g_lastQtMsg);
            WriteFile(hLog, qtBuf, (DWORD)nq, &written, NULL);
        }
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

// ============================================================================
// ★ 2026-09-13 UI主题：深炭蓝灰背景 #18252E + 红色边框 #C83030
//   · Fusion 样式 + 深色 QPalette：保证菜单/滚动条/输入框/禁用态等原生控件一致；
//   · 全局 QSS 只覆盖"底色 + 边框"两类关键属性，各面板自身的强调色（按钮/状态灯）保留；
//   · 面板内的表头/表格/输入框边框统一为红色 #C83030。
// ============================================================================
static void ApplyDarkTheme(QApplication& app)
{
    const QColor bg    ("#18252E");   // 深炭蓝灰（窗口背景）
    const QColor base  ("#101A22");   // 输入框/表格底色（比背景更深一档）
    const QColor alt   ("#16222B");   // 表格隔行
    const QColor header("#1F2F3A");   // 表头/普通按钮
    const QColor fg    ("#E6EDF3");   // 主文字
    const QColor fgDim ("#A8B7C2");   // 次要文字
    const QColor border("#C83030");   // 边框（红）
    const QColor disFg ("#6B7B87");   // 禁用文字

    app.setStyle(QStyleFactory::create("Fusion"));

    QPalette p;
    p.setColor(QPalette::Window,          bg);
    p.setColor(QPalette::WindowText,      fg);
    p.setColor(QPalette::Base,            base);
    p.setColor(QPalette::AlternateBase,   alt);
    p.setColor(QPalette::Text,            fg);
    p.setColor(QPalette::Button,          header);
    p.setColor(QPalette::ButtonText,      fg);
    p.setColor(QPalette::BrightText,      QColor("#FF6B6B"));
    p.setColor(QPalette::ToolTipBase,     bg);
    p.setColor(QPalette::ToolTipText,     fg);
    p.setColor(QPalette::Highlight,       border);
    p.setColor(QPalette::HighlightedText, QColor("#FFFFFF"));
    p.setColor(QPalette::Link,            QColor("#4FC3F7"));
    p.setColor(QPalette::Disabled, QPalette::Text,       disFg);
    p.setColor(QPalette::Disabled, QPalette::WindowText, disFg);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, disFg);
    app.setPalette(p);

    app.setStyleSheet(QString(
        "QMainWindow, QDialog, QMessageBox { background-color: %1; }"
        "QGroupBox { border: 1px solid %2; border-radius: 4px; margin-top: 12px; padding-top: 6px; }"
        "QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; left: 10px; padding: 0 4px; color: %3; }"
        "QTabWidget::pane { border: 1px solid %2; background: %1; }"
        "QTabBar::tab { background: %4; color: %5; border: 1px solid %2; padding: 8px 8px; margin: 2px 0; }"
        "QTabBar::tab:selected { background: %2; color: #FFFFFF; }"
        "QSplitter::handle { background-color: %2; }"
        "QSplitter::handle:hover { background-color: #E04A4A; }"
        "QTableWidget, QTableView, QTreeView, QListWidget, QListView { background-color: %6; alternate-background-color: %7;"
        " gridline-color: #2C3E4C; border: 1px solid %2; }"
        "QHeaderView::section { background-color: %4; color: %3; border: 1px solid %2; padding: 4px; }"
        "QLineEdit, QComboBox, QSpinBox, QDateEdit, QPlainTextEdit, QTextEdit { background-color: %6; color: %3;"
        " border: 1px solid %2; border-radius: 3px; padding: 2px 4px; }"
        "QComboBox QAbstractItemView { background-color: %6; color: %3; selection-background-color: %2; selection-color: #FFFFFF; }"
        "QPushButton { background-color: %4; color: %3; border: 1px solid %2; border-radius: 4px; padding: 4px 10px; }"
        "QPushButton:hover { background-color: #27394A; }"
        "QPushButton:disabled { color: %8; border-color: #5A2A2A; }"
        "QScrollArea { border: 1px solid %2; background-color: %1; }"
        "QScrollBar:vertical, QScrollBar:horizontal { background: %6; border: none; }"
        "QScrollBar::handle { background: #2C3E4C; border-radius: 3px; min-height: 24px; min-width: 24px; }"
        "QScrollBar::handle:hover { background: %2; }"
        "QCheckBox, QRadioButton { color: %3; }"
        "QToolTip { background-color: %1; color: %3; border: 1px solid %2; }"
        "QMenu { background-color: %4; color: %3; border: 1px solid %2; }"
        "QMenu::item:selected { background-color: %2; color: #FFFFFF; }"
    ).arg(bg.name()).arg(border.name()).arg(fg.name()).arg(header.name())
     .arg(fgDim.name()).arg(base.name()).arg(alt.name()).arg(disFg.name()));
}

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

    // ★ 2026-09-13 UI主题：深炭蓝灰背景 #18252E + 红色边框 #C83030
    //   在创建任何窗口/弹窗之前应用（含单实例提示框、主窗口、各对话弹窗）
    ApplyDarkTheme(app);

    // ★ 2026-09-07 Qt 消息钩子：qFatal 等文本进 run.log（崩溃前最后一条=abort 根因）
    qInstallMessageHandler(QtMsgHook);

    // ★ 2026-09-06：单实例互斥（防止双开导致端口 8191/2000/2010 被占用、数据库冲突）
    //   已有实例在运行时，第二个实例直接提示并退出（无需手动杀进程，用旧实例即可）
    //   Local\ 命名空间：同登录会话内互斥；进程结束（含崩溃）时互斥体自动释放
    HANDLE hSingleMutex = CreateMutexW(NULL, FALSE, L"Local\\WCS_HttpServer_SingleInstance");
    if (hSingleMutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        QMessageBox::warning(nullptr, QObject::tr("程序已在运行"),
            QObject::tr("WCS_httpServer 已经在运行中，请勿重复启动。\n\n"
                        "如果找不到运行窗口：\n"
                        "  ① 任务管理器 → 结束 WCS_httpServer.exe 进程\n"
                        "  ② 或重启电脑后重新打开本程序"));
        CloseHandle(hSingleMutex);
        return 0;
    }
    // hSingleMutex 保持到进程结束（随进程退出自动释放，无需显式关闭）

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
    // ★ 2026-09-06：RFID/SEND 分类日志已并入 run.log（现场反馈独立文件无法写入/不便查看），不再建 RFID/SEND 目录
    QDir().mkpath(exeDir + "/data");            // 分拣数据库目录（UI 查询独立于服务，需提前创建）

    // ★ 2026-09-04 启动步骤日志（方便排查"程序当前在做什么"）
    LOG_INFO("[启动] 程序启动 exeDir=%s", exeDir.toLocal8Bit().constData());
    LOG_INFO("[启动] 目录检查完成（config/log/data）");

    ConfigManager::instance()->load();
    LOG_INFO("[启动] 配置加载完成");

    MainWindow mainWindow;
    mainWindow.showMaximized();   // ★ 2026-09-08 UI调整：默认打开为最大化全屏
    LOG_INFO("[启动] 主窗口已显示（最大化）；设备(PLC/RFID)自动连接中，任务接收由界面按钮控制"
             "（开机自动开始接收任务一次，见 define.h AUTO_START_RECEIVE_ON_BOOT）");

    return app.exec();
}
