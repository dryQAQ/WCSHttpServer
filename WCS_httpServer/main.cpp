// ============================================================================
// WMS退货HTTP服务 — 独立应用程序入口
//
// 单端口架构:
//   8191: 对外接收WMS推送及提供查询API
//        (POST /api/.../InsertWaveInfo, GET /api/query,
//         POST /api/markSorted, POST /api/markException, GET /api/waveStatus)
//
// HP-Socket初始化: 程序启动时调用 HP_Init(HP_SOCKET, HP_HTTP)
// ============================================================================

#include <QApplication>
#include <QDir>
#include "MainWindow.h"
#include "ConfigManager.h"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("WMS_HttpServer");
    app.setApplicationVersion("1.0.0");

    QDir().mkpath(QApplication::applicationDirPath() + "/config");
    ConfigManager::instance()->load();

    MainWindow mainWindow;
    mainWindow.show();

    return app.exec();
}
