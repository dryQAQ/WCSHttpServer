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
    void setUrl(const QString& url)  { m_url = url; }        // WMS满箱回传接口地址（H7）
    void setEndUrl(const QString& url) { m_endUrl = url; }   // WMS完结回传接口地址（H8）
    void setAppkey(const QString& k) { m_appkey = k; }        // WMS认证AppKey（HTTP Header）
    void setTimeout(int ms)          { m_timeoutMs = ms; }    // 回传超时(ms)，默认HTTP_TIMEOUT_MS=3000
    void setRfidQueryUrl(const QString& url) { m_rfidQueryUrl = url; }  // ★ RFID SKU-EPC 绑定查询 URL
    void setRfidAppkey(const QString& k)     { m_rfidAppkey = k; }      // ★ 2026-09-05 RFID 查询鉴权 key（空=不发送）

    // ──── 业务 ────
    // 回传波次完结通知到WMS（异步，不阻塞主线程）
    // orderCode: 波次号
    // sumLocation: 落格分拣总件数（告知WMS分拣了多少件）
    void sendWaveComplete(const QString& orderCode, int sumLocation);

    // ★ 锁格回传：发送预构建 JSON 到 WMS（异步，使用 H7 URL）
    void sendGenericFeedback(const QJsonObject& json, const QString& context = QString());

    // ★ 完结回传：发送预构建 JSON 到 WMS（异步，使用 H8 URL）
    void sendEndFeedback(const QJsonObject& json, const QString& context = QString());

    // ★ SKU-EPC 绑定查询：向 RFID 查询 EPC→barcode 映射（异步批量）
    //    epcList: EPC 列表，一次查询多个
    //    完成后通过 rfidBindingResult 信号返回结果
    void queryRfidBinding(const QStringList& epcList);

signals:
    // 回传结果通知
    // orderCode: 波次号
    // success:   回传是否成功（HTTP 200 + body.success==true）
    // body:      WMS返回的原始响应体（失败时为空）
    void reportResult(const QString& orderCode, bool success, const QString& body);

    // ★ SKU-EPC 绑定查询结果（epc → barcode）
    //    epcBarcodeMap: EPC → barcode 映射，查询失败返回空 Map
    void rfidBindingResult(const QMap<QString, QString>& epcBarcodeMap);

private slots:
    void onReplyFinished();     // WMS回传 QNetworkReply::finished 回调
    void onReplyTimeout();      // WMS回传 QTimer::timeout 回调（超时保护）
    void onRfidBindingReplyFinished();  // ★ RFID 绑定查询 QNetworkReply::finished 回调

private:
    // ★ 必须为成员变量：QNetworkAccessManager 作为 parent 管理 QNetworkReply 和 QTimer，
    //    避免函数返回后子对象被提前析构导致信号触发时崩溃
    QNetworkAccessManager* m_pNetworkMgr;

    // ──── 配置成员 ────
    QString m_url;              // WMS满箱回传目标URL（H7）
    QString m_endUrl;           // WMS完结回传目标URL（H8）
    QString m_appkey;           // WMS认证AppKey（放入HTTP Header: AppKey=xxx）
    int     m_timeoutMs = HTTP_TIMEOUT_MS;  // 超时时间(ms)，默认3000
    QString m_rfidQueryUrl;     // ★ RFID SKU-EPC 绑定查询 URL（查询 EPC→barcode 映射）
    QString m_rfidAppkey;       // ★ 2026-09-05 RFID 查询鉴权 key（HTTP Header: Authorization: APP_KEYS <key>；空=不发送）

    // ──── 请求追踪 ────
    // 跟踪进行中的异步请求，用于超时处理和响应匹配
    struct PendingRequest {
        QNetworkReply* reply;       // Qt网络回复对象（finished信号宿主）
        QTimer*        timer;       // 超时定时器（单次触发）
        QString        orderCode;   // 波次号（用于回调时标识是哪个波次）
        int            sumLocation;  // 格口总数（用于日志）
        QString        url;         // ★ 2026-09-04：请求目标URL（网络失败/超时日志提示用）
    };
    // ★ reply → PendingRequest 映射：
    //    当 onReplyFinished 或 onReplyTimeout 触发时，通过 sender() 获取 reply/timer，
    //    再从此映射中查找对应的 PendingRequest 以获取波次信息
    QMap<QNetworkReply*, PendingRequest> m_pending;
};
