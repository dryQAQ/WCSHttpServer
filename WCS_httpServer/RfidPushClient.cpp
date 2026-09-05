#include "RfidPushClient.h"
#include "LogService.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonParseError>
#include <tchar.h>          // ★ _T() 宏（HP-Socket Start(LPCTSTR) 需要）

// ============================================================================
// RfidPushClient 实现（WCS 主动连接 RFID 服务端）
// ============================================================================

RfidPushClient::RfidPushClient(QObject* parent)
    : QObject(parent), m_client(this)
{
    // ★ 断线重连定时器（主线程）
    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setInterval(3000);   // 3s 检测一次
    connect(m_reconnectTimer, &QTimer::timeout, this, &RfidPushClient::onReconnectTimer);

    // ★ 2026-09-04：应用层心跳定时器（RFID 服务端要求每 2s 发一次心跳）
    //   定时器常驻，仅 m_connected（OnConnect 置位）为 true 时才真正发送
    //   （避免跨线程操作 QTimer：OnConnect 在工作线程，不直接 start 定时器）
    m_heartbeatTimer = new QTimer(this);
    m_heartbeatTimer->setInterval(RFID_HEARTBEAT_INTERVAL_MS);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &RfidPushClient::onHeartbeatTimer);
}

RfidPushClient::~RfidPushClient()
{
    stop();
}

bool RfidPushClient::start(const QString& ip, int port)
{
    m_ip   = ip;
    m_port = port;

    if (m_ip.isEmpty() || m_port <= 0)
    {
        RFID_ERROR("RFID服务端地址无效 ip=%s port=%d", m_ip.toLocal8Bit().constData(), m_port);
        return false;
    }

    // ★ 2026-09-04：TCP KeepAlive 心跳（协议层保活，对端无感知、无协议风险）
    //   - KeepAliveTime=10s：连接空闲 10 秒后开始发送探测包
    //   - KeepAliveInterval=5s：异常探测间隔 5 秒
    //   作用：对端异常静默断线（拔网线/RFID进程崩溃无FIN）时，Windows 探测失败会关闭连接
    //   → 触发 OnClose → 3s 重连定时器自动恢复（防止"连接假死、数据流中断且不自愈"）
    m_client->SetKeepAliveTime(10 * 1000);
    m_client->SetKeepAliveInterval(5 * 1000);

    // 发起异步连接（连接建立由 OnConnect 回调确认）
    std::wstring wip = m_ip.toStdWString();
    bool ok = m_client->Start(wip.c_str(), (USHORT)m_port, TRUE);
    RFID_INFO("RFID客户端发起连接 ip=%s port=%d keepAlive=%d/%dms result=%d",
              m_ip.toLocal8Bit().constData(), m_port, 10 * 1000, 5 * 1000, ok ? 1 : 0);

    // 启动断线重连定时器；心跳定时器按开关启停、间隔取配置（XML rfidHeartbeatEnable / rfidHeartbeatIntervalMs）
    m_heartbeatTimer->setInterval(m_heartbeatIntervalMs);   // ★ 应用配置的心跳间隔
    if (!m_reconnectTimer->isActive())
        m_reconnectTimer->start();
    if (m_heartbeatEnabled)
    {
        if (!m_heartbeatTimer->isActive())
            m_heartbeatTimer->start();
    }
    else
    {
        if (m_heartbeatTimer->isActive())
            m_heartbeatTimer->stop();
        RFID_WARN("RFID心跳已禁用（rfidHeartbeatEnable=0），不发送心跳包");
    }

    return ok;
}

void RfidPushClient::stop()
{
    if (m_reconnectTimer) m_reconnectTimer->stop();
    if (m_heartbeatTimer)  m_heartbeatTimer->stop();
    if (m_client && m_client->HasStarted())
    {
        m_client->Stop();
        RFID_INFO("RFID推送客户端已停止");
    }
    m_client.Reset();
}

bool RfidPushClient::isConnected() const
{
    return m_connected.load();
}

void RfidPushClient::onReconnectTimer()
{
    if (!m_client || m_ip.isEmpty() || m_port <= 0) return;

    // 已连接则不动作
    if (m_connected.load())
        return;

    // 连接已断开但组件仍在启动态（Start 已发起、连接结果未回）时不重复 Start
    EnServiceState st = m_client->GetState();
    if (st == SS_STARTING || st == SS_STARTED)
        return;

    // 停止状态 → 重新发起连接
    RFID_WARN("RFID断线重连 ip=%s port=%d", m_ip.toLocal8Bit().constData(), m_port);
    std::wstring wip = m_ip.toStdWString();
    m_client->Start(wip.c_str(), (USHORT)m_port, TRUE);
}

// ★ 2026-09-04：应用层心跳——RFID 服务端要求客户端每 2s 发一次心跳保活
//   仅已连接（OnConnect 置位的 m_connected）时发送；发送报文无条件落日志（与接收侧对称）
void RfidPushClient::onHeartbeatTimer()
{
    if (!m_heartbeatEnabled || !m_client || !m_connected.load()) return;   // 心跳关闭/未连接/连接中不发送

    static int s_beatCount = 0;
    QByteArray beat(RFID_HEARTBEAT_MSG);

    // ★ 2026-09-04：发送的报文也打印日志（hex + 文本），方便排查心跳格式
    QString beatText = QString::fromUtf8(beat);
    QString beatHex  = QString::fromLatin1(beat.toHex(' '));

    if (m_client->Send((const BYTE*)beat.constData(), beat.length()))
    {
        ++s_beatCount;
        RFID_INFO("[发送报文] len=%d hex=%s text=%s | 发送成功(累计%d次)",
                  beat.length(), beatHex.toLocal8Bit().constData(),
                  beatText.toLocal8Bit().constData(), s_beatCount);
    }
    else
    {
        RFID_WARN("[发送报文] len=%d hex=%s text=%s | 发送失败 err=%d（等待自动重连）",
                  beat.length(), beatHex.toLocal8Bit().constData(),
                  beatText.toLocal8Bit().constData(), (int)m_client->GetLastError());
    }
}

EnHandleResult RfidPushClient::OnConnect(ITcpClient* pSender, CONNID dwConnID)
{
    m_connected.store(true);   // ★ 连接成功置位（心跳/重连判断依据）
    RFID_INFO("RFID系统已连接 conn=%llu ip=%s port=%d",
              (unsigned long long)dwConnID, m_ip.toLocal8Bit().constData(), m_port);
    return HR_OK;
}

EnHandleResult RfidPushClient::OnClose(ITcpClient* pSender, CONNID dwConnID,
                                        EnSocketOperation enOperation, int iErrorCode)
{
    m_connected.store(false);   // ★ 断开复位

    // ★ 2026-09-04：断开原因可读化，方便现场判断是谁断开
    //   op: 2=连接阶段失败  3=发送时断开  4=接收时断开  5=主动关闭
    //   errCode: 0=对端正常关闭  10053=连接中止  10054=对端重置连接(服务端踢/崩溃)
    //            10060=连接超时(网络不通/端口未监听)  10061=连接被拒绝
    const char* errDesc = "";
    switch (iErrorCode)
    {
    case 0:     errDesc = "(对端正常关闭)"; break;
    case 10053: errDesc = "(连接中止)"; break;
    case 10054: errDesc = "(对端重置连接:服务端主动踢或崩溃)"; break;
    case 10060: errDesc = "(连接超时:IP/端口不可达)"; break;
    case 10061: errDesc = "(连接被拒绝:服务端未监听该端口)"; break;
    default:    errDesc = ""; break;
    }

    RFID_WARN("RFID连接断开 conn=%llu 阶段op=%d errCode=%d%s（3s后自动重连）",
              (unsigned long long)dwConnID, (int)enOperation, iErrorCode, errDesc);
    return HR_OK;
}

// ──── 帧尾清理：兼容两种结尾（现场协议帧尾为字面 "0D"；再容忍传统 \r\n）────
static void stripFrameTail(QByteArray& buf)
{
    if (buf.startsWith("0D"))
        buf.remove(0, 2);
    while (!buf.isEmpty() && (buf.at(0) == '\r' || buf.at(0) == '\n'))
        buf.remove(0, 1);
}

EnHandleResult RfidPushClient::OnReceive(ITcpClient* pSender, CONNID dwConnID,
                                         const BYTE* pData, int iLength)
{
    if (!pData || iLength <= 0) return HR_OK;

    // ★ 2026-09-04：原始报文无条件落日志（无论波次状态、无论格式对错，方便维护排查）
    QByteArray raw((const char*)pData, iLength);
    {
        bool truncated = (raw.size() > 512);
        QByteArray head = raw.left(512);
        RFID_INFO("[原始报文] conn=%llu len=%d hex=%s%s",
                  (unsigned long long)dwConnID, iLength,
                  head.toHex(' ').constData(),
                  truncated ? " ...(截断)" : "");
        RFID_INFO("[原始报文] conn=%llu text=%s%s",
                  (unsigned long long)dwConnID,
                  QString::fromUtf8(head).toLocal8Bit().constData(),
                  truncated ? " ...(截断)" : "");
    }

    {
        std::lock_guard<std::mutex> lock(m_bufMutex);
        m_recvBuffer.append(raw);

        // ★ 循环解析 ASCII 帧：{流水号|小车号|epc}0D（支持多帧粘连与 TCP 分包）
        while (!m_recvBuffer.isEmpty())
        {
            // ── 剥离帧边界残留（上一帧的帧尾字面 "0D"/CR/LF 与下一帧头之间的多余字节）──
            while (!m_recvBuffer.isEmpty() &&
                   (m_recvBuffer.startsWith("0D") ||
                    m_recvBuffer.at(0) == '\r' || m_recvBuffer.at(0) == '\n'))
            {
                m_recvBuffer.remove(0, m_recvBuffer.startsWith("0D") ? 2 : 1);
            }
            if (m_recvBuffer.isEmpty())
                break;

            // ── 心跳/应答帧（RFID{HEARTBEAT...} / {HEARTBEAT...} 文本帧，非业务数据）──
            //   识别并静默记录，避免每 2s 一次被"报文非法"告警刷屏
            if (m_recvBuffer.startsWith("RFID{HEARTBEAT") || m_recvBuffer.startsWith("{HEARTBEAT"))
            {
                int end = m_recvBuffer.indexOf('}');
                if (end < 0)
                    break;   // 帧不完整（还没收到 '}'）→ 等更多数据
                QByteArray frame = m_recvBuffer.left(end + 1);
                RFID_INFO("[心跳] 收到心跳/应答帧: %s",
                          QString::fromUtf8(frame).toLocal8Bit().constData());
                m_recvBuffer.remove(0, end + 1);
                stripFrameTail(m_recvBuffer);
                continue;
            }

            // ── 业务数据帧必须以 '{' 开头；否则丢弃前导垃圾直到下一帧头 ──
            if (!m_recvBuffer.startsWith('{'))
            {
                int b = m_recvBuffer.indexOf('{');
                int h = m_recvBuffer.indexOf("RFID{HEARTBEAT");
                if (b < 0) b = h;
                else if (h >= 0 && h < b) b = h;
                if (b < 0)
                {
                    RFID_WARN("RFID报文无帧头 丢弃缓冲 size=%d（原始报文已记录）", m_recvBuffer.size());
                    m_recvBuffer.clear();
                    break;
                }
                RFID_WARN("RFID报文前导垃圾丢弃 %d 字节", b);
                m_recvBuffer.remove(0, b);
                continue;
            }

            // ── 找帧尾 '}'（内容为 流水号|小车号|epc，不含 '{'/'}'，可直接定位）──
            int endBrace = m_recvBuffer.indexOf('}');
            if (endBrace < 0)
            {
                // 保护：异常长帧时丢弃（原始报文已记录）
                if (m_recvBuffer.size() > 16 * 1024)
                {
                    RFID_ERROR("RFID数据帧异常超长 size=%d，丢弃缓冲（原始报文已记录）",
                               m_recvBuffer.size());
                    m_recvBuffer.clear();
                }
                break;   // 帧不完整 → 等下一次 OnReceive
            }
            if (endBrace == 0)
            {
                m_recvBuffer.remove(0, 1);   // 孤立 '}'，丢弃
                continue;
            }

            QByteArray content = m_recvBuffer.mid(1, endBrace - 1);
            m_recvBuffer.remove(0, endBrace + 1);
            stripFrameTail(m_recvBuffer);

            // ── 解析 content：流水号|小车号|epc ──
            QList<QByteArray> parts = content.split('|');
            QString seq, carNum, epc;
            if (parts.size() >= 3)
            {
                seq    = QString::fromUtf8(parts[0]).trimmed();
                carNum = QString::fromUtf8(parts[1]).trimmed();
                epc    = QString::fromUtf8(parts[2]).trimmed();
            }
            else if (parts.size() == 2)
            {
                // 兼容两段帧：{流水号|epc}（缺小车号），仅告警仍尝试处理
                seq = QString::fromUtf8(parts[0]).trimmed();
                epc = QString::fromUtf8(parts[1]).trimmed();
                RFID_WARN("RFID帧仅2段(缺小车号) seq=%s epc=%s frame=%s",
                          seq.toLocal8Bit().constData(), epc.toLocal8Bit().constData(),
                          content.constData());
            }
            else
            {
                RFID_WARN("RFID帧格式非法 parts=%d frame=%s", parts.size(), content.constData());
                continue;
            }

            // ── 无条码帧：EPC 未读到（占位 NOREAD），仅记录不处理，不进入分拣 ──
            if (epc.isEmpty() || epc.compare("NOREAD", Qt::CaseInsensitive) == 0)
            {
                RFID_INFO("未读到EPC(NOREAD) seq=%s car=%s — 仅记录，不进入分拣",
                          seq.toLocal8Bit().constData(), carNum.toLocal8Bit().constData());
                continue;
            }

            // ── 组装与 handleRfidCarNumReport 兼容的 JSON（barcode 恒空 → 触发 HTTP 绑定查询）──
            QJsonObject item;
            item["epc"] = epc;
            item["carNum"] = carNum;
            item["seq"] = seq;   // ★ 2026-09-05 推送流水号（随数据进入下游保存/日志追溯）
            QJsonArray dataArr;
            dataArr.append(item);
            QJsonObject body;
            body["data"] = dataArr;

            RFID_INFO("[解析] 数据帧 seq=%s car=%s epc=%s → 交业务处理",
                      seq.toLocal8Bit().constData(), carNum.toLocal8Bit().constData(),
                      epc.toLocal8Bit().constData());
            emit rfidPushReceived(body);
        }
    }

    return HR_OK;
}
