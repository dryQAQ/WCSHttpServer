#pragma once
// ============================================================================
// SortingDatabase.h — 分拣记录 SQLite 本地存储（S0 阶段扩展）
//
// 仿照 WCSApp DataCenter 的设计，使用 Qt QSqlDatabase (SQLite 驱动)
// S0 阶段新增 7 张核心表：return_wave, return_wave_item, grid_box_bind,
//   sort_txn, outbox_fullbox, outbox_end, exception_record
//
// 原有表: sorting_records（保留，兼容现有查询功能）
// ============================================================================

#include <QObject>
#include <QString>
#include <QDateTime>
#include <QVector>
#include <QSet>
#include <QSqlDatabase>
#include <QThread>
#include <QThreadStorage>
#include <QAtomicInt>
#include <type_traits>
#include "LogService.h"

// ──── 分拣记录结构（原有，保留兼容）────
struct SortingRecord
{
    int     id          = 0;
    QString orderCode;
    QString barcode;       // ★ EPC编码
    QString sku;           // ★ SKU编码（RFID绑定获取）
    QString gridNum;
    QString carNum;        // 小车号（3字段格式时=小车，5字段格式时=首车）
    QString firstCar;      // ★ 首车号（5字段 PLC 反馈格式专用）
    QString lastCar;       // ★ 尾车号（5字段 PLC 反馈格式专用）
    int     gridCount   = 0;
    QString volu;
    QString sortTime;
    QString createTime;
    QString boxcode;       // ★ 2026-09-09 需求6：落格时的容器号（行进中换容器按新绑定记录）
    QString status;         // 分拣状态：已分拣 / 待分拣
};

// ──── 统计信息（原有，保留兼容）────
struct SortingStatistics
{
    int totalRecords    = 0;
    int todayRecords    = 0;
    int totalWaves      = 0;
    int totalGrids      = 0;
    QString lastSortTime;
};

// ★ 2026-09-09 需求2：按格口汇总（每个格口一行：分拣数量等）
struct GridSummaryRecord
{
    QString gridNum;
    int     sortedCount  = 0;   // 分拣件数（sorting_records 行数）
    int     skuCount     = 0;   // 涉及的 SKU 数
    QString boxcode;            // 最近落格的容器号
    QString lastSortTime;       // 最近分拣时间
};

// ──── 新增：退货波次头 ────
struct ReturnWaveRecord
{
    QString orderCode;
    int     orderQty   = 0;
    int     status     = 0;   // WaveStatus 枚举值
    QString createdAt;
    QString updatedAt;
};

// ★ 2026-09-06 波次记录（UI「波次数据记录」列表行：波次头 + 进度计数）
struct WaveRecordProgress
{
    QString orderCode;
    int     orderQty      = 0;
    int     status        = 0;   // WaveStatus 枚举值
    QString createdAt;
    QString updatedAt;
    int     sortedCount   = 0;   // 已分拣件数（sorting_records 去重计数）
    int     exceptionCount = 0;  // 异常件数（exception_record 去重计数）
};

// ──── 新增：波次明细 ────
struct ReturnWaveItemRecord
{
    int     id          = 0;
    QString orderCode;
    QString inco;
    QString gridNum;
    QString gridType    = "0";  // 0=分类, 1=异常, 2=发货
    int     planQty     = 0;
    int     sortedQty   = 0;
    QString volu;
    QString obxCode;        // 容器号（WMS 下发时携带）
};

// ──── 新增：格口容器绑定 ────
struct GridBoxBindRecord
{
    int     id          = 0;
    QString gridNum;
    QString boxcode;
    QString orderCode;
    bool    active      = true;
    QString bindTime;
    QString unbindTime;
};

// ──── 新增：分拣流水 ────
struct SortTxnRecord
{
    int     id          = 0;
    QString orderCode;
    QString epc;
    QString sku;
    QString gridNum;
    QString boxcode;
    QString result      = "success";
    QString reason;
    QString sortTime;
};

// ──── 新增：出站消息 ────
struct OutboxRecord
{
    QString msgId;
    QString orderCode;
    QString boxcode;        // 仅满箱回传（H7）使用
    QString grid;           // ★ 2026-09-08 满箱回传（H7）对应格口号（失败格口下拉直接读取，免解析 payload）
    QString payload;
    QString status          = "pending";
    int     retryCount      = 0;
    QString nextRetry;
    QString createdAt;
};

// ──── 新增：异常记录 ────
struct ExceptionRecord
{
    int     id          = 0;
    QString type;
    QString orderCode;
    QString epc;
    QString sku;
    QString reason;
    bool    handled     = false;   // ★ 2026-09-13 已闭环（该 EPC 之后成功落格 → handled=1）
    QString time;
};

class SortingDatabase
{
public:
    static SortingDatabase& instance();  // ★ 单例

    // ──── 生命周期 ────
    bool open(const QString& dbPath = QString());
    void close();
    bool isOpen() const;

    // ═══════════════════════════════════════════════════════════════
    // 原有接口（保留兼容）
    // ═══════════════════════════════════════════════════════════════
    bool insertRecord(const QString& orderCode, const QString& barcode,
                      const QString& sku,
                      const QString& gridNum, const QString& carNum,
                      const QString& firstCar, const QString& lastCar,
                      int gridCount, const QString& volu,
                      const QString& boxcode = QString());  // ★ 2026-09-09 需求6：落格容器号（行进中换容器按新绑定记录）
    QVector<SortingRecord> queryByBarcode(const QString& barcode, int limit = 500);
    QVector<SortingRecord> queryByTime(const QDateTime& from, const QDateTime& to, int limit = 1000);
    QVector<SortingRecord> queryByOrderCode(const QString& orderCode, int limit = 1000);
    QVector<SortingRecord> queryAll(int limit = 1000);
    QVector<SortingRecord> queryAllWithPending(int limit = 1000);  // ★ 留空查全部：已分拣 + 待分拣
    QVector<SortingRecord> queryByGrid(const QString& gridNum, int limit = 1000);   // ★ 2026-09-09 需求2：按格口查分拣明细（★ 2026-09-10 兼容 "7"/"007"/"22007"）
    QVector<SortingRecord> queryBySku(const QString& sku, int limit = 1000);        // ★ 2026-09-10 需求1：按 SKU 查落格明细（EPC ↔ 实际落格号）
    // ★ 2026-09-13 需求：按容器号查该容器下的所有 EPC 物件明细（按落格时间正序，便于核对装箱顺序）
    QVector<SortingRecord> queryByBoxcode(const QString& boxcode, int limit = 1000);
    // ★ 2026-09-13 需求：批量反查一组 EPC 各自出现过的"其他容器号"（key=EPC，value=其他容器号列表）
    //   用途：按容器号查询时判断"同一 EPC 是否被分到过别的容器"（跨容器漂移）
    QMap<QString, QStringList> queryOtherBoxcodesByEpc(const QStringList& epcs,
                                                       const QString& excludeBox = QString());
    // ★ 2026-09-11 重扫重投：某波次某 EPC 的首条落格号（无记录返回空串）
    QString getFirstSortedGrid(const QString& orderCode, const QString& epc);
    QVector<GridSummaryRecord> queryGridSummary();                                   // ★ 2026-09-09 需求2：全格口汇总（分拣数量）
    QVector<ReturnWaveItemRecord> querySkuGridMapping(const QString& sku, const QString& orderCode = "");  // ★ 按 SKU 查询格口分配
    SortingStatistics statistics();
    int recordCount();
    int todayRecordCount();
    void cleanupOldRecords(int retainDays = 30);

    // ═══════════════════════════════════════════════════════════════
    // 新增：波次管理
    // ═══════════════════════════════════════════════════════════════

    // 插入/更新波次头（幂等，未分拣时允许覆盖）
    bool upsertReturnWave(const QString& orderCode, int orderQty, int status);
    // 获取波次头
    ReturnWaveRecord getReturnWave(const QString& orderCode);
    // 更新波次状态
    bool updateWaveStatus(const QString& orderCode, int newStatus);
    // 获取波次状态（快速查询，用于状态机判定）
    int getWaveStatus(const QString& orderCode);
    // 查询最近一条未完成波次（排除已取消和已完成），用于软件重启后恢复波次数据
    ReturnWaveRecord getLatestUnfinishedWave();
    // ★ 2026-09-06：查询全部已传输波次（含已完成/已取消）+ 进度计数，UI「波次数据记录」列表用
    QVector<WaveRecordProgress> getAllWaves();
    // 查询全部未完成波次（排除已取消和已完成），供未完成波次手动重传面板展示
    QVector<ReturnWaveRecord> getAllUnfinishedWaves();

    // ═══════════════════════════════════════════════════════════════
    // 波次恢复查询（上一波次任务恢复用）
    // ═══════════════════════════════════════════════════════════════

    // 某波次全部已分拣 EPC（sorting_records，barcode=EPC）
    QSet<QString> getSortedEpcsByOrder(const QString& orderCode);
    // 某波次全部异常 EPC（exception_record）
    QSet<QString> getExceptionEpcsByOrder(const QString& orderCode);
    // 某波次是否存在成功满箱回传（H7）
    bool hasSuccessFullbox(const QString& orderCode);
    // H4 原始报文落库（单独表 wave_raw，INSERT OR REPLACE）
    void saveWaveRawPayload(const QString& orderCode, const QByteArray& body);
    // 查询某波次 H4 原始报文（追溯用）
    QByteArray getWaveRawPayload(const QString& orderCode);

    // ═══════════════════════════════════════════════════════════════
    // 每日峰值效率（2026-09-07：波次面板峰值效率持久化）
    // ═══════════════════════════════════════════════════════════════

    // 保存某日 1 分钟窗口件数峰值（INSERT OR REPLACE，peak_per_hour=×60 一并存）
    bool saveDailyPeak(const QString& date, int peakPerMinute);
    // 查询某日峰值（1 分钟窗口件数口径；无记录返回 0）
    int  getDailyPeakPerMinute(const QString& date);

    // 插入波次明细（先清旧再插新，支持覆盖重下）
    bool insertWaveItems(const QString& orderCode, const QVector<ReturnWaveItemRecord>& items);
    // 获取波次明细
    QVector<ReturnWaveItemRecord> getWaveItems(const QString& orderCode);
    // 增量已分拣件数（sorted_qty + 1）
    bool incrementSortedQty(const QString& orderCode, const QString& inco, const QString& gridNum);

    // ═══════════════════════════════════════════════════════════════
    // S0 新增：容器绑定（T-S0-02）
    // ═══════════════════════════════════════════════════════════════

    // 绑定容器（先归档旧绑定，再插入新绑定；orderCode 关联所属波次，供波次切换恢复绑定视图）
    bool bindGridBox(const QString& gridNum, const QString& boxcode, const QString& orderCode = QString());
    // ★ 2026-09-06 按波次查询绑定快照（每格取该波次最近一条绑定，含已归档）
    QMap<QString, QString> getBindsByOrder(const QString& orderCode);
    // ★ 2026-09-07 每格最近一次绑定（无当前 active 绑定时"沿用上一波次绑定"用）
    QMap<QString, QString> getLastKnownBinds();
    // 获取格口当前活跃绑定
    GridBoxBindRecord getActiveBind(const QString& gridNum);
    // 归档指定格口的所有活跃绑定（满箱/取消时调用）
    bool archiveGridBinds(const QString& gridNum);
    // 获取全部活跃绑定（程序重启后加载内存/UI 用）
    QVector<GridBoxBindRecord> getAllActiveBinds();
    // 归档全部活跃绑定（波次完结/取消时清空全部格口绑定）
    bool archiveAllBinds();

    // ═══════════════════════════════════════════════════════════════
    // S0 新增：完结波次历史存档（T-S0-02）
    // ═══════════════════════════════════════════════════════════════

    // 将指定波次的完整数据快照到历史数据库 wave_history.db
    // 包含：波次头、波次明细、分拣记录、异常记录、满箱回传、完结回传
    bool archiveWave(const QString& orderCode);

    // ═══════════════════════════════════════════════════════════════
    // S0 新增：分拣流水（T-S0-02）
    // ═══════════════════════════════════════════════════════════════

    // 插入分拣流水
    bool insertSortTxn(const SortTxnRecord& txn);
    // 检查 EPC 是否已成功分拣（防重）
    bool isEpcAlreadySorted(const QString& orderCode, const QString& epc);

    // ═══════════════════════════════════════════════════════════════
    // S0 新增：Outbox 出站消息（T-S0-05）
    // ═══════════════════════════════════════════════════════════════

    // 插入满箱回传出站消息（H7 满箱同步到WMS）
    bool insertOutboxFullbox(const OutboxRecord& msg);
    // 插入完结回传出站消息（H8 波次完结通知WMS）
    bool insertOutboxEnd(const OutboxRecord& msg);
    // 查询待重试的满箱回传出站消息（H7）
    QVector<OutboxRecord> getPendingOutboxFullbox(int limit = 10);
    // 查询待重试的完结回传出站消息（H8）
    QVector<OutboxRecord> getPendingOutboxEnd(int limit = 10);
    // 更新满箱回传出站消息状态（H7，重试用）
    bool updateOutboxFullboxStatus(const QString& msgId, const QString& newStatus, const QString& nextRetry);
    // 更新完结回传出站消息状态（H8，重试用）
    bool updateOutboxEndStatus(const QString& msgId, const QString& newStatus, const QString& nextRetry);
    // 标记满箱回传出站成功（H7）
    bool markOutboxFullboxSuccess(const QString& msgId);
    // 标记完结回传出站成功（H8）
    bool markOutboxEndSuccess(const QString& msgId);
    // 按波次号查询待重试出站消息（人工重发用）
    QVector<OutboxRecord> getOutboxByOrderCode(const QString& orderCode);
    // 按波次号查询全部 满箱回传（H7）出站消息（含状态，供未完成波次面板展示/重传）
    QVector<OutboxRecord> getOutboxFullboxByOrder(const QString& orderCode);
    // 按波次号查询全部 完结回传（H8）出站消息（含状态，供未完成波次面板展示/重传）
    QVector<OutboxRecord> getOutboxEndByOrder(const QString& orderCode);
    // 按 msgId 查询单条出站消息（人工重发用）
    OutboxRecord getOutboxFullboxByMsgId(const QString& msgId);
    OutboxRecord getOutboxEndByMsgId(const QString& msgId);     // ★ S6 完结回传按 msgId 查询（H8）
    // ★ 2026-09-08 UI 失败重传下拉：查询全部"重试耗尽失败/已取消重试"的满箱（H7）出站消息
    QVector<OutboxRecord> getFailedOutboxFullbox(int limit = 200);
    // ★ 2026-09-08 UI 失败重传下拉：查询全部"重试耗尽失败/已取消重试"的完结（H8）出站消息
    QVector<OutboxRecord> getFailedOutboxEnd(int limit = 200);

    // ═══════════════════════════════════════════════════════════════
    // S0 新增：异常记录（T-S0-02）
    // ═══════════════════════════════════════════════════════════════

    // 插入异常记录
    bool insertException(const ExceptionRecord& ex);
    // ★ 2026-09-13 异常及时清理：把某波次某 EPC 的未处理异常留痕标记为已处理（handled=1）
    //   触发时机：该 EPC 之后成功落格（异常数已 −1），留痕归档为"已处理"供弹窗/对账区分
    bool markExceptionResolved(const QString& orderCode, const QString& epc);
    // 查询未处理异常
    QVector<ExceptionRecord> getOpenExceptions(const QString& orderCode);
    // ★ S7 多条件异常查询（T-S7-03）
    QVector<ExceptionRecord> queryExceptions(const QString& orderCode, const QString& epc,
                                              const QString& type, const QString& startTime,
                                              const QString& endTime, int limit = 100);
    // ★ 2026-09-09 需求3：批量取异常原因 epc → "type: reason"（EPC查询面板状态列显示异常原因）
    QHash<QString, QString> queryExceptionReasons(const QString& orderCode = QString());

    // ──── 维护 ────
    QString databasePath() const { return m_dbPath; }

private:
    SortingDatabase();
    ~SortingDatabase();
    SortingDatabase(const SortingDatabase&) = delete;
    SortingDatabase& operator=(const SortingDatabase&) = delete;

    void createTables();        // 建表
    QString currentTimeStr() const;

    // ★ 获取当前线程的只读查询连接（懒创建，每线程独立）
    //   WAL 模式下只读连接与 DB 线程的写连接并行工作，落库任务占用 DB 线程时不阻塞调用线程
    //   用途：UI 查询接口（statistics/queryByBarcode/queryAllWithPending/cleanupOldRecords）
    QSqlDatabase queryDb() const;

    // ★ 在专用 DB 线程上执行操作（阻塞调用线程，等待完成）
    //   所有 SQLite 操作必须通过此方法委托到 DB 线程执行
    template<typename Func>
    auto runOnDbThread(Func&& func) -> decltype(func())
    {
        using ReturnType = decltype(func());

        // ★ 防护：数据库未初始化，直接返回默认值
        if (!m_pDbTarget) {
            Data_WARN("[SortingDB] runOnDbThread 失败: m_pDbTarget 为空 (DB 未初始化，请先调用 open())");
            if constexpr (std::is_void_v<ReturnType>)
                return;
            else
                return ReturnType{};
        }

        quintptr callerTid = (quintptr)QThread::currentThreadId();
        Data_INFO("[SortingDB] runOnDbThread 提交操作 callerTid=%llu targetTid=%llu",
            (unsigned long long)callerTid,
            (unsigned long long)(m_pDbTarget ? (quintptr)m_pDbTarget->thread()->currentThreadId() : 0));

        if constexpr (std::is_void_v<ReturnType>)
        {
            QMetaObject::invokeMethod(m_pDbTarget, [&]() {
                quintptr dbTid = (quintptr)QThread::currentThreadId();
                Data_INFO("[SortingDB] runOnDbThread( void) 开始执行 dbTid=%llu m_bOpened=%d",
                    (unsigned long long)dbTid, (int)m_bOpened);
                func();
                Data_INFO("[SortingDB] runOnDbThread( void) 执行完成 dbTid=%llu",
                    (unsigned long long)dbTid);
            }, Qt::BlockingQueuedConnection);
        }
        else
        {
            ReturnType result{};
            QMetaObject::invokeMethod(m_pDbTarget, [&]() {
                quintptr dbTid = (quintptr)QThread::currentThreadId();
                Data_INFO("[SortingDB] runOnDbThread(非void) 开始执行 dbTid=%llu m_bOpened=%d",
                    (unsigned long long)dbTid, (int)m_bOpened);
                result = func();
                Data_INFO("[SortingDB] runOnDbThread(非void) 执行完成 dbTid=%llu",
                    (unsigned long long)dbTid);
            }, Qt::BlockingQueuedConnection);
            return result;
        }
    }

    QString         m_dbPath;
    QThread*        m_pDbThread  = nullptr;  // ★ 专用数据库线程（单连接）
    QObject*        m_pDbTarget  = nullptr;  // ★ DB 线程上的事件接收者
    QAtomicInt      m_bOpened{0};            // ★ 原子标记，跨线程安全读取
    // ★ UI 只读查询连接缓存（每线程独立；线程退出时自动析构，无泄漏）
    mutable QThreadStorage<QSqlDatabase> m_queryConns;
};