#pragma once
// ============================================================================
// HttpServer.h — HP-Socket HTTP Server 封装
//
// 同时处理两类请求:
//   外部: POST /api/.../InsertWaveInfo     WMS推送波次
//   内部: GET  /api/query                  WCSApp查询格口
//         POST /api/markSorted             WCSApp标记已分拣
//         POST /api/markException           WCSApp标记异常
//         GET  /api/waveStatus             WCSApp查询波次状态
// ============================================================================

#include <QObject>
#include <QJsonObject>
#include "HPSocket.h"
#include "TaskQueue.h"
#include "DoubleBuffer.h"
#include "WaveManager.h"

class HttpServer;

// ──── HP-Socket 请求处理器 ────
class CHttpHandler : public IHttpServerHandler
{
public:
    CHttpHandler(HttpServer* pOwner);
    virtual EnHandleResult OnRequest(IHttpServer* pSender, CHttpRequest* pReq,
                                     CHttpResponse* pResp, int iSeq) override;
private:
    HttpServer* m_pOwner;
};

// ──── HTTP Server 管理类 ────
class HttpServer : public QObject
{
    Q_OBJECT
public:
    explicit HttpServer(QObject* parent = nullptr);
    ~HttpServer();

    bool start(int externalPort = 8191, int internalPort = 8192);
    void stop();
    bool isRunning() const { return m_pExtServer && m_pExtServer->IsStarted(); }

    // 组件访问
    TaskQueue*   taskQueue()    { return m_pQueue; }
    GridBuffer*  gridBuffer()   { return m_pBuffer; }
    WaveManager* waveManager()  { return m_pWaveMgr; }
    ParseWorker* parseWorker()  { return m_pWorker; }

signals:
    void serverStarted(int extPort, int intPort);
    void serverStopped();
    void logMessage(const QString& msg, bool isError = false);

private:
    friend class CHttpHandler;

    // 请求处理
    QJsonObject handleInsertWaveInfo(const QJsonObject& req);
    QJsonObject handleQuery(const QString& code);
    QJsonObject handleMarkSorted(const QJsonObject& req);
    QJsonObject handleMarkException(const QJsonObject& req);
    QJsonObject handleWaveStatus();

    // 响应辅助
    QJsonObject okResponse(const QString& data = "accepted");
    QJsonObject errResponse(const QString& msg, int code = -1);

    // 组件
    IHttpServerPtr  m_pExtServer;    // 外部端口（WMS推送）
    IHttpServerPtr  m_pIntServer;    // 内部端口（WCSApp查询）
    CHttpHandler    m_handler;
    TaskQueue*      m_pQueue    = nullptr;
    GridBuffer*     m_pBuffer   = nullptr;
    WaveManager*    m_pWaveMgr  = nullptr;
    ParseWorker*    m_pWorker   = nullptr;
};
