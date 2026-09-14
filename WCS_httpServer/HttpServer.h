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
#include <QHash>         // ★ 2026-09-11 重扫重投：在途时刻/冷却/重发计数（QHash）
#include <QByteArray>
#include <QStringList>   // ★ 2026-09-08 失败重传下拉（失败报文 msgId 列表）
#include <QTimer>
#include <atomic>
#include <mutex>
#include <deque>
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
    QString boxcode;       // ★ 2026-09-09 需求6：落格时的容器号（物件行进中换绑，按落格时刻的新绑定记录）
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

    // 设置期望绑定数量（波次下发时校验全部绑定用，默认1）
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

    // ★ 2026-09-13 实时面板/异常弹窗只读访问（全部为无副作用查询）
    // 格口当前容器号（内存绑定表，含补零兜底；无绑定返回空串）
    QString containerForGrid(const QString& grid) const;
    // 某 EPC 是否仍在途（已下发 PLC、未收到落格反馈）——实时面板判定"待落格/超时未反馈"用
    bool isPlcSendInFlight(const QString& epc) const;
    // 某 EPC 本波次重投次数（0=仅首投；实时面板状态列显示"重投k次"用）
    int  rescanResendTimes(const QString& epc) const;
    // 本次运行累计 RFID 扫描次数（= RFID 推送 EPC 次数，重复 EPC 重复计数；重启归 0）
    quint64 rfidPushTotal() const { return m_rfidPushTotal.load(std::memory_order_relaxed); }
    // ★ 性能核验（需求：证明实时面板不影响分拣主流程）
    struct PerfSnapshot {
        int  rfidToPlcP50 = -1, rfidToPlcP95 = -1, rfidToPlcP99 = -1;  // RFID推送→PLC下发(ms)
        int  fbLatencyP95 = -1;      // 下发→落格反馈(ms)
        int  eventLagP95  = -1;      // RFID 帧解析→业务入口（主线程事件滞后, ms）
        int  samples      = 0;       // rfid→plc 采样数
    };
    PerfSnapshot perfSnapshot() const;

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
    // ──── RFID 推送效率统计（★ 2026-09-07：滑动 1 分钟窗口）────
    void recordRfidPush();                              // 记录一次有效 RFID 推送（含 EPC）
    int  rfidPushPerMinute() const;                     // 最近 1 分钟接收件数（滑动窗口）
    int  rfidThroughputPerHour() const { return rfidPushPerMinute() * 60; }  // 折算件/时
    int  peakPerMinuteToday() const;                    // 当日峰值（1 分钟窗口件数口径，UI ×60 显示件/时）
    void persistDailyPeak();                            // 当日峰值落库（跨日结转/周期/退出/波次结束调用）
    // 效率统计图数据（弹窗每~1秒拉取，内存由调用方复用缓冲）：
    //   lastMinutes: 最近 N 个整分钟桶（旧→新，含当前进行中的桶；不足 N 个前补 0）
    //   hourPeaks:   今日 0~23 时每小时峰值（1 分钟窗口件数口径；无数据=0）——折线图每小时记 1 个峰值点
    void efficiencySeries(int lastMinutes, QVector<int>* pLastMinute,
                          QVector<int>* pHourPeaks) const;

    // ──── 上一波次任务恢复 ────
    // 选中波次的恢复摘要（orderCode/status/orderQty/sorted/exception/H7H8状态/更新时间）
    QJsonObject getUnfinishedWaveSummary(const QString& orderCode);
    // 恢复选中波次到内存（重建 GridBuffer + WaveManager 状态/进度），继续上次任务
    bool resumeUnfinishedWave(const QString& orderCode);
    // ★ 2026-09-06 新任务：当前波次进度/数据保留于 DB（可切换回来），内存清空回到空闲等待接收
    bool startNewWaveTask();

    // ════════════════════════════════════════════════════════════════════
    // ★ 2026-09-08 波次待执行队列（UI「查看接收波次队列」只读访问）
    //   当前波次执行中收到的新 H4 在队列中排队，不覆盖当前波次数据；
    //   出队时机：开始接收时自动重放队首 / 人工「切换选中波次」接管 / 新任务不再消费队列
    // ════════════════════════════════════════════════════════════════════
    struct PendingWave {
        QString    orderCode;
        int        orderQty  = 0;      // ★ 波次总件数（队列弹窗展示用）
        QByteArray rawBody;            // H4 原始报文（重放时交 ParseWorker 重新解析）
        QString    fullUrl;
        qint64     recvTime  = 0;
    };
    // ★ 只读元数据（不含 rawBody）：避免把大报文整份拷贝给 UI（队列可能驻留数 MB 报文）
    struct PendingWaveInfo {
        QString orderCode;
        int     orderQty = 0;
        qint64  recvTime = 0;
    };
    QVector<PendingWaveInfo> pendingWaves() const
    {
        QVector<PendingWaveInfo> out;
        out.reserve(m_pendingWaveQueue.size());
        for (const PendingWave& pw : m_pendingWaveQueue)
        {
            PendingWaveInfo info;
            info.orderCode = pw.orderCode;
            info.orderQty  = pw.orderQty;
            info.recvTime  = pw.recvTime;
            out.append(info);
        }
        return out;
    }   // 仅主线程读写
    int pendingWaveCount() const { return m_pendingWaveQueue.size(); }

    // ════════════════════════════════════════════════════════════════════
    // ★ 2026-09-08 失败重传下拉数据源（H7 失败格口 / H8 失败波次）
    //   范围：outbox 表中 status='failed'（重试耗尽）与 'cancelled'（波次切出后取消重试）——
    //   两者都属于"待人工重传"，必须一并可见，否则会从界面消失
    // ════════════════════════════════════════════════════════════════════
    struct FailedFullboxItem {
        QString     orderCode;
        QString     grid;          // 格口号（内部号；老数据缺失时为 "?"）
        QStringList msgIds;        // 该(波次,格口)下全部失败报文
        int         failCount = 0;
        QString     lastTime;
        QString     status;        // failed / cancelled（含两者时为空）
    };
    struct FailedEndItem {
        QString     orderCode;
        QStringList msgIds;
        int         failCount = 0;
        QString     lastTime;
        QString     status;
    };
    QVector<FailedFullboxItem> getFailedFullboxItems(int limit = 200);  // 按(波次,格口)聚合
    QVector<FailedEndItem>     getFailedEndItems(int limit = 200);      // 按波次聚合
    // 精确重传：只重发失败/已取消报文，不改波次状态、不影响主流程
    bool resendFailedFullboxGrid(const QString& orderCode, const QString& grid);
    bool resendFailedEnd(const QString& orderCode);

    // ★ 2026-09-13 超计划预警（供波次面板「预警」数字与「查看」弹窗，MainWindow 直接调用）
    //   ★ 审核要点：本块必须位于 public 区且**在 signals: 之前**——
    //     moc 会把 signals: 之后直到下一个访问修饰符之前的内容都当作信号声明，
    //     因而"普通方法 + 嵌套 struct"若放在信号区里，会报
    //     `Not a signal or slot declaration` 导致构建失败（QtRunWork 返回 false）。
    // 取某格口当前绑定的容器号（无绑定返回空串；只查内存绑定表，供 UI 只读调用）
    QString currentBoxOfGrid(const QString& grid) const;
    // 只读快照：按「格口 + SKU」比较 计划件数 与 PLC 确认真正落入该格口的去重件数（跨容器累计）
    struct OverplanWarning
    {
        QString gridKey;      // 格口号（内部 3 位 key）
        QString sku;
        int     planQty   = 0;      // ★ 本格口计划件数（多格口时各格口不同）
        int     skuPlanQty = 0;     // 该 SKU 计划总数（各格口之和，供人工参考）
        int     landedQty = 0;
        int     overQty   = 0;      // 多余件数 = landedQty - planQty
        QStringList epcs;           // 多余件 EPC 清单（本格口计划件数之外的那些）
    };
    QVector<OverplanWarning> overplanWarnings() const;
    int overplanWarningCount() const;                   // 超计划条目数（UI 面板数字）

    // ★ 2026-09-14 同品多格口「按计划件数分配」支撑接口（选格由 PlcManager 回调本方法取依据）
    //   计划件数：H4 解析时写入 GridEntry::planQtyPerGrid（每格口各几件）
    //   已落格件数：本类按 PLC 反馈（status=1 落格成功）累计，同一 EPC 只计一次，跨换箱持续累计
    PlcPlanAllocInfo planAllocOf(const QString& sku);
    // 落格成功登记：PLC 反馈确认落入某格口某 SKU 后调用（供选格计数使用，同一 EPC 只计一次）
    void noteGridLanded(const QString& sku, const QString& gridKey, const QString& epc);
    // 清空按格口落格计数（H4 新波次重下发/波次清理时调用，与计划件数一同重置）
    void clearGridLandedCount();

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
    // ★ 2026-09-08 待执行波次队列变化（入队/出队/人工接管）→ UI 刷新波次列表与队列弹窗
    void pendingWavesChanged();
    // ★ 2026-09-08 失败重传记录变化（重试耗尽标记失败 / 手动重传成功后清除）→ UI 刷新两个失败下拉
    void outboxFailedChanged();

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

    // ──── ★ 2026-09-11 同波次「重扫重投」（在途语义）────
    //   语义变更：m_sentEpcs 由"本波次已发送过（永久去重）"改为"**在途**（已下发、尚未收到该件落格反馈）"。
    //   · 发送成功 → markEpcInFlight()（记在途 + 时刻）
    //   · PLC 落格反馈到达（主线程批处理入口，含异常分支）→ clearEpcInFlight()
    //   · 之后该 EPC 再被 RFID 读到（操作员拿起重新上料）→ 允许按原 SKU→格口映射重新下发同一格口
    //   以上集合均在主线程访问（RFID 推送/定时器/反馈批处理入口 lambda 均为主线程），无需加锁
    QSet<QString>  m_sentEpcs;               // ★ 在途 EPC 集合（已下发、未收到落格反馈）
    QHash<QString, qint64> m_sentAtMs;       // ★ 在途 EPC 下发时刻(ms)，用于在途超时判定
    QHash<QString, qint64> m_lastPlcSendMs;  // ★ 同一 EPC 最近一次下发时刻(ms)，用于重扫冷却
    QHash<QString, int>    m_rescanResendTimes;  // ★ 同一 EPC 本波次重发次数（上限保护）

    bool isEpcInFlight(const QString& epc);       // 是否在途（含 plcInFlightTimeoutMs 超时判定，会顺手清理超时项）
    // ★ 2026-09-13 只读版本：不做任何写操作（UI 每秒查询"待落格/超时未反馈"用，避免 UI 改业务状态）
    bool isEpcInFlightReadOnly(const QString& epc) const;
    void markEpcInFlight(const QString& epc);     // 标记在途（发送成功后调用）
    void clearEpcInFlight(const QString& epc);    // 落格反馈到达 → 退出在途（之后允许重扫重投）
    void clearAllEpcRuntimeState();               // 波次切换/新波次/完结清理：在途+时刻+重发计数

    // ──── ★ 2026-09-13 性能核验：固定槽环形采样（无动态分配、无锁、只进日志不影响业务）────
    //   用途：回答"实时面板是否影响分拣速度"——给出 RFID推送→PLC下发 / 下发→反馈 / 主线程事件滞后的分位值
    static const int PERF_RING_SLOTS = 256;
    int  m_perfRfidToPlc[PERF_RING_SLOTS] = {0};   // 采样：RFID推送→PLC下发(ms)
    int  m_perfFbLatency[PERF_RING_SLOTS] = {0};   // 采样：PLC下发→落格反馈(ms)
    int  m_perfEventLag[PERF_RING_SLOTS]  = {0};   // 采样：RFID帧解析→业务入口(ms)
    std::atomic<int> m_perfIdx{0};                 // 写入游标（取模覆盖，读侧自行快照）
    std::atomic<quint64> m_perfRfidCount{0};
    std::atomic<quint64> m_perfFbCount{0};
    void perfSampleRfidToPlc(int ms);
    void perfSampleFbLatency(int ms);
    void perfSampleEventLag(int ms);
    static int perfPercentile(const int* ring, int validCount, double pct);

    // ──── ★ 2026-09-08 RFID 发送"不阻塞"保障 ────
    //   RFID 挂起集合：EPC 已就绪（SKU+carNum 齐）但处于非执行态（未开工/完结中等）→ 挂起，
    //   恢复分拣后自动补发 PLC 指令。
    //   （满箱回传 H7 已解耦状态机：锁格即直接入 Outbox 队列异步发送，无需挂起）
    QSet<QString>           m_pendingRfidPlcEpcs;    // RFID 挂起 EPC（线程池/主线程共用，需锁）
    std::mutex              m_pendingRfidMutex;      // 保护 m_pendingRfidPlcEpcs
    void                    replayPendingRfidPlcEpcs();   // 状态回 SORTING：重放挂起的 RFID EPC→PLC 发送

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

    // ──── ★ 2026-09-07 波次待执行队列（当前波次执行中收到的新 H4 排队；结构体定义见 public 区）────
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
    int                      m_expectedBindCount = DEFAULT_EXPECTED_BIND_COUNT;  // 期望绑定数量（波次下发时校验全部绑定用，默认1）

    // ──── 格口分拣记录（锁格回传用）────
    QMap<QString, QVector<GridSortRecord>> m_gridSortRecords;  // 格口号 → 分拣明细列表
    std::mutex m_gridRecordMutex;                               // 保护 m_gridSortRecords

    // ──── S7 格口分拣计数（T-S7-06：只记录落格已分拣件数，不做上限限制）────
    QMap<QString, int>      m_gridSortedCount;   // 格口号 → 已分拣件数
    std::mutex              m_gridCountMutex;     // 保护 m_gridSortedCount

    // ──── ★ 2026-09-13 计划数封顶 + 超计划件改投异常口（按客户四轮确认的口径）────
    //   目标不变量：对任一 (波次, SKU, 格口)：箱内该 SKU 件数 ≤ 计划件数；
    //               多余的 EPC 一律改投物理异常口，且不占箱内额度。
    //   额度消耗源 = **PLC 确认真正落入该格口的去重 EPC 数**（不是下发数、不是放置数）——
    //     因此：下发失败 / 落进异常口 都不白占额度，"缺件由人工重投异常件补上"才成立。
    //   口径：key = 格口号 + "\n" + SKU（★ 按格口而非容器，跨容器累计，换箱不重置额度）
    //   五条规则：
    //     ① 已达计划 → 该 SKU 后续件一律投异常口（无论重投多少次）
    //     ② 未达计划 → 允许投期望格口（含此前判去异常口的件被人工重投回来补缺口）
    //     ③ 正确落入该格口的 EPC 被拿出重投 → 仍允许回该格口，且不重复计数
    //     ④ 同一 EPC 只计 1 次（防重复反馈刷高额度）
    //     ⑤ 实际落格数 > 计划数（同时两件在线等）→ 登记"超计划预警"，且★不进入上传报文（乙方案）：
    //        H7 报文中该 SKU 的 qty 被裁剪到计划件数（先按 EPC 去重、再封顶），
    //        避免 WMS 因"超计划无法分配"整条驳回而牵连同批其它正常件
    QMap<QString, QSet<QString>> m_boxLandedEpcs;       // key → 真实落入该格口该 SKU 的 EPC 集合
    QSet<QString>                m_epcBoundException;   // 仅日志/统计：曾被判去异常口的 EPC（非黑名单）
    mutable std::mutex           m_boxLandedMutex;      // 保护上面两个容器（const 查询方法中需加锁，故 mutable）
    void clearBoxLandedCount();                         // 波次切换/完结/取消时清空
    // 判定：本件是否允许投期望格口（false=应改投异常口）；counted=该 EPC 此前已计入（重投回箱）
    bool allowIntoPlanGrid(const QString& gridKey, const QString& sku, const QString& epc,
                           int planQty, bool& counted) const;
    // 计数：落格反馈确认成功后调用；返回 true = 本次落格使该格口该 SKU 超出计划（登记预警）
    bool noteLandedIntoPlanGrid(const QString& gridKey, const QString& sku, const QString& epc,
                                int planQty, int& landedNow);
    // 已真实落入该格口该 SKU 的件数（去重 EPC）
    int landedCountOf(const QString& gridKey, const QString& sku) const;
    // ★ 2026-09-14 判定某格口是否配置的物理异常口（超计划件改投落点）
    bool isExceptionGridKey(const QString& gridKey) const;
    // ★ 2026-09-14 取「该 SKU 在该格口的计划件数」（多格口按格口取；无分格口计划时退回总数）
    int planQtyOfGrid(const QString& sku, const QString& gridKey) const;
    // ★ 乙方案：H7 报文裁剪——把各 SKU 行的 qty 裁剪到"计划件数"（先按 EPC 去重、再封顶）
    //   返回被裁掉的多余件总数（0=未裁剪）；明细写入 trimLog 供日志留痕
    int clampFullboxQtyToPlan(const QString& gridKey, QJsonArray& detailList, QStringList& trimLog) const;

    // ──── ★ 2026-09-14 同品多格口「按计划件数分配」计数（选格依据）────
    //   计划数来自 H4（GridEntry::planQtyPerGrid：该 SKU 在某格口计划几件）；
    //   本表记录「该 SKU 已在某格口落了几件」——★ 以 PLC 反馈落格成功为准（status=1），
    //   同一 EPC 只计一次，跨换箱持续累计，仅在 H4 新波次重下发/波次清理时清零。
    //   选格时取「已落格数 < 计划件数」的首个计划格口；全部满额 → 超计划件按策略处置。
    QMap<QString, QMap<QString, QSet<QString>>> m_gridLandedNum;  // SKU → (格口号 → 已落格 EPC 集合)
    mutable std::mutex                           m_gridLandedMutex;

    // ──── RFID 推送吞吐/峰值统计（★ 2026-09-07 效率与峰值显示）────
    //   滑动 1 分钟窗口（实时"效率"）用 deque；分桶（每分钟）与当日峰值用于
    //   峰值显示与效率统计图；跨日自动结转并把前一天最终峰值落库
    mutable std::deque<qint64> m_rfidPushTimes;       // 最近60秒有效推送时间戳(ms)
    mutable QMap<qint64, int>  m_rfidMinuteCount;     // 分桶：epochMin(epochMs/60000) → 该分钟推送件数（仅保留当日）
    mutable QString            m_peakDate;            // 当前统计日期 yyyy-MM-dd（跨日自动重置并落库前一日）
    mutable int                m_peakPerMinuteToday = 0; // 当日峰值（1 分钟窗口件数口径）
    mutable std::mutex         m_rfidPushMutex;       // 保护以上统计字段
    // ★ 2026-09-13 波次面板「RFID扫描次数」：本次运行累计 RFID 推送 EPC 次数
    //   口径：每推送一个 EPC 记 1 次；重复 EPC 重复计数；空 EPC/NOREAD 不计；跨波次不清零、重启归 0
    std::atomic<quint64>       m_rfidPushTotal{0};

    // ──── 回传耗时统计（H7/H8 网络请求慢排查）────
    QMap<QString, qint64>   m_msgSendTime;        // msgId → 发送时间戳（epoch ms）
    std::mutex              m_msgTimeMutex;        // 保护 m_msgSendTime

    // ★ 2026-09-04 P0修复：波次明细异步落库是否仍在进行
    //   提交异步任务时置 true，onWavePersistenceFinished 置 false；
    //   stop() 检测到 true 时同步补落库，保证停止/重启不丢数据
    std::atomic<bool>       m_wavePersistPending{false};
};
