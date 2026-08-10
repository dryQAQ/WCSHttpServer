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

    // ──── RFID 查询配置（WCS → RFID，SKU-EPC 绑定查询）────
    QString rfidQueryUrl      = RFID_QUERY_URL;        // RFID SKU-EPC 绑定查询 URL（查询 EPC→barcode 映射）

    // ──── WMS 业务参数 ────
    QString warehouseCode    = WMS_WAREHOUSE_CODE;    // 仓库编码
    QString goodsOwner       = WMS_GOODS_OWNER;       // 货主编码

    // ──── 波次配置 ────
    int     waveTimeoutMin    = WAVE_TIMEOUT_MIN_DEFAULT; // 波次超时(分钟, 0=不超时)
    int     maxRetryCount     = WAVE_MAX_RETRY;           // 异常 SKU 最大重试次数
    int     expectedBindCount = DEFAULT_EXPECTED_BIND_COUNT; // 期望绑定数量（波次下发时校验全部绑定用，默认66）

    // ──── 网络超时 ────
    int     httpTimeoutMs         = HTTP_TIMEOUT_MS;          // HTTP 请求超时(ms)
    int     waveCompleteTimeoutMs = WAVE_COMPLETE_TIMEOUT_MS; // 波次完成回传超时(ms)

    // ──── PLC 通信 ────
    int     plcListenPort     = PLC_LISTEN_PORT;        // PLC TCP 监听端口（默认 8192）
    QString plcS7Ip           = PLC_S7_IP;              // S7 PLC IP 地址
    int     plcS7Rack         = PLC_S7_RACK;            // S7 机架号
    int     plcS7Slot         = PLC_S7_SLOT;            // S7 槽位号

    // ──── 线程池 ────
    int     businessPoolSize  = BUSINESS_POOL_SIZE;     // 业务线程池大小
    int     plcSendPoolSize   = PLC_SEND_POOL_SIZE;     // PLC 发送线程池大小
    int     plcRecvPoolSize   = PLC_RECV_POOL_SIZE;     // PLC 反馈接收专用线程池大小
    

    // ──── 配置文件版本 ────
    int     configVersion     = CONFIG_VERSION;           // 配置文件版本号（与软件版本匹配检查）

    // ──── 满箱回传配置（H7 满箱同步到WMS）────
    QString fullboxFromLocationSource = FULLBOX_FROM_LOCATION_SOURCE;  // fromLocation 取值来源（config/volu）
    QString fullboxDefaultFromLocation = FULLBOX_DEFAULT_FROM_LOCATION; // 来源库位默认值（config模式时使用）

    // ──── S7 分拣增强配置（修改后需重启服务）────
    bool    sortingEpcDedup     = SORTING_EPC_DEDUP;          // EPC 任务内防重（true/false）
    QString sortingGridCapPolicy = SORTING_GRID_CAP_POLICY;   // 格口上限策略（reject/exception/allow）
    QString sortingOrderQtyValidate = SORTING_ORDERQTY_VALIDATE; // orderQty校验模式（strict/loose）
    QString sortingConflictPolicy = SORTING_CONFLICT_POLICY;   // 冲突策略（STRICT_EXCEPTION/LOOSE_FIRST）
    bool    sortingAllowOverrecv = SORTING_ALLOW_OVERRECV;     // 允许超收（true/false）

    // ──── API 路由（WMS 调用 WCS 的接口路径，可配置以适应 WMS 路径变更）────
    QString apiInsertWaveInfo     = API_INSERT_WAVE_INFO;      // ① 波次下发（H4 WMS推送波次数据）(POST)
    QString apiBindingLatticePort = API_BINDING_LATTICE_PORT;  // ② 容器绑定（H6 格口容器绑定）(POST)
    QString apiInsertWaveIn       = API_INSERT_WAVE_IN;        // ③ 波次取消（H5 退货任务取消）(POST)
    QString apiRfidCarNumReport   = API_RFID_CAR_NUM_REPORT;   // ④ RFID 小车号推送（RFID 主动推送小车号）(POST)

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
