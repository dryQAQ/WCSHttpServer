#include "HttpClient.h"
#include "WaveManager.h"
#include "log_center.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>

HttpClient::HttpClient(QObject* parent) : QObject(parent) {}
HttpClient::~HttpClient() {}

int HttpClient::sendWaveComplete(const QString& orderCode, int sumLocation)
{
    QJsonObject head;
    head["orderCode"]     = orderCode;
    head["orderType"]     = "02";
    head["sumLocation"]   = QString::number(sumLocation);
    head["operuserDate"]  = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    head["operuserCode"]  = "admin";
    head["operuserName"]  = "管理员";

    QJsonObject req;
    req["head"] = head;

    QByteArray body = QJsonDocument(req).toJson(QJsonDocument::Compact);

    IHttpClientPtr client(new IHttpClient(this));
    client->SetTimeout(m_timeoutMs);

    int seq = client->SendRequest(m_url.toUtf8().constData(), "POST",
                                   body.constData(), body.length());
    m_seqOrderCode[seq] = orderCode;

    WCS_INFO("[Report] 异步回传 orderCode=%s sumLocation=%d seq=%d",
        orderCode.toLocal8Bit().data(), sumLocation, seq);

    return seq;
}

EnHandleResult HttpClient::OnResponse(IHttpClient* pSender, CHttpResponse* pResp, int iSeq)
{
    QString body = QString::fromUtf8(pResp->GetBody(), pResp->GetBodyLength());
    int status = pResp->GetStatus();

    QString orderCode = m_seqOrderCode.take(iSeq);

    if (status == 200)
    {
        QJsonDocument doc = QJsonDocument::fromJson(body.toUtf8());
        bool success = doc.object()["success"].toBool(false);

        WCS_INFO("[Report] 响应 orderCode=%s success=%d", orderCode.toLocal8Bit().data(), success);
        LogCenter::Instance()->wcs_run_log_warn(true,
            QString("[Report] orderCode=%1 success=%2 body=%3")
                .arg(orderCode).arg(success).arg(body.left(200)));

        emit reportResult(orderCode, success, body);
    }
    else
    {
        LogCenter::Instance()->wcs_run_log_warn(false,
            QString("[Report] HTTP错误 orderCode=%1 status=%2").arg(orderCode).arg(status));
        emit reportResult(orderCode, false, body);
    }

    return HR_OK;
}

EnHandleResult HttpClient::OnError(IHttpClient* pSender, int iErrorCode, int iSeq)
{
    QString orderCode = m_seqOrderCode.take(iSeq);

    LogCenter::Instance()->wcs_run_log_warn(false,
        QString("[Report] 网络错误 orderCode=%1 error=%2").arg(orderCode).arg(iErrorCode));
    emit reportResult(orderCode, false, QString());
    return HR_OK;
}
