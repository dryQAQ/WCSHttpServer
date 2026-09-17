#pragma once
// ============================================================================
// RfidPushClient.h — RFID 推送 TCP 客户端（2026-09-04）
//
// 现场架构：WCS 作为「客户端」主动 TCP 连接 RFID 系统（RFID 是服务端），
// 连接建立后 RFID 系统通过该连接持续推送 ASCII 帧（现场协议确认）：
//   {流水号|设备编码|epc}0D         — 正常读到 EPC（0D 为帧尾字面字符）
//   {流水号|设备编码|NOREAD}0D      — EPC 未读到（无条码，仅留痕不处理）
//   心跳/应答: RFID{HEARTBEAT}0D 或 {HEARTBEAT...}（WCS 每 2s 发送；应答帧静默记录）
//   WCS 侧另走 HTTP 查询 EPC↔SKU 绑定关系（rfidQueryUrl，与 TCP 推送地址端口不同）
//
// 职责：
//   ① 主动连接 RFID 服务端（IP/端口由配置 rfidPushServerIp/rfidPushServerPort 提供）
//   ② 断线自动重连（QTimer 每 3s 检测，未连接则重新 Start）
//   ③ 处理 TCP 粘包/分包，逐条解析出完整 ASCII 帧 {流水号|设备编码|epc}0D
//   ④ 原始报文无条件落日志（★ 2026-09-06 起并入 run.log 统一查看，无论波次状态、无论格式对错）
//   ⑤ ★ 2026-09-15 EPC 识别：按 rfidEpcTruncateLen（默认 24）在串中**定位**
//      「'A' + (长度-1) 位数字」形态并取出（见 EpcCode.h）；未识别到时不猜、按原文继续并告警。
//      识别前的 EPC 原文（epcRaw）与整帧原文（raw）随 JSON 一并交下游留痕。
//   ⑥ NOREAD 帧同样上报（epc 空 + noread=true），供界面/数据库留痕，但**不进入分拣**
//   ⑦ 经 rfidPushReceived 信号（QueuedConnection 回主线程）交 HttpServer 处理，
//      复用 handleRfidCarNumReport()（存 EpcCache + SKU 查询 + 发 PLC）
// ============================================================================

#include <QObject>
#include <QByteArray>
#include <QJsonObject>
#include <QTimer>
#include <atomic>
#include <mutex>
#include "HPSocket.h"
#include "define.h"   // ★ 心跳宏 RFID_HEARTBEAT_* / RFID_EPC_TRUNCATE_LEN

class RfidPushClient : public QObject, public CTcpClientListener
{
    Q_OBJECT
public:
    explicit RfidPushClient(QObject* parent = nullptr);
    ~RfidPushClient();

    bool start(const QString& ip, int port);   // 发起连接 + 启动断线重连定时器
    void stop();
    bool isConnected() const;

    // ★ 2026-09-05：心跳开关（XML rfidHeartbeatEnable，true=发送 0=不发送）
    void setHeartbeatEnabled(bool on) { m_heartbeatEnabled = on; }
    // ★ 2026-09-05：心跳间隔（XML rfidHeartbeatIntervalMs，毫秒；≤0 时保持默认）
    void setHeartbeatIntervalMs(int ms) { if (ms > 0) m_heartbeatIntervalMs = ms; }

    // ★ 2026-09-15：EPC 码识别长度（XML rfidEpcTruncateLen，默认 24）
    //   识别形态 = 'A' + (len-1) 位数字；len < 2 = 不识别（整串原样使用，回退改造前行为）
    //   改配置后由 HttpServer 热下发（无需重启），解析线程读取的是同一份成员
    void setEpcTruncateLen(int len) { m_epcTruncateLen = len; }
    int  epcTruncateLen() const { return m_epcTruncateLen; }

signals:
    // ★ 解析出一条数据帧后发出（已转换为 body={"data":[{epc,epcRaw,carNum,seq,devCode,raw,normalized,noread}]}，
    //   供 handleRfidCarNumReport() 复用；心跳帧不发出；NOREAD 帧发出（epc 空 + noread=true）供留痕）
    void rfidPushReceived(const QJsonObject& body);

protected:
    // CTcpClientListener 回调
    EnHandleResult OnConnect(ITcpClient* pSender, CONNID dwConnID) override;
    EnHandleResult OnReceive(ITcpClient* pSender, CONNID dwConnID, const BYTE* pData, int iLength) override;
    EnHandleResult OnClose(ITcpClient* pSender, CONNID dwConnID,
                           EnSocketOperation enOperation, int iErrorCode) override;

private slots:
    void onReconnectTimer();   // 断线重连检测
    void onHeartbeatTimer();   // ★ 2026-09-04 应用层心跳（每 2s 发一次）

private:
    CTcpClientPtr m_client;      // HP-Socket TCP 客户端
    QString       m_ip;          // RFID 服务端 IP
    int           m_port = 0;    // RFID 服务端端口
    QTimer*       m_reconnectTimer = nullptr;  // 重连定时器（3s）
    QTimer*       m_heartbeatTimer  = nullptr; // ★ 心跳定时器（2s，仅已连接时发送）
    // ★ 2026-09-04：自维护连接状态（本版 HP-Socket 枚举无 SS_CONNECTED，
    //   OnConnect 置 true / OnClose 置 false，供心跳与重连判断）
    std::atomic<bool> m_connected{false};
    bool              m_heartbeatEnabled = true;  // ★ 2026-09-05 心跳开关（XML rfidHeartbeatEnable，默认发送）
    int               m_heartbeatIntervalMs = RFID_HEARTBEAT_INTERVAL_MS;  // ★ 2026-09-05 心跳间隔(ms, 默认2000)
    int               m_epcTruncateLen = RFID_EPC_TRUNCATE_LEN;  // ★ 2026-09-15 EPC 识别长度（默认24；<2=不识别）
    std::mutex    m_bufMutex;    // 保护接收缓冲
    QByteArray    m_recvBuffer;  // 接收缓冲（粘包/分包处理）
};
