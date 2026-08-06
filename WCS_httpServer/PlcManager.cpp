#include "PlcManager.h"
#include "SiemensPLC.h"
#include "ConfigManager.h"
#include <QRegularExpression>
#include <QRegularExpressionMatchIterator>
#include <QDebug>
#include <QThread>
#include <tchar.h>
#include <Windows.h>

// ============================================================================
// 构造 / 析构
// ============================================================================

PlcManager::PlcManager(QObject* parent)
    : QObject(parent)
    , m_tcpServer(this)
{
    // ★ 注册自定义类型到 Qt 元类型系统（QueuedConnection 跨线程信号需要）
    qRegisterMetaType<PlcFeedbackEntry>("PlcFeedbackEntry");
    qRegisterMetaType<QVector<PlcFeedbackEntry>>("QVector<PlcFeedbackEntry>");

    // ★ 创建 PLC 发送专用线程池（S7 DBWrite 同步阻塞 10~100ms，异步入池防 I/O 线程阻塞）
    {
        AppConfig& cfg = ConfigManager::instance()->config();
        m_pSendPool = new Hanchine::ThreadPool(cfg.plcSendPoolSize);
        PLC_LOG_INFO("PLC管理器已创建 sendPool=%d", cfg.plcSendPoolSize);
    }
}

PlcManager::~PlcManager()
{
    // 停止 S7 心跳线程
    m_bHeartThreadStart = false;
    if (m_heartThread.joinable())
        m_heartThread.join();

    // 断开 S7 连接
    disconnectS7();

    stop();

    // 销毁发送线程池
    if (m_pSendPool)
    {
        delete m_pSendPool;
        m_pSendPool = nullptr;
    }

    PLC_LOG_INFO("PLC管理器已销毁 tcpSend=%lld tcpErr=%lld s7Send=%lld s7Err=%lld recv=%lld",
        m_tcpSendCount.load(), m_tcpSendErrCount.load(),
        m_s7SendCount.load(), m_s7SendErrCount.load(),
        m_recvCount.load());
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
    m_tcpSendCount.store(0);
    m_recvCount.store(0);
    m_tcpSendErrCount.store(0);
    m_s7SendCount.store(0);
    m_s7SendErrCount.store(0);

    // ★ 启动批量反馈定时器（高并发场景下减少信号频率）
    if (!m_feedbackBatchTimer)
    {
        m_feedbackBatchTimer = new QTimer(this);
        m_feedbackBatchTimer->setTimerType(Qt::PreciseTimer);
        connect(m_feedbackBatchTimer, &QTimer::timeout, this, &PlcManager::flushFeedbackBatch);
    }
    m_feedbackBatchTimer->start(PLC_FEEDBACK_BATCH_INTERVAL_MS);

    PLC_LOG_INFO("PLC服务已启动 port=%d 等待PLC连接...", m_plcConfig.port);
    return true;
}

void PlcManager::stop()
{
    if (!m_bRunning.load()) return;

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

    // ★ 停止批量反馈定时器，最后一次刷新
    if (m_feedbackBatchTimer)
    {
        m_feedbackBatchTimer->stop();
        flushFeedbackBatch();
    }

    PLC_LOG_INFO("PLC服务已停止");
}

// ============================================================================
// 发送指令（S7 DBWrite + TCP 文本协议）
// S7: DB1 Offset 1000, 42 bytes (与 WCSApp 完全兼容)
// TCP: {识别码|格口|小车号}
//      TODO: 识别码可能为条码或EPC，客户尚未确定（2026-08-04）
//      TODO: 小车号应由RFID提供，客户尚未提供RFID小车号字段，当前默认=1（2026-08-04）
// 格口号格式: 3位补零，如格口15 → "015"
// 小车号格式: 3位补零，如小车1 → "001"
// ============================================================================

bool PlcManager::sendCodeInfo(const QString& code, const std::vector<int>& vecGrid, int car)
{
    if (!m_bRunning.load())
    {
        m_lastError = "PLC服务未运行";
        bool firstWarn = false;
        {
            std::lock_guard<std::mutex> lock(m_warnMutex);
            if (!m_warnedBarcodes.contains(code))
            {
                m_warnedBarcodes.insert(code);
                firstWarn = true;
            }
        }
        if (firstWarn)
        {
            PLC_LOG_WARN("发送失败: %s code=%s", m_lastError.toLocal8Bit().data(), code.toLocal8Bit().data());
        }
        return false;
    }

    // ── 1. S7 DBWrite 发送（与 WCSApp 完全一致）──
    bool s7Success = false;
    if (m_S7Plc && m_S7Plc->isConnected())
    {
        QByteArray sSend = code.toLatin1();
        s7Success = m_S7Plc->writeCodeInfo(sSend, vecGrid);
        if (s7Success)
            m_s7SendCount.fetch_add(1);
        else
            m_s7SendErrCount.fetch_add(1);
    }
    else
    {
        // S7 未连接时仅记录，不阻断（后续可配置为强制要求）
        PLC_LOG_WARN("S7未连接，跳过S7发送 code=%s", code.toLocal8Bit().data());
    }

    // ── 2. TCP 文本发送（与 WCSApp 一致）──
    bool tcpSuccess = false;

    std::unique_lock<std::mutex> clientLock(m_clientMutex);
    if (!m_mapClient.empty())
    {
        int nGrid = vecGrid.empty() ? 0 : vecGrid[0];

        QString b = QString("%1").arg(nGrid, 3, 10, QChar('0'));
        QString carStr = QString("%1").arg(car, 3, 10, QChar('0'));
        QString command = "{" + code + "|" + b + "|" + carStr + "}";

        QByteArray data = command.toLatin1();
        bool allSuccess = true;

        for (auto& pair : m_mapClient)
        {
            CONNID clientId = pair.first;
            if (!m_tcpServer->Send(clientId, (const BYTE*)data.constData(), data.length()))
            {
                m_tcpSendErrCount.fetch_add(1);
                allSuccess = false;
                PLC_LOG_ERROR("TCP发送失败 conn=%llu cmd=%s err=%d",
                    (unsigned long long)clientId, command.toLocal8Bit().data(),
                    (int)::GetLastError());
            }
        }

        if (allSuccess)
        {
            m_tcpSendCount.fetch_add(1);
            tcpSuccess = true;
        }
    }
    clientLock.unlock();

    // ── 3. 更新最近发送数据 ──
    {
        std::lock_guard<std::mutex> lock(m_lastDataMutex);
        m_lastBarcode = code;
        m_lastGrid = vecGrid.empty() ? "0" : QString("%1").arg(vecGrid[0], 3, 10, QChar('0'));
        m_lastCar = QString("%1").arg(car, 3, 10, QChar('0'));
        m_lastSendTimeMs = QDateTime::currentMSecsSinceEpoch();
    }

    // ── 4. 综合判断：S7 或 TCP 任一成功即视为成功 ──
    bool overallSuccess = s7Success || tcpSuccess;

    emit plcSendInfo(code, m_lastGrid + "|" + m_lastCar, overallSuccess);

    if (overallSuccess)
    {
        PLC_LOG_INFO("发送PLC指令成功 code=%s s7=%d tcp=%d",
            code.toLocal8Bit().data(), s7Success, tcpSuccess);
    }
    else
    {
        PLC_LOG_ERROR("发送PLC指令失败 code=%s s7=%d tcp=%d",
            code.toLocal8Bit().data(), s7Success, tcpSuccess);
    }

    return overallSuccess;
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
            m_tcpSendErrCount.fetch_add(1);
            allSuccess = false;
            PLC_LOG_ERROR("PLC发送失败 conn=%llu cmd=%s err=%d",
                (unsigned long long)clientId, command.toLocal8Bit().data(),
                (int)::GetLastError());
        }
    }

    return allSuccess;
}

// ============================================================================
// sendBatchCodes — 主动发送模式：批量发送波次识别码到PLC（与WCSApp一致）
// 不等待PLC查询报文，遍历波次所有识别码主动发送PLC分拣指令
// TODO: 识别码可能为条码或EPC，客户尚未确定（2026-08-04）
// codeGridMap: 识别码→格口字符串（如 "15" 或 "1,2,3"）
// ============================================================================
bool PlcManager::sendBatchCodes(const QMap<QString, QString>& codeGridMap)
{
    if (codeGridMap.isEmpty())
    {
        PLC_LOG_WARN("sendBatchCodes: 条码映射为空，跳过");
        return false;
    }

    PLC_LOG_INFO("sendBatchCodes: 开始批量发送 total=%d", codeGridMap.size());

    int successCount = 0;
    int failCount = 0;

    for (auto it = codeGridMap.constBegin(); it != codeGridMap.constEnd(); ++it)
    {
        const QString& code = it.key();
        const QString& gridStr = it.value();

        // 解析格口（支持逗号分隔的多格口 "1,2,3"）
        std::vector<int> vecGrid;
        for (const QString& g : gridStr.split(',', Qt::SkipEmptyParts))
        {
            bool ok = false;
            int n = g.trimmed().toInt(&ok);
            if (ok && n > 0) vecGrid.push_back(n);
        }

        if (vecGrid.empty())
        {
            PLC_LOG_WARN("sendBatchCodes: 格口解析失败 code=%s gridStr=%s",
                code.toLocal8Bit().data(), gridStr.toLocal8Bit().data());
            failCount++;
            continue;
        }

        // ★ 锁格过滤（多格口时跳过已锁定格口，与WCSApp一致）
        if (vecGrid.size() > 1)
        {
            std::vector<int> unlocked;
            for (int g : vecGrid)
            {
                if (!isGridLocked(g))
                    unlocked.push_back(g);
            }
            if (!unlocked.empty())
            {
                vecGrid = { unlocked[0] };
            }
            // 全部锁定则使用第一个格口（兜底）
        }

        // ★ 异步入池发送（与查询模式一致，防止S7阻塞）
        if (m_pSendPool)
        {
            m_pSendPool->commitNoWait([this, code, vecGrid]() {
                sendCodeInfo(code, vecGrid, 1);
            });
        }
        else
        {
            sendCodeInfo(code, vecGrid, 1);
        }

        successCount++;
    }

    PLC_LOG_INFO("sendBatchCodes: 批量发送完成 success=%d fail=%d total=%d",
        successCount, failCount, codeGridMap.size());

    return failCount == 0;
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
    s.uptimeSec    = uptimeSec();

    // ── S7 状态 ──
    s.s7Connected    = isS7Connected();
    s.s7Ip           = QString::fromLocal8Bit(m_plcConfig.szS7Ip);
    s.s7SendCount    = m_s7SendCount.load();
    s.s7SendErrCount = m_s7SendErrCount.load();

    // ── TCP 连接 ──
    {
        std::lock_guard<std::mutex> lock(m_clientMutex);
        s.clientCount = (int)m_mapClient.size();
    }
    s.recvCount    = m_recvCount.load();
    s.lastIp       = m_lastIp;
    s.lastPort     = m_lastPort;

    // ── TCP 发送统计 ──
    s.tcpSendCount    = m_tcpSendCount.load();
    s.tcpSendErrCount = m_tcpSendErrCount.load();

    // ── 锁格统计 ──
    s.lockedGridCount = lockedGridCount();

    // ── 最近数据 ──
    {
        std::lock_guard<std::mutex> lock(m_lastDataMutex);
        s.lastBarcode  = m_lastBarcode;
        s.lastGrid     = m_lastGrid;
        s.lastCar      = m_lastCar;
        s.lastRecvCode = m_lastRecvCode;
        s.lastRecvGrid = m_lastRecvGrid;
        s.lastRecvCar  = m_lastRecvCar;
        s.lastError    = m_lastError;
        s.lastSendTimeMs = m_lastSendTimeMs;
        s.lastRecvTimeMs = m_lastRecvTimeMs;
    }

    return s;
}

qint64 PlcManager::uptimeSec() const
{
    if (!m_bRunning.load() || !m_uptime.isValid())
        return 0;
    return m_uptime.elapsed() / 1000;
}

// ============================================================================
// S7 连接管理
// ============================================================================

bool PlcManager::connectS7(const char* ip)
{
    if (m_S7Plc)
    {
        PLC_LOG_WARN("S7已连接，先断开旧连接");
        disconnectS7();
    }

    m_S7Plc = new CSiemensPLC();
    if (!m_S7Plc->connectTo(ip))
    {
        QString err = m_S7Plc->lastErrorText();
        PLC_LOG_ERROR("S7连接失败 ip=%s err=%s", ip, err.toLocal8Bit().data());
        emit s7Error(QString("S7连接失败: %1").arg(err));
        delete m_S7Plc;
        m_S7Plc = nullptr;
        return false;
    }

    strncpy_s(m_plcConfig.szS7Ip, sizeof(m_plcConfig.szS7Ip), ip, 23);
    PLC_LOG_INFO("S7连接成功 ip=%s", ip);

    // 启动 S7 心跳线程（与 WCSApp simensS7::OnHeartThread 一致）
    m_bHeartThreadStart = true;
    m_heartThread = std::thread(&PlcManager::OnS7HeartThread, this);

    // ★ 启动 S7 锁格轮询定时器（与 WCSApp FrmMainV2 S7 边沿检测一致）
    if (!m_s7LockTimer)
    {
        m_s7LockTimer = new QTimer(this);
        connect(m_s7LockTimer, &QTimer::timeout, this, &PlcManager::pollS7LockStatus);
    }
    m_s7LockTimer->start(PLC_S7_LOCK_INTERVAL_MS);
    memset(m_s7PlcLastData, 0, PLC_S7_LOCK_READ_SIZE);  // 重置上次数据，避免重连后误判边沿
    PLC_LOG_INFO("S7锁格轮询已启动 interval=%dms", PLC_S7_LOCK_INTERVAL_MS);

    emit s7Connected(QString::fromLocal8Bit(ip));
    return true;
}

void PlcManager::disconnectS7()
{
    // ★ 先停止 S7 锁格轮询定时器
    if (m_s7LockTimer)
    {
        m_s7LockTimer->stop();
        PLC_LOG_INFO("S7锁格轮询已停止");
    }

    // ★ 先停止心跳线程
    m_bHeartThreadStart = false;
    if (m_heartThread.joinable())
        m_heartThread.join();

    if (m_S7Plc)
    {
        m_S7Plc->disconnect();
        delete m_S7Plc;
        m_S7Plc = nullptr;
        PLC_LOG_INFO("S7连接已断开");
        emit s7Disconnected(QString::fromLocal8Bit(m_plcConfig.szS7Ip));
    }
}

bool PlcManager::isS7Connected() const
{
    return m_S7Plc ? m_S7Plc->isConnected() : false;
}

// ============================================================================
// S7 锁格边沿检测轮询（与 WCSApp FrmMainV2 S7 轮询完全一致）
// DB77 Offset 0, 25 bytes (200位锁格状态)
// 每 PLC_S7_LOCK_INTERVAL_MS(1s) 读取一次，逐位比较检测上升沿/下降沿
// TCP 主动消息 {grid|L}/{grid|U} 作为补充机制，双重保障
// ============================================================================
void PlcManager::pollS7LockStatus()
{
    if (!m_S7Plc || !m_S7Plc->isConnected())
        return;

    byte tempData[PLC_S7_LOCK_READ_SIZE] = { 0 };
    if (!m_S7Plc->readData(PLC_S7_DB_READ, 0, PLC_S7_LOCK_READ_SIZE, tempData))
    {
        // DB77 读取失败，静默跳过（避免刷屏）
        return;
    }

    int lockCount = 0;
    int unlockCount = 0;

    // 逐位比较：25字节 × 8位 = 200位
    for (int i = 0; i < PLC_S7_LOCK_READ_SIZE; i++)
    {
        for (int a = 0; a < 8; a++)
        {
            int gridNum = i * 8 + a;
            if (gridNum >= PLC_S7_MAX_GRID_COUNT)
                break;

            bool bCurr = S7_GetBitAt(tempData, i, a);
            bool bPrev = S7_GetBitAt(m_s7PlcLastData, i, a);

            // 上升沿 → 锁格
            if (bCurr && !bPrev)
            {
                QString s_grid = QString("%1").arg(gridNum, 3, 10, QChar('0'));
                PLC_LOG_WARN("S7锁格检测(上升沿) grid=%d lock", gridNum);

                // 更新锁格状态缓存
                {
                    std::unique_lock<std::mutex> lock(m_lockGridPlc);
                    m_s7Grid_200[gridNum] = true;
                }

                // 入队 + 发射信号
                LockGridInfo info;
                info.gridNum = s_grid;
                info.type = 0;  // 锁格
                {
                    std::unique_lock<std::mutex> lock(m_lockGrid);
                    m_queueLockInfo.push(info);
                }
                emit gridLocked(s_grid);
                lockCount++;
            }

            // 下降沿 → 解锁
            if (!bCurr && bPrev)
            {
                QString s_grid = QString("%1").arg(gridNum, 3, 10, QChar('0'));
                PLC_LOG_WARN("S7锁格检测(下降沿) grid=%d unlock", gridNum);

                {
                    std::unique_lock<std::mutex> lock(m_lockGridPlc);
                    m_s7Grid_200[gridNum] = false;
                }

                LockGridInfo info;
                info.gridNum = s_grid;
                info.type = 1;  // 解锁
                {
                    std::unique_lock<std::mutex> lock(m_lockGrid);
                    m_queueLockInfo.push(info);
                }
                emit gridUnlocked(s_grid);
                unlockCount++;
            }
        }
    }

    // 保存本次数据，供下次边沿检测
    memcpy(m_s7PlcLastData, tempData, PLC_S7_LOCK_READ_SIZE);

    if (lockCount > 0 || unlockCount > 0)
    {
        PLC_LOG_INFO("S7锁格轮询完成 lock=%d unlock=%d totalLocked=%d",
            lockCount, unlockCount, lockedGridCount());
    }
}

// ============================================================================
// S7 心跳线程（与 WCSApp simensS7::OnHeartThread 完全一致）
// 每 2 秒检测 S7 连接状态，断线自动重连
// ============================================================================
void PlcManager::OnS7HeartThread()
{
    PLC_LOG_INFO("S7心跳线程已启动");

    while (m_bHeartThreadStart)
    {
        Sleep(2000);

        if (!m_bHeartThreadStart)
            break;

        if (m_S7Plc && !m_S7Plc->isConnected())
        {
            PLC_LOG_WARN("S7心跳检测到断线，尝试重连 ip=%s", m_plcConfig.szS7Ip);
            Sleep(100);
            m_S7Plc->connectTo(m_plcConfig.szS7Ip);
            if (m_S7Plc->isConnected())
            {
                PLC_LOG_INFO("S7心跳重连成功 ip=%s", m_plcConfig.szS7Ip);
                emit s7Connected(QString::fromLocal8Bit(m_plcConfig.szS7Ip));
            }
            else
            {
                PLC_LOG_ERROR("S7心跳重连失败 ip=%s", m_plcConfig.szS7Ip);
            }
        }
    }

    PLC_LOG_INFO("S7心跳线程已退出");
}

// ============================================================================
// 锁格查询
// ============================================================================

bool PlcManager::isGridLocked(int grid) const
{
    if (grid < 0 || grid >= PLC_S7_MAX_GRID_COUNT)
        return false;

    std::unique_lock<std::mutex> lock(m_lockGridPlc);
    return m_s7Grid_200[grid];
}

int PlcManager::lockedGridCount() const
{
    int count = 0;
    std::unique_lock<std::mutex> lock(m_lockGridPlc);
    for (int i = 0; i < PLC_S7_MAX_GRID_COUNT; i++)
    {
        if (m_s7Grid_200[i]) count++;
    }
    return count;
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
    m_lastPort = port;
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
    m_lastRecvTimeMs = QDateTime::currentMSecsSinceEpoch();

    // ★ 仅拷贝数据到缓冲区，解析和信号发射交给 parsePlcFeedback
    //    避免在 HP-Socket 工作线程中做耗时操作
    QByteArray rawData((const char*)pData, iLength);
    parsePlcFeedback(rawData);

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
//   反馈:  {barcode|grid|car}    — PLC 落格确认（分拣完成）            [≥3字段]
//   锁格:  {grid|L}              — PLC主动锁格（如 {222|L}）           [2字段, L]
//   解锁:  {grid|U}              — PLC主动解锁（如 {222|U}）           [2字段, U]
//   信号:  {start} / {stop}      — 批次开始/停止
//   支持粘包: {WV34S1|015|005}{WV34S2|016|006}{WV34S1|015|005|006|1}
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

        // 批次信号（低频，直接发射）
        if (content == "start")
        {
            emit plcBatchStart();
            continue;
        }
        if (content == "stop")
        {
            emit plcBatchStop();
            continue;
        }

        // 按 | 分割字段
        QStringList parts = content.split('|');
        int partCount = parts.size();

        // ═══════════════════════════════════════════════════════════════
        // ★ TCP 锁格/解锁消息: {grid|L} 或 {grid|U}  (2字段, 第二位为L或U)
        //   PLC主动发送锁格/解锁消息，与WCSApp一致
        //   {222|L} → 格口222锁定，{222|U} → 格口222解锁
        // ═══════════════════════════════════════════════════════════════
        if (partCount == 2)
        {
            QString field0 = parts[0].trimmed();
            QString field1 = parts[1].trimmed().toUpper();

            if (field1 == "L" || field1 == "U")
            {
                bool isLock = (field1 == "L");
                bool ok = false;
                int gridNum = field0.toInt(&ok);

                if (ok && gridNum >= 0 && gridNum < PLC_S7_MAX_GRID_COUNT)
                {
                    if (isLock)
                    {
                        PLC_LOG_WARN("TCP锁格消息: grid=%d lock", gridNum);

                        // 更新锁格状态缓存
                        {
                            std::unique_lock<std::mutex> lock(m_lockGridPlc);
                            m_s7Grid_200[gridNum] = true;
                        }

                        QString s_grid = QString("%1").arg(gridNum, 3, 10, QChar('0'));
                        LockGridInfo info;
                        info.gridNum = s_grid;
                        info.type = 0;  // 锁格
                        {
                            std::unique_lock<std::mutex> lock(m_lockGrid);
                            m_queueLockInfo.push(info);
                        }

                        emit gridLockedByPlc(s_grid);
                        emit gridLocked(s_grid);  // 兼容旧信号
                    }
                    else
                    {
                        PLC_LOG_WARN("TCP解锁消息: grid=%d unlock", gridNum);

                        // 更新锁格状态缓存
                        {
                            std::unique_lock<std::mutex> lock(m_lockGridPlc);
                            m_s7Grid_200[gridNum] = false;
                        }

                        QString s_grid = QString("%1").arg(gridNum, 3, 10, QChar('0'));
                        LockGridInfo info;
                        info.gridNum = s_grid;
                        info.type = 1;  // 解锁
                        {
                            std::unique_lock<std::mutex> lock(m_lockGrid);
                            m_queueLockInfo.push(info);
                        }

                        emit gridUnlockedByPlc(s_grid);
                        emit gridUnlocked(s_grid);  // 兼容旧信号
                    }
                    continue;
                }
                else
                {
                    PLC_LOG_WARN("TCP锁格消息格式异常: grid=%s type=%s", field0.toLocal8Bit().data(), field1.toLocal8Bit().data());
                    continue;
                }
            }
        }

        // ═══════════════════════════════════════════════════════════════
        // 反馈报文: {barcode|grid|car} (3字段)  — PLC反馈落格确认
        // 与WCSApp一致：WCS主动发送PLC指令，不等待PLC查询，2字段消息仅处理锁格/解锁
        // ═══════════════════════════════════════════════════════════════
        if (parts.size() >= 3)
        {
            QString code = parts[0].trimmed();
            QString grid = parts[1].trimmed();
            QString car  = parts[2].trimmed();

            // 更新最近接收数据
            {
                std::lock_guard<std::mutex> lock(m_lastDataMutex);
                m_lastRecvCode = code;
                m_lastRecvGrid = grid;
                m_lastRecvCar  = car;
            }

            // 记录生命周期
            LIFE_STAGE_PLC_FEEDBACK(code, grid);

            // ★ 回调（保持兼容）
            if (m_feedbackCb)
                m_feedbackCb(code, grid, car);

            // ★ 添加到批量缓冲区（100ms 定时刷新）
            {
                std::lock_guard<std::mutex> lock(m_feedbackBatchMutex);
                if (m_feedbackBatchBuffer.size() < PLC_FEEDBACK_BATCH_MAX_SIZE)
                {
                    PlcFeedbackEntry entry;
                    entry.code       = code;
                    entry.grid       = grid;
                    entry.car        = car;
                    entry.timestampMs = QDateTime::currentMSecsSinceEpoch();
                    m_feedbackBatchBuffer.append(entry);
                }
            }
        }
        else
        {
            PLC_LOG_WARN("PLC反馈格式异常: {%s}", content.toLocal8Bit().data());
        }
    }
}

// ============================================================================
// 批量反馈刷新（由 QTimer 定时触发，将缓冲区中的反馈统一发送给UI）
// 设计目的：高并发落格场景下，将N条反馈合并为1次信号，减少UI线程事件队列压力
// ============================================================================

void PlcManager::flushFeedbackBatch()
{
    QVector<PlcFeedbackEntry> batch;
    {
        std::lock_guard<std::mutex> lock(m_feedbackBatchMutex);
        if (m_feedbackBatchBuffer.isEmpty()) return;
        batch.swap(m_feedbackBatchBuffer);
        m_feedbackBatchBuffer.reserve(64);
    }

    // ★ UI 日志信号（MainWindow 处理）
    emit plcFeedbackBatch(batch);

    // ★ 业务信号（HttpServer 处理：markSorted + GridSortRecord）
    //    将 N 条反馈合并为 1 次 QueuedConnection 事件，避免主线程事件队列洪水
    emit plcFeedbackBusinessBatch(batch);
}