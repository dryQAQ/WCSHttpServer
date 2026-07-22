#pragma once
// ============================================================================
// HttpClient.h — HP-Socket HTTP Client（异步回传WMS）
// 异步回调模式，不阻塞调用线程，超时/失败入重试队列
// ============================================================================

#include <QObject>
#include <QTimer>
#include "HPSocket.h"

class WaveManager;

class HttpClient : public QObject, public IHttpClientHandler
{
    Q_OBJECT
public:
    explicit HttpClient(QObject* parent = nullptr);
    ~HttpClient();

    void setUrl(const QString& url)  { m_url = url; }
    void setAppkey(const QString& k) { m_appkey = k; }
    void setTimeout(int ms)          { m_timeoutMs = ms; }

    // 异步回传波次完结
    int sendWaveComplete(const QString& orderCode, int sumLocation);

signals:
    void reportResult(const QString& orderCode, bool success, const QString& body);

protected:
    EnHandleResult OnResponse(IHttpClient* pSender, CHttpResponse* pResp, int iSeq) override;
    EnHandleResult OnError(IHttpClient* pSender, int iErrorCode, int iSeq) override;

private:
    QString m_url;
    QString m_appkey;
    int     m_timeoutMs = 2000;
    QMap<int, QString> m_seqOrderCode;  // seq → orderCode
};
