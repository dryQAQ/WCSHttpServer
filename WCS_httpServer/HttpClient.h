#pragma once
// ============================================================================
// HttpClient.h — HTTP回传客户端（异步模式）
//
// 使用 QNetworkAccessManager 作为成员变量（遵循 project_memory 约束：
//   栈变量会导致 QTimer 子对象提前析构，造成崩溃）
// 异步模式：QNetworkReply::finished 信号驱动，QTimer 超时保护
// ============================================================================

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <QMap>
#include <QString>
#include <QJsonArray>

#include "define.h"

class HttpClient : public QObject
{
    Q_OBJECT
public:
    explicit HttpClient(QObject* parent = nullptr);
    ~HttpClient();

    // ──── 配置 ────
    void setUrl(const QString& url)  { m_url = url; }        // WMS回传接口地址
    void setAppkey(const QString& k) { m_appkey = k; }        // WMS认证AppKey（HTTP Header）
    void setTimeout(int ms)          { m_timeoutMs = ms; }    // 回传超时(ms)，默认HTTP_TIMEOUT_MS=3000
    void setRfidUrl(const QString& url) { m_rfidUrl = url; }  // RFID查询接口地址
    void setRfidTimeout(int ms)      { m_rfidTimeoutMs = ms; }// RFID查询超时(ms)，默认RFID_TIMEOUT_MS=2000

    // ──── 业务 ────
    // 回传波次完结通知到WMS（异步，不阻塞主线程）
    // orderCode: 波次号
    // sumLocation: 使用的格口总数（去重后）
    void sendWaveComplete(const QString& orderCode, int sumLocation);

    // ★ 锁格回传：发送预构建 JSON 到 WMS（异步）
    void sendGenericFeedback(const QJsonObject& json, const QString& context = QString());

    // ★ RFID查询：向RFID服务查询EPC对应的SKU/条码信息（异步）
    // epcList: EPC列表（每批最多 RFID_MAX_BATCH_SIZE 个）
    // context: 请求上下文（用于回调时标识，如波次号）
    void queryRfid(const QJsonArray& epcList, const QString& context = QString());

signals:
    // 回传结果通知
    // orderCode: 波次号
    // success:   回传是否成功（HTTP 200 + body.success==true）
    // body:      WMS返回的原始响应体（失败时为空）
    void reportResult(const QString& orderCode, bool success, const QString& body);

    // ★ RFID查询结果
    // result: RFID服务返回的原始JSON（包含data.data数组）
    // context: 请求上下文（如波次号）
    void rfidQueryResult(const QJsonObject& result, const QString& context);

private slots:
    void onReplyFinished();     // WMS回传 QNetworkReply::finished 回调
    void onReplyTimeout();      // WMS回传 QTimer::timeout 回调（超时保护）
    void onRfidReplyFinished(); // RFID查询 QNetworkReply::finished 回调
    void onRfidReplyTimeout();  // RFID查询 QTimer::timeout 回调（超时保护）

private:
    // ★ 必须为成员变量：QNetworkAccessManager 作为 parent 管理 QNetworkReply 和 QTimer，
    //    避免函数返回后子对象被提前析构导致信号触发时崩溃
    QNetworkAccessManager* m_pNetworkMgr;

    // ──── 配置成员 ────
    QString m_url;              // WMS回传目标URL
    QString m_appkey;           // WMS认证AppKey（放入HTTP Header: AppKey=xxx）
    int     m_timeoutMs = HTTP_TIMEOUT_MS;  // 超时时间(ms)，默认3000
    QString m_rfidUrl;          // RFID查询接口URL
    int     m_rfidTimeoutMs = RFID_TIMEOUT_MS; // RFID查询超时(ms)

    // ──── 请求追踪 ────
    // 跟踪进行中的异步请求，用于超时处理和响应匹配
    struct PendingRequest {
        QNetworkReply* reply;       // Qt网络回复对象（finished信号宿主）
        QTimer*        timer;       // 超时定时器（单次触发）
        QString        orderCode;   // 波次号（用于回调时标识是哪个波次）
        int            sumLocation;  // 格口总数（用于日志）
    };
    // ★ reply → PendingRequest 映射：
    //    当 onReplyFinished 或 onReplyTimeout 触发时，通过 sender() 获取 reply/timer，
    //    再从此映射中查找对应的 PendingRequest 以获取波次信息
    QMap<QNetworkReply*, PendingRequest> m_pending;

    // ★ RFID 查询请求追踪（独立于 WMS 回传）
    struct RfidPendingRequest {
        QNetworkReply* reply;
        QTimer*        timer;
        QString        context;     // 请求上下文（如波次号）
        QJsonArray     epcList;     // ★ 原始请求EPC列表（重试用）
        int            retryCount = 0; // ★ 已重试次数
    };
    QMap<QNetworkReply*, RfidPendingRequest> m_rfidPending;
    int m_rfidRetryMax = RFID_RETRY_MAX; // ★ RFID 最大重试次数
};
