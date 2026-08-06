#pragma once
// ============================================================================
// OutboxManager.h — 出站消息管理器（T-S0-05）
//
// 职责：
//   ① 持久化满箱回传/完结回传出站消息到 SQLite（H7/H8）
//   ② 定时扫描待重试消息，自动调用 HttpClient 重发
//   ③ 提供人工重发 API（按 msgId/orderCode 手动补传）
//   ④ 状态管理：pending → retrying → success / failed
//
// 重试策略：
//   - 首次发送立即执行
//   - 失败后按 OUTBOX_RETRY_INTERVAL_SEC 间隔重试
//   - 超过 OUTBOX_RETRY_MAX_DEFAULT 次标记为 failed
//   - 每 OUTBOX_POLL_INTERVAL_SEC 扫描一次待重试消息
//
// 使用方式：
//   OutboxManager mgr(db, httpClient);
//   mgr.start();                                    // 启动定时扫描
//   mgr.enqueueFullbox(orderCode, boxcode, json);   // 入队满箱回传（H7）
//   mgr.enqueueEnd(orderCode, json);                // 入队完结回传（H8）
//   mgr.retryByOrderCode("WAVE001");                // 人工重发
// ============================================================================

#include <QObject>
#include <QTimer>
#include <QString>
#include <QJsonObject>
#include <QUuid>
#include <functional>

class SortingDatabase;
class HttpClient;

class OutboxManager : public QObject
{
    Q_OBJECT
public:
    // 回调类型：实际发送逻辑（由调用方注入，解耦 HttpClient）
    // 参数: payload(JSON字符串), 返回: success(bool)
    using SendCallback = std::function<bool(const QString& payload)>;

    explicit OutboxManager(SortingDatabase* db, QObject* parent = nullptr);
    ~OutboxManager();

    // ──── 配置 ────
    void setRetryMax(int n)    { m_retryMax = n; }
    void setRetryInterval(int sec) { m_retryIntervalSec = sec; }
    void setPollInterval(int sec)  { m_pollIntervalSec = sec; }

    // // ──── 发送回调注册（满箱回传/完结回传分别注册）────
    // 注册满箱回传发送回调（H7）（由 HttpServer 注入，调用 HttpClient::sendGenericFeedback）
    void setFullboxSendCallback(SendCallback cb) { m_fullboxSendCb = std::move(cb); }
    // 注册完结回传发送回调（H8）（由 HttpServer 注入，调用 HttpClient::sendWaveComplete）
    void setEndSendCallback(SendCallback cb) { m_endSendCb = std::move(cb); }

    // ──── 生命周期 ────
    void start();   // 启动定时扫描
    void stop();    // 停止定时扫描

    // ──── 入队（立即持久化 + 首次尝试发送）────
    // 满箱回传出站（H7）
    QString enqueueFullbox(const QString& orderCode, const QString& boxcode, const QJsonObject& payload);
    // 完结回传出站（H8）
    QString enqueueEnd(const QString& orderCode, const QJsonObject& payload);

    // ──── 人工重发 ────
    void retryByOrderCode(const QString& orderCode);  // 按波次号重发所有失败消息
    void retryByMsgId(const QString& msgId);          // 按消息 ID 重发单条

    // ──── 查询 ────
    bool isRunning() const { return m_bRunning; }

signals:
    // 出站消息状态变更通知
    void outboxStatusChanged(const QString& msgId, const QString& status);

private slots:
    void onPollTimer();  // 定时扫描待重试消息

private:
    // 尝试发送单条消息，返回是否成功
    bool trySendFullbox(const QString& msgId, const QString& payload);
    bool trySendEnd(const QString& msgId, const QString& payload);

    SortingDatabase* m_pDb = nullptr;

    // 发送回调（由上层注入，解耦 HttpClient）
    SendCallback m_fullboxSendCb;
    SendCallback m_endSendCb;

    // 配置
    int m_retryMax        = 10;   // 最大重试次数
    int m_retryIntervalSec = 30;  // 重试间隔（秒）
    int m_pollIntervalSec  = 5;   // 扫描间隔（秒）

    QTimer* m_pollTimer = nullptr;
    bool    m_bRunning  = false;
};