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
#include <QMap>
#include <QHash>
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

// ★ 2026-09-15 出站报文状态计数（批量统计返回单元；total() = 三类之和）
struct OutboxStatusCount
{
    int success = 0;   // status = 'success'
    int pending = 0;   // status ∈ {pending, cancelled, 其它}（"待发"口径，与逐波次统计一致）
    int failed  = 0;   // status = 'failed'
    int total() const { return success + pending + failed; }
};

// ★ 2026-09-15 已落格明细（切回波次时补齐"已落格进度"的唯一持久权威来源）
//   口径与实时落格路径逐条对齐：键 = (orderCode, EPC)，含落格当时的容器号/SKU/库位/时间
struct LandedRecord
{
    QString epc;
    QString sku;
    QString gridNum;
    QString boxcode;    // 落格时固化的容器号（切回时按"当前绑定容器"过滤，旧箱件不重复上报）
    QString volu;       // 来源库位
    QString time;       // 落格时间（sort_time）
    qint64  timeMs = 0; // 同上，epoch 毫秒（时间不可解析时为 0）
};

// ★ 2026-09-15 RFID 原始推送报文留痕（逐帧结构化，长期可追溯）
//   epc = 识别归一后用于分拣的 EPC（空 = NOREAD 帧）；epcRaw = 识别前的 EPC 原文
//   rawFrame = RFID 原样推送的整帧报文（含 {} 与协议字面帧尾 0D）；noread = 仅留痕不进入分拣
struct RfidRawRecord
{
    int     id       = 0;
    QString time;             // 落库时间 yyyy-MM-dd HH:mm:ss
    QString epc;              // 识别归一后 EPC（空 = NOREAD）
    QString epcRaw;           // 识别前 EPC 原文
    QString carNum;           // 小车号（流水号中的数字）
    QString seq;              // 流水号原文（如 SN0098）
    QString devCode;          // 设备编码（如 01）
    QString rawFrame;         // 整帧原文
    int     bytes    = 0;     // 整帧字节数
    bool    noread   = false; // 1 = NOREAD 帧
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
    QVector<SortingRecord> queryByBarcode(const QString& barcode, const QDateTime& from, const QDateTime& to,
                                          int limit = 500);
    QVector<SortingRecord> queryByTime(const QDateTime& from, const QDateTime& to, int limit = 1000);
    QVector<SortingRecord> queryByOrderCode(const QString& orderCode, int limit = 1000);
    QVector<SortingRecord> queryAll(int limit = 1000);
    // ★ 留空查全部：已分拣 + 待分拣（待分拣=计划行，无落格时间，**不受日期区间影响**）
    QVector<SortingRecord> queryAllWithPending(const QDateTime& from, const QDateTime& to, int limit = 1000);
    // ★ 按格口查分拣明细（★ 2026-09-10 兼容 "7"/"007"/"22007"）；★ 2026-09-16 带日期区间
    QVector<SortingRecord> queryByGrid(const QString& gridNum, const QDateTime& from, const QDateTime& to,
                                       int limit = 1000);
    // ★ 2026-09-10 需求1：按 SKU 查落格明细（EPC ↔ 实际落格号）；★ 2026-09-16 带日期区间
    QVector<SortingRecord> queryBySku(const QString& sku, const QDateTime& from, const QDateTime& to,
                                      int limit = 1000);
    // ★ 2026-09-13 需求：按容器号查该容器下的所有 EPC 物件明细（按落格时间正序）；★ 2026-09-16 带日期区间
    QVector<SortingRecord> queryByBoxcode(const QString& boxcode, const QDateTime& from, const QDateTime& to,
                                          int limit = 1000);
    // ★ 2026-09-13 需求：批量反查一组 EPC 各自出现过的"其他容器号"（key=EPC，value=其他容器号列表）
    //   用途：按容器号查询时判断"同一 EPC 是否被分到过别的容器"（跨容器漂移）
    //   ★ 2026-09-16：与主查询同区间（否则"漏查其他箱"与"结果被日期裁剪"口径不一致）
    QMap<QString, QStringList> queryOtherBoxcodesByEpc(const QStringList& epcs,
                                                       const QString& excludeBox,
                                                       const QDateTime& from, const QDateTime& to);
    // ★ 2026-09-15 某波次全部已落格明细（按落格先后正序）——切回波次时补齐
    //   H7 满箱明细 / 计划额度 / 落格去重集合的唯一持久权威来源。
    //   走只读连接：不与主写连接争用，切回期间分拣主链路不受影响。
    QVector<LandedRecord> getLandingRecordsForWave(const QString& orderCode, int limit = 100000);
    // ★ 2026-09-11 重扫重投：某波次某 EPC 的首条落格号（无记录返回空串）
    QString getFirstSortedGrid(const QString& orderCode, const QString& epc);
    QVector<GridSummaryRecord> queryGridSummary(const QDateTime& from, const QDateTime& to);   // ★ 需求③：全格口汇总（只统计区间内落格）
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
    // ★ 2026-09-06 按波次查询绑定快照（每格取该波次最近一条绑定，含已归档）——追溯口径
    QMap<QString, QString> getBindsByOrder(const QString& orderCode);
    // ★ 2026-09-15 按波次查询"该波次结束时仍有效"的绑定（active=1 且为该格最后一条）
    //   用途：切回波次时恢复格口绑定；自动剔除之后被更晚波次解绑的滞后行。
    //   注：关闭软件/启动新任务会把绑定归档(active=0)，但本查询无视归档时间——
    //   只要该格全表最后一条记录属于本波次且 active=1 就能取回，因此"归档"与"可切回恢复"并存。
    //   ★ 2026-09-17：**已降级**为"确认口径"（只用于命中数/日志标签）。
    //     切回恢复的主口径改走 getLastBindsByOrder()，见下方注释（现场"切回后清空"根因之一）。
    QMap<QString, QString> getBindsByOrderActive(const QString& orderCode);
    // ★ 2026-09-17 切回波次恢复绑定的**主口径**：每格取"本波次内"最后一条绑定（boxcode 非空）
    //   —— 含已被更晚波次覆盖的格口；本波次从未绑过的格口不返回（保持未绑定，不臆造）。
    //   为什么不用 getBindsByOrderActive()：它要求"该格全表最后一条属本波次"，
    //   实测真实库 14 个历史波次里 13 个返回 0 行；且它一旦部分命中，调用方整体替换内存绑定
    //   就会丢掉其余格口（现场"切回后面板被清空"）。本查询是它的超集，先取本查询绝不会漏格口。
    //   同时带 id/active/bind_time/unbind_time，供 UI「查看绑定」弹窗与切回取数共用**同一口径**。
    QVector<GridBoxBindRecord> getLastBindsByOrder(const QString& orderCode);
    // ★ 2026-09-17 归属补齐/纠偏：把"本会话内、当前物理生效、尚未归属 orderCode"的绑定行改判给该波次。
    //   · sessionStart    — 本次会话启动时刻（'yyyy-MM-dd HH:mm:ss.zzz'）；早于它的历史行永不改判；
    //   · switchOutWave   — 本次切出窗口归属的波次号（可空）；仅当该行确实是窗口内误归属时才可纠偏；
    //   · switchOutTime   — 切出时刻（可空）；
    //   · 返回实际改判行数；改判前的候选行会经 Data_INFO 打明细（grid/box/原归属/bind_time）。
    //   只改 order_code，不动 active/bind_time/unbind_time，不新增/不删除行。
    int attributeBindsToWave(const QString& orderCode, const QString& sessionStart,
                             const QString& switchOutWave, const QString& switchOutTime);
    // ★ 2026-09-17 按波次统计"绑过几个格口"（批量，UI 波次列表「格口绑定」列用）
    QMap<QString, int> getBindCountsByOrder(bool* ok = nullptr);
    // ★ 2026-09-17 人工修复历史误归属（仅核查脚本/人工按需调用，产品代码不调用）
    bool updateBindOrderCode(int id, const QString& newOrderCode);
    // ★ 2026-09-15 按波次查询绑定历史明细（含绑定/解绑时间）——只读回溯用
    QVector<GridBoxBindRecord> getBindHistoryByOrder(const QString& orderCode);
    // ★★ 2026-09-15 铁律（客户口径）：历史数据不得被粘贴进其它波次任务，格口绑定状态尤其如此 ★★
    //   因此**刻意不提供** "每格历史上最后一次绑定"（原 getLastKnownBinds）与
    //   "无归属遗留行"（原 getBindsWithEmptyOrder）这类跨波次取数接口：
    //   它们曾被用来把旧箱复活/复制到新波次，导致面板多绑一堆、65 个旧箱全部回绑。
    //   跨波次取数一律走 getAllActiveBinds()（只反映"当前活跃"），并且只用于**显示**，不写库。
    // 获取格口当前活跃绑定
    GridBoxBindRecord getActiveBind(const QString& gridNum);
    // 归档指定格口的所有活跃绑定（满箱/取消时调用）
    bool archiveGridBinds(const QString& gridNum);
    // 获取全部活跃绑定（程序重启后加载内存/UI 用）
    QVector<GridBoxBindRecord> getAllActiveBinds();
    // 归档全部活跃绑定（波次完结/取消时清空全部格口绑定）
    bool archiveAllBinds();
    // ★ 2026-09-17 「新任务」清空格口绑定：只归档**已归属**（order_code 非空）的活跃行，
    //   保留 order_code='' 的"待补齐"行给 H4 到达时的归属补齐（否则新波次又会 0 绑定，见 define.h 注释）。
    //   与 archiveAllBinds 一样：只改 active/unbind_time，行与 order_code 永不改动 → 切回可恢复。
    bool archiveAttributedBinds();

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
    // ★ 2026-09-16 现场需求⑤：只查**指定波次**的失败/已取消满箱报文（H7）
    //   口径 = 重试耗尽 failed / 切出取消 cancelled，仅多一个波次条件；
    //   用途：UI「重传满箱切换(H7)」下拉只显示本波次数据（原"全部历史"版本已删除）
    QVector<OutboxRecord> getFailedOutboxFullboxByOrder(const QString& orderCode, int limit = 200);
    // ★ 2026-09-08 UI 失败重传下拉：查询全部"重试耗尽失败/已取消重试"的完结（H8）出站消息
    QVector<OutboxRecord> getFailedOutboxEnd(int limit = 200);

    // ═══════════════════════════════════════════════════════════════
    // ★ 2026-09-15 波次列表批量统计（3 条 GROUP BY 查询覆盖全部波次）
    //   背景：波次列表每次刷新原先"每波次 3 次查询"，上百波次时主线程数秒冻结。
    //   以下 3 个接口把统计压成各 1 次查询，与波次数量无关；口径与原逐波次逻辑完全一致。
    //   ═══════════════════════════════════════════════════════════════

    // 各波次 H7（满箱回传）状态计数；无报文的波次**不出现**在该 map 中（调用方按 0/无 处理）
    QMap<QString, OutboxStatusCount> getFullboxStatusCountAll(bool* okOut = nullptr);
    // 各波次 H8（完结回传）状态计数；无报文的波次不出现
    QMap<QString, OutboxStatusCount> getEndStatusCountAll(bool* okOut = nullptr);
    // 各波次"仍在异常口、尚未处理完"的件数（type ∈ {plc_no_grid, plc_info_incomplete} 且 handled=0，按 EPC 去重）
    QMap<QString, int> getPendingExceptionCountAll(bool* okOut = nullptr);

    // ═══════════════════════════════════════════════════════════════
    // ★ 2026-09-15 RFID 原始推送报文留痕（rfid_raw）
    //   · 写入：业务入口只入队，由定时器/满批触发 insertRfidRawBatch（单事务，不阻塞 RFID 主链路）
    //   · 回查：识别后 EPC 与识别前 EPC 双通道命中，便于核对"丢弃了什么"
    // ═══════════════════════════════════════════════════════════════

    // 批量写入（单事务）；返回成功写入行数。落库前所有文本列做空串归一
    //（Qt null QString 会被绑定为 SQL NULL → NOT NULL 约束失败而静默丢帧，已复现的真实缺陷）
    int insertRfidRawBatch(const QVector<RfidRawRecord>& rows);
    // 按 EPC 回查该 EPC 的全部原始帧（识别前后都能命中），按 id 倒序 = 最近帧在前
    QVector<RfidRawRecord> queryRfidRawByEpc(const QString& epc, int limit = 100);
    // 超期清理：只删除 time 早于"当前时间 - retainDays"的行，返回删除行数
    //   默认 7 天（= define.h 的 RFID_RAW_RETAIN_DAYS；此处写字面量避免在 include define.h 之前取宏）
    int cleanupOldRfidRaw(int retainDays = 7);
    // 表内总行数（界面上脚/健康日志观测用）
    int countRfidRaw();

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