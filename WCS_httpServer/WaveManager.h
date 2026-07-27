#pragma once
// ============================================================================
// WaveManager.h — 波次生命周期管理 + 分拣状态跟踪
//
// 职责:
//   ① 维护波次状态机 (IDLE→RECEIVED→SORTING→COMPLETING→CLEANED)
//   ② 跟踪每个 inco 的分拣状态（已分拣/异常/处理中/重试次数）
//   ③ 判定波次完结条件（全部完成 或 超时）
//   ④ 提供查询接口供 UI 和内部 API 使用
//
// 状态机转换:
//   IDLE → RECEIVED → SORTING → COMPLETING → CLEANED → IDLE
//              ↓                              ↑
//         CANCELLED ──────────────────────────┘
//
// 并发安全:
//   - m_waveStatus: std::atomic<int>（跨线程读写无需加锁）
//   - 分拣状态集（m_setCodeSorted/Exception/Processing）: std::mutex m_lock 保护
//   - m_bReported: std::atomic<bool> + exchange() 防重复回传
// ============================================================================

#include <QObject>
#include <QSet>
#include <QMap>
#include <QString>
#include <QDateTime>
#include <atomic>
#include <mutex>
#include "DoubleBuffer.h"
#include "define.h"

// ──── 波次状态枚举 ────
enum WaveStatus
{
    WAVE_IDLE       = 0,  // 空闲，可接收新波次
    WAVE_RECEIVED   = 1,  // 已接收波次数据，等待分拣开始
    WAVE_SORTING    = 2,  // 分拣进行中
    WAVE_COMPLETING = 3,  // 回传中（正在向WMS发送完结通知）
    WAVE_CLEANED    = 4,  // 已完成，数据已清理
    WAVE_ERROR      = 5,  // 异常状态
};

// ──── 波次快照（供UI展示，只读数据拷贝）────
struct WaveSnapshot
{
    QString orderCode;         // 波次号
    int     orderQty    = 0;   // 波次总件数（WMS推送的orderQty字段）
    int     skuCount    = 0;   // WMS推送的SKU种类数（去重后）
    int     sortedCount = 0;   // 已分拣数量
    int     exceptionCount = 0;// 异常数量
    int     totalRecv   = 0;   // 接收到的inco总数（= sorted + exception + processing）
    int     waveStatus  = WAVE_IDLE;  // 当前状态
    QString statusText;        // 状态文本（中文）
    qint64  elapsedSec  = 0;   // 已耗时（秒）
    int     sumLocation = 0;   // 使用的格口总数（去重后，供WMS回传）

    static QString statusToString(int s)
    {
        switch (s) {
        case WAVE_IDLE:       return QString::fromUtf8("空闲");       // 空闲
        case WAVE_RECEIVED:   return QString::fromUtf8("已接收");     // 已接收
        case WAVE_SORTING:    return QString::fromUtf8("分拣中");     // 分拣中
        case WAVE_COMPLETING: return QString::fromUtf8("回传中");     // 回传中
        case WAVE_CLEANED:    return QString::fromUtf8("已完成");     // 已完成
        case WAVE_ERROR:      return QString::fromUtf8("异常");       // 异常
        default:              return QString::fromUtf8("未知");       // 未知
        }
    }
};

class WaveManager : public QObject
{
    Q_OBJECT
public:
    explicit WaveManager(GridBuffer* pBuffer, QObject* parent = nullptr);

    // ──── 格口查询（无锁委托到 DoubleBuffer）────
    GridEntry getGrid(const QString& code) const { return m_pBuffer->get(code); }
    bool      contains(const QString& code) const { return m_pBuffer->contains(code); }
    int       gridCount() const { return m_pBuffer->size(); }

    // 获取波次中所有条码列表（快照诊断用）
    QStringList allCodes() const
    {
        QStringList codes;
        const QMap<QString, GridEntry>* pMap = m_pBuffer->activeMap();
        if (pMap) {
            for (auto it = pMap->constBegin(); it != pMap->constEnd(); ++it)
                codes.append(it.key());
        }
        return codes;
    }

    // ──── 波次数据设置（ParseWorker解析完成后调用）────
    void setWaveData(const QString& orderCode, int orderQty, int skuCount);
    void setRecvSet(const QSet<QString>& set);  // 设置接收到的完整inco集合

    // ──── 分拣状态（线程安全，内部加锁）────
    void markSorted(const QString& code);       // 标记已分拣
    void markException(const QString& code);    // 标记异常
    bool isSorted(const QString& code) const;   // 是否已分拣
    bool isException(const QString& code) const;// 是否异常
    int  retryCount(const QString& code) const; // 获取重试次数

    // ──── 波次判定 ────
    bool isWaveComplete() const;                // 检查波次是否完成（无锁读）
    bool checkWaveCompleteLocked() const;       // 检查波次是否完成（需持有m_lock）

    // ──── 状态查询 ────
    WaveSnapshot snapshot() const;              // 获取波次快照（供UI）
    int status() const { return m_waveStatus.load(); }
    bool setState(int newStatus);               // 状态转换（带合法性校验）
    QString orderCode() const { return m_orderCode; }
    int     totalRecv() const;
    int     sorted() const;
    int     exception() const;
    int     sumLocation() const;                // 去重格口总数（供WMS回传的sumLocation字段）

    // ──── 管理 ────
    void clearWave();                           // 清理当前波次数据，状态→CLEANED
    void setMaxRetry(int n) { m_maxRetry = n; }
    void setWaveTimeoutMin(int m) { m_waveTimeoutMin = m; }

signals:
    void waveReceived(const QString& orderCode, int skuCount);
    void waveSortingStarted(const QString& orderCode);     // 首次分拣时触发
    void waveReadyToReport(const QString& orderCode);      // 波次可回传（全部完成或超时）
    void waveStatusChanged(int newStatus);
    void codeMarked(const QString& code, bool sorted);     // sorted=true=分拣完成, false=异常

private:
    // ──── 数据源 ────
    GridBuffer*     m_pBuffer;               // DoubleBuffer 格口映射（只读，无需锁）

    // ──── 波次基本信息 ────
    QString         m_orderCode;             // 当前波次号
    int             m_orderQty         = 0;  // 波次总件数
    std::atomic<int> m_waveStatus{WAVE_IDLE};// 波次状态（原子操作，跨线程安全）
    QDateTime       m_waveStartTime;         // 波次开始时间
    bool            m_bSortingStarted  = false;  // 是否已开始分拣（防止重复发射 waveSortingStarted）
    int             m_maxRetry         = WAVE_MAX_RETRY;
    int             m_waveTimeoutMin   = WAVE_TIMEOUT_MIN_DEFAULT;

    // ──── 分拣状态集（由 m_lock 保护）────
    QSet<QString>      m_setCodeRecv;        // 波次中包含的所有 inco
    QSet<QString>      m_setCodeSorted;      // 已成功分拣的 inco
    QSet<QString>      m_setCodeException;   // 异常的 inco
    QSet<QString>      m_setCodeProcessing;  // 正在分拣中的 inco（防并发重复）
    QMap<QString, int> m_mapCodeRetry;       // 每个 inco 的已重试次数

    // ──── 并发控制 ────
    mutable std::mutex m_lock;               // 保护所有分拣状态集（QSet/QMap）
    std::atomic<bool>  m_bReported{false};   // 防重复回传：exchange(true)保证只有一个线程执行回传
};
