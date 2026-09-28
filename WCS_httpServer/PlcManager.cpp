#include "PlcManager.h"
#include "SiemensPLC.h"
#include "ConfigManager.h"
#include "EpcCache.h"
#include <QRegularExpression>
#include <QRegularExpressionMatchIterator>
#include <QDebug>
#include <QThread>
#include <tchar.h>
#include <Windows.h>
#include <vector>
#include "define.h"

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

    // ★ 2026-09-04 生产增强：S7 连接成功（首次成功 / 心跳线程重连成功）→ 在主线程启动锁格轮询
    //   心跳线程是 std::thread，不能直接操作主线程 QTimer（跨线程操作 QTimer 未定义行为），
    //   故经 s7Connected 信号（auto 连接：跨线程自动 Queued）回到主线程执行
    connect(this, &PlcManager::s7Connected, this, [this]() {
        if (!m_s7LockTimer)
        {
            m_s7LockTimer = new QTimer(this);
            connect(m_s7LockTimer, &QTimer::timeout, this, &PlcManager::pollS7LockStatus);
        }
        if (!m_s7LockTimer->isActive())
            m_s7LockTimer->start(PLC_S7_LOCK_INTERVAL_MS);
        memset(m_s7PlcLastData, 0, PLC_S7_LOCK_READ_SIZE);  // 重置上次数据，避免重连后误判边沿
        PLC_LOG_INFO("S7锁格轮询已启动 interval=%dms", PLC_S7_LOCK_INTERVAL_MS);
    });
}

PlcManager::~PlcManager()
{
    // ★ 2026-09-04 崩溃定位日志
    PLC_LOG_INFO("[析构] PlcManager 开始销毁");
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
    PLC_LOG_INFO("[析构] PlcManager 销毁完成 tcpSend=%lld tcpErr=%lld recv=%lld",
        m_tcpSendCount.load(), m_tcpSendErrCount.load(),
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
    PLC_LOG_INFO("PLC服务停止 开始 running=%d", m_bRunning.load() ? 1 : 0);

    // ★ 2026-09-06 防死锁/迭代器失效：
    //   原实现持 m_clientMutex 遍历 Disconnect——Disconnect 可能在工作线程同步触发 OnClose，
    //   OnClose 内再次加 m_clientMutex（非递归锁）→ 死锁；或迭代中 map 被 erase → 迭代器失效崩溃。
    //   改为：先快照 connId，锁外逐个 Disconnect，最后持锁清空。
    std::vector<CONNID> connIds;
    {
        std::unique_lock<std::mutex> lock(m_clientMutex);
        for (auto& pair : m_mapClient)
            connIds.push_back(pair.first);
    }
    PLC_LOG_INFO("PLC服务停止 断开连接数=%d", (int)connIds.size());
    for (CONNID id : connIds)
    {
        m_tcpServer->Disconnect(id, true);
        PLC_LOG_INFO("断开PLC连接 conn=%llu", (unsigned long long)id);
    }
    {
        std::unique_lock<std::mutex> lock(m_clientMutex);
        m_mapClient.clear();
    }
    PLC_LOG_INFO("PLC服务停止 连接已清空，执行 Server.Stop()");

    m_tcpServer->Stop();
    m_bRunning.store(false);
    PLC_LOG_INFO("PLC服务停止 Server.Stop() 完成");

    // ★ 停止批量反馈定时器，最后一次刷新
    if (m_feedbackBatchTimer)
    {
        m_feedbackBatchTimer->stop();
        flushFeedbackBatch();
    }
    PLC_LOG_INFO("PLC服务停止 反馈刷新完成，stop() 返回");
}

// ============================================================================
// 发送指令（S7 DBWrite + TCP 文本协议）
// S7: DB1 Offset 1000, 42 bytes (与 WCSApp 完全兼容)
// TCP: {识别码|格口|小车号}
//      识别码 = EPC（商品编码），客户已确认EPC（商品编码）即EPC编码（2026-08-10）
//      TODO: 小车号应由RFID提供，客户尚未提供RFID小车号字段，当前默认=1（2026-08-04）
// 格口号格式: 3位补零，如格口15 → "015"
// 小车号格式: 3位补零，如小车1 → CAR_NUM_STR(DEFAULT_CAR_NUM)="001"
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

    // ── 1. S7 DBWrite 发送 ──
    // ★ 已注释：与PLC交互仅保留 TCP消息发送+接收反馈 和 锁格检测(S7 DB77读取)
    //           S7 DB1分拣指令写入不再需要，PLC通过TCP文本协议接收分拣指令
    // bool s7Success = false;
    // if (m_S7Plc && m_S7Plc->isConnected())
    // {
    //     QByteArray sSend = code.toLatin1();
    //     s7Success = m_S7Plc->writeCodeInfo(sSend, vecGrid);
    //     if (s7Success)
    //         m_s7SendCount.fetch_add(1);
    //     else
    //         m_s7SendErrCount.fetch_add(1);
    // }
    // else
    // {
    //     // S7 未连接时仅记录，不阻断（后续可配置为强制要求）
    //     PLC_LOG_WARN("S7未连接，跳过S7发送 code=%s", code.toLocal8Bit().data());
    // }

    // ── 2. TCP 文本发送 ──
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
        int clientCount = (int)m_mapClient.size();
        int sentCount = 0;
        int errCount = 0;

        qint64 t1 = QDateTime::currentMSecsSinceEpoch();

        for (auto& pair : m_mapClient)
        {
            CONNID clientId = pair.first;
            if (!m_tcpServer->Send(clientId, (const BYTE*)data.constData(), data.length()))
            {
                m_tcpSendErrCount.fetch_add(1);
                allSuccess = false;
                errCount++;
                int winErr = (int)::GetLastError();
                PLC_LOG_ERROR("TCP发送失败 conn=%llu code=%s grid=%s car=%s err=%d",
                    (unsigned long long)clientId, code.toLocal8Bit().data(),
                    b.toLocal8Bit().data(), carStr.toLocal8Bit().data(), winErr);
            }
            else
            {
                sentCount++;
            }
        }

        qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - t1;

        if (allSuccess)
        {
            m_tcpSendCount.fetch_add(1);
            tcpSuccess = true;
            PLC_LOG_INFO("TCP发送成功 code=%s grid=%s car=%s cmd=%s clients=%d/%d elapsed=%lldms",
                code.toLocal8Bit().data(), b.toLocal8Bit().data(),
                carStr.toLocal8Bit().data(), command.toLocal8Bit().data(),
                sentCount, clientCount, elapsed);
        }
        else
        {
            PLC_LOG_WARN("TCP发送部分失败 code=%s grid=%s car=%s sent=%d/%d err=%d elapsed=%lldms",
                code.toLocal8Bit().data(), b.toLocal8Bit().data(),
                carStr.toLocal8Bit().data(), sentCount, clientCount, errCount, elapsed);
        }
    }
    else
    {
        PLC_LOG_WARN("TCP发送失败: 无PLC客户端连接 code=%s grid=%s car=%d",
            code.toLocal8Bit().data(),
            vecGrid.empty() ? "0" : QString("%1").arg(vecGrid[0], 3, 10, QChar('0')).toLocal8Bit().data(),
            car);
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

    // ── 4. 判断（仅 TCP 通道）──
    bool overallSuccess = tcpSuccess;

    emit plcSendInfo(code, m_lastGrid + "|" + m_lastCar, overallSuccess);

    if (overallSuccess)
    {
        PLC_LOG_INFO("发送PLC指令成功 code=%s tcp=%d",
            code.toLocal8Bit().data(), tcpSuccess);
    }
    else
    {
        PLC_LOG_ERROR("发送PLC指令失败 code=%s tcp=%d",
            code.toLocal8Bit().data(), tcpSuccess);
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
// 识别码 = EPC编码，客户已确认EPC（商品编码）即EPC编码（2026-08-10）
// codeGridMap: 识别码→格口字符串（如 "15" 或 "1,2,3"）
// ============================================================================
bool PlcManager::sendBatchCodes(const QMap<QString, QString>& codeGridMap)
{
    if (codeGridMap.isEmpty())
    {
        PLC_LOG_WARN("sendBatchCodes: EPC编码映射为空，跳过");
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
                sendCodeInfo(code, vecGrid, DEFAULT_CAR_NUM);
            });
        }
        else
        {
            sendCodeInfo(code, vecGrid, DEFAULT_CAR_NUM);
        }

        successCount++;
    }

    PLC_LOG_INFO("sendBatchCodes: 批量发送完成 success=%d fail=%d total=%d",
        successCount, failCount, codeGridMap.size());

    return failCount == 0;
}

// ============================================================================
// sendBatchCodesWithEpcCache — 从回调获取 RFID 小车号的批量发送
// 通过 m_carNumCb(code) 查询每个识别码的小车号，默认 DEFAULT_CAR_NUM
// ============================================================================
bool PlcManager::sendBatchCodesWithEpcCache(const QMap<QString, QString>& codeGridMap)
{
    if (codeGridMap.isEmpty())
    {
        PLC_LOG_WARN("sendBatchCodesWithEpcCache: EPC编码映射为空，跳过");
        return false;
    }

    PLC_LOG_INFO("sendBatchCodesWithEpcCache: 开始批量发送（RFID小车号） total=%d", codeGridMap.size());

    int successCount = 0;
    int failCount = 0;
    int rfidCarCount = 0;  // 使用 RFID 小车号的数量

    for (auto it = codeGridMap.constBegin(); it != codeGridMap.constEnd(); ++it)
    {
        const QString& code = it.key();
        const QString& gridStr = it.value();

        // 解析格口
        std::vector<int> vecGrid;
        for (const QString& g : gridStr.split(',', Qt::SkipEmptyParts))
        {
            bool ok = false;
            int n = g.trimmed().toInt(&ok);
            if (ok && n > 0) vecGrid.push_back(n);
        }

        if (vecGrid.empty())
        {
            PLC_LOG_WARN("sendBatchCodesWithEpcCache: 格口解析失败 code=%s gridStr=%s",
                code.toLocal8Bit().data(), gridStr.toLocal8Bit().data());
            failCount++;
            continue;
        }

        // ★ 2026-09-09 选格（客户口径，两阶段）：
        //   ① WCS 禁用格口（满箱后旧箱已归档、等待 WMS 重发 H6 重绑）一律不参与分配；
        //      若映射内【全部】格口都处于该状态 → 不发指令（记日志/异常，等重绑或人工处理），
        //      避免把货投到已满箱、无可用容器的格口（含单格口映射场景）
        //   ② 在可用格口内按需求5选格：无锁格→首个匹配；部分锁格→首个未锁格；全部锁格→首个匹配
        //   ★ 2026-09-20 现场问题④（no_bind）新增前置条件：
        //      · 已解锁 且 未绑定容器 → **不允许下发到该格口**（改投异常口；不计已分拣、
        //        不消耗计划额度）—— 否则件落进无容器格口，无法计入任何容器的 H7，
        //        换箱后新容器"满箱回传成功、人工复核多出一件"（现场 034 格口案例）。
        //      · 锁格 → **完全按原有逻辑处理**（本次不新增判定）；
        //      · 满箱未重绑(禁用) → 原有逻辑（不下发）。
        //      判据集中在 isGridDispatchable()，开关 sortingRequireBoundGrid 可逐字回退。
        QString selReason;
        const int mapCount = (int)vecGrid.size();
        // ★ 2026-09-14 计划分配表：本次下发的认领信息（发送失败时按 EPC 释放）
        QString allocClaimEpc;     // 已认领的 EPC（= code）
        bool    allocClaimHeld = false;
        // ★ 2026-09-20 现场问题④：本件最终是否放弃下发
        //   （不可落格：已解锁且未绑定容器，且异常口当前也不可下发）
        bool    bSkipSend = false;
        {
            // ── 候选集 = 映射串 ∪ 分配表内该 SKU 的计划格口 ──
            //   ★ 为什么取并集：改造前"候选"来自映射串、而"计划"来自计划表，两套真相；
            //     当 H4 计划了某格口但映射串没有（或反之）时，会静默退回"取首个格口"老逻辑。
            //     并集后候选集由两者共同决定，且**结果只取决于计划**（顺序在编译期已固化）。
            std::vector<int> cand = vecGrid;
            std::vector<int> avail;

            // ── ① 只读预查计划（bClaim=false：**不扣额度**，仅用于判断是否需要缺口搬迁）──
            //   为什么先只读：若这一步就认领额度，搬迁后还要再认领一次，前一次会泄漏为
            //   "在途"直到超时才归还（白占额度 → 计划件反而落不进）。
            PlcPlanAllocInfo pinfo;
            bool bHasPlan = false;
            if (m_planAllocCb)
            {
                pinfo = m_planAllocCb(code, code, false);
                bHasPlan = pinfo.valid;
            }

            // 合并计划格口进候选集（去重）
            if (bHasPlan)
            {
                for (auto pit = pinfo.planQtyPerGrid.constBegin(); pit != pinfo.planQtyPerGrid.constEnd(); ++pit)
                {
                    bool okG = false;
                    const int g = pit.key().toInt(&okG);
                    if (!okG || g <= 0) continue;
                    bool dup = false;
                    for (int x : cand) if (x == g) { dup = true; break; }
                    if (!dup) cand.push_back(g);
                }
                // ★ 确定性：候选按格口号升序（消除 items 到达顺序对决策的影响 → 可重现）
                std::sort(cand.begin(), cand.end());
            }

            // ★ 2026-09-20 现场问题④：候选过滤加上"已解锁且已绑定容器"这一前置条件
            //   （判据集中在 isGridDispatchable：禁用→不发；锁格→按原有逻辑照发；
            //     已解锁且未绑定容器→本次新增，不发）
            // ★ 2026-09-21 硬上限（客户口径：**格口计划多少件就只分多少件**）：
            //   候选还必须"该 (SKU,格口,属性) 仍有剩余额度" —— 剩余 = H4计划 − 已落 − 在途。
            //   为什么放在候选过滤这一层：认领、多格口兜底、单格口兜底、锁格兜底**全部**从这里取候选，
            //   所以放在这里等于"所有下发路径都受计划上限约束"，包括分配表失效时的回退路径
            //   （此前 ① 分配表一失效就没上限、② 老闸门拿 SKU 计划总数当本格口上限，是超计划的另两个成因）。
            int nDisabled = 0;        // 满箱未重绑(禁用) —— 原有判据
            int nLocked = 0;          // ★ 2026-09-26：物理锁格（含锁格状态未知）—— 一律不进候选
            int nUnboundBlocked = 0;  // 已解锁且未绑定容器 —— 2026-09-20 判据
            int nQuotaFull = 0;       // 该 (SKU,格口,类型) 计划额度已用尽（含在途）—— 2026-09-21 判据
            int nLockedWait = 0;      // ★ 2026-09-26：已绑新箱但 PLC 仍锁格（H6 先到、等解锁边沿）—— 暂不收件
            QString firstFullDesc;    // 供日志：首个额度用尽单元的"格口(属性):计划/已落/在途"
            for (int g : cand)
            {
                if (isGridDisabled(g))
                {
                    ++nDisabled;
                    // ★ 2026-09-26 现场口径（B 方案）：H6 已绑新箱 + PLC 仍锁格 ⇒ 该格等解锁边沿才恢复收件。
                    //   单独计数，便于现场从日志看出"不是没有容器，而是还没解锁"（原日志只报"满箱未重绑(禁用)"）。
                    if (isGridLocked(g) && isGridBound(g)) ++nLockedWait;
                    continue;
                }
                // ★ 2026-09-26 现场口径（第 1/2 条）：**锁格不落件，无论什么情况** ——
                //   锁格（或程序刚启动、首次快照未就绪导致状态未知）的格口一律不进候选，
                //   不再有"全部锁格→取首个匹配 / 单格口映射照发"这类兜底放行。
                if (!isLockStateKnown() || isGridLocked(g)) { ++nLocked; continue; }
                if (!isGridBound(g))   { ++nUnboundBlocked; continue; }  // 已解锁且无容器 → 不发
                // ★ 硬上限：计划额度已用尽（或该单元在 H4 计划里没有数量）→ 不进候选
                if (!gridHasQuotaLeft(pinfo, g, &firstFullDesc)) { ++nQuotaFull; continue; }
                avail.push_back(g);
            }

            bool bPlanDecided = false;
            // ★ 是否已正式认领额度（bClaim=true 只允许发生一次；见上方"只读预查"说明）
            bool bClaimed = false;

            if (avail.empty())
            {
                // 本件为什么没有任何可下发格口？四类根因（可并存）：
                //   · 未绑定容器（2026-09-20 判据）   → 改投异常口，额度保留（等 H6 绑定即恢复）
                //   · 计划额度已用尽（2026-09-21 判据）→ 超计划件，改投异常口
                //   · 满箱锁格 / 锁格状态未知（★ 2026-09-26 判据）→ **不发指令**（等 PLC 解锁）
                //   · 满箱未重绑(禁用)                  → **不发指令**（等换箱重绑）
                //   ★ 异常口自身同样严格：异常口不可下发（锁格/未绑定容器/禁用）时一律不发指令 + 留痕。
                const int exc = excGridFromConfig();
                const bool bUnboundCause  = (nUnboundBlocked > 0);
                const bool bQuotaCause    = (nQuotaFull > 0);
                const bool bLockedCause   = (nLocked > 0);
                const bool bDisabledCause = (nDisabled > 0);
                if ((bUnboundCause || bQuotaCause) && canDivertToExc(exc))
                {
                    vecGrid      = { exc };
                    bPlanDecided = true;   // ★ 不进入计划认领 ⇒ 不消耗任何额度（额度留在原格口）
                    if (bUnboundCause)
                    {
                        selReason = QString::fromUtf8("候选格口全部不可下发(未绑定容器%1个/满箱锁格%2个/满箱未重绑%3个) → 改投异常口%4")
                                        .arg(nUnboundBlocked).arg(nLocked).arg(nDisabled).arg(exc);
                        notifyExcRoute(code, QString::fromUtf8("格口未绑定容器"));
                        PLC_LOG_WARN("选格-未绑定容器 code=%s 映射=[%s] 候选格口全部不可下发"
                                     "（已解锁且未绑定容器%d个 / 满箱锁格%d个 / 满箱未重绑(禁用)%d个）→ 改投异常口%d"
                                     "（本件不落这些格口、不计已分拣、不消耗计划额度）",
                            code.toLocal8Bit().data(), gridStr.toLocal8Bit().data(),
                            nUnboundBlocked, nLocked, nDisabled, exc);
                    }
                    else
                    {
                        // ★ 硬上限：本 SKU 各候选单元的 H4 计划额度都已用尽 → 典型的超计划件
                        selReason = QString::fromUtf8("超计划[%1]本格口计划已满(%2) → 发往异常口%3")
                                        .arg(gridStr).arg(firstFullDesc).arg(exc);
                        PLC_LOG_WARN("选格-超计划(本格口计划已满) code=%s 映射=[%s] %s "
                                     "→ 发往异常口%d（严格不大于计划件数：该单元计划已满，本件不计已分拣、不消耗额度）",
                            code.toLocal8Bit().data(), gridStr.toLocal8Bit().data(),
                            firstFullDesc.toLocal8Bit().data(), exc);
                    }
                }
                else
                {
                    // 不发指令：根因逐类列出（锁格/禁用属"等格口恢复"，额度保留在原单元）
                    QString causeDesc;
                    if (bUnboundCause)  causeDesc += QString::fromUtf8("未绑定容器%1个 ").arg(nUnboundBlocked);
                    if (bQuotaCause)    causeDesc += QString::fromUtf8("计划已满%1个 ").arg(nQuotaFull);
                    if (bLockedCause)   causeDesc += QString::fromUtf8("满箱锁格%1个 ").arg(nLocked);
                    if (bDisabledCause) causeDesc += QString::fromUtf8("满箱未重绑(禁用)%1个 ").arg(nDisabled);
                    if (nLockedWait > 0)
                        causeDesc += QString::fromUtf8("其中已绑新箱待解锁%1个 ").arg(nLockedWait);
                    if (causeDesc.isEmpty()) causeDesc = QString::fromUtf8("(无候选) ");
                    const QString excSuffix =
                        ((bUnboundCause || bQuotaCause) && !canDivertToExc(exc))
                            ? QString::fromUtf8("，异常口%1当前亦不可下发")
                                  .arg(exc > 0 ? QString::number(exc) : QString::fromUtf8("(未配置)"))
                            : QString();
                    selReason = QString::fromUtf8("无可下发格口[%1] %2→ 不发指令").arg(gridStr).arg(causeDesc);
                    PLC_LOG_WARN("sendBatchCodesWithEpcCache: 映射内无可下发格口（%s%s） code=%s grids=%s "
                                 "—— 不发指令（锁格等 PLC 解锁；未绑定容器等 WMS 重发 H6；满箱未重绑等换箱重绑；"
                                 "计划已满属超计划件，请人工处理）；各单元计划额度均保留",
                        causeDesc.toLocal8Bit().data(), excSuffix.toLocal8Bit().data(),
                        code.toLocal8Bit().data(), gridStr.toLocal8Bit().data());
                    failCount++;
                    continue;   // ★ 决策③：无可下发格口不发（单格口映射同样跳过）
                }
            }

            // ════════════════════════════════════════════════════════════════════
            // ★ 2026-09-14 同品多格口「按计划件数分配」（客户口径：以PLC反馈落格成功为准计数）
            //   例：H4 下发 SKU=106101134113101 → 22034(正常分拣) 计划 1 件
            //                                  + 22048(发货)     计划 3 件，
            //       则前 1 件去 34、后 3 件去 48，而不是全部取首个格口。
            //   额度口径 = 计划 − 已落 − **在途认领**：已落/在途都算占用 →
            //       同时两件在线 / 人工多投时，第 2 件在**下发时刻**就被拦住并改投异常口
            //       （这是修复"箱内实落 > 计划"的关键，改造前只查已落数会漏判）。
            //   满额后：配置了异常口(66) → 多余件发往异常口；未配置 → 退回旧「取首个」逻辑。
            // ════════════════════════════════════════════════════════════════════
            if (bHasPlan)
            {
                // 类型显示名（每格口类型：0=正常分拣, 1=异常, 2=发货）
                auto typeNameOf = [](const QString& t) -> QString {
                    if (t == "1") return QString::fromUtf8("异常");
                    if (t == "2") return QString::fromUtf8("发货");
                    return QString::fromUtf8("正常分拣");
                };
                auto keyOf = [](int g) { return QString("%1").arg(g, GRID_KEY_PADDING, 10, QChar('0')); };
                // 分配表摘要（日志用）：「格口(类型):计划N件/已落M/在途K」
                auto planDescOf = [&]() -> QString {
                    QStringList sl;
                    for (auto pit = pinfo.planQtyPerGrid.constBegin(); pit != pinfo.planQtyPerGrid.constEnd(); ++pit)
                        sl << QString("%1(%2):%3件/已落%4/在途%5").arg(pit.key())
                                  .arg(typeNameOf(pinfo.gridTypePerGrid.value(pit.key())))
                                  .arg(pit.value())
                                  .arg(pinfo.landedNum.value(pit.key(), 0))
                                  .arg(pinfo.reservNum.value(pit.key(), 0));
                    return sl.join(" ");
                };

                // ── ① 计划缺口搬迁 **已整体删除**（★ 2026-09-26 现场口径）──
                //   现场要求：**不允许因锁格或其它原因搬迁计划额度**；计划是"格口多少件就落多少件、不能多"。
                //   被删除的是"搬迁能力"本身（`PlanAllocTable::moveGap` / `HttpServer::moveAllocGap` /
                //   `PlcMoveGapCallback` 三处一并删除），而不是把它默认关掉 —— 避免以后有人再打开开关
                //   或重写实现把"计划 2 件被抬到 83 件"（现场 SKU 105301083212803）那类事故带回来。
                //   不可用单元的额度**留在原单元**，件改用其它单元自身剩余额度，都没有则改投异常口 66
                //   或不发指令；格口恢复可用后自动继续按计划分配。

                // 正式认领（bClaim=false 的只读预查不扣额度）
                if (bHasPlan && !bClaimed)
                {
                    pinfo = m_planAllocCb(code, code, true);
                    bHasPlan = pinfo.valid;
                    bClaimed = true;
                }

                if (bHasPlan && pinfo.claimOk && isGridDispatchable(pinfo.claimGrid))
                {
                    // ── ② 已认领到额度（回调已在锁内扣减）：直接下发该格口 ──
                    //  pinfo.claimPlanIdx < 0 表示"该件此前已落入过原格口（重投）→ 放行且未新增认领"
                    //  ★ 2026-09-20：条件里额外校验"认领回来的格口此刻仍可下发"（掩码与判据同源，
                    //    正常不会不符；此处为防御 —— 若不符则走 ③ 释放该额度并按根因改投异常口，
                    //    绝不把件导向不可下发格口）。
                    vecGrid = { pinfo.claimGrid };
                    bPlanDecided = true;
                    allocClaimEpc  = code;
                    allocClaimHeld = (pinfo.claimPlanIdx >= 0);   // 未新增认领则无需释放
                    const QString ck = keyOf(pinfo.claimGrid);
                    // ★ 2026-09-26：类型取**认领单元的类型**（同格口两类型时各自封顶，
                    //   不能再拿"格口首个类型"糊过去 —— 认领的是哪个单元就报哪个类型）
                    const QString cType = typeNameOf(QString::number((int)pinfo.claimType));
                    selReason = QString::fromUtf8("按计划分配[%1]→单元%2|%3(计划%4件,已落%5,在途%6)%7")
                                    .arg(gridStr).arg(pinfo.claimGrid).arg(cType)
                                    .arg(pinfo.planQtyPerGrid.value(ck))
                                    .arg(pinfo.landedNum.value(ck, 0))
                                    .arg(pinfo.reservNum.value(ck, 0) + (pinfo.claimPlanIdx >= 0 ? 1 : 0))
                                    .arg(pinfo.allEpcsLanded ? QString::fromUtf8("(重投回原格口,不新增额度)") : QString());
                    if (m_selectLogCb ? m_selectLogCb() : true)
                    {
                        PLC_LOG_INFO("选格-按计划分配 code=%s 映射=[%s] 选中格=%d(%s) 计划=%d件 已落=%d件 在途=%d件 分配表=%s",
                            code.toLocal8Bit().data(), gridStr.toLocal8Bit().data(),
                            pinfo.claimGrid, cType.toLocal8Bit().data(),
                            pinfo.planQtyPerGrid.value(ck), pinfo.landedNum.value(ck, 0),
                            pinfo.reservNum.value(ck, 0), planDescOf().toLocal8Bit().data());
                    }
                }
                else if (bHasPlan)
                {
                    // ── ③ 按计划认领失败：区分根因 ──
                    //   ★ 2026-09-20 现场问题④ + 多格口额度承接口径：
                    //     a) 该 SKU 多格口：**优先用"其它可用计划格口"的剩余额度承接**（② 分支已覆盖 ——
                    //        掩码跳过不可用格口 ⇒ 认领自动落到其它可用且有额度的计划格口）；
                    //     b) 其它格口都没有可用额度（额度只剩在"不可下发格口"上）→ 本分支：
                    //        按根因改投异常口；异常口自身不可下发时不下发指令。
                    //     根因必须可区分：超计划（人工多投/额度真用尽） vs 额度停在不可下发格口
                    //     （未绑定容器 / 满箱未重绑(禁用)）。
                    //  ★ 防御：若 ② 拿到了认领但该格口此刻已不可下发 → 先释放额度，避免白占
                    if (pinfo.claimOk)
                    {
                        if (m_sendResultCb) m_sendResultCb(code, pinfo.claimGrid, false);   // 入待释放队列（主线程归还额度）
                        PLC_LOG_WARN("选格-认领格口已不可下发 code=%s 映射=[%s] 认领格口=%d（掩码/判据不符）"
                                     "→ 释放本次额度并按根因重新决策",
                            code.toLocal8Bit().data(), gridStr.toLocal8Bit().data(), pinfo.claimGrid);
                        pinfo.claimOk = false;
                        pinfo.claimPlanIdx = -1;
                    }

                    QString blockedGridsDesc;    // 形如 "034(未绑定容器,余1件) 048(满箱未重绑,余2件)"
                    bool bAnyUnbound = false;    // 额度停在"已解锁且未绑定容器"的格口上
                    bool bAnyDisabled = false;   // 额度停在"满箱未重绑(禁用)"的格口上
                    {
                        for (auto pit = pinfo.planQtyPerGrid.constBegin(); pit != pinfo.planQtyPerGrid.constEnd(); ++pit)
                        {
                            bool okG = false;
                            const int g = pit.key().toInt(&okG);
                            if (!okG || g <= 0) continue;
                            const int remain = pit.value()
                                             - pinfo.landedNum.value(pit.key(), 0)
                                             - pinfo.reservNum.value(pit.key(), 0);
                            if (remain <= 0) continue;              // 该格口确已满额（不算"额度停住"）
                            if (isGridDispatchable(g)) continue;    // 该格口可下发（额度应已被认领；防御跳过）

                            const bool bDis = isGridDisabled(g);
                            const QString why = bDis ? QString::fromUtf8("满箱未重绑")
                                                     : QString::fromUtf8("未绑定容器");
                            if (bDis) bAnyDisabled = true; else bAnyUnbound = true;
                            if (!blockedGridsDesc.isEmpty()) blockedGridsDesc += " ";
                            blockedGridsDesc += QString("%1(%2,余%3件)").arg(pit.key()).arg(why).arg(remain);
                        }
                    }
                    const bool bBlockedCause = !blockedGridsDesc.isEmpty();
                    // ★ 两个用途分开取名（现场日志标签 vs 异常表类型/UI 文案）：
                    //   · blockedTag   —— PLC 日志的 `选格-<tag>` 标签（短、固定、便于 grep/脚本核对）
                    //   · blockedReason—— 异常表类型后缀 `改投异常口(<原因>)` 与 UI 告警文案
                    //   未绑定容器优先：它对应"等 H6 绑定即可恢复"，与"满箱未重绑(等换箱/重绑)"是两条不同的现场动作。
                    // ★ 2026-09-26：不可下发根因三分类（优先级：未绑定容器 > 满箱锁格 > 满箱未重绑）
                    //   —— 现场动作不同：等 H6 / 等 PLC 解锁 / 等换箱重绑。
                    int nLockedBlocked = 0;
                    {
                        for (auto pit = pinfo.planQtyPerGrid.constBegin(); pit != pinfo.planQtyPerGrid.constEnd(); ++pit)
                        {
                            bool okG = false;
                            const int g = pit.key().toInt(&okG);
                            if (!okG || g <= 0) continue;
                            if (pit.value() - pinfo.landedNum.value(pit.key(), 0)
                                            - pinfo.reservNum.value(pit.key(), 0) <= 0) continue;
                            if (isGridDispatchable(g)) continue;
                            if (!isGridDisabled(g) && (isGridLocked(g) || !isLockStateKnown())) ++nLockedBlocked;
                        }
                    }
                    const bool bAnyLocked   = (nLockedBlocked > 0);
                    const QString blockedTag    = bAnyUnbound ? QString::fromUtf8("未绑定容器")
                                                  : bAnyLocked ? QString::fromUtf8("满箱锁格")
                                                               : QString::fromUtf8("满箱未重绑");
                    const QString blockedReason = bAnyUnbound ? QString::fromUtf8("格口未绑定容器")
                                                  : bAnyLocked ? QString::fromUtf8("满箱锁格")
                                                               : QString::fromUtf8("满箱未重绑");

                    if (pinfo.excGrid > 0)
                    {
                        // ★ 2026-09-26 现场口径：**异常口也严格** —— 异常口自身不可下发
                        //   （锁格 / 未绑定容器 / 禁用）时，一律"不发指令 + 留痕"，
                        //   不再有"异常口被禁用/锁格仍按策略发往该口"这条放行。
                        if (!canDivertToExc(pinfo.excGrid))
                        {
                            bSkipSend    = true;
                            bPlanDecided = true;
                            PLC_LOG_WARN("选格-%s code=%s 映射=[%s] 不可下发单元=%s，"
                                         "且异常口%d当前不可下发（锁格/未绑定容器/禁用）→ 不下发指令"
                                         "（锁格等解锁、未绑定等 H6；额度保留在原单元）",
                                blockedTag.toLocal8Bit().data(),
                                code.toLocal8Bit().data(), gridStr.toLocal8Bit().data(),
                                blockedGridsDesc.toLocal8Bit().data(), pinfo.excGrid);
                        }
                        else
                        {
                            vecGrid      = { pinfo.excGrid };
                            bPlanDecided = true;
                            if (bBlockedCause)
                            {
                                // ★ 额度不是用尽，而是停在"不可下发单元"上 → 件改投异常口，额度原样保留
                                selReason = QString::fromUtf8("计划单元不可下发[%1]%2(额度保留)→发往异常口%3")
                                                .arg(gridStr).arg(blockedGridsDesc).arg(pinfo.excGrid);
                                notifyExcRoute(code, blockedReason);
                                PLC_LOG_WARN("选格-%s code=%s 映射=[%s] 分配表=%s 不可下发单元=%s "
                                             "→ 发往异常口%d（本件不落这些单元、不计已分拣、不消耗计划额度；"
                                             "额度保留在原单元，恢复可用后自动继续分配）",
                                    blockedTag.toLocal8Bit().data(),
                                    code.toLocal8Bit().data(), gridStr.toLocal8Bit().data(),
                                    planDescOf().toLocal8Bit().data(), blockedGridsDesc.toLocal8Bit().data(),
                                    pinfo.excGrid);
                            }
                            else
                            {
                                selReason = QString::fromUtf8("超计划[%1]计划单元已满(%2)→发往异常口%3")
                                                .arg(gridStr).arg(planDescOf()).arg(pinfo.excGrid);
                                PLC_LOG_WARN("选格-超计划 code=%s 映射=[%s] 分配表=%s → 发往异常口%d（不再占用计划单元）",
                                    code.toLocal8Bit().data(), gridStr.toLocal8Bit().data(),
                                    planDescOf().toLocal8Bit().data(), pinfo.excGrid);
                            }
                        }
                    }
                    else if (bBlockedCause || avail.empty())
                    {
                        // ★ 额度停在不可下发单元 + 未配置异常口 → 不下发
                        //   ★ 2026-09-26：avail 为空（例如全部候选锁格）时同样不发指令，
                        //     原实现会退回 `avail[0]`（空 vector 越界）—— 已修掉。
                        bSkipSend    = true;
                        bPlanDecided = true;
                        PLC_LOG_WARN("选格-%s code=%s 映射=[%s] 不可下发单元=%s，"
                                     "且未配置异常口(exceptionGrid)或候选为空 → 不下发指令（等待恢复可用或人工处理）",
                            blockedTag.toLocal8Bit().data(),
                            code.toLocal8Bit().data(), gridStr.toLocal8Bit().data(),
                            blockedGridsDesc.toLocal8Bit().data());
                    }
                    else
                    {
                        // ★ 2026-09-26：原实现此处"按旧逻辑取首个格口照发"（可能把件投到额度已满/
                        //   不在本 SKU 计划内的单元）—— 与"计划多少落多少、不能多"冲突，已删除，
                        //   改为不发指令 + 留痕（由调用方写"无可用格口"）。
                        bSkipSend    = true;
                        bPlanDecided = true;
                        PLC_LOG_WARN("选格-超计划/无法认领 code=%s 映射=[%s] 分配表=%s → 未配置异常口，"
                                     "按新口径不发指令（不再退回“取首个格口照发”；请人工确认多余件）",
                            code.toLocal8Bit().data(), gridStr.toLocal8Bit().data(),
                            planDescOf().toLocal8Bit().data());
                    }
                }
            }

            if (!bPlanDecided && avail.size() > 1)
            {
                // ★ 2026-09-26：avail 已在候选过滤阶段排除"锁格/禁用/未绑定容器/额度用尽"
                //   ⇒ 这里不再需要"全部锁格→取首个匹配"的兜底（该兜底会向锁格格口下发，已删除）。
                //   保留 isGridLocked 防御断言：万一有路径漏过滤，宁可不发也不投向锁格格口。
                std::vector<int> pickable;
                for (int g : avail)
                {
                    if (!isGridLocked(g) && isGridBound(g))
                        pickable.push_back(g);
                }
                if (pickable.empty())
                {
                    bSkipSend    = true;
                    bPlanDecided = true;
                    selReason = QString::fromUtf8("多格口[%1]可用格口均不可下发(锁格/未绑定容器)→不发指令").arg(gridStr);
                    PLC_LOG_WARN("sendBatchCodesWithEpcCache: 多格口映射可用格口均不可下发(锁格/未绑定容器) "
                                 "code=%s grids=%s —— 不发指令（额度保留在原单元）",
                        code.toLocal8Bit().data(), gridStr.toLocal8Bit().data());
                }
                else
                {
                    if (pickable.size() < avail.size())
                        selReason = QString::fromUtf8("多格口[%1]部分格口不可下发(锁格/未绑定容器)→可用中取首个%2")
                                        .arg(gridStr).arg(pickable[0]);
                    else if ((int)avail.size() < mapCount)
                        selReason = QString::fromUtf8("多格口[%1]部分格口不可下发(禁用/锁格)→可用中取首个%2")
                                        .arg(gridStr).arg(pickable[0]);
                    else if (!bHasPlan)
                        selReason = QString::fromUtf8("无计划信息[%1]→按原逻辑取首个%2").arg(gridStr).arg(pickable[0]);
                    else
                        selReason = QString::fromUtf8("多格口[%1]取首个匹配%2").arg(gridStr).arg(pickable[0]);
                    vecGrid = { pickable[0] };
                }
            }
            else if (!bPlanDecided)
            {
                // ★ 2026-09-26：单格口映射同样受"锁格不落件 / 未绑定容器不落件"约束 ——
                //   原"单格口映射[NNN]物理锁格中→按需求照发"分支已删除（该分支正是把件投进锁格格口的路径）。
                if (avail.empty() || isGridLocked(avail[0]) || !isGridBound(avail[0]))
                {
                    bSkipSend    = true;
                    bPlanDecided = true;
                    selReason = QString::fromUtf8("单格口映射[%1]不可下发(锁格/未绑定容器)→不发指令").arg(gridStr);
                    PLC_LOG_WARN("sendBatchCodesWithEpcCache: 单格口映射不可下发(锁格/未绑定容器) code=%s grids=%s "
                                 "—— 不发指令（额度保留在原单元；锁格等解锁、未绑定等 H6）",
                        code.toLocal8Bit().data(), gridStr.toLocal8Bit().data());
                }
                else
                {
                    selReason = QString::fromUtf8("单格口映射[%1]").arg(gridStr);
                    vecGrid = { avail[0] };
                }
            }
        }

        // ★ 2026-09-26：本件被判定为"不落件"（锁格 / 未绑定容器 / 无非锁格候选，
        //   且异常口当前也不可下发）→ 不下发任何指令（额度未认领、无任何副作用），
        //   由调用方按既有"无可用格口"链路留痕（异常表 + UI 告警）。
        if (bSkipSend)
        {
            failCount++;
            PLC_LOG_WARN("sendBatchCodesWithEpcCache: 不可落格（锁格/未绑定容器，或异常口亦不可下发）"
                         " code=%s grids=%s 原因=%s —— 不发指令",
                code.toLocal8Bit().data(), gridStr.toLocal8Bit().data(),
                selReason.toLocal8Bit().data());
            continue;
        }

        // ★ 2026-09-09 选格结果日志：现场核对"这件货为什么去这个格口"（映射/锁格/禁用/最终选中）
        PLC_LOG_INFO("选格 code=%s 映射=[%s] 选中格=%d 原因=%s",
            code.toLocal8Bit().data(), gridStr.toLocal8Bit().data(),
            vecGrid.empty() ? -1 : vecGrid[0], selReason.toLocal8Bit().data());

        // ★ 从回调获取 RFID 小车号，默认 DEFAULT_CAR_NUM
        int car = DEFAULT_CAR_NUM;
        if (m_carNumCb)
        {
            QString carStr = m_carNumCb(code);
            if (!carStr.isEmpty() && carStr != CAR_NUM_STR(DEFAULT_CAR_NUM))
            {
                bool ok = false;
                int n = carStr.toInt(&ok);
                if (ok && n > 0)
                {
                    car = n;
                    rfidCarCount++;
                }
            }
        }

        if (m_pSendPool)
        {
            m_pSendPool->commitNoWait([this, code, vecGrid, car]() {
                const bool ok = sendCodeInfo(code, vecGrid, car);
                // ★ 2026-09-20 现场问题④：回调带出"最终下发格口"（HttpServer 记录本件本要发往哪个格口）
                if (m_sendResultCb) m_sendResultCb(code, vecGrid.empty() ? 0 : vecGrid[0], ok);
            });
        }
        else
        {
            const bool ok = sendCodeInfo(code, vecGrid, car);
            if (m_sendResultCb) m_sendResultCb(code, vecGrid.empty() ? 0 : vecGrid[0], ok);
        }

        successCount++;
        PLC_LOG_INFO("TCP批量发送 EPC[%d/%d] code=%s grid=%s car=%d",
            successCount, codeGridMap.size(), code.toLocal8Bit().data(),
            gridStr.toLocal8Bit().data(), car);
    }

    PLC_LOG_INFO("sendBatchCodesWithEpcCache: 发送完成 success=%d fail=%d total=%d rfidCar=%d",
        successCount, failCount, codeGridMap.size(), rfidCarCount);

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

    // ★ 保存目标 IP（首次失败后由心跳线程用此 IP 自动重连）
    if (ip)
        strncpy_s(m_plcConfig.szS7Ip, sizeof(m_plcConfig.szS7Ip), ip, 23);

    m_S7Plc = new CSiemensPLC();
    if (!m_S7Plc->connectTo(ip))
    {
        // ★ 2026-09-04 生产增强：首次连接失败（如 PLC 尚未开机/网络未就绪）不销毁对象，
        //   启动心跳线程每 2s 自动重连；重连成功后由心跳线程启动锁格轮询
        QString err = m_S7Plc->lastErrorText();
        PLC_LOG_WARN("S7首次连接失败 ip=%s err=%s（心跳线程将自动重连）", ip, err.toLocal8Bit().data());
        emit s7Error(QString("S7连接失败: %1（将自动重连）").arg(err));

        if (!m_bHeartThreadStart)
        {
            m_bHeartThreadStart = true;
            m_heartThread = std::thread(&PlcManager::OnS7HeartThread, this);
            PLC_LOG_INFO("S7心跳线程已启动（等待重连） ip=%s", m_plcConfig.szS7Ip);
        }
        return false;
    }

    PLC_LOG_INFO("S7连接成功 ip=%s", ip);

    // 启动 S7 心跳线程（与 WCSApp simensS7::OnHeartThread 一致）
    if (!m_bHeartThreadStart)
    {
        m_bHeartThreadStart = true;
        m_heartThread = std::thread(&PlcManager::OnS7HeartThread, this);
    }

    // ★ 锁格轮询由 s7Connected 信号统一在主线程启动（见构造器 self-connect，兼容心跳重连成功路径）
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

    // 全量更新 m_s7Grid_200 状态缓存（与 WCSApp 一致）
    // WCSApp 在边沿检测后，将 DB77 的 200 位完整快照写入 m_s7Grid_200
    // 确保 m_s7Grid_200 始终反映当前 PLC 锁格状态，而非仅依赖边沿事件
    {
        std::unique_lock<std::mutex> lock(m_lockGridPlc);
        for (int i = 0; i < PLC_S7_LOCK_READ_SIZE; i++)
        {
            for (int a = 0; a < 8; a++)
            {
                int grid_status = i * 8 + (a);
                if (grid_status >= PLC_S7_MAX_GRID_COUNT)
                    break;
                bool bCurr = S7_GetBitAt(m_s7PlcLastData, i, a);
                m_s7Grid_200[grid_status] = bCurr;
            }
        }
    }

    // ★ 2026-09-26：首次成功读回快照 ⇒ 锁格状态"已知"（此后不再清零；S7 掉线沿用最后快照值）
    if (!m_lockStateKnown.load())
    {
        m_lockStateKnown.store(true);
        PLC_LOG_INFO("[锁格] 首次锁格快照已就绪 → 锁格状态视为已知（此前按“锁格未知=不可下发”保守处理）totalLocked=%d",
            lockedGridCount());
    }

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
            // ★ 2026-09-04：连续失败日志降频（每约 20 次 ≈ 40~60s 一条），避免 PLC 长时间离线刷屏
            static int s_heartFailLog = 0;
            if (++s_heartFailLog == 1)
                PLC_LOG_WARN("S7心跳检测到断线，开始自动重连 ip=%s（此后每~40s汇报一次）", m_plcConfig.szS7Ip);
            Sleep(100);
            m_S7Plc->connectTo(m_plcConfig.szS7Ip);
            if (m_S7Plc->isConnected())
            {
                s_heartFailLog = 0;
                PLC_LOG_INFO("S7心跳重连成功 ip=%s", m_plcConfig.szS7Ip);
                emit s7Connected(QString::fromLocal8Bit(m_plcConfig.szS7Ip));
            }
            else
            {
                if (s_heartFailLog % 20 == 1)
                    PLC_LOG_ERROR("S7心跳重连失败（已连续%3d次未成功） ip=%s",
                                  s_heartFailLog, m_plcConfig.szS7Ip);
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

// ★ 2026-09-26 现场口径：锁格状态是否已知（S7 首次轮询快照是否就绪）。
//   · 未知期（程序刚启动、还没读到第一帧 DB77）⇒ 判据侧按"锁格"处理（不可下发）；
//   · 首次轮询写回快照后即永远为 true；
//   · **S7 掉线不重置**：掉线期间沿用最后已知快照值继续判断（既有行为，不引入"掉线即停线"），
//     重连后 ≤1s 由轮询刷新；"H6 已到但解锁边沿丢失"的格口由 HttpServer 的 10s 兜底对账恢复。
bool PlcManager::isLockStateKnown() const
{
    return m_lockStateKnown.load();
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
// 格口禁用管理（满箱锁格后禁用，WMS重新绑定H6时恢复）
// ============================================================================

void PlcManager::disableGrid(int grid)
{
    if (grid < 0 || grid >= PLC_S7_MAX_GRID_COUNT)
        return;
    std::lock_guard<std::mutex> lock(m_lockDisabledGrids);
    m_disabledGrids.insert(grid);
    PLC_LOG_INFO("格口已禁用 grid=%d (满箱锁格后禁止分配/落格)", grid);
}

void PlcManager::enableGrid(int grid)
{
    std::lock_guard<std::mutex> lock(m_lockDisabledGrids);
    if (m_disabledGrids.remove(grid))
        PLC_LOG_INFO("格口已恢复 grid=%d (WMS重新绑定H6)", grid);
}

bool PlcManager::isGridDisabled(int grid) const
{
    std::lock_guard<std::mutex> lock(m_lockDisabledGrids);
    return m_disabledGrids.contains(grid);
}

void PlcManager::enableAllGrids()
{
    std::lock_guard<std::mutex> lock(m_lockDisabledGrids);
    int count = m_disabledGrids.size();
    m_disabledGrids.clear();
    PLC_LOG_INFO("全部格口已恢复 count=%d (新波次开始)", count);
}

// ============================================================================
// ★ 2026-09-20 现场问题④（no_bind） + ★ 2026-09-26 现场口径（锁格一律不落件）
//
//   下发前置条件（**唯一判据**，全代码同源）：
//     **未禁用 且 未锁格（含锁格状态未知） 且 已绑定容器**
//
//   · 已禁用(满箱未重绑)      → 不下发（原有）
//   · 锁格 / 锁格状态未知      → **不下发**（★ 2026-09-26：锁格不落件，无论什么情况；
//                                含"程序启动时锁格位已经是 1"与"首次轮询前状态未知"两种情形）
//   · 已解锁 且 未绑定容器      → 不下发（2026-09-20 起；开关 sortingRequireBoundGrid 已废弃，恒开）
//   · 已解锁 且 已绑定容器      → 允许下发（再叠加既有条件：执行态/有映射/有额度/PLC已连接）
//   异常口自身同样受本判据约束（canDivertToExc 复用之）⇒ 不可下发时不发指令 + 留痕。
//
//   ★ 计划额度不因上述任一原因搬迁：不可用格口的额度留在原 (SKU,格口,类型) 单元，
//     件优先用其它"可下发且自身仍有剩余额度"的单元承接，都没有则改投异常口或不下发。
// ============================================================================

bool PlcManager::isGridBound(int grid) const
{
    if (!m_gridBoundCb) return true;   // 未注册回调 → 视为已绑定（保持既有行为）
    return m_gridBoundCb(grid);
}

bool PlcManager::isGridDispatchable(int grid) const
{
    if (isGridDisabled(grid)) return false;                        // 原有：满箱未重绑(禁用)不下发
    if (!isLockStateKnown() || isGridLocked(grid)) return false;    // ★ 锁格（含状态未知）一律不下发
    return isGridBound(grid);                                      // ★ 恒要求已绑容器（开关已废弃）
}

int PlcManager::excGridFromConfig() const
{
    const QString excCfg = ConfigManager::instance()->config().exceptionGrid.trimmed();
    bool ok = false;
    const int exc = excCfg.toInt(&ok);
    return (ok && exc > 0 && exc <= BINDING_SLOT_COUNT) ? exc : -1;
}

bool PlcManager::canDivertToExc(int exc) const
{
    return exc > 0 && isGridDispatchable(exc);
}

void PlcManager::notifyExcRoute(const QString& epc, const QString& reason) const
{
    if (m_excRouteCb) m_excRouteCb(epc, reason);
}

// ============================================================================
// ★ 2026-09-21 硬上限：该 (SKU,格口,分拣属性) 是否仍有剩余额度
//
//   客户口径：**格口计划多少件就只分多少件，严格不大于计划件数**；
//   额度以 (SKU,格口,属性) 为单位，分类与发货各自独立、互不借用。
//
//   判据：`H4计划 − 已落 − 在途 > 0`（planQtyPerGrid/landedNum/reservNum 由 HttpServer 提供；
//   分配表失效时 HttpServer 用 m_boxLandedEpcs + 按格口在途 组合同样的口径）。
//   · 该格口在 H4 计划里没有数量（plan <= 0）→ 视为 0 额度，不可投（进异常口），
//     不再"照映射串首个格口投"（那正是"某格口超计划那么多"的结构性来源之一）。
//   · descOut：首个被判满格口的可读描述（日志用，形如 `034(发货):计划2件/已落2/在途0`）。
bool PlcManager::gridHasQuotaLeft(const PlcPlanAllocInfo& pinfo, int grid, QString* descOut) const
{
    const QString gk = QString("%1").arg(grid, GRID_KEY_PADDING, 10, QChar('0'));
    const int plan   = pinfo.planQtyPerGrid.value(gk, 0);
    const int landed = pinfo.landedNum.value(gk, 0);
    const int reserv = pinfo.reservNum.value(gk, 0);
    const bool ok = (plan > 0 && landed + reserv < plan);
    if (!ok && descOut && descOut->isEmpty())
    {
        *descOut = QString::fromUtf8("%1(%2):计划%3件/已落%4/在途%5")
                       .arg(gk)
                       .arg(gridTypeName(pinfo.gridTypePerGrid.value(gk)))
                       .arg(plan).arg(landed).arg(reserv);
    }
    return ok;
}

QString PlcManager::gridTypeName(const QString& t)
{
    if (t == "1") return QString::fromUtf8("异常");
    if (t == "2") return QString::fromUtf8("发货");
    return QString::fromUtf8("分类");
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
    QString rawText = QString::fromLocal8Bit(rawData);
    PLC_LOG_INFO("TCP接收 rawData=%s text=%s len=%d conn=%llu",
        rawData.toHex(' ').constData(), rawText.toLocal8Bit().data(),
        iLength, (unsigned long long)dwConnID);
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
        // TCP 锁格/解锁消息: {grid|L} 或 {grid|U} — 已移除，与WCSApp保持一致
        // WCSApp退货窄带场景仅使用 S7 DB77 轮询检测锁格，不处理TCP锁格消息
        // ═══════════════════════════════════════════════════════════════
        // if (partCount == 2)
        // {
        //     QString field0 = parts[0].trimmed();
        //     QString field1 = parts[1].trimmed().toUpper();
        //
        //     if (field1 == "L" || field1 == "U")
        //     {
        //         bool isLock = (field1 == "L");
        //         bool ok = false;
        //         int gridNum = field0.toInt(&ok);
        //
        //         if (ok && gridNum >= 0 && gridNum < PLC_S7_MAX_GRID_COUNT)
        //         {
        //             if (isLock)
        //             {
        //                 PLC_LOG_WARN("TCP锁格消息: grid=%d lock", gridNum);
        //                 {
        //                     std::unique_lock<std::mutex> lock(m_lockGridPlc);
        //                     m_s7Grid_200[gridNum] = true;
        //                 }
        //                 QString s_grid = QString("%1").arg(gridNum, 3, 10, QChar('0'));
        //                 LockGridInfo info;
        //                 info.gridNum = s_grid;
        //                 info.type = 0;
        //                 {
        //                     std::unique_lock<std::mutex> lock(m_lockGrid);
        //                     m_queueLockInfo.push(info);
        //                 }
        //                 emit gridLockedByPlc(s_grid);
        //                 emit gridLocked(s_grid);
        //             }
        //             else
        //             {
        //                 PLC_LOG_WARN("TCP解锁消息: grid=%d unlock", gridNum);
        //                 {
        //                     std::unique_lock<std::mutex> lock(m_lockGridPlc);
        //                     m_s7Grid_200[gridNum] = false;
        //                 }
        //                 QString s_grid = QString("%1").arg(gridNum, 3, 10, QChar('0'));
        //                 LockGridInfo info;
        //                 info.gridNum = s_grid;
        //                 info.type = 1;
        //                 {
        //                     std::unique_lock<std::mutex> lock(m_lockGrid);
        //                     m_queueLockInfo.push(info);
        //                 }
        //                 emit gridUnlockedByPlc(s_grid);
        //                 emit gridUnlocked(s_grid);
        //             }
        //             continue;
        //         }
        //         else
        //         {
        //             PLC_LOG_WARN("TCP锁格消息格式异常: grid=%s type=%s", field0.toLocal8Bit().data(), field1.toLocal8Bit().data());
        //             continue;
        //         }
        //     }
        // }

        // ═══════════════════════════════════════════════════════════════
        // 反馈报文解析（精确匹配字段数，避免格式覆盖）
        //   5字段: {epc|grid|firstCar|lastCar|status}  — 含首车/尾车/分拣状态
        //   3字段: {barcode|grid|car}                  — PLC反馈落格确认
        // ═══════════════════════════════════════════════════════════
        if (parts.size() == 5)
        {
            // ★ 5字段格式: {epc|grid|firstCar|lastCar|status}
            QString code     = parts[0].trimmed();
            QString grid     = parts[1].trimmed();
            QString firstCar = parts[2].trimmed();
            QString lastCar  = parts[3].trimmed();
            QString car      = firstCar;  // car 取首车，兼容旧代码
            int     status   = parts[4].trimmed().toInt();

            // 更新最近接收数据
            {
                std::lock_guard<std::mutex> lock(m_lastDataMutex);
                m_lastRecvCode     = code;
                m_lastRecvGrid     = grid;
                m_lastRecvCar      = car;
                m_lastRecvFirstCar = firstCar;
                m_lastRecvLastCar  = lastCar;
                m_lastRecvStatus   = status;
            }

            // ★ 2026-09-07：日志带原始报文 raw={...}，便于核对 PLC 发来的 5 字段原始数据
            PLC_LOG_INFO("PLC反馈(5字段) raw={%s} code=%s grid=%s firstCar=%s lastCar=%s status=%d",
                content.toLocal8Bit().data(),
                code.toLocal8Bit().data(), grid.toLocal8Bit().data(),
                firstCar.toLocal8Bit().data(), lastCar.toLocal8Bit().data(), status);

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
                    entry.code        = code;
                    entry.grid        = grid;
                    entry.car         = car;
                    entry.firstCar    = firstCar;
                    entry.lastCar     = lastCar;
                    entry.status      = status;
                    entry.timestampMs = QDateTime::currentMSecsSinceEpoch();
                    m_feedbackBatchBuffer.append(entry);
                }
            }
        }
        else if (parts.size() == 3)
        {
            // ★ 3字段格式: {barcode|grid|car}（现有逻辑不变）
            QString code = parts[0].trimmed();
            QString grid = parts[1].trimmed();
            QString car  = parts[2].trimmed();

            PLC_LOG_INFO("PLC反馈(3字段) raw={%s} code=%s grid=%s car=%s",
                content.toLocal8Bit().data(),
                code.toLocal8Bit().data(), grid.toLocal8Bit().data(), car.toLocal8Bit().data());

            // 更新最近接收数据
            {
                std::lock_guard<std::mutex> lock(m_lastDataMutex);
                m_lastRecvCode     = code;
                m_lastRecvGrid     = grid;
                m_lastRecvCar      = car;
                m_lastRecvFirstCar = car;   // 3字段时首车=car
                m_lastRecvLastCar  = "";    // 3字段时无尾车
                m_lastRecvStatus   = 0;     // 3字段时无状态
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
                    entry.code        = code;
                    entry.grid        = grid;
                    entry.car         = car;
                    entry.firstCar    = car;   // 3字段时首车=car
                    entry.lastCar     = "";    // 3字段时无尾车
                    entry.status      = 0;     // 3字段时无状态
                    entry.timestampMs = QDateTime::currentMSecsSinceEpoch();
                    m_feedbackBatchBuffer.append(entry);
                }
            }
        }
        else
        {
            PLC_LOG_WARN("PLC反馈格式异常: {%s} fieldCount=%d rawData=%s",
                content.toLocal8Bit().data(), partCount, qdata.toLocal8Bit().data());
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