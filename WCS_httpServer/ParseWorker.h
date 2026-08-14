#pragma once
// ============================================================================
// ParseWorker.h — 后台JSON解析线程
//
// 职责：
//   从 TaskQueue 阻塞等待取任务 → 解析 WMS 推送的大 JSON
//   → 构建 QMap<inco, GridEntry> → 原子交换到 DoubleBuffer
//
// 低优先级运行（QThread::LowPriority），绝不抢占 PLC/扫描线程的 CPU 时间
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
    // 波次解析完成信号
    // orderCode:  波次号
    // skuCount:   WMS推送的SKU种类数（去重后）
    // orderQty:   波次总件数
    // elapsedMs:  解析耗时(ms)
    // recvSet:    波次包含的所有inco集合（用于波次完结判定）
    // epcList:    波次中包含的所有EPC列表（用于RFID查询SKU绑定）
    void waveParsed(const QString& orderCode, int skuCount, int orderQty,
                    qint64 elapsedMs, const QSet<QString>& recvSet,
                    const QStringList& epcList);
    // 解析异常信号
    void parseError(const QString& errorMsg);
    // ★ 格口号越界异常信号（不阻塞波次，仅记录异常便于核查和重传）
    // orderCode:  波次号
    // epc:        EPC 编码
    // gridNum:    越界的格口号
    // reason:     异常原因
    void parseException(const QString& orderCode, const QString& epc,
                        const QString& gridNum, const QString& reason);

protected:
    void run() override;

private:
    TaskQueue*   m_pQueue;           // 任务队列（输入源）
    GridBuffer*  m_pBuffer;           // DoubleBuffer（输出目标）
    bool         m_bRunning = true;   // 线程运行标志，stop()时置false
};
