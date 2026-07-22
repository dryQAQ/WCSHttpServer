#pragma once
// ============================================================================
// ConfigManager.h — 配置管理（兼容WCSApp的wcs.xml格式 + 本程序独立配置）
// ============================================================================

#include <QObject>
#include <QString>

struct AppConfig
{
    // ──── 服务配置 ────
    int     wmsListenPort    = 8191;   // WMS推送监听端口
    int     internalPort     = 8192;   // WCSApp查询端口

    // ──── 回传配置 ────
    QString feedbackUrl;               // 正式环境回传URL
    QString feedbackTestUrl;           // 测试环境回传URL
    QString appkey;
    QString appkeyTest;
    bool    useTestEnv  = true;        // 使用测试环境

    // ──── 波次配置 ────
    int     waveTimeoutMin = 0;        // 波次超时(分钟, 0=不超时)
    int     maxRetryCount  = 3;        // 最大重试次数

    // ──── 日志 ────
    int     logRetainDays  = 30;       // 日志保留天数

    bool loadFromFile(const QString& path = "./config/http_server.xml");
    bool saveToFile(const QString& path = "./config/http_server.xml") const;

    // 获取当前生效的回传URL
    QString activeFeedbackUrl() const
    {
        return useTestEnv ? feedbackTestUrl : feedbackUrl;
    }

    // 获取当前生效的appkey
    QString activeAppkey() const
    {
        return useTestEnv ? appkeyTest : appkey;
    }
};

class ConfigManager : public QObject
{
    Q_OBJECT
public:
    static ConfigManager* instance();

    AppConfig& config() { return m_config; }
    bool load();
    bool save();

private:
    ConfigManager() = default;
    AppConfig m_config;
};
