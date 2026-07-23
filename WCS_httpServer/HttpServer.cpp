#include "HttpServer.h"
#include "ParseWorker.h"
#include "log_center.h"
#include "hlog1.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QUrlQuery>
#include <QDateTime>
#include <cstring>
#include <windows.h>
#include <tchar.h>

// ============================================================================
// HP-Socket CHttpServerListener 模式
//   m_pServer(this)  // this = IHttpServerListener*
//   m_pServer->Start(_T("0.0.0.0"), port);
//   m_pServer->Stop();
// ============================================================================

HttpServer::HttpServer(QObject* parent)
    : QObject(parent)
    , m_pServer(this)           // ★ this = IHttpServerListener*
{
    m_pQueue   = new TaskQueue(3);
    m_pBuffer  = new GridBuffer();
    m_pWaveMgr = new WaveManager(m_pBuffer, this);
    m_pWorker  = new ParseWorker(m_pQueue, m_pBuffer, this);

    connect(m_pWorker, &ParseWorker::waveParsed, this,
        [this](const QString& orderCode, int skuCount, int orderQty, qint64, const QSet<QString>& recvSet) {
            m_pWaveMgr->setWaveData(orderCode, orderQty, skuCount);
            m_pWaveMgr->setRecvSet(recvSet);
        });

    connect(m_pWaveMgr, &WaveManager::waveReadyToReport, this, &HttpServer::waveReadyToReport);

    LogCenter::Instance()->wcs_run_log_warn(true, "[Http] HttpServer已创建");
}

HttpServer::~HttpServer()
{
    stop();
    m_pWorker->stop();
    m_pWorker->wait(3000);
}

bool HttpServer::start(int port)
{
    m_pWorker->start();

    // HP-Socket 模式 + Demo验证: Start(LPCTSTR, port)
    if (!m_pServer->Start(_T("0.0.0.0"), port))
    {
        LogCenter::Instance()->wcs_run_log_warn(false,
            QString("[Http] 启动失败 port=%1 err=%2").arg(port).arg((int)::GetLastError()));
        return false;
    }

    LogCenter::Instance()->wcs_run_log_warn(true,
        QString("[Http] 服务已启动 port=%1").arg(port));
    emit serverStarted(port);
    return true;
}

void HttpServer::stop()
{
    if (m_pServer && m_pServer->HasStarted())
    {
        m_pServer->Stop();
    }
    m_pServer.Reset();
    emit serverStopped();
}

// ============================================================================
// CHttpServerListener 回调
// ============================================================================

EnHttpParseResult HttpServer::OnRequestLine(IHttpServer* pSender, CONNID dwConnID,
                                             LPCSTR lpszMethod, LPCSTR lpszUrl)
{
    std::lock_guard<std::mutex> lock(m_connMutex);
    ConnState& st = m_connStates[dwConnID];
    st.method = QString::fromUtf8(lpszMethod);
    QString full = QString::fromUtf8(lpszUrl);
    int q = full.indexOf('?');
    st.path = (q >= 0) ? full.left(q) : full;
    st.queryString = (q >= 0) ? full.mid(q + 1) : QString();
    return HPR_OK;
}

EnHttpParseResult HttpServer::OnBody(IHttpServer* pSender, CONNID dwConnID,
                                      const BYTE* pData, int iLength)
{
    std::lock_guard<std::mutex> lock(m_connMutex);
    m_connStates[dwConnID].body.append((const char*)pData, iLength);
    return HPR_OK;
}

EnHttpParseResult HttpServer::OnMessageComplete(IHttpServer* pSender, CONNID dwConnID)
{
    ConnState state;
    {
        std::lock_guard<std::mutex> lock(m_connMutex);
        auto it = m_connStates.find(dwConnID);
        if (it != m_connStates.end()) state = it.value();
    }
    processRequest(pSender, dwConnID, state);
    return HPR_OK;
}

EnHandleResult HttpServer::OnClose(ITcpServer* pSender, CONNID dwConnID,
                                    EnSocketOperation, int)
{
    std::lock_guard<std::mutex> lock(m_connMutex);
    m_connStates.remove(dwConnID);
    return HR_OK;
}

// ============================================================================
// 请求分发
// ============================================================================

void HttpServer::processRequest(IHttpServer* pSender, CONNID dwConnID, ConnState& st)
{
    if (st.path == "/api/DispatchSortingCommand/InsertWaveInfo" && st.method == "POST")
    {
        if (m_pQueue->size() >= 3)
        {
            sendJsonResponse(pSender, dwConnID, errResponse("服务器繁忙", 503), 503);
        }
        else if (st.body.isEmpty())
        {
            sendJsonResponse(pSender, dwConnID, errResponse("Body为空"));
        }
        else
        {
            WaveTask task;
            task.rawBody  = st.body;
            task.recvTime = QDateTime::currentMSecsSinceEpoch();
            m_pQueue->push(task);
            sendJsonResponse(pSender, dwConnID, okResponse("accepted"));
            WCS_INFO("[Http] WMS入队 len=%d queue=%d", st.body.size(), m_pQueue->size());
        }
        return;
    }

    if (st.path == "/api/query" && st.method == "GET")
    {
        QUrlQuery q(st.queryString);
        sendJsonResponse(pSender, dwConnID, handleQuery(q.queryItemValue("code").trimmed()));
        return;
    }

    if (st.path == "/api/markSorted" && st.method == "POST")
    {
        QJsonDocument d = QJsonDocument::fromJson(st.body);
        sendJsonResponse(pSender, dwConnID, handleMarkSorted(d.object()));
        return;
    }

    if (st.path == "/api/markException" && st.method == "POST")
    {
        QJsonDocument d = QJsonDocument::fromJson(st.body);
        sendJsonResponse(pSender, dwConnID, handleMarkException(d.object()));
        return;
    }

    if (st.path == "/api/waveStatus" && st.method == "GET")
    {
        sendJsonResponse(pSender, dwConnID, handleWaveStatus());
        return;
    }

    sendJsonResponse(pSender, dwConnID, errResponse("Not Found", 404), 404);
}

// ============================================================================
// 业务处理
// ============================================================================

QJsonObject HttpServer::handleQuery(const QString& code)
{
    if (code.isEmpty()) return errResponse("缺少code参数");
    GridEntry e = m_pWaveMgr->getGrid(code);
    if (e.gridNum.isEmpty()) { m_pWaveMgr->markException(code); return errResponse("未找到格口映射"); }
    QString g = e.gridNum;
    if (g.contains(',')) g = g.split(',').first().trimmed();
    QJsonObject r;
    r["success"]=true; r["grid"]=g; r["gridType"]=e.gridType;
    r["gridCount"]=e.gridCount; r["orderCode"]=m_pWaveMgr->orderCode();
    return r;
}

QJsonObject HttpServer::handleMarkSorted(const QJsonObject& req)
{
    QString c = req["code"].toString().trimmed();
    if (c.isEmpty()) return errResponse("缺少code");
    m_pWaveMgr->markSorted(c);
    QJsonObject r;
    r["success"]=true; r["waveComplete"]=m_pWaveMgr->isWaveComplete();
    r["orderCode"]=m_pWaveMgr->orderCode();
    return r;
}

QJsonObject HttpServer::handleMarkException(const QJsonObject& req)
{
    QString c = req["code"].toString().trimmed();
    if (c.isEmpty()) return errResponse("缺少code");
    m_pWaveMgr->markException(c);
    QJsonObject r;
    r["success"]=true; r["waveComplete"]=m_pWaveMgr->isWaveComplete();
    r["orderCode"]=m_pWaveMgr->orderCode();
    return r;
}

QJsonObject HttpServer::handleWaveStatus()
{
    WaveSnapshot s = m_pWaveMgr->snapshot();
    QJsonObject r;
    r["success"]=true; r["orderCode"]=s.orderCode; r["waveStatus"]=s.waveStatus;
    r["statusText"]=s.statusText; r["orderQty"]=s.orderQty; r["skuCount"]=s.skuCount;
    r["sortedCount"]=s.sortedCount; r["exceptionCount"]=s.exceptionCount;
    r["totalRecv"]=s.totalRecv; r["sumLocation"]=s.sumLocation; r["elapsedSec"]=s.elapsedSec;
    return r;
}

void HttpServer::sendJsonResponse(IHttpServer* pSender, CONNID dwConnID,
                                   const QJsonObject& json, USHORT status)
{
    QByteArray d = QJsonDocument(json).toJson(QJsonDocument::Compact);
    THeader h[1];
    h[0].name = "Content-Type";
    h[0].value = "application/json; charset=UTF-8";
    pSender->SendResponse(dwConnID, status, nullptr, h, 1, (const BYTE*)d.constData(), d.length());
}

QJsonObject HttpServer::okResponse(const QString& data)
{
    QJsonObject r;
    r["resultCode"]="0"; r["success"]=true; r["resultData"]=data;
    r["resultTime"]=QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzzz");
    return r;
}

QJsonObject HttpServer::errResponse(const QString& msg, int code)
{
    QJsonObject r;
    r["resultCode"]=QString::number(code); r["success"]=false;
    r["errorMsg"]=msg;
    r["resultTime"]=QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzzz");
    return r;
}
