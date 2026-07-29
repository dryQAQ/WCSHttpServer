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
    // 停止 S7 锁格线程
    m_bS7ThreadStart = false;
    if (m_threadS7PLC.joinable())
        m_threadS7PLC.join();

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
// TCP: {条码|格口|小车号}
// 格口号格式: 3位补零，如格口15 → "015"
// 小车号格式: 3位补零，如小车1 → "001"
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

    // ── 1. S7 DBWrite 发送（与 WCSApp 完全一致）──
    bool s7Success = false;
    if (m_S7Plc && m_S7Plc->isConnected())
    {
        QByteArray sSend = barcode.toLatin1();
        s7Success = m_S7Plc->writeCodeInfo(sSend, vecGrid);
        if (s7Success)
            m_s7SendCount.fetch_add(1);
        else
            m_s7SendErrCount.fetch_add(1);
    }
    else
    {
        // S7 未连接时仅记录，不阻断（后续可配置为强制要求）
        PLC_LOG_WARN("S7未连接，跳过S7发送 barcode=%s", barcode.toLocal8Bit().data());
    }

    // ── 2. TCP 文本发送（与 WCSApp 一致）──
    bool tcpSuccess = false;

    std::unique_lock<std::mutex> clientLock(m_clientMutex);
    if (!m_mapClient.empty())
    {
        int nGrid = vecGrid.empty() ? 0 : vecGrid[0];

        QString b = QString("%1").arg(nGrid, 3, 10, QChar('0'));
        QString carStr = QString("%1").arg(car, 3, 10, QChar('0'));
        QString command = "{" + barcode + "|" + b + "|" + carStr + "}";

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
        m_lastBarcode = barcode;
        m_lastGrid = vecGrid.empty() ? "0" : QString("%1").arg(vecGrid[0], 3, 10, QChar('0'));
        m_lastCar = QString("%1").arg(car, 3, 10, QChar('0'));
        m_lastSendTimeMs = QDateTime::currentMSecsSinceEpoch();
    }

    // ── 4. 综合判断：S7 或 TCP 任一成功即视为成功 ──
    bool overallSuccess = s7Success || tcpSuccess;

    emit plcSendInfo(barcode, m_lastGrid + "|" + m_lastCar, overallSuccess);

    if (overallSuccess)
    {
        PLC_LOG_INFO("发送PLC指令成功 code=%s s7=%d tcp=%d",
            barcode.toLocal8Bit().data(), s7Success, tcpSuccess);
    }
    else
    {
        PLC_LOG_ERROR("发送PLC指令失败 code=%s s7=%d tcp=%d",
            barcode.toLocal8Bit().data(), s7Success, tcpSuccess);
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

    // 启动 S7 锁格轮询线程
    m_bS7ThreadStart = true;
    m_threadS7PLC = std::thread(&PlcManager::OnPlcS7Thread, this);

    emit s7Connected(QString::fromLocal8Bit(ip));
    return true;
}

void PlcManager::disconnectS7()
{
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
// S7 锁格轮询线程（与 WCSApp FrmMainV2::OnPlcS7Thread 完全一致）
// 每 1 秒读取 DB77 Offset 0, 25 字节（200位格口锁定位图）
// 上升沿 → 锁格，下降沿 → 解锁
// ============================================================================

void PlcManager::OnPlcS7Thread()
{
    while (m_bS7ThreadStart)
    {
        Sleep(PLC_S7_LOCK_INTERVAL_MS);

        if (!m_S7Plc || !m_S7Plc->isConnected())
            continue;

        byte tempData[PLC_S7_LOCK_READ_SIZE]{ 0 };

        if (!m_S7Plc->readData(PLC_S7_DB_READ, 0, PLC_S7_LOCK_READ_SIZE, tempData))
            continue;

        // 遍历 200 位格口锁定位图
        for (int i = 0; i < PLC_S7_LOCK_READ_SIZE; i++)
        {
            for (int a = 0; a < 8; a++)
            {
                int grid_status = i * 8 + a;

                bool bNew = S7_GetBitAt(tempData, i, a);
                bool bOld = S7_GetBitAt(m_s7PlcLastData, i, a);

                // 锁格（上升沿: 0→1）
                if (bNew && !bOld)
                {
                    QString s_grid = QString("%1").arg(grid_status, 3, 10, QChar('0'));
                    PLC_LOG_WARN("chutStatus %d, lock", grid_status);

                    LockGridInfo info;
                    info.gridNum = s_grid;
                    info.type = 0;  // 锁格

                    {
                        std::unique_lock<std::mutex> lock(m_lockGrid);
                        m_queueLockInfo.push(info);
                    }

                    emit gridLocked(s_grid);
                }

                // 解锁（下降沿: 1→0）
                if (bOld && !bNew)
                {
                    QString s_grid = QString("%1").arg(grid_status, 3, 10, QChar('0'));
                    PLC_LOG_WARN("chutStatus %d, unLock", grid_status);

                    LockGridInfo info;
                    info.gridNum = s_grid;
                    info.type = 1;  // 解锁

                    {
                        std::unique_lock<std::mutex> lock(m_lockGrid);
                        m_queueLockInfo.push(info);
                    }

                    emit gridUnlocked(s_grid);
                }
            }
        }

        // 更新锁格状态缓存
        memcpy(m_s7PlcLastData, tempData, PLC_S7_LOCK_READ_SIZE * sizeof(byte));

        {
            std::unique_lock<std::mutex> lock(m_lockGridPlc);
            for (int i = 0; i < PLC_S7_LOCK_READ_SIZE; i++)
            {
                for (int a = 0; a < 8; a++)
                {
                    int grid_status = i * 8 + a;
                    m_s7Grid_200[grid_status] = S7_GetBitAt(m_s7PlcLastData, i, a);
                }
            }
        }
    }
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
//   查询:  {barcode|car}         — 相机/PLC 扫描到条码，查询格口分配   [1~2字段]
//   反馈:  {barcode|grid|car}    — PLC 落格确认（分拣完成）            [≥3字段]
//   信号:  {start} / {stop}      — 批次开始/停止
//   支持粘包: {WV34S1|005}{WV34S2|006}{WV34S1|015|005|006|1}
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
        // ★ 查询报文: {barcode|car}  (2字段)
        //   相机扫描到条码 → WCS查格口 → 异步入池发送PLC分拣指令
        //   若条码不在波次中（lookup返回空），回退到反馈处理
        // ═══════════════════════════════════════════════════════════════
        if (partCount == 2 && m_lookupCb)
        {
            QString code = parts[0].trimmed();
            QString car  = parts[1].trimmed();

            // 查格口
            QString gridStr = m_lookupCb(code);
            if (!gridStr.isEmpty())
            {
                // 解析格口（支持逗号分隔的多格口 "1,2,3"）
                std::vector<int> vecGrid;
                for (const QString& g : gridStr.split(',', Qt::SkipEmptyParts))
                {
                    bool ok = false;
                    int n = g.trimmed().toInt(&ok);
                    if (ok && n > 0) vecGrid.push_back(n);
                }

                if (!vecGrid.empty())
                {
                    // ═══════════════════════════════════════════════════════════
                    // ★ 锁格过滤（参照 WCSApp FrmMainV2::OnPlcS7Thread）
                    //   多格口时：跳过已锁定的格口，选择第一个未锁定的
                    //   全部锁定：使用第一个格口（兜底）
                    //   单格口时：不做过滤（PLC 自行处理）
                    // ═══════════════════════════════════════════════════════════
                    if (vecGrid.size() > 1)
                    {
                        std::vector<int> unlocked;
                        int lockedCount = 0;
                        for (int g : vecGrid)
                        {
                            if (isGridLocked(g))
                                lockedCount++;
                            else
                                unlocked.push_back(g);
                        }

                        if (!unlocked.empty())
                        {
                            int chosen = unlocked[0];
                            PLC_LOG_INFO("锁格过滤 code=%s 总格口=%d 锁定=%d → 选择=%d",
                                code.toLocal8Bit().data(),
                                (int)vecGrid.size(), lockedCount, chosen);
                            vecGrid = { chosen };
                        }
                        else
                        {
                            // 全部锁定 → 使用第一个格口（兜底，与 WCSApp 一致）
                            int fallback = vecGrid[0];
                            PLC_LOG_WARN("锁格过滤 code=%s 所有格口已锁定(%d个) → 兜底=%d",
                                code.toLocal8Bit().data(), (int)vecGrid.size(), fallback);
                            vecGrid = { fallback };
                        }
                    }

                    int carNum = car.toInt();
                    if (carNum <= 0) carNum = 1;

                    // ★★★ 关键：异步入池发送，防止 S7 DBWrite 同步阻塞 HP-Socket I/O 线程 ★★★
                    // S7 DBWrite 是 Snap7 库的同步调用，会阻塞当前线程直到 PLC 响应（10~100ms）
                    // 高并发场景下若在 I/O 线程直接调用，会耗尽 HP-Socket 工作线程导致无法接收新数据
                    if (m_pSendPool)
                    {
                        m_pSendPool->commitNoWait([this, code, vecGrid, carNum, gridStr]() {
                            sendCodeInfo(code, vecGrid, carNum);
                            emit plcSendInfo(code, gridStr, true);
                        });
                    }
                    else
                    {
                        // 降级：线程池未就绪时同步发送
                        sendCodeInfo(code, vecGrid, carNum);
                        emit plcSendInfo(code, gridStr, true);
                    }
                    continue;
                }
                else
                {
                    PLC_LOG_WARN("格口号解析失败: code=%s gridStr=%s",
                        code.toLocal8Bit().data(), gridStr.toLocal8Bit().data());
                    continue;
                }
            }
            // 条码不在波次中 → 回退，当作反馈报文处理
            PLC_LOG_WARN("条码不在波次中，回退为反馈处理 code=%s", code.toLocal8Bit().data());
        }

        // ═══════════════════════════════════════════════════════════════
        // 反馈报文: {barcode|grid|car} (≥3字段)  — PLC反馈落格确认
        // ═══════════════════════════════════════════════════════════════
        if (parts.size() >= 2)
        {
            QString code = parts[0].trimmed();
            QString grid = parts[1].trimmed();
            QString car  = parts.size() >= 3 ? parts[2].trimmed() : "1";

            // 更新最近接收数据
            {
                std::lock_guard<std::mutex> lock(m_lastDataMutex);
                m_lastRecvCode = code;
                m_lastRecvGrid = grid;
                m_lastRecvCar  = car;
            }

            // 记录生命周期
            LIFE_STAGE_PLC_FEEDBACK(code, grid);

            // ★ 业务信号：每个反馈都发射（HttpServer需要逐条标记分拣）
            emit plcFeedbackReceived(code, grid, car);

            // ★ 回调（保持兼容）
            if (m_feedbackCb)
                m_feedbackCb(code, grid, car);

            // ★ 添加到批量缓冲区（供UI日志批量刷新，减少UI线程压力）
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
        // 预留容量，避免频繁重新分配
        // 预估：100ms内最多 ~50条反馈（500条/秒的速度）
        m_feedbackBatchBuffer.reserve(64);
    }

    emit plcFeedbackBatch(batch);
}