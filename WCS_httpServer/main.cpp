// ============================================================================
// WMS退货HTTP服务 — 独立应用程序入口
//
// 单端口架构:
//   8191: 对外接收WMS推送及提供查询API
//        (POST /api/.../InsertWaveInfo, GET /api/query,
//         POST /api/markSorted, POST /api/markException, GET /api/waveStatus)
//
// 日志系统:
//   hlog (log4cxx) → ./log/WCS/WCS.log            (WCS_INFO)
//                   → ./log/HTTP/http.log          (HTTP_INFO)
//                   → ./log/Run/run.log            (LOG_INFO)
//                   → ./log/PLC/PLC.log            (PLC_INFO)
//                   → ./log/LIFECYCLE/lifecycle.log (LIFE_LOG)
//                   → ./log/JT/JT.log              (JT_INFO)
//                   → ./log/DataBase/data.log      (Data_INFO)
//                   → ./log/Image/data.log         (ImageSave_INFO)
//                   → ./log/PLCWarn/PLCWarn.log    (PLCWarn_INFO)
// ============================================================================

#include <QApplication>
#include <QDir>
#include "MainWindow.h"
#include "ConfigManager.h"
#include "hlog1.h"

// ──── HTTP 服务专用日志宏（写入 ./log/HTTP/http.log）────
#define HTTP_INFO(fmt, ...)  hlog_format(HLOG_LEVEL_INFO,  "HTTP", "\t" fmt, ##__VA_ARGS__)
#define HTTP_WARN(fmt, ...)  hlog_format(HLOG_LEVEL_WARN,  "HTTP", "\t" fmt, ##__VA_ARGS__)
#define HTTP_ERROR(fmt, ...) hlog_format(HLOG_LEVEL_ERROR, "HTTP", "\t" fmt, ##__VA_ARGS__)

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("WMS_HttpServer");
    app.setApplicationVersion("1.0.0");

    // 创建必要目录
    QString exeDir = QApplication::applicationDirPath();
    QDir().mkpath(exeDir + "/config");
    QDir().mkpath(exeDir + "/log");
    QDir().mkpath(exeDir + "/log/WCS");
    QDir().mkpath(exeDir + "/log/HTTP");
    QDir().mkpath(exeDir + "/log/Run");
    QDir().mkpath(exeDir + "/log/PLC");         // PLC通信日志
    QDir().mkpath(exeDir + "/log/LIFECYCLE");   // 条码生命周期日志
    QDir().mkpath(exeDir + "/log/JT");          // 极兔分拣日志
    QDir().mkpath(exeDir + "/log/DataBase");    // 数据库操作日志
    QDir().mkpath(exeDir + "/log/Image");       // 图片处理日志
    QDir().mkpath(exeDir + "/log/PLCWarn");     // PLC告警日志

    ConfigManager::instance()->load();

    MainWindow mainWindow;
    mainWindow.show();

    return app.exec();
}
