#pragma once
// ============================================================================
// ParseWorker.h — 后台JSON解析线程
//
// 职责：
//   从 TaskQueue 阻塞等待取任务 → 解析 WMS 推送的大 JSON
//   → 构建 QMap<SKU, GridEntry> → 原子交换到 DoubleBuffer
//
// 低优先级运行（QThread::LowPriority），绝不抢占 PLC/扫描线程的 CPU 时间
// ============================================================================

#include <QThread>
#include "TaskQueue.h"
#include "DoubleBuffer.h"
#include "PlanAllocTable.h"   // ★ 2026-09-14 计划分配表编译入参（PlanGridInput）

// ──── ★ 2026-09-14 跨线程信号参数必须注册为元类型 ────
//   waveParsed 是 Qt::QueuedConnection 的跨线程信号（ParseWorker 线程 → 主线程），
//   Qt 在投递时必须按**类型名字面量**构造参数副本；**只要有一个参数类型未注册，
//   整条排队调用会被直接丢弃**（仅打一条 "QObject::connect: Cannot queue arguments
//   of type ..." 警告），表现为"波次解析完成但主线程回调完全不执行"
//   （波次不注册、不建分配表、明细不落库推进）。本项目原本就漏注册了
//   QSet<QString>（recvSet 参数），本次补上，消除这一整类静默失败。
//   注册动作在 ParseWorker 构造函数内完成（见 ParseWorker.cpp），
//   这里不用 Q_DECLARE_METATYPE：该宏遇模板内逗号会被预处理器拆成多参而报错，
//   且 qRegisterMetaType 的按名注册已足够（PlcManager 亦采用同样做法）。

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
    // recvSet:    波次中包含的所有SKU集合（用于波次完结判定）
    // rawBody:    ★ 2026-09-07 H4 原始报文（当前波次执行中时排队/延迟执行用）
    // ★ 2026-09-14 计划分配表：
    //   skuPlans:  SKU → [(格口, 类型, 该格口计划件数)]（顺序无关，主线程编译时固化顺序）
    //   planSum:   Σ 各格口计划件数（与 orderQty 对照，供编译日志与开工预检核对）
    void waveParsed(const QString& orderCode, int skuCount, int orderQty,
                    qint64 elapsedMs, const QSet<QString>& recvSet,
                    const QByteArray& rawBody,
                    const QVector<QPair<QString, QVector<PlanGridInput>>>& skuPlans,
                    int planSum);
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
