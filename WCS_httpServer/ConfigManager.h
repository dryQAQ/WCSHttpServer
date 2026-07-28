#pragma once
// ============================================================================
// ConfigManager.h — 配置管理（XML 格式，define.h 宏作为默认值）
//
// 配置文件: {exe}/config/http_server.xml
// 首次运行自动生成，所有默认值从 define.h 宏读取
// ============================================================================

#include <QObject>
#include <QString>
#include <QMap>
#include "define.h"

struct AppConfig
{
    // ──── 服务配置 ────
    int     wmsListenPort    = WMS_LISTEN_PORT;       // HTTP 监听端口（默认 8191）

    // ──── 回传配置（WCS → WMS）────
    QString feedbackUrl      = WMS_FEEDBACK_URL;      // 正式环境回传 URL
    QString feedbackTestUrl  = WMS_FEEDBACK_URL_TEST; // 测试环境回传 URL
    QString appkey            = WMS_APPKEY;            // 正式环境 AppKey
    QString appkeyTest        = WMS_APPKEY_TEST;       // 测试环境 AppKey
    bool    useTestEnv        = true;                  // true=使用测试环境

    // ──── 波次配置 ────
    int     waveTimeoutMin    = WAVE_TIMEOUT_MIN_DEFAULT; // 波次超时(分钟, 0=不超时)
    int     maxRetryCount     = WAVE_MAX_RETRY;           // 异常 SKU 最大重试次数

    // ──── 日志 ────
    int     logRetainDays     = LOG_RETAIN_DAYS;          // 日志保留天数

    // ──── 容器绑定（持久化，格口号→容器号）────
    QMap<QString, QString> containerBindings;

    bool loadFromFile(const QString& path = CONFIG_FILE);
    bool saveToFile(const QString& path = CONFIG_FILE) const;

    // 获取当前生效的回传 URL（根据 useTestEnv 切换测试/正式环境）
    QString activeFeedbackUrl() const
    {
        return useTestEnv ? feedbackTestUrl : feedbackUrl;
    }

    // 获取当前生效的 AppKey（根据 useTestEnv 切换测试/正式环境）
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
    bool load();    // 加载配置文件，不存在则自动创建
    bool save();    // 保存当前配置到文件

private:
    ConfigManager() = default;
    AppConfig m_config;
};
