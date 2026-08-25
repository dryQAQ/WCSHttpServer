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

#include "SortingDatabase.h"
#include "EpcCache.h"
#include "define.h"

class HttpClient;  // 前向声明（避免循环依赖）

class ParseWorker;

// ★ 格口分拣记录（锁格时回传 WMS 用）
struct GridSortRecord
{
    QString inco;          // SKU编码（客户确认 2026-08-14，H4 下发 inco 字段为 SKU 编码）
    QString car;           // 小车号（TODO: 应由RFID提供，客户尚未提供 2026-08-04）
    int     gridCount = 0; // 配货件数
    QString volu;          // 来源库位
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

    bool start(int port = 8191);
    void stop();
    bool isRunning() const { return m_pServer && m_pServer->HasStarted(); }

    TaskQueue*   taskQueue()   { return m_pQueue; }
    GridBuffer*  gridBuffer()  { return m_pBuffer; }
    WaveManager* waveManager() { return m_pWaveMgr; }
    PlcManager*  plcManager()  { return m_pPlcMgr; }
    
    SortingDatabase* sortingDb() { return m_pSortingDb; }  // ★ 分拣记录数据库

    // ★ API 路由设置（从 XML 配置加载后调用）
    void setApiInsertWaveInfo(const QString& path)     { m_apiInsertWaveInfo = path; }
    void setApiBindingLatticePort(const QString& path) { m_apiBindingLatticePort = path; }
    void setApiInsertWaveIn(const QString& path)       { m_apiInsertWaveIn = path; }
    void setApiRfidCarNumReport(const QString& path)   { m_apiRfidCarNumReport = path; }  // ★ RFID 小车号推送
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
    void sendEnd();                                 // ★ 完结触发入口（T-S6-01/02）
    void sendEndToWms(const QString& msgId, const QJsonObject& payload); // ★ 发送完结回传到 WMS（H8 波次完结通知WMS，T-S6-03）
    void onEndReplyFinished(const QString& msgId, bool success, const QString& body); // ★ 完结回传结果处理（H8 波次完结通知WMS，T-S6-04）
    void pollOutboxEnd();                           // ★ Outbox 完结回传重试调度（H8 波次完结通知WMS，T-S6-03）

    // ──── S8 新增：对账（T-S8-01/02）────
    WaveReconciliation getReconciliation() const;          // ★ 波次对账（T-S8-01/02）

    // ★ 启动时从数据库恢复未完成波次（软件重启后继续处理同一批次数据）
    void restoreWaveFromDB();

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

    // ──── S5 新增：满箱同步（H7 满箱同步到WMS，T-S5-01~T-S5-07）────
    QJsonObject buildFullboxPayload(const QString& orderCode, const QString& grid,
                                     const QString& boxCode, const QVector<GridSortRecord>& records); // ★ 构建满箱报文（H7 满箱同步到WMS）
    void sendFullbox(const QString& grid);                // ★ 满箱触发入口（T-S5-01）
    void sendFullboxToWms(const QString& msgId, const QJsonObject& payload); // ★ 发送满箱回传到 WMS（H7 满箱同步到WMS，T-S5-04）
    void pollOutboxFullbox();                              // ★ Outbox 重试调度（T-S5-04）
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
    QString m_apiRfidCarNumReport   = API_RFID_CAR_NUM_REPORT;  // ★ RFID 小车号推送

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

    // ──── S7 格口分拣计数（T-S7-06 格口上限检查）────
    QMap<QString, int>      m_gridSortedCount;   // 格口号 → 已分拣件数
    std::mutex              m_gridCountMutex;     // 保护 m_gridSortedCount

    // ──── 回传耗时统计（H7/H8 网络请求慢排查）────
    QMap<QString, qint64>   m_msgSendTime;        // msgId → 发送时间戳（epoch ms）
    std::mutex              m_msgTimeMutex;        // 保护 m_msgSendTime
};
