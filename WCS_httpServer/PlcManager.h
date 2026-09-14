#pragma once
// ============================================================================
// PlcManager.h — PLC 管理器（TCP 文本协议 + S7 锁格检测）
//
// 职责：
//   ① 通过 TCP 文本协议发送分拣指令 + 接收 PLC 落格反馈
//   ② 通过 S7 协议轮询读取 DB77 锁格状态（与 WCSApp 一致）
//   ③ 监听TCP端口，接受PLC主动连接（HP-Socket CTcpServerListener）
//   ④ 管理多PLC客户端连接（m_mapClient）
//   ⑤ S7 心跳线程：每2秒检测连接，断线自动重连
//   ⑥ 支持主动发送模式：批量发送波次所有EPC编码到PLC
//
// ★ 注意：S7 DB1分拣指令写入已移除，PLC通过TCP文本协议接收分拣指令
//
// 通信协议（与 WCSApp 完全兼容）：
//   TCP 发送: {识别码|格口|小车号}  — TCP 文本，格口和小车号 3位补零
//      识别码 = EPC编码，客户已确认（2026-08-10）
//      示例: {ST1234567890123|015|001}
//   TCP 反馈: {识别码|格口|小车号}  — 落格确认
//           {start}              — 批次开始
//           {stop}               — 批次停止
//   TCP 锁格: {格口号|L}          — PLC主动锁格（如 {222|L}）
//           {格口号|U}          — PLC主动解锁（如 {222|U}）
//   S7 锁格: 已移除S7锁格轮询，锁格由PLC通过TCP主动发送 {grid|L}/{grid|U}（与WCSApp一致）
//
// 参考: WCSApp\WCSApps\PlcCenter.h, FrmMainV2.cpp, simensS7.h
// ============================================================================

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QElapsedTimer>
#include <QDateTime>
#include <QSet>
#include <QTimer>
#include <QVector>
#include <QMap>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <thread>
#include <queue>
#include <functional>
#include "HPSocket.h"
#include "define.h"
#include "LifecycleLogger.h"
#include "ThreadPool.h"

class CSiemensPLC;

// ──── S7 位操作辅助 ────
static byte g_s7Mask[] = { 0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80 };

// 获取 byte 缓冲区中某一位的值（与 WCSApp simensS7.h 完全一致）
static inline bool S7_GetBitAt(byte Buffer[], int Pos, int Bit)
{
    if (Bit < 0) Bit = 0;
    if (Bit > 7) Bit = 7;
    return (Buffer[Pos] & g_s7Mask[Bit]) != 0;
}

// 设置 byte 缓冲区中某一位的值（与 WCSApp simensS7.h 完全一致）
static inline void S7_SetBitAt(byte Buffer[], int Pos, int Bit, bool Value)
{
    if (Bit < 0) Bit = 0;
    if (Bit > 7) Bit = 7;
    if (Value)
        Buffer[Pos] = (byte)(Buffer[Pos] | g_s7Mask[Bit]);
    else
        Buffer[Pos] = (byte)(Buffer[Pos] & ~g_s7Mask[Bit]);
}

// PLC配置结构
struct PlcConfig
{
    char szIp[24] = "0.0.0.0";
    int  port     = PLC_LISTEN_PORT;
    char szS7Ip[24] = PLC_S7_IP;   // S7 PLC IP
};

// 锁格事件结构
struct LockGridInfo
{
    QString gridNum;   // 格口号（3位补零）
    int     type = 0;  // 0=锁格, 1=解锁
};

// ──── 批量反馈条目（用于高并发场景下减少信号频率）────
struct PlcFeedbackEntry
{
    QString code;
    QString grid;
    QString car;
    QString firstCar;   // ★ 首车（5字段格式专用，3字段格式时 = car）
    QString lastCar;    // ★ 尾车（5字段格式专用，3字段格式时为空）
    int     status = 0; // ★ 分拣状态：1=成功 2=无格口 3=信息不全（5字段格式专用）
    qint64  timestampMs = 0;
};
Q_DECLARE_METATYPE(PlcFeedbackEntry)

// PLC统计快照（供UI展示）
struct PlcStats
{
    // ── 运行状态 ──
    bool    running      = false;
    int     port         = PLC_LISTEN_PORT;
    qint64  uptimeSec    = 0;

    // ── S7 状态 ──
    bool    s7Connected   = false;
    QString s7Ip;
    int64_t s7SendCount   = 0;
    int64_t s7SendErrCount = 0;

    // ── TCP 客户端状态 ──
    int     clientCount  = 0;
    int64_t recvCount    = 0;
    QString lastIp;
    int     lastPort     = 0;

    // ── TCP 发送统计 ──
    int64_t tcpSendCount    = 0;
    int64_t tcpSendErrCount = 0;

    // ── 最近数据 ──
    QString lastBarcode;
    QString lastGrid;
    QString lastCar;
    QString lastRecvCode;
    QString lastRecvGrid;
    QString lastRecvCar;
    QString lastError;
    qint64  lastSendTimeMs = 0;
    qint64  lastRecvTimeMs = 0;

    // ── 锁格状态 ──
    int     lockedGridCount = 0;  // 当前锁定格口数
};

// 回调类型
typedef std::function<void(std::string ip, int port, bool status)> PlcStatusCallback;
typedef std::function<void(QString code, QString grid, QString car)> PlcFeedbackCallback;
// ★ 格口查询回调：PLC/相机扫到识别码时调用，返回格口字符串（如 "15" 或 "1,2,3"）
// 识别码 = EPC编码，客户已确认（2026-08-10）
typedef std::function<QString(const QString& code)> PlcLookupCallback;
// ★ 小车号查询回调：从 EpcCache 获取 RFID 提供的小车号，返回 "001" 兜底
typedef std::function<QString(const QString& code)> PlcCarNumCallback;

// ★ 2026-09-14 同品多格口「计划分配」查询结果（由 HttpServer 依据 H4 计划 + 已落格计数提供）
//   ★ 2026-09-14 落格结构优化：改由 PlanAllocTable 提供，并**在查询时直接完成额度认领**
//     （锁内"判定 + 改数"），从而杜绝"先查已落数、再另行登记"造成的：
//       · 同时两件在线时两件都被判为"未满额" → 都发同一格口 → 箱内实落超计划；
//       · 计划格口被禁用/锁格时，该格口的计划件数被静默丢弃。
struct PlcPlanAllocInfo
{
    bool               valid = false;  // 是否查到该 SKU 的计划（false → 选格退回旧逻辑）
    QMap<QString, int> planQtyPerGrid; // 格口号(内部3位key) → 计划件数
    QMap<QString, QString> gridTypePerGrid; // 格口号(内部3位key) → 类型 "0"=正常分拣/"1"=异常/"2"=发货
    QMap<QString, int> landedNum;      // 格口号(内部3位key) → 已落格件数（PLC 反馈成功累计）
    QMap<QString, int> reservNum;      // ★ 格口号(内部3位key) → 在途认领件数
    int                excGrid = -1;   // 超计划（各计划格口均已满额）时的兜底去往格口，-1=未配置
    int                skuIdx  = -1;   // ★ 分配表内 SHA 下标（认领登记用）
    QString            orderCode;      // ★ 分配表所属波次（日志追溯用）

    // ── ★ 额度认领结果（HttpServer::planAllocOf 在锁内完成）──
    bool               claimOk   = false; // true=已成功认领一个计划格口（额度已扣）
    int                claimGrid = -1;    // 认领到的格口（内部号）
    qint16             claimPlanIdx = -1; // 认领单元在该 SKU 计划内的下标
    quint64            claimId   = 0;     // 认领号（日志串联 + 落格提交/失败释放）
    bool               allEpcsLanded = false; // 该 EPC 已在本 (SKU,格口) 落格（重投回原格口放行）
};
// 入参 = (EPC编码, SKU编码, 是否认领额度)，返回该 SKU 的计划分配信息
//   ★ 为什么要 bClaim=false 的"只读预查"模式：
//     选格流程需要先看一次计划（判断是否需要缺口搬迁），若那次调用就认领额度，
//     搬迁后再次调用会**二次认领**，前一次认领将泄漏为"在途"直至超时。
//     因此约定：bClaim=false 只读不扣额度；bClaim=true 才在锁内完成"判定 + 扣额度"。
//   ★ 为什么要传 EPC：分配表需在同一次加锁内判定"该件是否已落入过原格口
//     （重投放行）"与"额度是否还有"，避免两次加锁之间被其它线程插队
//     （这正是改造前"同时两件在线都判未满额"的超计划根因）。
//   注：PlcManager 侧调用时两个字符串入参都传 code（识别码=EPC），SKU 由 HttpServer 按 EPC 取。
typedef std::function<PlcPlanAllocInfo(const QString& epc, const QString& sku, bool bClaim)> PlcPlanAllocCallback;

// ★ 2026-09-14 计划缺口搬迁回调：(EPC, 不可用格口, 承接格口) → 实际搬迁件数
//   用途：计划格口满箱未重绑/锁格时，把其未完成计划件转给同 SKU 其它可用计划格口，
//         避免"计划有 N 件却只落 M 件"（客户口径：按各格口数量分）。
typedef std::function<int(const QString& epc, qint16 fromGrid, qint16 toGrid)> PlcMoveGapCallback;

// ★ 2026-09-14 选格成功日志节流回调：返回 true = 本次输出"选格-按计划分配"日志
//   目的：日万级件下把每件两条日志降到量级可控（异常/超计划/搬迁日志仍逐条保留）
typedef std::function<bool()> PlcSelectLogCallback;

// ★ 2026-09-14 单条下发结果回调（EPC, 是否成功）：发送失败时释放已认领的额度
//   为什么放在 sendCodeInfo 出口：sendBatchCodesWithEpcCache 内部存在"发送成功但
//   failCount 未归零"的路径，只在批出口释放会漏；出口回调保证每一条都可对账。
typedef std::function<void(const QString& epc, bool success)> PlcSendResultCallback;

class PlcManager : public QObject, public CTcpServerListener
{
    Q_OBJECT

    // ──── CTcpServerListener 回调 ────
    EnHandleResult OnAccept(ITcpServer* pSender, CONNID dwConnID, UINT_PTR soClient) override;
    EnHandleResult OnSend(ITcpServer* pSender, CONNID dwConnID, const BYTE* pData, int iLength) override;
    EnHandleResult OnShutdown(ITcpServer* pSender) override;
    EnHandleResult OnReceive(ITcpServer* pSender, CONNID dwConnID, const BYTE* pData, int iLength) override;
    EnHandleResult OnClose(ITcpServer* pSender, CONNID dwConnID, EnSocketOperation enOperation, int iErrorCode) override;
    EnHandleResult OnPrepareListen(ITcpServer* pSender, SOCKET soListen) override;

public:
    explicit PlcManager(QObject* parent = nullptr);
    ~PlcManager();

    bool start(const char* ip = nullptr, int port = PLC_LISTEN_PORT);
    void stop();
    bool isRunning() const { return m_bRunning.load(); }

    // ──── S7 连接管理 ────
    bool connectS7(const char* ip = PLC_S7_IP);
    void disconnectS7();
    bool isS7Connected() const;

    // ──── 锁格查询 ────
    bool isGridLocked(int grid) const;  // 查询指定格口是否锁定
    int  lockedGridCount() const;       // 当前锁定格口总数

    // ──── 格口禁用管理（满箱锁格后禁用，WMS重新绑定H6时恢复）────
    void disableGrid(int grid);         // 满箱锁格后禁用格口（禁止分配和落格）
    void enableGrid(int grid);          // WMS重新绑定H6时恢复格口
    bool isGridDisabled(int grid) const; // 查询格口是否被禁用
    void enableAllGrids();              // 全部启用（新波次开始时）

    void registerStatusCallback(PlcStatusCallback cb) { m_statusCb = std::move(cb); }
    void registerFeedbackCallback(PlcFeedbackCallback cb) { m_feedbackCb = std::move(cb); }
    void setLookupCallback(PlcLookupCallback cb) { m_lookupCb = std::move(cb); }
    // ★ 设置小车号查询回调（从 EpcCache 获取 RFID 提供的小车号）
    void setCarNumCallback(PlcCarNumCallback cb) { m_carNumCb = std::move(cb); }
    // ★ 2026-09-14 设置「计划分配」查询回调（同品多格口按计划件数分配的依据，含额度认领）
    void setPlanAllocCallback(PlcPlanAllocCallback cb) { m_planAllocCb = std::move(cb); }
    // ★ 2026-09-14 设置计划缺口搬迁回调（计划格口不可用 → 未完成件转同 SKU 其它计划格口）
    void setMoveGapCallback(PlcMoveGapCallback cb) { m_moveGapCb = std::move(cb); }
    // ★ 2026-09-14 设置选格成功日志节流回调
    void setSelectLogCallback(PlcSelectLogCallback cb) { m_selectLogCb = std::move(cb); }
    // ★ 2026-09-14 设置单条下发结果回调（发送失败 → 释放认领额度）
    void setSendResultCallback(PlcSendResultCallback cb) { m_sendResultCb = std::move(cb); }

    // ──── 发送指令 ────
    // code 为 EPC编码，客户已确认（2026-08-10）
    // TODO: car 小车号应由RFID提供，客户尚未提供RFID小车号字段，当前默认=1（2026-08-04）
    bool sendCodeInfo(const QString& code, const std::vector<int>& vecGrid, int car = 1);
    bool sendRawCommand(const QString& command);
    // ★ 主动发送模式：批量发送波次识别码到PLC（不等待PLC查询，与WCSApp一致）
    // codeGridMap 的 key 为 EPC编码（RFID推送），value 为格口号列表
    // 格式: {EPC|格口|小车号} 发送给 PLC
    bool sendBatchCodes(const QMap<QString, QString>& codeGridMap);

    // ★ 从 EpcCache 获取小车号发送（RFID 提供小车号，carNum 默认 "001"）
    bool sendBatchCodesWithEpcCache(const QMap<QString, QString>& codeGridMap);

    int  connectedClientCount() const;
    bool hasConnectedClients() const { return connectedClientCount() > 0; }
    QString lastError() const { return m_lastError; }
    PlcStats stats() const;
    int64_t s7SendCount() const { return m_s7SendCount.load(); }
    int64_t tcpSendCount() const { return m_tcpSendCount.load(); }
    int64_t recvCount() const { return m_recvCount.load(); }
    qint64  uptimeSec() const;

signals:
    // ── TCP 信号 ──
    void plcConnected(const QString& ip, int port);
    void plcDisconnected(const QString& ip, int port);
    void plcFeedbackReceived(const QString& code, const QString& grid, const QString& car);
    void plcFeedbackBatch(const QVector<PlcFeedbackEntry>& entries);  // ★ 批量反馈信号（UI日志用，100ms间隔）
    void plcFeedbackBusinessBatch(const QVector<PlcFeedbackEntry>& entries); // ★ 批量业务信号（HttpServer分拣标记，100ms间隔）
    void plcBatchStart();
    void plcBatchStop();
    void plcSendInfo(const QString& code, const QString& grids, bool success);

    // ── S7 信号 ──
    void s7Connected(const QString& ip);
    void s7Disconnected(const QString& ip);
    void s7Error(const QString& errMsg);
    void gridLocked(const QString& gridNum);     // 格口被锁定（S7边沿检测）
    void gridUnlocked(const QString& gridNum);   // 格口解锁（S7边沿检测）
    void gridLockedByPlc(const QString& gridNum);  // ★ 格口被锁定（PLC主动TCP消息）
    void gridUnlockedByPlc(const QString& gridNum);// ★ 格口解锁（PLC主动TCP消息）

    private:
    void parsePlcFeedback(const QByteArray& data);
    void OnS7HeartThread();                    // ★ S7 心跳线程：每2秒检测连接，断线重连（与 WCSApp simensS7::OnHeartThread 一致）
    void flushFeedbackBatch();                 // ★ 定时刷新批量反馈到UI

    // ──── TCP 通信 ────
    CTcpServerPtr m_tcpServer;

    PlcConfig m_plcConfig;
    std::atomic<bool> m_bRunning{false};

    // ★ PLC 发送专用线程池：S7 DBWrite 同步阻塞，异步入池防止阻塞 HP-Socket I/O 线程
    Hanchine::ThreadPool* m_pSendPool = nullptr;

    mutable std::mutex m_clientMutex;
    std::map<CONNID, std::string> m_mapClient;

    PlcStatusCallback m_statusCb;
    PlcFeedbackCallback m_feedbackCb;
    PlcLookupCallback  m_lookupCb;   // ★ 相机查询回调：查格口
    PlcCarNumCallback  m_carNumCb;   // ★ 小车号查询回调：从 EpcCache 获取 RFID 小车号
    PlcPlanAllocCallback m_planAllocCb;  // ★ 2026-09-14 计划分配查询回调（同品多格口按计划件数选格+额度认领）
    PlcMoveGapCallback   m_moveGapCb;    // ★ 2026-09-14 计划缺口搬迁回调（计划格口不可用时转移未完成件）
    PlcSelectLogCallback m_selectLogCb;  // ★ 2026-09-14 选格成功日志节流回调（日万级件日志量控制）
    PlcSendResultCallback m_sendResultCb;// ★ 2026-09-14 单条下发结果回调（失败释放认领额度）

    // ──── TCP 统计 ────
    std::atomic<int64_t> m_tcpSendCount{0};
    std::atomic<int64_t> m_recvCount{0};
    std::atomic<int64_t> m_tcpSendErrCount{0};

    // ──── S7 通信 ────
    CSiemensPLC* m_S7Plc = nullptr;
    std::atomic<int64_t> m_s7SendCount{0};
    std::atomic<int64_t> m_s7SendErrCount{0};

    // ──── S7 锁格状态（双重检测：S7 DB77边沿轮询 + TCP主动消息 {grid|L}/{grid|U}，与WCSApp一致）────
    bool        m_s7Grid_200[PLC_S7_MAX_GRID_COUNT]{ false }; // 当前锁格状态（200位）
    byte        m_s7PlcLastData[PLC_S7_LOCK_READ_SIZE]{ 0 };  // ★ 上一次S7锁格数据（边沿检测用，与WCSApp一致）
    mutable std::mutex m_lockGridPlc;       // 保护 m_s7Grid_200

    // ──── 格口禁用集合（满箱锁格后禁用，WMS重新绑定H6时恢复）────
    QSet<int>            m_disabledGrids;          // 已禁用的格口号集合
    mutable std::mutex   m_lockDisabledGrids;      // 保护 m_disabledGrids

    // ──── S7 锁格轮询定时器 ────
    QTimer*     m_s7LockTimer = nullptr;     // ★ S7锁格轮询定时器（1秒间隔，与WCSApp S7边沿检测一致）
    void pollS7LockStatus();                 // ★ S7锁格轮询：读DB77→边沿检测→发射信号

    // ──── S7 心跳线程（与 WCSApp simensS7::OnHeartThread 一致）────
    std::thread m_heartThread;              // ★ S7 心跳线程
    bool        m_bHeartThreadStart = false; // ★ 心跳线程启动标志

    // ──── 锁格事件队列 ────
    std::mutex m_lockGrid;                  // 保护锁格队列
    std::queue<LockGridInfo> m_queueLockInfo;

    QElapsedTimer m_uptime;

    // ──── 最近数据（供UI查询）───
    QString m_lastBarcode;
    QString m_lastGrid;             // 最近一次发送的格口
    QString m_lastCar;
    QString m_lastRecvCode;         // 最近一次接收的EPC编码
    QString m_lastRecvGrid;         // 最近一次接收的格口
    QString m_lastRecvCar;          // 最近一次接收的小车
    QString m_lastRecvFirstCar;     // ★ 最近一次接收的首车（5字段格式）
    QString m_lastRecvLastCar;      // ★ 最近一次接收的尾车（5字段格式）
    int     m_lastRecvStatus = 0;   // ★ 最近一次接收的分拣状态（5字段格式）
    qint64  m_lastSendTimeMs = 0;   // 最近发送时间戳
    qint64  m_lastRecvTimeMs = 0;   // 最近接收时间戳
    mutable std::mutex m_lastDataMutex;  // 保护最近数据

    // ──── 错误信息 ────
    QString m_lastError;
    QString m_lastIp;              // 最后连接的PLC IP
    int     m_lastPort = 0;         // 最后连接的PLC端口

    // ──── 接收缓冲区（粘包处理）────
    std::mutex m_recvMutex;
    QByteArray m_recvBuffer;

    // ──── PLC发送失败日志限流 ────
    QSet<QString> m_warnedBarcodes;  // 已警告过的EPC编码（PLC连接时重置）
    std::mutex    m_warnMutex;       // 保护m_warnedBarcodes

    // ──── 批量反馈机制（减少高并发下的信号频率）────
    QTimer*       m_feedbackBatchTimer = nullptr; // 批量刷新定时器（100ms间隔）
    std::mutex    m_feedbackBatchMutex;           // 保护批量反馈队列
    QVector<PlcFeedbackEntry> m_feedbackBatchBuffer; // 批量反馈缓冲区
};