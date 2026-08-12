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
//                   → ./log/JT/JT.log          (JT_INFO)
//                   → ./log/DataBase/DataBase.log (Data_INFO)
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
    QDir().mkpath(exeDir + "/log/LIFECYCLE");   // EPC编码生命周期日志
    QDir().mkpath(exeDir + "/log/DataBase");    // 数据库操作日志
    QDir().mkpath(exeDir + "/log/JT");          // 极兔分拣日志
    QDir().mkpath(exeDir + "/log/EPC");         // EpcCache 日志
    QDir().mkpath(exeDir + "/data");            // 分拣数据库目录（UI 查询独立于服务，需提前创建）

    ConfigManager::instance()->load();

    MainWindow mainWindow;
    mainWindow.show();

    return app.exec();
}
