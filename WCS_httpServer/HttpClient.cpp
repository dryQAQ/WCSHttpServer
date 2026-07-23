#include "HttpClient.h"
#include "log_center.h"
#include "hlog1.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QEventLoop>
#include <QTimer>
#include <QNetworkRequest>

HttpClient::HttpClient(QObject* parent)
    : QObject(parent)
{
    m_pNetworkMgr = new QNetworkAccessManager(this);
}

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
    QByteArray postData = QJsonDocument(req).toJson(QJsonDocument::Compact);


    QUrl url(m_url);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json; charset=UTF-8");
    request.setRawHeader("AppKey", m_appkey.toUtf8());

    QTimer timer;
    timer.setSingleShot(true);
    timer.setInterval(m_timeoutMs);
    timer.start();

    QNetworkReply* reply = m_pNetworkMgr->post(request, postData);

    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    loop.exec();

    if (!reply->isFinished())
    {
        reply->abort();
        reply->deleteLater();
        WCS_INFO("[Report] 超时 orderCode=%s", orderCode.toLocal8Bit().data());
        emit reportResult(orderCode, false, QString());
        return -1;
    }

    QByteArray respBody = reply->readAll();
    int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();

    QJsonDocument doc = QJsonDocument::fromJson(respBody);
    bool success = doc.object()["success"].toBool(false);

    WCS_INFO("[Report] 回传 orderCode=%s status=%d success=%d",
        orderCode.toLocal8Bit().data(), statusCode, success);
    LogCenter::Instance()->wcs_run_log_warn(success,
        QString("[Report] orderCode=%1 success=%2 body=%3")
            .arg(orderCode).arg(success).arg(QString::fromUtf8(respBody).left(200)));

    emit reportResult(orderCode, success, QString::fromUtf8(respBody));
    return success ? 0 : -1;
}
