#include "HttpClient.h"
#include "log_center.h"
#include "hlog1.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QNetworkRequest>

// HTTP 服务专用日志宏
#ifndef HTTP_INFO
#define HTTP_INFO(fmt, ...)  hlog_format(HLOG_LEVEL_INFO,  "HTTP", "\t" fmt, ##__VA_ARGS__)
#define HTTP_WARN(fmt, ...)  hlog_format(HLOG_LEVEL_WARN,  "HTTP", "\t" fmt, ##__VA_ARGS__)
#define HTTP_ERROR(fmt, ...) hlog_format(HLOG_LEVEL_ERROR, "HTTP", "\t" fmt, ##__VA_ARGS__)
#endif

HttpClient::HttpClient(QObject* parent)
    : QObject(parent)
{
    m_pNetworkMgr = new QNetworkAccessManager(this);
}

HttpClient::~HttpClient()
{
    // 取消所有进行中的 WMS 回传请求
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it)
    {
        if (it->timer)  { it->timer->stop(); delete it->timer; }
        if (it->reply)  { it->reply->abort(); it->reply->deleteLater(); }
    }
    m_pending.clear();

    // 取消所有进行中的 RFID 查询请求
    for (auto it = m_rfidPending.begin(); it != m_rfidPending.end(); ++it)
    {
        if (it->timer)  { it->timer->stop(); delete it->timer; }
        if (it->reply)  { it->reply->abort(); it->reply->deleteLater(); }
    }
    m_rfidPending.clear();
}

void HttpClient::sendWaveComplete(const QString& orderCode, int sumLocation)
{
    // ★ 空URL防护：避免QNetworkAccessManager::post崩溃
    if (m_url.isEmpty())
    {
        HTTP_ERROR("回传URL为空，跳过 orderCode=%s", orderCode.toLocal8Bit().data());
        LogCenter::Instance()->wcs_run_log_warn(false,
            QString("[Report] 回传URL为空 orderCode=%1").arg(orderCode));
        emit reportResult(orderCode, false, "URL is empty");
        return;
    }

    HTTP_INFO("回传开始 orderCode=%s sumLocation=%d", orderCode.toLocal8Bit().data(), sumLocation);
    LogCenter::Instance()->wcs_run_log_warn(true,
        QString("[Report] 开始回传 orderCode=%1 sumLocation=%2").arg(orderCode).arg(sumLocation));

    // ──── 构造回传 JSON（格式由WMS接口文档定义）────
    QJsonObject head;
    head["orderCode"]     = orderCode;                                    // 波次号
    head["orderType"]     = WMS_ORDER_TYPE;                              // 业务类型（define.h: 02=退货分类）
    head["sumLocation"]   = QString::number(sumLocation);                // 使用的格口总数
    head["operuserDate"]  = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    head["operuserCode"]  = WMS_OPERUSER_CODE;                           // 操作人编码（define.h）
    head["operuserName"]  = QString::fromUtf8(WMS_OPERUSER_NAME);        // 操作人名称（define.h）

    QJsonObject req;
    req["head"] = head;
    QByteArray postData = QJsonDocument(req).toJson(QJsonDocument::Compact);

    QUrl url(m_url);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json; charset=UTF-8");
    request.setRawHeader("AppKey", m_appkey.toUtf8());  // WMS鉴权Header

    QNetworkReply* reply = m_pNetworkMgr->post(request, postData);

    // ──── 超时定时器：单次触发，到时触发 onReplyTimeout() ────
    QTimer* timer = new QTimer(this);
    timer->setSingleShot(true);
    timer->setInterval(m_timeoutMs);  // 默认3000ms

    PendingRequest pr;
    pr.reply       = reply;
    pr.timer       = timer;
    pr.orderCode   = orderCode;
    pr.sumLocation = sumLocation;
    m_pending.insert(reply, pr);

    // 连接信号（异步，不阻塞主线程）
    connect(reply, &QNetworkReply::finished, this, &HttpClient::onReplyFinished);
    connect(timer, &QTimer::timeout, this, &HttpClient::onReplyTimeout);

    timer->start();
}

void HttpClient::onReplyFinished()
{
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    auto it = m_pending.find(reply);
    if (it == m_pending.end()) return;

    PendingRequest& pr = it.value();
    if (pr.timer) { pr.timer->stop(); pr.timer->deleteLater(); pr.timer = nullptr; }  // 取消超时定时器

    QByteArray respBody = reply->readAll();
    int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();

    QJsonDocument doc = QJsonDocument::fromJson(respBody);
    bool success = doc.object()["success"].toBool(false);

    HTTP_INFO("回传完成 orderCode=%s status=%d success=%d",
        pr.orderCode.toLocal8Bit().data(), statusCode, success);
    LogCenter::Instance()->wcs_run_log_warn(success,
        QString("[Report] orderCode=%1 success=%2 status=%3 body=%4")
            .arg(pr.orderCode).arg(success).arg(statusCode)
            .arg(QString::fromUtf8(respBody).left(RESP_BODY_LOG_TRUNCATE)));  // 截断防止日志过长

    emit reportResult(pr.orderCode, success, QString::fromUtf8(respBody));
    m_pending.erase(it);
}

void HttpClient::onReplyTimeout()
{
    QTimer* timer = qobject_cast<QTimer*>(sender());
    if (!timer) return;

    // 查找超时定时器对应的 PendingRequest
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it)
    {
        if (it->timer == timer)
        {
            PendingRequest& pr = it.value();
            HTTP_WARN("回传超时 orderCode=%s timeout=%dms",
                pr.orderCode.toLocal8Bit().data(), m_timeoutMs);
            LogCenter::Instance()->wcs_run_log_warn(false,
                QString("[Report] 回传超时 orderCode=%1 timeout=%2ms")
                    .arg(pr.orderCode).arg(m_timeoutMs));

            // ★ 关键修复：先断开 finished 信号再 abort
            //   防止 onReplyFinished 在 abort 时同步触发导致双重 erase
            if (pr.reply) {
                disconnect(pr.reply, &QNetworkReply::finished, this, &HttpClient::onReplyFinished);
                pr.reply->abort();
                pr.reply->deleteLater();
            }
            pr.timer->deleteLater();
            emit reportResult(pr.orderCode, false, QString());
            m_pending.erase(it);
            break;
        }
    }
}

void HttpClient::sendGenericFeedback(const QJsonObject& json, const QString& context)
{
    if (m_url.isEmpty())
    {
        HTTP_ERROR("回传URL为空，跳过 context=%s", context.toLocal8Bit().data());
        return;
    }

    QByteArray postData = QJsonDocument(json).toJson(QJsonDocument::Compact);

    QUrl url(m_url);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json; charset=UTF-8");
    request.setRawHeader("AppKey", m_appkey.toUtf8());

    QNetworkReply* reply = m_pNetworkMgr->post(request, postData);

    QTimer* timer = new QTimer(this);
    timer->setSingleShot(true);
    timer->setInterval(m_timeoutMs);

    PendingRequest pr;
    pr.reply       = reply;
    pr.timer       = timer;
    pr.orderCode   = context.isEmpty() ? "lockGrid" : context;
    pr.sumLocation = 0;
    m_pending.insert(reply, pr);

    connect(reply, &QNetworkReply::finished, this, &HttpClient::onReplyFinished);
    connect(timer, &QTimer::timeout, this, &HttpClient::onReplyTimeout);
    timer->start();

    HTTP_INFO("锁格回传发送 context=%s len=%d", context.toLocal8Bit().data(), postData.size());
}

// ============================================================================
// queryRfid — 向RFID服务查询EPC对应的SKU/条码信息（异步）
// 请求格式: POST {rfidUrl}  Body: {"epcList":["EPC1","EPC2",...]}
// 响应格式: {"data":{"data":[{"barcode":"xxx","epc":"xxx",...}]},"success":true}
// ============================================================================
void HttpClient::queryRfid(const QJsonArray& epcList, const QString& context)
{
    if (m_rfidUrl.isEmpty())
    {
        HTTP_ERROR("RFID查询URL为空，跳过 context=%s epcCount=%d",
            context.toLocal8Bit().data(), epcList.size());
        emit rfidQueryResult(QJsonObject(), context);
        return;
    }

    if (epcList.isEmpty())
    {
        HTTP_WARN("RFID查询 EPC列表为空 context=%s", context.toLocal8Bit().data());
        emit rfidQueryResult(QJsonObject(), context);
        return;
    }

    HTTP_INFO("RFID查询开始 context=%s epcCount=%d", context.toLocal8Bit().data(), epcList.size());

    QJsonObject reqBody;
    reqBody["epcList"] = epcList;
    QByteArray postData = QJsonDocument(reqBody).toJson(QJsonDocument::Compact);

    QUrl url(m_rfidUrl);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json; charset=UTF-8");

    QNetworkReply* reply = m_pNetworkMgr->post(request, postData);

    QTimer* timer = new QTimer(this);
    timer->setSingleShot(true);
    timer->setInterval(m_rfidTimeoutMs);

    RfidPendingRequest rpr;
    rpr.reply   = reply;
    rpr.timer   = timer;
    rpr.context = context;
    rpr.epcList = epcList;  // ★ 保存原始EPC列表用于重试
    rpr.retryCount = 0;     // ★ 初始化重试计数
    m_rfidPending.insert(reply, rpr);

    connect(reply, &QNetworkReply::finished, this, &HttpClient::onRfidReplyFinished);
    connect(timer, &QTimer::timeout, this, &HttpClient::onRfidReplyTimeout);
    timer->start();
}

void HttpClient::onRfidReplyFinished()
{
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    auto it = m_rfidPending.find(reply);
    if (it == m_rfidPending.end()) return;

    RfidPendingRequest& rpr = it.value();
    if (rpr.timer) { rpr.timer->stop(); rpr.timer->deleteLater(); rpr.timer = nullptr; }

    QByteArray respBody = reply->readAll();
    int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();

    QJsonDocument doc = QJsonDocument::fromJson(respBody);
    QJsonObject result = doc.object();
    bool success = result["success"].toBool(false);

    if (success)
    {
        QJsonObject dataObj = result["data"].toObject();
        QJsonArray dataArr  = dataObj["data"].toArray();
        HTTP_INFO("RFID查询成功 context=%s status=%d epcMatched=%d",
            rpr.context.toLocal8Bit().data(), statusCode, dataArr.size());
        emit rfidQueryResult(result, rpr.context);
        m_rfidPending.erase(it);
    }
    else
    {
        QString errMsg = result["msg"].toString();
        HTTP_WARN("RFID查询失败 context=%s status=%d msg=%s retry=%d/%d",
            rpr.context.toLocal8Bit().data(), statusCode,
            errMsg.toLocal8Bit().data(),
            rpr.retryCount, m_rfidRetryMax);

        // ★ S4 重试机制（T-S4-03）
        if (rpr.retryCount < m_rfidRetryMax)
        {
            rpr.retryCount++;
            // 重新发起请求（使用已保存的 epcList）
            QJsonObject reqBody;
            reqBody["epcList"] = rpr.epcList;
            QByteArray postData = QJsonDocument(reqBody).toJson(QJsonDocument::Compact);
            QUrl url(m_rfidUrl);
            QNetworkRequest request(url);
            request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json; charset=UTF-8");
            QNetworkReply* newReply = m_pNetworkMgr->post(request, postData);

            QTimer* newTimer = new QTimer(this);
            newTimer->setSingleShot(true);
            newTimer->setInterval(m_rfidTimeoutMs);

            RfidPendingRequest newRpr;
            newRpr.reply      = newReply;
            newRpr.timer      = newTimer;
            newRpr.context    = rpr.context;
            newRpr.epcList    = rpr.epcList;
            newRpr.retryCount = rpr.retryCount;

            m_rfidPending.erase(it);
            m_rfidPending.insert(newReply, newRpr);

            connect(newReply, &QNetworkReply::finished, this, &HttpClient::onRfidReplyFinished);
            connect(newTimer, &QTimer::timeout, this, &HttpClient::onRfidReplyTimeout);
            newTimer->start();
        }
        else
        {
            // 重试耗尽，返回失败
            HTTP_ERROR("RFID查询重试耗尽 context=%s retry=%d",
                rpr.context.toLocal8Bit().data(), rpr.retryCount);
            emit rfidQueryResult(result, rpr.context);
            m_rfidPending.erase(it);
        }
    }
}

void HttpClient::onRfidReplyTimeout()
{
    QTimer* timer = qobject_cast<QTimer*>(sender());
    if (!timer) return;

    for (auto it = m_rfidPending.begin(); it != m_rfidPending.end(); ++it)
    {
        if (it->timer == timer)
        {
            RfidPendingRequest& rpr = it.value();
            HTTP_WARN("RFID查询超时 context=%s timeout=%dms retry=%d/%d",
                rpr.context.toLocal8Bit().data(), m_rfidTimeoutMs,
                rpr.retryCount, m_rfidRetryMax);

            if (rpr.reply) {
                disconnect(rpr.reply, &QNetworkReply::finished, this, &HttpClient::onRfidReplyFinished);
                rpr.reply->abort();
                rpr.reply->deleteLater();
            }
            rpr.timer->deleteLater();

            // ★ S4 重试机制（T-S4-03）
            if (rpr.retryCount < m_rfidRetryMax)
            {
                rpr.retryCount++;
                // 重新发起请求
                QJsonObject reqBody;
                reqBody["epcList"] = rpr.epcList;
                QByteArray postData = QJsonDocument(reqBody).toJson(QJsonDocument::Compact);
                QUrl url(m_rfidUrl);
                QNetworkRequest request(url);
                request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json; charset=UTF-8");
                QNetworkReply* newReply = m_pNetworkMgr->post(request, postData);

                QTimer* newTimer = new QTimer(this);
                newTimer->setSingleShot(true);
                newTimer->setInterval(m_rfidTimeoutMs);

                RfidPendingRequest newRpr;
                newRpr.reply      = newReply;
                newRpr.timer      = newTimer;
                newRpr.context    = rpr.context;
                newRpr.epcList    = rpr.epcList;
                newRpr.retryCount = rpr.retryCount;

                m_rfidPending.erase(it);
                m_rfidPending.insert(newReply, newRpr);

                connect(newReply, &QNetworkReply::finished, this, &HttpClient::onRfidReplyFinished);
                connect(newTimer, &QTimer::timeout, this, &HttpClient::onRfidReplyTimeout);
                newTimer->start();
            }
            else
            {
                HTTP_ERROR("RFID查询超时重试耗尽 context=%s retry=%d",
                    rpr.context.toLocal8Bit().data(), rpr.retryCount);
                emit rfidQueryResult(QJsonObject(), rpr.context);
                m_rfidPending.erase(it);
            }
            break;
        }
    }
}
