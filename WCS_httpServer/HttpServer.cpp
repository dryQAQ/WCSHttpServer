#include "HttpServer.h"
#include "ParseWorker.h"
#include "log_center.h"
#include "hlog1.h"
#include "ConfigManager.h"
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

    // ★ 设置格口查询回调：相机/PLC 扫到 {条码|小车号} 时 → 查 DoubleBuffer → 返回格口号
    m_pPlcMgr->setLookupCallback([this](const QString& barcode) -> QString {
        if (!m_pBuffer) return QString();
        GridEntry entry = m_pBuffer->get(barcode);
        return entry.gridNum;  // 返回 "15" 或 "1,2,3"（多格口逗号分隔）
    });

    // ──── 业务线程池 ────
    {
        AppConfig& cfg = ConfigManager::instance()->config();
        m_pBusinessPool = new Hanchine::ThreadPool(cfg.businessPoolSize);
        HTTP_INFO("业务线程池已创建 threads=%d", cfg.businessPoolSize);
    }

    // 显式指定 Qt::QueuedConnection：ParseWorker::run() 在独立线程中运行，
    // 使用 AutoConnection 时因 sender/receiver 的 thread() 都在主线程，
    // 但 emit 发生在工作线程，导致信号跨线程传递异常。
    connect(m_pWorker, &ParseWorker::waveParsed, this,
        [this](const QString& orderCode, int skuCount, int orderQty, qint64, const QSet<QString>& recvSet, const QStringList& epcList) {
            m_pWaveMgr->setWaveData(orderCode, orderQty, skuCount);
            m_pWaveMgr->setRecvSet(recvSet);
            // ★ 新波次到来，重置 PLC 发送失败警告集合
            {
                std::lock_guard<std::mutex> lock(m_warnMutex);
                m_warnedPlcFailCodes.clear();
            }
            // ★ 新波次到来，清空旧格口分拣记录
            {
                std::lock_guard<std::mutex> lock(m_gridRecordMutex);
                m_gridSortRecords.clear();
            }
            // ★ 新波次到来，清空旧 EPC→SKU 映射
            {
                std::lock_guard<std::mutex> lock(m_epcSkuMutex);
                m_epcSkuMap.clear();
            }

            // ★ 触发RFID查询：按批次大小拆分EPC列表，分批请求
            if (!epcList.isEmpty())
            {
                HTTP_LOG_INFO("波次解析完成 触发RFID查询 order=%s epcCount=%d",
                    orderCode.toLocal8Bit().data(), epcList.size());

                QJsonArray batch;
                for (int i = 0; i < epcList.size(); ++i)
                {
                    batch.append(epcList[i]);
                    if (batch.size() >= RFID_MAX_BATCH_SIZE || i == epcList.size() - 1)
                    {
                        QString batchCtx = QString("%1_batch%2").arg(orderCode).arg(i / RFID_MAX_BATCH_SIZE + 1);
                        emit rfidQueryRequested(batch, batchCtx);
                        batch = QJsonArray();
                    }
                }
            }
        }, Qt::QueuedConnection);

    // ★ 波次完成 → 异步入池构建 33.md JSON（避免主线程遍历大量数据卡 UI）
    connect(m_pWaveMgr, &WaveManager::waveReadyToReport, this,
        [this](const QString& orderCode) {
            // 1. 快速拷贝格口记录（主线程，加锁 ≤1ms）
            QMap<QString, QVector<GridSortRecord>> recordsCopy;
            {
                std::lock_guard<std::mutex> lock(m_gridRecordMutex);
                recordsCopy = m_gridSortRecords;
                m_gridSortRecords.clear();  // 立即清空，避免双写
            }

            if (recordsCopy.isEmpty())
            {
                HTTP_LOG_WARN("波次完成回传: 无分拣记录 order=%s", orderCode.toLocal8Bit().data());
                emit logMessage(QString("[波次] 无分拣记录，跳过回传 order=%1").arg(orderCode));
                return;
            }

            // 2. 异步入池构建 JSON + 发出信号（业务线程池，不卡主线程）
            if (m_pBusinessPool)
            {
                m_pBusinessPool->commitNoWait([this, orderCode, recordsCopy]() {
                    QJsonObject report = buildReportFromRecords(orderCode, recordsCopy);
                    emit waveCompleteReportReady(report);
                });
            }
            else
            {
                QJsonObject report = buildReportFromRecords(orderCode, recordsCopy);
                emit waveCompleteReportReady(report);
            }
        }, Qt::QueuedConnection);

    // ──── PLC反馈 → 分拣标记 + 按格口记录 ────
    // PLC确认落格后自动标记为已分拣，同时记录格口→条码映射（供锁格回传用）
    connect(m_pPlcMgr, &PlcManager::plcFeedbackReceived, this,
        [this](const QString& code, const QString& grid, const QString& car) {
            if (!m_pWaveMgr) return;
            m_pWaveMgr->markSorted(code);

            // ★ 按格口记录分拣明细（锁格时回传 WMS 用）
            {
                std::lock_guard<std::mutex> lock(m_gridRecordMutex);
                GridSortRecord rec;
                rec.inco   = code;
                rec.car    = car;
                rec.timeMs = QDateTime::currentMSecsSinceEpoch();

                // 从 DoubleBuffer 获取该条码的 gridCount 和 volu
                GridEntry entry = m_pBuffer->get(code);
                rec.gridCount = entry.gridCount;
                rec.volu      = entry.volu.isEmpty() ? QString("--") : entry.volu;

                m_gridSortRecords[grid].append(rec);
            }

            HTTP_LOG_INFO("PLC反馈自动分拣 code=%s grid=%s", code.toLocal8Bit().data(), grid.toLocal8Bit().data());
            emit logMessage(QString("[PLC] 反馈落格分拣 code=%1 grid=%2 car=%3").arg(code).arg(grid).arg(car));
        }, Qt::QueuedConnection);

    // ★ S7 锁格 → 触发 WMS 回传
    connect(m_pPlcMgr, &PlcManager::gridLocked, this,
        [this](const QString& grid) {
            emit logMessage(QString("[锁格] 格口%1 已锁定，准备回传WMS").arg(grid), true);
            sendGridLockFeedback(grid);
        }, Qt::QueuedConnection);

    // PLC连接状态日志（使用 QueuedConnection 确保跨线程安全）
    connect(m_pPlcMgr, &PlcManager::plcConnected, this,
        [this](const QString& ip, int port) {
            HTTP_LOG_INFO("PLC已连接 ip=%s port=%d", ip.toLocal8Bit().data(), port);
            emit logMessage(QString("[PLC] 已连接 %1:%2").arg(ip).arg(port));
        }, Qt::QueuedConnection);
    connect(m_pPlcMgr, &PlcManager::plcDisconnected, this,
        [this](const QString& ip, int port) {
            HTTP_LOG_WARN("PLC断开连接 ip=%s port=%d", ip.toLocal8Bit().data(), port);
            emit logMessage(QString("[PLC] 断开连接 %1:%2").arg(ip).arg(port), true);
        }, Qt::QueuedConnection);

    // PLC批次信号
    connect(m_pPlcMgr, &PlcManager::plcBatchStart, this,
        [this]() {
            HTTP_LOG_INFO("PLC批次开始");
            emit logMessage("[PLC] 批次开始信号");
        }, Qt::QueuedConnection);
    connect(m_pPlcMgr, &PlcManager::plcBatchStop, this,
        [this]() {
            HTTP_LOG_INFO("PLC批次停止");
            emit logMessage("[PLC] 批次停止信号");
        }, Qt::QueuedConnection);

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
        QString("[Http] 服务已启动 port=%1")
            .arg(port));

    // ★ 同时启动PLC监听服务
    if (m_pPlcMgr)
    {
        AppConfig& cfg = ConfigManager::instance()->config();
        if (m_pPlcMgr->start("0.0.0.0", cfg.plcListenPort))
        {
            HTTP_LOG_INFO("PLC监听服务已启动 port=%d", cfg.plcListenPort);
            emit logMessage(QString("[PLC] 监听服务已启动 port=%1").arg(cfg.plcListenPort));
        }
        else
        {
            HTTP_LOG_ERROR("PLC监听服务启动失败 port=%d", cfg.plcListenPort);
            emit logMessage(QString("[PLC] 监听服务启动失败 port=%1").arg(cfg.plcListenPort), true);
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

    // ★ 提取 Host 头（用于重建完整原始 URL）
    {
        THeader headers[64];
        DWORD dwCount = sizeof(headers) / sizeof(headers[0]);
        if (pSender->GetAllHeaders(dwConnID, headers, dwCount))
        {
            for (DWORD i = 0; i < dwCount; ++i)
            {
                if (headers[i].name && _stricmp(headers[i].name, "Host") == 0)
                {
                    state.rawHost = QString::fromUtf8(headers[i].value);
                    break;
                }
            }
        }
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
    // 每 CONN_LOG_THROTTLE_INTERVAL 个连接输出一次统计（避免日志洪水）
    // active > CONN_ACTIVE_WARN_THRESHOLD 时额外输出（连接数偏高，提前关注）
    if (total % CONN_LOG_THROTTLE_INTERVAL == 0 || active > CONN_ACTIVE_WARN_THRESHOLD)
    {
        HTTP_INFO("连接接受 conn=%llu totalAccept=%lld active=%d",
            (unsigned long long)dwConnID, total, active);
    }

    // active > CONN_ACTIVE_HIGH_THRESHOLD 时输出警告（连接数偏高，可能存在连接泄漏或异常流量）
    if (active > CONN_ACTIVE_HIGH_THRESHOLD)
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

    // ★ 原始报文：重建 WMS 发送的完整 URL 字符串
    {
        QString fullUrl = "http://" + st.rawHost + st.path;
        if (!st.queryString.isEmpty())
            fullUrl += "?" + st.queryString;

        if (st.body.isEmpty())
        {
            HTTP_INFO("[原始报文] %s  req#=%lld", fullUrl.toLocal8Bit().data(), reqNum);
        }
        else
        {
            // Body 截断显示
            QByteArray bodyPreview = st.body.left(RAW_REQ_BODY_LOG_LEN);
            bool truncated = st.body.size() > RAW_REQ_BODY_LOG_LEN;
            HTTP_INFO("[原始报文] %s  body=%s%s(%d字节)  req#=%lld",
                fullUrl.toLocal8Bit().data(),
                bodyPreview.constData(),
                truncated ? "..." : "",
                st.body.size(), reqNum);
        }
    }
    emit logMessage(QString("[请求] %1 %2").arg(st.method).arg(st.path));

    // ═══════════════════════════════════════════════════════════════════════
    // 路由1: WMS波次数据推送（P0核心接口）
    // 调用方: WMS系统
    // 报文: POST /api/DispatchSortingCommand/InsertWaveInfo
    // 功能: 推送波次数据，包含条码-格口映射，WCS解析后存储
    // ═══════════════════════════════════════════════════════════════════════
    if (st.path == API_INSERT_WAVE_INFO && st.method == "POST")
    {
        // ★ 前置校验：所有格口必须已绑定容器，否则拒绝波次
        if (!areAllBindingsComplete())
        {
            int bc = boundCount();
            HTTP_WARN("InsertWaveInfo 容器未全部绑定 bound=%d/%d", bc, BINDING_SLOT_COUNT);
            emit logMessage(QString("[WMS] 容器未全部绑定 已绑定:%1/%2 拒绝波次").arg(bc).arg(BINDING_SLOT_COUNT), true);
            QJsonObject err;
            err["code"]    = "400";
            err["message"] = QString("容器未全部绑定，已绑定: %1/%2，请等待WMS下发全部容器绑定").arg(bc).arg(BINDING_SLOT_COUNT);
            err["sentTime"] = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.000");
            sendJsonResponse(pSender, dwConnID, err, 400);
            return;
        }

        if (m_pQueue->size() >= MAX_TASK_QUEUE)
        {
            HTTP_WARN("队列已满 拒绝入队 queue=%d", m_pQueue->size());
            sendJsonResponse(pSender, dwConnID, errResponse("服务器繁忙", "503"), 503);
            emit logMessage("[WMS] 队列已满，拒绝入队 返回503", true);
        }
        else if (st.body.isEmpty() || st.body == "null" || st.body == "{}")
        {
            HTTP_WARN("Body为空或无效 拒绝 body=%s", st.body.constData());
            sendJsonResponse(pSender, dwConnID, errResponse("Body为空或无效", "400"));
            emit logMessage("[WMS] Body为空或无效", true);
        }
        else
        {
            WaveTask task;
            task.rawBody  = st.body;
            task.recvTime = QDateTime::currentMSecsSinceEpoch();
            m_pQueue->push(task);
            sendJsonResponse(pSender, dwConnID, okResponse(""));
            HTTP_INFO("InsertWaveInfo 入队 len=%d queue=%d elapsed=%lldms", st.body.size(), m_pQueue->size(), reqTimer.elapsed());
            emit logMessage(QString("[WMS] InsertWaveInfo 入队 size=%1 queue=%2").arg(st.body.size()).arg(m_pQueue->size()));
        }
        return;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 路由2: 格口容器绑定
    // 调用方: WMS系统
    // 报文: POST /api/DispatchSortingCommand/BindingLatticePort?latticehole=格口号&boxcode=容器号
    // 功能: 绑定容器号与格口的对应关系，用于后续装箱数据同步
    // ═══════════════════════════════════════════════════════════════════════
    if (st.path == API_BINDING_LATTICE_PORT && st.method == "POST")
    {
        // 优先从 queryString 解析参数（WMS 标准格式）
        QUrlQuery q(st.queryString);
        QString latticehole = q.queryItemValue("latticehole").trimmed();
        QString boxcode     = q.queryItemValue("boxcode").trimmed();

        // 如果 queryString 为空，尝试从 Body JSON 解析
        if (latticehole.isEmpty() || boxcode.isEmpty())
        {
            QJsonDocument d = QJsonDocument::fromJson(st.body);
            QJsonObject obj = d.object();
            latticehole = obj["latticehole"].toString().trimmed();
            boxcode     = obj["boxcode"].toString().trimmed();
        }

        QJsonObject result = handleBindingLatticePort(latticehole, boxcode);
        sendJsonResponse(pSender, dwConnID, result);
        HTTP_LOG_INFO("BindingLatticePort latticehole=%s boxcode=%s elapsed=%lldms",
            latticehole.toLocal8Bit().data(), boxcode.toLocal8Bit().data(), reqTimer.elapsed());
        return;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 路由3: 退货任务取消
    // 调用方: WMS系统
    // 报文: POST /api/DispatchSortingCommand/InsertWaveIn
    // 功能: WMS 下发取消指令，清除当前波次数据
    // ═══════════════════════════════════════════════════════════════════════
    if (st.path == API_INSERT_WAVE_IN && st.method == "POST")
    {
        QJsonDocument d = QJsonDocument::fromJson(st.body);
        QJsonObject result = handleCancelWave(d.object());
        sendJsonResponse(pSender, dwConnID, result);
        HTTP_INFO("InsertWaveIn 退货取消 body=%d elapsed=%lldms", st.body.size(), reqTimer.elapsed());
        emit logMessage(QString("[WMS] InsertWaveIn 退货取消 elapsed=%1ms").arg(reqTimer.elapsed()));
        return;
    }

    // 未知路径
    HTTP_WARN("未知路径 %s elapsed=%lldms", st.path.toLocal8Bit().data(), reqTimer.elapsed());
    sendJsonResponse(pSender, dwConnID, errResponse("Not Found", "404"), 404);
    emit logMessage(QString("[请求] 未知路径 %1").arg(st.path), true);
}

// ============================================================================
// 业务处理
// ============================================================================

// ============================================================================
// sendJsonResponse — HTTP JSON 响应
// ============================================================================
void HttpServer::sendJsonResponse(IHttpServer* pSender, CONNID dwConnID,
                                   const QJsonObject& json, USHORT status)
{
    QByteArray d = QJsonDocument(json).toJson(QJsonDocument::Compact);
    THeader h[1];
    h[0].name = "Content-Type";
    h[0].value = "application/json; charset=UTF-8";
    pSender->SendResponse(dwConnID, status, nullptr, h, 1, (const BYTE*)d.constData(), d.length());
}

QJsonObject HttpServer::okResponse(const QString& msg)
{
    QJsonObject r;
    r["code"]     = "200";       // 正常响应码（新文档格式）
    r["message"]  = msg;         // 响应信息（成功时为空）
    r["sentTime"] = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.000");
    return r;
}

QJsonObject HttpServer::errResponse(const QString& msg, const QString& code)
{
    QJsonObject r;
    r["code"]     = code;        // 异常响应码（如 "404", "500"）
    r["message"]  = msg;         // 异常原因
    r["sentTime"] = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.000");
    return r;
}

// ============================================================================
// handleBindingLatticePort — 格口容器绑定
// WMS 下发容器与格口的绑定关系，用于后续装箱数据同步
// 参数来源: JSON Body {"boxcode":"fDD03","latticehole":"00001"} 或 queryString
// ============================================================================
QJsonObject HttpServer::handleBindingLatticePort(const QString& latticehole, const QString& boxcode)
{
    if (boxcode.isEmpty() || latticehole.isEmpty())
    {
        HTTP_LOG_WARN("BindingLatticePort 参数缺失 boxcode=%s latticehole=%s",
            boxcode.toLocal8Bit().data(), latticehole.toLocal8Bit().data());
        emit logMessage(QString("[容器绑定] 参数缺失 boxcode=%1 latticehole=%2")
            .arg(boxcode).arg(latticehole), true);
        QJsonObject r;
        r["code"] = 100;
        r["msg"]  = QString("参数缺失: latticehole和boxcode均为必填");
        return r;
    }

    // ★ 格口号范围校验：必须在 1～BINDING_SLOT_COUNT 范围内
    bool ok = false;
    int gridNum = latticehole.toInt(&ok);
    if (!ok || gridNum < 1 || gridNum > BINDING_SLOT_COUNT)
    {
        HTTP_LOG_WARN("BindingLatticePort 格口号越界 latticehole=%s range=1..%d",
            latticehole.toLocal8Bit().data(), BINDING_SLOT_COUNT);
        emit logMessage(QString("[容器绑定] 格口号越界 latticehole=%1 (有效范围: 1~%2)")
            .arg(latticehole).arg(BINDING_SLOT_COUNT), true);
        QJsonObject r;
        r["code"] = 100;
        r["msg"]  = QString("格口号越界，有效范围: 1~%1").arg(BINDING_SLOT_COUNT);
        return r;
    }

    {
        std::lock_guard<std::mutex> lock(m_containerMutex);

        // ★ 重复绑定告警：同一格口绑定不同容器时记录警告
        auto it = m_containerBindings.find(latticehole);
        if (it != m_containerBindings.end() && it.value() != boxcode)
        {
            HTTP_LOG_WARN("BindingLatticePort 重复绑定覆盖 latticehole=%s old=%s new=%s",
                latticehole.toLocal8Bit().data(),
                it.value().toLocal8Bit().data(),
                boxcode.toLocal8Bit().data());
            emit logMessage(QString("[容器绑定] 格口%1 覆盖绑定: %2 → %3")
                .arg(latticehole).arg(it.value()).arg(boxcode), true);
        }

        m_containerBindings[latticehole] = boxcode;
    }

    HTTP_LOG_INFO("容器绑定成功 latticehole=%s → boxcode=%s total=%d",
        latticehole.toLocal8Bit().data(), boxcode.toLocal8Bit().data(),
        boundCount());
    emit logMessage(QString("[容器绑定] 格口%1 → 容器%2 (共%3/%4)")
        .arg(latticehole).arg(boxcode).arg(boundCount()).arg(BINDING_SLOT_COUNT));
    emit bindingUpdated();  // ★ 通知 UI 即时刷新

    QJsonObject r;
    r["code"] = 200;
    r["msg"]  = "收到信息";
    return r;
}

// ============================================================================
// handleCancelWave — 退货任务取消
// WMS 下发取消指令，清除当前波次数据
// 只能在 IDLE/RECEIVED 状态下取消，已开始分拣的波次不允许取消
// ============================================================================
QJsonObject HttpServer::handleCancelWave(const QJsonObject& req)
{
    QString orderCode    = req["orderCode"].toString().trimmed();
    QString cancelReason = req["cancelReason"].toString().trimmed();

    if (orderCode.isEmpty())
    {
        HTTP_LOG_WARN("CancelWave 缺少orderCode");
        emit logMessage("[取消波次] 缺少orderCode", true);
        return errResponse("缺少orderCode", "400");
    }

    // 校验波次号是否匹配当前活跃波次
    if (!m_pWaveMgr || m_pWaveMgr->orderCode() != orderCode)
    {
        HTTP_LOG_WARN("CancelWave 波次不匹配 req=%s current=%s",
            orderCode.toLocal8Bit().data(),
            m_pWaveMgr ? m_pWaveMgr->orderCode().toLocal8Bit().data() : "null");
        emit logMessage(QString("[取消波次] 波次不匹配或不存在 req=%1").arg(orderCode), true);
        return errResponse("波次不存在或已完结", "404");
    }

    // 状态校验: 只能在 IDLE/RECEIVED 状态下取消
    int status = m_pWaveMgr->status();
    if (status >= WAVE_SORTING)
    {
        HTTP_LOG_WARN("CancelWave 波次已开始分拣 不允许取消 orderCode=%s status=%d",
            orderCode.toLocal8Bit().data(), status);
        emit logMessage(QString("[取消波次] 已开始分拣 不允许取消 orderCode=%1 status=%2")
            .arg(orderCode).arg(status), true);
        return errResponse("波次已开始分拣，不允许取消", "400");
    }

    // 执行取消: 清理波次数据
    m_pWaveMgr->clearWave();

    // 清理容器绑定（属于被取消的波次）
    {
        std::lock_guard<std::mutex> lock(m_containerMutex);
        int count = m_containerBindings.size();
        m_containerBindings.clear();
        HTTP_LOG_INFO("CancelWave 已清理容器绑定 count=%d", count);
    }

    // ★ 清理格口分拣记录
    {
        std::lock_guard<std::mutex> lock(m_gridRecordMutex);
        m_gridSortRecords.clear();
    }

    // 清理 TaskQueue 中积压的待解析任务（可能有同波次的重复推送）
    // TaskQueue 没有 clear()，通过 pop 直到空

    HTTP_LOG_INFO("CancelWave 波次已取消 orderCode=%s reason=%s",
        orderCode.toLocal8Bit().data(),
        cancelReason.isEmpty() ? "无" : cancelReason.toLocal8Bit().data());
    emit logMessage(QString("[取消波次] 波次已取消 orderCode=%1 reason=%2")
        .arg(orderCode).arg(cancelReason.isEmpty() ? "无" : cancelReason));

    return okResponse();
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

    // 活跃连接 > CONN_ACTIVE_ALERT_THRESHOLD 时触发告警
    if (active > CONN_ACTIVE_ALERT_THRESHOLD)
    {
        HTTP_WARN("连接数异常偏高 active=%d accept=%lld close=%lld",
            active, accept, close);
    }

    // 业务线程池满载检测：idle==0 说明所有线程都在工作
    // POOL_OVERLOAD_MULTIPLIER 倍数阈值：积压任务超过线程数×N时告警
    if (poolIdl == 0 && poolThr > 0 && poolTask > poolThr * POOL_OVERLOAD_MULTIPLIER)
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

// ============================================================================
// sendGridLockFeedback — 锁格时构建分拣明细，发送 WMS 回传
// 报文格式: 33.md "gwisSubProductClassifyOrder"
// ============================================================================
void HttpServer::sendGridLockFeedback(const QString& grid)
{
    // ── 1. 获取容器绑定 ──
    QString boxCode;
    {
        std::lock_guard<std::mutex> lock(m_containerMutex);
        // 格口号可能是零填充 "00001" 或普通 "1"，都查一下
        boxCode = m_containerBindings.value(grid);
        if (boxCode.isEmpty())
        {
            // 尝试去掉前置 0 再查
            bool ok = false;
            int g = grid.toInt(&ok);
            if (ok && g >= 1)
                boxCode = m_containerBindings.value(QString("%1").arg(g, GRID_KEY_PADDING, 10, QChar('0')));
        }
    }

    // ── 2. 获取波次号 ──
    QString orderCode = m_pWaveMgr ? m_pWaveMgr->orderCode() : QString();

    // ── 3. 获取该格口的分拣记录 ──
    QVector<GridSortRecord> records;
    {
        std::lock_guard<std::mutex> lock(m_gridRecordMutex);
        auto it = m_gridSortRecords.find(grid);
        if (it != m_gridSortRecords.end())
            records = it.value();
    }

    if (records.isEmpty())
    {
        HTTP_LOG_WARN("锁格回传: 格口 %s 无分拣记录, 跳过", grid.toLocal8Bit().data());
        emit logMessage(QString("[锁格] 格口%1 无分拣记录，跳过回传").arg(grid));
        return;
    }

    // ── 4. 构建 WMS 回传 JSON ──
    AppConfig& cfg = ConfigManager::instance()->config();
    QJsonObject head;
    head["orderCode"]      = orderCode;
    head["orderType"]      = WMS_ORDER_TYPE;
    head["warehouseCode"]  = cfg.warehouseCode;
    head["goodsOwner"]     = cfg.goodsOwner;
    head["fromLocation"]   = records.first().volu;  // 取第一条的 volu
    head["createDate"]     = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.000");
    head["createUserCode"] = WMS_OPERUSER_CODE;
    head["createUserName"] = QString::fromUtf8(WMS_OPERUSER_NAME);
    head["remark"]         = "";
    head["gwf1-20"]        = "";

    QJsonArray detailList;
    int lineNum = 1;
    for (const GridSortRecord& rec : records)
    {
        QJsonObject item;
        item["lineNum"]        = QString::number(lineNum++);
        item["num"]            = boxCode.isEmpty() ? rec.car : boxCode;  // 框号优先容器号
        item["targetLocation"] = grid;
        item["sku"]            = rec.inco;
        item["qty"]            = QString::number(rec.gridCount);
        item["batchCode"]      = orderCode;
        item["gwf1-20"]        = "";
        detailList.append(item);
    }
    head["detailList"] = detailList;

    QJsonObject report;
    report["head"] = head;

    HTTP_LOG_INFO("锁格回传 grid=%s box=%s order=%s items=%d",
        grid.toLocal8Bit().data(), boxCode.toLocal8Bit().data(),
        orderCode.toLocal8Bit().data(), records.size());
    emit logMessage(QString("[锁格] 回传WMS grid=%1 box=%2 items=%3")
        .arg(grid).arg(boxCode).arg(records.size()));

    // ★ 信号发送后清理该格口的记录（避免重复回传）
    {
        std::lock_guard<std::mutex> lock(m_gridRecordMutex);
        m_gridSortRecords.remove(grid);
    }

    emit gridLockReportReady(report);
}

// ============================================================================
// buildReportFromRecords — 从记录副本构建 33.md 格式回传 JSON（线程安全，无锁）
// 由业务线程池调用，不在主线程执行，不持有任何锁
// ============================================================================
QJsonObject HttpServer::buildReportFromRecords(const QString& orderCode,
                                                const QMap<QString, QVector<GridSortRecord>>& records)
{
    AppConfig& cfg = ConfigManager::instance()->config();

    QJsonObject head;
    head["orderCode"]      = orderCode;
    head["orderType"]      = WMS_ORDER_TYPE;
    head["warehouseCode"]  = cfg.warehouseCode;
    head["goodsOwner"]     = cfg.goodsOwner;
    head["createDate"]     = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.000");
    head["createUserCode"] = WMS_OPERUSER_CODE;
    head["createUserName"] = QString::fromUtf8(WMS_OPERUSER_NAME);
    head["remark"]         = "";
    head["gwf1-20"]        = "";

    QJsonArray detailList;
    int lineNum = 1;
    int totalItems = 0;
    QString firstVolu;

    for (auto it = records.constBegin(); it != records.constEnd(); ++it)
    {
        const QString& grid = it.key();
        for (const GridSortRecord& rec : it.value())
        {
            if (firstVolu.isEmpty() && !rec.volu.isEmpty() && rec.volu != "--")
                firstVolu = rec.volu;

            QJsonObject item;
            item["lineNum"]        = QString::number(lineNum++);
            item["num"]            = rec.car;
            item["targetLocation"] = grid;
            item["sku"]            = rec.inco;
            item["qty"]            = QString::number(rec.gridCount);
            item["batchCode"]      = orderCode;
            item["gwf1-20"]        = "";
            detailList.append(item);
            totalItems++;
        }
    }

    head["fromLocation"] = firstVolu.isEmpty() ? "--" : firstVolu;
    head["detailList"]   = detailList;

    QJsonObject report;
    report["head"] = head;

    HTTP_LOG_INFO("波次完成回传 order=%s grids=%d items=%d",
        orderCode.toLocal8Bit().data(), (int)records.size(), totalItems);

    return report;
}

// ============================================================================
// onRfidQueryResult — 接收RFID服务返回的EPC→SKU查询结果
// 解析格式: {"data":{"data":[{"barcode":"xxx","epc":"xxx",...}]},"success":true}
// 将 EPC→barcode 映射存储到 m_epcSkuMap
// ============================================================================
void HttpServer::onRfidQueryResult(const QJsonObject& result, const QString& context)
{
    if (result.isEmpty())
    {
        HTTP_LOG_WARN("RFID查询结果为空 context=%s", context.toLocal8Bit().data());
        emit logMessage(QString("[RFID] 查询结果为空 context=%1").arg(context), true);
        return;
    }

    bool success = result["success"].toBool(false);
    if (!success)
    {
        QString errMsg = result["msg"].toString();
        HTTP_LOG_WARN("RFID查询失败 context=%s msg=%s", context.toLocal8Bit().data(), errMsg.toLocal8Bit().data());
        emit logMessage(QString("[RFID] 查询失败 context=%1 msg=%2").arg(context).arg(errMsg), true);
        return;
    }

    QJsonObject dataObj = result["data"].toObject();
    QJsonArray dataArr  = dataObj["data"].toArray();

    if (dataArr.isEmpty())
    {
        HTTP_LOG_WARN("RFID查询成功但无数据 context=%s", context.toLocal8Bit().data());
        emit logMessage(QString("[RFID] 查询成功但无数据 context=%1").arg(context));
        return;
    }

    // 解析每条 EPC→barcode 映射
    int newCount = 0;
    {
        std::lock_guard<std::mutex> lock(m_epcSkuMutex);
        for (const QJsonValue& val : dataArr)
        {
            QJsonObject item = val.toObject();
            QString epc     = item["epc"].toString().trimmed();
            QString barcode = item["barcode"].toString().trimmed();

            if (epc.isEmpty()) continue;

            if (!barcode.isEmpty())
            {
                m_epcSkuMap[epc] = barcode;
                newCount++;
            }
        }
    }

    HTTP_LOG_INFO("RFID查询结果已存储 context=%s total=%d new=%d",
        context.toLocal8Bit().data(), dataArr.size(), newCount);
    emit logMessage(QString("[RFID] EPC→SKU映射已存储 context=%1 返回%2条 有效%3条")
        .arg(context).arg(dataArr.size()).arg(newCount));
}
