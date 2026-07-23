#pragma once
// ============================================================================
// HttpClient.h — HTTP回传客户端（使用 QNetworkAccessManager）
//
// 使用 QNetworkAccessManager 作为成员变量（遵循 project_memory 约束）
// 同步模式：QEventLoop + QTimer 超时
// ============================================================================

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QString>

class HttpClient : public QObject
{
    Q_OBJECT
public:
    explicit HttpClient(QObject* parent = nullptr);
    ~HttpClient();

    void setUrl(const QString& url)  { m_url = url; }
    void setAppkey(const QString& k) { m_appkey = k; }
    void setTimeout(int ms)          { m_timeoutMs = ms; }

    // 回传波次完结（同步，2秒超时）
    int sendWaveComplete(const QString& orderCode, int sumLocation);

signals:
    void reportResult(const QString& orderCode, bool success, const QString& body);

private:
    QNetworkAccessManager* m_pNetworkMgr;
    QString m_url;
    QString m_appkey;
    int     m_timeoutMs = 2000;
};
