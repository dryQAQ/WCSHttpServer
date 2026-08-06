﻿﻿﻿#include "OutboxManager.h"
#include "SortingDatabase.h"
#include "define.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QDebug>

OutboxManager::OutboxManager(SortingDatabase* db, QObject* parent)
    : QObject(parent)
    , m_pDb(db)
{
    m_pollTimer = new QTimer(this);
    connect(m_pollTimer, &QTimer::timeout, this, &OutboxManager::onPollTimer);
}

OutboxManager::~OutboxManager()
{
    stop();
}

void OutboxManager::start()
{
    if (m_bRunning) return;
    m_bRunning = true;
    m_pollTimer->start(m_pollIntervalSec * 1000);
    qDebug() << "[Outbox] 出站管理器已启动 pollInterval=" << m_pollIntervalSec << "s";
}

void OutboxManager::stop()
{
    if (!m_bRunning) return;
    m_bRunning = false;
    m_pollTimer->stop();
    qDebug() << "[Outbox] 出站管理器已停止";
}

// ============================================================================
// 入队
// ============================================================================

QString OutboxManager::enqueueFullbox(const QString& orderCode, const QString& boxcode, const QJsonObject& payload)
{
    if (!m_pDb) return QString();

    QString msgId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString payloadStr = QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));

    // 首次重试时间 = 现在 + 重试间隔
    QString nextRetry = QDateTime::currentDateTime().addSecs(m_retryIntervalSec).toString("yyyy-MM-dd HH:mm:ss");
    QString now = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");

    OutboxRecord msg;
    msg.msgId     = msgId;
    msg.orderCode = orderCode;
    msg.boxcode   = boxcode;
    msg.payload   = payloadStr;
    msg.nextRetry = nextRetry;
    msg.createdAt = now;

    if (!m_pDb->insertOutboxFullbox(msg)) {
        qWarning() << "[Outbox] 满箱回传入队失败 msgId=" << msgId;
        return QString();
    }

    qDebug() << "[Outbox] 满箱回传入队成功 msgId=" << msgId << "orderCode=" << orderCode << "boxcode=" << boxcode;

    // 首次尝试立即发送
    if (trySendFullbox(msgId, payloadStr)) {
        m_pDb->markOutboxFullboxSuccess(msgId);
        emit outboxStatusChanged(msgId, "success");
    }

    return msgId;
}

QString OutboxManager::enqueueEnd(const QString& orderCode, const QJsonObject& payload)
{
    if (!m_pDb) return QString();

    QString msgId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString payloadStr = QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));

    QString nextRetry = QDateTime::currentDateTime().addSecs(m_retryIntervalSec).toString("yyyy-MM-dd HH:mm:ss");
    QString now = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");

    OutboxRecord msg;
    msg.msgId     = msgId;
    msg.orderCode = orderCode;
    msg.payload   = payloadStr;
    msg.nextRetry = nextRetry;
    msg.createdAt = now;

    if (!m_pDb->insertOutboxEnd(msg)) {
        qWarning() << "[Outbox] 完结回传入队失败 msgId=" << msgId;
        return QString();
    }

    qDebug() << "[Outbox] 完结回传入队成功 msgId=" << msgId << "orderCode=" << orderCode;

    // 首次尝试立即发送
    if (trySendEnd(msgId, payloadStr)) {
        m_pDb->markOutboxEndSuccess(msgId);
        emit outboxStatusChanged(msgId, "success");
    }

    return msgId;
}

// ============================================================================
// 发送尝试
// ============================================================================

bool OutboxManager::trySendFullbox(const QString& msgId, const QString& payload)
{
    if (!m_fullboxSendCb) {
        qWarning() << "[Outbox] 满箱回传发送回调未注册 msgId=" << msgId;
        return false;
    }
    return m_fullboxSendCb(payload);
}

bool OutboxManager::trySendEnd(const QString& msgId, const QString& payload)
{
    if (!m_endSendCb) {
        qWarning() << "[Outbox] 完结回传发送回调未注册 msgId=" << msgId;
        return false;
    }
    return m_endSendCb(payload);
}

// ============================================================================
// 定时扫描
// ============================================================================

void OutboxManager::onPollTimer()
{
    if (!m_pDb) return;

    // ──── 扫描满箱回传待重试消息（H7）────
    QVector<OutboxRecord> pendingFullbox = m_pDb->getPendingOutboxFullbox(OUTBOX_POLL_BATCH_SIZE);
    for (const auto& msg : pendingFullbox) {
        if (msg.retryCount >= m_retryMax) {
            // 超过最大重试次数，标记为失败
            m_pDb->updateOutboxFullboxStatus(msg.msgId, "failed", "");
            emit outboxStatusChanged(msg.msgId, "failed");
            qWarning() << "[Outbox] 满箱回传重试耗尽 msgId=" << msg.msgId << "retryCount=" << msg.retryCount;
            continue;
        }

        // 尝试发送
        bool success = trySendFullbox(msg.msgId, msg.payload);
        if (success) {
            m_pDb->markOutboxFullboxSuccess(msg.msgId);
            emit outboxStatusChanged(msg.msgId, "success");
            qDebug() << "[Outbox] 满箱回传重试成功 msgId=" << msg.msgId << "retryCount=" << msg.retryCount;
        } else {
            // 更新下次重试时间
            QString nextRetry = QDateTime::currentDateTime().addSecs(m_retryIntervalSec).toString("yyyy-MM-dd HH:mm:ss");
            m_pDb->updateOutboxFullboxStatus(msg.msgId, "pending", nextRetry);
            qDebug() << "[Outbox] 满箱回传重试失败 msgId=" << msg.msgId << "retryCount=" << msg.retryCount + 1;
        }
    }

    // ──── 扫描完结回传待重试消息（H8）────
    QVector<OutboxRecord> pendingEnd = m_pDb->getPendingOutboxEnd(OUTBOX_POLL_BATCH_SIZE);
    for (const auto& msg : pendingEnd) {
        if (msg.retryCount >= m_retryMax) {
            // 超过最大重试次数，标记为失败
            m_pDb->updateOutboxEndStatus(msg.msgId, "failed", "");
            emit outboxStatusChanged(msg.msgId, "failed");
            qWarning() << "[Outbox] 完结回传重试耗尽 msgId=" << msg.msgId << "retryCount=" << msg.retryCount;
            continue;
        }

        // 尝试发送
        bool success = trySendEnd(msg.msgId, msg.payload);
        if (success) {
            m_pDb->markOutboxEndSuccess(msg.msgId);
            emit outboxStatusChanged(msg.msgId, "success");
            qDebug() << "[Outbox] 完结回传重试成功 msgId=" << msg.msgId << "retryCount=" << msg.retryCount;
        } else {
            // 更新下次重试时间
            QString nextRetry = QDateTime::currentDateTime().addSecs(m_retryIntervalSec).toString("yyyy-MM-dd HH:mm:ss");
            m_pDb->updateOutboxEndStatus(msg.msgId, "pending", nextRetry);
            qDebug() << "[Outbox] 完结回传重试失败 msgId=" << msg.msgId << "retryCount=" << msg.retryCount + 1;
        }
    }
}

// ============================================================================
// 人工重发
// ============================================================================

void OutboxManager::retryByOrderCode(const QString& orderCode)
{
    if (!m_pDb) return;
    qDebug() << "[Outbox] 人工重发 orderCode=" << orderCode;

    // 查询该波次下所有待重试的满箱回传出站消息（H7）
    QVector<OutboxRecord> messages = m_pDb->getOutboxByOrderCode(orderCode);
    if (messages.isEmpty()) {
        qDebug() << "[Outbox] 人工重发 无待重试消息 orderCode=" << orderCode;
        return;
    }

    int successCount = 0;
    int failCount = 0;
    for (const auto& msg : messages) {
        if (msg.retryCount >= m_retryMax) {
            m_pDb->updateOutboxFullboxStatus(msg.msgId, "failed", "");
            emit outboxStatusChanged(msg.msgId, "failed");
            failCount++;
            continue;
        }

        bool success = trySendFullbox(msg.msgId, msg.payload);
        if (success) {
            m_pDb->markOutboxFullboxSuccess(msg.msgId);
            emit outboxStatusChanged(msg.msgId, "success");
            successCount++;
        } else {
            QString nextRetry = QDateTime::currentDateTime().addSecs(m_retryIntervalSec).toString("yyyy-MM-dd HH:mm:ss");
            m_pDb->updateOutboxFullboxStatus(msg.msgId, "pending", nextRetry);
            failCount++;
        }
    }

    qDebug() << "[Outbox] 人工重发完成 orderCode=" << orderCode
             << "total=" << messages.size() << "success=" << successCount << "fail=" << failCount;
}

void OutboxManager::retryByMsgId(const QString& msgId)
{
    if (!m_pDb) return;
    qDebug() << "[Outbox] 人工重发 msgId=" << msgId;

    // 查询单条消息
    OutboxRecord msg = m_pDb->getOutboxFullboxByMsgId(msgId);
    if (msg.msgId.isEmpty()) {
        qWarning() << "[Outbox] 人工重发 消息不存在 msgId=" << msgId;
        return;
    }

    if (msg.retryCount >= m_retryMax) {
        m_pDb->updateOutboxFullboxStatus(msg.msgId, "failed", "");
        emit outboxStatusChanged(msg.msgId, "failed");
        qWarning() << "[Outbox] 人工重发 已达最大重试次数 msgId=" << msgId;
        return;
    }

    bool success = trySendFullbox(msg.msgId, msg.payload);
    if (success) {
        m_pDb->markOutboxFullboxSuccess(msg.msgId);
        emit outboxStatusChanged(msg.msgId, "success");
        qDebug() << "[Outbox] 人工重发成功 msgId=" << msgId;
    } else {
        QString nextRetry = QDateTime::currentDateTime().addSecs(m_retryIntervalSec).toString("yyyy-MM-dd HH:mm:ss");
        m_pDb->updateOutboxFullboxStatus(msg.msgId, "pending", nextRetry);
        qWarning() << "[Outbox] 人工重发失败 msgId=" << msgId;
    }
}