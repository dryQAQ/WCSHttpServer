#include "PlcManager.h"
#include "SiemensPLC.h"
#include <QRegularExpression>
#include <QRegularExpressionMatchIterator>
#include <QDebug>
#include <tchar.h>

// ============================================================================
// 构造 / 析构
// ============================================================================

PlcManager::PlcManager(QObject* parent)
    : QObject(parent)
    , m_tcpServer(this)   // ★ this = CTcpServerListener*
{
    m_S7Plc = new CSiemensPLC();
    PLC_LOG_INFO("PLC管理器已创建");
}

PlcManager::~PlcManager()
{
    stop();
    delete m_S7Plc;
    m_S7Plc = nullptr;
    PLC_LOG_INFO("PLC管理器已销毁 send=%lld recv=%lld err=%lld",
        m_sendCount.load(), m_recvCount.load(), m_sendErrorCount.load());
}

// ============================================================================
// 启动 / 停止
// ============================================================================

bool PlcManager::start(const char* ip, int port)
{
    if (m_bRunning.load())
    {
        PLC_LOG_WARN("PLC服务已在运行中");
        return true;
    }

    if (ip) strncpy_s(m_plcConfig.szIp, sizeof(m_plcConfig.szIp), ip, 23);
    m_plcConfig.port = port;

    // 启动HP-Socket TCP Server
    if (!m_tcpServer->Start(_T("0.0.0.0"), m_plcConfig.port))
    {
        m_lastError = QString("PLC服务启动失败 port=%1 err=%2")
            .arg(m_plcConfig.port).arg((int)::GetLastError());
        PLC_LOG_ERROR("%s", m_lastError.toLocal8Bit().data());
        return false;
    }

    m_bRunning.store(true);
    m_uptime.start();
    m_sendCount.store(0);
    m_recvCount.store(0);
    m_sendErrorCount.store(0);

    PLC_LOG_INFO("PLC服务已启动 port=%d 等待PLC连接...", m_plcConfig.port);
    return true;
}

void PlcManager::stop()
{
    if (!m_bRunning.load()) return;

    // 断开 S7 连接
    disconnectS7();

    // 断开所有客户端
    {
        std::unique_lock<std::mutex> lock(m_clientMutex);
        for (auto& pair : m_mapClient)
        {
            m_tcpServer->Disconnect(pair.first, true);
            PLC_LOG_INFO("断开PLC连接 conn=%llu", (unsigned long long)pair.first);
        }
        m_mapClient.clear();
    }

    m_tcpServer->Stop();
    m_bRunning.store(false);
    PLC_LOG_INFO("PLC服务已停止");
}

// ============================================================================
// 发送指令（S7 DBWrite）
// ============================================================================

bool PlcManager::sendCodeInfo(const QString& barcode, const std::vector<int>& vecGrid, int car)
{
    if (!m_bRunning.load())
    {
        m_lastError = "PLC服务未运行";
        bool firstWarn = false;
        {
            std::lock_guard<std::mutex> lock(m_warnMutex);
            if (!m_warnedBarcodes.contains(barcode))
            {
                m_warnedBarcodes.insert(barcode);
                firstWarn = true;
            }
        }
        if (firstWarn)
        {
            PLC_LOG_WARN("发送失败: %s barcode=%s", m_lastError.toLocal8Bit().data(), barcode.toLocal8Bit().data());
        }
        return false;
    }

    // 检查 S7 连接
    if (!m_S7Plc || !m_S7Plc->isConnected())
    {
        m_lastError = "S7未连接PLC";
        bool firstWarn = false;
        {
            std::lock_guard<std::mutex> lock(m_warnMutex);
            if (!m_warnedBarcodes.contains(barcode))
            {
                m_warnedBarcodes.insert(barcode);
                firstWarn = true;
            }
        }
        if (firstWarn)
        {
            PLC_LOG_WARN("发送失败: %s barcode=%s", m_lastError.toLocal8Bit().data(), barcode.toLocal8Bit().data());
        }
        return false;
    }

    // 构建指令文本（用于日志，实际发送用 S7 二进制格式）
    QString command = "{";
    command += barcode;
    for (int g : vecGrid)
    {
        command += "|" + QString::number(g);
    }
    command += "|" + QString::number(car);
    command += "}";

    // 通过 S7 DBWrite 发送条码+格口信息
    QByteArray codeData = barcode.toLatin1();
    bool success = m_S7Plc->writeCodeInfo(codeData, vecGrid);

    if (success)
    {
        m_sendCount.fetch_add(1);
        PLC_LOG_INFO("S7发送PLC指令成功 code=%s cmd=%s",
            barcode.toLocal8Bit().data(), command.toLocal8Bit().data());
    }
    else
    {
        m_sendErrorCount.fetch_add(1);
        PLC_LOG_ERROR("S7发送PLC指令失败 code=%s err=%s",
            barcode.toLocal8Bit().data(), m_S7Plc->lastErrorText().toLocal8Bit().data());
    }

    return success;
}

bool PlcManager::sendRawCommand(const QString& command)
{
    if (command.isEmpty()) return false;

    QByteArray data = command.toLatin1();
    bool allSuccess = true;

    std::unique_lock<std::mutex> lock(m_clientMutex);
    for (auto& pair : m_mapClient)
    {
        CONNID clientId = pair.first;
        if (!m_tcpServer->Send(clientId, (const BYTE*)data.constData(), data.length()))
        {
            m_sendErrorCount.fetch_add(1);
            allSuccess = false;
            PLC_LOG_ERROR("PLC发送失败 conn=%llu cmd=%s err=%d",
                (unsigned long long)clientId, command.toLocal8Bit().data(),
                (int)::GetLastError());
        }
    }

    return allSuccess;
}

// ============================================================================
// 状态查询
// ============================================================================

int PlcManager::connectedClientCount() const
{
    std::unique_lock<std::mutex> lock(const_cast<std::mutex&>(m_clientMutex));
    return (int)m_mapClient.size();
}

PlcStats PlcManager::stats() const
{
    PlcStats s;
    s.running      = m_bRunning.load();
    s.port         = m_plcConfig.port;
    s.clientCount  = connectedClientCount();
    s.sendCount    = m_sendCount.load();
    s.recvCount    = m_recvCount.load();
    s.sendErrCount = m_sendErrorCount.load();
    s.uptimeSec    = uptimeSec();
    s.lastIp       = m_lastIp;
    s.lastError    = m_lastError;
    return s;
}

// ============================================================================
// S7 连接管理
// ============================================================================

bool PlcManager::connectS7(const char* ip)
{
    if (!m_S7Plc)
    {
        PLC_LOG_ERROR("S7客户端未初始化");
        return false;
    }

    // 如果已连接，先断开
    if (m_S7Plc->isConnected())
    {
        disconnectS7();
    }

    bool ok = m_S7Plc->connectTo(ip);
    if (ok)
    {
        m_s7Ip = QString::fromLatin1(ip);
        PLC_LOG_INFO("S7连接成功 ip=%s", ip);
    }
    else
    {
        m_s7Ip.clear();
        PLC_LOG_ERROR("S7连接失败 ip=%s err=%s", ip, m_S7Plc->lastErrorText().toLocal8Bit().data());
    }
    return ok;
}

void PlcManager::disconnectS7()
{
    if (m_S7Plc && m_S7Plc->isConnected())
    {
        m_S7Plc->disconnect();
        PLC_LOG_INFO("S7连接已断开 ip=%s", m_s7Ip.toLocal8Bit().data());
    }
    m_s7Ip.clear();
}

bool PlcManager::isS7Connected() const
{
    return m_S7Plc ? m_S7Plc->isConnected() : false;
}

qint64 PlcManager::uptimeSec() const
{
    if (!m_bRunning.load() || !m_uptime.isValid())
        return 0;
    return m_uptime.elapsed() / 1000;
}

// ============================================================================
// CTcpServerListener 回调
// ============================================================================

EnHandleResult PlcManager::OnPrepareListen(ITcpServer* pSender, SOCKET soListen)
{
    PLC_LOG_INFO("PLC监听端口准备就绪 port=%d", m_plcConfig.port);
    return HR_OK;
}

EnHandleResult PlcManager::OnAccept(ITcpServer* pSender, CONNID dwConnID, UINT_PTR soClient)
{
    TCHAR szIp[24] = { 0 };
    int ipLen = 24;
    USHORT port = 0;
    pSender->GetRemoteAddress(dwConnID, szIp, ipLen, port);

    {
        std::unique_lock<std::mutex> lock(m_clientMutex);
        m_mapClient.insert(std::make_pair(dwConnID, "client"));
    }

    // ★ PLC连接建立，重置发送失败警告集合
    {
        std::lock_guard<std::mutex> lock(m_warnMutex);
        m_warnedBarcodes.clear();
    }

    std::string sip = (char*)szIp;
    m_lastIp = QString::fromStdString(sip);
    PLC_LOG_INFO("PLC已连接 conn=%llu ip=%s port=%d total=%d",
        (unsigned long long)dwConnID, sip.c_str(), port, (int)m_mapClient.size());

    emit plcConnected(QString::fromStdString(sip), port);

    if (m_statusCb)
        m_statusCb(sip, port, true);

    return HR_OK;
}

EnHandleResult PlcManager::OnSend(ITcpServer* pSender, CONNID dwConnID,
                                   const BYTE* pData, int iLength)
{
    return HR_OK;
}

EnHandleResult PlcManager::OnReceive(ITcpServer* pSender, CONNID dwConnID,
                                      const BYTE* pData, int iLength)
{
    m_recvCount.fetch_add(1);

    // 拷贝数据到缓冲区
    QByteArray rawData((const char*)pData, iLength);
    PLC_LOG_INFO("收到PLC原始数据 conn=%llu len=%d data=%s",
        (unsigned long long)dwConnID, iLength, rawData.toHex().constData());

    // 转换为QString解析
    QString qdata = QString::fromLocal8Bit(rawData.constData(), rawData.size());
    parsePlcFeedback(qdata.toLocal8Bit());

    return HR_OK;
}

EnHandleResult PlcManager::OnClose(ITcpServer* pSender, CONNID dwConnID,
                                    EnSocketOperation enOperation, int iErrorCode)
{
    TCHAR szIp[24] = { 0 };
    int ipLen = 24;
    USHORT port = 0;
    pSender->GetRemoteAddress(dwConnID, szIp, ipLen, port);
    std::string sip = (char*)szIp;

    {
        std::unique_lock<std::mutex> lock(m_clientMutex);
        m_mapClient.erase(dwConnID);
    }

    PLC_LOG_INFO("PLC断开连接 conn=%llu ip=%s port=%d errCode=%d remaining=%d",
        (unsigned long long)dwConnID, sip.c_str(), port, iErrorCode,
        (int)m_mapClient.size());

    emit plcDisconnected(QString::fromStdString(sip), port);

    if (m_statusCb)
        m_statusCb(sip, port, false);

    return HR_OK;
}

EnHandleResult PlcManager::OnShutdown(ITcpServer* pSender)
{
    PLC_LOG_INFO("PLC服务关闭");
    return HR_OK;
}

// ============================================================================
// PLC反馈解析
// 协议格式（与WCSApp完全兼容）:
//   {barcode|grid|car}  — 落格确认
//   {start}             — 批次开始
//   {stop}              — 批次停止
//   支持粘包: {barcode1|grid1|car1}{barcode2|grid2|car2}
// ============================================================================

void PlcManager::parsePlcFeedback(const QByteArray& rawData)
{
    QString qdata = QString::fromLocal8Bit(rawData);

    // 使用正则表达式匹配每对{}中的内容
    QRegularExpression re("\\{(.*?)\\}");
    QRegularExpressionMatchIterator it = re.globalMatch(qdata);

    while (it.hasNext())
    {
        QRegularExpressionMatch match = it.next();
        QString content = match.captured(1).trimmed();

        if (content.isEmpty()) continue;

        PLC_LOG_INFO("PLC反馈解析: {%s}", content.toLocal8Bit().data());

        // 批次信号
        if (content == "start")
        {
            PLC_LOG_INFO("PLC批次开始信号");
            emit plcBatchStart();
            continue;
        }
        if (content == "stop")
        {
            PLC_LOG_INFO("PLC批次停止信号");
            emit plcBatchStop();
            continue;
        }

        // 落格确认: {barcode|grid|car}
        QStringList parts = content.split('|');
        if (parts.size() >= 2)
        {
            QString code = parts[0].trimmed();
            QString grid = parts[1].trimmed();
            QString car  = parts.size() >= 3 ? parts[2].trimmed() : "1";

            PLC_LOG_INFO("PLC反馈落格 code=%s grid=%s car=%s",
                code.toLocal8Bit().data(), grid.toLocal8Bit().data(), car.toLocal8Bit().data());

            // 记录生命周期
            LIFE_STAGE_PLC_FEEDBACK(code, grid);

            // 发射信号
            emit plcFeedbackReceived(code, grid, car);

            // 回调
            if (m_feedbackCb)
                m_feedbackCb(code, grid, car);
        }
        else
        {
            PLC_LOG_WARN("PLC反馈格式异常: {%s}", content.toLocal8Bit().data());
        }
    }
}