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
#include <QSqlDatabase>
#include <QMutex>
#include <QMutexLocker>

// ──── 分拣记录结构（原有，保留兼容）────
struct SortingRecord
{
    int     id          = 0;
    QString orderCode;
    QString barcode;
    QString gridNum;
    QString carNum;
    int     gridCount   = 0;
    QString volu;
    QString sortTime;
    QString createTime;
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

// ──── S0 新增：退货波次头 ────
struct ReturnWaveRecord
{
    QString orderCode;
    int     orderQty   = 0;
    int     status     = 0;   // WaveStatus 枚举值
    QString createdAt;
    QString updatedAt;
};

// ──── S0 新增：波次明细 ────
struct ReturnWaveItemRecord
{
    int     id          = 0;
    QString orderCode;
    QString inco;
    QString gridNum;
    QString gridType    = "普通格口";
    int     planQty     = 0;
    int     sortedQty   = 0;
    QString volu;
};

// ──── S0 新增：格口容器绑定 ────
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

// ──── S0 新增：分拣流水 ────
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

// ──── S0 新增：出站消息 ────
struct OutboxRecord
{
    QString msgId;
    QString orderCode;
    QString boxcode;        // 仅满箱回传（H7）使用
    QString payload;
    QString status          = "pending";
    int     retryCount      = 0;
    QString nextRetry;
    QString createdAt;
};

// ──── S0 新增：异常记录 ────
struct ExceptionRecord
{
    int     id          = 0;
    QString type;
    QString orderCode;
    QString epc;
    QString sku;
    QString reason;
    bool    handled     = false;
    QString time;
};

class SortingDatabase
{
public:
    SortingDatabase();
    ~SortingDatabase();

    // ──── 生命周期 ────
    bool open(const QString& dbPath = QString());
    void close();
    bool isOpen() const;

    // ═══════════════════════════════════════════════════════════════
    // 原有接口（保留兼容）
    // ═══════════════════════════════════════════════════════════════
    bool insertRecord(const QString& orderCode, const QString& barcode,
                      const QString& gridNum, const QString& carNum,
                      int gridCount, const QString& volu);
    QVector<SortingRecord> queryByBarcode(const QString& barcode, int limit = 500);
    QVector<SortingRecord> queryByTime(const QDateTime& from, const QDateTime& to, int limit = 1000);
    QVector<SortingRecord> queryByOrderCode(const QString& orderCode, int limit = 1000);
    QVector<SortingRecord> queryAll(int limit = 1000);
    QVector<SortingRecord> queryAllWithPending(int limit = 1000);  // ★ 留空查全部：已分拣 + 待分拣
    SortingStatistics statistics();
    int recordCount() const;
    int todayRecordCount() const;
    void cleanupOldRecords(int retainDays = 30);

    // ═══════════════════════════════════════════════════════════════
    // S0 新增：波次管理（T-S0-02）
    // ═══════════════════════════════════════════════════════════════

    // 插入/更新波次头（幂等，未分拣时允许覆盖）
    bool upsertReturnWave(const QString& orderCode, int orderQty, int status);
    // 获取波次头
    ReturnWaveRecord getReturnWave(const QString& orderCode);
    // 更新波次状态
    bool updateWaveStatus(const QString& orderCode, int newStatus);
    // 获取波次状态（快速查询，用于状态机判定）
    int getWaveStatus(const QString& orderCode);

    // 插入波次明细（先清旧再插新，支持覆盖重下）
    bool insertWaveItems(const QString& orderCode, const QVector<ReturnWaveItemRecord>& items);
    // 获取波次明细
    QVector<ReturnWaveItemRecord> getWaveItems(const QString& orderCode);
    // 增量已分拣件数（sorted_qty + 1）
    bool incrementSortedQty(const QString& orderCode, const QString& inco, const QString& gridNum);

    // ═══════════════════════════════════════════════════════════════
    // S0 新增：容器绑定（T-S0-02）
    // ═══════════════════════════════════════════════════════════════

    // 绑定容器（先归档旧绑定，再插入新绑定）
    bool bindGridBox(const QString& gridNum, const QString& boxcode, const QString& orderCode);
    // 获取格口当前活跃绑定
    GridBoxBindRecord getActiveBind(const QString& gridNum);
    // 归档指定格口的所有活跃绑定（满箱/取消时调用）
    bool archiveGridBinds(const QString& gridNum);

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
    // 按 msgId 查询单条出站消息（人工重发用）
    OutboxRecord getOutboxFullboxByMsgId(const QString& msgId);
    OutboxRecord getOutboxEndByMsgId(const QString& msgId);     // ★ S6 完结回传按 msgId 查询（H8）

    // ═══════════════════════════════════════════════════════════════
    // S0 新增：异常记录（T-S0-02）
    // ═══════════════════════════════════════════════════════════════

    // 插入异常记录
    bool insertException(const ExceptionRecord& ex);
    // 查询未处理异常
    QVector<ExceptionRecord> getOpenExceptions(const QString& orderCode);
    // ★ S7 多条件异常查询（T-S7-03）
    QVector<ExceptionRecord> queryExceptions(const QString& orderCode, const QString& epc,
                                              const QString& type, const QString& startTime,
                                              const QString& endTime, int limit = 100);

    // ──── 维护 ────
    QString databasePath() const { return m_dbPath; }

private:
    void createTables();        // 建表（含 S0 新增表）
    QString currentTimeStr() const;
    QSqlDatabase ensureConnection();  // ★ 跨线程安全：在调用线程中按需创建数据库连接

    QString         m_dbPath;
    QString         m_connectionName;
    mutable QMutex  m_mutex;
    bool            m_bOpened = false;
};