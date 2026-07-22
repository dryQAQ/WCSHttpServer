#pragma once
// ============================================================================
// TaskQueue.h — 线程安全的生产者-消费者队列
//
// 生产者: HP-Socket OnRequest 回调（多线程）
// 消费者: ParseWorker 解析线程（单线程）
// 削峰填谷，水位保护(maxPending=3)
// ============================================================================

#include <QQueue>
#include <QMutex>
#include <QWaitCondition>
#include <QByteArray>

struct WaveTask
{
    QByteArray rawBody;      // JSON原始字节流（完整拷贝）
    QString    clientIP;     // 来源IP
    qint64     recvTime;     // 接收时间戳

    WaveTask() : recvTime(0) {}
};

class TaskQueue
{
public:
    explicit TaskQueue(int maxPending = 3) : m_maxPending(maxPending) {}

    // 入队（非阻塞），返回false表示水位超限
    bool push(const WaveTask& task)
    {
        QMutexLocker locker(&m_mutex);
        if (m_queue.size() >= m_maxPending)
            return false;
        m_queue.enqueue(task);
        m_cond.wakeOne();
        return true;
    }

    // 出队（阻塞等待）
    WaveTask pop()
    {
        QMutexLocker locker(&m_mutex);
        while (m_queue.isEmpty())
            m_cond.wait(&m_mutex);
        return m_queue.dequeue();
    }

    // 非阻塞出队，队列空返回空task
    WaveTask tryPop()
    {
        QMutexLocker locker(&m_mutex);
        if (m_queue.isEmpty())
            return WaveTask();
        return m_queue.dequeue();
    }

    int size()
    {
        QMutexLocker locker(&m_mutex);
        return m_queue.size();
    }

    void clear()
    {
        QMutexLocker locker(&m_mutex);
        m_queue.clear();
    }

    // 唤醒阻塞的pop（用于停止线程）
    void wakeAll()
    {
        QMutexLocker locker(&m_mutex);
        m_cond.wakeAll();
    }

private:
    QQueue<WaveTask> m_queue;
    QMutex           m_mutex;
    QWaitCondition   m_cond;
    int              m_maxPending;
};
