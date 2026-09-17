#include "WaveManager.h"
#include "LogService.h"

WaveManager::WaveManager(GridBuffer* pBuffer, QObject* parent)
    : QObject(parent), m_pBuffer(pBuffer)
{
}

void WaveManager::setWaveData(const QString& orderCode, int orderQty, int skuCount)
{
    // ★ 已完成（FINISHED）和空闲（IDLE）都允许新波次下发
    //   FINISHED 需先清理旧数据→IDLE，再→CREATED
    if (m_waveStatus != WAVE_IDLE && m_waveStatus != WAVE_CANCELLED)
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

bool WaveManager::restoreWave(const QString& orderCode, int orderQty, int skuCount,
                              int targetStatus,
                              const QSet<QString>& recvSet,
                              const QSet<QString>& sortedSet,
                              const QSet<QString>& exceptionSet,
                              bool hasFullboxRecord)
{
    // 前置校验：仅 IDLE 可恢复；目标状态允许业务态 CREATED/BOUND/SORTING/ENDING，
    // ★ 2026-09-06 另支持终态 FINISHED/CANCELLED（原"载入查看"模式）。
    //   ★ 2026-09-16 现场需求①：终态波次**不再允许切回**（HttpServer::resumeUnfinishedWave
    //     入口守卫直接拒绝），因此调用方不会再传终态进来；本分支与其 bViewOnly 处理
    //     作为状态机能力保留（不参与分拣/回传），仅用于防御。
    if (m_waveStatus.load() != WAVE_IDLE)
    {
        WCS_WARN("[WaveMgr] 恢复拒绝 当前状态非IDLE status=%d(%s)",
            m_waveStatus.load(), WaveSnapshot::statusToString(m_waveStatus.load()).toLocal8Bit().data());
        return false;
    }
    const bool bViewOnly = (targetStatus == WAVE_FINISHED || targetStatus == WAVE_CANCELLED);
    if (targetStatus != WAVE_CREATED && targetStatus != WAVE_BOUND &&
        targetStatus != WAVE_SORTING && targetStatus != WAVE_ENDING && !bViewOnly)
    {
        WCS_WARN("[WaveMgr] 恢复拒绝 非法目标状态 target=%d", targetStatus);
        return false;
    }

    // 重建进度集合与波次基本信息
    {
        std::unique_lock<std::mutex> lock(m_lock);
        m_setCodeRecv      = recvSet;
        m_setCodeSorted    = sortedSet;
        m_setCodeException = exceptionSet;
        m_setCodeProcessing.clear();
        m_mapCodeRetry.clear();
        // ★ 2026-09-09 需求8：恢复时计数=集合大小（DB 重建无重复反馈信息，按去重数起算）
        m_sortedTotal    = sortedSet.size();
        m_exceptionTotal = exceptionSet.size();
    }
    m_orderCode       = orderCode;
    m_orderQty        = orderQty;
    m_waveStartTime   = QDateTime::currentDateTime();
    m_bSortingStarted = (targetStatus == WAVE_SORTING || targetStatus == WAVE_ENDING);
    m_hasFullboxRecord.store(hasFullboxRecord);

    // 已完成件数已达计划 → 置已回传标记，避免恢复后重复触发波次完成回传
    int done = sortedSet.size() + exceptionSet.size();
    m_bReported.store((orderQty > 0 && done >= orderQty) || bViewOnly);

    // 链式走合法状态迁移（全部在白名单内）
    bool ok = false;
    if (bViewOnly)
    {
        // ★ 2026-09-06 终态查看：IDLE→CREATED→(业务链)→终态
        ok = setState(WAVE_CREATED);
        if (targetStatus == WAVE_CANCELLED)
            ok = ok && setState(WAVE_CANCELLED);                    // CREATED→CANCELLED
        else
        {
            ok = ok && setState(WAVE_BOUND);                        // CREATED→BOUND
            ok = ok && setState(WAVE_SORTING);                      // BOUND→SORTING
            ok = ok && setState(WAVE_ENDING);                       // SORTING→ENDING
            ok = ok && setState(WAVE_FINISHED);                     // ENDING→FINISHED
        }
    }
    else
    {
        ok = setState(WAVE_CREATED);                                // IDLE→CREATED
        if (ok && targetStatus >= WAVE_BOUND)
            ok = setState(WAVE_BOUND);                              // CREATED→BOUND
        if (ok && targetStatus >= WAVE_SORTING && targetStatus != WAVE_ENDING)
            ok = setState(WAVE_SORTING);                            // BOUND→SORTING
        if (ok && targetStatus == WAVE_ENDING)
            ok = setState(WAVE_ENDING);                             // BOUND→ENDING
    }

    if (!ok)
    {
        WCS_WARN("[WaveMgr] 恢复失败 状态链迁移异常 order=%s", orderCode.toLocal8Bit().data());
        clearWave();  // 兜底回滚（清数据，尽力回到 IDLE）
        return false;
    }

    WCS_INFO("[WaveMgr] 恢复波次 order=%s qty=%d SKU=%d target=%d(%s)%s sorted=%d exception=%d fullbox=%d",
        orderCode.toLocal8Bit().data(), orderQty, skuCount, targetStatus,
        WaveSnapshot::statusToString(targetStatus).toLocal8Bit().data(),
        bViewOnly ? "(查看模式)" : "", (int)sortedSet.size(), (int)exceptionSet.size(), hasFullboxRecord ? 1 : 0);

    emit waveStatusChanged(targetStatus);
    return true;
}

void WaveManager::markSorted(const QString& code)
{
    bool complete = false;
    bool alreadyReported = false;

    {
        std::unique_lock<std::mutex> lock(m_lock);

        // ★ 2026-09-09 需求8：分拣数量以 PLC 实时反馈为准——每次成功反馈累计 +1（含重复反馈）
        m_sortedTotal++;

        // 快速路径：波次已回传，跳过所有检查，仅记录状态
        if (m_bReported.load(std::memory_order_relaxed))
        {
            m_setCodeSorted.insert(code);
            // ★ 2026-09-06 双计修复：曾异常(如PLC临时失败)后重投成功的件，从异常集合移除
            // ★ 2026-09-13 异常及时清理：移除时同步让"异常件数" −1（内部同锁完成，重复调用只减一次）
            removeExceptionOnSortedLocked(code);
            m_setCodeProcessing.remove(code);
            m_mapCodeRetry.remove(code);
            lock.unlock();
            emit codeMarked(code, true);
            return;
        }

        m_setCodeSorted.insert(code);
        // ★ 2026-09-06 双计修复：同一 code 曾入异常集合（PLC 报无格口/信息不全等临时失败后重投成功），
        //   成功时必须从异常集合移除——否则 sorted 与 exception 两集合同时含该 code，
        //   导致 UI 异常数虚高、波次完成判定/对账双计
        // ★ 2026-09-13 异常及时清理：同一 EPC 之后成功落格 → 异常数立即 −1（不再一直保留）
        removeExceptionOnSortedLocked(code);
        m_setCodeProcessing.remove(code);
        m_mapCodeRetry.remove(code);

        // ★ 已移除自动开工逻辑（原首件落格自动 BOUND→SORTING）
        //   状态转换仅通过 MainWindow 手动点击「开始分拣」按钮触发 startSorting()
        //   避免未点击按钮时状态自动推进到 SORTING 导致后续步骤被意外触发
        // if (!m_bSortingStarted && !m_orderCode.isEmpty())
        // {
        //     m_bSortingStarted = true;
        //     WCS_INFO("[WaveMgr] 波次分拣开始 orderCode=%s", m_orderCode.toLocal8Bit().data());
        //     emit waveSortingStarted(m_orderCode);
        //     setState(WAVE_SORTING);
        // }

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
        // ★ 锁格回传（H7）：仅发送状态报告，不改变波次状态
        //   状态迁移（→ENDING）仅由用户点击"结束任务"触发 completeToEnding()
        emit waveReadyToReport(m_orderCode);
    }
}

void WaveManager::markException(const QString& code)
{
    bool complete = false;
    bool alreadyReported = false;

    {
        std::unique_lock<std::mutex> lock(m_lock);

        // ★ 2026-09-13 异常口径（客户口径：界面「处理」/「异常口」= 同一个量）：
        //   m_setCodeException = 当前仍在异常口、尚未处理完的件（去重 EPC）。
        //   同一 EPC 反复掉入异常口（重复反馈 / 重投再失败）只算 1 件，不刷高件数。
        m_setCodeException.insert(code);
        m_exceptionTotal = m_setCodeException.size();

        // 快速路径：波次已回传，跳过所有检查，仅记录状态
        if (m_bReported.load(std::memory_order_relaxed))
        {
            m_setCodeProcessing.remove(code);
            int retry = m_mapCodeRetry.value(code, 0) + 1;
            m_mapCodeRetry[code] = retry;
            lock.unlock();
            emit codeMarked(code, false);
            return;
        }

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
        // ★ 锁格回传（H7）：仅发送状态报告，不改变波次状态
        emit waveReadyToReport(m_orderCode);
    }
}

bool WaveManager::removeExceptionOnSortedLocked(const QString& code)
{
    // 调用方必须已持有 m_lock
    if (!m_setCodeException.remove(code))   // Qt5 QSet::remove 返回 bool
        return false;
    if (m_exceptionTotal > 0)
        --m_exceptionTotal;
    if (m_exceptionTotal != m_setCodeException.size())   // 防御：两者永不失步
        m_exceptionTotal = m_setCodeException.size();
    // ★ 2026-09-13 客户口径：成功落格即视为"已处理" → 「处理」与「异常口」两个数字同步 −1
    return true;
}

bool WaveManager::removeExceptionOnSorted(const QString& code)
{
    std::unique_lock<std::mutex> lock(m_lock);
    const bool removed = removeExceptionOnSortedLocked(code);
    if (removed)
    {
        WCS_INFO("[WaveMgr] 异常件已处理完 code=%s 成功落格 → 处理/异常口 -1（当前=%d）",
            code.toLocal8Bit().data(), m_exceptionTotal);
    }
    return removed;
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
    // ★ 使用 m_orderQty（总件数）而非 m_setCodeRecv.size()（去重后的SKU数）
    //    因为同一个SKU下可能存在多个EPC（如一箱5件同款），SKU数量 ≠ 实际分拣件数
    int total = m_orderQty;
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

    // 优先从 GridEntry 读取批次信息（与EPC编码查询结果一致）
    const QMap<QString, GridEntry>* pMap = m_pBuffer->activeMap();
    if (pMap && !pMap->isEmpty())
    {
        snap.skuCount = pMap->constBegin().value().skuCount;
    }
    else
    {
        snap.skuCount = m_pBuffer->size();
    }

    snap.sumLocation   = sorted();
    snap.lastWaveCode  = m_lastOrderCode;  // ★ UI「上波次」显示

    {
        std::unique_lock<std::mutex> lock(m_lock);
        // ★ 2026-09-09 需求8：面板已分拣按 PLC 实时反馈累计数（以实时记录为准）
        snap.sortedCount    = m_sortedTotal;
        // ★ 2026-09-13 客户口径：界面「处理」= 仍在异常口、尚未处理完的件数（去重 EPC）；
        //   该 EPC 成功落格即视为已处理 → 此数实时减少（不做只增累计）
        snap.exceptionCount = m_setCodeException.size();
        snap.totalRecv      = m_orderQty;  // ★ 总件数，非去重SKU数
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
        // 已下发 → 已绑定（容器绑定成功）| 完结中（手动结束任务）| 已取消（波次取消成功）| 异常挂起
        allowed = (newStatus == WAVE_BOUND || newStatus == WAVE_ENDING || newStatus == WAVE_CANCELLED || newStatus == WAVE_HELD);
        break;
    case WAVE_BOUND:
        // 已绑定 → 分拣中（开工）| 完结中（手动结束任务）| 已取消（波次取消且未分拣）| 异常挂起
        allowed = (newStatus == WAVE_SORTING || newStatus == WAVE_ENDING || newStatus == WAVE_CANCELLED || newStatus == WAVE_HELD);
        break;
    case WAVE_SORTING:
        // 分拣中 → 满箱同步中（满箱触发）| 完结中（满足完结条件）| 异常挂起
        allowed = (newStatus == WAVE_FULLBOX_SYNC || newStatus == WAVE_ENDING || newStatus == WAVE_HELD);
        break;
    case WAVE_FULLBOX_SYNC:
        // 满箱同步中 → 分拣中（满箱回传成功 + 新容器绑定）| 完结中（手动结束任务）| 异常挂起（满箱回传失败耗尽）
        allowed = (newStatus == WAVE_SORTING || newStatus == WAVE_ENDING || newStatus == WAVE_HELD);
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
        // 完结中 → 已完成（回传成功）| 异常挂起（完结回传失败耗尽）
        allowed = (newStatus == WAVE_FINISHED || newStatus == WAVE_HELD);
        break;
    case WAVE_FINISHED:
        // 已完成 → 空闲（终态，可接收新波次）
        allowed = (newStatus == WAVE_IDLE);
        break;
    case WAVE_HELD:
        // 异常挂起 → 空闲（人工清理后）| 已完成（人工强制完结）| 已绑定（人工恢复后重新开工，闭环重新回传完结）
        allowed = (newStatus == WAVE_IDLE || newStatus == WAVE_FINISHED || newStatus == WAVE_BOUND);
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
    return m_orderQty;  // ★ 总件数，非去重SKU数（同一SKU可能有多个EPC）
}

int WaveManager::sorted() const
{
    // ★ 2026-09-09 需求8：返回 PLC 实时反馈累计数（以实时记录为准），而非去重集合大小
    std::unique_lock<std::mutex> lock(m_lock);
    return m_sortedTotal;
}

int WaveManager::exception() const
{
    // ★ 2026-09-13 客户口径：返回「处理」件数 = 仍在异常口、尚未处理完的件数（去重 EPC）。
    //   该 EPC 成功落格即视为已处理 → 此数实时减少（不做只增累计）。
    std::unique_lock<std::mutex> lock(m_lock);
    return m_setCodeException.size();
}

QStringList WaveManager::exceptionEpcs() const
{
    std::unique_lock<std::mutex> lock(m_lock);
    QStringList list = m_setCodeException.values();
    list.sort();
    return list;
}

int WaveManager::sumLocation() const
{
    return m_pBuffer->uniqueValueCount();
}

QSet<QString> WaveManager::getUnsortedCodes() const
{
    std::unique_lock<std::mutex> lock(m_lock);
    QSet<QString> unsorted = m_setCodeRecv;
    unsorted.subtract(m_setCodeSorted);
    unsorted.subtract(m_setCodeException);
    return unsorted;
}

void WaveManager::clearWave()
{
    std::unique_lock<std::mutex> lock(m_lock);
    m_setCodeRecv.clear();
    m_setCodeSorted.clear();
    m_setCodeException.clear();
    m_mapCodeRetry.clear();
    m_setCodeProcessing.clear();
    // ★ 2026-09-09 需求8：清波次时累计计数一并清零
    m_sortedTotal    = 0;
    m_exceptionTotal = 0;
    // ★ 上波次记录：清空前把当前波次号保存为“上波次”（覆盖/新任务/切出场景 UI 显示用）
    if (!m_orderCode.isEmpty())
        m_lastOrderCode = m_orderCode;
    m_orderCode.clear();
    m_orderQty = 0;
    m_bSortingStarted = false;
    m_bReported.store(false);
    m_hasFullboxRecord.store(false);   // ★ 2026-09-07 修复：清空时同步复位满箱标记，防 IDLE 下 isSortingStarted() 误判
    WCS_INFO("[WaveMgr] 波次已清理");
    // ★ 2026-09-07 修复：BOUND/CREATED/SORTING/ENDING/FULLBOX_SYNC 等状态白名单不允许迁移 IDLE，
    //   但 clearWave 语义即"强制清空回空闲"（H4覆盖/切换切出/新任务/取消收尾共用）。
    //   此前 setState(WAVE_IDLE) 被拒后状态残留（如切到"已绑定"波次后再切其它，
    //   restoreWave 因"当前状态非IDLE"拒绝 → 无法切换，现场实测）。白名单拒绝时直接置位。
    if (!setState(WAVE_IDLE))
    {
        m_waveStatus.store(WAVE_IDLE);
        emit waveStatusChanged(WAVE_IDLE);
    }
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
// ★ 2026-09-08 幂等化：满箱回传已解耦状态机（不再进入 FULLBOX_SYNC），
//   当前状态已是 SORTING 时直接视为成功（多格口并发满箱时回执乱序安全），不再告警
// ============================================================================
bool WaveManager::resumeSorting()
{
    int current = m_waveStatus.load();
    if (current == WAVE_SORTING)
    {
        m_hasFullboxRecord.store(true);
        return true;   // 幂等：状态已是分拣中（多格口并发满箱回执乱序/未进入同步态）
    }
    if (current != WAVE_FULLBOX_SYNC)
    {
        WCS_WARN("[WaveMgr] 恢复分拣失败 当前状态非SORTING/FULLBOX_SYNC status=%d(%s)",
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
// ★ 纠正：不受状态影响，只要不是异常（CANCELLED）或挂起（HELD），点击「结束任务」即可触发完结
//   不再要求状态=SORTING，也不再要求计划完成（sorted+exception >= total）
//   完成后状态重新变为空闲（IDLE），可接收新波次
// ============================================================================
bool WaveManager::canComplete() const
{
    int status = m_waveStatus.load();

    // 仅拦截异常状态：已取消（CANCELLED）或异常挂起（HELD）
    if (status == WAVE_CANCELLED || status == WAVE_HELD)
    {
        WCS_WARN("[WaveMgr] 完结条件不满足 波次状态异常 status=%d(%s)",
            status, WaveSnapshot::statusToString(status).toLocal8Bit().data());
        return false;
    }

    WCS_INFO("[WaveMgr] 完结条件满足 orderCode=%s status=%d(%s) 手动触发完结",
        m_orderCode.toLocal8Bit().data(), status, WaveSnapshot::statusToString(status).toLocal8Bit().data());
    return true;
}

// ============================================================================
// completeToEnding — 手动触发完结
// 校验 canComplete()（仅拦截 CANCELLED/HELD）后，迁移当前状态→ENDING
// ★ 不受状态影响，完成后 WMS 回传成功 → IDLE（空闲）
// ============================================================================
bool WaveManager::completeToEnding()
{
    if (!canComplete())
    {
        return false;
    }

    // 如果状态已经是 ENDING（如 markSorted 自动触发），幂等返回
    if (m_waveStatus.load() == WAVE_ENDING)
    {
        WCS_INFO("[WaveMgr] 手动触发完结 状态已是ENDING（幂等） orderCode=%s",
            m_orderCode.toLocal8Bit().data());
        return true;
    }

    int oldStatus = m_waveStatus.load();
    if (!setState(WAVE_ENDING))
    {
        WCS_WARN("[WaveMgr] 手动触发完结 状态迁移失败 current=%d(%s) orderCode=%s",
            oldStatus, WaveSnapshot::statusToString(oldStatus).toLocal8Bit().data(),
            m_orderCode.toLocal8Bit().data());
        return false;
    }

    WCS_INFO("[WaveMgr] 手动触发完结 %s→ENDING orderCode=%s",
        WaveSnapshot::statusToString(oldStatus).toLocal8Bit().data(),
        m_orderCode.toLocal8Bit().data());
    return true;
}

// ============================================================================
// reconcile — 波次对账
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
        r.totalRecv    = m_orderQty;  // ★ 总件数，非去重SKU数

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

// ★ 2026-09-14 仅供无人值守 E2E 的强制取消（跳过"已开始分拣"判定）
//   目的：让"结束当前波次 → 接替下一波次 → 挂起件补发"这条链路可在无人工点击时验证。
//   调用方负责校验环境变量；生产路径不会走到这里。
bool WaveManager::forceCancelForE2E()
{
    std::unique_lock<std::mutex> cancelLock(m_cancelSortMutex);
    const int prevStatus = m_waveStatus.load();
    if (!setState(WAVE_CANCELLED))
    {
        WCS_WARN("[WaveMgr] E2E强制取消失败 状态迁移被拒 order=%s prev=%d now=%d",
            m_orderCode.toLocal8Bit().data(), prevStatus, m_waveStatus.load());
        return false;
    }
    WCS_WARN("[WaveMgr] E2E强制取消成功（测试钩子）order=%s %d→CANCELLED",
        m_orderCode.toLocal8Bit().data(), prevStatus);
    return true;
}
