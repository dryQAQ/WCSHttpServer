#pragma once
// ============================================================================
// PlcManager.h — PLC 管理器（TCP 文本协议 + S7 协议 + 锁格）
//
// 职责：
//   ① 通过 S7 DBWrite 发送EPC编码+格口分拣指令到 PLC（与 WCSApp 一致）
//   ② 通过 TCP 文本协议发送指令 + 接收 PLC 落格反馈
//   ③ 监听TCP端口，接受PLC主动连接（HP-Socket CTcpServerListener）
//   ④ 接收 PLC 主动发送的 TCP 锁格消息 {grid|L}/{grid|U}（与WCSApp一致，PLC主动发，WCS被动接收执行）
//   ⑤ 管理多PLC客户端连接（m_mapClient）
//   ⑥ S7 心跳线程：每2秒检测连接，断线自动重连（与 WCSApp simensS7 一致）
//   ⑦ 支持主动发送模式：批量发送波次所有EPC编码到PLC（不等待PLC查询，与WCSApp一致）
//
// 通信协议（与 WCSApp 完全兼容）：
//   S7 发送: DB1 Offset 1000, 42 bytes (EPC编码+格口二进制包)
//   TCP 发送: {识别码|格口|小车号}  — TCP 文本，格口和小车号 3位补零
//      识别码 = EPC编码，客户已确认（2026-08-10）
//      TODO: 小车号应由RFID提供，客户尚未提供RFID小车号字段（2026-08-04）
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