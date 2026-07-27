#include "WaveManager.h"
#include "log_center.h"
#include "hlog1.h"

WaveManager::WaveManager(GridBuffer* pBuffer, QObject* parent)
    : QObject(parent), m_pBuffer(pBuffer)
{
}

void WaveManager::setWaveData(const QString& orderCode, int orderQty, int skuCount)
{
    if (m_waveStatus != WAVE_IDLE && m_waveStatus != WAVE_CLEANED)
    {
        WCS_INFO("[WaveMgr] 覆盖旧波次 old=%s status=%d",
            m_orderCode.toLocal8Bit().data(), m_waveStatus.load());
        clearWave();
    }

    m_orderCode = orderCode;
    m_orderQty  = orderQty;
    m_waveStartTime = QDateTime::currentDateTime();
    m_bSortingStarted = false;

    WCS_INFO("[WaveMgr] 波次注册 orderCode=%s qty=%d SKU=%d",
        orderCode.toLocal8Bit().data(), orderQty, skuCount);
    setState(WAVE_RECEIVED);
    emit waveReceived(orderCode, skuCount);
}

void WaveManager::setRecvSet(const QSet<QString>& set)
{
    std::unique_lock<std::mutex> lock(m_lock);
    m_setCodeRecv = set;
    WCS_INFO("[WaveMgr] 接收inco集合 size=%d", m_setCodeRecv.size());
}

void WaveManager::markSorted(const QString& code)
{
    bool complete = false;
    bool alreadyReported = false;

    {
        std::unique_lock<std::mutex> lock(m_lock);

        // 快速路径：波次已回传，跳过所有检查，仅记录状态
        if (m_bReported.load(std::memory_order_relaxed))
        {
            m_setCodeSorted.insert(code);
            m_setCodeProcessing.remove(code);
            m_mapCodeRetry.remove(code);
            lock.unlock();
            emit codeMarked(code, true);
            return;
        }

        m_setCodeSorted.insert(code);
        m_setCodeProcessing.remove(code);
        m_mapCodeRetry.remove(code);

        if (!m_bSortingStarted && !m_orderCode.isEmpty())
        {
            m_bSortingStarted = true;
            WCS_INFO("[WaveMgr] 波次分拣开始 orderCode=%s", m_orderCode.toLocal8Bit().data());
            emit waveSortingStarted(m_orderCode);
            setState(WAVE_SORTING);
        }

        complete = checkWaveCompleteLocked();
        if (complete)
        {
            // ★ 关键修复：exchange 在锁内执行，与 checkWaveCompleteLocked 原子化
            // 消除 TOCTOU 竞态：防止多线程同时通过快速路径检查后重复触发状态转换
            alreadyReported = m_bReported.exchange(true);
        }
    }

    emit codeMarked(code, true);
    if (complete && !alreadyReported)
    {
        WCS_INFO("[WaveMgr] 波次完成 orderCode=%s sorted=%d exception=%d total=%d",
            m_orderCode.toLocal8Bit().data(),
            m_setCodeSorted.size(), m_setCodeException.size(), m_setCodeRecv.size());
        emit waveReadyToReport(m_orderCode);
        setState(WAVE_COMPLETING);
    }
}

void WaveManager::markException(const QString& code)
{
    bool complete = false;
    bool alreadyReported = false;

    {
        std::unique_lock<std::mutex> lock(m_lock);

        // 快速路径：波次已回传，跳过所有检查，仅记录状态
        if (m_bReported.load(std::memory_order_relaxed))
        {
            m_setCodeException.insert(code);
            m_setCodeProcessing.remove(code);
            int retry = m_mapCodeRetry.value(code, 0) + 1;
            m_mapCodeRetry[code] = retry;
            lock.unlock();
            emit codeMarked(code, false);
            return;
        }

        m_setCodeException.insert(code);
        m_setCodeProcessing.remove(code);

        int retry = m_mapCodeRetry.value(code, 0) + 1;
        m_mapCodeRetry[code] = retry;

        complete = checkWaveCompleteLocked();
        if (complete)
        {
            // ★ 关键修复：exchange 在锁内执行，与 checkWaveCompleteLocked 原子化
            alreadyReported = m_bReported.exchange(true);
        }
    }

    emit codeMarked(code, false);
    if (complete && !alreadyReported)
    {
        WCS_INFO("[WaveMgr] 波次完成(异常) orderCode=%s sorted=%d exception=%d total=%d",
            m_orderCode.toLocal8Bit().data(),
            m_setCodeSorted.size(), m_setCodeException.size(), m_setCodeRecv.size());
        emit waveReadyToReport(m_orderCode);
        setState(WAVE_COMPLETING);
    }
}

bool WaveManager::isSorted(const QString& code) const
{
    std::unique_lock<std::mutex> lock(m_lock);
    return m_setCodeSorted.contains(code);
}

bool WaveManager::isException(const QString& code) const
{
    std::unique_lock<std::mutex> lock(m_lock);
    return m_setCodeException.contains(code);
}

int WaveManager::retryCount(const QString& code) const
{
    std::unique_lock<std::mutex> lock(m_lock);
    return m_mapCodeRetry.value(code, 0);
}

bool WaveManager::isWaveComplete() const
{
    std::unique_lock<std::mutex> lock(m_lock);
    return checkWaveCompleteLocked();
}

bool WaveManager::checkWaveCompleteLocked() const
{
    int total = m_setCodeRecv.size();
    int done  = m_setCodeSorted.size() + m_setCodeException.size();
    if (total > 0 && done >= total)
        return true;

    if (m_bSortingStarted && m_waveTimeoutMin > 0)
    {
        qint64 elapsed = m_waveStartTime.secsTo(QDateTime::currentDateTime());
        if (elapsed > m_waveTimeoutMin * 60)
            return true;
    }

    return false;
}

WaveSnapshot WaveManager::snapshot() const
{
    WaveSnapshot snap;
    snap.orderCode     = m_orderCode;
    snap.orderQty      = m_orderQty;
    snap.waveStatus    = m_waveStatus;
    snap.statusText    = WaveSnapshot::statusToString(m_waveStatus);

    // 优先从 GridEntry 读取批次信息（与条码查询结果一致）
    const QMap<QString, GridEntry>* pMap = m_pBuffer->activeMap();
    if (pMap && !pMap->isEmpty())
    {
        snap.skuCount = pMap->constBegin().value().skuCount;
    }
    else
    {
        snap.skuCount = m_pBuffer->size();
    }

    snap.sumLocation   = sumLocation();

    {
        std::unique_lock<std::mutex> lock(m_lock);
        snap.sortedCount    = m_setCodeSorted.size();
        snap.exceptionCount = m_setCodeException.size();
        snap.totalRecv      = m_setCodeRecv.size();
    }

    snap.elapsedSec = m_waveStartTime.secsTo(QDateTime::currentDateTime());
    return snap;
}

bool WaveManager::setState(int newStatus)
{
    int current = m_waveStatus.load();

    bool allowed = false;
    switch (current)
    {
    case WAVE_IDLE:
        allowed = (newStatus == WAVE_RECEIVED);
        break;
    case WAVE_RECEIVED:
        allowed = (newStatus == WAVE_SORTING || newStatus == WAVE_CLEANED);
        break;
    case WAVE_SORTING:
        allowed = (newStatus == WAVE_COMPLETING || newStatus == WAVE_ERROR);
        break;
    case WAVE_COMPLETING:
        allowed = (newStatus == WAVE_CLEANED || newStatus == WAVE_ERROR);
        break;
    case WAVE_CLEANED:
        allowed = (newStatus == WAVE_RECEIVED);
        break;
    case WAVE_ERROR:
        allowed = (newStatus == WAVE_CLEANED);
        break;
    }

    if (allowed)
    {
        m_waveStatus.store(newStatus);
        WCS_INFO("[WaveMgr] 状态转换 %d -> %d", current, newStatus);
        emit waveStatusChanged(newStatus);
        return true;
    }

    WCS_WARN("[WaveMgr] 状态转换拒绝 %d -> %d", current, newStatus);
    return false;
}

int WaveManager::totalRecv() const
{
    std::unique_lock<std::mutex> lock(m_lock);
    return m_setCodeRecv.size();
}

int WaveManager::sorted() const
{
    std::unique_lock<std::mutex> lock(m_lock);
    return m_setCodeSorted.size();
}

int WaveManager::exception() const
{
    std::unique_lock<std::mutex> lock(m_lock);
    return m_setCodeException.size();
}

int WaveManager::sumLocation() const
{
    return m_pBuffer->uniqueValueCount();
}

void WaveManager::clearWave()
{
    std::unique_lock<std::mutex> lock(m_lock);
    m_setCodeRecv.clear();
    m_setCodeSorted.clear();
    m_setCodeException.clear();
    m_mapCodeRetry.clear();
    m_setCodeProcessing.clear();
    m_orderCode.clear();
    m_orderQty = 0;
    m_bSortingStarted = false;
    m_bReported.store(false);
    WCS_INFO("[WaveMgr] 波次已清理");
    setState(WAVE_CLEANED);
}
