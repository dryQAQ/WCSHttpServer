#include "HttpClient.h"
#include "EpcCode.h"     // ★ 2026-09-15 绑定查询响应侧 EPC 归一（与推送侧同源）
#include "LogService.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QNetworkRequest>
#include <QUrlQuery>

HttpClient::HttpClient(QObject* parent)
    : QObject(parent)
{
    m_pNetworkMgr = new QNetworkAccessManager(this);
}

HttpClient::~HttpClient()
{
    // ★ 2026-09-04 崩溃定位日志
    HTTP_LOG_INFO("[析构] HttpClient 开始销毁 pending=%d", m_pending.size());
    // 取消所有进行中的 WMS 回传请求
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it)
    {
        if (it->timer)  { it->timer->stop(); delete it->timer; }
        if (it->reply)
        {
            // ★ 2026-09-02 防崩溃：abort() 可能同步触发 finished → onReplyFinished /
            //   onRfidBindingReplyFinished 执行 m_pending.erase()，导致本循环迭代器失效（UB/崩溃）。
            //   先断开 finished 连接再 abort（与 onReplyTimeout 中的处理一致）
            disconnect(it->reply, &QNetworkReply::finished, this, &HttpClient::onReplyFinished);
            disconnect(it->reply, &QNetworkReply::finished, this, &HttpClient::onRfidBindingReplyFinished);
            it->reply->abort();
            it->reply->deleteLater();
        }
    }
    m_pending.clear();
    HTTP_LOG_INFO("[析构] HttpClient 销毁完成");
}

// ──── 出站报文归档辅助 ────
// ★ 2026-09-06：统一写入 ./log/Run/run.log（不再单独建 send.log 文件，现场反馈独立文件无法写入/不便查看）
// body 去除换行后整行记录，便于单行检索；检索 "[原始报文]" 即可同时定位 发送/响应
static void archiveSend(const char* kind, const QString& url, const QString& appkey,
                        const QString& context, const QByteArray& body)
{
    QString text = QString::fromUtf8(body);
    text.replace('\r', ' ').replace('\n', ' ');
    LOG_INFO("[原始报文] [WMS出站发送] 类型=%s context=%s url=%s header=AppKey:%s len=%d body=%s",
             kind, context.toLocal8Bit().constData(), url.toLocal8Bit().constData(),
             appkey.toLocal8Bit().constData(), body.size(), text.toLocal8Bit().constData());
}

// ★ 2026-09-06：组装带 WMS 网关参数的完整回传 URL
//   base（XML 配置，不含参数） + 追加 appkey/method 查询参数
//   ★ 2026-09-07：若 URL 已自带 appkey/method（某些网关要求整串 URL），
//   则不重复追加（以 URL 内为准）；未带时用配置项补上。
//   （appkey=xxx 来自 <appkey>/<appkeyTest>，method=xxx 来自 <feedbackMethod>/<feedbackEndMethod>）
QUrl HttpClient::buildFeedbackUrl(const QString& baseUrl, const QString& method) const
{
    QUrl url(baseUrl);
    QUrlQuery q(url);
    if (!q.hasQueryItem("appkey") && !m_appkey.isEmpty())
        q.addQueryItem("appkey", m_appkey);
    if (!q.hasQueryItem("method") && !method.isEmpty())
        q.addQueryItem("method", method);
    url.setQuery(q);
    return url;
}

static void archiveResp(const QString& url, const QString& context,
                        int status, bool success, const QByteArray& body)
{
    QString text = QString::fromUtf8(body);
    text.replace('\r', ' ').replace('\n', ' ');
    LOG_INFO("[原始报文] [WMS出站响应] context=%s url=%s status=%d success=%d len=%d body=%s",
             context.toLocal8Bit().constData(), url.toLocal8Bit().constData(),
             status, success ? 1 : 0, body.size(), text.toLocal8Bit().constData());
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

    // ★ 2026-09-06：完整 URL = base + ?appkey=..&method=..（网关要求）
    const QUrl url = buildFeedbackUrl(m_url, m_feedbackMethod);
    const QString finalUrl = url.toString();
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
    pr.url         = finalUrl;   // ★ 2026-09-04：记录目标URL（失败/超时日志提示用）
    archiveSend("波次完成回传", finalUrl, m_appkey, orderCode, postData);   // ★ 2026-09-06 发送报文归档
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
    // ★ 2026-09-04：日志与 UI 明确提示"回传地址不通/网络失败"，带 URL 与错误描述
    if (statusCode == 0) {
        QString errText = reply->errorString();
        HTTP_LOG_ERROR("回传网络层失败（地址不通或网络异常） url=%s orderCode=%s error=%d errorString=%s",
            pr.url.toLocal8Bit().data(),
            pr.orderCode.toLocal8Bit().data(),
            reply->error(),
            errText.toLocal8Bit().data());
        LogCenter::Instance()->wcs_run_log_warn(false,
            QString("[回传] 网络失败（请检查回传地址是否可达） url=%1 orderCode=%2 error=%3")
                .arg(pr.url).arg(pr.orderCode).arg(errText));
        SEND_ERROR("[响应] 网络层失败(未收到HTTP响应) context=%s url=%s error=%d desc=%s",
            pr.orderCode.toLocal8Bit().constData(), pr.url.toLocal8Bit().constData(),
            (int)reply->error(), errText.toLocal8Bit().constData());
    }

    reply->deleteLater();

    QJsonDocument doc = QJsonDocument::fromJson(respBody);
    bool success = doc.object()["success"].toBool(false);

    // ★ 2026-09-06：响应归档（完整 body，统一写入 run.log，见 archiveResp）
    archiveResp(pr.url, pr.orderCode, statusCode, success, respBody);

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
            // ★ 2026-09-04：超时日志明确提示"可能地址不通/响应慢"，带 URL
            HTTP_LOG_WARN("回传超时（可能地址不通或响应过慢） url=%s orderCode=%s timeout=%dms",
                pr.url.toLocal8Bit().data(),
                pr.orderCode.toLocal8Bit().data(), m_timeoutMs);
            LogCenter::Instance()->wcs_run_log_warn(false,
                QString("[回传] 超时（可能地址不通或响应过慢） url=%1 orderCode=%2 timeout=%3ms")
                    .arg(pr.url).arg(pr.orderCode).arg(m_timeoutMs));
            SEND_WARN("[响应] 超时(无响应) context=%s url=%s timeout=%dms",
                pr.orderCode.toLocal8Bit().constData(), pr.url.toLocal8Bit().constData(), m_timeoutMs);

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

    // ★ 2026-09-06：完整 URL = base + ?appkey=..&method=..（method=满箱/锁格/波次完成类）
    const QUrl url = buildFeedbackUrl(m_url, m_feedbackMethod);
    const QString finalUrl = url.toString();
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
    pr.url         = finalUrl;   // ★ 2026-09-04：记录目标URL
    m_pending.insert(reply, pr);

    connect(reply, &QNetworkReply::finished, this, &HttpClient::onReplyFinished);
    connect(timer, &QTimer::timeout, this, &HttpClient::onReplyTimeout);
    timer->start();

    // ★ 2026-09-06：发送报文归档（完整 body，统一写入 run.log，见 archiveSend）
    //   类型文案按 context 区分，方便检索：fullbox_*=满箱回传(H7)、lockGrid_*=锁格、其它=波次完成
    QByteArray kindStr;
    if (context.startsWith("fullbox_") || context.startsWith("resendFullbox_"))
        kindStr = QByteArray("满箱回传(H7)");
    else if (context.startsWith("lockGrid_"))
        kindStr = QByteArray("锁格回传");
    else
        kindStr = QByteArray("波次完成回传");
    archiveSend(kindStr.constData(), finalUrl, m_appkey, context, postData);
    HTTP_LOG_INFO("回传发送 kind=%s context=%s len=%d", kindStr.constData(), context.toLocal8Bit().data(), postData.size());
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

    // ── 步骤3: 打印请求参数（完整URL、AppKey、Payload、超时） ──
    // ★ 2026-09-06：完整 URL = base + ?appkey=..&method=..（method=完结回传类）
    const QUrl url = buildFeedbackUrl(m_endUrl, m_endFeedbackMethod);
    const QString finalUrl = url.toString();
    HTTP_LOG_INFO("完结回传（H8）请求参数: URL=%s AppKey=%s timeout=%dms",
        finalUrl.toLocal8Bit().data(), m_appkey.toLocal8Bit().data(), m_timeoutMs);
    HTTP_LOG_INFO("完结回传（H8）请求体 context=%s len=%d payload=%s",
        context.toLocal8Bit().data(), postData.size(), payloadPreview.toLocal8Bit().data());
    // ★ 2026-09-06：发送报文归档（完整 body，统一写入 run.log，见 archiveSend）
    archiveSend("完结回传(H8)", finalUrl, m_appkey, context, postData);

    // ── 步骤4: 构建 HTTP 请求 ──
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
    pr.url         = finalUrl;   // ★ 2026-09-04：记录 H8 完结回传目标URL
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
    // ★ 2026-09-05：RFID 查询接口鉴权 —— Authorization 头完整值（形如 "APP_KEYS xxx"）由 XML 配置
    //   （rfidAppkey）直接提供，代码不做拼凑；空则不发送，兼容本地 mock
    if (!m_rfidAppkey.isEmpty())
        request.setRawHeader("Authorization", m_rfidAppkey.toUtf8());

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
    pr.url         = m_rfidQueryUrl;   // ★ 2026-09-04：记录目标URL
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
        HTTP_LOG_WARN("RFID绑定查询失败 HTTP状态异常 url=%s status=%d body=%s epcList大小=%d",
            pr.url.toLocal8Bit().data(),
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

        // ★ 2026-09-15 响应侧 EPC 归一（与推送侧同源同参数，见 EpcCode.h）：
        //   缓存键来自推送侧"识别归一后"的 EPC，若响应回的是整串（如 32 位），
        //   直接入库会变成"缓存里有两个键、按归一 EPC 查不到绑定" → 该件永远查不到 SKU。
        //   归一后入缓存即两边口径一致；未识别（形态不符）则原样保留，不臆造。
        const QString epcRawResp = epc;
        if (m_epcTruncateLen >= 2)
        {
            QString cut;
            if (EpcCode::extract(epc, m_epcTruncateLen, cut) && cut != epc)
            {
                HTTP_LOG_WARN("RFID绑定查询响应 EPC已识别归一 原文(%d位)=%s → EPC(%d位)=%s",
                    epc.size(), epc.toLocal8Bit().data(), cut.size(), cut.toLocal8Bit().data());
                epc = cut;
            }
        }

        epcBarcodeMap[epc] = barcode;
        const QByteArray respRawNote = (epc != epcRawResp)
            ? QString("（响应原文 %1）").arg(epcRawResp).toLocal8Bit()
            : QByteArray();
        HTTP_LOG_INFO("RFID绑定查询 解析[%d] epc=%s barcode=%s tid=%s uniqueCode=%s metal=%d%s",
            i, epc.toLocal8Bit().data(), barcode.toLocal8Bit().data(),
            tid.toLocal8Bit().data(), uniqueCode.toLocal8Bit().data(), hasMetal,
            respRawNote.constData());
    }

    HTTP_LOG_INFO("RFID绑定查询完成 查询数=%d 匹配=%d 跳过空=%d status=%d",
        pr.sumLocation, epcBarcodeMap.size(), skipEmpty, statusCode);

    m_pending.erase(it);
    emit rfidBindingResult(epcBarcodeMap);
}