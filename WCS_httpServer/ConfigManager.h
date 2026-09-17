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
    QString feedbackUrl      = WMS_FEEDBACK_URL;      // 正式环境满箱回传 base URL（H7，不含 appkey/method 参数）
    QString feedbackTestUrl  = WMS_FEEDBACK_URL_TEST; // 测试环境满箱回传 base URL（H7）
    QString feedbackEndUrl   = WMS_FEEDBACK_END_URL;  // 正式环境完结回传 base URL（H8）
    QString feedbackEndTestUrl = WMS_FEEDBACK_END_URL_TEST; // 测试环境完结回传 base URL（H8）
    // ★ 2026-09-06：WMS 网关 method 参数（发送时拼成 ?appkey=xxx&method=yyy，独立配置便于调整）
    QString feedbackMethod    = WMS_METHOD_FULLBOX;   // 满箱/锁格/波次完成等回传 method（默认 gwisSubProductClassifyOrder）
    QString feedbackEndMethod = WMS_METHOD_END;       // 完结回传(H8) method（默认 gwisSubProductClassifyEndOrder）
    QString appkey            = WMS_APPKEY;            // 正式环境 AppKey（HTTP Header + URL 参数 appkey 同值）
    QString appkeyTest        = WMS_APPKEY_TEST;       // 测试环境 AppKey
    bool    useTestEnv        = true;                  // true=使用测试环境

    // ──── RFID 查询配置（WCS → RFID，SKU-EPC 绑定查询）────
    QString rfidQueryUrl      = RFID_QUERY_URL;        // RFID SKU-EPC 绑定查询 URL（查询 EPC→barcode 映射）
    QString rfidAppkey        = RFID_APPKEY;           // ★ 2026-09-05：RFID 查询 Authorization 头完整值（如 "APP_KEYS xxx"，整串由 XML 提供；空=不发送）
    // ★ 2026-09-04：RFID 推送服务端（WCS 作为 TCP 客户端主动连接，接收 EPC+carNum JSON）
    QString rfidPushServerIp  = RFID_SERVER_IP;        // RFID 服务端 IP
    int     rfidPushServerPort = RFID_SERVER_PORT;     // RFID 服务端端口
    // ★ 2026-09-05：是否向 RFID 服务端发送心跳包（1=发送 0=不发送）
    int     rfidHeartbeatEnable = 1;                   // 心跳开关（默认发送）
    int     rfidHeartbeatIntervalMs = RFID_HEARTBEAT_INTERVAL_MS;  // 心跳间隔(毫秒, 默认2000=2s)
    // ★ 2026-09-15：EPC 码识别长度（字符数，默认 24）：识别『A + (长度-1) 位数字』形态
    //   < 2 = 不启用识别（整串原样使用，回退改造前行为）；非法值保持默认并在 run.log 告警
    //   改配置点「保存并生效」即热生效（无需重启）
    int     rfidEpcTruncateLen = RFID_EPC_TRUNCATE_LEN;

    // ──── WMS 业务参数 ────
    QString warehouseCode    = WMS_WAREHOUSE_CODE;    // 仓库编码
    QString goodsOwner       = WMS_GOODS_OWNER;       // 货主编码

    // ──── 波次配置 ────
    int     waveTimeoutMin    = WAVE_TIMEOUT_MIN_DEFAULT; // 波次超时(分钟, 0=不超时)
    int     maxRetryCount     = WAVE_MAX_RETRY;           // 异常 SKU 最大重试次数
    int     expectedBindCount = DEFAULT_EXPECTED_BIND_COUNT; // 期望绑定数量（波次下发时校验全部绑定用，默认1）

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
    QString fullboxDefaultTargetLocation = FULLBOX_DEFAULT_TARGET_LOCATION; // 目标库位默认值（无容器号时兜底）

    // ──── WMS 格口编码配置（2026-09-07：对外格口号 = gridCodePrefix + gridCodeWidth 位补零，如 5→"22005"）────
    //   作用域：H7 满箱回传 detailList[].num 出参用该编码；H4 下发 gridNum / H6 绑定 latticehole 入参兼容剥离该前缀。
    //   前缀留空 = 关闭转换（出参仅补零、入参不剥离），可回退现网行为
    QString gridCodePrefix   = WMS_GRID_CODE_PREFIX_DEFAULT;   // 格口编码前缀（默认 "22"，例 5→22005）
    int     gridCodeWidth    = WMS_GRID_CODE_WIDTH_DEFAULT;    // 格口号补零宽度（默认 3，内部/PLC 3 位一致）

    // ──── S7 分拣增强配置（修改后需重启服务）────
    bool    sortingEpcDedup     = SORTING_EPC_DEDUP;          // EPC 任务内防重（true/false）
    // ★ 2026-09-11 同波次重扫重投（拿起已落格的件重新上料 → 仍按原格口下发）
    bool    rescanResendEnabled = RESCAN_RESEND_ENABLED;      // 重扫重投开关
    int     rescanResendCooldownMs = RESCAN_RESEND_COOLDOWN_MS; // 同一 EPC 两次下发最小间隔(ms)
    int     rescanResendMaxTimes   = RESCAN_RESEND_MAX_TIMES;   // 同一 EPC 每波次最多重发次数
    int     plcInFlightTimeoutMs   = PLC_INFLIGHT_TIMEOUT_MS;   // PLC 在途超时(ms)，超时后允许重投
    QString sortingGridCapPolicy = SORTING_GRID_CAP_POLICY;   // 【已废弃】格口上限策略（历史遗留，仅读写不判）
    // ★ 2026-09-13 计划数封顶：按「格口+SKU」计划件数封顶，超计划件改投物理异常口
    QString sortingOverplanPolicy = SORTING_OVERPLAN_POLICY;  // exception=改投异常口 / block=拒发并标注 / allow=仅标注
    QString exceptionGrid         = DEFAULT_EXCEPTION_GRID;   // 物理异常格口号（3位，空/"0"=未配置→自动降级为 block）
    QString sortingOrderQtyValidate = SORTING_ORDERQTY_VALIDATE; // orderQty校验模式（strict/loose）
    QString sortingConflictPolicy = SORTING_CONFLICT_POLICY;   // 冲突策略（STRICT_EXCEPTION/LOOSE_FIRST）
    bool    sortingAllowOverrecv = SORTING_ALLOW_OVERRECV;     // 允许超收（true/false）

    // ──── ★ 2026-09-14 计划分配表（落格结构优化）配置 ────
    //   客户口径：一个 SKU 可同时计划到「正常分拣格口」与「发货格口」，
    //   每个格口各有一份数量，落格按各格口数量分；已落+在途 ≥ 计划 → 改投异常口。
    bool    allocEnabled            = ALLOC_ENABLED;             // 总开关（false=回退改造前"恒取首个格口"行为）
    bool    allocRequirePlanValid   = ALLOC_REQUIRE_PLAN_VALID;  // true=Σ每格口计划≠orderQty 时拒绝开工
    bool    allocGapMoveOnDisabled  = ALLOC_GAP_MOVE_ON_DISABLED;// 计划格口禁用时把未完成件转给同 SKU 其它计划格口
    bool    allocGapMoveOnLocked    = ALLOC_GAP_MOVE_ON_LOCKED;  // 物理锁格时同样转移
    int     allocMaxInflight        = ALLOC_MAX_INFLIGHT;        // 在途认领上限（超出强制清扫+留痕）
    int     allocAuditIntervalMs    = ALLOC_AUDIT_INTERVAL_MS;   // 不变量巡检+认领清扫周期(ms)
    int     allocClaimTimeoutMs     = ALLOC_CLAIM_TIMEOUT_MS;    // 认领超时(ms)
    int     allocPlanLogTail        = ALLOC_PLAN_LOG_TAIL;       // 选格成功日志前 N 条逐条
    int     allocPlanLogStep        = ALLOC_PLAN_LOG_STEP;       // 之后每 N 条一条

    // ──── ★ 2026-09-17 界面页显隐开关（现场要求：可隐藏「实时面板」「计划分配表」）────
    //   ★★ 只在**启动时读取一次**（改 XML 需重启才生效 —— 现场要求"不立刻生效"）★★
    //   · false（默认）= 该页不加入标签页；实时面板连表格都不创建 → 隐藏期间零开销
    //   · true  = 与改造前一致，页面正常显示
    bool    showLivePage      = UI_SHOW_LIVE_PAGE_DEFAULT;        // 实时面板页（落格反馈数据）
    bool    showPlanAllocPage = UI_SHOW_PLAN_ALLOC_PAGE_DEFAULT;  // 计划分配表页

    // ──── API 路由（WMS 调用 WCS 的接口路径，可配置以适应 WMS 路径变更）────
    QString apiInsertWaveInfo     = API_INSERT_WAVE_INFO;      // ① 波次下发（H4 WMS推送波次数据）(POST)
    QString apiBindingLatticePort = API_BINDING_LATTICE_PORT;  // ② 容器绑定（H6 格口容器绑定）(POST)
    QString apiInsertWaveIn       = API_INSERT_WAVE_IN;        // ③ 波次取消（H5 退货任务取消）(POST)

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

    // 获取当前生效的完结回传 URL（H8，根据 useTestEnv 切换测试/正式环境）
    QString activeEndFeedbackUrl() const
    {
        return useTestEnv ? feedbackEndTestUrl : feedbackEndUrl;
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
    QByteArray m_seenHash;            // ★ 2026-09-07 最近一次"程序读写后"的配置文件内容哈希
                                      //   保存前比对：外部(手工/编辑器)改动过则先同步内存再写，绝不覆盖手改
};
