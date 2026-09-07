#include "HttpServer.h"
#include "ParseWorker.h"
#include "HttpClient.h"
#include "LogService.h"
#include "ConfigManager.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QUrlQuery>
#include <QDateTime>
#include <QElapsedTimer>
#include <QThread>
#include <QCoreApplication>
#include <QUuid>
#include <cstring>
#include <windows.h>
#include <tchar.h>
#include "define.h"

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
    m_pRfidPush = new RfidPushClient(this);  // ★ 2026-09-04 RFID 推送 TCP 客户端（主动连 RFID 服务端）
    m_pSortingDb = &SortingDatabase::instance(); // ★ 分拣记录数据库（单例）
    m_pEpcCache  = new EpcCache(RFID_CACHE_TTL_SEC); // ★ S4 EPC短缓存（T-S4-04）

    // ★ 2026-09-04：RFID 推送（TCP 客户端接收）→ 复用 handleRfidCarNumReport 处理
    //   必须 QueuedConnection：OnReceive 在 HP-Socket 工作线程，handleRfidCarNumReport
    //   会调用 HttpClient(QNetworkAccessManager) 等主线程 affinity 对象，需回主线程执行
    connect(m_pRfidPush, &RfidPushClient::rfidPushReceived, this,
            &HttpServer::handleRfidCarNumReport, Qt::QueuedConnection);

    // ★ 打开本地数据库（路径: exe同目录/data/sorting_records.db）
    {
        QString dbPath = QCoreApplication::applicationDirPath() + "/" + SORTING_DB_FILE;
        if (m_pSortingDb->open(dbPath))
        {
            HTTP_LOG_INFO("分拣记录数据库已打开 path=%s", dbPath.toLocal8Bit().data());
        }
        else
        {
            HTTP_LOG_ERROR("分拣记录数据库打开失败 path=%s", dbPath.toLocal8Bit().data());
        }
    }

    // ★ 设置格口查询回调：PLC/相机扫到识别码时 → 查 DoubleBuffer → 返回格口号
    // 识别码 = EPC（商品编码），客户已确认EPC（商品编码）即EPC编码（2026-08-10）
    m_pPlcMgr->setLookupCallback([this](const QString& code) -> QString {
        if (!m_pBuffer) return QString();
        GridEntry entry = m_pBuffer->get(code);
        return entry.gridNum;  // 返回 "15" 或 "1,2,3"（多格口逗号分隔）
    });

    // ★ 设置小车号查询回调：从 EpcCache 获取 RFID 提供的小车号
    m_pPlcMgr->setCarNumCallback([this](const QString& code) -> QString {
        if (!m_pEpcCache) return CAR_NUM_STR(DEFAULT_CAR_NUM);
        QString carNum = m_pEpcCache->getCarNum(code);
        return carNum.isEmpty() ? CAR_NUM_STR(DEFAULT_CAR_NUM) : carNum;
    });

    // ──── 业务线程池 ────
    {
        AppConfig& cfg = ConfigManager::instance()->config();
        m_pBusinessPool = new Hanchine::ThreadPool(cfg.businessPoolSize);
        HTTP_INFO("业务线程池已创建 threads=%d", cfg.businessPoolSize);

        // ★ 专用业务线程池（参考WCSApp分类线程池设计，不同业务用独立池，方便管理内存）

        m_pPlcRecvPool = new Hanchine::ThreadPool(cfg.plcRecvPoolSize);

        HTTP_INFO("PLC反馈接收专用线程池已创建 threads=%d", cfg.plcRecvPoolSize);

        
    }

    // ★ 2026-09-04 P0修复：波次明细异步落库完成 → 主线程推进 BOUND
    //   emit 发生在业务线程池线程（跨线程），AutoConnection 自动回到主线程执行
    connect(this, &HttpServer::wavePersistenceFinished, this,
        &HttpServer::onWavePersistenceFinished);

    // ★ 波次状态变化 → 同步 return_wave.status 到数据库（异步）
    //   未完成波次面板/重启检测/恢复 依赖 DB 状态准确；此前仅取消时落库，其余状态变更未持久化
    connect(m_pWaveMgr, &WaveManager::waveStatusChanged, this,
        [this](int newStatus) {
            QString orderCode = m_pWaveMgr->orderCode();
            if (orderCode.isEmpty() || !m_pSortingDb || !m_pSortingDb->isOpen())
                return;
            if (m_pBusinessPool)
                m_pBusinessPool->commitNoWait([this, orderCode, newStatus]() {
                    m_pSortingDb->updateWaveStatus(orderCode, newStatus);
                });
            else
                m_pSortingDb->updateWaveStatus(orderCode, newStatus);

            // ★ 2026-09-07 波次到达终态（已完结/已取消）且有待执行波次 → 自动开始下一波次
            //   （H8 成功/失败耗尽/会话超时兜底、H5 取消 全部经由此处触发，主线程串行安全）
            if (newStatus == WAVE_FINISHED || newStatus == WAVE_CANCELLED)
                maybeStartPendingWave();
        });

    // 显式指定 Qt::QueuedConnection：ParseWorker::run() 在独立线程中运行，
    // 使用 AutoConnection 时因 sender/receiver 的 thread() 都在主线程，
    // 但 emit 发生在工作线程，导致信号跨线程传递异常。
    connect(m_pWorker, &ParseWorker::waveParsed, this,
        [this](const QString& orderCode, int skuCount, int orderQty, qint64, const QSet<QString>& recvSet,
               const QByteArray& rawBody) {
            // ★ 2026-09-06 接收闸门：停止接收后到达的队列残余任务不注册波次，仅记录
            //   ★ 2026-09-07 待执行队列重放（自动开始下一波次）放行
            if (!m_receiving.load() && !m_replayingPending.load())
            {
                HTTP_LOG_WARN("未接收任务，跳过波次注册 orderCode=%s（停止接收后到达的残留任务）",
                    orderCode.toLocal8Bit().data());
                return;
            }
            m_replayingPending.store(false);   // ★ 重放任务已到达注册点，清除放行标志

            // ── ★ 2026-09-07 波次执行中收到新波次 → 排队，不覆盖当前任务 ──
            {
                WaveManager* wm = m_pWaveMgr;
                QString curOrder = wm ? wm->orderCode() : QString();
                int curSt        = wm ? wm->status() : -1;
                bool busy = !curOrder.isEmpty() && curOrder != orderCode
                    && (curSt == WAVE_CREATED || curSt == WAVE_BOUND || curSt == WAVE_SORTING
                        || curSt == WAVE_FULLBOX_SYNC || curSt == WAVE_HELD
                        || curSt == WAVE_CANCEL_PENDING || curSt == WAVE_ENDING);
                if (busy)
                {
                    // 防重复排队：队列中已有同单 → 忽略本次
                    for (const PendingWave& p : m_pendingWaveQueue)
                    {
                        if (p.orderCode == orderCode)
                        {
                            HTTP_LOG_WARN("波次已在待执行队列，忽略重复 orderCode=%s", orderCode.toLocal8Bit().data());
                            emit logMessage(QString("[波次] %1 已在待执行队列，忽略重复下发").arg(orderCode), true);
                            return;
                        }
                    }
                    PendingWave pw;
                    pw.orderCode = orderCode;
                    pw.rawBody   = rawBody;
                    pw.recvTime  = QDateTime::currentMSecsSinceEpoch();
                    m_pendingWaveQueue.append(pw);
                    // ★ 落波次头（波次记录列表可见，状态=已下发/待执行）；明细待执行时随格口映射一起落库
                    if (m_pSortingDb)
                        m_pSortingDb->upsertReturnWave(orderCode, orderQty, WAVE_CREATED);
                    HTTP_LOG_INFO("新波次排队 orderCode=%s qty=%d 当前波次=%s 队列=%d",
                        orderCode.toLocal8Bit().data(), orderQty, curOrder.toLocal8Bit().data(),
                        m_pendingWaveQueue.size());
                    emit logMessage(QString("[波次] 新波次 %1 已排队（待执行 %2 个）——当前波次 %3 结束后自动开始")
                        .arg(orderCode).arg(m_pendingWaveQueue.size()).arg(curOrder));
                    return;
                }
            }

            m_pWaveMgr->setWaveData(orderCode, orderQty, skuCount);
            m_pWaveMgr->setRecvSet(recvSet);

            // ★ 新波次下发时重启 Outbox 重试定时器（上一波次完结时已停止）
            if (m_outboxEndTimer)     m_outboxEndTimer->start(OUTBOX_POLL_INTERVAL_SEC * 1000);
            if (m_outboxFullboxTimer) m_outboxFullboxTimer->start(OUTBOX_POLL_INTERVAL_SEC * 1000);

            // ★ 波次下发后，数据库写入完成即默认已绑定，不再检查所有格口是否绑定
            //   直接推进到 BOUND 状态，等待用户点击"开始分拣"按钮手动触发
            //   点击前允许取消波次，点击后分拣开始，取消被拒绝
            // ★ [已废弃] 旧逻辑：检查所有格口是否绑定完成才推进 BOUND
            // if (m_pWaveMgr->status() == WAVE_CREATED && areAllBindingsComplete())
            // {
            //     m_pWaveMgr->setState(WAVE_BOUND);
            //     HTTP_LOG_INFO("波次自动推进 CREATED→BOUND orderCode=%s bound=%d/%d (等待手动开始分拣)",
            //         orderCode.toLocal8Bit().data(), boundCount(), m_expectedBindCount);
            //     emit logMessage(QString("[波次] 自动推进: 已下发→已绑定 orderCode=%1 (等待手动开始分拣)").arg(orderCode));
            // }

            // ★ S1 新增：波次数据落库（T-S1-01/03）—— 2026-09-04 P0修复：头同步+明细异步
            //   持久化 ReturnWave 头 + ReturnWaveItem 明细到 SQLite
            if (m_pSortingDb)
            {
                // ── 波次头（单行，同步执行，约1ms，不卡UI）──
                //   先落头：即使进程崩溃，DB 中也有波次头（CREATED），重启可发现"有头无明细"并提示
                m_pSortingDb->upsertReturnWave(orderCode, orderQty, m_pWaveMgr->status());

                // ── 波次明细（万级，异步提交业务线程池，不阻塞主线程）──
                //   insertWaveItems 内部 runOnDbThread 阻塞的是业务线程池线程而非主线程
                //   可靠性：失败重试 WAVE_PERSIST_RETRY_MAX 次 → 仍失败写异常表+保持CREATED
                QVector<ReturnWaveItemRecord> items = buildWaveItems(orderCode);
                int persistSkuCount = skuCount;
                m_wavePersistPending.store(true);
                m_pBusinessPool->commitNoWait([this, orderCode, items, orderQty, persistSkuCount]() {
                    bool bOk = false;
                    for (int attempt = 0; attempt <= WAVE_PERSIST_RETRY_MAX; ++attempt)
                    {
                        bOk = m_pSortingDb->insertWaveItems(orderCode, items);
                        if (bOk) break;
                        HTTP_ERROR("波次明细落库失败 重试中 attempt=%d/%d orderCode=%s items=%d",
                            attempt, WAVE_PERSIST_RETRY_MAX,
                            orderCode.toLocal8Bit().data(), items.size());
                        QThread::msleep(WAVE_PERSIST_RETRY_INTERVAL_MS);
                    }
                    if (bOk)
                    {
                        HTTP_INFO("波次数据已落库(异步) orderCode=%s items=%d skuCount=%d orderQty=%d",
                            orderCode.toLocal8Bit().data(), items.size(), persistSkuCount, orderQty);
                    }
                    else
                    {
                        // ★ 本地SQLite落库失败（磁盘满/损坏等极端情况）：写异常表供人工排查
                        ExceptionRecord ex;
                        ex.type      = QString::fromUtf8("波次落库失败");
                        ex.orderCode = orderCode;
                        ex.reason    = QString::fromUtf8("明细插入重试%1次仍失败 items=%2").arg(WAVE_PERSIST_RETRY_MAX).arg(items.size());
                        ex.time      = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
                        m_pSortingDb->insertException(ex);
                        HTTP_ERROR("波次明细落库失败 已写异常表 orderCode=%s items=%d (保持CREATED，需人工处理)",
                            orderCode.toLocal8Bit().data(), items.size());
                    }
                    // ★ 回主线程推进 BOUND（AutoConnection：业务线程池线程 emit → 主线程槽执行）
                    emit wavePersistenceFinished(orderCode, bOk, persistSkuCount);
                });
            }
            else
            {
                // 极端降级：DB 不可用时无法落库，保持 CREATED 并提示（不阻塞分拣流程）
                HTTP_ERROR("波次落库跳过: DB 未初始化 orderCode=%s", orderCode.toLocal8Bit().data());
                emit wavePersistenceFinished(orderCode, false, skuCount);
            }
        }, Qt::QueuedConnection);

    // ★ 格口号越界异常记录（ParseWorker 解析时检测到越界格口号，写入异常表）
    connect(m_pWorker, &ParseWorker::parseException, this,
        [this](const QString& orderCode, const QString& epc,
               const QString& gridNum, const QString& reason) {
            if (m_pSortingDb && m_pSortingDb->isOpen())
            {
                ExceptionRecord ex;
                ex.type      = QString::fromUtf8("格口号越界");
                ex.orderCode = orderCode;
                ex.epc       = epc;
                ex.reason    = reason;
                ex.time      = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
                m_pSortingDb->insertException(ex);
                HTTP_WARN("格口号越界异常已记录 orderCode=%s epc=%s gridNum=%s",
                    orderCode.toLocal8Bit().data(), epc.toLocal8Bit().data(),
                    gridNum.toLocal8Bit().data());
                emit logMessage(QString("[异常] 格口号越界 EPC=%1 gridNum=%2 → 已写入异常表")
                    .arg(epc).arg(gridNum), true);
            }
        }, Qt::QueuedConnection);

    // ★ 波次分拣完成 → 仅提示，不自动发送「波次完成回传」（2026-09-07 客户确认）：
    //   满箱切换(H7) 仅由人工 PLC 锁格触发；分拣完成但未锁格的格口数据保留在内存，
    //   点「结束任务」时由完结前兜底补发（flushUnreportedFullboxes）统一按 H7 上传，随后发 H8。
    //   旧「波次完成回传」（buildReportFromRecords / waveCompleteReportReady）已停用——
    //   其原先会清空 m_gridSortRecords，导致完结前无数据可补发（现场实测"直接结束不上传剩余框数据"）。
    connect(m_pWaveMgr, &WaveManager::waveReadyToReport, this,
        [this](const QString& orderCode) {
            HTTP_LOG_INFO("波次分拣完成 order=%s（未锁格的格口数据将在点「结束任务」时自动补发 H7）",
                orderCode.toLocal8Bit().data());
            emit logMessage(QString("[波次] 分拣完成 order=%1——未满箱格口将在点「结束任务」时自动补发 H7")
                .arg(orderCode));
        }, Qt::QueuedConnection);

    // ──── PLC反馈批次 → 分拣标记 + 按格口记录 ────
    // ★ 提交到 PLC 反馈接收专用线程池处理（参考WCSApp m_threadPoolPLCRecvPtr）
    //   不占用主线程，避免高并发落格反馈阻塞 UI
    //   数据流: PlcManager::flushFeedbackBatch (主线程QTimer) → plcFeedbackBusinessBatch → PLC接收池 → markSorted + SQLite
    //
    // 
    //   - 仅 SORTING 状态接受投线（FINISHED/IDLE 拒绝）
    //   - 无匹配/无绑定 → 异常口记录，不计成功数
    //   - 首件落格时自动 BOUND→SORTING
    connect(m_pPlcMgr, &PlcManager::plcFeedbackBusinessBatch, this,
        [this](const QVector<PlcFeedbackEntry>& entries) {
            if (!m_pWaveMgr) return;

            // ★ 提交到 PLC 反馈接收专用线程池（不阻塞主线程）
            if (m_pPlcRecvPool)
            {
                m_pPlcRecvPool->commitNoWait([this, entries]() {
                    AppConfig& cfg = ConfigManager::instance()->config();
                    int waveStatus = m_pWaveMgr->status();

                    for (const PlcFeedbackEntry& e : entries)
                    {
                        // ──── 仅 SORTING / FULLBOX_SYNC 接受投线 ────
                        // 非上述状态拒绝处理（BOUND 等待手动开始分拣，落格反馈暂不处理）。
                        // ★ 2026-09-07 修复：满箱同步期间（H7 网络往返窗口）其他格口的落格反馈照常计数，
                        //   此前一律拒绝导致满箱瞬间的反馈丢失、已分拣数小于实投数（mock 实测）
                        if (waveStatus != WAVE_SORTING && waveStatus != WAVE_FULLBOX_SYNC)
                        {
                            HTTP_LOG_WARN("PLC反馈被拒绝 非SORTING状态 orderCode=%s code=%s status=%d",
                                m_pWaveMgr->orderCode().toLocal8Bit().data(),
                                e.code.toLocal8Bit().data(), waveStatus);
                            continue; // 跳过本条
                        }

                        // ──── PLC 5字段反馈状态过滤（1=成功, 2=无格口, 3=信息不全）────
                        // 3字段格式无状态字段（status=0），5字段格式 status=1/2/3
                        // PLC 判定失败（2/3）的反馈不得计入成功分拣，转为异常记录
                        if (e.status == 2 || e.status == 3)
                        {
                            QString statusDesc = (e.status == 2) ? QString::fromUtf8("无格口") : QString::fromUtf8("信息不全");
                            HTTP_LOG_WARN("PLC反馈状态异常 code=%s grid=%s status=%d(%s) 不计入成功分拣",
                                e.code.toLocal8Bit().data(), e.grid.toLocal8Bit().data(),
                                e.status, statusDesc.toLocal8Bit().data());
                            if (m_pWaveMgr)
                                m_pWaveMgr->markException(e.code);
                            if (m_pSortingDb && m_pSortingDb->isOpen())
                            {
                                ExceptionRecord exRec;
                                exRec.type      = (e.status == 2) ? "plc_no_grid" : "plc_info_incomplete";
                                exRec.orderCode = m_pWaveMgr ? m_pWaveMgr->orderCode() : QString();
                                exRec.epc       = e.code;
                                exRec.sku       = m_pEpcCache ? m_pEpcCache->get(e.code) : QString();
                                exRec.reason    = QString("PLC反馈status=%1(%2) grid=%3，不计入成功分拣")
                                                    .arg(e.status).arg(statusDesc).arg(e.grid);
                                exRec.time      = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
                                m_pSortingDb->insertException(exRec);
                            }
                            continue;
                        }

                        // ──── 异常处理 ────
                        // ★ 先通过 EpcCache 将 EPC 转为 SKU，再用 SKU 查 GridBuffer
                        //   PLC 反馈的 code 是 EPC 编码，GridBuffer 的 key 是 SKU 编码
                        QString sku = m_pEpcCache ? m_pEpcCache->get(e.code) : e.code;
                        GridEntry entry;
                        if (!sku.isEmpty())
                            entry = m_pBuffer->get(sku);
                        // 如果 SKU 查找失败，回退直接用 code 查找（兼容旧逻辑）
                        if (entry.gridNum.isEmpty() && sku != e.code)
                            entry = m_pBuffer->get(e.code);
                        if (entry.gridNum.isEmpty())
                        {
                            // 识别码不在当前波次中 → 异常口
                            HTTP_LOG_WARN("PLC反馈识别码无匹配 code=%s grid=%s sku=%s 记录为异常",
                                e.code.toLocal8Bit().data(), e.grid.toLocal8Bit().data(), sku.toLocal8Bit().data());
                            if (m_pWaveMgr)
                                m_pWaveMgr->markException(e.code);
                            // 写入异常记录
                            if (m_pSortingDb)
                            {
                                ExceptionRecord exRec;
                                exRec.type      = "no_match";
                                exRec.orderCode = m_pWaveMgr->orderCode();
                                exRec.epc       = e.code;
                                exRec.sku       = "";
                                exRec.reason    = QString("格口%1：当前波次中不存在该识别码").arg(e.grid);
                                exRec.time      = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
                                m_pSortingDb->insertException(exRec);
                            }
                            continue;
                        }

                        // 校验格口是否有活跃容器绑定（T-S4-06 绑定校验）
                        {
                            std::lock_guard<std::mutex> lock(m_containerMutex);
                            if (!m_containerBindings.contains(e.grid))
                            {
                                // 格口无绑定 → 异常口
                                HTTP_LOG_WARN("PLC反馈格口无绑定 code=%s grid=%s",
                                    e.code.toLocal8Bit().data(), e.grid.toLocal8Bit().data());
                                if (m_pWaveMgr)
                                    m_pWaveMgr->markException(e.code);
                                if (m_pSortingDb)
                                {
                                    ExceptionRecord exRec;
                                    exRec.type      = "no_bind";
                                    exRec.orderCode = m_pWaveMgr->orderCode();
                                    exRec.epc       = e.code;
                                    exRec.sku       = "";
                                    exRec.reason    = QString("格口%1：格口未绑定容器").arg(e.grid);
                                    exRec.time      = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
                                    m_pSortingDb->insertException(exRec);
                                }
                                continue;
                            }
                        }

                        // ──── 同品分类vs发货冲突检测 ────
                        // 当同一商品同时存在于不同 gridType（如分类格口与发货格口）时
                        // 按配置策略处理：STRICT_EXCEPTION=入异常口, LOOSE_FIRST=取首个匹配
                        {
                            // 检查当前格口的 gridType，判断是否存在冲突
                            QString currentGridType = entry.gridType;
                            const QMap<QString, GridEntry>* pMap = m_pBuffer->activeMap();
                            if (pMap && !currentGridType.isEmpty())
                            {
                                QSet<QString> otherTypes;
                                for (auto it = pMap->constBegin(); it != pMap->constEnd(); ++it)
                                {
                                    if (it.key() == e.code && it.value().gridType != currentGridType
                                        && !it.value().gridType.isEmpty())
                                    {
                                        otherTypes.insert(it.value().gridType);
                                    }
                                }
                                if (!otherTypes.isEmpty())
                                {
                                    // 存在冲突：同一商品映射到不同类型的格口
                                    QStringList typeList = otherTypes.values();
                                    if (QString(cfg.sortingConflictPolicy) == "STRICT_EXCEPTION")
                                    {
                                        HTTP_LOG_WARN("分类vs发货冲突 code=%s grid=%s type=%s otherTypes=%s 入异常口",
                                            e.code.toLocal8Bit().data(), e.grid.toLocal8Bit().data(),
                                            currentGridType.toLocal8Bit().data(),
                                            typeList.join(",").toLocal8Bit().data());
                                        m_pWaveMgr->markException(e.code);
                                        if (m_pSortingDb)
                                        {
                                            ExceptionRecord exRec;
                                            exRec.type      = "conflict";
                                            exRec.orderCode = m_pWaveMgr->orderCode();
                                            exRec.epc       = e.code;
                                            exRec.sku       = "";
                                            exRec.reason    = QString("同品分类vs发货冲突 grid=%1 type=%2 otherTypes=%3")
                                                .arg(e.grid).arg(currentGridType).arg(typeList.join(","));
                                            exRec.time      = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
                                            m_pSortingDb->insertException(exRec);
                                        }
                                        continue;
                                    }
                                    // LOOSE_FIRST：取首个匹配，仅日志记录
                                    HTTP_LOG_INFO("分类vs发货冲突(宽松模式) code=%s grid=%s type=%s otherTypes=%s",
                                        e.code.toLocal8Bit().data(), e.grid.toLocal8Bit().data(),
                                        currentGridType.toLocal8Bit().data(),
                                        typeList.join(",").toLocal8Bit().data());
                                }
                            }
                        }

                        // ──── EPC 任务内防重（orderCode+epc）────
                        // 同一波次内同一 EPC 只计一次成功，防止重复落格双计
                        if (cfg.sortingEpcDedup)
                        {
                            if (m_pWaveMgr->isCodeSorted(e.code))
                            {
                                HTTP_LOG_WARN("EPC防重拦截 code=%s order=%s 已成功分拣，跳过",
                                    e.code.toLocal8Bit().data(),
                                    m_pWaveMgr->orderCode().toLocal8Bit().data());
                                continue;
                            }
                            // 数据库级防重：检查 sort_txn 表是否已有记录
                            if (m_pSortingDb && m_pSortingDb->isEpcAlreadySorted(m_pWaveMgr->orderCode(), e.code))
                            {
                                HTTP_LOG_WARN("EPC防重拦截(DB) code=%s order=%s 数据库已有记录，跳过",
                                    e.code.toLocal8Bit().data(),
                                    m_pWaveMgr->orderCode().toLocal8Bit().data());
                                m_pWaveMgr->markSorted(e.code); // 同步内存状态
                                continue;
                            }
                        }

                        // ──── 正常落格处理 ────
                        // ★ 2026-09-07 客户确认：不做格口计划上限限制，只如实记录落格已分拣数量。
                        //   原「格口达计划上限 拒收/入异常口」策略已移除——多SKU共格时按SKU计划数误拒
                        //   （mock 实测 6/12），且职责上只需记录已分拣数量，超不超由 WMS 计划侧把握。

                        m_pWaveMgr->markSorted(e.code);

                        // ★ 记录该格口已分拣件数（只计数，不做上限拦截）
                        {
                            bool ok = false;
                            int gn = e.grid.toInt(&ok);
                            QString gridKey = ok ? QString::number(gn) : e.grid;
                            std::lock_guard<std::mutex> lock(m_gridCountMutex);
                            m_gridSortedCount[gridKey]++;
                        }

                        // 按格口记录分拣明细（锁格/满箱时回传 WMS 用）
                        {
                            std::lock_guard<std::mutex> lock(m_gridRecordMutex);
                            GridSortRecord rec;
                            rec.inco   = e.code;                                  // EPC（链路主键）
                            rec.sku    = m_pEpcCache ? m_pEpcCache->get(e.code) : QString();  // ★ SKU（EPC→SKU 映射，落格时固化，供 WMS 报文 sku 字段）
                            rec.car    = e.car;
                            rec.timeMs = e.timestampMs;
                            // ★ 2026-09-06 件数口径修复：一物一码，每条落格记录 = 1 件。
                            //   原值 entry.gridCount（该SKU计划件数）被灌进单条记录，导致
                            //   DB 明细"件数"列与报文 qty 按计划数虚高（例: 计划12实分3 → 3行×12=36）。
                            //   计划数仅用于格口上限策略(m_gridSortedCount)与波次计划(planQty)。
                            rec.gridCount = 1;
                            rec.volu      = entry.volu.isEmpty() ? QString("--") : entry.volu;

                            m_gridSortRecords[e.grid].append(rec);

                            // 写入分拣记录到本地 SQLite 数据库
                            if (m_pSortingDb)
                            {
                                // ★ 从 EpcCache 获取 SKU 编码
                                QString sku = m_pEpcCache ? m_pEpcCache->get(e.code) : "";
                                m_pSortingDb->insertRecord(
                                    m_pWaveMgr->orderCode(),
                                    e.code, sku, e.grid, e.car,
                                    e.firstCar, e.lastCar,
                                    1 /* ★ 2026-09-07 每条落格记录=1件（与 rec.gridCount 口径一致，不再写 SKU 计划数）*/,
                                    entry.volu);
                            }
                        }
                    }

                    if (entries.size() == 1)
                    {
                        HTTP_LOG_INFO("PLC反馈自动分拣 code=%s grid=%s",
                            entries[0].code.toLocal8Bit().data(),
                            entries[0].grid.toLocal8Bit().data());
                    }
                });
            }
        });

    // ★ S7 锁格 → 满箱回传（H7 满箱同步到WMS，G2 修复）
    // PLC 锁格信号 = 满箱事件：PLC 锁定格口后，WCS 将该格口所有分拣数据组装满箱回传报文（H7）回传 WMS
    connect(m_pPlcMgr, &PlcManager::gridLocked, this,
        [this](const QString& grid) {
            // ── 锁格上下文快照 ──
            QString orderCode = m_pWaveMgr ? m_pWaveMgr->orderCode() : QString();
            int waveStatus = m_pWaveMgr ? m_pWaveMgr->status() : -1;
            int totalSorted = m_pWaveMgr ? m_pWaveMgr->sorted() : 0;
            int totalException = m_pWaveMgr ? m_pWaveMgr->exception() : 0;

            // 获取该格口的容器绑定和分拣记录数
            QString boxCode;
            int gridRecordCount = 0;
            {
                std::lock_guard<std::mutex> lock(m_containerMutex);
                boxCode = m_containerBindings.value(grid);
            }
            {
                std::lock_guard<std::mutex> lock(m_gridRecordMutex);
                gridRecordCount = m_gridSortRecords.value(grid).size();
            }

            HTTP_LOG_INFO("[锁格→满箱] PLC锁格信号 grid=%s order=%s waveStatus=%d sorted=%d exc=%d box=%s gridRecords=%d",
                grid.toLocal8Bit().data(), orderCode.toLocal8Bit().data(),
                waveStatus, totalSorted, totalException,
                boxCode.toLocal8Bit().data(), gridRecordCount);
            emit logMessage(QString("[锁格→满箱] 格口%1 order=%2 box=%3 gridRecords=%4 waveStatus=%5")
                .arg(grid).arg(orderCode).arg(boxCode).arg(gridRecordCount).arg(waveStatus), true);

            // ★ 2026-09-06 客户确认：PLC 锁格 = 满箱事件，只发 H7 满箱回传报文
            //   （WMS 网关 method=gwisSubProductClassifyOrder，报文 num=格口号/targetLocation=容器号/sku映射）。
            //   原「步骤1 锁格明细回传」（lockGrid 旧报文 num=容器号/targetLocation=格口号）已停用：
            //   ① 其字段语义与 WMS 网关 Order 接口不符（曾返回 UNKNOWN_OPERATE_ERROR）；
            //   ② 它在发送后立即清空该格分拣记录，导致紧随其后的 H7 满箱报文永远取不到明细。
            HTTP_LOG_INFO("[锁格→满箱] 满箱回传（H7） grid=%s box=%s", grid.toLocal8Bit().data(), boxCode.toLocal8Bit().data());
            sendFullbox(grid);

            // ★ 步骤3: 满箱回传后禁用格口（纠正5）
            //   该格口不再被分配、不再接收落格，直到 WMS 重新发送 H6 绑定请求
            {
                bool ok = false;
                int gNum = grid.toInt(&ok);
                if (ok && m_pPlcMgr)
                {
                    m_pPlcMgr->disableGrid(gNum);
                    HTTP_LOG_INFO("[锁格→满箱] 格口已禁用 grid=%d (等待WMS重新绑定H6)", gNum);
                    emit logMessage(QString("[格口] 禁用格口%1 (满箱锁格，等待WMS重新绑定)").arg(gNum), true);
                }
            }

            HTTP_LOG_INFO("[锁格→满箱] 完成 grid=%s order=%s", grid.toLocal8Bit().data(), orderCode.toLocal8Bit().data());
        }, Qt::QueuedConnection);

    // PLC连接状态日志（使用 QueuedConnection 确保跨线程安全）
    connect(m_pPlcMgr, &PlcManager::plcConnected, this,
        [this](const QString& ip, int port) {
            HTTP_LOG_INFO("PLC已连接 ip=%s port=%d", ip, port);
            emit logMessage(QString("[PLC] 已连接 %1:%2").arg(ip).arg(port));
        }, Qt::QueuedConnection);
    connect(m_pPlcMgr, &PlcManager::plcDisconnected, this,
        [this](const QString& ip, int port) {
            HTTP_LOG_WARN("PLC断开连接 ip=%s port=%d", ip, port);
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

    // ★ S5 新增：满箱回传 Outbox 重试调度器（H7 满箱同步）
    m_outboxFullboxTimer = new QTimer(this);
    connect(m_outboxFullboxTimer, &QTimer::timeout, this, &HttpServer::pollOutboxFullbox);
    m_outboxFullboxTimer->start(OUTBOX_POLL_INTERVAL_SEC * 1000);

    // ★ S6 完结回传 Outbox 重试调度器（H8 波次完结通知WMS，T-S6-03）
    m_outboxEndTimer = new QTimer(this);
    connect(m_outboxEndTimer, &QTimer::timeout, this, &HttpServer::pollOutboxEnd);
    m_outboxEndTimer->start(OUTBOX_POLL_INTERVAL_SEC * 1000);

    // ★ 2026-09-02 修复"结束任务卡死/闪退"：H8 完结回传会话兜底定时器（单次）
    //   sendEnd() 启动；成功/耗尽/超时任一结束点都会 stop 并发出 endReportFinished，
    //   保证 MainWindow 的等待必然结束（不卡死），服务停止后软件保持运行
    m_endSessionTimer = new QTimer(this);
    m_endSessionTimer->setSingleShot(true);
    connect(m_endSessionTimer, &QTimer::timeout, this, &HttpServer::onEndSessionTimeout);

    LogCenter::Instance()->wcs_run_log_warn(true, "[Http] HttpServer已创建");
}

HttpServer::~HttpServer()
{
    // ★ 2026-09-04 崩溃定位：析构各阶段日志——若闪退，最后一条日志即崩溃点
    LOG_INFO("[析构] HttpServer 开始销毁");
    // 析构清理说明：
    //   m_pBuffer(GridBuffer)/m_pQueue(TaskQueue) 为构造器 new 的无 QObject parent 成员，本析构函数末尾 delete 释放
    //   m_pWorker/m_pWaveMgr/m_pPlcMgr 均有 QObject parent=this，由 Qt 对象树自动释放，无需处理
    // ★ 2026-09-06：实例常驻（程序启动建一次，退出才析构）——析构=全停（接收+设备）
    stopReceive();
    LOG_INFO("[析构] step1 停止接收 完成");
    stopDevices();
    LOG_INFO("[析构] step2 设备已停止(PLC/RFID/Worker)");
    // ★ 关闭分拣记录数据库（单例；仅进程退出前析构时调用一次，无并发竞态）
    if (m_pSortingDb) {
        m_pSortingDb->close();
        m_pSortingDb = nullptr;
    }
    LOG_INFO("[析构] step3 数据库已关闭");
    // ★ 清理 EPC 缓存
    if (m_pEpcCache) {
        delete m_pEpcCache;
        m_pEpcCache = nullptr;
    }
    LOG_INFO("[析构] step4 EpcCache 已删除");
    if (m_pBusinessPool) {
        delete m_pBusinessPool;
        m_pBusinessPool = nullptr;
    }
    LOG_INFO("[析构] step5 业务线程池已删除");
    // ★ 清理专用业务线程池（PLC反馈接收池）
    if (m_pPlcRecvPool) {
        delete m_pPlcRecvPool;
        m_pPlcRecvPool = nullptr;
    }
    LOG_INFO("[析构] step6 PLC反馈池已删除");
    // ★ 清理无 QObject parent 的堆成员（m_pWorker/m_pWaveMgr/m_pPlcMgr 均有 parent=this，由 Qt 对象树自动释放）
    //   m_pBuffer 是 m_pWaveMgr/m_pWorker 的依赖，且两者已 stop/wait 不再使用，此处删除安全
    delete m_pQueue;  m_pQueue  = nullptr;
    LOG_INFO("[析构] step7 TaskQueue 已删除");
    delete m_pBuffer; m_pBuffer = nullptr;
    LOG_INFO("[析构] step8 GridBuffer 已删除，Qt子对象将自动释放");
}

// ============================================================================
// ★ 2026-09-06 设备连接与任务接收解耦：
//   startDevices() —— 设备层：程序启动即自动连接 PLC/RFID（常驻，与按钮无关）
//   startReceive()/stopReceive() —— 接收层：按钮控制是否接收 WMS 任务下发
// ============================================================================

bool HttpServer::startDevices()
{
    m_stopping.store(false);

    // ParseWorker 解析线程：常驻（仅首次启动；QThread 不可重复 start）
    if (m_pWorker && !m_pWorker->isRunning())
        m_pWorker->start();

    AppConfig& cfg = ConfigManager::instance()->config();

    // ── PLC：TCP 监听（PLC 主动连入收发分拣指令）+ S7 连接（DB77 锁格检测，断线自动重连）──
    if (m_pPlcMgr)
    {
        if (m_pPlcMgr->start("0.0.0.0", cfg.plcListenPort))
        {
            HTTP_LOG_INFO("PLC监听服务已启动 port=%d（等待PLC连接）", cfg.plcListenPort);
            emit logMessage(QString("[PLC] 监听服务已启动 port=%1").arg(cfg.plcListenPort));
        }
        else
        {
            HTTP_LOG_ERROR("PLC监听服务启动失败 port=%d", cfg.plcListenPort);
            emit logMessage(QString("[PLC] 监听服务启动失败 port=%1").arg(cfg.plcListenPort), true);
        }
        // S7 锁格通道（首次失败由心跳线程自动重连，不阻断设备层）
        QByteArray s7ip = cfg.plcS7Ip.toLocal8Bit();
        m_pPlcMgr->connectS7(s7ip.constData());
    }

    // ── RFID：TCP 客户端主动连接 RFID 服务端（断线自动重连 + 应用层心跳）──
    if (m_pRfidPush)
    {
        m_pRfidPush->setHeartbeatEnabled(cfg.rfidHeartbeatEnable != 0);
        m_pRfidPush->setHeartbeatIntervalMs(cfg.rfidHeartbeatIntervalMs);
        if (m_pRfidPush->start(cfg.rfidPushServerIp, cfg.rfidPushServerPort))
        {
            HTTP_LOG_INFO("RFID推送客户端发起连接 ip=%s port=%d",
                cfg.rfidPushServerIp.toLocal8Bit().constData(), cfg.rfidPushServerPort);
            emit logMessage(QString("[RFID] 推送客户端已发起连接 %1:%2")
                .arg(cfg.rfidPushServerIp).arg(cfg.rfidPushServerPort));
        }
        else
        {
            HTTP_LOG_ERROR("RFID推送客户端启动失败 ip=%s port=%d",
                cfg.rfidPushServerIp.toLocal8Bit().constData(), cfg.rfidPushServerPort);
            emit logMessage(QString("[RFID] 推送客户端启动失败 %1:%2")
                .arg(cfg.rfidPushServerIp).arg(cfg.rfidPushServerPort), true);
        }
    }

    HTTP_LOG_INFO("设备层启动完成（PLC/RFID 常驻；WMS 任务接收由\"开始接收任务\"控制）");
    return true;
}

bool HttpServer::startReceive(int port)
{
    m_stopping.store(false);

    // ★ 2026-09-06 接收/设备解耦：同一常驻实例可多轮「开始/结束接收」——
    //   stopReceive() 中 m_pServer.Reset() 已销毁旧 HP 对象（CHPObjectPtr 不会自动重建），
    //   本轮开始接收前必须重建，否则 operator-> 空指针崩溃
    if (!m_pServer.IsValid())
        m_pServer = HP_Create_HttpServer(this);

    m_receiving.store(true);   // ★ 先置接收标志（HTTP Start 成功后即收 WMS 推送）

    // ──── HP-Socket 性能调优（参考WCSApp线程池架构）────
    {
        m_pServer->SetWorkerThreadCount(HP_WORKER_THREADS);
        m_pServer->SetMaxConnectionCount(HP_MAX_CONNECTIONS);
        m_pServer->SetKeepAliveTime(HP_KEEPALIVE_TIME_MS);
        HTTP_INFO("HP-Socket配置: workerThreads=%d maxConn=%d keepAlive=%dms",
            HP_WORKER_THREADS, HP_MAX_CONNECTIONS, HP_KEEPALIVE_TIME_MS);
    }

    if (!m_pServer->Start(_T("0.0.0.0"), port))
    {
        m_receiving.store(false);
        LogCenter::Instance()->wcs_run_log_warn(false,
            QString("[Http] 启动失败 port=%1 err=%2").arg(port).arg((int)::GetLastError()));
        return false;
    }

    LogCenter::Instance()->wcs_run_log_warn(true,
        QString("[Http] 服务已启动 port=%1（开始接收任务）")
            .arg(port));

    // 开始接收时提示数据库中未完成波次（不恢复，人工确认）
    restoreWaveFromDB();

    // ★ 2026-09-07 若存在"执行中排队"的待执行波次，开启接收后自动执行
    if (!m_pendingWaveQueue.isEmpty())
        maybeStartPendingWave();

    emit serverStarted(port);
    return true;
}

void HttpServer::stopReceive()
{
    // ★ 防崩溃：置位停止标志——线程池任务（processRequest）与 sendJsonResponse
    //   检测到 m_stopping 后立即返回，不再触碰 HP-Socket 对象（防 Reset 后空指针）
    m_stopping.store(true);

    // ★ 停止兜底：波次明细异步落库未完成时同步补落库（数据不丢；幂等，重复调用安全）
    if (m_wavePersistPending.load() && m_pSortingDb && m_pWaveMgr)
    {
        QString orderCode = m_pWaveMgr->orderCode();
        if (!orderCode.isEmpty() && m_pWaveMgr->status() == WAVE_CREATED)
        {
            HTTP_WARN("停止接收时波次明细落库未完成，同步补落库 orderCode=%s",
                orderCode.toLocal8Bit().data());
            m_pSortingDb->upsertReturnWave(orderCode, m_pWaveMgr->orderQty(), m_pWaveMgr->status());
            QVector<ReturnWaveItemRecord> items = buildWaveItems(orderCode);
            bool bOk = false;
            for (int attempt = 0; attempt <= WAVE_PERSIST_RETRY_MAX; ++attempt)
            {
                bOk = m_pSortingDb->insertWaveItems(orderCode, items);
                if (bOk) break;
                QThread::msleep(WAVE_PERSIST_RETRY_INTERVAL_MS);
            }
            HTTP_INFO("停止兜底落库 %s orderCode=%s items=%d",
                bOk ? "成功" : "失败", orderCode.toLocal8Bit().data(), items.size());
            m_wavePersistPending.store(false);
        }
    }

    // HTTP 停止（WMS 推送入口关闭）——设备（PLC/RFID）与 Outbox 补传定时器保持
    if (m_pServer && m_pServer->HasStarted())
        m_pServer->Stop();
    m_pServer.Reset();
    m_receiving.store(false);
    emit serverStopped();
}

void HttpServer::stopDevices()
{
    // ★ 2026-09-07 防析构竞态：先断开设备 → 本对象的所有信号连接，
    //   避免 stop() 期间设备 emit（plcFeedbackBusinessBatch/plcConnected 等）打到正在析构的本对象 lambda
    if (m_pPlcMgr)   m_pPlcMgr->disconnect(this);
    if (m_pRfidPush) m_pRfidPush->disconnect(this);

    // 停止 Outbox/H8 会话定时器（进程退出时；接收停止不调用本方法）
    if (m_outboxEndTimer)     m_outboxEndTimer->stop();
    if (m_outboxFullboxTimer) m_outboxFullboxTimer->stop();
    if (m_endSessionTimer)    m_endSessionTimer->stop();
    LOG_INFO("[析构] step2a 出站定时器已停止");

    if (m_pPlcMgr)
    {
        m_pPlcMgr->disconnectS7();   // 断开 S7 + 停心跳线程/锁格轮询
        LOG_INFO("[析构] step2b S7已断开");
        m_pPlcMgr->stop();           // 停 PLC TCP 监听
        LOG_INFO("[析构] step2c PLC TCP已停止");
    }
    if (m_pRfidPush)
    {
        m_pRfidPush->stop();         // 停 RFID 客户端（含重连/心跳定时器）
        LOG_INFO("[析构] step2d RFID已停止");
    }

    if (m_pWorker && m_pWorker->isRunning())
    {
        m_pWorker->stop();
        m_pWorker->wait(WORKER_WAIT_MS);
        LOG_INFO("[析构] step2e Worker已停止 running=%d",
            m_pWorker->isRunning() ? 1 : 0);
    }
}

// ★ 启动时：① 从数据库加载活跃容器绑定到内存/UI（重启后绑定不丢）；
//          ② 检查数据库中的未完成波次（仅日志提示，不恢复到当前任务流，
//             由"未完成波次手动重传"面板查看/重传）
void HttpServer::restoreWaveFromDB()
{
    if (!m_pSortingDb || !m_pSortingDb->isOpen()) return;

    // ① 加载活跃绑定
    QVector<GridBoxBindRecord> activeBinds = m_pSortingDb->getAllActiveBinds();
    if (!activeBinds.isEmpty())
    {
        QMap<QString, QString> binds;
        for (const GridBoxBindRecord& b : activeBinds)
        {
            if (!b.gridNum.isEmpty() && !b.boxcode.isEmpty())
                binds.insert(b.gridNum, b.boxcode);
        }
        loadContainerBindings(binds);
        emit bindingUpdated();
        HTTP_LOG_INFO("启动时从数据库恢复容器绑定 count=%d", (int)binds.size());
        emit logMessage(QString("[容器绑定] 启动时已从数据库恢复 %1 个格口绑定").arg(binds.size()));
    }

    // ② 未完成波次提示
    ReturnWaveRecord wave = m_pSortingDb->getLatestUnfinishedWave();
    if (wave.orderCode.isEmpty()) return;

    // 加载波次明细
    QVector<ReturnWaveItemRecord> items = m_pSortingDb->getWaveItems(wave.orderCode);

    HTTP_LOG_INFO("检测到上一波次未完成 orderCode=%s status=%d items=%d updatedAt=%s（不恢复到当前任务流）",
        wave.orderCode.toLocal8Bit().data(), wave.status, items.size(),
        wave.updatedAt.toLocal8Bit().data());
    emit logMessage(QString::fromUtf8("[提示] 检测到上一波次未完成: orderCode=%1 状态=%2 明细数=%3 更新时间=%4（可在\"未完成波次\"面板查看/重传）")
        .arg(wave.orderCode)
        .arg(WaveSnapshot::statusToString(wave.status))
        .arg(items.size())
        .arg(wave.updatedAt));
}

// ============================================================================
// 未完成波次手动重传面板支撑接口（补充手段，不影响主流程）
// ============================================================================

QVector<ReturnWaveRecord> HttpServer::getUnfinishedWaves()
{
    if (!m_pSortingDb || !m_pSortingDb->isOpen()) return QVector<ReturnWaveRecord>();
    return m_pSortingDb->getAllUnfinishedWaves();
}

// ★ 2026-09-06 UI「波次数据记录」：全部已传输波次（含已完成/已取消）+ 进度
QVector<WaveRecordProgress> HttpServer::getAllWaves()
{
    if (!m_pSortingDb || !m_pSortingDb->isOpen()) return QVector<WaveRecordProgress>();
    return m_pSortingDb->getAllWaves();
}

QVector<OutboxRecord> HttpServer::getWaveFullboxOutbox(const QString& orderCode)
{
    if (!m_pSortingDb || !m_pSortingDb->isOpen()) return QVector<OutboxRecord>();
    return m_pSortingDb->getOutboxFullboxByOrder(orderCode);
}

QVector<OutboxRecord> HttpServer::getWaveEndOutbox(const QString& orderCode)
{
    if (!m_pSortingDb || !m_pSortingDb->isOpen()) return QVector<OutboxRecord>();
    return m_pSortingDb->getOutboxEndByOrder(orderCode);
}

// ============================================================================
// ★ 2026-09-07 清空格口容器绑定（人工重置按钮触发）：
//   ① 内存绑定清空；② DB 全部归档（active=0，历史行保留=记录在库，可追溯/可沿用）；
//   ③ 恢复满箱锁格禁用的格口（回到初始可分配状态）；④ 提示 + 日志
// ============================================================================
void HttpServer::clearAllGridBinds()
{
    int cleared = 0;
    {
        std::lock_guard<std::mutex> lock(m_containerMutex);
        cleared = m_containerBindings.size();
        m_containerBindings.clear();
    }
    if (m_pSortingDb && m_pSortingDb->isOpen())
        m_pSortingDb->archiveAllBinds();   // ★ DB 归档留史（行保留），不做物理删除
    if (m_pPlcMgr)
        m_pPlcMgr->enableAllGrids();       // 恢复禁用格口（满箱锁格后）

    HTTP_LOG_INFO("人工清空格口容器绑定 cleared=%d（DB 已归档留史，历史可追溯）", cleared);
    emit logMessage(QString("[绑定] 已清空格口容器绑定（%1 个）——格口恢复初始状态；历史记录已归档保留于数据库")
        .arg(cleared));
    emit bindingUpdated();
}

void HttpServer::resendOutbox(const QString& orderCode, bool resendH7, bool resendH8)
{
    if (!m_pSortingDb || !m_pSortingDb->isOpen() || orderCode.isEmpty()) return;

    int h7Sent = 0, h8Sent = 0;

    if (resendH7)
    {
        QVector<OutboxRecord> fullbox = m_pSortingDb->getOutboxFullboxByOrder(orderCode);
        for (const OutboxRecord& rec : fullbox)
        {
            if (rec.status == "success") continue;  // 已成功的不重发
            QJsonDocument doc = QJsonDocument::fromJson(rec.payload.toUtf8());
            if (doc.isNull() || !doc.isObject())
            {
                HTTP_LOG_WARN("未完成波次面板 H7 重传 payload 解析失败 order=%s msgId=%s",
                    orderCode.toLocal8Bit().data(), rec.msgId.toLocal8Bit().data());
                continue;
            }
            emit outboxResendReady("fullbox", doc.object(), rec.msgId);
            ++h7Sent;
        }
    }

    if (resendH8)
    {
        QVector<OutboxRecord> end = m_pSortingDb->getOutboxEndByOrder(orderCode);
        for (const OutboxRecord& rec : end)
        {
            if (rec.status == "success") continue;  // 已成功的不重发
            QJsonDocument doc = QJsonDocument::fromJson(rec.payload.toUtf8());
            if (doc.isNull() || !doc.isObject())
            {
                HTTP_LOG_WARN("未完成波次面板 H8 重传 payload 解析失败 order=%s msgId=%s",
                    orderCode.toLocal8Bit().data(), rec.msgId.toLocal8Bit().data());
                continue;
            }
            emit outboxResendReady("end", doc.object(), rec.msgId);
            ++h8Sent;
        }
    }

    HTTP_LOG_INFO("未完成波次面板 手动重传 order=%s H7=%d H8=%d",
        orderCode.toLocal8Bit().data(), h7Sent, h8Sent);
    emit logMessage(QString("[未完成波次] 手动重传 order=%1 H7=%2 H8=%3")
        .arg(orderCode).arg(h7Sent).arg(h8Sent));
}

void HttpServer::onOutboxResendReply(const QString& msgId, bool isH7, bool success)
{
    if (!m_pSortingDb || !m_pSortingDb->isOpen()) return;

    QString orderCode;
    if (isH7)
    {
        if (success)
            m_pSortingDb->markOutboxFullboxSuccess(msgId);
        OutboxRecord rec = m_pSortingDb->getOutboxFullboxByMsgId(msgId);
        orderCode = rec.orderCode;
    }
    else
    {
        if (success)
            m_pSortingDb->markOutboxEndSuccess(msgId);
        OutboxRecord rec = m_pSortingDb->getOutboxEndByMsgId(msgId);
        orderCode = rec.orderCode;
    }

    HTTP_LOG_INFO("未完成波次面板 重传结果 kind=%s msgId=%s success=%d",
        isH7 ? "H7" : "H8", msgId.toLocal8Bit().data(), success ? 1 : 0);
    emit outboxResendResult(orderCode, isH7 ? "fullbox" : "end", msgId, success);
}

// ============================================================================
// 上一波次任务恢复
// ============================================================================

QJsonObject HttpServer::getUnfinishedWaveSummary(const QString& orderCode)
{
    QJsonObject summary;
    if (!m_pSortingDb || !m_pSortingDb->isOpen() || orderCode.isEmpty())
        return summary;

    ReturnWaveRecord wave = m_pSortingDb->getReturnWave(orderCode);
    if (wave.orderCode.isEmpty())
        return summary;

    QSet<QString> sortedEpcs    = m_pSortingDb->getSortedEpcsByOrder(orderCode);
    QSet<QString> exceptionEpcs = m_pSortingDb->getExceptionEpcsByOrder(orderCode);

    QString h7Text = QString::fromUtf8("无");
    {
        QVector<OutboxRecord> fb = m_pSortingDb->getOutboxFullboxByOrder(orderCode);
        if (!fb.isEmpty())
        {
            int pend = 0, succ = 0, fail = 0;
            for (const OutboxRecord& r : fb)
            {
                if (r.status == "success") ++succ;
                else if (r.status == "failed") ++fail;
                else ++pend;
            }
            h7Text = QString("成功%1/待发%2/失败%3").arg(succ).arg(pend).arg(fail);
        }
    }
    QString h8Text = QString::fromUtf8("无");
    {
        QVector<OutboxRecord> eb = m_pSortingDb->getOutboxEndByOrder(orderCode);
        if (!eb.isEmpty())
        {
            int pend = 0, succ = 0, fail = 0;
            for (const OutboxRecord& r : eb)
            {
                if (r.status == "success") ++succ;
                else if (r.status == "failed") ++fail;
                else ++pend;
            }
            h8Text = QString("成功%1/待发%2/失败%3").arg(succ).arg(pend).arg(fail);
        }
    }

    summary["orderCode"]       = wave.orderCode;
    summary["status"]          = wave.status;
    summary["statusText"]      = WaveSnapshot::statusToString(wave.status);
    summary["orderQty"]        = wave.orderQty;
    summary["sortedCount"]     = (int)sortedEpcs.size();
    summary["exceptionCount"]  = (int)exceptionEpcs.size();
    summary["h7"]              = h7Text;
    summary["h8"]              = h8Text;
    summary["updatedAt"]       = wave.updatedAt;
    return summary;
}

bool HttpServer::resumeUnfinishedWave(const QString& orderCode)
{
    if (orderCode.isEmpty())
    {
        emit logMessage("[恢复] 请先选中一条波次", true);
        return false;
    }

    if (!m_pWaveMgr)
    {
        HTTP_LOG_WARN("恢复拒绝 WaveManager未初始化");
        return false;
    }

    int curStatus = m_pWaveMgr->status();

    // ── 同波次快路径：选中的就是当前内存中的波次（同一次运行内挂起，未重启）──
    //   内存数据（GridBuffer/进度集合）齐全，仅需按映射调整状态（如 异常挂起→已绑定 人工闭环），无需重建
    {
        QString curOrder = m_pWaveMgr->orderCode();
        if (!curOrder.isEmpty() && curOrder == orderCode)
        {
            // ★ 2026-09-06 列表全量后常见操作：选中的就是当前内存波次且已终态（已完成/已取消）——
            //   面板/查询区即其数据，无需恢复，不报错
            if (curStatus == WAVE_FINISHED || curStatus == WAVE_CANCELLED)
            {
                HTTP_LOG_INFO("恢复(同会话) 波次已终态无需恢复 order=%s status=%d(%s)",
                    orderCode.toLocal8Bit().data(), curStatus,
                    WaveSnapshot::statusToString(curStatus).toLocal8Bit().data());
                emit logMessage(QString("[恢复] 波次 %1 已完成（%2），当前面板即其数据；如需补发 H7/H8 请用「重传」按钮")
                    .arg(orderCode).arg(WaveSnapshot::statusToString(curStatus)));
                emit waveResumed(orderCode, curStatus);
                return true;
            }
            int target;
            switch (curStatus)
            {
            case WAVE_CREATED:        target = WAVE_BOUND;   break;
            case WAVE_BOUND:          target = WAVE_BOUND;   break;
            case WAVE_SORTING:        target = WAVE_SORTING; break;
            case WAVE_FULLBOX_SYNC:   target = WAVE_SORTING; break;
            case WAVE_ENDING:         target = WAVE_ENDING;  break;
            case WAVE_HELD:
            case WAVE_CANCEL_PENDING: target = WAVE_BOUND;   break;  // 人工闭环：重新开工→结束（新H8）
            default: target = -1; break;
            }
            if (target < 0)
            {
                emit logMessage(QString("[恢复] 该波次状态(%1)不支持恢复").arg(WaveSnapshot::statusToString(curStatus)), true);
                return false;
            }
            if (curStatus == target)
            {
                HTTP_LOG_INFO("恢复(同会话) 已处于目标状态 order=%s status=%d(%s)",
                    orderCode.toLocal8Bit().data(), target, WaveSnapshot::statusToString(target).toLocal8Bit().data());
                emit logMessage(QString("[恢复] 波次 %1 已处于 %2 状态，无需恢复")
                    .arg(orderCode).arg(WaveSnapshot::statusToString(target)));
                emit waveResumed(orderCode, target);
                return true;
            }
            if (!m_pWaveMgr->setState(target))
            {
                HTTP_LOG_ERROR("恢复(同会话) 状态调整失败 order=%s %d(%s)→%d(%s)",
                    orderCode.toLocal8Bit().data(), curStatus,
                    WaveSnapshot::statusToString(curStatus).toLocal8Bit().data(),
                    target, WaveSnapshot::statusToString(target).toLocal8Bit().data());
                emit logMessage(QString("[恢复] 状态调整失败 order=%1 %2→%3")
                    .arg(orderCode).arg(WaveSnapshot::statusToString(curStatus))
                    .arg(WaveSnapshot::statusToString(target)), true);
                return false;
            }
            HTTP_LOG_INFO("恢复(同会话) 成功 order=%s %d(%s)→%d(%s)",
                orderCode.toLocal8Bit().data(), curStatus,
                WaveSnapshot::statusToString(curStatus).toLocal8Bit().data(),
                target, WaveSnapshot::statusToString(target).toLocal8Bit().data());
            emit logMessage(QString("[恢复] 状态已调整 order=%1 %2 → %3（数据在内存中完整，直接继续）")
                .arg(orderCode).arg(WaveSnapshot::statusToString(curStatus))
                .arg(WaveSnapshot::statusToString(target)));
            emit waveResumed(orderCode, target);
            return true;
        }
    }

    // 校验1：★ 2026-09-06 波次状态隔离——任意状态都允许切出（当前波次状态/进度均已落 DB，
    //   切出后列表可见、切回时按 DB 状态恢复）。不再因"当前有进行中任务"拒绝切换。
    if (curStatus != WAVE_IDLE)
    {
        HTTP_LOG_INFO("切换前保存并切出当前波次 order=%s status=%d(%s)",
            m_pWaveMgr->orderCode().toLocal8Bit().data(), curStatus,
            WaveSnapshot::statusToString(curStatus).toLocal8Bit().data());
        emit logMessage(QString("[切换] 已保存并切出波次 %1（%2）——进度与数据保留，可随时切回")
            .arg(m_pWaveMgr->orderCode()).arg(WaveSnapshot::statusToString(curStatus)));
        switchAwayCurrentWave();
    }

    if (!m_pSortingDb || !m_pSortingDb->isOpen())
    {
        HTTP_LOG_WARN("恢复拒绝 数据库不可用");
        emit logMessage("[恢复] 数据库不可用，恢复失败", true);
        return false;
    }

    // 校验2：波次存在且可恢复；仅 CANCELLED/FINISHED 终态不提供恢复
    ReturnWaveRecord wave = m_pSortingDb->getReturnWave(orderCode);
    if (wave.orderCode.isEmpty())
    {
        emit logMessage(QString("[恢复] 波次不存在 order=%1").arg(orderCode), true);
        return false;
    }
    int targetStatus;
    switch (wave.status)
    {
    // ★ 已下发直接恢复到「已绑定」：波次已落库=默认已绑定（与 onWavePersistenceFinished 口径一致），
    //   恢复后即可人工点「开始分拣」（BOUND 才能激活按钮），避免卡在 CREATED 无法开工
    case WAVE_CREATED:      targetStatus = WAVE_BOUND;   break;
    case WAVE_BOUND:        targetStatus = WAVE_BOUND;   break;
    case WAVE_SORTING:      targetStatus = WAVE_SORTING; break;
    case WAVE_FULLBOX_SYNC: targetStatus = WAVE_SORTING; break;  // 满箱中断：回 SORTING，pending H7 由定时器重试
    case WAVE_ENDING:       targetStatus = WAVE_ENDING;  break;  // pending H8 由定时器重试
    // ★ 异常挂起/取消处理中：恢复到「已绑定」，人工闭环——
    //   点「开始分拣」→「结束任务」会生成新的 H8 完结回传，成功后 FINISHED（HELD 不允许直接完结）
    case WAVE_HELD:
    case WAVE_CANCEL_PENDING:
        targetStatus = WAVE_BOUND;
        break;
    // ★ 2026-09-06 已完结/已取消（历史波次）：「载入查看」——重建数据供面板/查询/重传查看，
    //   不参与分拣回传（m_bReported=true 由 restoreWave 处理）；如需重新执行请让 WMS 重新下发
    case WAVE_FINISHED:     targetStatus = WAVE_FINISHED;  break;
    case WAVE_CANCELLED:    targetStatus = WAVE_CANCELLED; break;
    default:
        HTTP_LOG_WARN("恢复拒绝 该状态不支持恢复 order=%s status=%d(%s)",
            orderCode.toLocal8Bit().data(), wave.status,
            WaveSnapshot::statusToString(wave.status).toLocal8Bit().data());
        emit logMessage(QString("[恢复] 该波次状态(%1)不支持恢复，缺的 H7/H8 请用「重传」按钮补发")
            .arg(WaveSnapshot::statusToString(wave.status)), true);
        return false;
    }

    // 步骤3：重建 SKU→格口映射（GridBuffer）
    QVector<ReturnWaveItemRecord> items = m_pSortingDb->getWaveItems(orderCode);
    // ★ 2026-09-06 降级恢复：完结中(ENDING)/终态等波次的明细可能为空（下发时落库失败/历史数据缺失），
    //   此时仍应允许切换——按 DB 恢复进度与状态，GridBuffer 置空，仅支持「补发 H7/H8 + 查看」，
    //   无法继续分拣（无格口映射）。否则仍拒绝。
    bool allowEmptyItems = (wave.status == WAVE_ENDING || wave.status == WAVE_FINISHED ||
                            wave.status == WAVE_CANCELLED || wave.status == WAVE_HELD ||
                            wave.status == WAVE_CANCEL_PENDING);
    if (items.isEmpty() && !allowEmptyItems)
    {
        HTTP_LOG_WARN("恢复拒绝 波次明细为空 order=%s status=%d",
            orderCode.toLocal8Bit().data(), wave.status);
        emit logMessage(QString("[恢复] 波次明细为空，无法恢复 order=%1（分拣明细缺失，无法继续分拣）")
            .arg(orderCode), true);
        return false;
    }

    auto* newMap = new QMap<QString, GridEntry>();
    QSet<QString> recvSet;
    for (const ReturnWaveItemRecord& it : items)
    {
        recvSet.insert(it.inco);
        if (newMap->contains(it.inco))
        {
            GridEntry& e = (*newMap)[it.inco];
            QStringList grids = e.gridNum.split(',', Qt::SkipEmptyParts);
            if (!grids.contains(it.gridNum))
                e.gridNum = e.gridNum.isEmpty() ? it.gridNum : e.gridNum + "," + it.gridNum;
        }
        else
        {
            GridEntry entry;
            entry.gridNum   = it.gridNum;
            entry.gridType  = it.gridType.isEmpty() ? "0" : it.gridType;
            entry.gridCount = it.planQty;
            entry.volu      = it.volu;
            entry.obxCode   = it.obxCode;
            entry.orderCode = orderCode;
            entry.orderQty  = wave.orderQty;
            entry.skuCount  = 0;
            newMap->insert(it.inco, entry);
        }
    }
    if (items.isEmpty())
    {
        // 降级：明细缺失，仍替换为空映射（防残留上个波次映射），并提示不可继续分拣
        HTTP_LOG_WARN("切换降级恢复 order=%s 明细为空，仅支持补发H7/H8与查看，不可继续分拣", orderCode.toLocal8Bit().data());
        emit logMessage(QString("[切换] 波次 %1 明细缺失——已降级载入（仅支持补发H7/H8与查看，无法继续分拣）")
            .arg(orderCode), true);
    }
    int skuCount = newMap->size();
    for (auto it = newMap->begin(); it != newMap->end(); ++it)
        it.value().skuCount = skuCount;

    // ★ 恢复时 ParseWorker 空闲（无排队任务），主线程 prepareSwap 安全
    m_pBuffer->prepareSwap(newMap);

    // 步骤4：重建进度集合
    QSet<QString> sortedEpcs    = m_pSortingDb->getSortedEpcsByOrder(orderCode);
    QSet<QString> exceptionEpcs = m_pSortingDb->getExceptionEpcsByOrder(orderCode);
    bool hasFullbox = m_pSortingDb->hasSuccessFullbox(orderCode);

    // 步骤5：恢复 WaveManager
    if (!m_pWaveMgr->restoreWave(orderCode, wave.orderQty, skuCount, targetStatus,
                                 recvSet, sortedEpcs, exceptionEpcs, hasFullbox))
    {
        HTTP_LOG_ERROR("恢复失败 restoreWave 返回false order=%s", orderCode.toLocal8Bit().data());
        emit logMessage(QString("[恢复] 恢复失败 order=%1").arg(orderCode), true);
        return false;
    }

    // 步骤6：★ 2026-09-07 绑定不与波次死绑——切换不改写内存绑定（面板恒显示物理当前绑定/沿用结果）；
    //   本波次的历史绑定记录在 DB（grid_box_bind.order_code）供追溯，此处仅提示数量
    {
        QMap<QString, QString> binds;
        if (m_pSortingDb)
            binds = m_pSortingDb->getBindsByOrder(orderCode);
        {
            std::lock_guard<std::mutex> lock(m_containerMutex);
            HTTP_LOG_INFO("切换波次 order=%s：历史绑定记录 %d 个（物理当前绑定 %d 个保持不变，DB 历史可追溯）",
                orderCode.toLocal8Bit().data(), binds.size(), m_containerBindings.size());
        }
        if (binds.isEmpty())
        {
            emit logMessage(QString("[切换] 波次 %1 无历史绑定记录（当时未绑定/旧数据），当前沿用物理绑定显示")
                .arg(orderCode));
        }
        else
        {
            emit logMessage(QString("[切换] 波次 %1 历史绑定 %2 个（保留于数据库）；当前面板显示物理绑定，未做覆盖")
                .arg(orderCode).arg(binds.size()));
        }
    }
    emit bindingUpdated();

    HTTP_LOG_INFO("恢复波次成功 order=%s target=%d(%s) qty=%d SKU=%d sorted=%d exc=%d fullbox=%d",
        orderCode.toLocal8Bit().data(), targetStatus,
        WaveSnapshot::statusToString(targetStatus).toLocal8Bit().data(),
        wave.orderQty, skuCount, (int)sortedEpcs.size(), (int)exceptionEpcs.size(), hasFullbox ? 1 : 0);
    if (targetStatus == WAVE_FINISHED || targetStatus == WAVE_CANCELLED)
    {
        emit logMessage(QString("[恢复] 已载入历史波次（查看模式） order=%1 状态=%2 已分拣=%3 异常=%4\n"
                                "不参与分拣/回传；补发请用「重传H7/H8」，重新执行请让 WMS 重新下发覆盖")
            .arg(orderCode).arg(WaveSnapshot::statusToString(targetStatus))
            .arg(sortedEpcs.size()).arg(exceptionEpcs.size()));
    }
    else
    {
        emit logMessage(QString("[恢复] 已进入上次任务 order=%1 状态=%2 已分拣=%3 异常=%4")
            .arg(orderCode).arg(WaveSnapshot::statusToString(targetStatus))
            .arg(sortedEpcs.size()).arg(exceptionEpcs.size()));
    }

    // ★ 2026-09-06 切回波次自动补发：把该波次未成功的 H7/H8（pending/failed/cancelled）补发一轮，
    //   恢复到切出前的回传进度（查看模式终态波次不自动补发，避免重复打扰）
    if (targetStatus != WAVE_FINISHED && targetStatus != WAVE_CANCELLED)
    {
        resendOutbox(orderCode, true, true);
    }

    // ★ 2026-09-07 人工切换到待执行队列中的波次 → 从队列移除，防止自动重复执行
    for (int i = 0; i < m_pendingWaveQueue.size(); ++i)
    {
        if (m_pendingWaveQueue[i].orderCode == orderCode)
        {
            HTTP_LOG_INFO("人工切换已接管待执行波次 orderCode=%s（从队列移除）", orderCode.toLocal8Bit().data());
            m_pendingWaveQueue.removeAt(i);
            break;
        }
    }

    emit waveResumed(orderCode, targetStatus);
    return true;
}

// ============================================================================
// ★ 2026-09-06 挂起切出当前波次（供「切换波次」与「新任务」共用）：
//   清空内存（WaveManager/格口映射/格口运行数据），状态与进度保留于 DB；
//   未成功的 H7/H8 报文保留 outbox（切回该波次时自动补发 / 可手动重传）
// ============================================================================
void HttpServer::switchAwayCurrentWave()
{
    if (!m_pWaveMgr) return;

    // 1. 清空 WaveManager（集合/计数/m_bReported/状态→IDLE；DB 状态保留原值，列表可见可切回）
    m_pWaveMgr->clearWave();

    // 2. 清空 SKU→格口映射（新波次下发/切换回来时会重新构建）
    auto* emptyMap = new QMap<QString, GridEntry>();
    m_pBuffer->prepareSwap(emptyMap);

    // 3. 清空按格口运行数据（分拣记录/计数），防残留影响其它波次
    {
        std::lock_guard<std::mutex> lock(m_gridRecordMutex);
        m_gridSortRecords.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_gridCountMutex);
        m_gridSortedCount.clear();
    }
    m_pendingSkuQuery.clear();
    m_skuQueryRetryCount.clear();
    m_notReadyRetryCount.clear();
    m_sentEpcs.clear();
    // ★ 满箱锁格禁用的格口保持禁用（物理状态未变），该波次重新落库/恢复时统一处理
}

// ============================================================================
// ★ 2026-09-06 新任务：当前波次进度/数据保留于 DB（可切换回来继续），
//   内存清空回到空闲（IDLE + 空 SKU 映射），等待 WMS 下发新波次。
//   说明：不落库状态（DB 保留切出前状态=列表显示）；H7/H8 后台补发定时器保持运行
//   （pollOutbox* 只重试当前内存波次的消息，切出波次的未成功报文在切换回时自动补发/可手动重传）
// ============================================================================
bool HttpServer::startNewWaveTask()
{
    if (!m_pWaveMgr)
    {
        HTTP_LOG_WARN("新任务失败 WaveManager未初始化");
        emit logMessage("[新任务] 失败: WaveManager未初始化", true);
        return false;
    }

    QString oldOrder = m_pWaveMgr->orderCode();
    int oldStatus    = m_pWaveMgr->status();
    if (oldOrder.isEmpty() && oldStatus == WAVE_IDLE)
    {
        HTTP_LOG_INFO("新任务：当前本就空闲（无波次）");
    }
    else
    {
        if (oldStatus == WAVE_ENDING || oldStatus == WAVE_FULLBOX_SYNC)
        {
            HTTP_LOG_INFO("新任务：当前波次处于 %s，仍将安全切出（未成功 H7/H8 保留，切换回时自动补发）",
                WaveSnapshot::statusToString(oldStatus).toLocal8Bit().data());
        }
        switchAwayCurrentWave();
        HTTP_LOG_INFO("新任务执行完成 old=%s status=%d(%s) -> 空闲（数据已保留于DB，可切换回）",
            oldOrder.toLocal8Bit().data(), oldStatus,
            WaveSnapshot::statusToString(oldStatus).toLocal8Bit().data());
        emit logMessage(QString("[新任务] 已保存并切出波次 %1（%2）——数据保留，可从「波次数据记录」切换回来")
            .arg(oldOrder).arg(WaveSnapshot::statusToString(oldStatus)));
    }

    // ★ 2026-09-07 新任务：绑定保持（内存/DB 均不清）——下一波次无新 H6 时默认沿用；
    //   恢复满箱禁用格口（物理已处理或 WMS 将重新绑定）、刷新绑定面板
    if (m_pPlcMgr)
        m_pPlcMgr->enableAllGrids();
    {
        std::lock_guard<std::mutex> lock(m_containerMutex);
        HTTP_LOG_INFO("新任务 格口已恢复，容器绑定保留 %d 个（供下一波次沿用，WMS 新 H6 会覆盖）",
            m_containerBindings.size());
    }
    emit bindingUpdated();

    // ★ 2026-09-07 若有待执行波次（执行中接收到的 H4），自动开始执行
    if (!m_pendingWaveQueue.isEmpty())
    {
        emit logMessage(QString("[波次] 检测到 %1 个待执行波次，自动开始执行...").arg(m_pendingWaveQueue.size()));
        maybeStartPendingWave();
    }
    else
    {
        emit logMessage("[新任务] 已回到空闲：等待 WMS 下发新波次；旧波次可随时从「波次数据记录」切换回来");
    }

    emit waveResumed(QString(), WAVE_IDLE);
    return true;
}

// ============================================================================
// ★ 2026-09-07 自动开始下一波次：从待执行队列取队首重放给 ParseWorker 解析注册。
//   仅当内存空闲（IDLE/终态）时执行；解析完成回到 waveParsed（重放标志放行接收闸门）。
// ============================================================================
void HttpServer::maybeStartPendingWave()
{
    if (m_pendingWaveQueue.isEmpty()) return;
    if (!m_pWaveMgr || !m_pQueue) return;

    int st = m_pWaveMgr->status();
    if (!(st == WAVE_IDLE || st == WAVE_CANCELLED || st == WAVE_FINISHED))
        return;   // 当前波次仍进行中，不插队

    // 队首重放（push 成功才出队；水位满则下次触发再试）
    const PendingWave& pw = m_pendingWaveQueue.first();
    WaveTask task;
    task.rawBody  = pw.rawBody;
    task.fullUrl  = pw.fullUrl.isEmpty() ? QString("queue://replay") : pw.fullUrl;
    task.recvTime = pw.recvTime;
    if (!m_pQueue->push(task))
    {
        HTTP_LOG_WARN("待执行波次入队失败(队列满) orderCode=%s，稍后自动重试", pw.orderCode.toLocal8Bit().data());
        return;
    }
    m_replayingPending.store(true);
    m_pendingWaveQueue.removeFirst();
    HTTP_LOG_INFO("自动开始待执行波次 orderCode=%s 队列剩余=%d", pw.orderCode.toLocal8Bit().data(),
        m_pendingWaveQueue.size());
    emit logMessage(QString("[波次] 自动开始下一波次 %1（待执行剩 %2 个）——解析完成后自动进入该任务")
        .arg(pw.orderCode).arg(m_pendingWaveQueue.size()));
}

// ============================================================================
// ★ 2026-09-07 绑定沿用：新波次注册后，若当前无任何 active 绑定（如上一波次完结已归档清空），
//   自动从 DB 取每格最近一次绑定恢复为 active（即"沿用上一波次的绑定"），
//   WMS 下发新 H6 时对应用户正常覆盖。
// ============================================================================
void HttpServer::restoreBindsIfEmpty(const QString& orderCode)
{
    if (!m_pSortingDb || !m_pSortingDb->isOpen() || orderCode.isEmpty()) return;
    {
        std::lock_guard<std::mutex> lock(m_containerMutex);
        if (!m_containerBindings.isEmpty()) return;   // 已有绑定（含沿用/新 H6）则不动
    }
    QMap<QString, QString> last = m_pSortingDb->getLastKnownBinds();
    if (last.isEmpty()) return;   // 从未绑定过（首次使用），保持空由 WMS 新 H6 绑定

    for (auto it = last.constBegin(); it != last.constEnd(); ++it)
        m_pSortingDb->bindGridBox(it.key(), it.value(), orderCode);   // 恢复 active（历史行保留）
    {
        std::lock_guard<std::mutex> lock(m_containerMutex);
        m_containerBindings = last;
    }
    emit bindingUpdated();
    HTTP_LOG_INFO("沿用最近绑定 orderCode=%s binds=%d（WMS 未下发新 H6，默认沿用上一波次绑定）",
        orderCode.toLocal8Bit().data(), last.size());
    emit logMessage(QString("[绑定] 波次 %1 无新绑定下发——已沿用最近绑定 %2 个；WMS 下发新 H6 时自动覆盖")
        .arg(orderCode).arg(last.size()));
}

// ★ 构建波次明细记录（从 GridBuffer 读取全量 SKU→格口映射，供落库复用）
//   2026-09-04 P0修复：主线程构建（万级循环仅几十ms）+ 业务线程池异步落库
QVector<ReturnWaveItemRecord> HttpServer::buildWaveItems(const QString& orderCode)
{
    QVector<ReturnWaveItemRecord> items;
    const QMap<QString, GridEntry>* pMap = m_pBuffer ? m_pBuffer->activeMap() : nullptr;
    if (pMap)
    {
        for (auto it = pMap->constBegin(); it != pMap->constEnd(); ++it)
        {
            ReturnWaveItemRecord rec;
            rec.orderCode = orderCode;
            rec.inco      = it.key();  // inco=SKU编码
            // gridNum 可能为 "1,2,3"（一品多格口），拆分为多条
            QStringList grids = it.value().gridNum.split(',', Qt::SkipEmptyParts);
            for (const QString& g : grids)
            {
                rec.gridNum   = g.trimmed();
                rec.gridType  = it.value().gridType.isEmpty() ? "0" : it.value().gridType;  // 0=分类, 1=异常, 2=发货
                rec.planQty   = it.value().gridCount;
                rec.volu      = it.value().volu;
                rec.obxCode   = it.value().obxCode;     // 容器号
                items.append(rec);
            }
        }
    }
    return items;
}

// ★ 2026-09-04 P0修复：波次明细异步落库完成回调（主线程）——推进 BOUND + 清理旧波次状态
//   由 wavePersistenceFinished 信号触发（业务线程池 emit → AutoConnection 回主线程）
void HttpServer::onWavePersistenceFinished(const QString& orderCode, bool ok, int skuCount)
{
    m_wavePersistPending.store(false);

    // ★ 停止流程中到达的落库完成信号：stop() 已同步补落库并即将销毁，跳过状态推进
    if (m_stopping.load())
    {
        HTTP_INFO("停止中，跳过波次状态推进 orderCode=%s ok=%d", orderCode.toLocal8Bit().data(), ok ? 1 : 0);
        return;
    }

    if (ok)
    {
        HTTP_LOG_INFO("波次落库完成(异步) orderCode=%s skuCount=%d", orderCode.toLocal8Bit().data(), skuCount);
        // ★ 数据库写入完成后，默认已绑定，直接推进到 BOUND 状态（仅允许 CREATED→BOUND）
        if (m_pWaveMgr && m_pWaveMgr->status() == WAVE_CREATED)
        {
            m_pWaveMgr->setState(WAVE_BOUND);
            HTTP_LOG_INFO("波次自动推进 CREATED→BOUND orderCode=%s (数据库已落库，默认已绑定，等待手动开始分拣)",
                orderCode.toLocal8Bit().data());
            emit logMessage(QString("[波次] 自动推进: 已下发→已绑定 orderCode=%1 (等待手动开始分拣)").arg(orderCode));

            // ★ 2026-09-07 绑定沿用：无任何 active 绑定（如上一波次完结已归档）时，恢复最近绑定
            restoreBindsIfEmpty(orderCode);
        }
    }
    else
    {
        // ★ 落库失败：保持 CREATED，不推进（防止无明细的波次进入分拣流程）
        HTTP_ERROR("波次落库失败 保持CREATED orderCode=%s (明细重试已耗尽，已写异常表)", orderCode.toLocal8Bit().data());
        emit logMessage(QString("[异常] 波次落库失败 orderCode=%1，保持未绑定状态，请人工检查异常表").arg(orderCode), true);
    }

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
    // ★ S7 新波次到来，清空格口分拣计数
    {
        std::lock_guard<std::mutex> lock(m_gridCountMutex);
        m_gridSortedCount.clear();
    }
    // ★ 新波次到来，清空旧波次相关数据（EpcCache 保留，RFID 推送独立于波次生命周期）
    // ★ 纠正5: 新波次开始时恢复所有禁用格口
    if (m_pPlcMgr)
        m_pPlcMgr->enableAllGrids();
    // ★ 新波次开始时清空 SKU 查询防重标记和重试计数
    m_pendingSkuQuery.clear();
    m_skuQueryRetryCount.clear();
    m_notReadyRetryCount.clear();
    m_sentEpcs.clear();
}

// ============================================================================
// CHttpServerListener 回调
// ============================================================================

EnHttpParseResult HttpServer::OnRequestLine(IHttpServer* pSender, CONNID dwConnID,
                                             LPCSTR lpszMethod, LPCSTR lpszUrl)
{
    std::lock_guard<std::mutex> lock(m_connMutex);
    ConnState& st = m_connStates[dwConnID];
    st.body.clear();  // ★ 新请求到来时清空 body，防止 keep-alive 连接下 body 拼接
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

    // ★ 2026-09-02 防崩溃：停止标志已置位（服务停止中）→ 直接放弃处理，
    //   防止在 m_pServer.Reset() 后继续使用 pSender（悬垂指针）
    if (m_stopping.load())
        return;

    // ★ 2026-09-06 接收闸门：未"开始接收任务"时拒绝所有请求（按钮控制任务下发）
    if (!m_receiving.load())
    {
        QJsonObject err;
        err["code"] = "500";
        err["message"] = "未开始接收任务，请先在界面点击\"开始接收任务\"";
        err["sentTime"] = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.000");
        sendJsonResponse(pSender, dwConnID, err, 500);
        HTTP_LOG_WARN("未接收任务，拒绝请求 path=%s method=%s", st.path.toLocal8Bit().data(), st.method.toLocal8Bit().data());
        return;
    }

    // ★ 服务未运行时拒绝所有 WMS 请求（防御性检查）
    if (!isRunning())
    {
        QJsonObject err;
        err["code"] = "500";
        err["message"] = "服务未启动，请先点击按钮启动服务";
        err["sentTime"] = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.000");
        sendJsonResponse(pSender, dwConnID, err, 500);
        HTTP_LOG_WARN("服务未运行，拒绝请求 path=%s method=%s", st.path.toLocal8Bit().data(), st.method.toLocal8Bit().data());
        return;
    }

    int64_t reqNum = m_requestCount.fetch_add(1) + 1;

    // ★ 记录请求来源 IP:端口（RFID/WMS 推送的客户端地址，便于核对来源，如 192.168.100.125:2010）
    QString remoteAddr;
    {
        TCHAR szAddr[64] = {0};
        int iAddrLen = 64;
        USHORT usPort = 0;
        if (pSender->GetRemoteAddress(dwConnID, szAddr, iAddrLen, usPort))
            remoteAddr = QString::fromWCharArray(szAddr) + ":" + QString::number(usPort);
    }
    if (!remoteAddr.isEmpty())
    {
        HTTP_INFO("[请求来源] %s  path=%s  req#=%lld",
            remoteAddr.toLocal8Bit().data(), st.path.toLocal8Bit().data(), reqNum);
    }

    // ★ 原始报文：重建 WMS 发送的完整 URL 字符串
    {
        QString fullUrl = "http://" + st.rawHost + st.path;
        if (!st.queryString.isEmpty())
            fullUrl += "?" + st.queryString;

        if (st.body.isEmpty())
        {
            HTTP_INFO("[原始报文] %s  req#=%lld", fullUrl.toLocal8Bit().data(), reqNum);
            LOG_INFO("[原始报文] %s  req#=%lld", fullUrl.toLocal8Bit().data(), reqNum);
        }
        else
        {
#if DEBUG_LOG_FULL_BODY
            // ★ 调试模式：打印完整请求体（不截断）
            HTTP_INFO("[原始报文] %s  body=%s(%d字节)  req#=%lld",
                fullUrl.toLocal8Bit().data(),
                st.body.constData(),
                st.body.size(), reqNum);
            LOG_INFO("[原始报文] %s  body=%s(%d字节)  req#=%lld",
                fullUrl.toLocal8Bit().data(),
                st.body.constData(),
                st.body.size(), reqNum);
#else
            // Body 截断显示
            QByteArray bodyPreview = st.body.left(RAW_REQ_BODY_LOG_LEN);
            bool truncated = st.body.size() > RAW_REQ_BODY_LOG_LEN;
            HTTP_INFO("[原始报文] %s  body=%s%s(%d字节)  req#=%lld",
                fullUrl.toLocal8Bit().data(),
                bodyPreview.constData(),
                truncated ? "..." : "",
                st.body.size(), reqNum);
            LOG_INFO("[原始报文] %s  body=%s%s(%d字节)  req#=%lld",
                fullUrl.toLocal8Bit().data(),
                bodyPreview.constData(),
                truncated ? "..." : "",
                st.body.size(), reqNum);
#endif
        }
    }
    emit logMessage(QString("[请求] %1 %2").arg(st.method).arg(st.path));

    // ═══════════════════════════════════════════════════════════════════════
    // 路由1: WMS波次数据推送（P0核心接口）
    // 调用方: WMS系统
    // 报文: POST /api/DispatchSortingCommand/InsertWaveInfo
    // 功能: 推送波次数据，包含EPC编码-格口映射，WCS解析后存储
    // 需求: §5 波次下发（H4 InsertWaveInfo，T-S1-01~T-S1-06）
    // ═══════════════════════════════════════════════════════════════════════
    if (st.path == m_apiInsertWaveInfo && st.method == "POST")
    {
        // 步骤1: 解析 JSON — 剥离 BOM 头（UTF-8 BOM: EF BB BF）
        //   部分工具导出的 JSON 文件带 BOM，QJsonDocument 可能无法正确解析
        QByteArray cleanBody = st.body;
        if (cleanBody.startsWith("\xEF\xBB\xBF"))
        {
            cleanBody.remove(0, 3);
            HTTP_INFO("InsertWaveInfo 检测到UTF-8 BOM，已剥离");
        }
        // 移除 null 字节（trimmed() 不处理 \x00，用 char 重载避免 strlen("\x00")==0 的 no-op bug）
        cleanBody.replace('\x00', "");
        // 去除首尾空白字符（防止 curl 传参时引入的换行符等）
        cleanBody = cleanBody.trimmed();

        QJsonParseError parseErr;
        QJsonDocument doc = QJsonDocument::fromJson(cleanBody, &parseErr);
        if (doc.isNull() || !doc.isObject())
        {
            // ★ 增强诊断：输出原始 body 前 200 字节的十六进制，便于排查编码问题
            QByteArray hexPreview = cleanBody.left(200).toHex(' ');
            HTTP_WARN("InsertWaveInfo JSON解析失败: %s offset=%d bodySize=%d hex=[%s]",
                parseErr.errorString().toLocal8Bit().data(),
                parseErr.offset, cleanBody.size(), hexPreview.constData());
            emit logMessage(QString("[WMS] JSON解析失败: %1 (offset=%2, size=%3)")
                .arg(parseErr.errorString()).arg(parseErr.offset).arg(cleanBody.size()), true);
            QJsonObject err;
            err["code"] = "500";
            err["message"] = QString("JSON解析失败: %1 (offset=%2)")
                .arg(parseErr.errorString()).arg(parseErr.offset);
            sendJsonResponse(pSender, dwConnID, err, 500);
            return;
        }

        QJsonObject root = doc.object();
        QString orderCode = root["orderCode"].toString().trimmed();

        // 步骤2: 参数校验（T-S1-02）
        QJsonObject validateErr = validateInsertWaveInfo(root);
        if (!validateErr.isEmpty())
        {
            HTTP_WARN("InsertWaveInfo 参数校验失败 orderCode=%s msg=%s",
                orderCode.toLocal8Bit().data(),
                validateErr["message"].toString().toLocal8Bit().data());
            emit logMessage(QString("[WMS] 参数校验失败: %1").arg(validateErr["message"].toString()), true);
            sendJsonResponse(pSender, dwConnID, validateErr, 500);
            return;
        }

        // 步骤2.5: 容器格口绑定不再在开头阻塞，仅保留格口号越界判断
        // 步骤3: 幂等检查（T-S1-04）— 已分拣拒绝覆盖，未分拣允许覆盖
        if (m_pSortingDb && !orderCode.isEmpty())
        {
            int existingStatus = m_pSortingDb->getWaveStatus(orderCode);
            if (existingStatus >= 0)
            {
                // 已存在波次，检查是否已开始分拣
                if (m_pWaveMgr && m_pWaveMgr->orderCode() == orderCode && m_pWaveMgr->isSortingStarted())
                {
                    HTTP_WARN("InsertWaveInfo 幂等拒绝 波次已分拣 orderCode=%s status=%d",
                        orderCode.toLocal8Bit().data(), existingStatus);
                    emit logMessage(QString("[WMS] 波次已分拣，拒绝覆盖 orderCode=%1").arg(orderCode), true);
                    QJsonObject err;
                    err["code"] = "500";
                    err["message"] = "波次已开始分拣，不允许覆盖";
                    err["orderCode"] = orderCode;
                    sendJsonResponse(pSender, dwConnID, err, 500);
                    return;
                }
                // 未分拣，允许覆盖：先清理旧数据
                HTTP_INFO("InsertWaveInfo 覆盖旧波次 orderCode=%s oldStatus=%d",
                    orderCode.toLocal8Bit().data(), existingStatus);
                emit logMessage(QString("[WMS] 覆盖旧波次 orderCode=%1（未分拣）").arg(orderCode));
            }
        }

        // 步骤5: 检查队列容量
        if (m_pQueue->size() >= MAX_TASK_QUEUE)
        {
            HTTP_WARN("InsertWaveInfo 队列已满 orderCode=%s queue=%d", orderCode.toLocal8Bit().data(), m_pQueue->size());
            emit logMessage("[WMS] 队列已满，拒绝入队 返回503", true);
            QJsonObject err;
            err["code"] = "500";
            err["message"] = "服务器繁忙，请稍后重试";
            err["orderCode"] = orderCode;
            sendJsonResponse(pSender, dwConnID, err, 500);
            return;
        }

        // 步骤6: 入队 + 返回成功（T-S1-05 响应格式）
        {
            WaveTask task;
            task.rawBody  = st.body;
            task.fullUrl  = "http://" + st.rawHost + st.path;  // ★ 完整 URL 传给 ParseWorker 用于明细日志
            task.recvTime = QDateTime::currentMSecsSinceEpoch();
            m_pQueue->push(task);

            // ★ 需求7增强：H4 原始报文单独落库（wave_raw 表，异步写，不影响响应）
            if (m_pSortingDb && !orderCode.isEmpty())
            {
                QByteArray rawBody = st.body;  // 拷贝，连接状态可能被后续请求复用
                if (m_pBusinessPool)
                    m_pBusinessPool->commitNoWait([this, orderCode, rawBody]() {
                        m_pSortingDb->saveWaveRawPayload(orderCode, rawBody);
                    });
                else
                    m_pSortingDb->saveWaveRawPayload(orderCode, rawBody);
            }

            QJsonObject resp;
            resp["code"]      = "200";
            resp["message"]   = "successed.";
            resp["sentTime"]  = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
            sendJsonResponse(pSender, dwConnID, resp, 200);

            // 审计日志（T-S1-06）
            int itemsCount = root["items"].toArray().size();
            int orderQty = 0;
            QJsonValue qv = root["orderQty"];
            orderQty = qv.isString() ? qv.toString().toInt() : qv.toInt();
            HTTP_INFO("InsertWaveInfo 入队成功 orderCode=%s items=%d orderQty=%d queue=%d elapsed=%lldms",
                orderCode.toLocal8Bit().data(), itemsCount, orderQty, m_pQueue->size(), reqTimer.elapsed());
            emit logMessage(QString("[WMS] InsertWaveInfo 入队 orderCode=%1 items=%2 orderQty=%3 queue=%4")
                .arg(orderCode).arg(itemsCount).arg(orderQty).arg(m_pQueue->size()));
        }
        return;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 路由2: 格口容器绑定
    // 调用方: WMS系统
    // 报文: POST /api/DispatchSortingCommand/BindingLatticePort?latticehole=格口号&boxcode=容器号
    // 功能: 绑定容器号与格口的对应关系，用于后续装箱数据同步
    // ═══════════════════════════════════════════════════════════════════════
    if (st.path == m_apiBindingLatticePort && st.method == "POST")
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

        // ★ 异步持久化到数据库（HTTP 响应已返回，不阻塞 WMS 等待）
        //   bindGridBox 内部有 QMutexLocker 保护，线程安全
        if (m_pSortingDb && !latticehole.isEmpty() && !boxcode.isEmpty())
        {
            bool ok = false;
            int gridNum = latticehole.toInt(&ok);
            if (ok && gridNum >= 1 && gridNum <= BINDING_SLOT_COUNT)
            {
                QString normalizedGrid = QString("%1").arg(gridNum, GRID_KEY_PADDING, 10, QChar('0'));
                // ★ 2026-09-06 绑定关联所属波次（提交时刻快照），供波次切换恢复格口绑定视图
                QString bindOrderCode = m_pWaveMgr ? m_pWaveMgr->orderCode() : QString();
                HTTP_LOG_INFO("BindingLatticePort 提交数据库写入 grid=%s box=%s order=%s",
                    normalizedGrid.toLocal8Bit().data(), boxcode.toLocal8Bit().data(),
                    bindOrderCode.toLocal8Bit().data());

                if (m_pBusinessPool)
                {
                    m_pBusinessPool->commitNoWait([this, normalizedGrid, boxcode, bindOrderCode]() {
                        HTTP_LOG_INFO("BindingLatticePort 数据库写入开始 grid=%s box=%s order=%s",
                            normalizedGrid.toLocal8Bit().data(), boxcode.toLocal8Bit().data(), bindOrderCode.toLocal8Bit().data());
                        bool ok = m_pSortingDb->bindGridBox(normalizedGrid, boxcode, bindOrderCode);
                        if (ok)
                            HTTP_LOG_INFO("BindingLatticePort 数据库写入成功 grid=%s box=%s", normalizedGrid.toLocal8Bit().data(), boxcode.toLocal8Bit().data());
                        else
                            HTTP_LOG_ERROR("BindingLatticePort 数据库写入失败 grid=%s box=%s (详见DataBase日志)", normalizedGrid.toLocal8Bit().data(), boxcode.toLocal8Bit().data());
                    });
                }
                else
                {
                    // 降级：线程池未就绪时同步写入（此时已在业务线程池中，不阻塞 HP-Socket I/O 线程）
                    HTTP_LOG_WARN("BindingLatticePort 线程池未就绪，降级同步写入 grid=%s box=%s", normalizedGrid.toLocal8Bit().data(), boxcode.toLocal8Bit().data());
                    bool ok = m_pSortingDb->bindGridBox(normalizedGrid, boxcode, bindOrderCode);
                    if (ok)
                        HTTP_LOG_INFO("BindingLatticePort 同步写入成功 grid=%s box=%s", normalizedGrid.toLocal8Bit().data(), boxcode.toLocal8Bit().data());
                    else
                        HTTP_LOG_ERROR("BindingLatticePort 同步写入失败 grid=%s box=%s", normalizedGrid.toLocal8Bit().data(), boxcode.toLocal8Bit().data());
                }
            }
            else
            {
                HTTP_LOG_ERROR("BindingLatticePort 数据库写入跳过 格口号无效 latticehole=%s", latticehole.toLocal8Bit().data());
            }
        }
        else
        {
            if (!m_pSortingDb)
                HTTP_LOG_WARN("BindingLatticePort 数据库写入跳过 m_pSortingDb为空");
            else
                HTTP_LOG_WARN("BindingLatticePort 数据库写入跳过 参数为空 latticehole=%s boxcode=%s", latticehole.toLocal8Bit().data(), boxcode.toLocal8Bit().data());
        }

        HTTP_LOG_INFO("BindingLatticePort latticehole=%s boxcode=%s elapsed=%lldms",
            latticehole.toLocal8Bit().data(), boxcode.toLocal8Bit().data(), reqTimer.elapsed());
        return;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 路由3: 波次取消（退货任务取消）
    // 调用方: WMS系统
    // 报文: POST /api/DispatchSortingCommand/InsertWaveIn
    // 功能: WMS 下发取消指令，清除当前波次数据
    // ═══════════════════════════════════════════════════════════════════════
    if (st.path == m_apiInsertWaveIn && st.method == "POST")
    {
        QJsonDocument d = QJsonDocument::fromJson(st.body);
        QJsonObject result = handleCancelWave(d.object());
        sendJsonResponse(pSender, dwConnID, result);
        HTTP_INFO("InsertWaveIn 退货取消 body=%d elapsed=%lldms", st.body.size(), reqTimer.elapsed());
        emit logMessage(QString("[WMS] InsertWaveIn 退货取消 elapsed=%1ms").arg(reqTimer.elapsed()));
        return;
    }

    // ★ 2026-09-04：原「路由4 RFID 小车号推送（HTTP /api/rfid/carNumReport）」已废弃删除
    //   RFID 推送改由 RfidPushClient 作为 TCP 客户端主动连接 RFID 服务端（rfidPushServerIp:rfidPushServerPort）接收，
    //   处理逻辑仍复用 handleRfidCarNumReport()（经 rfidPushReceived 信号 QueuedConnection 调用）

    // 未知路径
    HTTP_WARN("未知路径 %s elapsed=%lldms", st.path.toLocal8Bit().data(), reqTimer.elapsed());
    sendJsonResponse(pSender, dwConnID, errResponse("Not Found", "500"), 500);
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
    // ★ 2026-09-02 防崩溃：停止中不发送响应（pSender 可能已随 m_pServer.Reset() 失效）
    if (m_stopping.load() || !pSender || !m_pServer || !m_pServer->HasStarted())
        return;

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
    r["message"] = msg + " successed.";         // 响应信息（成功时为空）
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
// handleBindingLatticePort — 格口容器绑定（S2 完整实现）
// WMS 下发容器与格口的绑定关系，用于后续装箱数据同步
// 参数来源: JSON Body {"boxcode":"fDD03","latticehole":"00001"} 或 queryString
// 需求: §7 容器绑定（H6 BindingLatticePort，T-S2-01~T-S2-06）
// ============================================================================
QJsonObject HttpServer::handleBindingLatticePort(const QString& latticehole, const QString& boxcode)
{
    // ──── 步骤1: 参数校验（T-S2-01）────
    if (boxcode.isEmpty() || latticehole.isEmpty())
    {
        HTTP_LOG_WARN("BindingLatticePort 参数缺失 boxcode=%s latticehole=%s",
            boxcode.toLocal8Bit().data(), latticehole.toLocal8Bit().data());
        emit logMessage(QString("[容器绑定] 参数缺失 boxcode=%1 latticehole=%2")
            .arg(boxcode).arg(latticehole), true);
        QJsonObject r;
        r["code"] = "500";
        r["message"] = QString("参数缺失: 未识别到latticehole和boxcode，请检查格式.");
        return r;
    }

    // ──── 步骤2: 格口号范围校验 ────
    bool ok = false;
    int gridNum = latticehole.toInt(&ok);
    if (!ok || gridNum < 1 || gridNum > BINDING_SLOT_COUNT)
    {
        HTTP_LOG_WARN("BindingLatticePort 格口号越界 latticehole=%s range=1..%d",
            latticehole.toLocal8Bit().data(), BINDING_SLOT_COUNT);
        emit logMessage(QString("[容器绑定] 格口号越界 latticehole=%1 (有效范围: 1~%2)")
            .arg(latticehole).arg(BINDING_SLOT_COUNT), true);
        QJsonObject r;
        r["code"] = "500";
        r["message"] = QString("格口号越界，有效范围: 1~%1").arg(BINDING_SLOT_COUNT);
        return r;
    }

    // ──── 步骤3: 获取当前活跃波次（T-S2-04 格口反查活跃任务）────
    QString orderCode;
    int waveStatus = -1;
    if (m_pWaveMgr)
    {
        orderCode = m_pWaveMgr->orderCode();
        waveStatus = m_pWaveMgr->status();
    }

    // ★ 统一格口号为 GRID_KEY_PADDING 位补零格式（兼容 WMS 传入 "12" 或 "012"）
    //   UI 面板、容器绑定校验、数据库均使用补零后的 key，避免格式不一致导致查找失败
    QString normalizedGrid = QString("%1").arg(gridNum, GRID_KEY_PADDING, 10, QChar('0'));

    // ──── 步骤3.1: 允许无波次时预绑定（IDLE 状态属于预绑定阶段）────
    // ★ 修改（需求）：容器绑定不因工作流状态（含 CANCELLED/FINISHED/HELD 终态）而停止，
    //   只要服务已启动即可接收并执行绑定

    // ──── 步骤4: 格口不在当前任务中（T-S2-03）────
    // ★ 已移除格口-任务校验：容器绑定与波次下发互不依赖，不分先后
    //   绑定可先于波次下发（预绑定），波次下发也可先于绑定
    //   格口号范围校验（步骤2）已确保格口在有效范围 1~66 内
    // if (!orderCode.isEmpty() && m_pBuffer)
    // {
    //     bool gridExists = false;
    //     const QMap<QString, GridEntry>* pMap = m_pBuffer->activeMap();
    //     if (pMap)
    //     {
    //         for (auto it = pMap->constBegin(); it != pMap->constEnd(); ++it)
    //         {
    //             QStringList grids = it.value().gridNum.split(',', Qt::SkipEmptyParts);
    //             for (const QString& g : grids)
    //             {
    //                 bool ok = false;
    //                 int gInt = g.trimmed().toInt(&ok);
    //                 if (ok && gInt == gridNum)
    //                 {
    //                     gridExists = true;
    //                     break;
    //                 }
    //             }
    //             if (gridExists) break;
    //         }
    //     }
    //     if (!gridExists)
    //     {
    //         HTTP_LOG_WARN("BindingLatticePort 格口不在当前任务中 latticehole=%s orderCode=%s",
    //             latticehole.toLocal8Bit().data(), orderCode.toLocal8Bit().data());
    //         emit logMessage(QString("[容器绑定] 格口%1 不在当前波次任务中 orderCode=%2")
    //             .arg(latticehole).arg(orderCode), true);
    //         QJsonObject r;
    //         r["code"] = "500";
    //         r["message"] = QString("格口%1不在当前波次任务中").arg(latticehole);
    //         return r;
    //     }
    // }

    // ──── 步骤6: 执行绑定（T-S2-02 归档旧绑定 + 写新绑定）────
    {
        std::lock_guard<std::mutex> lock(m_containerMutex);

        // ★ 重复绑定：旧容器归档到数据库（切箱操作）
        auto it = m_containerBindings.find(normalizedGrid);
        if (it != m_containerBindings.end() && it.value() != boxcode)
        {
            HTTP_LOG_INFO("BindingLatticePort 切箱 latticehole=%s old=%s new=%s",
                normalizedGrid.toLocal8Bit().data(),
                it.value().toLocal8Bit().data(),
                boxcode.toLocal8Bit().data());
            emit logMessage(QString("[容器绑定] 格口%1 切箱: %2 → %3")
                .arg(normalizedGrid).arg(it.value()).arg(boxcode));
        }

        m_containerBindings[normalizedGrid] = boxcode;
    }

    // ★ 纠正5: H6 绑定时恢复格口（满箱锁格后WMS重新绑定，格口恢复正常分拣）
    if (m_pPlcMgr)
    {
        m_pPlcMgr->enableGrid(gridNum);
        HTTP_LOG_INFO("H6绑定 格口恢复 grid=%d box=%s", gridNum, boxcode.toLocal8Bit().data());
    }

    // ★ 状态迁移：CREATED → BOUND（需求 §3.2，TC-RT-02）
    // 首次有效绑定将波次从"已下发"推进到"已绑定"
    if (m_pWaveMgr && waveStatus == WAVE_CREATED)
    {
        m_pWaveMgr->setState(WAVE_BOUND);
        HTTP_LOG_INFO("波次状态迁移 CREATED→BOUND orderCode=%s", orderCode.toLocal8Bit().data());
        emit logMessage(QString("[波次] 状态迁移: 已下发→已绑定 orderCode=%1").arg(orderCode));
    }

    HTTP_LOG_INFO("容器绑定成功 latticehole=%s → boxcode=%s orderCode=%s total=%d/%d",
        normalizedGrid.toLocal8Bit().data(), boxcode.toLocal8Bit().data(),
        orderCode.toLocal8Bit().data(), boundCount(), BINDING_SLOT_COUNT);
    emit logMessage(QString("[容器绑定] 格口%1 → 容器%2  (共%3/%4)")
        .arg(normalizedGrid).arg(boxcode).arg(boundCount()).arg(BINDING_SLOT_COUNT));
    emit bindingUpdated();  // ★ 通知 UI 即时刷新

    QJsonObject r;
    r["code"] = "200";
    r["message"] = "收到信息";
    return r;
}

// ============================================================================
// handleCancelWave — 波次取消（退货任务取消，需求 §6）
//
// 流程（T-S3-01~T-S3-05）：
//   步骤1: 参数校验（orderCode 非空）
//   步骤2: 波次匹配校验（不存在 → 404）
//   步骤3: 终态幂等（CANCELLED/FINISHED → 200，无需重复取消）
//   步骤4: 已分拣判定（isSortingStarted → 400，拒绝取消）
//   步骤5: 原子状态迁移（setState CANCELLED，与开工互斥）
//   步骤6: 清理数据（解绑容器、清分拣记录、更新DB状态）
//   步骤7: 返回成功（cancellable=true）
// ============================================================================
QJsonObject HttpServer::handleCancelWave(const QJsonObject& req)
{
    QString orderCode    = req["orderCode"].toString().trimmed();
    QString cancelReason = req["cancelReason"].toString().trimmed();

    // ──── 步骤1: 参数校验（T-S3-01）────
    if (orderCode.isEmpty())
    {
        HTTP_LOG_WARN("CancelWave 缺少orderCode");
        emit logMessage("[取消波次] 缺少orderCode", true);
        QJsonObject r;
        r["code"] = "500";
        r["message"] = "缺少orderCode";
        r["cancellable"] = false;
        return r;
    }

    // ──── 步骤2: 波次匹配校验（T-S3-05 不存在）────
    if (!m_pWaveMgr || m_pWaveMgr->orderCode() != orderCode)
    {
        // 检查数据库中是否已终态（幂等：已取消/已完成的波次再次取消返回成功）
        if (m_pSortingDb)
        {
            int dbStatus = m_pSortingDb->getWaveStatus(orderCode);
            if (dbStatus == WAVE_CANCELLED || dbStatus == WAVE_FINISHED)
            {
                HTTP_LOG_INFO("CancelWave 波次已终态（幂等） orderCode=%s dbStatus=%d",
                    orderCode.toLocal8Bit().data(), dbStatus);
                QJsonObject r;
                r["code"] = "200";
                r["message"] = "波次已终态，无需重复取消";
                r["cancellable"] = true;
                return r;
            }
        }
        HTTP_LOG_WARN("CancelWave 波次不匹配 req=%s current=%s",
            orderCode.toLocal8Bit().data(),
            m_pWaveMgr ? m_pWaveMgr->orderCode().toLocal8Bit().data() : "null");
        emit logMessage(QString("[取消波次] 波次不匹配或不存在 req=%1").arg(orderCode), true);
        QJsonObject r;
        r["code"] = "500";
        r["message"] = "波次不存在或已完结";
        r["cancellable"] = false;
        return r;
    }

    int currentStatus = m_pWaveMgr->status();

    // ──── 步骤3: 终态幂等（T-S3-05）────
    // 已取消/已完成的波次，返回成功，不重复清理
    if (currentStatus == WAVE_CANCELLED || currentStatus == WAVE_FINISHED)
    {
        HTTP_LOG_INFO("CancelWave 波次已终态（幂等） orderCode=%s status=%d",
            orderCode.toLocal8Bit().data(), currentStatus);
        QJsonObject r;
        r["code"] = "200";
        r["message"] = "波次已终态，无需重复取消";
        r["cancellable"] = true;
        return r;
    }

    // ──── 步骤4: 已分拣判定 + 原子状态迁移（T-S8-06 互斥锁）────
    // 使用 tryCancelWave() 加锁后判定 isSortingStarted + 原子迁移 CANCELLED
    // 与 startSorting() 使用同一把锁，保证取消与首件分拣互斥
    if (!m_pWaveMgr->tryCancelWave())
    {
        int currentStatus = m_pWaveMgr->status();
        QByteArray statusText = WaveSnapshot::statusToString(currentStatus).toLocal8Bit();
        HTTP_LOG_WARN("CancelWave 取消失败 orderCode=%s status=%d(%s) sorted=%d",
            orderCode.toLocal8Bit().data(), currentStatus, statusText.data(),
            m_pWaveMgr->sorted());
        emit logMessage(QString("[取消波次] 取消失败 orderCode=%1 status=%2 sorted=%3")
            .arg(orderCode).arg(QString::fromLocal8Bit(statusText)).arg(m_pWaveMgr->sorted()), true);
        QJsonObject r;
        r["code"] = "500";
        r["message"] = QString("波次已开始分拣(status=%1 sorted=%2)，不允许取消")
            .arg(QString::fromLocal8Bit(statusText)).arg(m_pWaveMgr->sorted());
        r["cancellable"] = false;
        return r;
    }

    // ──── 步骤5: 取消后清理（T-S3-02）────
    // tryCancelWave() 已原子完成状态迁移，此处执行清理操作
    // 清理容器绑定（内存清空 + DB 归档留史，历史行保留可追溯/可沿用）、分拣记录、GridBuffer、WaveManager

    // 清理容器绑定（内存 + 数据库归档：★ 2026-09-07 清空当前绑定但历史记录保留在 DB）
    {
        std::lock_guard<std::mutex> lock(m_containerMutex);
        int count = m_containerBindings.size();
        m_containerBindings.clear();
        HTTP_LOG_INFO("CancelWave 已清空容器绑定(内存) count=%d orderCode=%s（DB历史已归档留档）",
            count, orderCode.toLocal8Bit().data());
    }
    if (m_pSortingDb)
        m_pSortingDb->archiveAllBinds();
    emit bindingUpdated();

    // 清理格口分拣记录
    {
        std::lock_guard<std::mutex> lock(m_gridRecordMutex);
        m_gridSortRecords.clear();
    }

    // ★ 清理 GridBuffer（双缓冲置空），避免 H6 绑定校验旧波次数据
    if (m_pBuffer)
    {
        m_pBuffer->prepareSwap(nullptr);
        HTTP_LOG_INFO("CancelWave 已清理GridBuffer orderCode=%s", orderCode.toLocal8Bit().data());
    }

    // ★ 重置 WaveManager 为 IDLE（含清空 orderCode），使 H4/H6 不再校验旧波次
    if (m_pWaveMgr)
    {
        m_pWaveMgr->clearWave();
        HTTP_LOG_INFO("CancelWave 已重置WaveManager orderCode=%s", orderCode.toLocal8Bit().data());
    }

    // 更新数据库状态为 CANCELLED
    if (m_pSortingDb)
    {
        m_pSortingDb->updateWaveStatus(orderCode, WAVE_CANCELLED);
    }

    // 审计日志
    HTTP_LOG_INFO("CancelWave 波次已取消 orderCode=%s reason=%s status=%d→%d",
        orderCode.toLocal8Bit().data(),
        cancelReason.isEmpty() ? "无" : cancelReason.toLocal8Bit().data(),
        currentStatus, WAVE_CANCELLED);
    emit logMessage(QString("[取消波次] 波次已取消 orderCode=%1 reason=%2")
        .arg(orderCode).arg(cancelReason.isEmpty() ? "无" : cancelReason));

    // ──── 步骤7: 返回成功 ────
    QJsonObject r;
    r["code"] = "200";
    r["message"] = "取消成功";
    r["cancellable"] = true;
    return r;
}

// ============================================================================
// validateInsertWaveInfo — 波次下发参数校验（H4，T-S1-02）
// 校验 orderCode/orderQty/items 及各明细字段
// 返回空对象表示校验通过，非空对象包含错误信息
// ============================================================================
QJsonObject HttpServer::validateInsertWaveInfo(const QJsonObject& root)
{
    AppConfig& cfg = ConfigManager::instance()->config();

    // 校验 orderCode（§5.3.1：非空，唯一键）
    QString orderCode = root["orderCode"].toString().trimmed();
    if (orderCode.isEmpty())
    {
        QJsonObject err;
        err["code"] = "500";
        err["message"] = "缺少必填字段: orderCode";
        return err;
    }

    // 校验 orderQty（§5.3.1：>0）
    QJsonValue qv = root["orderQty"];
    int orderQty = qv.isString() ? qv.toString().toInt() : qv.toInt();
    if (orderQty <= 0)
    {
        QJsonObject err;
        err["code"] = "500";
        err["message"] = QString("orderQty 必须大于0，当前值: %1").arg(orderQty);
        err["orderCode"] = orderCode;
        return err;
    }

    // 校验 items（§5.3.1：至少 1 条）
    QJsonArray items = root["items"].toArray();
    if (items.isEmpty())
    {
        QJsonObject err;
        err["code"] = "500";
        err["message"] = "items 不能为空，至少需要 1 条明细";
        err["orderCode"] = orderCode;
        return err;
    }

    // 校验每条明细（§5.3.2）
    for (int i = 0; i < items.size(); ++i)
    {
        QJsonObject item = items[i].toObject();
        QString inco     = item["inco"].toString().trimmed();
        QString gridNum  = item["gridNum"].toString().trimmed();
        QString gridType = item["gridType"].toString().trimmed();

        if (inco.isEmpty())
        {
            QJsonObject err;
            err["code"] = "500";
            err["message"] = QString("items[%1] 缺少必填字段: inco").arg(i);
            err["orderCode"] = orderCode;
            return err;
        }
        if (gridNum.isEmpty())
        {
            QJsonObject err;
            err["code"] = "500";
            err["message"] = QString("items[%1] 缺少必填字段: gridNum").arg(i);
            err["orderCode"] = orderCode;
            return err;
        }
        // ★ gridType 已改为非必填，空值时自动兜底为"0"（分类），客户使用 0=分类, 1=异常, 2=发货
        // if (gridType.isEmpty())
        // {
        //     QJsonObject err;
        //     err["code"] = "500";
        //     err["message"] = QString("items[%1] 缺少必填字段: gridType").arg(i);
        //     err["orderCode"] = orderCode;
        //     return err;
        // }

        // ★ 格口号越界检测（仅告警，不拒绝波次 — 异常 item 由 ParseWorker 跳过并记录异常表）
        {
            bool ok = false;
            int gNum = gridNum.toInt(&ok);
            if (ok && (gNum < 1 || gNum > BINDING_SLOT_COUNT))
            {
                HTTP_WARN("InsertWaveInfo 格口号越界 orderCode=%s items[%d] gridNum=%s range=1..%d",
                    orderCode.toLocal8Bit().data(), i, gridNum.toLocal8Bit().data(), BINDING_SLOT_COUNT);
                emit logMessage(QString("[WMS] 格口号越界 items[%1] gridNum=%2 (有效范围: 1~%3)，已记录异常")
                    .arg(i).arg(gridNum).arg(BINDING_SLOT_COUNT), true);
            }
        }

        // gridNumber > 0
        QJsonValue gnv = item["gridNumber"];
        int gridNumber = gnv.isString() ? gnv.toString().toInt() : gnv.toInt();
        if (gridNumber <= 0)
        {
            QJsonObject err;
            err["code"] = "500";
            err["message"] = QString("items[%1] gridNumber 必须大于0，当前值: %2").arg(i).arg(gridNumber);
            err["orderCode"] = orderCode;
            return err;
        }
    }

    // ──── T-S7-07: orderQty 与明细合计校验（RT-H4-08）────
    // 严格模式：orderQty != ΣgridNumber 时拒绝
    // 宽松模式：仅告警，继续接收
    if (cfg.sortingOrderQtyValidate == "strict")
    {
        int totalGridNumber = 0;
        for (int i = 0; i < items.size(); ++i)
        {
            QJsonObject item = items[i].toObject();
            QJsonValue gnv = item["gridNumber"];
            int gridNumber = gnv.isString() ? gnv.toString().toInt() : gnv.toInt();
            totalGridNumber += gridNumber;
        }
        if (orderQty != totalGridNumber)
        {
            QJsonObject err;
            err["code"] = "500";
            err["message"] = QString("orderQty(%1)与明细合计(%2)不一致").arg(orderQty).arg(totalGridNumber);
            err["orderCode"] = orderCode;
            return err;
        }
    }

    // 校验通过
    return QJsonObject();
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
    int     epcCacheSize = m_pEpcCache ? m_pEpcCache->size() : 0;
    int     plcRecvTask = m_pPlcRecvPool ? m_pPlcRecvPool->taskCount() : 0;

    HTTP_INFO("健康检查 accept=%lld close=%lld active=%d requests=%lld queue=%d bizPool=%d/%d tasks=%d plcRecvTasks=%d epcCache=%d",
        accept, close, active, request, queueSize, poolIdl, poolThr, poolTask, plcRecvTask, epcCacheSize);

    // ★ 2026-09-04 P2观测：EpcCache 膨胀检测（300s TTL 懒清理，理论容量=推送速率×300s，超阈值预警）
    if (epcCacheSize > EPC_CACHE_ALERT_THRESHOLD)
    {
        HTTP_WARN("EpcCache 条目数偏高 epcCache=%d (>%d)，检查 RFID 推送速率", epcCacheSize, EPC_CACHE_ALERT_THRESHOLD);
    }
    // ★ 2026-09-04 P2观测：PLC 接收池队列堆积检测（DB 写入阻塞反馈处理时先暴露于此）
    if (plcRecvTask > (m_pPlcRecvPool ? m_pPlcRecvPool->thrCount() : 4) * POOL_OVERLOAD_MULTIPLIER)
    {
        HTTP_WARN("PLC接收池队列堆积 plcRecvTasks=%d", plcRecvTask);
    }

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
// ★ 2026-09-06 已停用：锁格时刻统一改发 H7 满箱回传报文（sendFullbox），
//   本函数（lockGrid 旧报文：num=容器号/targetLocation=格口号/sku原码）不再被调用，
//   仅保留备用（若 WMS 要求恢复旧报文可在此恢复调用）。
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
        // ★ 2026-09-06：本函数已停用。若恢复，sku 必须取 rec.sku（SKU编码），不可用 rec.inco（EPC码）
        item["sku"]            = rec.sku.isEmpty() ? getSkuByEpc(rec.inco) : rec.sku;
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
            // ★ 2026-09-06 客户确认：sku 必须是 SKU 编码（rec.sku 落格时固化；兜底查缓存），
            //   不能把 rec.inco（EPC 码）当 sku 发送
            {
                QString sku = rec.sku.isEmpty() ? getSkuByEpc(rec.inco) : rec.sku;
                if (sku.isEmpty())
                    HTTP_WARN("波次完成回传: EPC未映射到SKU epc=%s grid=%s（sku留空，WMS可能拒绝）",
                        rec.inco.toLocal8Bit().data(), grid.toLocal8Bit().data());
                item["sku"]            = sku;
            }
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
// S5 新增：满箱回传（H7 满箱同步到WMS，需求 §10，T-S5-01~T-S5-07）
// ============================================================================

// ============================================================================
// buildFullboxPayload — 构建满箱回传报文（H7 gwisSubProductClassifyOrder，T-S5-02/03）
// 报文格式（仅含 WMS 所需字段，不发送多余字段）：
//   head:
//     orderCode      = 波次下发编号（H4）
//     orderType      = 固定 "02"（WMS_ORDER_TYPE）
//     warehouseCode  = 配置项（默认 "A"）
//     goodsOwner     = 配置项（默认 "BXH_ZS"）
//     fromLocation   = 任务下发时的 sobi 字段值（H4 items[].sobi，如 "H-01-AB"）
//     createDate     = 当前时间
//     createUserCode = 固定 "admin"
//     createUserName = 固定 "管理员"
//   detailList（★ 2026-09-06 按 SKU 聚合，一行一 SKU）:
//     num            = 格口号
//     targetLocation = 目标库位编码（格口绑定的容器号）
//     sku            = SKU 编码（EPC→SKU 绑定查询结果，落格时固化；不能是 EPC 码/占位串）
//     qty            = 该容器(格口)落入该 SKU 的实际件数（Σqty=该格实分件数）
// ============================================================================
QJsonObject HttpServer::buildFullboxPayload(const QString& orderCode, const QString& grid,
                                              const QString& boxCode, const QVector<GridSortRecord>& records)
{
    AppConfig& cfg = ConfigManager::instance()->config();

    // ── fromLocation 取值：WMS 下发 items[].sobi（来源库位，旧协议 volu 兼容已由解析层统一）──
    QString fromLocation;
    for (const GridSortRecord& rec : records)
    {
        if (!rec.volu.isEmpty() && rec.volu != "--")
        {
            fromLocation = rec.volu;
            break;
        }
    }
    if (fromLocation.isEmpty())
        fromLocation = FULLBOX_DEFAULT_FROM_LOCATION;

    QJsonObject head;
    head["orderCode"]      = orderCode;
    head["orderType"]      = WMS_ORDER_TYPE;
    head["warehouseCode"]  = cfg.warehouseCode;
    head["goodsOwner"]     = cfg.goodsOwner;
    head["fromLocation"]   = fromLocation;
    head["createDate"]     = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.000");
    head["createUserCode"] = WMS_OPERUSER_CODE;
    head["createUserName"] = QString::fromUtf8(WMS_OPERUSER_NAME);
    // head["remark"]         = "";
    // head["gwf1"]           = "";  // 备用字段

    // ── detailList 构建 ──
    // ★ 2026-09-06 客户确认口径：qty = 该容器(格口)落入该 SKU 的实际件数
    //   → 同一格口内按 SKU 聚合（每条落格记录=1件），不再逐 EPC 行塞计划数
    QJsonArray detailList;
    QMap<QString, int> skuQtyMap;      // sku → 该格实分件数
    QStringList skuOrder;              // 保持出现顺序
    for (const GridSortRecord& rec : records)
    {
        // ★ 2026-09-06 客户确认：sku 字段必须是 SKU 编码（EPC→SKU 绑定查询结果，落格时已固化 rec.sku），
        //   不能是 EPC 码、也不能发"未找到sku"占位串；缺 SKU 的情形已在 sendFullbox 前置整单拦截
        QString sku = rec.sku.isEmpty() ? getSkuByEpc(rec.inco) : rec.sku;
        if (sku.isEmpty())
            continue;   // 防御：前置已整单拦截，正常不会走到
        if (!skuQtyMap.contains(sku))
            skuOrder.append(sku);
        skuQtyMap[sku] += rec.gridCount;   // 每条落格记录=1件
    }
    for (const QString& sku : skuOrder)
    {
        QJsonObject item;
        item["num"]            = grid;                              // 格口号
        item["targetLocation"] = boxCode.isEmpty() ? cfg.fullboxDefaultTargetLocation : boxCode;  // 目标库位 = 容器号，无容器号时兜底
        item["sku"]            = sku;
        item["qty"]            = QString::number(skuQtyMap.value(sku));
        detailList.append(item);
    }
    head["detailList"] = detailList;

    // ★ 2026-09-06：满箱总件数日志（Σqty 应=该格实分件数，供对账）
    {
        int totalQty = 0;
        for (auto it = skuQtyMap.constBegin(); it != skuQtyMap.constEnd(); ++it)
            totalQty += it.value();
        HTTP_LOG_INFO("满箱回传报文（H7） Σqty=%d 聚合sku行=%d 原始落格记录=%d grid=%s",
            totalQty, skuQtyMap.size(), records.size(), grid.toLocal8Bit().data());
    }

    QJsonObject report;
    report["head"] = head;

    HTTP_LOG_INFO("满箱回传报文构建（H7） order=%s grid=%s box=%s items=%d fromLocation=%s",
        orderCode.toLocal8Bit().data(), grid.toLocal8Bit().data(),
        boxCode.toLocal8Bit().data(), records.size(),
        fromLocation.toLocal8Bit().data());

    return report;
}

// ============================================================================
// sendFullbox — 满箱触发入口（T-S5-01）
// 流程：
//   1. 校验波次状态为 SORTING
//   2. 状态迁移 SORTING→FULLBOX_SYNC
//   3. 锁容器明细（获取当前容器绑定和分拣记录）
//   4. 构建满箱回传报文（H7）
//   5. 生成 msgId（UUID 幂等键）
//   6. 插入 Outbox 出站表
//   7. 发送到 WMS
// ============================================================================
void HttpServer::sendFullbox(const QString& grid)
{
    QElapsedTimer funcTimer;
    funcTimer.start();
    qint64 epochMs = QDateTime::currentMSecsSinceEpoch();

    if (!m_pWaveMgr)
    {
        HTTP_LOG_WARN("满箱回传触发失败（H7） WaveManager未初始化");
        emit logMessage("[满箱回传] 触发失败: WaveManager未初始化", true);
        return;
    }

    // ── 步骤1-2: 触发满箱状态迁移 ──
    if (!m_pWaveMgr->triggerFullbox())
    {
        HTTP_LOG_WARN("满箱回传触发失败（H7） 状态迁移拒绝 grid=%s status=%d",
            grid.toLocal8Bit().data(), m_pWaveMgr->status());
        emit logMessage(QString("[满箱回传] 触发失败: 状态迁移拒绝 grid=%1 status=%2")
            .arg(grid).arg(m_pWaveMgr->status()), true);
        return;
    }

    // ── 步骤3: 获取容器绑定（内存快速路径 → 数据库现查兜底）──
    QString boxCode = lookupGridBoxCode(grid);

    if (boxCode.isEmpty())
    {
        HTTP_LOG_WARN("满箱回传触发失败（H7） 格口 %s 无容器绑定", grid.toLocal8Bit().data());
        emit logMessage(QString("[满箱回传] 触发失败: 格口%1 无容器绑定").arg(grid), true);
        m_pWaveMgr->resumeSorting();  // 回退状态
        return;
    }

    // ── 步骤3: 获取分拣记录 ──
    QString orderCode = m_pWaveMgr->orderCode();
    QVector<GridSortRecord> records;
    {
        std::lock_guard<std::mutex> lock(m_gridRecordMutex);
        auto it = m_gridSortRecords.find(grid);
        if (it != m_gridSortRecords.end())
            records = it.value();
    }

    if (records.isEmpty())
    {
        HTTP_LOG_WARN("满箱回传触发（H7） grid=%s 无分拣记录 跳过", grid.toLocal8Bit().data());
        emit logMessage(QString("[满箱回传] 格口%1 无分拣记录，跳过").arg(grid));
        m_pWaveMgr->resumeSorting();  // 回退状态
        return;
    }

    // ★ 2026-09-06 客户确认：报文 sku 字段必须是 SKU 编码——不能是 EPC 码，也不能发"未找到sku"占位串。
    //   SKU 在落格时已固化（rec.sku = EPC→SKU 绑定查询结果），此处兜底再查一次 EpcCache；
    //   仍有缺失 → 拒绝整单发送（WMS 会因非法 sku 拒绝整个报文），保留该格记录，
    //   回退分拣状态并写异常表，待 RFID 绑定恢复后人工重锁格重发
    {
        QStringList missingSkuEpcs;
        for (auto it = records.begin(); it != records.end(); ++it)
        {
            if (it->sku.isEmpty())
                it->sku = getSkuByEpc(it->inco);   // 兜底：缓存中可能刚写入
            if (it->sku.isEmpty())
                missingSkuEpcs << it->inco;
        }
        if (!missingSkuEpcs.isEmpty())
        {
            HTTP_LOG_ERROR("满箱回传（H7）中止: %d 条EPC未映射到SKU grid=%s box=%s 保留记录待重试 epc=[%s]",
                missingSkuEpcs.size(), grid.toLocal8Bit().data(), boxCode.toLocal8Bit().data(),
                missingSkuEpcs.join(",").left(400).toLocal8Bit().data());
            emit logMessage(QString("[满箱回传] 中止: 格口%1 有 %2 条未查到 SKU（不发报文，已保留记录；"
                                    "请核查 RFID 绑定查询后重锁该格重发）")
                .arg(grid).arg(missingSkuEpcs.size()), true);
            if (m_pSortingDb && m_pSortingDb->isOpen())
            {
                ExceptionRecord ex;
                ex.type      = QString::fromUtf8("满箱SKU缺失");
                ex.orderCode = orderCode;
                ex.epc       = missingSkuEpcs.first();
                ex.sku       = "";
                ex.reason    = QString("格口%1 满箱时 %2 条EPC未查到SKU: [%3]（RFID绑定查询缺失）")
                                   .arg(grid).arg(missingSkuEpcs.size())
                                   .arg(missingSkuEpcs.join(",").left(400));
                ex.time      = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
                m_pSortingDb->insertException(ex);
            }
            m_pWaveMgr->resumeSorting();  // 回退 FULLBOX_SYNC → SORTING
            return;
        }
    }

    qint64 t1 = funcTimer.elapsed();  // ★ 耗时：前置校验

    // ── 步骤4: 构建满箱回传报文（H7）──
    QJsonObject payload = buildFullboxPayload(orderCode, grid, boxCode, records);
    qint64 t2 = funcTimer.elapsed();  // ★ 耗时：payload构建

    // ── 步骤5-6: 生成 msgId（UUID 幂等键）+ 插入 Outbox ──
    // 幂等键设计（T-S5-06）：orderCode + boxcode + 摘要
    QString msgId = QString::fromUtf8(QUuid::createUuid().toByteArray().toHex());
    QString nowStr = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    QString nextRetry = QDateTime::currentDateTime().addSecs(OUTBOX_RETRY_INTERVAL_SEC)
                            .toString("yyyy-MM-dd HH:mm:ss");

    OutboxRecord outMsg;
    outMsg.msgId     = msgId;
    outMsg.orderCode = orderCode;
    outMsg.boxcode   = boxCode;
    outMsg.payload   = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    outMsg.status    = "pending";
    outMsg.retryCount = 0;
    outMsg.nextRetry  = nextRetry;
    outMsg.createdAt  = nowStr;

    bool outboxOk = false;
    if (m_pSortingDb)
    {
        outboxOk = m_pSortingDb->insertOutboxFullbox(outMsg);
    }
    qint64 t3 = funcTimer.elapsed();  // ★ 耗时：Outbox写入

    // ★ 2026-09-06：满箱报文已入 Outbox（后续成功确认/失败重试均走 Outbox 里的 payload，
    //   不再依赖内存记录），此时移除该格内存分拣记录——原由已停用的旧 lockGrid 回传承担清理，
    //   防止同一格重复触发满箱时记录叠加/重复发送
    if (outboxOk)
    {
        std::lock_guard<std::mutex> lock(m_gridRecordMutex);
        m_gridSortRecords.remove(grid);
        HTTP_LOG_INFO("满箱回传（H7） 已清理该格内存分拣记录 grid=%s items=%d",
            grid.toLocal8Bit().data(), records.size());
    }
    else
    {
        HTTP_LOG_ERROR("满箱回传（H7） Outbox写入失败，保留内存记录待重试 grid=%s items=%d",
            grid.toLocal8Bit().data(), records.size());
    }

    // ★ 耗时统计：记录发送时间戳（用于 onFullboxReplyFinished 计算网络往返耗时）
    {
        std::lock_guard<std::mutex> lock(m_msgTimeMutex);
        m_msgSendTime[msgId] = epochMs;
    }

    HTTP_LOG_INFO("满箱回传消息已入Outbox（H7） order=%s grid=%s box=%s items=%d",
        orderCode.toLocal8Bit().data(),
        grid.toLocal8Bit().data(), boxCode.toLocal8Bit().data(), records.size());
    emit logMessage(QString("[满箱回传] 消息入Outbox grid=%1 box=%2 items=%3")
        .arg(grid).arg(boxCode).arg(records.size()));

    // ★ 原始报文：满箱回传（H7）写入 run.log（QString 必须转 UTF-8，constData() 是 QChar* 会被 %s 截断）
    LOG_INFO("[原始报文] [满箱回传] body=%s(%d字节)",
        outMsg.payload.toUtf8().data(), outMsg.payload.size());

    // ── 步骤7: 发送到 WMS ──
    sendFullboxToWms(msgId, payload);
    qint64 t4 = funcTimer.elapsed();  // ★ 耗时：信号发送

    // ★ 耗时汇总日志（不包含网络往返，仅本地处理）
    LOG_INFO("[耗时] [满箱回传] grid=%s 前置校验=%lldms payload构建=%lldms Outbox写入=%lldms 信号发送=%lldms 本地总计=%lldms",
        grid.toLocal8Bit().data(),
        t1, t2 - t1, t3 - t2, t4 - t3, t4);
    HTTP_LOG_INFO("满箱回传耗时（H7） grid=%s 前置校验=%lldms payload构建=%lldms Outbox=%lldms 信号发送=%lldms 总计=%lldms",
        grid.toLocal8Bit().data(), t1, t2 - t1, t3 - t2, t4 - t3, t4);
}

// ============================================================================
// lookupGridBoxCode — 取格口当前容器号（内存快速路径 → 数据库现查兜底）
// 满箱回传（H7）与「完结前补发」共用；返回空表示该格口当前无绑定
// ============================================================================
QString HttpServer::lookupGridBoxCode(const QString& grid)
{
    QString boxCode;
    {
        std::lock_guard<std::mutex> lock(m_containerMutex);
        boxCode = m_containerBindings.value(grid);
        if (boxCode.isEmpty())
        {
            bool ok = false;
            int g = grid.toInt(&ok);
            if (ok && g >= 1)
                boxCode = m_containerBindings.value(QString("%1").arg(g, GRID_KEY_PADDING, 10, QChar('0')));
        }
    }
    // ★ 内存未命中时从数据库现查当前格口绑定（程序重启后内存为空，仍能取到容器号）
    if (boxCode.isEmpty() && m_pSortingDb && m_pSortingDb->isOpen())
    {
        bool ok = false;
        int g = grid.toInt(&ok);
        QString normalizedGrid = ok ? QString("%1").arg(g, GRID_KEY_PADDING, 10, QChar('0')) : grid;
        GridBoxBindRecord bind = m_pSortingDb->getActiveBind(normalizedGrid);
        boxCode = bind.boxcode;
        if (!boxCode.isEmpty())
            HTTP_LOG_INFO("容器号取自数据库 grid=%s box=%s",
                normalizedGrid.toLocal8Bit().data(), boxCode.toLocal8Bit().data());
    }
    return boxCode;
}

// ============================================================================
// sendFullboxForGrid — 单格口 H7 满箱补发（「完结前兜底补发」与「手动满箱切换」共用）
// 不动波次状态机、不禁用格口；无容器号/缺SKU/Outbox失败 时返回 false（记录保留内存）
// ============================================================================
bool HttpServer::sendFullboxForGrid(const QString& orderCode, const QString& grid,
                                    QVector<GridSortRecord> records)
{
    if (records.isEmpty()) return false;

    // 1. 容器号
    QString boxCode = lookupGridBoxCode(grid);
    if (boxCode.isEmpty())
    {
        HTTP_LOG_WARN("满箱补发 格口 %s 无容器绑定，跳过（记录保留内存，可人工核对）",
            grid.toLocal8Bit().data());
        emit logMessage(QString("[满箱补发] 格口%1 无容器绑定，跳过（记录保留内存，可人工核对）").arg(grid), true);
        return false;
    }

    // 2. SKU 校验（兜底补查 EpcCache；仍缺则跳过并写异常）
    {
        QStringList missingSkuEpcs;
        for (auto rit = records.begin(); rit != records.end(); ++rit)
        {
            if (rit->sku.isEmpty())
                rit->sku = getSkuByEpc(rit->inco);
            if (rit->sku.isEmpty())
                missingSkuEpcs << rit->inco;
        }
        if (!missingSkuEpcs.isEmpty())
        {
            HTTP_LOG_ERROR("满箱补发 格口 %s 有 %d 条EPC未映射SKU，跳过 epc=[%s]",
                grid.toLocal8Bit().data(), missingSkuEpcs.size(),
                missingSkuEpcs.join(",").left(400).toLocal8Bit().data());
            emit logMessage(QString("[满箱补发] 格口%1 有 %2 条未查到SKU，跳过（记录保留内存，可人工核对）")
                .arg(grid).arg(missingSkuEpcs.size()), true);
            if (m_pSortingDb && m_pSortingDb->isOpen())
            {
                ExceptionRecord ex;
                ex.type      = QString::fromUtf8("满箱补发SKU缺失");
                ex.orderCode = orderCode;
                ex.epc       = missingSkuEpcs.first();
                ex.sku       = "";
                ex.reason    = QString("格口%1 满箱补发时 %2 条EPC未查到SKU: [%3]")
                                   .arg(grid).arg(missingSkuEpcs.size())
                                   .arg(missingSkuEpcs.join(",").left(400));
                ex.time      = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
                m_pSortingDb->insertException(ex);
            }
            return false;
        }
    }

    // 3. 构建 H7 报文 + 入 Outbox + 清理该格记录 + 发送
    QJsonObject payload = buildFullboxPayload(orderCode, grid, boxCode, records);
    QString msgId = QString::fromUtf8(QUuid::createUuid().toByteArray().toHex());
    QString nowStr = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    QString nextRetry = QDateTime::currentDateTime().addSecs(OUTBOX_RETRY_INTERVAL_SEC)
                            .toString("yyyy-MM-dd HH:mm:ss");

    OutboxRecord outMsg;
    outMsg.msgId      = msgId;
    outMsg.orderCode  = orderCode;
    outMsg.boxcode    = boxCode;
    outMsg.payload    = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    outMsg.status     = "pending";
    outMsg.retryCount = 0;
    outMsg.nextRetry  = nextRetry;
    outMsg.createdAt  = nowStr;

    bool outboxOk = false;
    if (m_pSortingDb)
        outboxOk = m_pSortingDb->insertOutboxFullbox(outMsg);

    if (outboxOk)
    {
        std::lock_guard<std::mutex> lock(m_gridRecordMutex);
        m_gridSortRecords.remove(grid);
    }
    else
    {
        HTTP_LOG_ERROR("满箱补发 Outbox写入失败 grid=%s（保留内存记录）",
            grid.toLocal8Bit().data());
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_msgTimeMutex);
        m_msgSendTime[msgId] = QDateTime::currentMSecsSinceEpoch();
    }

    HTTP_LOG_INFO("满箱补发（H7） grid=%s box=%s items=%d msgId=%s",
        grid.toLocal8Bit().data(), boxCode.toLocal8Bit().data(), records.size(),
        msgId.toLocal8Bit().data());
    emit logMessage(QString("[满箱补发] 满箱回传(H7) grid=%1 box=%2 items=%3")
        .arg(grid).arg(boxCode).arg(records.size()));
    LOG_INFO("[原始报文] [满箱回传-补发] body=%s(%d字节)",
        outMsg.payload.toUtf8().data(), outMsg.payload.size());

    sendFullboxToWms(msgId, payload);
    return true;
}

// ============================================================================
// flushUnreportedFullboxes — 完结回传前兜底补发（2026-09-07）
// 把内存中尚未满箱回传的格口分拣数据按 H7 满箱回传发给 WMS，再发 H8 完结回传。
// 不动波次状态机、不禁用格口；无容器号/缺SKU 的格口跳过并告警（不阻塞完结）。
// 返回实际补发的格口数。
// ============================================================================
int HttpServer::flushUnreportedFullboxes(const QString& orderCode)
{
    QMap<QString, QVector<GridSortRecord>> pending;
    {
        std::lock_guard<std::mutex> lock(m_gridRecordMutex);
        pending = m_gridSortRecords;
    }

    int flushed = 0;
    for (auto it = pending.constBegin(); it != pending.constEnd(); ++it)
    {
        if (it.value().isEmpty()) continue;
        if (sendFullboxForGrid(orderCode, it.key(), it.value()))
            ++flushed;
    }

    HTTP_LOG_INFO("完结前满箱补发完成 order=%s flushed=%d",
        orderCode.toLocal8Bit().data(), flushed);
    return flushed;
}

// ============================================================================
// manualFullbox — 手动满箱切换（2026-09-07）
// UI 在「重传满箱切换」旁输入格口号后点击：读取该格口当前分拣记录与容器号，
// 立即按 H7 满箱回传上传（替代人工 PLC 锁格的补传手段，不动波次状态机）
// ============================================================================
bool HttpServer::manualFullbox(const QString& grid)
{
    if (!m_pWaveMgr)
    {
        emit logMessage("[手动满箱] 服务未就绪", true);
        return false;
    }
    if (grid.isEmpty())
    {
        emit logMessage("[手动满箱] 请先输入格口号", true);
        return false;
    }

    QString orderCode = m_pWaveMgr->orderCode();
    if (orderCode.isEmpty())
    {
        emit logMessage("[手动满箱] 当前无运行波次，无法执行手动满箱", true);
        return false;
    }

    QVector<GridSortRecord> records;
    {
        std::lock_guard<std::mutex> lock(m_gridRecordMutex);
        auto it = m_gridSortRecords.find(grid);
        if (it != m_gridSortRecords.end())
            records = it.value();
    }
    if (records.isEmpty())
    {
        HTTP_LOG_WARN("手动满箱 格口 %s 无分拣记录（可能已满箱回传或未落格）", grid.toLocal8Bit().data());
        emit logMessage(QString("[手动满箱] 格口%1 无待上传的分拣记录（可能已满箱或未落格）").arg(grid), true);
        return false;
    }

    bool ok = sendFullboxForGrid(orderCode, grid, records);
    if (ok)
        emit logMessage(QString("[手动满箱] 格口%1 已按 H7 满箱回传上传（order=%2）").arg(grid).arg(orderCode));
    return ok;
}

// ============================================================================
// sendFullboxToWms — 发送满箱回传报文到 WMS（H7，T-S5-04）
// 通过 HttpClient 异步发送，结果由 onFullboxReplyFinished 处理
// ============================================================================
void HttpServer::sendFullboxToWms(const QString& msgId, const QJsonObject& payload)
{
    // 通过 MainWindow 持有的 HttpClient 发送（HttpServer 不直接持有 HttpClient）
    // 发出 fullboxReportReady 信号，由 MainWindow 中继到 HttpClient 并回传结果
    emit fullboxReportReady(payload, msgId);
}

// ============================================================================
// onFullboxReplyFinished — 满箱回传结果处理（H7，T-S5-05）
// 成功：归档容器，标记 Outbox 成功，状态回 SORTING
// 失败：更新 Outbox 重试信息，由 pollOutboxFullbox 调度重试
// ============================================================================
void HttpServer::onFullboxReplyFinished(const QString& msgId, bool success, const QString& body)
{
    // ★ 耗时统计：网络往返耗时
    qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    qint64 sendMs = 0;
    {
        std::lock_guard<std::mutex> lock(m_msgTimeMutex);
        auto it = m_msgSendTime.find(msgId);
        if (it != m_msgSendTime.end())
        {
            sendMs = it.value();
            m_msgSendTime.erase(it);
        }
    }
    qint64 roundTripMs = (sendMs > 0) ? (nowMs - sendMs) : -1;

    if (!m_pSortingDb)
    {
        HTTP_LOG_WARN("满箱回传结果处理（H7） SortingDatabase未初始化");
        return;
    }

    if (success)
    {
        // ── 成功处理（T-S5-05）────
        // 1. 标记 Outbox 成功
        m_pSortingDb->markOutboxFullboxSuccess(msgId);

        // 2. 获取 Outbox 消息以获取 boxcode 和 orderCode
        OutboxRecord outMsg = m_pSortingDb->getOutboxFullboxByMsgId(msgId);
        QString boxCode = outMsg.boxcode;
        QString orderCode = outMsg.orderCode;

        HTTP_LOG_INFO("满箱回传成功（H7） order=%s box=%s 网络往返=%lldms",
            orderCode.toLocal8Bit().data(),
            boxCode.toLocal8Bit().data(), roundTripMs);
        LOG_INFO("[耗时] [满箱回传] 网络往返=%lldms order=%s box=%s success=1",
            roundTripMs, orderCode.toLocal8Bit().data(), boxCode.toLocal8Bit().data());
        emit logMessage(QString("[满箱回传] 回传成功 order=%1 box=%2")
            .arg(orderCode).arg(boxCode));

        // ★ 2026-09-06 归属防护：H7 报文可能属于"已切出/新任务"的旧波次（在途请求补发）。
        //   DB 归档（上方标记）与日志照常；容器归档与状态迁移只作用于当前内存波次，防污染
        bool bIsCurrent = m_pWaveMgr && m_pWaveMgr->orderCode() == orderCode;
        if (!bIsCurrent)
        {
            HTTP_LOG_INFO("满箱回传（H7） 成功但该波次已非当前内存波次 order=%s（仅更新数据库，跳过状态/容器处理）",
                orderCode.toLocal8Bit().data());
            emit logMessage(QString("[满箱回传] order=%1 已成功（该波次已切出，仅记录；切回时将自动补发/同步）")
                .arg(orderCode));
        }

        // 3. 归档容器绑定（允许新容器绑定（H6））—— 仅当前波次
        // 找到绑定该 boxcode 的格口，归档旧绑定
        if (bIsCurrent)
        {
            std::lock_guard<std::mutex> lock(m_containerMutex);
            for (auto it = m_containerBindings.begin(); it != m_containerBindings.end(); ++it)
            {
                if (it.value() == boxCode)
                {
                    QString gridNum = it.key();
                    if (m_pSortingDb)
                        m_pSortingDb->archiveGridBinds(gridNum);
                    HTTP_LOG_INFO("满箱回传（H7） 容器已归档 grid=%s box=%s",
                        gridNum.toLocal8Bit().data(), boxCode.toLocal8Bit().data());
                    emit logMessage(QString("[满箱回传] 容器已归档 grid=%1 box=%2")
                        .arg(gridNum).arg(boxCode));
                    break;
                }
            }
        }

        // 4. 状态恢复：FULLBOX_SYNC → SORTING（T-S5-05）—— 仅当前波次
        if (bIsCurrent && m_pWaveMgr)
        {
            m_pWaveMgr->resumeSorting();
        }
    }
    else
    {
        // ── 失败处理（T-S5-04）────
        LOG_INFO("[耗时] [满箱回传] 网络往返=%lldms success=0",
            roundTripMs);

        // 查询当前重试次数
        OutboxRecord outMsg = m_pSortingDb->getOutboxFullboxByMsgId(msgId);

        if (outMsg.retryCount >= OUTBOX_RETRY_MAX_H7)
        {
            // 重试耗尽：标记 failed，记录异常，状态回 SORTING（满箱失败不阻塞分拣，failed 报文可面板补发）
            m_pSortingDb->updateOutboxFullboxStatus(msgId, "failed",
                QDateTime::currentDateTime().addSecs(OUTBOX_RETRY_INTERVAL_SEC)
                    .toString("yyyy-MM-dd HH:mm:ss"));

            HTTP_LOG_ERROR("满箱回传失败（H7） 重试耗尽 order=%s retry=%d body=%s",
                outMsg.orderCode.toLocal8Bit().data(),
                outMsg.retryCount, body.left(200).toLocal8Bit().data());
            emit logMessage(QString("[满箱回传] 重试耗尽 order=%1 retry=%2（失败报文可在面板重传，分拣继续）")
                .arg(outMsg.orderCode).arg(outMsg.retryCount), true);

            // ★ 记录异常到 exception_record 表
            if (m_pSortingDb)
            {
                ExceptionRecord ex;
                ex.type      = QString::fromUtf8("满箱回传失败");
                ex.orderCode = outMsg.orderCode;
                ex.epc       = "";
                ex.sku       = "";
                ex.reason    = QString("满箱回传（H7）重试%1次后仍失败 body=%2")
                                  .arg(outMsg.retryCount).arg(body.left(200));
                ex.time      = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
                m_pSortingDb->insertException(ex);
            }

            // ★ 状态回 SORTING（FULLBOX_SYNC→SORTING 合法迁移），满箱失败不阻塞分拣
            //   （原 setState(WAVE_IDLE) 被状态机白名单拒绝，属无效调用）
            // ★ 2026-09-06 归属防护：该波次已非当前内存波次时不动状态（切回时按 DB 状态恢复）
            bool bIsCurrent = m_pWaveMgr && m_pWaveMgr->orderCode() == outMsg.orderCode;
            if (bIsCurrent && m_pWaveMgr)
            {
                if (m_pWaveMgr->setState(WAVE_SORTING))
                    HTTP_LOG_INFO("满箱回传失败（H7） 状态回SORTING order=%s 分拣继续（failed报文可重传补发）",
                        outMsg.orderCode.toLocal8Bit().data());
                else
                    HTTP_LOG_WARN("满箱回传失败（H7） 状态回SORTING被拒绝 order=%s current=%d",
                        outMsg.orderCode.toLocal8Bit().data(), m_pWaveMgr->status());
            }

            // ★ 重试耗尽后停止 H7 满箱回传定时器（已标记 failed，不再需要轮询；仅当前波次停止全局轮询）
            //   非当前波次的失败报文保留为 failed，切回该波次时自动补发（resendOutbox）
            if (bIsCurrent && m_outboxFullboxTimer) m_outboxFullboxTimer->stop();
            HTTP_LOG_INFO("满箱回传失败（H7） retry耗尽 order=%s 当前内存=%d（failed报文可手动重传补发）",
                outMsg.orderCode.toLocal8Bit().data(), bIsCurrent ? 1 : 0);
        }
        else
        {
            // 未耗尽：更新重试信息，等待 pollOutboxFullbox 调度
            HTTP_LOG_WARN("满箱回传失败（H7） 将重试 order=%s retry=%d/%d",
                outMsg.orderCode.toLocal8Bit().data(),
                outMsg.retryCount + 1, OUTBOX_RETRY_MAX_H7);
            emit logMessage(QString("[满箱回传] 回传失败 将重试 order=%1 retry=%2/%3")
                .arg(outMsg.orderCode).arg(outMsg.retryCount + 1).arg(OUTBOX_RETRY_MAX_H7), true);
        }
    }
}

// ============================================================================
// pollOutboxFullbox — 满箱回传 Outbox 重试调度（H7，T-S5-04）
// 定时扫描 outbox_fullbox 表中 status='pending' 且 next_retry <= now 的消息
// 重新发送到 WMS
// ★ 仅重试当前波次的消息，非当前波次的消息标记为 cancelled 跳过
// ============================================================================
void HttpServer::pollOutboxFullbox()
{
    if (!m_pSortingDb) return;

    QVector<OutboxRecord> pending = m_pSortingDb->getPendingOutboxFullbox(OUTBOX_POLL_BATCH_SIZE);
    if (pending.isEmpty()) return;

    HTTP_LOG_INFO("满箱回传 Outbox 重试调度（H7） 扫描到 %d 条待重试消息", pending.size());

    // ★ 当前波次 orderCode，用于过滤非当前波次的旧消息
    QString currentOrderCode = m_pWaveMgr ? m_pWaveMgr->orderCode() : QString();

    for (const OutboxRecord& msg : pending)
    {
        // ★ 非当前波次的消息：标记为 cancelled，跳过重试
        if (currentOrderCode.isEmpty() || msg.orderCode != currentOrderCode)
        {
            HTTP_LOG_INFO("满箱回传 Outbox 跳过非当前波次消息（H7） msgOrder=%s currentOrder=%s",
                msg.orderCode.toLocal8Bit().data(),
                currentOrderCode.toLocal8Bit().data());
            if (m_pSortingDb)
                m_pSortingDb->updateOutboxFullboxStatus(msg.msgId, "cancelled",
                    QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
            continue;
        }

        // 解析 payload
        QJsonDocument doc = QJsonDocument::fromJson(msg.payload.toUtf8());
        if (doc.isNull() || !doc.isObject())
        {
            HTTP_LOG_WARN("满箱回传 Outbox payload解析失败（H7）");
            // 标记为 failed
            if (m_pSortingDb)
                m_pSortingDb->updateOutboxFullboxStatus(msg.msgId, "failed",
                    QDateTime::currentDateTime().addSecs(OUTBOX_RETRY_INTERVAL_SEC)
                        .toString("yyyy-MM-dd HH:mm:ss"));
            continue;
        }

        QJsonObject payload = doc.object();

        // 更新重试信息
        QDateTime now = QDateTime::currentDateTime();
        QString nextRetry = now.addSecs(OUTBOX_RETRY_INTERVAL_SEC)
                               .toString("yyyy-MM-dd HH:mm:ss");
        m_pSortingDb->updateOutboxFullboxStatus(msg.msgId, "pending", nextRetry);

        HTTP_LOG_INFO("满箱回传 Outbox 重试发送（H7） order=%s retry=%d",
            msg.orderCode.toLocal8Bit().data(),
            msg.retryCount + 1);

        // 重新发送
        sendFullboxToWms(msg.msgId, payload);
    }
}

// ============================================================================
// S6 新增：完结回传（H8 波次完结通知WMS，需求 §11，T-S6-01~T-S6-05）
// ============================================================================

// ============================================================================
// buildEndPayload — 构建完结回传报文（H8 gwisSubProductClassifyEndOrder，T-S6-02）
// 报文格式详见需求 §11.3
// 字段取值：
//   orderCode      = 波次下发编号（H4）
//   orderType      = 固定 "02"（WMS_ORDER_TYPE）
//   sumLocation    = 落格分拣总件数（sorted()）
//   operuserDate   = 当前时间
//   operuserCode   = 默认 "admin"
//   operuserName   = 默认 "管理员"
// ============================================================================
QJsonObject HttpServer::buildEndPayload(const QString& orderCode, int sumLocation)
{
    AppConfig& cfg = ConfigManager::instance()->config();

    QJsonObject head;
    head["orderCode"]      = orderCode;
    head["orderType"]      = WMS_ORDER_TYPE;
    head["sumLocation"]    = QString::number(sumLocation);
    head["operuserDate"]   = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.000");
    head["operuserCode"]   = WMS_OPERUSER_CODE;
    head["operuserName"]   = QString::fromUtf8(WMS_OPERUSER_NAME);

    QJsonObject report;
    report["head"] = head;

    HTTP_LOG_INFO("完结回传报文构建（H8） order=%s sumLocation=%d",
        orderCode.toLocal8Bit().data(), sumLocation);

    return report;
}

// ============================================================================
// sendEnd — 完结触发入口（T-S6-01/02）
// ★ 纠正2: 唯一触发时机 — 用户点击"结束任务"按钮
// 流程：
//   1. 校验 canComplete()
//   2. 状态迁移 SORTING→ENDING
//   3. 构建完结回传报文（H8）
//   4. 生成 msgId（UUID 幂等键）
//   5. 插入 Outbox 出站表
//   6. 发送到 WMS
// ============================================================================
// ★ 完结触发入口（T-S6-01/02）；返回 true=已进入完结回传流程，false=同步拒绝（调用方可立即收尾）
bool HttpServer::sendEnd()
{
    QElapsedTimer funcTimer;
    funcTimer.start();
    qint64 epochMs = QDateTime::currentMSecsSinceEpoch();
    if (!m_pWaveMgr)
    {
        HTTP_LOG_WARN("完结回传触发失败（H8） WaveManager未初始化");
        emit logMessage("[完结回传] 触发失败: WaveManager未初始化", true);
        return false;
    }

    // ★ 终态幂等判断：已取消（CANCELLED）、已完成（FINISHED）、异常挂起（HELD）拒绝操作
    int status = m_pWaveMgr->status();
    if (status == WAVE_CANCELLED || status == WAVE_FINISHED || status == WAVE_HELD || status == WAVE_CANCEL_PENDING)
    {
        HTTP_LOG_WARN("完结回传（H8） 波次状态异常，拒绝完结 status=%d(%s) orderCode=%s",
            status, WaveSnapshot::statusToString(status).toLocal8Bit().data(),
            m_pWaveMgr->orderCode().toLocal8Bit().data());
        emit logMessage(QString("[完结回传] 波次状态异常，拒绝完结 order=%1 status=%2")
            .arg(m_pWaveMgr->orderCode()).arg(WaveSnapshot::statusToString(status)), true);
        return false;
    }

    // ★ 2026-09-07 完结前兜底补发：把尚未满箱回传的格口数据先按 H7 发给 WMS，再发 H8 完结回传
    {
        int flushedGrids = flushUnreportedFullboxes(m_pWaveMgr->orderCode());
        if (flushedGrids > 0)
        {
            emit logMessage(QString("[完结前补发] 共补发 %1 个格口的满箱回传（H7），随后发送完结回传（H8）")
                .arg(flushedGrids));
        }
    }

    // ── 步骤1-2: 校验可完结条件 + 状态迁移 ──
    if (!m_pWaveMgr->completeToEnding())
    {
        HTTP_LOG_WARN("完结回传触发失败（H8） 条件不满足或状态迁移拒绝 status=%d",
            m_pWaveMgr->status());
        emit logMessage(QString("[完结回传] 触发失败: 条件不满足 status=%1")
            .arg(m_pWaveMgr->status()), true);
        return false;
    }

    QString orderCode = m_pWaveMgr->orderCode();
    int sumLocation = m_pWaveMgr->sorted();  // 落格分拣总件数（告知WMS分拣了多少件）

    // ★ 波次完成时检查未分拣数量（计数对比，不逐个列举EPC/SKU）
    //    注：m_setCodeRecv 存储 SKU 编码（inco），m_setCodeSorted 存储 EPC 编码，
    //    两者是不同标识符空间，不能直接做差集运算，用计数对比即可
    {
        int unsortedCount = m_pWaveMgr->totalRecv() - m_pWaveMgr->sorted() - m_pWaveMgr->exception();
        if (unsortedCount > 0)
        {
            HTTP_LOG_WARN("完结回传（H8） 存在未分拣 order=%s unsortedCount=%d total=%d sorted=%d exc=%d",
                orderCode.toLocal8Bit().data(), unsortedCount,
                m_pWaveMgr->totalRecv(), m_pWaveMgr->sorted(), m_pWaveMgr->exception());

            if (m_pSortingDb)
            {
                ExceptionRecord ex;
                ex.type      = QString::fromUtf8("未分拣");
                ex.orderCode = orderCode;
                ex.epc       = "";
                ex.sku       = "";
                ex.reason    = QString::fromUtf8("波次完结时仍有%1个未分拣（总%2/已分拣%3/异常%4）")
                                  .arg(unsortedCount)
                                  .arg(m_pWaveMgr->totalRecv())
                                  .arg(m_pWaveMgr->sorted())
                                  .arg(m_pWaveMgr->exception());
                ex.time      = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
                m_pSortingDb->insertException(ex);

                HTTP_LOG_INFO("完结回传（H8） 未分拣已记录异常 order=%s unsortedCount=%d",
                    orderCode.toLocal8Bit().data(), unsortedCount);
            }
            emit logMessage(QString("[完结回传] 未分拣异常 order=%1 unsortedCount=%2")
                .arg(orderCode).arg(unsortedCount), true);
        }
    }

    // ── 步骤3: 构建完结回传报文（H8）──
    QJsonObject payload = buildEndPayload(orderCode, sumLocation);
    qint64 t1 = funcTimer.elapsed();  // ★ 耗时：payload构建

    // ── 步骤4-5: 生成 msgId（UUID 幂等键）+ 插入 Outbox ──
    QString msgId = QString::fromUtf8(QUuid::createUuid().toByteArray().toHex());
    QString nowStr = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    // ★ 2026-09-02 修复"结束任务卡死"：首次失败后按快速间隔（5s）安排重试，
    //   避免旧逻辑 next_retry=+30s 造成"失败后长时间无重试"的静默等待
    QString nextRetry = QDateTime::currentDateTime()
                            .addMSecs(OUTBOX_RETRY_INTERVAL_FAST_MS)
                            .toString("yyyy-MM-dd HH:mm:ss");

    OutboxRecord outMsg;
    outMsg.msgId      = msgId;
    outMsg.orderCode  = orderCode;
    outMsg.boxcode    = "";  // 完结回传（H8）不关联容器
    outMsg.payload    = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    outMsg.status     = "pending";
    outMsg.retryCount = 0;
    outMsg.nextRetry  = nextRetry;
    outMsg.createdAt  = nowStr;

    if (m_pSortingDb)
    {
        m_pSortingDb->insertOutboxEnd(outMsg);
    }
    qint64 t2 = funcTimer.elapsed();  // ★ 耗时：Outbox写入

    // ★ 耗时统计：记录发送时间戳（用于 onEndReplyFinished 计算网络往返耗时）
    {
        std::lock_guard<std::mutex> lock(m_msgTimeMutex);
        m_msgSendTime[msgId] = epochMs;
    }

    HTTP_LOG_INFO("完结回传消息已入Outbox（H8） order=%s sumLocation=%d",
        orderCode.toLocal8Bit().data(), sumLocation);
    emit logMessage(QString("[完结回传] 消息入Outbox order=%1 sumLocation=%2")
        .arg(orderCode).arg(sumLocation));

    // ★ 原始报文：完结回传（H8）写入 run.log（QString 必须转 UTF-8，constData() 是 QChar* 会被 %s 截断）
    LOG_INFO("[原始报文] [完结回传] body=%s(%d字节)",
        outMsg.payload.toUtf8().data(), outMsg.payload.size());

    // ── 步骤6: 发送到 WMS ──
    sendEndToWms(msgId, payload);
    qint64 t3 = funcTimer.elapsed();  // ★ 耗时：信号发送
    HTTP_LOG_INFO("[耗时] [完结回传] order=%s payload构建=%lldms Outbox写入=%lldms 信号发送=%lldms 本地总计=%lldms",
        orderCode.toLocal8Bit().data(), t1, t2 - t1, t3 - t2, t3);
    HTTP_LOG_INFO("完结回传耗时（H8） order=%s payload构建=%lldms Outbox=%lldms 信号发送=%lldms 总计=%lldms",
        orderCode.toLocal8Bit().data(), t1, t2 - t1, t3 - t2, t3);

    // ★ 2026-09-02 修复"结束任务卡死"：启动 H8 会话兜底定时器
    //   任何网络异常（无响应/丢包/回调丢失）下，END_WAIT_TIMEOUT_MS 后 onEndSessionTimeout
    //   会强制结束会话并 emit endReportFinished → MainWindow 停止服务（不卡死、不退出程序）。
    //   未确认的 H8 消息保留在 outbox_end，下次启动由 pollOutboxEnd 补传/归档。
    // ★ 2026-09-06 会话归属波次：切出/切换后超时兜底只处理本波次，不污染其它波次
    m_endSessionOrderCode = orderCode;
    if (m_endSessionTimer)
    {
        m_endSessionTimer->start(END_WAIT_TIMEOUT_MS);
        HTTP_LOG_INFO("完结回传（H8） 会话兜底定时器已启动 timeout=%dms order=%s",
            END_WAIT_TIMEOUT_MS, orderCode.toLocal8Bit().data());
    }

    return true;   // ★ 2026-09-02：已成功进入完结回传流程
}

// ============================================================================
// onEndSessionTimeout — H8 完结回传会话超时兜底（2026-09-02 新增）
// 触发条件：点击"结束任务"后 END_WAIT_TIMEOUT_MS 内未收到任何回传结果
//  （网络完全无响应 / 回调丢失等极端场景）；正常失败重试会在耗尽后自行结束会话。
// 行为：记录异常 → 波次终结（FINISHED，不再异常挂起阻塞工作流）→ 发出 endReportFinished，
//   H8 未确认报文保留在 outbox_end，由定时器继续重试/面板手动补发。
// ============================================================================
void HttpServer::onEndSessionTimeout()
{
    // ★ 2026-09-06 会话归属波次：超时兜底只处理发起会话的波次（切出后内存可能是别的波次/空闲）
    QString orderCode = m_endSessionOrderCode;
    m_endSessionOrderCode.clear();
    if (m_endSessionTimer) m_endSessionTimer->stop();
    if (orderCode.isEmpty())
        orderCode = m_pWaveMgr ? m_pWaveMgr->orderCode() : QString();

    HTTP_LOG_ERROR("完结回传（H8） 会话超时兜底触发 timeout=%dms order=%s",
        END_WAIT_TIMEOUT_MS, orderCode.toLocal8Bit().data());
    emit logMessage(QString("[完结回传] 等待超时(%1s)，强制结束回传会话（未确认报文保留在outbox继续重试/可面板补发）")
        .arg(END_WAIT_TIMEOUT_MS / 1000), true);

    // 记录异常（可追溯：H8 超时未确认）
    if (m_pSortingDb && !orderCode.isEmpty())
    {
        ExceptionRecord ex;
        ex.type      = QString::fromUtf8("完结回传超时");
        ex.orderCode = orderCode;
        ex.epc       = "";
        ex.sku       = "";
        ex.reason    = QString("完结回传（H8）等待%1s无结果，会话超时强制结束（未确认报文保留在outbox待补发）")
                          .arg(END_WAIT_TIMEOUT_MS / 1000);
        ex.time      = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
        m_pSortingDb->insertException(ex);
    }

    // ★ 波次终结：完结流程结束即 FINISHED（不再进入异常挂起，避免阻塞工作流）
    //   未确认的 H8 保留在 outbox_end（status=pending），outboxEndTimer 继续自动重试
    // ★ 2026-09-06 防护：若内存波次已不在 ENDING（等待期间执行了「新任务/切换」），
    //   只归档 DB，不动新波次的内存数据
    if (m_pWaveMgr && m_pWaveMgr->status() == WAVE_ENDING)
    {
        m_pWaveMgr->setState(WAVE_FINISHED);
        HTTP_LOG_INFO("完结回传（H8） 会话超时 波次终结 ENDING→FINISHED order=%s（H8保留outbox继续重试）",
            orderCode.toLocal8Bit().data());

        // 收尾：存档 + 清空运行时状态（★ 2026-09-07 客户确认：完结不清空容器绑定，
        //   绑定信息保留在数据库，供后续追溯/切回查看；下一波次由 H6 按格口覆盖）
        if (m_pSortingDb && !orderCode.isEmpty())
            m_pSortingDb->archiveWave(orderCode);
        {
            std::lock_guard<std::mutex> lock(m_gridRecordMutex);
            m_gridSortRecords.clear();
        }
        {
            std::lock_guard<std::mutex> lock(m_gridCountMutex);
            m_gridSortedCount.clear();
        }
    }
    else
    {
        HTTP_LOG_WARN("完结回传（H8） 会话超时但内存波次已不在完结等待（可能已切出/新任务），跳过状态收尾 order=%s current=%d",
            orderCode.toLocal8Bit().data(),
            m_pWaveMgr ? m_pWaveMgr->status() : -1);
        if (m_pSortingDb && !orderCode.isEmpty())
            m_pSortingDb->updateWaveStatus(orderCode, WAVE_FINISHED);
    }

    // ★ 保证等待必然结束：通知 MainWindow 执行收尾（幂等，重复触发无副作用；非停止等待时 UI 忽略）
    emit endReportFinished();
}

// ============================================================================
// sendEndToWms — 发送完结回传报文到 WMS（H8，T-S6-03）
// 通过 HttpClient 异步发送，结果由 onEndReplyFinished 处理
// ============================================================================
void HttpServer::sendEndToWms(const QString& msgId, const QJsonObject& payload)
{
    emit endReportReady(payload, msgId);
}

// ============================================================================
// onEndReplyFinished — 完结回传结果处理（H8，T-S6-04）
// 成功：标记 Outbox 成功，状态→FINISHED，拒绝继续分拣
// 失败：更新 Outbox 重试信息，由 pollOutboxEnd 调度重试
// ============================================================================
void HttpServer::onEndReplyFinished(const QString& msgId, bool success, const QString& body)
{
    // ★ 耗时统计：网络往返耗时
    qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    qint64 sendMs = 0;
    {
        std::lock_guard<std::mutex> lock(m_msgTimeMutex);
        auto it = m_msgSendTime.find(msgId);
        if (it != m_msgSendTime.end())
        {
            sendMs = it.value();
            m_msgSendTime.erase(it);
        }
    }
    qint64 roundTripMs = (sendMs > 0) ? (nowMs - sendMs) : -1;

    if (!m_pSortingDb)
    {
        HTTP_LOG_WARN("完结回传结果处理（H8） SortingDatabase未初始化");
        return;
    }

    if (success)
    {
        // ── 成功处理（T-S6-04）────
        m_pSortingDb->markOutboxEndSuccess(msgId);

        OutboxRecord outMsg = m_pSortingDb->getOutboxEndByMsgId(msgId); // ★ S6 按 msgId 查询

        // ★ 2026-09-06 会话归属：该 msgId 所属会话若在兜底等待中，收到结果即结束兜底
        if (m_endSessionOrderCode == outMsg.orderCode)
        {
            m_endSessionOrderCode.clear();
            if (m_endSessionTimer) m_endSessionTimer->stop();
        }

        HTTP_LOG_INFO("完结回传成功（H8） order=%s 网络往返=%lldms",
            outMsg.orderCode.toLocal8Bit().data(), roundTripMs);
        LOG_INFO("[耗时] [完结回传] 网络往返=%lldms order=%s success=1",
            roundTripMs, outMsg.orderCode.toLocal8Bit().data());
        emit logMessage(QString("[完结回传] 回传成功 order=%1")
            .arg(outMsg.orderCode));

        // ★ 2026-09-06 归属防护：H8 可能属于已切出的旧波次（在途/手动补发）。
        //   DB 标记与历史存档照常；状态推进/内存清理/通知 UI 仅当该波次是当前内存波次
        bool bIsCurrent = m_pWaveMgr && m_pWaveMgr->orderCode() == outMsg.orderCode;
        if (!bIsCurrent)
        {
            HTTP_LOG_INFO("完结回传成功（H8） 但该波次已非当前内存波次 order=%s（仅更新数据库，终态化）",
                outMsg.orderCode.toLocal8Bit().data());
            emit logMessage(QString("[完结回传] order=%1 完结已成功（该波次已切出，仅记录）")
                .arg(outMsg.orderCode));
            m_pSortingDb->updateWaveStatus(outMsg.orderCode, WAVE_FINISHED);
        }

        // 状态：ENDING → FINISHED（已完成，终态）—— 仅当前波次
        if (bIsCurrent && m_pWaveMgr)
        {
            m_pWaveMgr->setState(WAVE_FINISHED);
        }

        // ★ 波次完结后存档到历史数据库（方便追溯查找）—— 无条件（DB 事实归档）
        if (m_pSortingDb)
        {
            m_pSortingDb->archiveWave(outMsg.orderCode);
            HTTP_LOG_INFO("完结回传（H8） 波次历史已存档 order=%s", outMsg.orderCode.toLocal8Bit().data());
        }

        // 以下（定时器/内存清理/通知）仅当前波次
        if (!bIsCurrent)
            return;

        // ★ 2026-09-02：H8 已收到成功结果，停止会话兜底定时器
        if (m_endSessionTimer) m_endSessionTimer->stop();

        // ★ 完结回传成功后停止 Outbox 重试定时器，新波次下发时重新启动
        if (m_outboxEndTimer)     m_outboxEndTimer->stop();
        if (m_outboxFullboxTimer) m_outboxFullboxTimer->stop();
        HTTP_LOG_INFO("完结回传（H8） Outbox重试定时器已停止 order=%s", outMsg.orderCode.toLocal8Bit().data());

        // ★ 波次完结后清理运行时状态（与 H4 新波次下发的重置逻辑一致）
        {
            std::lock_guard<std::mutex> lock(m_gridRecordMutex);
            m_gridSortRecords.clear();
        }
        {
            std::lock_guard<std::mutex> lock(m_gridCountMutex);
            m_gridSortedCount.clear();
        }
        if (m_pPlcMgr)
            m_pPlcMgr->enableAllGrids();
        m_pendingSkuQuery.clear();
        m_skuQueryRetryCount.clear();
        m_notReadyRetryCount.clear();
        m_sentEpcs.clear();
        // ★ 2026-09-07 客户确认：完结后不清空容器绑定——绑定信息保留在内存/数据库，
        //   供追溯与切回查看；下一波次由 H6 按格口重新绑定覆盖
        HTTP_LOG_INFO("波次完结清理完成 order=%s（格口记录/计数/待查SKU/重试/已发送EPC 清空；容器绑定保留）",
            outMsg.orderCode.toLocal8Bit().data());

        // ★ 通知 MainWindow：完结回传处理完毕，可以停止服务
        emit endReportFinished();
    }
    else
    {
        // ── 失败处理（T-S6-03）────
        LOG_INFO("[耗时] [完结回传] 网络往返=%lldms success=0",
            roundTripMs);

        OutboxRecord outMsg = m_pSortingDb->getOutboxEndByMsgId(msgId); // ★ S6 按 msgId 查询
        int retryCount = outMsg.retryCount;
        QString orderCode = outMsg.orderCode;

        // ★ 2026-09-06 会话归属：该 msgId 所属会话若在兜底等待中，收到结果即结束兜底
        if (m_endSessionOrderCode == orderCode)
        {
            m_endSessionOrderCode.clear();
            if (m_endSessionTimer) m_endSessionTimer->stop();
        }

        if (retryCount >= OUTBOX_RETRY_MAX_DEFAULT)
        {
            // 重试耗尽：标记 failed，记录异常，状态→HELD 挂起（2026-09-02 修正：
            //   原 setState(WAVE_IDLE) 被状态机白名单拒绝（ENDING→IDLE 非法）导致波次卡 ENDING，
            //   改为合法的 ENDING→HELD；服务随后停止，新实例启动即 IDLE，不阻塞新波次）
            m_pSortingDb->updateOutboxEndStatus(msgId, "failed",
                QDateTime::currentDateTime().addSecs(OUTBOX_RETRY_INTERVAL_SEC)
                    .toString("yyyy-MM-dd HH:mm:ss"));

            HTTP_LOG_ERROR("完结回传失败（H8） 重试耗尽 order=%s retry=%d body=%s",
                orderCode.toLocal8Bit().data(),
                retryCount, body.left(200).toLocal8Bit().data());
            // ★ 2026-09-04：UI 提示明确"回传地址不可达"排查方向
            emit logMessage(QString("[完结回传] 重试耗尽 order=%1 retry=%2\n请检查完结回传地址是否可达：%3")
                .arg(orderCode).arg(retryCount)
                .arg(ConfigManager::instance()->config().activeEndFeedbackUrl()), true);

            // ★ 记录异常到 exception_record 表
            if (m_pSortingDb)
            {
                ExceptionRecord ex;
                ex.type      = QString::fromUtf8("完结回传失败");
                ex.orderCode = orderCode;
                ex.epc       = "";
                ex.sku       = "";
                ex.reason    = QString("完结回传（H8）重试%1次后仍失败 body=%2")
                                  .arg(retryCount).arg(body.left(200));
                ex.time      = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
                m_pSortingDb->insertException(ex);
            }

            // ★ 2026-09-06 归属防护：该波次已非当前内存波次时，仅 DB 终态化，不动内存/定时器/不通知 UI
            bool bIsCurrent = m_pWaveMgr && m_pWaveMgr->orderCode() == orderCode;
            if (!bIsCurrent)
            {
                HTTP_LOG_INFO("完结回传失败耗尽（H8） 但该波次已非当前内存波次 order=%s（仅更新数据库，终态化）",
                    orderCode.toLocal8Bit().data());
                emit logMessage(QString("[完结回传] order=%1 补发失败已归档（该波次已切出，切回后可再次重传）")
                    .arg(orderCode), true);
                m_pSortingDb->updateWaveStatus(orderCode, WAVE_FINISHED);
                m_pSortingDb->archiveWave(orderCode);
                return;
            }

            // ★ 波次终结：完结流程结束即 FINISHED（不再异常挂起阻塞工作流）
            //   failed 的 H8 报文保留在 outbox_end，由「重传任务完结(H8)」按钮手动补发
            if (m_pWaveMgr)
            {
                m_pWaveMgr->setState(WAVE_FINISHED);
                HTTP_LOG_INFO("完结回传失败（H8） 波次终结 ENDING→FINISHED order=%s（failed报文可手动重传补发）",
                    orderCode.toLocal8Bit().data());
            }

            // 收尾：存档 + 清空运行时状态（★ 2026-09-07 完结不清空容器绑定，绑定保留于数据库）
            if (m_pSortingDb)
                m_pSortingDb->archiveWave(orderCode);
            {
                std::lock_guard<std::mutex> lock(m_gridRecordMutex);
                m_gridSortRecords.clear();
            }
            {
                std::lock_guard<std::mutex> lock(m_gridCountMutex);
                m_gridSortedCount.clear();
            }

            // ★ 2026-09-02：H8 会话已结束，停止兜底定时器
            if (m_endSessionTimer) m_endSessionTimer->stop();

            // ★ 重试耗尽后停止 H8 完结回传定时器（已标记 failed，不再需要轮询；手动补发走面板）
            if (m_outboxEndTimer) m_outboxEndTimer->stop();
            HTTP_LOG_INFO("完结回传失败（H8） Outbox重试定时器(H8)已停止 order=%s",
                orderCode.toLocal8Bit().data());

            // ★ 通知 MainWindow：完结回传处理完毕（重试耗尽），可以停止服务
            emit endReportFinished();
        }
        else
        {
            // 未耗尽：更新重试信息，等待 pollOutboxEnd 调度
            HTTP_LOG_WARN("完结回传失败（H8） 将重试 order=%s retry=%d/%d",
                orderCode.toLocal8Bit().data(),
                retryCount + 1, OUTBOX_RETRY_MAX_DEFAULT);
            // ★ 2026-09-04：UI 提示明确"回传地址不可达"排查方向
            emit logMessage(QString("[完结回传] 回传失败 将重试 order=%1 retry=%2/%3\n请检查完结回传地址是否可达：%4")
                .arg(orderCode).arg(retryCount + 1).arg(OUTBOX_RETRY_MAX_DEFAULT)
                .arg(ConfigManager::instance()->config().activeEndFeedbackUrl()), true);
        }
    }
}

// ============================================================================
// pollOutboxEnd — 完结回传 Outbox 重试调度（H8，T-S6-03）
// 定时扫描 outbox_end 表中 status='pending' 且 next_retry <= now 的消息
// 重新发送到 WMS
// ★ 仅重试当前波次的消息，非当前波次的消息标记为 cancelled 跳过
// ============================================================================
void HttpServer::pollOutboxEnd()
{
    if (!m_pSortingDb) return;

    QVector<OutboxRecord> pending = m_pSortingDb->getPendingOutboxEnd(OUTBOX_POLL_BATCH_SIZE);
    if (pending.isEmpty()) return;

    HTTP_LOG_INFO("完结回传 Outbox 重试调度（H8） 扫描到 %d 条待重试消息", pending.size());

    // ★ 当前波次 orderCode，用于过滤非当前波次的旧消息
    QString currentOrderCode = m_pWaveMgr ? m_pWaveMgr->orderCode() : QString();

    for (const OutboxRecord& msg : pending)
    {
        // ★ 非当前波次的消息：标记为 cancelled，跳过重试
        //   防止软件重启后发送上一波次未回传的完结数据
        if (currentOrderCode.isEmpty() || msg.orderCode != currentOrderCode)
        {
            HTTP_LOG_INFO("完结回传 Outbox 跳过非当前波次消息（H8） msgOrder=%s currentOrder=%s",
                msg.orderCode.toLocal8Bit().data(),
                currentOrderCode.toLocal8Bit().data());
            if (m_pSortingDb)
                m_pSortingDb->updateOutboxEndStatus(msg.msgId, "cancelled",
                    QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
            continue;
        }

        // 解析 payload
        QJsonDocument doc = QJsonDocument::fromJson(msg.payload.toUtf8());
        if (doc.isNull() || !doc.isObject())
        {
            HTTP_LOG_WARN("完结回传 Outbox payload解析失败（H8）");
            if (m_pSortingDb)
                m_pSortingDb->updateOutboxEndStatus(msg.msgId, "failed",
                    QDateTime::currentDateTime().addSecs(OUTBOX_RETRY_INTERVAL_SEC)
                        .toString("yyyy-MM-dd HH:mm:ss"));
            continue;
        }

        QJsonObject payload = doc.object();

        // 更新重试信息（★ 2026-09-02：H8 用快速重试间隔 5s，保证失败后尽快重发，
        //   避免旧逻辑 +30s 造成"结束任务"等待窗口内长时间无重试）
        QDateTime now = QDateTime::currentDateTime();
        QString nextRetry = now.addMSecs(OUTBOX_RETRY_INTERVAL_FAST_MS)
                               .toString("yyyy-MM-dd HH:mm:ss");
        m_pSortingDb->updateOutboxEndStatus(msg.msgId, "pending", nextRetry);

        HTTP_LOG_INFO("完结回传 Outbox 重试发送（H8） order=%s retry=%d",
            msg.orderCode.toLocal8Bit().data(),
            msg.retryCount + 1);

        // 重新发送
        sendEndToWms(msg.msgId, payload);
    }
}

// ============================================================================
// getReconciliation — 波次对账（T-S8-01/02，需求 §14.2）
// 执行计划/实分/满箱回传（H7）累计/完结回传（H8）状态对账，差异超过阈值时告警
// ============================================================================
WaveReconciliation HttpServer::getReconciliation() const
{
    WaveReconciliation r;
    if (!m_pWaveMgr)
    {
        WCS_WARN("[对账] WaveManager未初始化");
        return r;
    }

    r = m_pWaveMgr->reconcile();

    // 从 DB 补充满箱回传（H7）累计和未处理异常计数
    if (m_pSortingDb)
    {
        // 按 orderCode 查询 Outbox 消息，统计满箱回传（H7）成功/待发送
        QVector<OutboxRecord> outboxList = m_pSortingDb->getOutboxByOrderCode(r.orderCode);
        int fullboxTotal = 0, endReportTotal = 0;
        for (const OutboxRecord& msg : outboxList)
        {
            if (msg.boxcode.isEmpty())
            {
                endReportTotal++;
                continue; // 跳过完结回传（H8）消息
            }
            fullboxTotal++;
            if (msg.status == "success")
            {
                r.fullboxSuccessCount++;
                WCS_INFO("[对账] 满箱回传成功（H7） order=%s boxcode=%s retry=%d",
                    r.orderCode.toLocal8Bit().data(),
                    msg.boxcode.toLocal8Bit().data(), msg.retryCount);
            }
            else if (msg.status == "pending" || msg.status == "failed")
            {
                r.fullboxPendingCount++;
                WCS_WARN("[对账] 满箱回传未完成（H7） order=%s boxcode=%s status=%s retry=%d",
                    r.orderCode.toLocal8Bit().data(),
                    msg.boxcode.toLocal8Bit().data(), msg.status.toLocal8Bit().data(),
                    msg.retryCount);
            }
        }

        WCS_INFO("[对账] Outbox统计 order=%s 满箱回传总数=%d 满箱回传成功=%d 满箱回传待处理=%d 完结回传总数=%d",
            r.orderCode.toLocal8Bit().data(), fullboxTotal, r.fullboxSuccessCount,
            r.fullboxPendingCount, endReportTotal);

        // 查询未处理异常
        QVector<ExceptionRecord> exceptions = m_pSortingDb->getOpenExceptions(r.orderCode);
        r.unhandledException = exceptions.size();
        if (r.unhandledException > 0)
        {
            WCS_WARN("[对账] 未处理异常 order=%s count=%d",
                r.orderCode.toLocal8Bit().data(), r.unhandledException);
            // 打印前3条异常详情
            for (int i = 0; i < qMin(exceptions.size(), 3); ++i)
            {
                WCS_WARN("[对账] 异常详情[%d] type=%s epc=%s reason=%s time=%s",
                    i, exceptions[i].type.toLocal8Bit().data(),
                    exceptions[i].epc.toLocal8Bit().data(),
                    exceptions[i].reason.toLocal8Bit().data(),
                    exceptions[i].time.toLocal8Bit().data());
            }
        }
    }

    // 差异告警（T-S8-02）
    if (r.hasDiff())
    {
        WCS_WARN("[对账] 差异告警 order=%s plan=%d sorted=%d exc=%d recv=%d 满箱回传成功=%d 满箱回传待处理=%d unhandled=%d status=%s diff=%s",
            r.orderCode.toLocal8Bit().data(), r.planQty, r.sortedQty, r.exceptionQty,
            r.totalRecv, r.fullboxSuccessCount, r.fullboxPendingCount, r.unhandledException,
            r.statusText.toLocal8Bit().data(), r.diffSummary().toLocal8Bit().data());
    }
    else
    {
        WCS_INFO("[对账] 无差异 order=%s plan=%d sorted=%d exc=%d 满箱回传成功=%d status=%s",
            r.orderCode.toLocal8Bit().data(), r.planQty, r.sortedQty, r.exceptionQty,
            r.fullboxSuccessCount, r.statusText.toLocal8Bit().data());
    }

    return r;
}

// ============================================================================
// handleRfidCarNumReport — RFID 主动推送 EPC→barcode+carNum 映射
// 请求格式: {"data":[{"epc":"xxx","barcode":"xxx","carNum":"002"},...]}
// 存入 EpcCache，后续 PLC 发送时通过回调查询小车号
// ============================================================================
QJsonObject HttpServer::handleRfidCarNumReport(const QJsonObject& body)
{
    QJsonArray dataArr = body["data"].toArray();
    if (dataArr.isEmpty())
    {
        HTTP_LOG_WARN("RFID推送 数据为空");
        QJsonObject r;
        r["code"] = "500";
        r["message"] = "data为空";
        return r;
    }

    // ★ 2026-09-05：接收即查 —— EPC→SKU 绑定查询与主线工作流互不影响（只要服务启动收到 EPC 就查绑定），
    //   - 分拣中(WAVE_SORTING)：存储 + 查询 + 发送 PLC（正常处理）
    //   - 其他状态：存储 + 查询绑定（提前备好 SKU），仅不发送 PLC（发送限分拣中）
    int waveStatus = m_pWaveMgr ? m_pWaveMgr->status() : -1;
    bool bSorting = (waveStatus == WAVE_SORTING);
    if (!bSorting)
    {
        HTTP_LOG_WARN("RFID推送 已接收但非SORTING状态，仅存储不处理 status=%d(%s) orderCode=%s",
            waveStatus, WaveSnapshot::statusToString(waveStatus).toLocal8Bit().data(),
            m_pWaveMgr ? m_pWaveMgr->orderCode().toLocal8Bit().data() : "(null)");
        emit logMessage(QString("[RFID] 非分拣中状态(status=%1)，数据已接收并存储，暂不处理")
            .arg(WaveSnapshot::statusToString(waveStatus)), true);
    }

    // 解析每条 EPC→barcode+carNum 映射，存入 EpcCache（T-S4-04 TTL缓存）
    int newCount = 0;
    int carCount = 0;
    int skuNeedQueryCount = 0;  // ★ 需要查询 SKU 的 EPC 数量
    QStringList epcNeedSkuQuery;  // ★ 需要查询 SKU 的 EPC 列表
    QMap<QString, QPair<QString, QString>> batchMap;  // epc → {barcode, carNum}
    QMap<QString, QString> epcSeqMap;                 // ★ 2026-09-05 epc → 推送流水号（保存追溯）
    for (const QJsonValue& val : dataArr)
    {
        QJsonObject item = val.toObject();
        QString epc     = item["epc"].toString().trimmed();
        QString barcode = item["barcode"].toString().trimmed();  // ★ barcode=SKU编码（客户确认 2026-08-14）
        QString carNum  = item["carNum"].toString().trimmed();   // ★ RFID 小车号
        QString seq     = item["seq"].toString().trimmed();      // ★ 2026-09-05 RFID 推送流水号

        if (epc.isEmpty()) continue;
        if (!seq.isEmpty()) epcSeqMap[epc] = seq;   // ★ 2026-09-05 保存流水号（供日志/发送追溯）

        // ★ 纠正: 接受无 barcode 的 EPC+carNum（RFID 只推送 EPC+小车号时 barcode 为空）
        //   无论 barcode 是否有值，都存入 EpcCache
        batchMap[epc] = {barcode, carNum};
        newCount++;
        if (!carNum.isEmpty() && carNum != CAR_NUM_STR(DEFAULT_CAR_NUM)) carCount++;

        // ★ 2026-09-05：接收即查 —— SKU 绑定查询与主线工作流互不影响，不依赖「分拣中」状态；
        //   仅「发送 PLC」限分拣中（由下方发送循环 + trySendToPlcForEpc 双重把关）
        bool needSkuQuery = false;
        if (m_pEpcCache && m_pEpcCache->isReadyForPlc(epc))
        {
            needSkuQuery = false;                 // 已绑定过且未过期 → 无需重复查询
        }
        else if (barcode.isEmpty())
        {
            needSkuQuery = true;                  // TCP 推送 barcode 恒空 → 需要查询
        }
        else if (m_pEpcCache)
        {
            // barcode 有值但可能 skuBound 还未设置（旧缓存未绑定）
            needSkuQuery = !m_pEpcCache->isReadyForPlc(epc);
        }

        if (needSkuQuery)
        {
            // ★ 防重查：同一 EPC 不重复提交 SKU 查询
            if (!m_pendingSkuQuery.contains(epc))
            {
                epcNeedSkuQuery.append(epc);
                m_pendingSkuQuery.insert(epc);
                skuNeedQueryCount++;
                HTTP_LOG_INFO("RFID推送 SKU查询入队 epc=%s carNum=%s seq=%s status=%d pendingSize=%d",
                    epc.toLocal8Bit().data(), carNum.toLocal8Bit().data(),
                    seq.toLocal8Bit().data(), waveStatus, m_pendingSkuQuery.size());
            }
            else
            {
                // ★ 日志: 记录被防重拦截的 EPC，方便排查重复推送
                int retryCount = m_skuQueryRetryCount.value(epc, 0);
                HTTP_LOG_INFO("RFID推送 SKU查询已提交跳过(防重) epc=%s carNum=%s seq=%s retryCount=%d pendingSize=%d",
                    epc.toLocal8Bit().data(), carNum.toLocal8Bit().data(),
                    seq.toLocal8Bit().data(), retryCount, m_pendingSkuQuery.size());
            }
        }
        else if (bSorting)
        {
            // ★ 日志: 分拣中且 SKU 已就绪，无需查询，直接进入下方发送流程
            HTTP_LOG_INFO("RFID推送 SKU已就绪无需查询 epc=%s barcode=%s carNum=%s seq=%s",
                epc.toLocal8Bit().data(), barcode.toLocal8Bit().data(), carNum.toLocal8Bit().data(),
                seq.toLocal8Bit().data());
        }

        // ★ 非分拣中状态：仅存储 + 查询，不发送 PLC（保留逐条日志便于核对「收到但未发送」）
        if (!bSorting)
        {
            HTTP_LOG_INFO("RFID推送 仅存储不处理(非分拣中) epc=%s carNum=%s seq=%s status=%d %s",
                epc.toLocal8Bit().data(), carNum.toLocal8Bit().data(),
                seq.toLocal8Bit().data(), waveStatus,
                needSkuQuery ? "(SKU绑定查询已提交/排队)" : "(SKU已绑定，待开始分拣后发送)");
        }
    }

    // ★ 批量写入 EpcCache（含 carNum，TTL 自动管理，保留已有 SKU 绑定数据）
    if (m_pEpcCache && !batchMap.isEmpty())
    {
        m_pEpcCache->setBatchWithCar(batchMap);
    }

    // ★ 2026-09-05：保存推送流水号（写入 EpcCache 随条目保存，供发送/异常日志追溯；随 TTL 清理）
    if (m_pEpcCache && !epcSeqMap.isEmpty())
    {
        for (auto it2 = epcSeqMap.constBegin(); it2 != epcSeqMap.constEnd(); ++it2)
            m_pEpcCache->setSeq(it2.key(), it2.value());
    }

    // ★ 立即触发 SKU 查询（使用新线程异步执行，不阻塞 RFID 推送响应）
    //   接收即查：只要服务启动收到 EPC 就查绑定（与主线工作流状态互不影响）
    if (!epcNeedSkuQuery.isEmpty())
    {
        HTTP_LOG_INFO("RFID推送 触发SKU查询 epcCount=%d epcList=[%s]",
            epcNeedSkuQuery.size(), epcNeedSkuQuery.join(",").toLocal8Bit().data());
        emit logMessage(QString("[RFID] 触发 SKU 查询 %1 条").arg(epcNeedSkuQuery.size()));
        if (m_pHttpClient)
        {
            // ★ HttpClient::queryRfidBinding 内部使用 QNetworkAccessManager 异步请求
            //   完成后通过 rfidBindingResult 信号回调 → onRfidBindingResult → trySendToPlcForEpc
            m_pHttpClient->queryRfidBinding(epcNeedSkuQuery);
        }
    }

    // ★ 逐条检查：EPC 是否已就绪（SKU 已绑定 + carNum 已获取）
    //   就绪则立即发送 PLC 指令
    int sentCount = 0;
    int skuNotFoundCount = 0;
    for (auto it = batchMap.constBegin(); it != batchMap.constEnd(); ++it)
    {
        QString epc = it.key();
        // ★ 非分拣中状态不做处理（仅存储 + 日志）
        if (!bSorting)
            continue;

        bool isReady = m_pEpcCache && m_pEpcCache->isReadyForPlc(epc);
        
        if (!isReady)
        {
            skuNotFoundCount++;
            HTTP_LOG_WARN("RFID推送 EPC已到但SKU未绑定 epc=%s barcode=%s carNum=%s (等待RFID SKU查询结果)", 
                epc.toLocal8Bit().data(), 
                it.value().first.toLocal8Bit().data(),
                it.value().second.toLocal8Bit().data());
        }

        // ★ 防重复：已发送的 EPC 跳过（避免 scheduleNotReadyRetry 延迟重试时重复发送）
        if (m_sentEpcs.contains(epc))
        {
            HTTP_LOG_INFO("RFID推送 已发送跳过 epc=%s (已在 m_sentEpcs 中)", 
                epc.toLocal8Bit().data());
            sentCount++;
            continue;
        }

        if (trySendToPlcForEpc(epc))
            sentCount++;
    }

    HTTP_LOG_INFO("RFID推送已存储 total=%d new=%d carNum=%d sent=%d skuNotFound=%d skuQuery=%d bSorting=%d",
        dataArr.size(), newCount, carCount, sentCount, skuNotFoundCount, skuNeedQueryCount, bSorting ? 1 : 0);
    emit logMessage(QString("[RFID] 小车号推送 接收%1条 有效%2条 小车号%3条 已发送PLC%4条 SKU未绑定%5条 触发SKU查询%6条 %7")
        .arg(dataArr.size()).arg(newCount).arg(carCount).arg(sentCount).arg(skuNotFoundCount).arg(skuNeedQueryCount)
        .arg(bSorting ? QString() : QString::fromUtf8("(非分拣中：已存储+已查绑定，未发送PLC)")));

    QJsonObject r;
    r["code"] = "200";
    r["message"] = "OK";
    r["stored"] = newCount;
    r["processed"] = bSorting;
    return r;
}

// ============================================================================
// setHttpClient — 设置 HTTP 客户端，连接 RFID 绑定查询结果信号
// ============================================================================
void HttpServer::setHttpClient(HttpClient* client)
{
    m_pHttpClient = client;
    if (m_pHttpClient)
    {
        connect(m_pHttpClient, &HttpClient::rfidBindingResult, this,
            &HttpServer::onRfidBindingResult, Qt::QueuedConnection);
    }
}

// ============================================================================
// submitEpcBindingQueries — 波次下发后提交 EPC 绑定查询
// 向 RFID 查询 EPC→barcode 映射，异步非阻塞，不阻塞主流程
// ============================================================================
void HttpServer::submitEpcBindingQueries(const QStringList& epcList)
{
    if (!m_pHttpClient)
    {
        HTTP_LOG_WARN("EPC绑定查询 HttpClient未设置，跳过 epcCount=%d", epcList.size());
        return;
    }
    m_pHttpClient->queryRfidBinding(epcList);
}

// ============================================================================
// scheduleSkuQueryRetry — SKU 查询失败后延迟重试
// 使用 QTimer::singleShot 延迟 SKU_QUERY_RETRY_INTERVAL_MS 毫秒后重新发起查询
// 重试时重新加入 m_pendingSkuQuery 防重标记
// ============================================================================
void HttpServer::scheduleSkuQueryRetry(const QStringList& epcList)
{
    if (epcList.isEmpty() || !m_pHttpClient)
        return;

    HTTP_LOG_INFO("SKU查询延迟重试 epcCount=%d delayMs=%d epcList=[%s]",
        epcList.size(), SKU_QUERY_RETRY_INTERVAL_MS,
        epcList.join(",").toLocal8Bit().data());
    emit logMessage(QString("[EPC] SKU查询延迟重试 %1条 间隔%2ms").arg(epcList.size()).arg(SKU_QUERY_RETRY_INTERVAL_MS));

    // ★ 延迟后重新发起 SKU 查询
    QTimer::singleShot(SKU_QUERY_RETRY_INTERVAL_MS, this, [this, epcList]() {
        // 重新加入防重标记
        for (const QString& epc : epcList)
        {
            m_pendingSkuQuery.insert(epc);
        }

        HTTP_LOG_INFO("SKU查询重试发起 epcCount=%d epcList=[%s]",
            epcList.size(), epcList.join(",").toLocal8Bit().data());
        emit logMessage(QString("[EPC] SKU查询重试发起 %1条").arg(epcList.size()));

        if (m_pHttpClient)
        {
            m_pHttpClient->queryRfidBinding(epcList);
        }
    });
}

// ============================================================================
// scheduleNotReadyRetry — 未就绪(carNum未到)时延迟重试
// 使用 QTimer::singleShot 延迟 NOT_READY_RETRY_INTERVAL_MS 毫秒后重新检查
// 如果 carNum 已到 → 发送 PLC；如果仍不到 → 递增重试计数
// 超过 NOT_READY_RETRY_MAX 次 → 写入异常记录表
// 已发送的 EPC（m_sentEpcs）跳过，防重复发送
// ============================================================================
void HttpServer::scheduleNotReadyRetry(const QString& epc)
{
    int retryCount = m_notReadyRetryCount.value(epc, 0);
    if (retryCount > NOT_READY_RETRY_MAX)
    {
        // ★ 超过最大重试次数，写入异常记录表
        HTTP_LOG_ERROR("未就绪重试 最终失败 epc=%s retry=%d/%d 写入异常记录",
            epc.toLocal8Bit().data(), retryCount, NOT_READY_RETRY_MAX);
        emit logMessage(QString("[异常] 未就绪最终失败 epc=%1 carNum未到 已重试%2次").arg(epc).arg(retryCount));
        m_notReadyRetryCount.remove(epc);

        if (m_pSortingDb)
        {
            ExceptionRecord ex;
            ex.type      = QString::fromUtf8("carNum未到");
            ex.orderCode = m_pWaveMgr->orderCode();
            ex.epc       = epc;
            ex.sku       = epc;
            ex.reason    = QString::fromUtf8("SKU已绑定但carNum推送未到达，已重试%1次后放弃").arg(retryCount);
            m_pSortingDb->insertException(ex);
        }
        return;
    }

    HTTP_LOG_INFO("未就绪重试 调度 epc=%s retry=%d/%d delayMs=%d",
        epc.toLocal8Bit().data(), retryCount, NOT_READY_RETRY_MAX,
        NOT_READY_RETRY_INTERVAL_MS);

    // ★ 延迟后重新检查
    QTimer::singleShot(NOT_READY_RETRY_INTERVAL_MS, this, [this, epc]() {
        // ★ 防重复：如果已发送，跳过
        if (m_sentEpcs.contains(epc))
        {
            HTTP_LOG_INFO("未就绪重试 已发送跳过 epc=%s (carNum已到且已发送PLC)",
                epc.toLocal8Bit().data());
            m_notReadyRetryCount.remove(epc);
            return;
        }

        if (!m_pEpcCache)
        {
            m_notReadyRetryCount.remove(epc);
            return;
        }

        // ★ 超时检查：从 RFID 推送到达起算，超过 PLC_SEND_TIMEOUT_MS 则停止重试
        if (m_pEpcCache->isSendTimeout(epc))
        {
            qint64 elapsed = m_pEpcCache->getElapsedMs(epc);
            HTTP_LOG_WARN("未就绪重试 已超时 epc=%s elapsed=%lldms 超时阈值=%dms 入异常格口",
                epc.toLocal8Bit().data(), elapsed, PLC_SEND_TIMEOUT_MS);
            emit logMessage(QString("[异常] 未就绪超时 epc=%1 耗时%2ms 入异常格口").arg(epc).arg(elapsed));
            m_notReadyRetryCount.remove(epc);

            // 写入异常记录表（等待carNum超时，入异常格口）
            if (m_pSortingDb)
            {
                ExceptionRecord ex;
                ex.type      = QString::fromUtf8("未就绪超时");
                ex.orderCode = m_pWaveMgr->orderCode();
                ex.epc       = epc;
                ex.sku       = m_pEpcCache->get(epc);   // ★ 取真实SKU，查不到则留空，不伪造数据
                ex.reason    = QString::fromUtf8("等待carNum超时，RFID推送→重试耗时%1ms，超过阈值%2ms，入异常格口")
                    .arg(elapsed).arg(PLC_SEND_TIMEOUT_MS);
                m_pSortingDb->insertException(ex);
            }
            return;
        }

        bool isReady = m_pEpcCache->isReadyForPlc(epc);
        HTTP_LOG_INFO("未就绪重试 检查 epc=%s isReady=%d retryCount=%d",
            epc.toLocal8Bit().data(), isReady,
            m_notReadyRetryCount.value(epc, 0));

        if (isReady)
        {
            // ★ carNum 已到，发送 PLC
            if (trySendToPlcForEpc(epc))
            {
                HTTP_LOG_INFO("未就绪重试 发送成功 epc=%s", epc.toLocal8Bit().data());
                m_notReadyRetryCount.remove(epc);
            }
            else
            {
                HTTP_LOG_WARN("未就绪重试 发送失败 epc=%s (就绪但发送PLC失败)",
                    epc.toLocal8Bit().data());
                // 继续重试
                int curRetry = m_notReadyRetryCount.value(epc, 0) + 1;
                m_notReadyRetryCount[epc] = curRetry;
                scheduleNotReadyRetry(epc);
            }
        }
        else
        {
            // ★ 仍未就绪，递增重试计数并继续
            int curRetry = m_notReadyRetryCount.value(epc, 0) + 1;
            m_notReadyRetryCount[epc] = curRetry;
            HTTP_LOG_INFO("未就绪重试 仍未就绪 epc=%s retry=%d/%d",
                epc.toLocal8Bit().data(), curRetry, NOT_READY_RETRY_MAX);
            scheduleNotReadyRetry(epc);
        }
    });
}

// ============================================================================
// onRfidBindingResult — RFID 绑定查询结果回调
// 将 EPC→barcode 映射存入 EpcCache（skuBound=true），
// 然后检查每条 EPC 是否已就绪（carNum 也已获取），就绪则发送 PLC
// ============================================================================
void HttpServer::onRfidBindingResult(const QMap<QString, QString>& epcBarcodeMap)
{
    if (epcBarcodeMap.isEmpty())
    {
        // ★ 日志: 记录超时/失败的 EPC 列表，方便排查
        QStringList pendingEpcs = m_pendingSkuQuery.values();
        HTTP_LOG_WARN("EPC绑定查询结果为空（RFID超时或失败） pendingEpcs=[%s] count=%d",
            pendingEpcs.join(",").toLocal8Bit().data(), pendingEpcs.size());
        emit logMessage(QString("[EPC] SKU-EPC 绑定查询超时/失败 待处理%1条 EPC=%2")
            .arg(pendingEpcs.size()).arg(pendingEpcs.join(",")));

        // ★ 修复: 清除防重标记，否则 EPC 永远无法重试
        //   遍历 m_pendingSkuQuery 中所有 EPC，逐个判断是否重试
        QStringList retryEpcs;  // 需要重试的 EPC
        for (const QString& epc : pendingEpcs)
        {
            int retryCount = m_skuQueryRetryCount.value(epc, 0) + 1;
            m_skuQueryRetryCount[epc] = retryCount;
            m_pendingSkuQuery.remove(epc);  // ★ 清除防重标记

            if (retryCount <= SKU_QUERY_MAX_RETRY)
            {
                // 未超过最大重试次数，加入重试列表
                retryEpcs.append(epc);
                HTTP_LOG_INFO("SKU查询重试 epc=%s retry=%d/%d",
                    epc.toLocal8Bit().data(), retryCount, SKU_QUERY_MAX_RETRY);
            }
            else
            {
                // ★ 超过最大重试次数，写入异常记录表
                HTTP_LOG_ERROR("SKU查询最终失败 epc=%s retry=%d/%d 写入异常记录",
                    epc.toLocal8Bit().data(), retryCount, SKU_QUERY_MAX_RETRY);
                emit logMessage(QString("[异常] SKU查询最终失败 epc=%1 已重试%2次").arg(epc).arg(retryCount));
                m_skuQueryRetryCount.remove(epc);  // 清理重试计数

                // 写入异常记录表
                if (m_pSortingDb)
                {
                    ExceptionRecord ex;
                    ex.type      = QString::fromUtf8("SKU查询超时");
                    ex.orderCode = m_pWaveMgr->orderCode();
                    ex.epc       = epc;
                    ex.sku       = epc;
                    ex.reason    = QString::fromUtf8("RFID SKU-EPC绑定查询超时/失败，已重试%1次").arg(retryCount);
                    m_pSortingDb->insertException(ex);
                }
            }
        }

        // ★ 延迟重试（3 秒后重新发起 SKU 查询）
        if (!retryEpcs.isEmpty())
        {
            scheduleSkuQueryRetry(retryEpcs);
        }
        return;
    }

    int cacheSizeBefore = m_pEpcCache ? m_pEpcCache->size() : 0;
    HTTP_LOG_INFO("EPC绑定查询结果接收 匹配数=%d 缓存当前大小=%d", epcBarcodeMap.size(), cacheSizeBefore);
    emit logMessage(QString("[EPC] SKU-EPC 绑定查询完成 匹配%1条 缓存已有%2条")
        .arg(epcBarcodeMap.size()).arg(cacheSizeBefore));

    // 逐条存入 EpcCache（skuBound=true），并检查是否就绪
    int readyCount = 0;
    int notReadyCount = 0;
    int skuInGridCount = 0;
    int skuNotInGridCount = 0;
    for (auto it = epcBarcodeMap.constBegin(); it != epcBarcodeMap.constEnd(); ++it)
    {
        QString epc     = it.key();
        QString sku     = it.value();  // ★ RFID 返回的 SKU 编码（barcode=SKU）

        // ★ 纠正: 检查 SKU 在 GridBuffer 中是否存在（GridBuffer key 是 SKU）
        bool skuInGrid = m_pBuffer && !m_pBuffer->get(sku).gridNum.isEmpty();
        if (skuInGrid)
            skuInGridCount++;
        else
        {
            skuNotInGridCount++;
            HTTP_LOG_WARN("EPC绑定 SKU未在GridBuffer中找到 epc=%s sku=%s (H4未下发此SKU)",
                epc.toLocal8Bit().data(), sku.toLocal8Bit().data());
        }

        if (m_pEpcCache)
        {
            m_pEpcCache->setSkuBinding(epc, sku);
            HTTP_LOG_INFO("EPC绑定 已存入EpcCache epc=%s sku=%s skuBound=true skuInGrid=%d",
                epc.toLocal8Bit().data(), sku.toLocal8Bit().data(), skuInGrid);
        }

        // ★ 清除防重标记（SKU 查询已完成，允许后续重查）
        m_pendingSkuQuery.remove(epc);

        // 检查是否已就绪（carNum 也已获取）
        bool isReady = m_pEpcCache && m_pEpcCache->isReadyForPlc(epc);
        HTTP_LOG_INFO("EPC绑定 就绪检查 epc=%s isReady=%d", epc.toLocal8Bit().data(), isReady);

        if (isReady)
        {
            if (trySendToPlcForEpc(epc))
            {
                readyCount++;
                HTTP_LOG_INFO("EPC绑定 已发送PLC epc=%s", epc.toLocal8Bit().data());
            }
            else
            {
                HTTP_LOG_WARN("EPC绑定 发送PLC失败 epc=%s (就绪但发送失败，可能超时或格口禁用)",
                    epc.toLocal8Bit().data());
            }
        }
        else
        {
            notReadyCount++;
            // ★ 超时检查：如果已超过 PLC_SEND_TIMEOUT_MS，直接入异常，不再重试
            if (m_pEpcCache && m_pEpcCache->isSendTimeout(epc))
            {
                qint64 elapsed = m_pEpcCache->getElapsedMs(epc);
                HTTP_LOG_WARN("EPC绑定 未就绪且已超时 epc=%s elapsed=%lldms 入异常格口",
                    epc.toLocal8Bit().data(), elapsed);
                emit logMessage(QString("[异常] 未就绪超时 epc=%1 耗时%2ms 入异常格口").arg(epc).arg(elapsed));
                if (m_pSortingDb)
                {
                    ExceptionRecord ex;
                    ex.type      = QString::fromUtf8("发送超时");
                    ex.orderCode = m_pWaveMgr->orderCode();
                    ex.epc       = epc;
                    ex.sku       = sku;
                    ex.reason    = QString::fromUtf8("SKU已绑定但carNum未到，RFID推送→回调耗时%1ms，超过阈值%2ms，入异常格口")
                        .arg(elapsed).arg(PLC_SEND_TIMEOUT_MS);
                    m_pSortingDb->insertException(ex);
                }
                continue;
            }
            // ★ SKU 已绑定但 carNum 未到，启动延迟重试机制
            //   避免 carNum 推送丢失导致 EpcCache TTL 过期后数据静默丢失
            int retryCount = m_notReadyRetryCount.value(epc, 0) + 1;
            m_notReadyRetryCount[epc] = retryCount;
            HTTP_LOG_INFO("EPC绑定 未就绪 epc=%s retry=%d/%d (等待RFID推送carNum)",
                epc.toLocal8Bit().data(), retryCount, NOT_READY_RETRY_MAX);
            emit logMessage(QString("[EPC] 未就绪 epc=%1 carNum未到 重试%2/%3")
                .arg(epc).arg(retryCount).arg(NOT_READY_RETRY_MAX));
            scheduleNotReadyRetry(epc);
        }
    }

    int cacheSizeAfter = m_pEpcCache ? m_pEpcCache->size() : 0;
    HTTP_LOG_INFO("EPC绑定完成 匹配=%d 就绪=%d 未就绪=%d SKU在Grid=%d SKU不在Grid=%d 缓存大小=%d→%d",
        epcBarcodeMap.size(), readyCount, notReadyCount, skuInGridCount, skuNotInGridCount,
        cacheSizeBefore, cacheSizeAfter);
    emit logMessage(QString("[EPC] SKU-EPC 绑定完成 匹配%1条 就绪%2条 未就绪%3条 SKU在Grid%4/不在%5 (缓存%6→%7)")
        .arg(epcBarcodeMap.size()).arg(readyCount).arg(notReadyCount)
        .arg(skuInGridCount).arg(skuNotInGridCount)
        .arg(cacheSizeBefore).arg(cacheSizeAfter));
}

// ============================================================================
// trySendToPlcForEpc — 尝试发送单条 EPC 到 PLC（就绪检查）
// 就绪条件：SKU 已绑定（skuBound=true）+ carNum 已获取
// 通过 EpcCache 获取 barcode + carNum，再从 GridBuffer 获取 gridNum
// 返回 true 表示已发送 PLC 指令
// ============================================================================
bool HttpServer::trySendToPlcForEpc(const QString& epc)
{
    if (!m_pEpcCache || !m_pPlcMgr || !m_pBuffer)
    {
        HTTP_LOG_WARN("trySendToPlcForEpc 前置条件不满足 epc=%s EpcCache=%d PlcMgr=%d Buffer=%d",
            epc.toLocal8Bit().data(), m_pEpcCache!=nullptr, m_pPlcMgr!=nullptr, m_pBuffer!=nullptr);
        return false;
    }

    // ★ 2026-09-05：发送 PLC 仅限「分拣中」状态（与主线一致）；
    //   SKU 绑定查询不受此限（接收即查已提前完成），此处兜底所有调用路径（含异步查询回调）
    {
        int st = m_pWaveMgr ? m_pWaveMgr->status() : -1;
        if (st != WAVE_SORTING)
        {
            HTTP_LOG_INFO("trySendToPlcForEpc 非分拣中不发送 epc=%s status=%d(%s)",
                epc.toLocal8Bit().data(), st,
                st >= 0 ? WaveSnapshot::statusToString(st).toLocal8Bit().data() : "?");
            return false;
        }
    }

    // 检查是否就绪
    if (!m_pEpcCache->isReadyForPlc(epc))
    {
        HTTP_LOG_INFO("trySendToPlcForEpc 未就绪 epc=%s (skuBound+carNum未同时满足)",
            epc.toLocal8Bit().data());
        return false;
    }

    // 获取 SKU + carNum（EpcCache 中 barcode=SKU 来自 RFID 实时查询）
    QPair<QString, QString> plcData = m_pEpcCache->getPlcData(epc);
    QString sku    = plcData.first;  // RFID 返回的 SKU 编码（用于 GridBuffer 查找格口）
    QString carNum = plcData.second;
    QString seq    = m_pEpcCache->getSeq(epc);   // ★ 2026-09-05 RFID 推送流水号（追溯）
    if (sku.isEmpty())
    {
        HTTP_LOG_WARN("trySendToPlcForEpc SKU为空 epc=%s (EpcCache中barcode为空)",
            epc.toLocal8Bit().data());
        return false;
    }

    // ★ GridBuffer 的 key 是 SKU 编码（H4 下发 inco=SKU）
    //   用 RFID 返回的 SKU 查找格口映射
    GridEntry entry = m_pBuffer->get(sku);
    if (entry.gridNum.isEmpty())
    {
        // ★ 定位失败：输出 GridBuffer 中所有 SKU key，方便对比排查
        const QMap<QString, GridEntry>* pMap = m_pBuffer->activeMap();
        if (pMap)
        {
            QStringList keys;
            for (int i = 0; i < qMin(pMap->size(), 20); ++i)
            {
                auto it = pMap->constBegin() + i;
                keys << it.key();
            }
            HTTP_LOG_WARN("trySendToPlcForEpc SKU未在GridBuffer中找到 epc=%s sku=%s 容器大小=%d 前20个key=[%s]",
                epc.toLocal8Bit().data(), sku.toLocal8Bit().data(), 
                pMap->size(), keys.join(",").toLocal8Bit().data());
        }
        else
        {
            HTTP_LOG_WARN("trySendToPlcForEpc SKU未在GridBuffer中找到 epc=%s sku=%s (GridBuffer为空)",
                epc.toLocal8Bit().data(), sku.toLocal8Bit().data());
        }
        // ★ 写入异常记录表（SKU 不在 H4 下发的格口映射中）
        if (m_pSortingDb)
        {
            ExceptionRecord ex;
            ex.type      = QString::fromUtf8("SKU不在GridBuffer");
            ex.orderCode = m_pWaveMgr->orderCode();
            ex.epc       = epc;
            ex.sku       = sku;
            ex.reason    = QString::fromUtf8("RFID返回SKU=%1，但H4下发未包含此SKU的格口映射").arg(sku);
            m_pSortingDb->insertException(ex);
        }
        return false;
    }

    // 检查 PLC 连接
    if (!m_pPlcMgr->hasConnectedClients())
    {
        HTTP_LOG_WARN("trySendToPlcForEpc PLC未连接 epc=%s sku=%s", 
            epc.toLocal8Bit().data(), sku.toLocal8Bit().data());
        return false;
    }

    // ★ 发送 PLC 指令：{EPC|格口号|小车号}
    //   EPC 来自 RFID 推送，格口号来自 SKU 映射查找，小车号来自 RFID 推送
    QMap<QString, QString> codeGridMap;
    codeGridMap[epc] = entry.gridNum;

    // ★ 超时检查：从 RFID 推送到达起算，超过 PLC_SEND_TIMEOUT_MS 则入异常格口
    if (m_pEpcCache && m_pEpcCache->isSendTimeout(epc))
    {
        qint64 elapsed = m_pEpcCache->getElapsedMs(epc);
        HTTP_LOG_WARN("PLC发送超时 epc=%s sku=%s grid=%s seq=%s elapsed=%lldms 超时阈值=%dms 入异常格口",
            epc.toLocal8Bit().data(), sku.toLocal8Bit().data(),
            entry.gridNum.toLocal8Bit().data(), seq.toLocal8Bit().data(), elapsed, PLC_SEND_TIMEOUT_MS);
        emit logMessage(QString("[异常] 发送超时 epc=%1 耗时%2ms 入异常格口").arg(epc).arg(elapsed));

        // 写入异常记录表（发送超时，入异常格口）
        if (m_pSortingDb)
        {
            ExceptionRecord ex;
            ex.type      = QString::fromUtf8("发送超时");
            ex.orderCode = m_pWaveMgr->orderCode();
            ex.epc       = epc;
            ex.sku       = sku;
            ex.reason    = QString::fromUtf8("RFID推送→PLC发送耗时%1ms，超过阈值%2ms，入异常格口")
                .arg(elapsed).arg(PLC_SEND_TIMEOUT_MS);
            m_pSortingDb->insertException(ex);
        }
        return false;
    }

    // ★ 计时终点：发送动作执行后即结束 PLC_SEND_TIMEOUT_MS 计时（无论成败）
    //   1s 限制衡量的是「收到→发出PLC指令」的延迟，发出则计时结束；
    //   发送结果（成功/失败）由 sendOk 独立判断，与计时互不干扰
    bool sendOk = m_pPlcMgr->sendBatchCodesWithEpcCache(codeGridMap);
    if (m_pEpcCache) m_pEpcCache->markSent(epc);   // ★ 记录 PLC 发送指令时间（计时终点，无论成败）
    qint64 sendElapsed = m_pEpcCache ? m_pEpcCache->getHandleSendMs(epc) : -1;   // 开始处理→发送 耗时
    if (!sendOk)
    {
        // ★ 发送失败（可能因格口禁用或PLC未连接），不标记为已发送
        HTTP_LOG_WARN("PLC发送失败 epc=%s sku=%s grid=%s carNum=%s seq=%s elapsed=%lldms (格口禁用或PLC未连接，不标记已发送)",
            epc.toLocal8Bit().data(), sku.toLocal8Bit().data(),
            entry.gridNum.toLocal8Bit().data(), carNum.toLocal8Bit().data(),
            seq.toLocal8Bit().data(), sendElapsed);
        emit logMessage(QString("[PLC] 发送失败 epc=%1 sku=%2 格口%3 小车%4 seq=%5 耗时%6ms")
            .arg(epc).arg(sku).arg(entry.gridNum).arg(carNum).arg(seq).arg(sendElapsed), true);
        return false;
    }

    HTTP_LOG_INFO("PLC发送成功 epc=%s sku=%s grid=%s carNum=%s seq=%s elapsed=%lldms",
        epc.toLocal8Bit().data(), sku.toLocal8Bit().data(),
        entry.gridNum.toLocal8Bit().data(), carNum.toLocal8Bit().data(),
        seq.toLocal8Bit().data(), sendElapsed);
    emit logMessage(QString("[PLC] 发送 %1 → 格口%2 小车%3 (sku=%4 seq=%5) 耗时%6ms")
        .arg(epc).arg(entry.gridNum).arg(carNum).arg(sku).arg(seq).arg(sendElapsed));

    // ★ 标记已发送，防止重复发送
    m_sentEpcs.insert(epc);

    return true;
}

