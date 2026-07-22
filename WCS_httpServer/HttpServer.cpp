#include "HttpServer.h"
#include "ParseWorker.h"
#include "log_center.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QUrlQuery>
#include <QDateTime>
#include <cstring>

// ============================================================================
// CHttpHandler 实现
// ============================================================================

CHttpHandler::CHttpHandler(HttpServer* pOwner) : m_pOwner(pOwner) {}

EnHandleResult CHttpHandler::OnRequest(IHttpServer* pSender, CHttpRequest* pReq,
                                        CHttpResponse* pResp, int iSeq)
{
    LPCSTR path = pReq->GetPath();
    if (!path) return HR_SKIP;

    LPCSTR method = pReq->GetMethod();
    int bodyLen   = pReq->GetBodyLength();
    LPCSTR body   = pReq->GetBody();

    QJsonObject respJson;

    // ──── 外部接口：WMS推送波次 ────
    if (strcmp(path, "/api/DispatchSortingCommand/InsertWaveInfo") == 0 && method && strcmp(method, "POST") == 0)
    {
        // 水位保护
        if (m_pOwner->m_pQueue->size() >= 3)
        {
            respJson = m_pOwner->errResponse("服务器繁忙，请稍后重试", 503);
        }
        else if (bodyLen <= 0 || !body)
        {
            respJson = m_pOwner->errResponse("请求Body为空");
        }
        else
        {
            // 入队（只拷贝，不解析）
            WaveTask task;
            task.rawBody  = QByteArray(body, bodyLen);
            task.recvTime = QDateTime::currentMSecsSinceEpoch();
            m_pOwner->m_pQueue->push(task);

            respJson = m_pOwner->okResponse("accepted");
            WCS_INFO("[Http] WMS波次入队 bodyLen=%d queueSize=%d", bodyLen, m_pOwner->m_pQueue->size());
        }

        pResp->SetStatus(respJson["success"].toBool() ? 200 : 503);
        pResp->SetContentType("application/json; charset=UTF-8");
        QByteArray respData = QJsonDocument(respJson).toJson(QJsonDocument::Compact);
        pResp->SetBody(respData.constData(), respData.length());
        return HR_OK;
    }

    // ──── 内部接口：WCSApp查询格口 ────
    if (strcmp(path, "/api/query") == 0 && method && strcmp(method, "GET") == 0)
    {
        LPCSTR rawParams = pReq->GetQueryString();
        QString params = rawParams ? QString::fromUtf8(rawParams) : QString();
        QUrlQuery query(params);
        QString code = query.queryItemValue("code").trimmed();

        respJson = m_pOwner->handleQuery(code);
        pResp->SetStatus(200);
        pResp->SetContentType("application/json; charset=UTF-8");
        QByteArray respData = QJsonDocument(respJson).toJson(QJsonDocument::Compact);
        pResp->SetBody(respData.constData(), respData.length());
        return HR_OK;
    }

    // ──── 内部接口：WCSApp标记已分拣 ────
    if (strcmp(path, "/api/markSorted") == 0 && method && strcmp(method, "POST") == 0)
    {
        QJsonDocument doc = QJsonDocument::fromJson(QByteArray(body, bodyLen));
        respJson = m_pOwner->handleMarkSorted(doc.object());
        pResp->SetStatus(200);
        pResp->SetContentType("application/json; charset=UTF-8");
        QByteArray respData = QJsonDocument(respJson).toJson(QJsonDocument::Compact);
        pResp->SetBody(respData.constData(), respData.length());
        return HR_OK;
    }

    // ──── 内部接口：WCSApp标记异常 ────
    if (strcmp(path, "/api/markException") == 0 && method && strcmp(method, "POST") == 0)
    {
        QJsonDocument doc = QJsonDocument::fromJson(QByteArray(body, bodyLen));
        respJson = m_pOwner->handleMarkException(doc.object());
        pResp->SetStatus(200);
        pResp->SetContentType("application/json; charset=UTF-8");
        QByteArray respData = QJsonDocument(respJson).toJson(QJsonDocument::Compact);
        pResp->SetBody(respData.constData(), respData.length());
        return HR_OK;
    }

    // ──── 内部接口：WCSApp查询波次状态 ────
    if (strcmp(path, "/api/waveStatus") == 0 && method && strcmp(method, "GET") == 0)
    {
        respJson = m_pOwner->handleWaveStatus();
        pResp->SetStatus(200);
        pResp->SetContentType("application/json; charset=UTF-8");
        QByteArray respData = QJsonDocument(respJson).toJson(QJsonDocument::Compact);
        pResp->SetBody(respData.constData(), respData.length());
        return HR_OK;
    }

    return HR_SKIP;
}

// ============================================================================
// HttpServer 实现
// ============================================================================

HttpServer::HttpServer(QObject* parent)
    : QObject(parent), m_handler(this)
{
    m_pQueue   = new TaskQueue(3);       // 最多积压3个波次
    m_pBuffer  = new GridBuffer();
    m_pWaveMgr = new WaveManager(m_pBuffer, this);
    m_pWorker  = new ParseWorker(m_pQueue, m_pBuffer, this);

    // 解析完成后通知WaveManager设置波次数据
    connect(m_pWorker, &ParseWorker::waveParsed, this, [this](const QString& orderCode, int skuCount, int orderQty, qint64) {
        m_pWaveMgr->setWaveData(orderCode, orderQty, skuCount);
    });

    // 波次可回传时发送信号
    connect(m_pWaveMgr, &WaveManager::waveReadyToReport, this, [this](const QString& orderCode) {
        WCS_INFO("[Http] 波次可回传 orderCode=%s", orderCode.toLocal8Bit().data());
    });

    LogCenter::Instance()->wcs_run_log_warn(true, "[Http] HttpServer已创建");
}

HttpServer::~HttpServer()
{
    stop();
    m_pWorker->stop();
    m_pWorker->wait(3000);
}

bool HttpServer::start(int externalPort, int internalPort)
{
    // 启动解析线程
    m_pWorker->start();

    // 启动外部HTTP Server (WMS推送端口)
    m_pExtServer = IHttpServerPtr(new IHttpServer(m_handler));
    if (!m_pExtServer->Start("0.0.0.0", externalPort))
    {
        LogCenter::Instance()->wcs_run_log_warn(false,
            QString("[Http] 外部端口启动失败 port=%1").arg(externalPort));
        return false;
    }

    // 启动内部HTTP Server (WCSApp查询端口)
    m_pIntServer = IHttpServerPtr(new IHttpServer(m_handler));
    if (!m_pIntServer->Start("127.0.0.1", internalPort))
    {
        LogCenter::Instance()->wcs_run_log_warn(false,
            QString("[Http] 内部端口启动失败 port=%1").arg(internalPort));
        m_pExtServer->Stop();
        return false;
    }

    LogCenter::Instance()->wcs_run_log_warn(true,
        QString("[Http] 服务已启动 WMS端口=%1 内部端口=%2").arg(externalPort).arg(internalPort));

    emit serverStarted(externalPort, internalPort);
    return true;
}

void HttpServer::stop()
{
    if (m_pExtServer) { m_pExtServer->Stop(); m_pExtServer.reset(); }
    if (m_pIntServer) { m_pIntServer->Stop(); m_pIntServer.reset(); }
    emit serverStopped();
}

// ============================================================================
// 请求处理
// ============================================================================

QJsonObject HttpServer::handleQuery(const QString& code)
{
    if (code.isEmpty())
        return errResponse("缺少code参数");

    GridEntry entry = m_pWaveMgr->getGrid(code);
    if (entry.gridNum.isEmpty())
    {
        m_pWaveMgr->markException(code);
        return errResponse("未找到格口映射");
    }

    // 处理同品多格口：取第一个
    QString grid = entry.gridNum;
    if (grid.contains(','))
        grid = grid.split(',').first().trimmed();

    QJsonObject resp;
    resp["success"]   = true;
    resp["grid"]      = grid;
    resp["gridType"]  = entry.gridType;
    resp["gridCount"] = entry.gridCount;
    resp["orderCode"] = m_pWaveMgr->orderCode();
    return resp;
}

QJsonObject HttpServer::handleMarkSorted(const QJsonObject& req)
{
    QString code = req["code"].toString().trimmed();
    if (code.isEmpty())
        return errResponse("缺少code字段");

    m_pWaveMgr->markSorted(code);

    QJsonObject resp;
    resp["success"]      = true;
    resp["waveComplete"]  = m_pWaveMgr->isWaveComplete();
    resp["orderCode"]     = m_pWaveMgr->orderCode();
    return resp;
}

QJsonObject HttpServer::handleMarkException(const QJsonObject& req)
{
    QString code = req["code"].toString().trimmed();
    if (code.isEmpty())
        return errResponse("缺少code字段");

    m_pWaveMgr->markException(code);

    QJsonObject resp;
    resp["success"]      = true;
    resp["waveComplete"]  = m_pWaveMgr->isWaveComplete();
    resp["orderCode"]     = m_pWaveMgr->orderCode();
    return resp;
}

QJsonObject HttpServer::handleWaveStatus()
{
    WaveSnapshot snap = m_pWaveMgr->snapshot();

    QJsonObject resp;
    resp["success"]         = true;
    resp["orderCode"]       = snap.orderCode;
    resp["waveStatus"]      = snap.waveStatus;
    resp["statusText"]      = snap.statusText;
    resp["orderQty"]        = snap.orderQty;
    resp["skuCount"]        = snap.skuCount;
    resp["sortedCount"]     = snap.sortedCount;
    resp["exceptionCount"]  = snap.exceptionCount;
    resp["totalRecv"]       = snap.totalRecv;
    resp["sumLocation"]     = snap.sumLocation;
    resp["elapsedSec"]      = snap.elapsedSec;
    return resp;
}

// ============================================================================
// 响应辅助
// ============================================================================

QJsonObject HttpServer::okResponse(const QString& data)
{
    QJsonObject resp;
    resp["resultCode"]   = "0";
    resp["success"]      = true;
    resp["resultData"]   = data;
    resp["resultTime"]   = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzzz");
    return resp;
}

QJsonObject HttpServer::errResponse(const QString& msg, int code)
{
    QJsonObject resp;
    resp["resultCode"]   = QString::number(code);
    resp["success"]      = false;
    resp["errorMsg"]     = msg;
    resp["resultTime"]   = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzzz");
    return resp;
}
