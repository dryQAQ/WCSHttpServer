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
#include "CameraManager.h"
#include "SortingDatabase.h"
#include "define.h"

class ParseWorker;

// ★ 格口分拣记录（锁格时回传 WMS 用）
struct GridSortRecord
{
    QString inco;          // 条码
    QString car;           // 小车号
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
    CameraManager* cameraManager() { return m_pCameraMgr; }
    SortingDatabase* sortingDb() { return m_pSortingDb; }  // ★ 分拣记录数据库

    // ★ API 路由设置（从 XML 配置加载后调用）
    void setApiInsertWaveInfo(const QString& path)     { m_apiInsertWaveInfo = path; }
    void setApiBindingLatticePort(const QString& path) { m_apiBindingLatticePort = path; }
    void setApiInsertWaveIn(const QString& path)       { m_apiInsertWaveIn = path; }

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

    // 检查所有格口是否已绑定容器（线程安全）
    bool areAllBindingsComplete() const {
        std::lock_guard<std::mutex> lock(m_containerMutex);
        return (int)m_containerBindings.size() >= BINDING_SLOT_COUNT;
    }

    // 获取已绑定数量（线程安全）
    int boundCount() const {
        std::lock_guard<std::mutex> lock(m_containerMutex);
        return m_containerBindings.size();
    }

    // ★ RFID查询：接收RFID服务返回的EPC→SKU映射结果
    void onRfidQueryResult(const QJsonObject& result, const QString& context);

    // ★ RFID查询：根据EPC获取对应的SKU/条码
    QString getSkuByEpc(const QString& epc) const {
        std::lock_guard<std::mutex> lock(m_epcSkuMutex);
        return m_epcSkuMap.value(epc);
    }

    // ★ RFID查询：获取EPC→SKU映射快照
    QMap<QString, QString> getEpcSkuMap() const {
        std::lock_guard<std::mutex> lock(m_epcSkuMutex);
        return m_epcSkuMap;
    }

signals:
    void serverStarted(int port);
    void serverStopped();
    void waveReadyToReport(const QString& orderCode);
    void logMessage(const QString& msg, bool isError = false);
    void bindingUpdated();  // 容器绑定变更通知
    void gridLockReportReady(const QJsonObject& reportJson);  // ★ 锁格回传 WMS
    void waveCompleteReportReady(const QJsonObject& reportJson); // ★ 波次完成回传（异步入池构建后发出）
    void rfidQueryRequested(const QJsonArray& epcList, const QString& context); // ★ 请求RFID查询EPC→SKU

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
    void sendGridLockFeedback(const QString& grid);  // ★ 锁格时回传分拣明细到 WMS
    QJsonObject buildReportFromRecords(const QString& orderCode,
                                       const QMap<QString, QVector<GridSortRecord>>& records); // ★ 从记录副本构建 33.md JSON（线程安全）
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
    CameraManager*  m_pCameraMgr = nullptr;  // ★ 相机通信管理器
    SortingDatabase* m_pSortingDb = nullptr;  // ★ 分拣记录本地数据库

    // ──── API 路由（从 XML 配置读取，可动态修改）────
    QString m_apiInsertWaveInfo     = API_INSERT_WAVE_INFO;
    QString m_apiBindingLatticePort = API_BINDING_LATTICE_PORT;
    QString m_apiInsertWaveIn       = API_INSERT_WAVE_IN;

    // ──── 业务线程池 ────
    Hanchine::ThreadPool* m_pBusinessPool   = nullptr;
    Hanchine::ThreadPool* m_pPlcRecvPool    = nullptr;  // ★ PLC 反馈接收专用线程池（落格反馈→分拣标记→SQLite写入）
    Hanchine::ThreadPool* m_pCameraProcPool = nullptr;  // ★ 相机数据处理专用线程池（相机扫描→格口查询→PLC发送）

    QMap<CONNID, ConnState> m_connStates;
    QMap<CONNID, qint64>    m_connAcceptTime;
    std::mutex              m_connMutex;

    // ──── 连接统计 ────
    std::atomic<int64_t>    m_acceptCount{0};
    std::atomic<int64_t>    m_closeCount{0};
    std::atomic<int64_t>    m_requestCount{0};
    std::atomic<int>        m_activeConns{0};
    QTimer*                 m_healthTimer = nullptr;

    // ──── PLC发送失败日志限流 ────
    QSet<QString>           m_warnedPlcFailCodes;
    std::mutex              m_warnMutex;

    // ──── 格口容器绑定 ────
    QMap<QString, QString>  m_containerBindings;  // latticehole(格口号) → boxcode(容器号)
    mutable std::mutex       m_containerMutex;     // 保护 m_containerBindings（const方法中需加锁）

    // ──── 格口分拣记录（锁格回传用）────
    QMap<QString, QVector<GridSortRecord>> m_gridSortRecords;  // 格口号 → 分拣明细列表
    std::mutex m_gridRecordMutex;                               // 保护 m_gridSortRecords

    // ──── EPC→SKU 映射（RFID查询结果）────
    QMap<QString, QString>  m_epcSkuMap;      // EPC → 条码/SKU 映射
    mutable std::mutex       m_epcSkuMutex;    // 保护 m_epcSkuMap（const方法中需加锁）
};
