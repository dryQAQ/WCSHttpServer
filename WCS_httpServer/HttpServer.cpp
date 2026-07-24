#include "HttpServer.h"
#include "ParseWorker.h"
#include "log_center.h"
#include "hlog1.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QUrlQuery>
#include <QDateTime>
#include <QElapsedTimer>
#include <cstring>
#include <windows.h>
#include <tchar.h>

// ──── HTTP 服务专用日志宏（写入 ./log/HTTP/http.log）────
#ifndef HTTP_INFO
#define HTTP_INFO(fmt, ...)  hlog_format(HLOG_LEVEL_INFO,  "HTTP", "\t" fmt, ##__VA_ARGS__)
#define HTTP_WARN(fmt, ...)  hlog_format(HLOG_LEVEL_WARN,  "HTTP", "\t" fmt, ##__VA_ARGS__)
#define HTTP_ERROR(fmt, ...) hlog_format(HLOG_LEVEL_ERROR, "HTTP", "\t" fmt, ##__VA_ARGS__)
#endif

// ============================================================================
// HP-Socket CHttpServerListener 模式
//   m_pServer(this)  // this = IHttpServerListener*
//   m_pServer->Start(_T("0.0.0.0"), port);
//   m_pServer->Stop();
// ============================================================================

#include "define.h"

#define MAX_TASK_QUEUE      TASK_QUEUE_MAX_SIZE

HttpServer::HttpServer(QObject* parent)
    : QObject(parent)
    , m_pServer(this)           // ★ this = IHttpServerListener*
{
    m_pQueue   = new TaskQueue(MAX_TASK_QUEUE);
    m_pBuffer  = new GridBuffer();
    m_pWaveMgr = new WaveManager(m_pBuffer, this);
    m_pWorker  = new ParseWorker(m_pQueue, m_pBuffer, this);
    m_pPlcMgr  = new PlcManager(this);  // ★ PLC直连管理器

    // ──── 业务线程池 ────
    // BUSINESS_POOL_SIZE=90：预估16台扫描仪×5并发查询 + HTTP回传 + 异常处理 + 余量
    // 参考WCSApp架构中的 ThreadPool 模式，将业务逻辑 offload 到线程池避免阻塞 I/O 线程
    {
        m_pBusinessPool = new Hanchine::ThreadPool(BUSINESS_POOL_SIZE);
        HTTP_INFO("业务线程池已创建 threads=%d", BUSINESS_POOL_SIZE);
    }

    // 显式指定 Qt::QueuedConnection：ParseWorker::run() 在独立线程中运行，
    // 使用 AutoConnection 时因 sender/receiver 的 thread() 都在主线程，
    // 但 emit 发生在工作线程，导致信号跨线程传递异常。
    connect(m_pWorker, &ParseWorker::waveParsed, this,
        [this](const QString& orderCode, int skuCount, int orderQty, qint64, const QSet<QString>& recvSet) {
            m_pWaveMgr->setWaveData(orderCode, orderQty, skuCount);
            m_pWaveMgr->setRecvSet(recvSet);
            // ★ 新波次到来，重置PLC发送失败警告集合
            {
                std::lock_guard<std::mutex> lock(m_warnMutex);
                m_warnedPlcFailCodes.clear();
            }
        }, Qt::QueuedConnection);

    connect(m_pWaveMgr, &WaveManager::waveReadyToReport, this, &HttpServer::waveReadyToReport);

    // ──── PLC反馈 → 分拣标记 ────
    // PLC确认落格后自动标记为已分拣
    connect(m_pPlcMgr, &PlcManager::plcFeedbackReceived, this,
        [this](const QString& code, const QString& grid, const QString& car) {
            Q_UNUSED(grid);
            Q_UNUSED(car);
            if (!m_pWaveMgr) return;
            m_pWaveMgr->markSorted(code);
            HTTP_LOG_INFO("PLC反馈自动分拣 code=%s", code.toLocal8Bit().data());
            emit logMessage(QString("[PLC] 反馈落格分拣 code=%1 grid=%2 car=%3").arg(code).arg(grid).arg(car));
        }, Qt::QueuedConnection);

    // PLC连接状态日志
    connect(m_pPlcMgr, &PlcManager::plcConnected, this,
        [this](const QString& ip, int port) {
            HTTP_LOG_INFO("PLC已连接 ip=%s port=%d", ip.toLocal8Bit().data(), port);
            emit logMessage(QString("[PLC] 已连接 %1:%2").arg(ip).arg(port));
        });
    connect(m_pPlcMgr, &PlcManager::plcDisconnected, this,
        [this](const QString& ip, int port) {
            HTTP_LOG_WARN("PLC断开连接 ip=%s port=%d", ip.toLocal8Bit().data(), port);
            emit logMessage(QString("[PLC] 断开连接 %1:%2").arg(ip).arg(port), true);
        });

    // PLC批次信号
    connect(m_pPlcMgr, &PlcManager::plcBatchStart, this,
        [this]() {
            HTTP_LOG_INFO("PLC批次开始");
            emit logMessage("[PLC] 批次开始信号");
        });
    connect(m_pPlcMgr, &PlcManager::plcBatchStop, this,
        [this]() {
            HTTP_LOG_INFO("PLC批次停止");
            emit logMessage("[PLC] 批次停止信号");
        });

    // 健康检查定时器：每10秒输出连接统计
    m_healthTimer = new QTimer(this);
    connect(m_healthTimer, &QTimer::timeout, this, &HttpServer::logHealthStatus);
    m_healthTimer->start(HEALTH_CHECK_INTERVAL_MS);

    LogCenter::Instance()->wcs_run_log_warn(true, "[Http] HttpServer已创建");
}

HttpServer::~HttpServer()
{
    stop();
    m_pWorker->stop();
    m_pWorker->wait(WORKER_WAIT_MS);
    if (m_pBusinessPool) {
        delete m_pBusinessPool;
        m_pBusinessPool = nullptr;
    }
}

bool HttpServer::start(int port)
{
    m_pWorker->start();

    // ──── HP-Socket 性能调优（参考WCSApp线程池架构）────
    {
        // 设置I/O工作线程数（默认2×CPU核心=32，减到8减少上下文切换）
        m_pServer->SetWorkerThreadCount(HP_WORKER_THREADS);
        // 设置最大连接数，防止连接池耗尽
        m_pServer->SetMaxConnectionCount(HP_MAX_CONNECTIONS);
        // 设置KeepAlive探活时间，检测死连接
        m_pServer->SetKeepAliveTime(HP_KEEPALIVE_TIME_MS);

        HTTP_INFO("HP-Socket配置: workerThreads=%d maxConn=%d keepAlive=%dms",
            HP_WORKER_THREADS, HP_MAX_CONNECTIONS, HP_KEEPALIVE_TIME_MS);
    }

    // HP-Socket 模式 + Demo验证: Start(LPCTSTR, port)
    if (!m_pServer->Start(_T("0.0.0.0"), port))
    {
        LogCenter::Instance()->wcs_run_log_warn(false,
            QString("[Http] 启动失败 port=%1 err=%2").arg(port).arg((int)::GetLastError()));
        return false;
    }

    LogCenter::Instance()->wcs_run_log_warn(true,
        QString("[Http] 服务已启动 port=%1 businessPool=%2 hpWorker=%3")
            .arg(port).arg(BUSINESS_POOL_SIZE).arg(HP_WORKER_THREADS));

    // ★ 同时启动PLC监听服务
    if (m_pPlcMgr)
    {
        if (m_pPlcMgr->start("0.0.0.0", PLC_LISTEN_PORT))
        {
            HTTP_LOG_INFO("PLC监听服务已启动 port=%d", PLC_LISTEN_PORT);
            emit logMessage(QString("[PLC] 监听服务已启动 port=%1").arg(PLC_LISTEN_PORT));

            // ★ 启动 S7 直连（与 WCSApp 一致）
            if (m_pPlcMgr->connectS7(PLC_S7_IP))
            {
                HTTP_LOG_INFO("PLC S7连接成功 ip=%s", PLC_S7_IP);
                emit logMessage(QString("[PLC] S7连接成功 %1").arg(PLC_S7_IP));
            }
            else
            {
                HTTP_LOG_WARN("PLC S7连接失败 ip=%s", PLC_S7_IP);
                emit logMessage(QString("[PLC] S7连接失败 %1，将使用TCP文本协议").arg(PLC_S7_IP), true);
            }
        }
        else
        {
            HTTP_LOG_ERROR("PLC监听服务启动失败 port=%d", PLC_LISTEN_PORT);
            emit logMessage(QString("[PLC] 监听服务启动失败 port=%1").arg(PLC_LISTEN_PORT), true);
        }
    }

    emit serverStarted(port);
    return true;
}

void HttpServer::stop()
{
    if (m_pPlcMgr) m_pPlcMgr->stop();  // ★ 先停PLC

    if (m_pServer && m_pServer->HasStarted())
    {
        m_pServer->Stop();
    }
    m_pServer.Reset();
    emit serverStopped();
}

// ============================================================================
// CHttpServerListener 回调
// ============================================================================

EnHttpParseResult HttpServer::OnRequestLine(IHttpServer* pSender, CONNID dwConnID,
                                             LPCSTR lpszMethod, LPCSTR lpszUrl)
{
    std::lock_guard<std::mutex> lock(m_connMutex);
    ConnState& st = m_connStates[dwConnID];
    st.method = QString::fromUtf8(lpszMethod);
    QString full = QString::fromUtf8(lpszUrl);
    int q = full.indexOf('?');
    st.path = (q >= 0) ? full.left(q) : full;
    st.queryString = (q >= 0) ? full.mid(q + 1) : QString();
    return HPR_OK;
}

EnHttpParseResult HttpServer::OnBody(IHttpServer* pSender, CONNID dwConnID,
                                      const BYTE* pData, int iLength)
{
    std::lock_guard<std::mutex> lock(m_connMutex);
    m_connStates[dwConnID].body.append((const char*)pData, iLength);
    return HPR_OK;
}

EnHttpParseResult HttpServer::OnMessageComplete(IHttpServer* pSender, CONNID dwConnID)
{
    ConnState state;
    {
        std::lock_guard<std::mutex> lock(m_connMutex);
        auto it = m_connStates.find(dwConnID);
        if (it != m_connStates.end()) state = it.value();
    }

    // ──── 异步化：offload到业务线程池，HP-Socket worker立即返回 ────
    // 参考WCSApp架构：CtrlMain将业务逻辑提交到线程池，避免阻塞I/O线程
    // SendResponse 在 HP-Socket 中是线程安全的
    if (m_pBusinessPool && m_pServer && m_pServer->HasStarted())
    {
        m_pBusinessPool->commitNoWait([this, dwConnID, state]() {
            if (!m_pServer || !m_pServer->HasStarted()) return;
            ConnState st = state;  // 拷贝到线程池线程栈
            processRequest(m_pServer.Get(), dwConnID, st);
        });
    }
    else
    {
        // 降级：线程池未就绪时直接同步处理
        processRequest(pSender, dwConnID, state);
    }

    return HPR_OK;
}

EnHttpParseResult HttpServer::OnParseError(IHttpServer* pSender, CONNID dwConnID,
                                            int iErrorCode, LPCSTR lpszErrorDesc)
{
    HTTP_ERROR("解析错误 conn=%llu errCode=%d desc=%s",
        (unsigned long long)dwConnID, iErrorCode,
        lpszErrorDesc ? lpszErrorDesc : "unknown");
    return HPR_OK;
}

EnHandleResult HttpServer::OnAccept(ITcpServer* pSender, CONNID dwConnID, UINT_PTR soClient)
{
    int64_t total = m_acceptCount.fetch_add(1) + 1;
    int active = m_activeConns.fetch_add(1) + 1;

    // 记录连接接受时间
    {
        std::lock_guard<std::mutex> lock(m_connMutex);
        m_connAcceptTime[dwConnID] = QDateTime::currentMSecsSinceEpoch();
    }

    // ──── 日志滤重阈值 ────
    // 每100个连接输出一次统计（避免日志洪水）
    // active > 50 时额外输出（连接数偏高，提前关注）
    if (total % 100 == 0 || active > 50)
    {
        HTTP_INFO("连接接受 conn=%llu totalAccept=%lld active=%d",
            (unsigned long long)dwConnID, total, active);
    }

    // active > 100 时输出警告（连接数偏高，可能存在连接泄漏或异常流量）
    if (active > 100)
    {
        HTTP_WARN("连接数偏高 conn=%llu active=%d totalAccept=%lld",
            (unsigned long long)dwConnID, active, total);
    }

    return HR_OK;
}

EnHandleResult HttpServer::OnClose(ITcpServer* pSender, CONNID dwConnID,
                                    EnSocketOperation enOperation, int iErrorCode)
{
    {
        std::lock_guard<std::mutex> lock(m_connMutex);
        m_connStates.remove(dwConnID);

        // 计算连接持续时间
        auto it = m_connAcceptTime.find(dwConnID);
        if (it != m_connAcceptTime.end())
        {
            qint64 connDuration = QDateTime::currentMSecsSinceEpoch() - it.value();
            m_connAcceptTime.erase(it);

            // 只记录异常关闭或长连接(>10s)
            if (iErrorCode != 0 || connDuration > CONN_LONG_DURATION_MS)
            {
                HTTP_INFO("连接关闭 conn=%llu duration=%lldms operation=%d errCode=%d",
                    (unsigned long long)dwConnID, connDuration,
                    (int)enOperation, iErrorCode);
            }
        }
    }

    int64_t totalClose = m_closeCount.fetch_add(1) + 1;
    int active = m_activeConns.fetch_sub(1) - 1;

    // 异常关闭时记录详细信息
    if (iErrorCode != 0)
    {
        HTTP_WARN("连接异常关闭 conn=%llu operation=%d errCode=%d active=%d totalClose=%lld",
            (unsigned long long)dwConnID, (int)enOperation, iErrorCode, active, totalClose);
    }

    return HR_OK;
}

// ============================================================================
// 请求分发
// ============================================================================

void HttpServer::processRequest(IHttpServer* pSender, CONNID dwConnID, ConnState& st)
{
    QElapsedTimer reqTimer;
    reqTimer.start();

    int64_t reqNum = m_requestCount.fetch_add(1) + 1;

    HTTP_INFO("请求 %s %s body=%d conn=%llu req#=%lld", st.method.toLocal8Bit().data(),
        st.path.toLocal8Bit().data(), st.body.size(), (unsigned long long)dwConnID, reqNum);
    emit logMessage(QString("[请求] %1 %2").arg(st.method).arg(st.path));

    // ═══════════════════════════════════════════════════════════════════════
    // 路由1: WMS波次推送（P0核心接口）
    // 调用方: WMS系统自动触发
    // ═══════════════════════════════════════════════════════════════════════
    if (st.path == "/api/DispatchSortingCommand/InsertWaveInfo" && st.method == "POST")
    {
        if (m_pQueue->size() >= MAX_TASK_QUEUE)
        {
            HTTP_WARN("队列已满 拒绝入队 queue=%d", m_pQueue->size());
            sendJsonResponse(pSender, dwConnID, errResponse("服务器繁忙", 503), 503);
            emit logMessage("[WMS] 队列已满，拒绝入队 返回503", true);
        }
        else if (st.body.isEmpty())
        {
            HTTP_WARN("Body为空 拒绝");
            sendJsonResponse(pSender, dwConnID, errResponse("Body为空"));
            emit logMessage("[WMS] Body为空", true);
        }
        else
        {
            WaveTask task;
            task.rawBody  = st.body;
            task.recvTime = QDateTime::currentMSecsSinceEpoch();
            m_pQueue->push(task);
            sendJsonResponse(pSender, dwConnID, okResponse("accepted"));
            HTTP_INFO("WMS入队 len=%d queue=%d elapsed=%lldms", st.body.size(), m_pQueue->size(), reqTimer.elapsed());
            emit logMessage(QString("[WMS] 波次入队 size=%1 queue=%2").arg(st.body.size()).arg(m_pQueue->size()));
        }
        return;
    }

    // 路由2: 格口查询（调用方: PLC/扫描仪扫码后查询对应格口）
    if (st.path == "/api/query" && st.method == "GET")
    {
        QUrlQuery q(st.queryString);
        QString code = q.queryItemValue("code").trimmed();
        QJsonObject result = handleQuery(code);
        sendJsonResponse(pSender, dwConnID, result);
        HTTP_INFO("请求完成 %s code=%s elapsed=%lldms", st.path.toLocal8Bit().data(),
            code.toLocal8Bit().data(), reqTimer.elapsed());
        return;
    }

    // 路由3: 分拣完成标记（调用方: PLC落格确认后/外部程序手动标记）
    if (st.path == "/api/markSorted" && st.method == "POST")
    {
        QJsonDocument d = QJsonDocument::fromJson(st.body);
        QJsonObject result = handleMarkSorted(d.object());
        sendJsonResponse(pSender, dwConnID, result);
        QString code = d.object()["code"].toString();
        HTTP_INFO("请求完成 %s code=%s elapsed=%lldms", st.path.toLocal8Bit().data(),
            code.toLocal8Bit().data(), reqTimer.elapsed());
        return;
    }

    // 路由4: 异常标记（调用方: 扫描仪无法识别/格口查找失败时）
    if (st.path == "/api/markException" && st.method == "POST")
    {
        QJsonDocument d = QJsonDocument::fromJson(st.body);
        QJsonObject result = handleMarkException(d.object());
        sendJsonResponse(pSender, dwConnID, result);
        QString code = d.object()["code"].toString();
        HTTP_INFO("请求完成 %s code=%s elapsed=%lldms", st.path.toLocal8Bit().data(),
            code.toLocal8Bit().data(), reqTimer.elapsed());
        return;
    }

    // 路由5: 波次状态查询（调用方: UI/外部监控程序）
    if (st.path == "/api/waveStatus" && st.method == "GET")
    {
        sendJsonResponse(pSender, dwConnID, handleWaveStatus());
        HTTP_INFO("请求完成 %s elapsed=%lldms", st.path.toLocal8Bit().data(), reqTimer.elapsed());
        return;
    }

    // 路由6: 手动发送PLC指令（调用方: 调试/异常补救）
    if (st.path == "/api/sendToPlc" && st.method == "POST")
    {
        QJsonDocument d = QJsonDocument::fromJson(st.body);
        QJsonObject result = handleSendToPlc(d.object());
        sendJsonResponse(pSender, dwConnID, result);
        QString code = d.object()["code"].toString();
        HTTP_LOG_INFO("请求完成 %s code=%s elapsed=%lldms", st.path.toLocal8Bit().data(),
            code.toLocal8Bit().data(), reqTimer.elapsed());
        return;
    }

    // 路由7: PLC连接状态查询（调用方: UI/外部监控程序）
    if (st.path == "/api/plcStatus" && st.method == "GET")
    {
        sendJsonResponse(pSender, dwConnID, handlePlcStatus());
        HTTP_INFO("请求完成 %s elapsed=%lldms", st.path.toLocal8Bit().data(), reqTimer.elapsed());
        return;
    }

    HTTP_WARN("未知路径 %s elapsed=%lldms", st.path.toLocal8Bit().data(), reqTimer.elapsed());
    sendJsonResponse(pSender, dwConnID, errResponse("Not Found", 404), 404);
    emit logMessage(QString("[请求] 未知路径 %1").arg(st.path), true);
}

// ============================================================================
// 业务处理
// ============================================================================

QJsonObject HttpServer::handleQuery(const QString& code)
{
    if (code.isEmpty()) {
        HTTP_WARN("查询 缺少code参数");
        emit logMessage("[查询] 缺少code参数", true);
        return errResponse("缺少code参数");
    }
    GridEntry e = m_pWaveMgr->getGrid(code);
    if (e.gridNum.isEmpty()) {
        HTTP_WARN("查询 code=%s 未找到格口", code.toLocal8Bit().data());
        m_pWaveMgr->markException(code);
        emit logMessage(QString("[查询] code=%1 未找到格口").arg(code), true);
        return errResponse("未找到格口映射");
    }
    QString g = e.gridNum;
    if (g.contains(',')) g = g.split(',').first().trimmed();
    HTTP_INFO("查询 code=%s grid=%s", code.toLocal8Bit().data(), g.toLocal8Bit().data());
    emit logMessage(QString("[查询] code=%1 → 格口=%2").arg(code).arg(g));

    // ★ 查询到格口后，发送指令到PLC
    if (m_pPlcMgr && m_pPlcMgr->isRunning())
    {
        // 解析格口列表（支持多格口: "122,133,144"）
        std::vector<int> vecGrid;
        QStringList gridList = g.split(',');
        for (const QString& gs : gridList)
        {
            bool ok = false;
            int gn = gs.trimmed().toInt(&ok);
            if (ok) vecGrid.push_back(gn);
        }
        if (!vecGrid.empty())
        {
            bool sent = m_pPlcMgr->sendCodeInfo(code, vecGrid, 1);
            if (sent)
            {
                HTTP_LOG_INFO("PLC发送 code=%s grids=%s", code.toLocal8Bit().data(), g.toLocal8Bit().data());
            }
            else
            {
                // ★ 限流：每个条码只警告一次，避免日志洪水
                bool firstWarn = false;
                {
                    std::lock_guard<std::mutex> lock(m_warnMutex);
                    if (!m_warnedPlcFailCodes.contains(code))
                    {
                        m_warnedPlcFailCodes.insert(code);
                        firstWarn = true;
                    }
                }
                if (firstWarn)
                {
                    HTTP_LOG_WARN("PLC发送失败 code=%s err=%s", code.toLocal8Bit().data(),
                        m_pPlcMgr->lastError().toLocal8Bit().data());
                }
            }
        }
    }

    QJsonObject r;
    r["success"]=true; r["grid"]=g; r["gridType"]=e.gridType;
    r["gridCount"]=e.gridCount; r["orderCode"]=m_pWaveMgr->orderCode();
    return r;
}

QJsonObject HttpServer::handleMarkSorted(const QJsonObject& req)
{
    QString c = req["code"].toString().trimmed();
    if (c.isEmpty()) {
        HTTP_WARN("分拣 缺少code");
        emit logMessage("[分拣] 缺少code", true);
        return errResponse("缺少code");
    }
    m_pWaveMgr->markSorted(c);
    bool complete = m_pWaveMgr->isWaveComplete();
    HTTP_INFO("分拣 code=%s waveComplete=%d", c.toLocal8Bit().data(), complete);
    emit logMessage(QString("[分拣] code=%1 已分拣 waveComplete=%2").arg(c).arg(complete));
    QJsonObject r;
    r["success"]=true; r["waveComplete"]=complete;
    r["orderCode"]=m_pWaveMgr->orderCode();
    return r;
}

QJsonObject HttpServer::handleMarkException(const QJsonObject& req)
{
    QString c = req["code"].toString().trimmed();
    if (c.isEmpty()) {
        HTTP_WARN("异常 缺少code");
        emit logMessage("[异常] 缺少code", true);
        return errResponse("缺少code");
    }
    m_pWaveMgr->markException(c);
    bool complete = m_pWaveMgr->isWaveComplete();
    HTTP_INFO("异常 code=%s waveComplete=%d", c.toLocal8Bit().data(), complete);
    emit logMessage(QString("[异常] code=%1 标记异常 waveComplete=%2").arg(c).arg(complete));
    QJsonObject r;
    r["success"]=true; r["waveComplete"]=complete;
    r["orderCode"]=m_pWaveMgr->orderCode();
    return r;
}

QJsonObject HttpServer::handleWaveStatus()
{
    WaveSnapshot s = m_pWaveMgr->snapshot();
    QJsonObject r;
    r["success"]=true; r["orderCode"]=s.orderCode; r["waveStatus"]=s.waveStatus;
    r["statusText"]=s.statusText; r["orderQty"]=s.orderQty; r["skuCount"]=s.skuCount;
    r["sortedCount"]=s.sortedCount; r["exceptionCount"]=s.exceptionCount;
    r["totalRecv"]=s.totalRecv; r["sumLocation"]=s.sumLocation; r["elapsedSec"]=s.elapsedSec;
    return r;
}

void HttpServer::sendJsonResponse(IHttpServer* pSender, CONNID dwConnID,
                                   const QJsonObject& json, USHORT status)
{
    QByteArray d = QJsonDocument(json).toJson(QJsonDocument::Compact);
    THeader h[1];
    h[0].name = "Content-Type";
    h[0].value = "application/json; charset=UTF-8";
    pSender->SendResponse(dwConnID, status, nullptr, h, 1, (const BYTE*)d.constData(), d.length());
}

QJsonObject HttpServer::okResponse(const QString& data)
{
    QJsonObject r;
    r["resultCode"]="0"; r["success"]=true; r["resultData"]=data;
    r["resultTime"]=QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzzz");
    return r;
}

QJsonObject HttpServer::errResponse(const QString& msg, int code)
{
    QJsonObject r;
    r["resultCode"]=QString::number(code); r["success"]=false;
    r["errorMsg"]=msg;
    r["resultTime"]=QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzzz");
    return r;
}

QJsonObject HttpServer::handleSendToPlc(const QJsonObject& req)
{
    QString code = req["code"].toString().trimmed();
    if (code.isEmpty())
    {
        HTTP_LOG_WARN("sendToPlc 缺少code");
        return errResponse("缺少code参数");
    }

    // 从GridBuffer查询格口
    if (!m_pWaveMgr || !m_pWaveMgr->contains(code))
    {
        HTTP_LOG_WARN("sendToPlc code=%s 未找到格口映射", code.toLocal8Bit().data());
        return errResponse("未找到格口映射");
    }

    GridEntry e = m_pWaveMgr->getGrid(code);
    QString g = e.gridNum;
    if (g.contains(',')) g = g.split(',').first().trimmed();

    // 解析格口列表
    std::vector<int> vecGrid;
    QStringList gridList = g.split(',');
    for (const QString& gs : gridList)
    {
        bool ok = false;
        int gn = gs.trimmed().toInt(&ok);
        if (ok) vecGrid.push_back(gn);
    }

    if (vecGrid.empty())
    {
        HTTP_LOG_WARN("sendToPlc code=%s 格口列表为空", code.toLocal8Bit().data());
        return errResponse("格口列表为空");
    }

    int car = req["car"].toInt(1);
    if (!m_pPlcMgr || !m_pPlcMgr->isRunning())
    {
        HTTP_LOG_WARN("sendToPlc PLC服务未运行");
        return errResponse("PLC服务未运行");
    }

    bool sent = m_pPlcMgr->sendCodeInfo(code, vecGrid, car);
    if (sent)
    {
        LIFE_STAGE_PLC_SEND(code, g, QString::number(car));
        HTTP_LOG_INFO("sendToPlc 成功 code=%s grids=%s car=%d", code.toLocal8Bit().data(), g.toLocal8Bit().data(), car);
        emit logMessage(QString("[PLC] 手动发送 code=%1 grids=%2 car=%3").arg(code).arg(g).arg(car));
        return okResponse("sent");
    }
    else
    {
        LIFE_STAGE_PLC_SEND_FAIL(code);
        HTTP_LOG_ERROR("sendToPlc 失败 code=%s err=%s", code.toLocal8Bit().data(),
            m_pPlcMgr->lastError().toLocal8Bit().data());
        return errResponse(m_pPlcMgr->lastError());
    }
}

QJsonObject HttpServer::handlePlcStatus()
{
    QJsonObject r;
    r["success"] = true;
    if (m_pPlcMgr)
    {
        r["plcRunning"] = m_pPlcMgr->isRunning();
        r["connectedClients"] = m_pPlcMgr->connectedClientCount();
        r["hasClients"] = m_pPlcMgr->hasConnectedClients();
        r["lastError"] = m_pPlcMgr->lastError();
    }
    else
    {
        r["plcRunning"] = false;
        r["connectedClients"] = 0;
        r["hasClients"] = false;
    }
    return r;
}

void HttpServer::logHealthStatus()
{
    int64_t accept  = m_acceptCount.load();
    int64_t close   = m_closeCount.load();
    int64_t request = m_requestCount.load();
    int     active  = m_activeConns.load();
    int     queueSize = m_pQueue ? m_pQueue->size() : 0;
    int     poolThr = m_pBusinessPool ? m_pBusinessPool->thrCount() : 0;
    int     poolIdl = m_pBusinessPool ? m_pBusinessPool->idlCount() : 0;
    int     poolTask = m_pBusinessPool ? m_pBusinessPool->taskCount() : 0;

    HTTP_INFO("健康检查 accept=%lld close=%lld active=%d requests=%lld queue=%d bizPool=%d/%d tasks=%d",
        accept, close, active, request, queueSize, poolIdl, poolThr, poolTask);

    // 活跃连接 > 200 时触发告警（正常场景下不应超过此值，超过可能存在连接池泄漏或异常流量）
    if (active > 200)
    {
        HTTP_WARN("连接数异常偏高 active=%d accept=%lld close=%lld",
            active, accept, close);
    }

    // 业务线程池满载检测：idle==0 说明所有线程都在工作，pendingTasks > thr×2 说明积压严重
    // thr×2 阈值：积压任务超过线程数2倍时告警，提示可能需要扩容线程池
    if (poolIdl == 0 && poolThr > 0 && poolTask > poolThr * 2)
    {
        HTTP_WARN("业务线程池满载 idle=%d/%d pendingTasks=%d",
            poolIdl, poolThr, poolTask);
    }

    // 阻塞检测：10秒内无新请求处理但仍有活跃连接 → 可能发生了线程阻塞
    // s_lastRequest 是函数内静态变量，跨健康检查周期持久化
    static int64_t s_lastRequest = 0;
    if (s_lastRequest > 0 && request == s_lastRequest && active > 0)
    {
        HTTP_WARN("服务可能阻塞: 10秒内无新请求处理 active=%d lastReq#=%lld",
            active, request);
    }
    s_lastRequest = request;
}
