// ============================================================================
// WMS退货HTTP服务 — 独立应用程序入口
//
// 双端口架构:
//   8191: 对外接收WMS推送 (POST /api/.../InsertWaveInfo)
//   8192: 对内提供WCSApp查询 (GET /api/query, POST /api/markSorted等)
//
// HP-Socket初始化: 程序启动时调用 HP_Init(HP_SOCKET, HP_HTTP)
// ============================================================================

#include <QApplication>
#include <QDir>
#include "MainWindow.h"
#include "ConfigManager.h"
#include "log_center.h"
#include "HPSocket.h"

int main(int argc, char* argv[])
{
    // ──── HP-Socket 全局初始化 ────
    HP_Init(HP_SOCKET, HP_HTTP);

    // ──── Qt 初始化 ────
    QApplication app(argc, argv);
    app.setApplicationName("WMS_HttpServer");
    app.setApplicationVersion("1.0.0");

    // 确保配置目录存在
    QDir().mkpath(QApplication::applicationDirPath() + "/config");

    // ──── 加载配置 ────
    ConfigManager::instance()->load();

    // ──── 启动主窗口 ────
    MainWindow mainWindow;
    mainWindow.show();

    int result = app.exec();

    // ──── 清理 ────
    HP_Cleanup();

    return result;
}
