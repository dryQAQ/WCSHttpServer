#pragma once
// ============================================================================
// PlcManager.h — PLC 管理器（S7 发送 + TCP 接收）
//
// 职责：
//   ① 通过 S7 协议 (Snap7) 写入条码+格口指令到 PLC DB1（与 WCSApp 一致）
//   ② 监听TCP端口，接受PLC主动连接（HP-Socket CTcpServerListener）
//   ③ 接收PLC反馈：{barcode|grid|car} 落格确认 / {start} / {stop}
//   ④ 管理多PLC客户端连接（m_mapClient）
//   ⑤ 连接状态监控和心跳检测
//
// 通信协议（与WCSApp完全兼容）：
//   发送: S7 DBWrite → DB1 Offset 1000, 42 bytes 二进制格式
//   反馈: {条码|格口|小车号}  — 落格确认（TCP 文本）
//        {start}              — 批次开始
//        {stop}               — 批次停止
//
// 参考: WCSApp\WCSApps\PlcCenter.h + SiemensPLC.h
// ============================================================================

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QElapsedTimer>
#include <QDateTime>
#include <QSet>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <functional>
#include "HPSocket.h"
#include "define.h"
#include "LifecycleLogger.h"

class CSiemensPLC;

// PLC配置结构
struct PlcConfig
{
    char szIp[24] = "0.0.0.0";
    int  port     = PLC_LISTEN_PORT;
};

// PLC统计快照（供UI展示）
struct PlcStats
{
    bool    running      = false;
    int     port         = PLC_LISTEN_PORT;
    int     clientCount  = 0;
    int64_t sendCount    = 0;
    int64_t recvCount    = 0;
    int64_t sendErrCount = 0;
    qint64  uptimeSec    = 0;
    QString lastIp;
    QString lastError;
};

// 回调类型
typedef std::function<void(std::string ip, int port, bool status)> PlcStatusCallback;
typedef std::function<void(QString code, QString grid, QString car)> PlcFeedbackCallback;

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

    // ──── 生命周期 ────
    bool start(const char* ip = nullptr, int port = PLC_LISTEN_PORT);
    void stop();
    bool isRunning() const { return m_bRunning.load(); }

    // ──── S7 连接管理 ────
    bool connectS7(const char* ip);
    void disconnectS7();
    bool isS7Connected() const;
    QString s7Ip() const { return m_s7Ip; }

    // ──── 回调注册 ────
    void registerStatusCallback(PlcStatusCallback cb) { m_statusCb = std::move(cb); }
    void registerFeedbackCallback(PlcFeedbackCallback cb) { m_feedbackCb = std::move(cb); }

    // ──── 发送指令 ────
    // 发送条码+格口信息到PLC: {barcode|grid1|grid2|grid3|car}
    bool sendCodeInfo(const QString& barcode, const std::vector<int>& vecGrid, int car = 1);
    // 发送自定义指令
    bool sendRawCommand(const QString& command);

    // ──── 状态查询 ────
    int  connectedClientCount() const;
    bool hasConnectedClients() const { return connectedClientCount() > 0; }
    QString lastError() const { return m_lastError; }
    PlcStats stats() const;           // 获取统计快照
    int64_t sendCount() const { return m_sendCount.load(); }
    int64_t recvCount() const { return m_recvCount.load(); }
    int64_t sendErrorCount() const { return m_sendErrorCount.load(); }
    qint64  uptimeSec() const;

signals:
    // PLC连接状态变化
    void plcConnected(const QString& ip, int port);
    void plcDisconnected(const QString& ip, int port);
    // PLC反馈：落格确认
    void plcFeedbackReceived(const QString& code, const QString& grid, const QString& car);
    // PLC状态信号
    void plcBatchStart();
    void plcBatchStop();

private:
    // 解析PLC反馈数据
    void parsePlcFeedback(const QByteArray& data);

    // ──── HP-Socket Server ────
    CTcpServerPtr m_tcpServer;

    // ──── S7 客户端 ────
    CSiemensPLC* m_S7Plc{ nullptr };
    QString m_s7Ip;                    // S7 连接的 PLC IP

    // ──── 配置 ────
    PlcConfig m_plcConfig;
    std::atomic<bool> m_bRunning{false};

    // ──── 连接管理 ────
    std::mutex m_clientMutex;
    std::map<CONNID, std::string> m_mapClient;  // CONNID → "client"

    // ──── 回调 ────
    PlcStatusCallback m_statusCb;
    PlcFeedbackCallback m_feedbackCb;

    // ──── 统计 ────
    std::atomic<int64_t> m_sendCount{0};
    std::atomic<int64_t> m_recvCount{0};
    std::atomic<int64_t> m_sendErrorCount{0};
    QElapsedTimer m_uptime;

    // ──── 错误信息 ────
    QString m_lastError;
    QString m_lastIp;              // 最后连接的PLC IP

    // ──── 接收缓冲区（粘包处理）────
    std::mutex m_recvMutex;
    QByteArray m_recvBuffer;

    // ──── PLC发送失败日志限流 ────
    QSet<QString> m_warnedBarcodes;  // 已警告过的条码（PLC连接时重置）
    std::mutex    m_warnMutex;       // 保护m_warnedBarcodes
};