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

    // ──── WMS 业务参数 ────
    QString warehouseCode    = WMS_WAREHOUSE_CODE;    // 仓库编码
    QString goodsOwner       = WMS_GOODS_OWNER;       // 货主编码

    // ──── 波次配置 ────
    int     waveTimeoutMin    = WAVE_TIMEOUT_MIN_DEFAULT; // 波次超时(分钟, 0=不超时)
    int     maxRetryCount     = WAVE_MAX_RETRY;           // 异常 SKU 最大重试次数

    // ──── 网络超时 ────
    int     httpTimeoutMs         = HTTP_TIMEOUT_MS;          // HTTP 请求超时(ms)
    int     waveCompleteTimeoutMs = WAVE_COMPLETE_TIMEOUT_MS; // 波次完成回传超时(ms)

    // ──── PLC 通信 ────
    int     plcListenPort     = PLC_LISTEN_PORT;        // PLC TCP 监听端口（默认 8192）
    QString plcS7Ip           = PLC_S7_IP;              // S7 PLC IP 地址
    int     plcS7Rack         = PLC_S7_RACK;            // S7 机架号
    int     plcS7Slot         = PLC_S7_SLOT;            // S7 槽位号

    // ──── 相机通信 ────
    int     cameraListenPort  = CAMERA_LISTEN_PORT;  // 相机 TCP 监听端口（默认 8193）
    int     cameraPlatType    = 1;              // 相机协议类型: 1=DaHua({条码|小车号}), 2=Kenyence(STX{car:barcode}ETX)

    // ──── 线程池 ────
    int     businessPoolSize  = BUSINESS_POOL_SIZE;     // 业务线程池大小
    int     plcSendPoolSize   = PLC_SEND_POOL_SIZE;     // PLC 发送线程池大小
    int     plcRecvPoolSize   = PLC_RECV_POOL_SIZE;     // PLC 反馈接收专用线程池大小
    int     cameraProcPoolSize = CAMERA_PROC_POOL_SIZE; // 相机数据处理专用线程池大小

    // ──── RFID ────
    QString rfidUrl          = RFID_QUERY_URL;           // RFID EPC查询接口URL

    // ──── API 路由（WMS 调用 WCS 的接口路径，可配置以适应 WMS 路径变更）────
    QString apiInsertWaveInfo     = API_INSERT_WAVE_INFO;      // ① WMS 推送波次数据 (POST)
    QString apiBindingLatticePort = API_BINDING_LATTICE_PORT;  // ② WMS 绑定格口容器 (POST)
    QString apiInsertWaveIn       = API_INSERT_WAVE_IN;        // ③ WMS 退货任务取消 (POST)

    // ──── 日志 ────
    int     logRetainDays     = LOG_RETAIN_DAYS;          // 日志保留天数

    // ──── 容器绑定（持久化，格口号→容器号）────
    QMap<QString, QString> containerBindings;

    // ──── 格口显示名（持久化，格口号 → 自定义名称，不设则显示零填充序号）────
    QMap<QString, QString> gridNames;

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
    bool load();         // 加载配置文件，不存在则自动创建
    bool save();         // ★ 延迟保存：2秒内多次调用只写一次盘
    bool saveNow();      // ★ 立即保存（服务停止等关键时刻）

private:
    ConfigManager();
    AppConfig m_config;
    QTimer*  m_saveTimer  = nullptr;  // 延迟保存定时器（2秒）
    bool     m_saveDirty   = false;   // 脏标记
};
