#include "WaveManager.h"
#include "log_center.h"
#include "hlog1.h"

WaveManager::WaveManager(GridBuffer* pBuffer, QObject* parent)
    : QObject(parent), m_pBuffer(pBuffer)
{
}

void WaveManager::setWaveData(const QString& orderCode, int orderQty, int skuCount)
{
    if (m_waveStatus != WAVE_IDLE && m_waveStatus != WAVE_FINISHED && m_waveStatus != WAVE_CANCELLED)
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
    setState(WAVE_CREATED);
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
        setState(WAVE_ENDING);
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
        setState(WAVE_ENDING);
    }
}

bool WaveManager::isSorted(const QString& code) const
{
    std::unique_lock<std::mutex> lock(m_lock);
    return m_setCodeSorted.contains(code);
}

bool WaveManager::isCodeSorted(const QString& code) const
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
        // 空闲 → 已下发（波次下发成功）
        allowed = (newStatus == WAVE_CREATED);
        break;
    case WAVE_CREATED:
        // 已下发 → 已绑定（容器绑定成功）| 已取消（波次取消成功）| 异常挂起
        allowed = (newStatus == WAVE_BOUND || newStatus == WAVE_CANCELLED || newStatus == WAVE_HELD);
        break;
    case WAVE_BOUND:
        // 已绑定 → 分拣中（开工）| 已取消（波次取消且未分拣）| 异常挂起
        allowed = (newStatus == WAVE_SORTING || newStatus == WAVE_CANCELLED || newStatus == WAVE_HELD);
        break;
    case WAVE_SORTING:
        // 分拣中 → 满箱同步中（满箱触发）| 完结中（满足完结条件）| 异常挂起
        allowed = (newStatus == WAVE_FULLBOX_SYNC || newStatus == WAVE_ENDING || newStatus == WAVE_HELD);
        break;
    case WAVE_FULLBOX_SYNC:
        // 满箱同步中 → 分拣中（满箱回传成功 + 新容器绑定）| 异常挂起（满箱回传失败耗尽）
        allowed = (newStatus == WAVE_SORTING || newStatus == WAVE_HELD);
        break;
    case WAVE_CANCEL_PENDING:
        // 取消处理中 → 已取消（判定成功）| 保持原状态（判定失败，拒绝取消）
        allowed = (newStatus == WAVE_CANCELLED || newStatus == WAVE_CREATED || newStatus == WAVE_BOUND);
        break;
    case WAVE_CANCELLED:
        // 已取消 → 空闲（终态，可接收新波次）
        allowed = (newStatus == WAVE_IDLE);
        break;
    case WAVE_ENDING:
        // 完结中 → 已完成（完结回传成功）| 异常挂起（完结回传失败耗尽）
        allowed = (newStatus == WAVE_FINISHED || newStatus == WAVE_HELD);
        break;
    case WAVE_FINISHED:
        // 已完成 → 空闲（终态，可接收新波次）
        allowed = (newStatus == WAVE_IDLE);
        break;
    case WAVE_HELD:
        // 异常挂起 → 空闲（人工清理后）| 已完成（人工强制完结）
        allowed = (newStatus == WAVE_IDLE || newStatus == WAVE_FINISHED);
        break;
    }

    if (allowed)
    {
        m_waveStatus.store(newStatus);
        WCS_INFO("[WaveMgr] 状态转换 %d(%s) -> %d(%s)",
            current, WaveSnapshot::statusToString(current).toLocal8Bit().data(),
            newStatus, WaveSnapshot::statusToString(newStatus).toLocal8Bit().data());
        emit waveStatusChanged(newStatus);
        return true;
    }

    WCS_WARN("[WaveMgr] 状态转换拒绝 %d(%s) -> %d(%s)",
        current, WaveSnapshot::statusToString(current).toLocal8Bit().data(),
        newStatus, WaveSnapshot::statusToString(newStatus).toLocal8Bit().data());
    return false;
}

// ============================================================================
// startSorting — 开工（T-S4-05）
// 将波次从 BOUND 推进到 SORTING，仅当绑定全部完成且未开始分拣时允许
// 返回 true 表示开工成功，false 表示条件不满足
// ============================================================================
bool WaveManager::startSorting()
{
    // ★ S8 波次取消与首件分拣互斥锁（T-S8-06）
    std::unique_lock<std::mutex> cancelLock(m_cancelSortMutex);

    int current = m_waveStatus.load();

    // 前置条件：状态必须为 BOUND
    if (current != WAVE_BOUND)
    {
        WCS_WARN("[WaveMgr] 开工失败 当前状态非BOUND status=%d(%s)",
            current, WaveSnapshot::statusToString(current).toLocal8Bit().data());
        return false;
    }

    // 原子状态迁移：BOUND → SORTING
    if (!setState(WAVE_SORTING))
    {
        return false;
    }

    m_bSortingStarted = true;
    WCS_INFO("[WaveMgr] 开工成功 BOUND→SORTING orderCode=%s",
        m_orderCode.toLocal8Bit().data());
    emit waveSortingStarted(m_orderCode);
    return true;
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
    setState(WAVE_IDLE);
}

bool WaveManager::isSortingStarted() const
{
    // 需求 §3.3 已开始分拣判定（四条件，满足任一即视为已开始）
    
    // 条件1：任务状态 ∈ {SORTING, FULLBOX_SYNC, ENDING, FINISHED, HELD}
    int status = m_waveStatus.load();
    if (status == WAVE_SORTING || status == WAVE_FULLBOX_SYNC ||
        status == WAVE_ENDING || status == WAVE_FINISHED || status == WAVE_HELD)
        return true;

    // 条件2：成功分拣计数 > 0
    if (sorted() > 0) return true;

    // 条件3：已存在任意成功的满箱回传记录（H7）
    if (hasFullboxRecord()) return true;

    // 条件4：现场已点击「开始分拣」或设备进入运行态
    if (m_bSortingStarted) return true;

    return false;
}

// ============================================================================
// hasFullboxRecord — S5 实现（T-S5-01）
// 通过 m_hasFullboxRecord 原子标记位返回，HttpServer 在满箱成功时设置
// ============================================================================
bool WaveManager::hasFullboxRecord() const
{
    return m_hasFullboxRecord.load();
}

// ============================================================================
// triggerFullbox — 满箱触发（T-S5-01）
// 校验当前状态为 SORTING，原子迁移到 FULLBOX_SYNC
// 返回 true 表示成功触发满箱流程
// ============================================================================
bool WaveManager::triggerFullbox()
{
    int current = m_waveStatus.load();
    if (current != WAVE_SORTING)
    {
        WCS_WARN("[WaveMgr] 满箱触发失败 当前状态非SORTING status=%d(%s)",
            current, WaveSnapshot::statusToString(current).toLocal8Bit().data());
        return false;
    }
    if (!setState(WAVE_FULLBOX_SYNC))
    {
        return false;
    }
    WCS_INFO("[WaveMgr] 满箱触发成功 SORTING→FULLBOX_SYNC orderCode=%s",
        m_orderCode.toLocal8Bit().data());
    return true;
}

// ============================================================================
// resumeSorting — 满箱成功后恢复分拣（T-S5-05）
// FULLBOX_SYNC→SORTING
// ============================================================================
bool WaveManager::resumeSorting()
{
    int current = m_waveStatus.load();
    if (current != WAVE_FULLBOX_SYNC)
    {
        WCS_WARN("[WaveMgr] 恢复分拣失败 当前状态非FULLBOX_SYNC status=%d(%s)",
            current, WaveSnapshot::statusToString(current).toLocal8Bit().data());
        return false;
    }
    if (!setState(WAVE_SORTING))
    {
        return false;
    }
    m_hasFullboxRecord.store(true);
    WCS_INFO("[WaveMgr] 满箱成功恢复分拣 FULLBOX_SYNC→SORTING orderCode=%s",
        m_orderCode.toLocal8Bit().data());
    return true;
}

// ============================================================================
// holdAfterFullboxFail — 满箱失败耗尽重试（T-S5-04）
// FULLBOX_SYNC→HELD
// ============================================================================
bool WaveManager::holdAfterFullboxFail()
{
    int current = m_waveStatus.load();
    if (current != WAVE_FULLBOX_SYNC)
    {
        WCS_WARN("[WaveMgr] 满箱失败挂起 当前状态非FULLBOX_SYNC status=%d(%s)",
            current, WaveSnapshot::statusToString(current).toLocal8Bit().data());
        return false;
    }
    if (!setState(WAVE_HELD))
    {
        return false;
    }
    WCS_INFO("[WaveMgr] 满箱失败耗尽重试 FULLBOX_SYNC→HELD orderCode=%s",
        m_orderCode.toLocal8Bit().data());
    return true;
}

// ============================================================================
// canComplete — 可完结条件检查（T-S6-01）
// 需求 §11.5：须同时满足所有条件方可完结
//   1. 任务处于 SORTING
//   2. 计划完成：Σ sortedQty 达到计划（或剩余件全部进入已结案异常）
//   3. 所有已产生满箱的满箱回传（H7）全部成功（当前无 FULLBOX_SYNC 状态）
//   4. 无进行中的波次取消（H5）（当前无 CANCEL_PENDING 状态）
//   5. 无 FULLBOX_SYNC/未完成出站
// ============================================================================
bool WaveManager::canComplete() const
{
    // 条件1：任务处于 SORTING
    int status = m_waveStatus.load();
    if (status != WAVE_SORTING)
    {
        WCS_WARN("[WaveMgr] 完结条件不满足 当前状态非SORTING status=%d(%s)",
            status, WaveSnapshot::statusToString(status).toLocal8Bit().data());
        return false;
    }

    // 条件2：计划完成（sorted + exception >= total）
    std::unique_lock<std::mutex> lock(m_lock);
    int total = m_setCodeRecv.size();
    int done  = m_setCodeSorted.size() + m_setCodeException.size();
    lock.unlock();

    if (total == 0 || done < total)
    {
        WCS_WARN("[WaveMgr] 完结条件不满足 计划未完成 total=%d done=%d(sorted=%d exc=%d)",
            total, done, sorted(), exception());
        return false;
    }

    // 条件3/4/5：状态检查已覆盖（仅 SORTING 允许，FULLBOX_SYNC/CANCEL_PENDING 已在条件1排除）
    // 条件3：无进行中的满箱同步（FULLBOX_SYNC 状态下条件1已拒绝）
    // 条件4：无进行中的取消（CANCEL_PENDING 状态下条件1已拒绝）

    WCS_INFO("[WaveMgr] 完结条件满足 orderCode=%s total=%d done=%d",
        m_orderCode.toLocal8Bit().data(), total, done);
    return true;
}

// ============================================================================
// completeToEnding — 手动触发完结（T-S6-01/04）
// 校验 canComplete() 后，原子迁移 SORTING→ENDING
// ============================================================================
bool WaveManager::completeToEnding()
{
    if (!canComplete())
    {
        return false;
    }

    if (!setState(WAVE_ENDING))
    {
        return false;
    }

    WCS_INFO("[WaveMgr] 手动触发完结 SORTING→ENDING orderCode=%s",
        m_orderCode.toLocal8Bit().data());
    return true;
}

// ============================================================================
// reconcile — 波次对账（T-S8-01/02，需求 §14.2）
// 核对：计划数/实分数/异常数/满箱回传累计/完结回传状态
// 返回 WaveReconciliation 结构，调用方根据 hasDiff() 判断是否需告警
// ============================================================================
WaveReconciliation WaveManager::reconcile() const
{
    WaveReconciliation r;
    r.orderCode    = m_orderCode;
    r.planQty      = m_orderQty;
    r.waveStatus   = m_waveStatus.load();
    r.statusText   = WaveSnapshot::statusToString(r.waveStatus);

    WCS_INFO("[WaveMgr] 对账开始 order=%s plan=%d status=%d(%s)",
        m_orderCode.toLocal8Bit().data(), r.planQty,
        r.waveStatus, r.statusText.toLocal8Bit().data());

    {
        std::unique_lock<std::mutex> lock(m_lock);
        r.sortedQty    = m_setCodeSorted.size();
        r.exceptionQty = m_setCodeException.size();
        r.totalRecv    = m_setCodeRecv.size();

        WCS_INFO("[WaveMgr] 对账 内存计数 sorted=%d exception=%d totalRecv=%d",
            r.sortedQty, r.exceptionQty, r.totalRecv);

        // 计划数与接收数差异分析
        if (r.planQty > 0 && (r.sortedQty + r.exceptionQty) != r.totalRecv)
        {
            WCS_WARN("[WaveMgr] 对账 计划≠接收 plan=%d recv=%d gap=%d",
                r.planQty, r.totalRecv, r.planQty - r.totalRecv);
            WCS_WARN("[WaveMgr] 对账 明细 sorted=%d exception=%d recvSetSize=%d",
                r.sortedQty, r.exceptionQty, m_setCodeRecv.size());
        }
    }

    // 完结回传（H8）是否成功
    r.endReportSuccess = (r.waveStatus == WAVE_FINISHED);
    if (!r.endReportSuccess && r.waveStatus != WAVE_IDLE && r.waveStatus != WAVE_CREATED)
    {
        WCS_INFO("[WaveMgr] 对账 H8未完成 order=%s waveStatus=%d(%s)",
            m_orderCode.toLocal8Bit().data(), r.waveStatus, r.statusText.toLocal8Bit().data());
    }

    // 满箱回传（H7）累计和异常计数由外部填充（HttpServer通过DB查询后补入）
    WCS_INFO("[WaveMgr] 对账完成 order=%s plan=%d sorted=%d exc=%d recv=%d status=%s diff=%s",
        r.orderCode.toLocal8Bit().data(), r.planQty, r.sortedQty, r.exceptionQty,
        r.totalRecv, r.statusText.toLocal8Bit().data(), r.diffSummary().toLocal8Bit().data());

    return r;
}

// ============================================================================
// tryCancelWave — 波次取消与首件分拣互斥（T-S8-06，需求 §3.3 竞态要求）
// 加锁后判定 isSortingStarted()，若未分拣则原子迁移到 CANCELLED
// 返回 true 表示取消成功，false 表示已分拣/状态迁移失败
// ============================================================================
bool WaveManager::tryCancelWave()
{
    // ★ S8 波次取消与首件分拣互斥锁（T-S8-06）
    // 与 startSorting() 使用同一把锁，保证取消与首件分拣互斥
    WCS_INFO("[WaveMgr] H5取消 尝试获取互斥锁 order=%s", m_orderCode.toLocal8Bit().data());

    std::unique_lock<std::mutex> cancelLock(m_cancelSortMutex);

    WCS_INFO("[WaveMgr] H5取消 已获取互斥锁 order=%s", m_orderCode.toLocal8Bit().data());

    // 已分拣判定
    bool started = isSortingStarted();
    int curStatus = m_waveStatus.load();
    int curSorted = sorted();
    int curException = exception();

    WCS_INFO("[WaveMgr] H5取消 分拣判定 started=%d status=%d sorted=%d exception=%d order=%s",
        started ? 1 : 0, curStatus, curSorted, curException,
        m_orderCode.toLocal8Bit().data());

    if (started)
    {
        WCS_WARN("[WaveMgr] H5取消失败 已分拣 order=%s sorted=%d exception=%d status=%d",
            m_orderCode.toLocal8Bit().data(), curSorted, curException, curStatus);
        return false;
    }

    // 原子状态迁移
    int prevStatus = m_waveStatus.load();
    if (!setState(WAVE_CANCELLED))
    {
        int newStatus = m_waveStatus.load();
        WCS_WARN("[WaveMgr] H5取消失败 状态迁移拒绝 order=%s prev=%d→target=CANCELLED now=%d",
            m_orderCode.toLocal8Bit().data(), prevStatus, newStatus);
        return false;
    }

    WCS_INFO("[WaveMgr] H5取消成功 order=%s %d→CANCELLED",
        m_orderCode.toLocal8Bit().data(), prevStatus);
    return true;
}
