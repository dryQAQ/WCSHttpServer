#pragma once
// ============================================================================
// WaveManager.h — 波次生命周期管理 + 分拣状态跟踪
//
// 职责:
//   ① 维护波次状态机（10 状态，S0 升级）
//   ② 跟踪每个 inco 的分拣状态（已分拣/异常/处理中/重试次数）
//   ③ 判定波次完结条件（全部完成 或 超时）
//   ④ 提供查询接口供 UI 和内部 API 使用
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

// ──── 波次状态枚举（S0 升级：与需求 §3.1 对齐）────
enum WaveStatus
{
    WAVE_IDLE         = 0,  // 空闲，可接收新波次
    WAVE_CREATED      = 1,  // 已下发（波次下发成功落库，尚未绑定或未开工）
    WAVE_BOUND        = 2,  // 已绑定（至少一个业务格口完成有效容器绑定）
    WAVE_SORTING      = 3,  // 分拣中（已开始分拣）
    WAVE_FULLBOX_SYNC = 4,  // 满箱同步中（正在调用/重试满箱回传）
    WAVE_CANCEL_PENDING = 5, // 取消处理中（收到波次取消请求，判定中）
    WAVE_CANCELLED    = 6,  // 已取消（波次取消成功，终态）
    WAVE_ENDING       = 7,  // 完结中（正在调用/重试完结回传）
    WAVE_FINISHED     = 8,  // 已完成（完结回传成功，终态）
    WAVE_HELD         = 9,  // 异常挂起（接口失败/数据冲突，待人工）

    // 兼容旧名称别名
    WAVE_COMPLETING   = WAVE_ENDING,   // 回传进行中
    WAVE_CLEANED      = WAVE_FINISHED, // 回传成功
    WAVE_ERROR        = WAVE_HELD,     // 回传失败
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
    int     sumLocation = 0;   // 落格分拣总件数（供WMS回传H8的sumLocation字段）
    QString lastWaveCode;      // ★ 上一个波次号（本次会话内被覆盖/切出的最近波次，供UI「上波次」显示）

    static QString statusToString(int s)
    {
        switch (s) {
        case WAVE_IDLE:           return QString::fromUtf8("空闲");
        case WAVE_CREATED:        return QString::fromUtf8("已下发");
        case WAVE_BOUND:          return QString::fromUtf8("已绑定");
        case WAVE_SORTING:        return QString::fromUtf8("分拣中");
        case WAVE_FULLBOX_SYNC:   return QString::fromUtf8("满箱同步中");
        case WAVE_CANCEL_PENDING: return QString::fromUtf8("取消处理中");
        case WAVE_CANCELLED:      return QString::fromUtf8("已取消");
        case WAVE_ENDING:         return QString::fromUtf8("完结中");
        case WAVE_FINISHED:       return QString::fromUtf8("已完成");
        case WAVE_HELD:           return QString::fromUtf8("异常挂起");
        default:                  return QString::fromUtf8("未知");
        }
    }
};

// ──── S8 波次对账结果（T-S8-01/02，需求 §14.2）────
struct WaveReconciliation
{
    QString orderCode;          // 波次号
    int     planQty    = 0;     // 计划总件数（orderQty）
    int     sortedQty  = 0;     // 实分成功件数（Σ sorted）
    int     exceptionQty = 0;   // 实分异常件数（Σ exception）
    int     totalRecv  = 0;     // 波次接收到的总识别码数（= sorted + exception）
    int     fullboxSuccessCount = 0; // 满箱回传成功次数（H7，DB查询）
    int     fullboxPendingCount = 0; // 满箱回传待发送/重试中次数（H7，DB查询）
    int     unhandledException = 0; // 未处理异常记录数（DB查询）
    int     waveStatus = WAVE_IDLE; // 当前波次状态
    QString statusText;          // 状态文本
    bool    endReportSuccess = false;   // 完结回传是否已成功（H8，FINISHED状态）

    // 差异指标
    bool    hasDiff() const      // 是否存在差异
    {
        return (planQty > 0 && (sortedQty + exceptionQty) != totalRecv)
            || (fullboxPendingCount > 0)
            || (unhandledException > 0)
            || (waveStatus == WAVE_HELD);
    }
    QString diffSummary() const  // 差异摘要
    {
        QStringList diffs;
        if (planQty > 0 && (sortedQty + exceptionQty) != totalRecv)
            diffs << QString("计划%1≠接收%2").arg(planQty).arg(totalRecv);
        if (fullboxPendingCount > 0)
            diffs << QString("H7待发送%1条").arg(fullboxPendingCount);
        if (unhandledException > 0)
            diffs << QString("未处理异常%1条").arg(unhandledException);
        if (waveStatus == WAVE_HELD)
            diffs << "任务异常挂起";
        return diffs.isEmpty() ? QString::fromUtf8("无差异") : diffs.join(", ");
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

    // 获取波次中所有EPC编码列表（快照诊断用）
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

    // ──── 波次恢复（上一波次任务恢复用）────
    // 程序重启后把未完成波次恢复到内存（状态/进度集合/计数），继续上次任务。
    // targetStatus 仅允许 CREATED/BOUND/SORTING/ENDING（由调用方按映射规则给出）；
    // 前置条件：当前状态必须为 IDLE（调用方保证）。
    // 内部按 IDLE→CREATED→BOUND→SORTING / →ENDING 的合法链式迁移，不破坏状态机。
    bool restoreWave(const QString& orderCode, int orderQty, int skuCount,
                     int targetStatus,
                     const QSet<QString>& recvSet,
                     const QSet<QString>& sortedSet,
                     const QSet<QString>& exceptionSet,
                     bool hasFullboxRecord);

    // ──── 分拣状态（线程安全，内部加锁）────
    void markSorted(const QString& code);       // 标记已分拣
    void markException(const QString& code);    // 标记异常
    bool isSorted(const QString& code) const;   // 是否已分拣
    bool isCodeSorted(const QString& code) const; // ★ S7 是否已分拣（含DB防重，T-S7-02）
    bool isException(const QString& code) const;// 是否异常
    int  retryCount(const QString& code) const; // 获取重试次数
    int  sortedCountByGrid(const QString& grid) const; // ★ S7 按格口统计已分拣数（T-S7-06）

    // ──── 波次判定 ────
    bool isWaveComplete() const;                // 检查波次是否完成（无锁读）
    bool checkWaveCompleteLocked() const;       // 检查波次是否完成（需持有m_lock）

    // ──── 状态查询 ────
    WaveSnapshot snapshot() const;              // 获取波次快照（供UI）
    int status() const { return m_waveStatus.load(); }
    bool setState(int newStatus);               // 状态转换（带合法性校验）
    bool startSorting();                        // ★ S4 开工：BOUND→SORTING（T-S4-05）
    // ──── S0 新增：已开始分拣判定（T-S0-04）────
    // 满足任一条件即视为已开始分拣（需求 §3.3）：
    //   1. 任务状态 ∈ {SORTING, FULLBOX_SYNC, ENDING, FINISHED, HELD}
    //   2. 成功分拣计数 > 0（sorted > 0）
    //   3. 已存在任意成功的满箱回传记录（H7，通过 hasFullboxRecord 查询）
    //   4. 现场已点击「开始分拣」（m_bSortingStarted == true）
    bool isSortingStarted() const;

    // 获取当前波次 sortedQty 总数（用于波次取消判定）
    int sortedQty() const { return sorted(); }

    // 是否已存在满箱回传记录（用于波次取消判定，S5 阶段通过 DB 查询实现）
    bool hasFullboxRecord() const;

    // ──── S5 新增：满箱管理 ────
    bool triggerFullbox();                           // 满箱触发：SORTING→FULLBOX_SYNC（T-S5-01）
    bool resumeSorting();                            // 满箱成功后恢复：FULLBOX_SYNC→SORTING（T-S5-05）
    bool holdAfterFullboxFail();                     // 满箱失败耗尽：FULLBOX_SYNC→HELD（T-S5-04）

    // ──── S6 新增：完结回传管理（H8）────
    bool canComplete() const;                        // 可完结条件检查（T-S6-01）
    bool completeToEnding();                         // 手动触发完结：SORTING→ENDING（T-S6-01/04）
    QString orderCode() const { return m_orderCode; }
    int     totalRecv() const;
    int     sorted() const;
    int     exception() const;
    int     sumLocation() const;                // 去重格口总数（供WMS回传的sumLocation字段）
    int     orderQty() const { return m_orderQty; }  // ★ S8 波次总件数（对账用，T-S8-01）
    QSet<QString> getUnsortedCodes() const;          // 获取未分拣的EPC列表（received - sorted - exception）

    // ──── S8 新增：对账 + H5互斥（T-S8-01/02/06）────
    WaveReconciliation reconcile() const;            // 波次对账：计划/实分/异常/完结状态
    bool tryCancelWave();                            // ★ H5取消：加锁后判定isSortingStarted+原子迁移（T-S8-06）

    // ──── 管理 ────
    void clearWave();                           // 清理当前波次数据，状态→IDLE
    void setMaxRetry(int n) { m_maxRetry = n; }
    void setWaveTimeoutMin(int m) { m_waveTimeoutMin = m; }

signals:
    void waveReceived(const QString& orderCode, int skuCount);
    void waveSortingStarted(const QString& orderCode);     // 首次分拣时触发
    void waveReadyToReport(const QString& orderCode);      // 波次可回传（全部完成或超时）
    void waveStatusChanged(int newStatus);
    void waveBound(const QString& orderCode);  // 波次绑定完成（容器绑定全部绑完时触发）
    void codeMarked(const QString& code, bool sorted);     // sorted=true=分拣完成, false=异常

private:
    // ──── 数据源 ────
    GridBuffer*     m_pBuffer;               // DoubleBuffer 格口映射（只读，无需锁）

    // ──── 波次基本信息 ────
    QString         m_orderCode;             // 当前波次号
    QString         m_lastOrderCode;         // ★ 上一个波次号（覆盖/切出时记录，UI「上波次」显示）
    int             m_orderQty         = 0;  // 波次总件数
    std::atomic<int> m_waveStatus{WAVE_IDLE};// 波次状态（原子操作，跨线程安全）
    QDateTime       m_waveStartTime;         // 波次开始时间
    bool            m_bSortingStarted  = false;  // 是否已开始分拣（防止重复发射 waveSortingStarted）
    std::atomic<bool> m_hasFullboxRecord{false}; // ★ S5：是否已存在成功满箱回传记录（波次取消判定用，HttpServer成功时设置）
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
    mutable std::mutex m_cancelSortMutex;         // ★ S8 H5取消与首件分拣互斥锁（T-S8-06）
};
