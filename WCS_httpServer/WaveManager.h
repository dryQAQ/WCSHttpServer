#pragma once
// ============================================================================
// WaveManager.h — 波次生命周期管理 + 分拣状态跟踪
//
// 职责:
//   ① 维护波次状态机 (IDLE→RECEIVED→SORTING→COMPLETING→CLEANED)
//   ② 跟踪每个 inco 的分拣状态（已分拣/异常/处理中/重试次数）
//   ③ 判定波次完结条件（全部完成 或 超时）
//   ④ 提供查询接口供 UI 和内部 API 使用
// ============================================================================

#include <QObject>
#include <QSet>
#include <QMap>
#include <QString>
#include <QDateTime>
#include <atomic>
#include <mutex>
#include "DoubleBuffer.h"

// ──── 波次状态枚举 ────
enum WaveStatus
{
    WAVE_IDLE       = 0,
    WAVE_RECEIVED   = 1,
    WAVE_SORTING    = 2,
    WAVE_COMPLETING = 3,
    WAVE_CLEANED    = 4,
    WAVE_ERROR      = 5,
};

// ──── 波次快照（供UI展示）────
struct WaveSnapshot
{
    QString orderCode;
    int     orderQty    = 0;
    int     skuCount    = 0;   // WMS推送的SKU种类数
    int     sortedCount = 0;
    int     exceptionCount = 0;
    int     totalRecv   = 0;   // 接收到的inco总数
    int     waveStatus  = WAVE_IDLE;
    QString statusText;
    qint64  elapsedSec  = 0;
    int     sumLocation = 0;

    static QString statusToString(int s)
    {
        switch (s) {
        case WAVE_IDLE:       return "空闲";
        case WAVE_RECEIVED:   return "已接收";
        case WAVE_SORTING:    return "分拣中";
        case WAVE_COMPLETING: return "回传中";
        case WAVE_CLEANED:    return "已完成";
        case WAVE_ERROR:      return "异常";
        default:              return "未知";
        }
    }
};

class WaveManager : public QObject
{
    Q_OBJECT
public:
    explicit WaveManager(GridBuffer* pBuffer, QObject* parent = nullptr);

    // ──── 格口查询（无锁委托）────
    GridEntry getGrid(const QString& code) const { return m_pBuffer->get(code); }
    bool      contains(const QString& code) const { return m_pBuffer->contains(code); }
    int       gridCount() const { return m_pBuffer->size(); }

    // ──── 波次数据设置（解析完成后调用）────
    void setWaveData(const QString& orderCode, int orderQty, int skuCount);

    // ──── 分拣状态（线程安全）────
    void markSorted(const QString& code);
    void markException(const QString& code);
    bool isSorted(const QString& code) const;
    bool isException(const QString& code) const;
    int  retryCount(const QString& code) const;

    // ──── 波次判定 ────
    bool isWaveComplete() const;

    // ──── 状态查询 ────
    WaveSnapshot snapshot() const;
    std::atomic<int>& status() { return m_waveStatus; }
    QString orderCode() const { return m_orderCode; }
    int     totalRecv() const;
    int     sorted() const;
    int     exception() const;
    int     sumLocation() const;

    // ──── 管理 ────
    void clearWave();
    void setMaxRetry(int n) { m_maxRetry = n; }
    void setWaveTimeoutMin(int m) { m_waveTimeoutMin = m; }

signals:
    void waveReceived(const QString& orderCode, int skuCount);
    void waveSortingStarted(const QString& orderCode);
    void waveReadyToReport(const QString& orderCode);  // 波次可回传
    void waveStatusChanged(int newStatus);
    void codeMarked(const QString& code, bool sorted); // 分拣/异常标记

private:
    GridBuffer*     m_pBuffer;
    QString         m_orderCode;
    int             m_orderQty         = 0;
    std::atomic<int> m_waveStatus{WAVE_IDLE};
    QDateTime       m_waveStartTime;
    bool            m_bSortingStarted  = false;
    int             m_maxRetry         = 3;
    int             m_waveTimeoutMin   = 0;

    QSet<QString>      m_setCodeRecv;
    QSet<QString>      m_setCodeSorted;
    QSet<QString>      m_setCodeException;
    QSet<QString>      m_setCodeProcessing;
    QMap<QString, int> m_mapCodeRetry;

    mutable std::mutex m_lock;
};
