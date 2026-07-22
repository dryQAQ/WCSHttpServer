#include "WaveManager.h"
#include "log_center.h"

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
    m_waveStatus = WAVE_RECEIVED;
    m_waveStartTime = QDateTime::currentDateTime();
    m_bSortingStarted = false;

    WCS_INFO("[WaveMgr] 波次注册 orderCode=%s qty=%d SKU=%d",
        orderCode.toLocal8Bit().data(), orderQty, skuCount);
    emit waveReceived(orderCode, skuCount);
    emit waveStatusChanged(WAVE_RECEIVED);
}

void WaveManager::markSorted(const QString& code)
{
    std::unique_lock<std::mutex> lock(m_lock);

    m_setCodeSorted.insert(code);
    m_setCodeProcessing.remove(code);
    m_mapCodeRetry.remove(code);

    if (!m_bSortingStarted)
    {
        m_bSortingStarted = true;
        m_waveStatus = WAVE_SORTING;
        WCS_INFO("[WaveMgr] 波次分拣开始 orderCode=%s", m_orderCode.toLocal8Bit().data());
        emit waveSortingStarted(m_orderCode);
        emit waveStatusChanged(WAVE_SORTING);
    }

    emit codeMarked(code, true);
}

void WaveManager::markException(const QString& code)
{
    std::unique_lock<std::mutex> lock(m_lock);

    m_setCodeException.insert(code);
    m_setCodeProcessing.remove(code);

    int retry = m_mapCodeRetry.value(code, 0) + 1;
    m_mapCodeRetry[code] = retry;

    emit codeMarked(code, false);
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
    snap.skuCount      = m_pBuffer->size();
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
    m_waveStatus = WAVE_CLEANED;
    m_bSortingStarted = false;
    WCS_INFO("[WaveMgr] 波次已清理");
}
