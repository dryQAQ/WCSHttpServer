#include "HttpClient.h"
#include "LogService.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QNetworkRequest>

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
}

void HttpClient::sendWaveComplete(const QString& orderCode, int sumLocation)
{
    // ★ 空URL防护：避免QNetworkAccessManager::post崩溃
    if (m_url.isEmpty())
    {
        HTTP_LOG_ERROR("回传URL为空，跳过 orderCode=%s", orderCode.toLocal8Bit().data());
        LogCenter::Instance()->wcs_run_log_warn(false,
            QString("[Report] 回传URL为空 orderCode=%1").arg(orderCode));
        emit reportResult(orderCode, false, "URL is empty");
        return;
    }

    HTTP_LOG_INFO("回传开始 orderCode=%s sumLocation=%d", orderCode.toLocal8Bit().data(), sumLocation);
    LogCenter::Instance()->wcs_run_log_warn(true,
        QString("[Report] 开始回传 orderCode=%1 sumLocation=%2").arg(orderCode).arg(sumLocation));

    // ──── 构造回传 JSON（格式由WMS接口文档定义）────
    QJsonObject head;
    head["orderCode"]     = orderCode;                                    // 波次号
    head["orderType"]     = WMS_ORDER_TYPE;                              // 业务类型（define.h: 02=退货分类）
    head["sumLocation"]   = QString::number(sumLocation);                // 落格分拣总件数（告知WMS分拣了多少件）
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

    // ★ status=0 时记录 Qt 网络层错误，方便排查连接失败原因
    if (statusCode == 0) {
        HTTP_LOG_ERROR("回传网络层失败 orderCode=%s error=%d errorString=%s",
            pr.orderCode.toLocal8Bit().data(),
            reply->error(),
            reply->errorString().toLocal8Bit().data());
    }

    reply->deleteLater();

    QJsonDocument doc = QJsonDocument::fromJson(respBody);
    bool success = doc.object()["success"].toBool(false);

    HTTP_LOG_INFO("回传完成 orderCode=%s status=%d success=%d",
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
            HTTP_LOG_WARN("回传超时 orderCode=%s timeout=%dms",
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

// ============================================================================
// sendGenericFeedback — 发送锁格回传/满箱回传/完结回传到WMS（异步）
// ============================================================================
void HttpClient::sendGenericFeedback(const QJsonObject& json, const QString& context)
{
    if (m_url.isEmpty())
    {
        HTTP_LOG_ERROR("回传URL为空，跳过 context=%s", context.toLocal8Bit().data());
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

    HTTP_LOG_INFO("锁格回传发送 context=%s len=%d", context.toLocal8Bit().data(), postData.size());
}

// ============================================================================
// sendEndFeedback — 发送完结回传到WMS（异步，使用 H8 专用 URL）
// 与 sendGenericFeedback 逻辑相同，仅目标 URL 不同
// ============================================================================
void HttpClient::sendEndFeedback(const QJsonObject& json, const QString& context)
{
    // ── 步骤1: URL 校验 ──
    if (m_endUrl.isEmpty())
    {
        HTTP_LOG_ERROR("完结回传URL为空，跳过 context=%s", context.toLocal8Bit().data());
        return;
    }

    // ── 步骤2: 序列化请求体 ──
    QByteArray postData = QJsonDocument(json).toJson(QJsonDocument::Compact);
    QString payloadPreview = QString::fromUtf8(postData).left(RESP_BODY_LOG_TRUNCATE);

    // ── 步骤3: 打印请求参数（URL、AppKey、Payload、超时） ──
    HTTP_LOG_INFO("完结回传（H8）请求参数: URL=%s AppKey=%s timeout=%dms",
        m_endUrl.toLocal8Bit().data(), m_appkey.toLocal8Bit().data(), m_timeoutMs);
    HTTP_LOG_INFO("完结回传（H8）请求体 context=%s len=%d payload=%s",
        context.toLocal8Bit().data(), postData.size(), payloadPreview.toLocal8Bit().data());

    // ── 步骤4: 构建 HTTP 请求 ──
    QUrl url(m_endUrl);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json; charset=UTF-8");
    request.setRawHeader("AppKey", m_appkey.toUtf8());

    QNetworkReply* reply = m_pNetworkMgr->post(request, postData);

    // ── 步骤5: 超时保护 ──
    QTimer* timer = new QTimer(this);
    timer->setSingleShot(true);
    timer->setInterval(m_timeoutMs);

    PendingRequest pr;
    pr.reply       = reply;
    pr.timer       = timer;
    pr.orderCode   = context.isEmpty() ? "end" : context;
    pr.sumLocation = 0;
    m_pending.insert(reply, pr);

    connect(reply, &QNetworkReply::finished, this, &HttpClient::onReplyFinished);
    connect(timer, &QTimer::timeout, this, &HttpClient::onReplyTimeout);
    timer->start();

    HTTP_LOG_INFO("完结回传（H8）请求已发送 context=%s pendingCount=%d",
        context.toLocal8Bit().data(), m_pending.size());
}

// ============================================================================
// queryRfidBinding — SKU-EPC 绑定查询（异步批量，不阻塞主线程）
// 向 RFID 查询 EPC→barcode 映射，完成后通过 rfidBindingResult 信号返回
// ============================================================================
void HttpClient::queryRfidBinding(const QStringList& epcList)
{
    if (m_rfidQueryUrl.isEmpty())
    {
        HTTP_LOG_WARN("RFID查询URL为空，跳过 SKU-EPC 绑定 epcCount=%d", epcList.size());
        emit rfidBindingResult({});
        return;
    }

    if (epcList.isEmpty())
    {
        HTTP_LOG_WARN("RFID查询 epcList为空，跳过");
        emit rfidBindingResult({});
        return;
    }

    HTTP_LOG_INFO("RFID绑定查询开始 url=%s epcCount=%d", m_rfidQueryUrl.toLocal8Bit().data(), epcList.size());

    // 构建请求 JSON: {"epcList":["EPC001","EPC002",...]}
    QJsonArray arr;
    for (const QString& epc : epcList)
    {
        arr.append(epc);
    }
    QJsonObject req;
    req["epcList"] = arr;
    QByteArray postData = QJsonDocument(req).toJson(QJsonDocument::Compact);

    QUrl url(m_rfidQueryUrl);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json; charset=UTF-8");

    QNetworkReply* reply = m_pNetworkMgr->post(request, postData);

    // 超时定时器（5秒）
    QTimer* timer = new QTimer(this);
    timer->setSingleShot(true);
    timer->setInterval(RFID_QUERY_TIMEOUT_MS);

    PendingRequest pr;
    pr.reply       = reply;
    pr.timer       = timer;
    pr.orderCode   = "rfidQuery";
    pr.sumLocation = epcList.size();
    m_pending.insert(reply, pr);

    connect(reply, &QNetworkReply::finished, this, &HttpClient::onRfidBindingReplyFinished);
    connect(timer, &QTimer::timeout, this, [this, timer, reply]() {
        HTTP_LOG_WARN("RFID绑定查询超时 url=%s timeout=%dms",
            m_rfidQueryUrl.toLocal8Bit().data(), RFID_QUERY_TIMEOUT_MS);
        // 断开 finished 信号，防止双重触发
        disconnect(reply, &QNetworkReply::finished, this, &HttpClient::onRfidBindingReplyFinished);
        reply->abort();
        reply->deleteLater();
        timer->deleteLater();
        m_pending.remove(reply);
        emit rfidBindingResult({});  // 超时返回空结果
    });
    timer->start();
}

// ============================================================================
// onRfidBindingReplyFinished — RFID 绑定查询响应回调
// 解析 RFID 返回的 EPC→barcode 映射，通过 rfidBindingResult 信号发出
// ============================================================================
void HttpClient::onRfidBindingReplyFinished()
{
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    auto it = m_pending.find(reply);
    if (it == m_pending.end()) return;

    PendingRequest& pr = it.value();
    if (pr.timer) { pr.timer->stop(); pr.timer->deleteLater(); pr.timer = nullptr; }

    QByteArray respBody = reply->readAll();
    int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();

    // ★ 详细日志：原始响应
    HTTP_LOG_INFO("RFID绑定查询响应 status=%d bodySize=%d body(前500)=%s",
        statusCode, respBody.size(), QString::fromUtf8(respBody.left(500)).toLocal8Bit().data());

    QMap<QString, QString> epcBarcodeMap;
    if (statusCode != 200)
    {
        HTTP_LOG_WARN("RFID绑定查询失败 HTTP状态异常 status=%d body=%s epcList大小=%d",
            statusCode, QString::fromUtf8(respBody).left(200).toLocal8Bit().data(),
            pr.sumLocation);
        // ★ 日志: 详细记录失败信息，方便排查是网络问题还是 RFID 服务问题
        QString rawBody = QString::fromUtf8(respBody);
        if (rawBody.length() > 200)
        {
            HTTP_LOG_WARN("RFID绑定查询失败 完整响应体(前500)=%s", 
                rawBody.left(500).toLocal8Bit().data());
        }
        m_pending.erase(it);
        emit rfidBindingResult(epcBarcodeMap);  // 空 map → 触发 onRfidBindingResult 空结果分支
        return;
    }

    // ★ JSON 解析
    QJsonParseError parseErr;
    QJsonDocument doc = QJsonDocument::fromJson(respBody, &parseErr);
    if (doc.isNull() || !doc.isObject())
    {
        QByteArray hexPreview = respBody.left(200).toHex(' ');
        HTTP_LOG_WARN("RFID绑定查询 JSON解析失败: %s offset=%d bodySize=%d hex=[%s]",
            parseErr.errorString().toLocal8Bit().data(), parseErr.offset,
            respBody.size(), hexPreview.constData());
        m_pending.erase(it);
        emit rfidBindingResult(epcBarcodeMap);
        return;
    }

    QJsonObject root = doc.object();

    // ★ 外层校验
    bool success = root["success"].toBool(false);
    int rstStatus = root["status"].toInt(0);
    QString msg = root["msg"].toString();
    HTTP_LOG_INFO("RFID绑定查询 外层校验 success=%d status=%d msg=%s",
        success, rstStatus, msg.toLocal8Bit().data());

    // RFID 响应格式: {"data":{"data":[{...}]},"status":200,"success":true}
    QJsonObject dataObj = root["data"].toObject();
    QJsonArray dataArr = dataObj["data"].toArray();
    HTTP_LOG_INFO("RFID绑定查询 内层data数组 size=%d", dataArr.size());

    int skipEmpty = 0;
    for (int i = 0; i < dataArr.size(); ++i)
    {
        QJsonObject item = dataArr[i].toObject();
        QString epc     = item["epc"].toString().trimmed();
        QString barcode = item["barcode"].toString().trimmed();
        QString tid     = item["tid"].toString().trimmed();
        QString uniqueCode = item["uniqueCode"].toString().trimmed();
        bool hasMetal    = item["productContainsMetal"].toBool(false);

        if (epc.isEmpty() || barcode.isEmpty())
        {
            skipEmpty++;
            HTTP_LOG_WARN("RFID绑定查询 跳过空字段[%d] epc=%s barcode=%s tid=%s uniqueCode=%s",
                i, epc.toLocal8Bit().data(), barcode.toLocal8Bit().data(),
                tid.toLocal8Bit().data(), uniqueCode.toLocal8Bit().data());
            continue;
        }

        epcBarcodeMap[epc] = barcode;
        HTTP_LOG_INFO("RFID绑定查询 解析[%d] epc=%s barcode=%s tid=%s uniqueCode=%s metal=%d",
            i, epc.toLocal8Bit().data(), barcode.toLocal8Bit().data(),
            tid.toLocal8Bit().data(), uniqueCode.toLocal8Bit().data(), hasMetal);
    }

    HTTP_LOG_INFO("RFID绑定查询完成 查询数=%d 匹配=%d 跳过空=%d status=%d",
        pr.sumLocation, epcBarcodeMap.size(), skipEmpty, statusCode);

    m_pending.erase(it);
    emit rfidBindingResult(epcBarcodeMap);
}