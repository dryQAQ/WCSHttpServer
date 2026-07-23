#pragma once
// ============================================================================
// ParseWorker.h — 后台JSON解析线程
// 从TaskQueue取任务 → 解析大JSON → 构建新Map → 原子交换到DoubleBuffer
// 低优先级运行，绝不抢占PLC/扫描线程
// ============================================================================

#include <QThread>
#include "TaskQueue.h"
#include "DoubleBuffer.h"

class ParseWorker : public QThread
{
    Q_OBJECT
public:
    ParseWorker(TaskQueue* pQueue, GridBuffer* pBuffer, QObject* parent = nullptr);
    void stop();

signals:
    void waveParsed(const QString& orderCode, int skuCount, int orderQty, qint64 elapsedMs, const QSet<QString>& recvSet);
    void parseError(const QString& errorMsg);

protected:
    void run() override;

private:
    TaskQueue*   m_pQueue;
    GridBuffer*  m_pBuffer;
    bool         m_bRunning = true;
};
