#pragma once
// ============================================================================
// HttpServer.h — HP-Socket HTTP Server
//
// 设计模式：
//   HttpServer 继承 CHttpServerListener
//   m_pServer 在初始化列表: m_pServer(this — IHttpServerListener*)
//   Start: m_pServer->Start((TCHAR*)"0.0.0.0", port)
//   HasStarted() 检查状态
//   OnMessageComplete／OnBody／OnRequestLine 处理 HTTP
// ============================================================================

#include <QObject>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QByteArray>
#include <QTimer>
#include <atomic>
#include <mutex>
#include "HPSocket.h"
#include "TaskQueue.h"
#include "DoubleBuffer.h"
#include "WaveManager.h"
#include "ThreadPool.h"
#include "PlcManager.h"
#include "RfidPushClient.h"   // ★ 2026-09-04 RFID 推送 TCP 客户端（WCS 主动连接 RFID 服务端）

#include "SortingDatabase.h"
#include "EpcCache.h"
#include "define.h"

class HttpClient;  // 前向声明（避免循环依赖）

class ParseWorker;

// ★ 格口分拣记录（锁格/满箱时回传 WMS 用）
struct GridSortRecord
{
    QString inco;          // 运行时为主键码（EPC；链路以 EPC 为流水主键，字段名为历史遗留）
    QString sku;           // ★ 2026-09-06 SKU 编码（EPC→SKU 绑定查询结果，落格时固化；
                           //   WMS 报文中 sku 字段必须是它——不能是 EPC 或占位串）
    QString car;           // 小车号
    int     gridCount = 0; // 配货件数
    QString volu;          // 来源库位（=WMS下发 items[].sobi；满箱回传报文 head.fromLocation 来源）
    qint64  timeMs   = 0;  // 分拣时间
};

// 连接状态
struct ConnState
{
    QByteArray body;
    QString    method;
    QString    path;
    QString    queryString;
    QString    rawHost;         // ★ Host 头原始值（用于重建完整 URL: http://{Host}{path}?{query}）
};

class HttpServer : public QObject, public CHttpServerListener
{
    Q_OBJECT
public:
    explicit HttpServer(QObject* parent = nullptr);
    ~HttpServer();

    // ★ 2026-09-06 设备连接与任务接收解耦：
    //   设备层（PLC/RFID）随程序启动常驻；接收层（WMS HTTP 推送）由按钮控制
    bool startDevices();          // 设备层：ParseWorker+PLC(TCP/S7)+RFID 客户端（程序启动调用一次；失败仅提示不阻断）
    bool startReceive(int port);  // 接收层：HTTP 8191 开始接收 WMS 推送（按钮"开始接收任务"）
    void stopReceive();           // 接收层：停止接收（设备保持连接；按钮"结束任务"收尾后调用）
    void stopDevices();           // 设备层：PLC/S7/RFID/ParseWorker 全停（程序退出时调用）
    bool isReceiving() const { return m_receiving.load(); }   // 当前是否在接收任务
    bool isRunning() const { return m_pServer && m_pServer->HasStarted(); }   // 兼容：=接收中

    RfidPushClient* rfidPush() { return m_pRfidPush; }   // ★ 2026-09-06 设备状态查询（UI 用）

    TaskQueue*   taskQueue()   { return m_pQueue; }
    GridBuffer*  gridBuffer()  { return m_pBuffer; }
    WaveManager* waveManager() { return m_pWaveMgr; }
    PlcManager*  plcManager()  { return m_pPlcMgr; }
    
    SortingDatabase* sortingDb() { return m_pSortingDb; }  // ★ 分拣记录数据库

    // ★ API 路由设置（从 XML 配置加载后调用）
    void setApiInsertWaveInfo(const QString& path)     { m_apiInsertWaveInfo = path; }
    void setApiBindingLatticePort(const QString& path) { m_apiBindingLatticePort = path; }
    void setApiInsertWaveIn(const QString& path)       { m_apiInsertWaveIn = path; }
    void setHttpClient(HttpClient* client);              // ★ HTTP 客户端（用于 RFID 查询，连接 rfidBindingResult 信号）

    // 获取容器绑定快照（线程安全拷贝）
    QMap<QString, QString> getContainerBindings() const {
        std::lock_guard<std::mutex> lock(m_containerMutex);
        return m_containerBindings;
    }

    // 加载容器绑定（从配置文件恢复，线程安全）
    void loadContainerBindings(const QMap<QString, QString>& bindings) {
        std::lock_guard<std::mutex> lock(m_containerMutex);
        m_containerBindings = bindings;
    }

    // 设置期望绑定数量（波次下发时校验全部绑定用，默认66）
    void setExpectedBindCount(int count) { m_expectedBindCount = count; }
    int  expectedBindCount() const { return m_expectedBindCount; }

    // 检查所有格口是否已绑定容器（使用可配置的期望数量，线程安全）
    bool areAllBindingsComplete() const {
        std::lock_guard<std::mutex> lock(m_containerMutex);
        return (int)m_containerBindings.size() >= m_expectedBindCount;
    }

    // 获取已绑定数量（线程安全）
    int boundCount() const {
        std::lock_guard<std::mutex> lock(m_containerMutex);
        return m_containerBindings.size();
    }

    // ★ RFID查询：接收RFID服务返回的EPC→SKU映射结果
    QJsonObject handleRfidCarNumReport(const QJsonObject& body);  // ★ RFID 小车号推送（T-S4-03）

    // ──── SKU-EPC 绑定查询（WCS → RFID，逐条绑定）────
    void submitEpcBindingQueries(const QStringList& epcList);      // ★ 波次下发后提交 EPC 绑定查询
    void onRfidBindingResult(const QMap<QString, QString>& epcBarcodeMap);  // ★ RFID 绑定查询结果回调
    bool trySendToPlcForEpc(const QString& epc);                   // ★ 尝试发送单条 EPC 到 PLC（就绪检查），返回 true=已发送
    void scheduleSkuQueryRetry(const QStringList& epcList);          // ★ SKU 查询失败后延迟重试（最多重试 SKU_QUERY_MAX_RETRY 次）
    void scheduleNotReadyRetry(const QString& epc);                // ★ 未就绪(carNum未到)时延迟重试（最多重试 NOT_READY_RETRY_MAX 次）

    // ★ RFID查询：根据EPC获取对应的SKU/EPC（商品编码）（T-S4-04 EpcCache TTL缓存）
    QString getSkuByEpc(const QString& epc) const {
        if (m_pEpcCache) return m_pEpcCache->get(epc);
        return QString();
    }

    // ★ S5 满箱回传结果处理（H7 满箱同步到WMS，MainWindow 回调，必须 public）
    void onFullboxReplyFinished(const QString& msgId, bool success, const QString& body);

    // ──── S6 新增：完结回传（H8 波次完结通知WMS，T-S6-01~T-S6-05）────
    QJsonObject buildEndPayload(const QString& orderCode, int sumLocation); // ★ 构建完结报文（H8 波次完结通知WMS）
    // ★ 完结触发入口（T-S6-01/02）；2026-09-02 返回 bool：true=已进入完结回传流程
    //   （调用方应进入等待/兜底流程）；false=同步拒绝（无波次/状态不允许），调用方可立即收尾
    bool sendEnd();
    void sendEndToWms(const QString& msgId, const QJsonObject& payload); // ★ 发送完结回传到 WMS（H8 波次完结通知WMS，T-S6-03）
    void onEndReplyFinished(const QString& msgId, bool success, const QString& body); // ★ 完结回传结果处理（H8 波次完结通知WMS，T-S6-04）
    void pollOutboxEnd();                           // ★ Outbox 完结回传重试调度（H8 波次完结通知WMS，T-S6-03）

    // ──── S8 新增：对账（T-S8-01/02）────
    WaveReconciliation getReconciliation() const;          // ★ 波次对账（T-S8-01/02）

    // ★ 启动时从数据库恢复未完成波次（软件重启后继续处理同一批次数据）
    void restoreWaveFromDB();

    // ★ 构建波次明细记录（从 GridBuffer 读取全量 SKU→格口映射，供落库复用）
    QVector<ReturnWaveItemRecord> buildWaveItems(const QString& orderCode);

    // ★ 2026-09-06 波次记录/重传面板（★ 2026-09-06 升级为「波次数据记录」：全部已传输波次）────
    QVector<WaveRecordProgress> getAllWaves();                       // ★ 全部波次（含已完成/已取消）+ 进度
    QVector<ReturnWaveRecord> getUnfinishedWaves();                  // 所有未完成波次（旧面板兼容）
    QVector<OutboxRecord> getWaveFullboxOutbox(const QString& orderCode); // 某波次 H7 出站消息（含状态）
    QVector<OutboxRecord> getWaveEndOutbox(const QString& orderCode);     // 某波次 H8 出站消息（含状态）
    // ★ 2026-09-07 清空格口容器绑定（人工重置）：内存清空 + DB 归档留史 + 恢复禁用格口
    void clearAllGridBinds();
    void resendOutbox(const QString& orderCode, bool resendH7, bool resendH8); // 手动重传选中波次的 H7/H8
    void onOutboxResendReply(const QString& msgId, bool isH7, bool success);   // 手动重传结果（轻量，不动波次状态/绑定）
    // ★ 2026-09-07 手动满箱切换：UI 输入格口号 → 读取该格口当前记录+容器号，按 H7 满箱回传上传
    bool manualFullbox(const QString& grid);

    // ──── 上一波次任务恢复 ────
    // 选中波次的恢复摘要（orderCode/status/orderQty/sorted/exception/H7H8状态/更新时间）
    QJsonObject getUnfinishedWaveSummary(const QString& orderCode);
    // 恢复选中波次到内存（重建 GridBuffer + WaveManager 状态/进度），继续上次任务
    bool resumeUnfinishedWave(const QString& orderCode);
    // ★ 2026-09-06 新任务：当前波次进度/数据保留于 DB（可切换回来），内存清空回到空闲等待接收
    bool startNewWaveTask();

signals:
    void serverStarted(int port);
    void serverStopped();
    void waveReadyToReport(const QString& orderCode);
    void logMessage(const QString& msg, bool isError = false);
    void bindingUpdated();  // 容器绑定变更通知
    void gridLockReportReady(const QJsonObject& reportJson);  // ★ 锁格回传 WMS
    void waveCompleteReportReady(const QJsonObject& reportJson); // ★ 波次完成回传（异步入池构建后发出）
    void fullboxReportReady(const QJsonObject& payload, const QString& msgId); // ★ S5 满箱回传（H7 满箱同步到WMS，T-S5-04）
    void endReportReady(const QJsonObject& payload, const QString& msgId);     // ★ S6 完结回传（H8 波次完结通知WMS，T-S6-03）
    void endReportFinished();  // ★ H8完结回传处理完毕（成功/重试耗尽），通知MainWindow可以停止服务
    // ★ 2026-09-04 P0修复：波次明细异步落库完成（业务线程池执行完发回主线程，推进 BOUND）
    //   ok=true 推进 BOUND；ok=false 保持 CREATED（落库重试已耗尽，写异常表+UI告警）
    void wavePersistenceFinished(const QString& orderCode, bool ok, int skuCount);

    // ──── 未完成波次手动重传面板信号 ────
    // outboxResendReady: 请求发送一条历史出站报文（kind: "fullbox"|"end"），由 MainWindow 中继到 HttpClient
    void outboxResendReady(const QString& kind, const QJsonObject& payload, const QString& msgId);
    // outboxResendResult: 手动重传结果回执（供面板刷新状态）
    void outboxResendResult(const QString& orderCode, const QString& kind, const QString& msgId, bool success);
    // waveResumed: 上一波次恢复完成（UI 刷新波次面板）
    void waveResumed(const QString& orderCode, int status);

protected:
    // CHttpServerListener 回调
    EnHttpParseResult OnRequestLine(IHttpServer* pSender, CONNID dwConnID,
                                     LPCSTR lpszMethod, LPCSTR lpszUrl) override;
    EnHttpParseResult OnBody(IHttpServer* pSender, CONNID dwConnID,
                              const BYTE* pData, int iLength) override;
    EnHttpParseResult OnMessageComplete(IHttpServer* pSender, CONNID dwConnID) override;
    EnHttpParseResult OnHeadersComplete(IHttpServer* pSender, CONNID dwConnID) override { return HPR_OK; }
    EnHttpParseResult OnParseError(IHttpServer* pSender, CONNID dwConnID, int iErrorCode, LPCSTR lpszErrorDesc) override;

    // ITcpServerListener 回调（连接级日志）
    EnHandleResult OnAccept(ITcpServer* pSender, CONNID dwConnID, UINT_PTR soClient) override;
    EnHandleResult OnClose(ITcpServer* pSender, CONNID dwConnID,
                            EnSocketOperation enOperation, int iErrorCode) override;

private:
    void processRequest(IHttpServer* pSender, CONNID dwConnID, ConnState& state);
    QJsonObject handleBindingLatticePort(const QString& latticehole, const QString& boxcode); // 格口容器绑定
    QJsonObject handleCancelWave(const QJsonObject& req);                           // 退货任务取消
    QJsonObject validateInsertWaveInfo(const QJsonObject& root);                  // 波次下发参数校验（H4 WMS推送波次数据）
    void sendGridLockFeedback(const QString& grid);  // ★ 锁格时回传分拣明细到 WMS
    QJsonObject buildReportFromRecords(const QString& orderCode,
                                       const QMap<QString, QVector<GridSortRecord>>& records); // ★ 从记录副本构建 33.md JSON（线程安全）
    // ★ 2026-09-04 P0修复：波次明细异步落库完成回调（主线程）——推进 BOUND + 清理旧波次状态
    void onWavePersistenceFinished(const QString& orderCode, bool ok, int skuCount);

    // ──── S5 新增：满箱同步（H7 满箱同步到WMS，T-S5-01~T-S5-07）────
    QJsonObject buildFullboxPayload(const QString& orderCode, const QString& grid,
                                     const QString& boxCode, const QVector<GridSortRecord>& records); // ★ 构建满箱报文（H7 满箱同步到WMS）
    void sendFullbox(const QString& grid);                // ★ 满箱触发入口（T-S5-01）
    void sendFullboxToWms(const QString& msgId, const QJsonObject& payload); // ★ 发送满箱回传到 WMS（H7 满箱同步到WMS，T-S5-04）
    void pollOutboxFullbox();                              // ★ Outbox 重试调度（T-S5-04）
    // ──── 完结前兜底补发（2026-09-07）：H8 前把未满箱格口数据补发 H7 ────
    QString lookupGridBoxCode(const QString& grid);        // 格口当前容器号（内存→DB 兜底，sendFullbox/补发共用）
    int     flushUnreportedFullboxes(const QString& orderCode); // 补发内存中未满箱格口的 H7，返回补发格口数
    bool    sendFullboxForGrid(const QString& orderCode, const QString& grid,
                               QVector<GridSortRecord> records); // 单格口 H7 补发（完结前补发/手动满箱共用）
    void sendJsonResponse(IHttpServer* pSender, CONNID dwConnID,
                          const QJsonObject& json, USHORT status = 200);
    QJsonObject okResponse(const QString& msg = "");
    QJsonObject errResponse(const QString& msg, const QString& code = "500");
    void logHealthStatus();

    // HP-Socket 模式: m_pServer(this)
    CHttpServerPtr m_pServer;
    TaskQueue*     m_pQueue   = nullptr;
    GridBuffer*    m_pBuffer  = nullptr;
    WaveManager*   m_pWaveMgr = nullptr;
    ParseWorker*   m_pWorker  = nullptr;
    PlcManager*    m_pPlcMgr  = nullptr;
    RfidPushClient* m_pRfidPush = nullptr;  // ★ 2026-09-04 RFID 推送 TCP 客户端（主动连接 RFID 服务端）
    
    SortingDatabase* m_pSortingDb = nullptr;  // ★ 分拣记录本地数据库
    EpcCache*       m_pEpcCache  = nullptr;  // ★ S4 EPC短缓存（T-S4-04）
    HttpClient*     m_pHttpClient = nullptr;  // ★ HTTP 客户端（用于 RFID SKU-EPC 绑定查询）
    QSet<QString>  m_pendingSkuQuery;        // ★ 防重：已提交 SKU 查询的 EPC 集合（避免同一 EPC 重复查询）
    QMap<QString, int> m_skuQueryRetryCount; // ★ 重试计数：每个 EPC 的 SKU 查询重试次数（key=epc, value=已重试次数）
    QMap<QString, int> m_notReadyRetryCount; // ★ 未就绪重试计数：SKU已绑定但carNum未到时的重试次数
    QSet<QString>  m_sentEpcs;               // ★ 已发送PLC的EPC集合（防重复发送）

    // ──── API 路由（从 XML 配置读取，可动态修改）────
    QString m_apiInsertWaveInfo     = API_INSERT_WAVE_INFO;
    QString m_apiBindingLatticePort = API_BINDING_LATTICE_PORT;
    QString m_apiInsertWaveIn       = API_INSERT_WAVE_IN;

    // ──── 业务线程池 ────
    Hanchine::ThreadPool* m_pBusinessPool   = nullptr;
    Hanchine::ThreadPool* m_pPlcRecvPool    = nullptr;  // ★ PLC 反馈接收专用线程池（落格反馈→分拣标记→SQLite写入）
    

    QMap<CONNID, ConnState> m_connStates;
    QMap<CONNID, qint64>    m_connAcceptTime;
    std::mutex              m_connMutex;

    // ──── 连接统计 ────
    std::atomic<int64_t>    m_acceptCount{0};
    std::atomic<int64_t>    m_closeCount{0};
    std::atomic<int64_t>    m_requestCount{0};
    std::atomic<int>        m_activeConns{0};
    QTimer*                 m_healthTimer = nullptr;

    // ──── S5 新增：Outbox 满箱回传重试调度（H7 满箱同步到WMS）────
    QTimer*                 m_outboxFullboxTimer = nullptr;  // ★ 满箱回传出站重试调度器（H7 满箱同步到WMS，T-S5-04）
    QTimer*                 m_outboxEndTimer = nullptr;  // ★ S6 完结回传出站重试调度器（H8 波次完结通知WMS，T-S6-03）
    // ★ 2026-09-02 修复"结束任务卡死"：H8 完结回传会话兜底定时器（单次）
    //   点击"结束任务"后启动，到期无论回传是否完成都强制结束会话（onEndSessionTimeout），
    //   保证 endReportFinished 必然发出 → MainWindow 停止服务（不卡死、不退出程序）
    QTimer*                 m_endSessionTimer = nullptr;
    QString                 m_endSessionOrderCode;          // ★ 2026-09-06 H8 会话归属波次（切出后超时兜底只处理该波次）
    void                    onEndSessionTimeout();          // ★ H8 会话超时兜底（内部方法，定时器回调）
    void                    switchAwayCurrentWave();        // ★ 2026-09-06 挂起切出当前波次（清内存；状态/进度保留 DB）

    // ──── ★ 2026-09-07 波次待执行队列（当前波次执行中收到的新 H4 排队，结束后自动执行）────
    struct PendingWave { QString orderCode; QByteArray rawBody; QString fullUrl; qint64 recvTime = 0; };
    QVector<PendingWave>    m_pendingWaveQueue;             // FIFO（仅主线程读写）
    std::atomic<bool>       m_replayingPending{false};      // 队列重放标志（放行接收闸门）
    void                    maybeStartPendingWave();        // 空闲时取队首重放（由 ParseWorker 重新解析注册）
    void                    restoreBindsIfEmpty(const QString& orderCode);  // ★ 2026-09-07 无 active 绑定则沿用最近绑定

    // ★ 2026-09-02 防崩溃（停止与在途请求竞态）：服务停止标志
    //   stop() 最先置位；processRequest/sendJsonResponse 检测到后立即返回，
    //   防止线程池任务在 m_pServer.Reset() 后继续调用 HP-Socket SendResponse（空指针崩溃）
    std::atomic<bool>       m_stopping{false};
    // ★ 2026-09-06 设备/接收解耦：任务接收标志（HTTP 接收中=true；设备连接与其无关）
    std::atomic<bool>       m_receiving{false};

    // ──── PLC发送失败日志限流 ────
    QSet<QString>           m_warnedPlcFailCodes;
    std::mutex              m_warnMutex;

    // ──── 格口容器绑定 ────
    QMap<QString, QString>  m_containerBindings;  // latticehole(格口号) → boxcode(容器号)
    mutable std::mutex       m_containerMutex;     // 保护 m_containerBindings（const方法中需加锁）
    int                      m_expectedBindCount = DEFAULT_EXPECTED_BIND_COUNT;  // 期望绑定数量（波次下发时校验全部绑定用，默认66）

    // ──── 格口分拣记录（锁格回传用）────
    QMap<QString, QVector<GridSortRecord>> m_gridSortRecords;  // 格口号 → 分拣明细列表
    std::mutex m_gridRecordMutex;                               // 保护 m_gridSortRecords

    // ──── S7 格口分拣计数（T-S7-06：只记录落格已分拣件数，不做上限限制）────
    QMap<QString, int>      m_gridSortedCount;   // 格口号 → 已分拣件数
    std::mutex              m_gridCountMutex;     // 保护 m_gridSortedCount

    // ──── 回传耗时统计（H7/H8 网络请求慢排查）────
    QMap<QString, qint64>   m_msgSendTime;        // msgId → 发送时间戳（epoch ms）
    std::mutex              m_msgTimeMutex;        // 保护 m_msgSendTime

    // ★ 2026-09-04 P0修复：波次明细异步落库是否仍在进行
    //   提交异步任务时置 true，onWavePersistenceFinished 置 false；
    //   stop() 检测到 true 时同步补落库，保证停止/重启不丢数据
    std::atomic<bool>       m_wavePersistPending{false};
};
