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
#include "define.h"

class ParseWorker;

// 连接状态
struct ConnState
{
    QByteArray body;
    QString    method;
    QString    path;
    QString    queryString;
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

signals:
    void serverStarted(int port);
    void serverStopped();
    void waveReadyToReport(const QString& orderCode);
    void logMessage(const QString& msg, bool isError = false);  // 通知UI刷新日志

protected:
    // CHttpServerListener 回调（只重写需要的）
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
    QJsonObject handleQuery(const QString& code);
    QJsonObject handleMarkSorted(const QJsonObject& req);
    QJsonObject handleMarkException(const QJsonObject& req);
    QJsonObject handleWaveStatus();
    QJsonObject handleSendToPlc(const QJsonObject& req);  // PLC发送接口
    QJsonObject handlePlcStatus();                         // PLC状态查询
    void sendJsonResponse(IHttpServer* pSender, CONNID dwConnID,
                          const QJsonObject& json, USHORT status = 200);
    QJsonObject okResponse(const QString& data = "accepted");
    QJsonObject errResponse(const QString& msg, int code = -1);
    void logHealthStatus();   // 周期性健康检查

    // HP-Socket 模式: m_pServer(this)
    CHttpServerPtr m_pServer;
    TaskQueue*     m_pQueue   = nullptr;
    GridBuffer*    m_pBuffer  = nullptr;
    WaveManager*   m_pWaveMgr = nullptr;
    ParseWorker*   m_pWorker  = nullptr;
    PlcManager*    m_pPlcMgr  = nullptr;  // PLC直连管理器

    // ──── 业务线程池（参考WCSApp架构）────
    Hanchine::ThreadPool* m_pBusinessPool = nullptr;  // 查询/分拣/异常处理

    QMap<CONNID, ConnState> m_connStates;
    QMap<CONNID, qint64>    m_connAcceptTime;   // 连接接受时间戳(ms)
    std::mutex              m_connMutex;

    // ──── 连接统计（用于诊断连接池耗尽问题）────
    std::atomic<int64_t>    m_acceptCount{0};    // 累计接受连接数
    std::atomic<int64_t>    m_closeCount{0};     // 累计关闭连接数
    std::atomic<int64_t>    m_requestCount{0};   // 累计请求数
    std::atomic<int>        m_activeConns{0};     // 当前活跃连接数
    QTimer*                 m_healthTimer = nullptr;  // 健康检查定时器

    // ──── PLC发送失败日志限流 ────
    QSet<QString>           m_warnedPlcFailCodes;  // 已警告过的条码（每波次重置）
    std::mutex              m_warnMutex;           // 保护m_warnedPlcFailCodes
};
