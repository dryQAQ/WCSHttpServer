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
#include <QCoreApplication>
#include <QUuid>
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
    m_pSortingDb = new SortingDatabase(); // ★ 分拣记录数据库
    m_pEpcCache  = new EpcCache(RFID_CACHE_TTL_SEC); // ★ S4 EPC短缓存（T-S4-04）

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
    // TODO: 识别码可能为条码或EPC，客户尚未确定（2026-08-04）
    m_pPlcMgr->setLookupCallback([this](const QString& code) -> QString {
        if (!m_pBuffer) return QString();
        GridEntry entry = m_pBuffer->get(code);
        return entry.gridNum;  // 返回 "15" 或 "1,2,3"（多格口逗号分隔）
    });

    // ──── 业务线程池 ────
    {
        AppConfig& cfg = ConfigManager::instance()->config();
        m_pBusinessPool = new Hanchine::ThreadPool(cfg.businessPoolSize);
        HTTP_INFO("业务线程池已创建 threads=%d", cfg.businessPoolSize);
        HTTP_INFO("业务线程池已创建 threads=%d", cfg.businessPoolSize);

        // ★ 专用业务线程池（参考WCSApp分类线程池设计，不同业务用独立池，方便管理内存）

        m_pPlcRecvPool = new Hanchine::ThreadPool(cfg.plcRecvPoolSize);

        HTTP_INFO("PLC反馈接收专用线程池已创建 threads=%d", cfg.plcRecvPoolSize);

        
    }

    // 显式指定 Qt::QueuedConnection：ParseWorker::run() 在独立线程中运行，
    // 使用 AutoConnection 时因 sender/receiver 的 thread() 都在主线程，
    // 但 emit 发生在工作线程，导致信号跨线程传递异常。
    connect(m_pWorker, &ParseWorker::waveParsed, this,
        [this](const QString& orderCode, int skuCount, int orderQty, qint64, const QSet<QString>& recvSet, const QStringList& epcList) {
            m_pWaveMgr->setWaveData(orderCode, orderQty, skuCount);
            m_pWaveMgr->setRecvSet(recvSet);

            // ★ 波次下发后自动检查绑定状态：如果全部绑定已完成，自动推进到 SORTING 状态
            //   解决预绑定场景（先绑定容器再下发波次）下波次停留在 CREATED 无法自动开工的问题
            //   ★ 修复：CREATED→BOUND→SORTING 全链路同步推进，消除"首件落格才开工"的竞态窗口
            //     多批次并发落格时，若批次 N 读到 BOUND 但批次 1 已置 isSortingStarted=true，
            //     批次 N 的落格会被拒绝（"PLC反馈被拒绝 非SORTING状态"），导致分拣记录丢失
            if (m_pWaveMgr->status() == WAVE_CREATED && areAllBindingsComplete())
            {
                m_pWaveMgr->setState(WAVE_BOUND);
                if (m_pWaveMgr->startSorting())
                {
                    HTTP_LOG_INFO("预绑定场景 波次自动推进 CREATED→BOUND→SORTING orderCode=%s bound=%d/%d",
                        orderCode.toLocal8Bit().data(), boundCount(), m_expectedBindCount);
                    emit logMessage(QString("[波次] 自动推进: 已下发→已绑定→分拣中 (预绑定) orderCode=%1").arg(orderCode));
                }
                else
                {
                    HTTP_LOG_WARN("预绑定场景 波次自动开工失败 orderCode=%s bound=%d/%d",
                        orderCode.toLocal8Bit().data(), boundCount(), m_expectedBindCount);
                    emit logMessage(QString("[波次] 自动推进: 已下发→已绑定 (开工失败) orderCode=%1").arg(orderCode));
                }
            }

            // ★ S1 新增：波次数据落库（T-S1-01/03）
            //   持久化 ReturnWave 头 + ReturnWaveItem 明细到 SQLite
            if (m_pSortingDb)
            {
                // 波次头（UPSERT，未分拣时允许覆盖，使用实际波次状态）
                m_pSortingDb->upsertReturnWave(orderCode, orderQty, m_pWaveMgr->status());

                // 波次明细（从 GridBuffer 读取全量 inco→格口映射）
                QVector<ReturnWaveItemRecord> items;
                const QMap<QString, GridEntry>* pMap = m_pBuffer->activeMap();
                if (pMap)
                {
                    for (auto it = pMap->constBegin(); it != pMap->constEnd(); ++it)
                    {
                        ReturnWaveItemRecord rec;
                        rec.orderCode = orderCode;
                        rec.inco      = it.key();
                        // gridNum 可能为 "1,2,3"（一品多格口），拆分为多条
                        QStringList grids = it.value().gridNum.split(',', Qt::SkipEmptyParts);
                        for (const QString& g : grids)
                        {
                            rec.gridNum   = g.trimmed();
                            rec.gridType  = it.value().gridType.isEmpty() ? "普通格口" : it.value().gridType;
                            rec.planQty   = it.value().gridCount;
                            rec.volu      = it.value().volu;
                            items.append(rec);
                        }
                    }
                }
                m_pSortingDb->insertWaveItems(orderCode, items);

                HTTP_INFO("波次数据已落库 orderCode=%s items=%d skuCount=%d orderQty=%d",
                    orderCode.toLocal8Bit().data(), items.size(), skuCount, orderQty);
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
            // ★ 新波次到来，清空旧 EPC→SKU 映射
            {
                if (m_pEpcCache) m_pEpcCache->clear();
            }

            // ★ 主动发送模式：波次解析完成后，批量发送所有识别码到PLC（与WCSApp一致）
            //   不等待PLC查询报文，主动遍历波次所有识别码发送PLC分拣指令
            //   TODO: 识别码可能为条码或EPC，客户尚未确定（2026-08-04）
            if (m_pPlcMgr && m_pPlcMgr->hasConnectedClients())
            {
                QMap<QString, QString> codeGridMap;
                for (const QString& code : recvSet)
                {
                    GridEntry entry = m_pBuffer->get(code);
                    if (!entry.gridNum.isEmpty())
                    {
                        codeGridMap[code] = entry.gridNum;
                    }
                }
                if (!codeGridMap.isEmpty())
                {
                    HTTP_LOG_INFO("波次解析完成 主动批量发送PLC指令 order=%s count=%d",
                        orderCode.toLocal8Bit().data(), codeGridMap.size());
                    emit logMessage(QString("[PLC] 主动批量发送 %1 条指令").arg(codeGridMap.size()));
                    m_pPlcMgr->sendBatchCodes(codeGridMap);
                }
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

    // ──── PLC反馈批次 → 分拣标记 + 按格口记录 ────
    // ★ 提交到 PLC 反馈接收专用线程池处理（参考WCSApp m_threadPoolPLCRecvPtr）
    //   不占用主线程，避免高并发落格反馈阻塞 UI
    //   数据流: PlcManager::flushFeedbackBatch (主线程QTimer) → plcFeedbackBusinessBatch → PLC接收池 → markSorted + SQLite
    //
    // S4 更新（T-S4-05/06/08/09）：
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
                        // ──── T-S4-09: 仅 SORTING 接受投线 ────
                        // 非 SORTING 状态拒绝处理（FINISHED/IDLE/CANCELLED 等）
                        if (waveStatus != WAVE_SORTING)
                        {
                            // 特例：BOUND 状态首件落格 → 自动开工（T-S4-05 自动模式）
                            if (waveStatus == WAVE_BOUND && !m_pWaveMgr->isSortingStarted())
                            {
                                if (m_pWaveMgr->startSorting())
                                {
                                    waveStatus = WAVE_SORTING;
                                    HTTP_LOG_INFO("首件落格自动开工 orderCode=%s code=%s",
                                        m_pWaveMgr->orderCode().toLocal8Bit().data(),
                                        e.code.toLocal8Bit().data());
                                    emit logMessage(QString("[分拣] 首件落格自动开工 code=%1").arg(e.code));
                                }
                                else
                                {
                                    HTTP_LOG_WARN("开工失败（首件落格）orderCode=%s code=%s",
                                        m_pWaveMgr->orderCode().toLocal8Bit().data(),
                                        e.code.toLocal8Bit().data());
                                    continue; // 开工失败，跳过本条
                                }
                            }
                            else
                            {
                                HTTP_LOG_WARN("PLC反馈被拒绝 非SORTING状态 code=%s status=%d",
                                    e.code.toLocal8Bit().data(), waveStatus);
                                continue; // 非分拣状态，跳过
                            }
                        }

                        // ──── T-S4-08: 异常处理 ────
                        // 查 DoubleBuffer 确认识别码在当前波次中
                        // TODO: 识别码可能为条码或EPC，客户尚未确定（2026-08-04）
                        GridEntry entry = m_pBuffer->get(e.code);
                        if (entry.gridNum.isEmpty())
                        {
                            // 识别码不在当前波次中 → 异常口
                            HTTP_LOG_WARN("PLC反馈识别码无匹配 code=%s grid=%s 记录为异常",
                                e.code.toLocal8Bit().data(), e.grid.toLocal8Bit().data());
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

                        // ──── T-S7-04: 同品分类vs发货冲突检测（RT-CF-01~04）────
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

                        // ──── T-S7-02: EPC 任务内防重（orderCode+epc）────
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
                        // ──── T-S7-06: 格口达计划上限策略（RT-SE-05）────
                        // 检查格口是否已达计划上限，按配置选择拒收/异常口/允许超收
                        if (!cfg.sortingAllowOverrecv)
                        {
                            int currentSorted = 0;
                            {
                                std::lock_guard<std::mutex> lock(m_gridCountMutex);
                                currentSorted = m_gridSortedCount.value(e.grid, 0);
                            }
                            int planQty = entry.gridCount; // 该格口在当前SKU下的计划数
                            if (currentSorted >= planQty && planQty > 0)
                            {
                                if (cfg.sortingGridCapPolicy == "reject")
                                {
                                    HTTP_LOG_WARN("格口达计划上限 拒收 code=%s grid=%s sorted=%d plan=%d",
                                        e.code.toLocal8Bit().data(), e.grid.toLocal8Bit().data(),
                                        currentSorted, planQty);
                                    continue;
                                }
                                // exception 策略：入异常口
                                HTTP_LOG_WARN("格口达计划上限 入异常口 code=%s grid=%s sorted=%d plan=%d",
                                    e.code.toLocal8Bit().data(), e.grid.toLocal8Bit().data(),
                                    currentSorted, planQty);
                                m_pWaveMgr->markException(e.code);
                                if (m_pSortingDb)
                                {
                                    ExceptionRecord exRec;
                                    exRec.type      = "grid_cap";
                                    exRec.orderCode = m_pWaveMgr->orderCode();
                                    exRec.epc       = e.code;
                                    exRec.sku       = "";
                                    exRec.reason    = QString("格口%1已达计划上限(sorted=%2, plan=%3)")
                                        .arg(e.grid).arg(currentSorted).arg(planQty);
                                    exRec.time      = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
                                    m_pSortingDb->insertException(exRec);
                                }
                                continue;
                            }
                        }

                        m_pWaveMgr->markSorted(e.code);

                        // ★ S7 格口分拣计数递增（T-S7-06）
                        {
                            std::lock_guard<std::mutex> lock(m_gridCountMutex);
                            m_gridSortedCount[e.grid]++;
                        }

                        // 按格口记录分拣明细（锁格时回传 WMS 用）
                        {
                            std::lock_guard<std::mutex> lock(m_gridRecordMutex);
                            GridSortRecord rec;
                            rec.inco   = e.code;
                            rec.car    = e.car;
                            rec.timeMs = e.timestampMs;
                            rec.gridCount = entry.gridCount;
                            rec.volu      = entry.volu.isEmpty() ? QString("--") : entry.volu;

                            m_gridSortRecords[e.grid].append(rec);

                            // 写入分拣记录到本地 SQLite 数据库
                            if (m_pSortingDb)
                            {
                                m_pSortingDb->insertRecord(
                                    m_pWaveMgr->orderCode(),
                                    e.code, e.grid, e.car,
                                    entry.gridCount, entry.volu);
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

            // 步骤1: 锁格回传分拣明细（原有逻辑）
            HTTP_LOG_INFO("[锁格→满箱] 步骤1 锁格回传 grid=%s", grid.toLocal8Bit().data());
            sendGridLockFeedback(grid);

            // 步骤2: 满箱回传（H7 满箱同步到WMS，G2 新增）
            // PLC 锁格 = 满箱信号，触发满箱回传报文组装（H7）+ Outbox 可靠投递
            HTTP_LOG_INFO("[锁格→满箱] 步骤2 满箱回传（H7） grid=%s box=%s", grid.toLocal8Bit().data(), boxCode.toLocal8Bit().data());
            sendFullbox(grid);

            HTTP_LOG_INFO("[锁格→满箱] 完成 grid=%s order=%s", grid.toLocal8Bit().data(), orderCode.toLocal8Bit().data());
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

    // ★ S5 新增：满箱回传 Outbox 重试调度器（H7 满箱同步）
    m_outboxTimer = new QTimer(this);
    connect(m_outboxTimer, &QTimer::timeout, this, &HttpServer::pollOutboxFullbox);
    m_outboxTimer->start(OUTBOX_POLL_INTERVAL_SEC * 1000);

    // ★ S6 完结回传 Outbox 重试调度器（H8 波次完结通知WMS，T-S6-03）
    m_outboxEndTimer = new QTimer(this);
    connect(m_outboxEndTimer, &QTimer::timeout, this, &HttpServer::pollOutboxEnd);
    m_outboxEndTimer->start(OUTBOX_POLL_INTERVAL_SEC * 1000);

    LogCenter::Instance()->wcs_run_log_warn(true, "[Http] HttpServer已创建");
}

HttpServer::~HttpServer()
{
    stop();
    m_pWorker->stop();
    m_pWorker->wait(WORKER_WAIT_MS);
    // ★ 关闭分拣记录数据库
    if (m_pSortingDb) {
        m_pSortingDb->close();
        delete m_pSortingDb;
        m_pSortingDb = nullptr;
    }
    // ★ 清理 EPC 缓存
    if (m_pEpcCache) {
        delete m_pEpcCache;
        m_pEpcCache = nullptr;
    }
    if (m_pBusinessPool) {
        delete m_pBusinessPool;
        m_pBusinessPool = nullptr;
    }
    // ★ 清理专用业务线程池（PLC反馈接收池）
    if (m_pPlcRecvPool) {
        delete m_pPlcRecvPool;
        m_pPlcRecvPool = nullptr;
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
    // 需求: §5 波次下发（H4 InsertWaveInfo，T-S1-01~T-S1-06）
    // ═══════════════════════════════════════════════════════════════════════
    if (st.path == m_apiInsertWaveInfo && st.method == "POST")
    {
        // 步骤1: 解析 JSON
        QJsonParseError parseErr;
        QJsonDocument doc = QJsonDocument::fromJson(st.body, &parseErr);
        if (doc.isNull() || !doc.isObject())
        {
            HTTP_WARN("InsertWaveInfo JSON解析失败: %s", parseErr.errorString().toLocal8Bit().data());
            emit logMessage(QString("[WMS] JSON解析失败: %1").arg(parseErr.errorString()), true);
            QJsonObject err;
            err["code"] = "400";
            err["message"] = QString("JSON解析失败: %1").arg(parseErr.errorString());
            sendJsonResponse(pSender, dwConnID, err, 400);
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
            sendJsonResponse(pSender, dwConnID, validateErr, 400);
            return;
        }

        // 步骤2.5: 容器格口绑定完整性校验（G1 修复）
        // 期望绑定数量由 expectedBindCount 参数控制（默认66，每批次可配置不同数量）
        // 防止分拣时无容器接收
        if (!areAllBindingsComplete())
        {
            int boundCount = this->boundCount();
            int expectedCount = m_expectedBindCount;
            int missingCount = expectedCount - boundCount;

            // 收集未绑定的格口号列表（全部记录，不设上限，方便运维排查）
            QStringList missingGrids;
            {
                std::lock_guard<std::mutex> lock(m_containerMutex);
                for (int i = 1; i <= expectedCount; ++i)
                {
                    QString gridKey = QString("%1").arg(i, GRID_KEY_PADDING, 10, QChar('0'));
                    if (!m_containerBindings.contains(gridKey))
                        missingGrids << QString::number(i);
                }
            }

            HTTP_WARN("InsertWaveInfo 绑定不完整 orderCode=%s bound=%d/%d missing=%d grids=[%s]",
                orderCode.toLocal8Bit().data(), boundCount, expectedCount,
                missingCount, missingGrids.join(",").toLocal8Bit().data());
            emit logMessage(QString("[WMS] 格口绑定不完整(%1/%2)，未绑定格口: %3，拒绝波次 orderCode=%4")
                .arg(boundCount).arg(expectedCount).arg(missingGrids.join(",")).arg(orderCode), true);

            QJsonObject err;
            err["code"] = "400";
            err["message"] = QString("格口绑定不完整(%1/%2)，请先完成全部容器绑定后再下发波次")
                .arg(boundCount).arg(expectedCount);
            sendJsonResponse(pSender, dwConnID, err, 400);
            return;
        }

        // 绑定检查通过
        HTTP_LOG_INFO("InsertWaveInfo 绑定校验通过 orderCode=%s bound=%d/%d",
            orderCode.toLocal8Bit().data(), boundCount(), m_expectedBindCount);

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
                    err["code"] = "409";
                    err["message"] = "波次已开始分拣，不允许覆盖";
                    err["orderCode"] = orderCode;
                    sendJsonResponse(pSender, dwConnID, err, 409);
                    return;
                }
                // 未分拣，允许覆盖：先清理旧数据
                HTTP_INFO("InsertWaveInfo 覆盖旧波次 orderCode=%s oldStatus=%d",
                    orderCode.toLocal8Bit().data(), existingStatus);
                emit logMessage(QString("[WMS] 覆盖旧波次 orderCode=%1（未分拣）").arg(orderCode));
            }
        }

        // 步骤4: 前置校验 — 所有格口必须已绑定容器
        if (!areAllBindingsComplete())
        {
            int bc = boundCount();
            HTTP_WARN("InsertWaveInfo 容器未全部绑定 bound=%d/%d", bc, BINDING_SLOT_COUNT);
            emit logMessage(QString("[WMS] 容器未全部绑定 已绑定:%1/%2 拒绝波次").arg(bc).arg(BINDING_SLOT_COUNT), true);
            QJsonObject err;
            err["code"] = "400";
            err["message"] = QString("容器未全部绑定，已绑定: %1/%2，请等待WMS下发全部容器绑定").arg(bc).arg(BINDING_SLOT_COUNT);
            err["orderCode"] = orderCode;
            sendJsonResponse(pSender, dwConnID, err, 400);
            return;
        }

        // 步骤5: 检查队列容量
        if (m_pQueue->size() >= MAX_TASK_QUEUE)
        {
            HTTP_WARN("InsertWaveInfo 队列已满 orderCode=%s queue=%d", orderCode.toLocal8Bit().data(), m_pQueue->size());
            emit logMessage("[WMS] 队列已满，拒绝入队 返回503", true);
            QJsonObject err;
            err["code"] = "503";
            err["message"] = "服务器繁忙，请稍后重试";
            err["orderCode"] = orderCode;
            sendJsonResponse(pSender, dwConnID, err, 503);
            return;
        }

        // 步骤6: 入队 + 返回成功（T-S1-05 响应格式）
        {
            WaveTask task;
            task.rawBody  = st.body;
            task.recvTime = QDateTime::currentMSecsSinceEpoch();
            m_pQueue->push(task);

            QJsonObject resp;
            resp["code"]      = "200";
            resp["message"]   = "";
            resp["orderCode"] = orderCode;
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
        r["code"] = 100;
        r["Message"] = QString("参数缺失: latticehole和boxcode均为必填");
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
        r["code"] = 100;
        r["Message"] = QString("格口号越界，有效范围: 1~%1").arg(BINDING_SLOT_COUNT);
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

    // ──── 步骤3.1: 允许无波次时预绑定（IDLE 状态属于预绑定阶段）────
    // 容器绑定在波次下发之前执行，IDLE 状态允许绑定（预绑定）
    // 仅 CANCELLED/FINISHED/HELD 终态拒绝绑定
    if (waveStatus == WAVE_CANCELLED || waveStatus == WAVE_FINISHED || waveStatus == WAVE_HELD)
    {
        HTTP_LOG_WARN("BindingLatticePort 终态拒绝 latticehole=%s orderCode=%s status=%d",
            latticehole.toLocal8Bit().data(), orderCode.toLocal8Bit().data(), waveStatus);
        emit logMessage(QString("[容器绑定] 波次已终态，拒绝绑定 latticehole=%1 orderCode=%2 status=%3")
            .arg(latticehole).arg(orderCode).arg(waveStatus), true);
        QJsonObject r;
        r["code"] = 100;
        r["Message"] = QString("波次已终态(status=%1)，不允许绑定新容器").arg(waveStatus);
        return r;
    }

    // ──── 步骤4: 格口不存在于当前任务（T-S2-03）────
    // 检查该格口是否在当前波次的商品分配中
    if (!orderCode.isEmpty() && m_pBuffer)
    {
        bool gridExists = false;
        const QMap<QString, GridEntry>* pMap = m_pBuffer->activeMap();
        if (pMap)
        {
            for (auto it = pMap->constBegin(); it != pMap->constEnd(); ++it)
            {
                QStringList grids = it.value().gridNum.split(',', Qt::SkipEmptyParts);
                for (const QString& g : grids)
                {
                    if (g.trimmed() == latticehole)
                    {
                        gridExists = true;
                        break;
                    }
                }
                if (gridExists) break;
            }
        }
        if (!gridExists)
        {
            HTTP_LOG_WARN("BindingLatticePort 格口不在当前任务中 latticehole=%s orderCode=%s",
                latticehole.toLocal8Bit().data(), orderCode.toLocal8Bit().data());
            emit logMessage(QString("[容器绑定] 格口%1 不在当前波次任务中 orderCode=%2")
                .arg(latticehole).arg(orderCode), true);
            QJsonObject r;
            r["code"] = 100;
            r["Message"] = QString("格口%1不在当前波次任务中").arg(latticehole);
            return r;
        }
    }

    // ──── 步骤6: 执行绑定（T-S2-02 归档旧绑定 + 写新绑定）────
    {
        std::lock_guard<std::mutex> lock(m_containerMutex);

        // ★ 重复绑定：旧容器归档到数据库（切箱操作）
        auto it = m_containerBindings.find(latticehole);
        if (it != m_containerBindings.end() && it.value() != boxcode)
        {
            HTTP_LOG_INFO("BindingLatticePort 切箱 latticehole=%s old=%s new=%s",
                latticehole.toLocal8Bit().data(),
                it.value().toLocal8Bit().data(),
                boxcode.toLocal8Bit().data());
            emit logMessage(QString("[容器绑定] 格口%1 切箱: %2 → %3")
                .arg(latticehole).arg(it.value()).arg(boxcode));
        }

        m_containerBindings[latticehole] = boxcode;
    }

    // ★ 持久化到数据库（T-S2-04 关联 orderCode）
    if (m_pSortingDb && !orderCode.isEmpty())
    {
        m_pSortingDb->bindGridBox(latticehole, boxcode, orderCode);
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
        latticehole.toLocal8Bit().data(), boxcode.toLocal8Bit().data(),
        orderCode.toLocal8Bit().data(), boundCount(), BINDING_SLOT_COUNT);
    emit logMessage(QString("[容器绑定] 格口%1 → 容器%2 orderCode=%3 (共%4/%5)")
        .arg(latticehole).arg(boxcode).arg(orderCode).arg(boundCount()).arg(BINDING_SLOT_COUNT));
    emit bindingUpdated();  // ★ 通知 UI 即时刷新

    QJsonObject r;
    r["code"] = 200;
    r["Message"] = "收到信息";
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
        r["code"] = "400";
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
        r["code"] = "404";
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
        r["code"] = "400";
        r["message"] = QString("波次已开始分拣(status=%1 sorted=%2)，不允许取消")
            .arg(QString::fromLocal8Bit(statusText)).arg(m_pWaveMgr->sorted());
        r["cancellable"] = false;
        return r;
    }

    // ──── 步骤5: 取消后清理（T-S3-02）────
    // tryCancelWave() 已原子完成状态迁移，此处执行清理操作
    // 注意：不使用 clearWave()（会重置为 IDLE），仅清除容器绑定和分拣记录

    // 清理容器绑定
    {
        std::lock_guard<std::mutex> lock(m_containerMutex);
        int count = m_containerBindings.size();
        m_containerBindings.clear();
        HTTP_LOG_INFO("CancelWave 已清理容器绑定 count=%d orderCode=%s",
            count, orderCode.toLocal8Bit().data());
    }

    // 清理格口分拣记录
    {
        std::lock_guard<std::mutex> lock(m_gridRecordMutex);
        m_gridSortRecords.clear();
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
        err["code"] = "400";
        err["message"] = "缺少必填字段: orderCode";
        return err;
    }

    // 校验 orderQty（§5.3.1：>0）
    QJsonValue qv = root["orderQty"];
    int orderQty = qv.isString() ? qv.toString().toInt() : qv.toInt();
    if (orderQty <= 0)
    {
        QJsonObject err;
        err["code"] = "400";
        err["message"] = QString("orderQty 必须大于0，当前值: %1").arg(orderQty);
        err["orderCode"] = orderCode;
        return err;
    }

    // 校验 items（§5.3.1：至少 1 条）
    QJsonArray items = root["items"].toArray();
    if (items.isEmpty())
    {
        QJsonObject err;
        err["code"] = "400";
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
            err["code"] = "400";
            err["message"] = QString("items[%1] 缺少必填字段: inco").arg(i);
            err["orderCode"] = orderCode;
            return err;
        }
        if (gridNum.isEmpty())
        {
            QJsonObject err;
            err["code"] = "400";
            err["message"] = QString("items[%1] 缺少必填字段: gridNum").arg(i);
            err["orderCode"] = orderCode;
            return err;
        }
        if (gridType.isEmpty())
        {
            QJsonObject err;
            err["code"] = "400";
            err["message"] = QString("items[%1] 缺少必填字段: gridType").arg(i);
            err["orderCode"] = orderCode;
            return err;
        }

        // gridNumber > 0
        QJsonValue gnv = item["gridNumber"];
        int gridNumber = gnv.isString() ? gnv.toString().toInt() : gnv.toInt();
        if (gridNumber <= 0)
        {
            QJsonObject err;
            err["code"] = "400";
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
            err["code"] = "400";
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
// S5 新增：满箱回传（H7 满箱同步到WMS，需求 §10，T-S5-01~T-S5-07）
// ============================================================================

// ============================================================================
// buildFullboxPayload — 构建满箱回传报文（H7 gwisSubProductClassifyOrder，T-S5-02/03）
// 报文格式详见需求 §10.3–10.4
// 字段取值：
//   orderCode      = 波次下发编号（H4）
//   orderType      = 固定 "02"（WMS_ORDER_TYPE）
//   warehouseCode  = 配置项（默认 "A"）
//   goodsOwner     = 配置项（默认 "BXH_ZS"）
//   fromLocation   = 根据 FULLBOX_FROM_LOCATION_SOURCE 取值（config/volu）
//   createDate     = 当前时间
//   detailList     = 容器内已分拣明细
//     lineNum      = 从 1 递增
//     num          = boxcode（容器号）
//     targetLocation = 格口号
//     sku          = inco / barcode
//     qty          = 该容器内实分数量
// ============================================================================
QJsonObject HttpServer::buildFullboxPayload(const QString& orderCode, const QString& grid,
                                              const QString& boxCode, const QVector<GridSortRecord>& records)
{
    AppConfig& cfg = ConfigManager::instance()->config();

    // ── fromLocation 取值（T-S5-07）────
    QString fromLocation;
    if (QString(FULLBOX_FROM_LOCATION_SOURCE) == "config")
    {
        fromLocation = FULLBOX_DEFAULT_FROM_LOCATION;
    }
    else
    {
        // 默认使用 volu：取第一条有 volu 的记录
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
    }

    QJsonObject head;
    head["orderCode"]      = orderCode;
    head["orderType"]      = WMS_ORDER_TYPE;
    head["warehouseCode"]  = cfg.warehouseCode;
    head["goodsOwner"]     = cfg.goodsOwner;
    head["fromLocation"]   = fromLocation;
    head["createDate"]     = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.000");
    head["createUserCode"] = WMS_OPERUSER_CODE;
    head["createUserName"] = QString::fromUtf8(WMS_OPERUSER_NAME);
    head["remark"]         = "";
    head["gwf1"]           = "";  // 备用字段

    QJsonArray detailList;
    int lineNum = 1;
    for (const GridSortRecord& rec : records)
    {
        QJsonObject item;
        item["lineNum"]        = QString::number(lineNum++);
        item["num"]            = boxCode.isEmpty() ? rec.car : boxCode;  // 框号 = 容器号
        item["targetLocation"] = grid;
        item["sku"]            = rec.inco;
        item["qty"]            = QString::number(rec.gridCount);
        item["batchCode"]      = orderCode;
        detailList.append(item);
    }
    head["detailList"] = detailList;

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

    // ── 步骤3: 获取容器绑定 ──
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

    // ── 步骤4: 构建满箱回传报文（H7）──
    QJsonObject payload = buildFullboxPayload(orderCode, grid, boxCode, records);

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

    if (m_pSortingDb)
    {
        m_pSortingDb->insertOutboxFullbox(outMsg);
    }

    HTTP_LOG_INFO("满箱回传消息已入Outbox（H7） msgId=%s order=%s grid=%s box=%s items=%d",
        msgId.toLocal8Bit().data(), orderCode.toLocal8Bit().data(),
        grid.toLocal8Bit().data(), boxCode.toLocal8Bit().data(), records.size());
    emit logMessage(QString("[满箱回传] 消息入Outbox msgId=%1 grid=%2 box=%3 items=%4")
        .arg(msgId).arg(grid).arg(boxCode).arg(records.size()));

    // ── 步骤7: 发送到 WMS ──
    sendFullboxToWms(msgId, payload);
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
    if (!m_pSortingDb)
    {
        HTTP_LOG_WARN("满箱回传结果处理（H7） SortingDatabase未初始化 msgId=%s", msgId.toLocal8Bit().data());
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

        HTTP_LOG_INFO("满箱回传成功（H7） msgId=%s order=%s box=%s",
            msgId.toLocal8Bit().data(), orderCode.toLocal8Bit().data(),
            boxCode.toLocal8Bit().data());
        emit logMessage(QString("[满箱回传] 回传成功 msgId=%1 order=%2 box=%3")
            .arg(msgId).arg(orderCode).arg(boxCode));

        // 3. 归档容器绑定（允许新容器绑定（H6））
        // 找到绑定该 boxcode 的格口，归档旧绑定
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

        // 4. 状态恢复：FULLBOX_SYNC → SORTING（T-S5-05）
        if (m_pWaveMgr)
        {
            m_pWaveMgr->resumeSorting();
        }
    }
    else
    {
        // ── 失败处理（T-S5-04）────
        // 查询当前重试次数
        OutboxRecord outMsg = m_pSortingDb->getOutboxFullboxByMsgId(msgId);

        if (outMsg.retryCount >= OUTBOX_RETRY_MAX_DEFAULT)
        {
            // 重试耗尽：标记 failed，状态→HELD
            m_pSortingDb->updateOutboxFullboxStatus(msgId, "failed",
                QDateTime::currentDateTime().addSecs(OUTBOX_RETRY_INTERVAL_SEC)
                    .toString("yyyy-MM-dd HH:mm:ss"));

            HTTP_LOG_ERROR("满箱回传失败（H7） 重试耗尽 msgId=%s order=%s retry=%d body=%s",
                msgId.toLocal8Bit().data(), outMsg.orderCode.toLocal8Bit().data(),
                outMsg.retryCount, body.left(200).toLocal8Bit().data());
            emit logMessage(QString("[满箱回传] 重试耗尽 msgId=%1 order=%2 retry=%3")
                .arg(msgId).arg(outMsg.orderCode).arg(outMsg.retryCount), true);

            if (m_pWaveMgr)
            {
                m_pWaveMgr->holdAfterFullboxFail();
            }
        }
        else
        {
            // 未耗尽：更新重试信息，等待 pollOutboxFullbox 调度
            HTTP_LOG_WARN("满箱回传失败（H7） 将重试 msgId=%s order=%s retry=%d/%d",
                msgId.toLocal8Bit().data(), outMsg.orderCode.toLocal8Bit().data(),
                outMsg.retryCount + 1, OUTBOX_RETRY_MAX_DEFAULT);
            emit logMessage(QString("[满箱回传] 回传失败 将重试 msgId=%1 retry=%2/%3")
                .arg(msgId).arg(outMsg.retryCount + 1).arg(OUTBOX_RETRY_MAX_DEFAULT), true);
        }
    }
}

// ============================================================================
// pollOutboxFullbox — 满箱回传 Outbox 重试调度（H7，T-S5-04）
// 定时扫描 outbox_fullbox 表中 status='pending' 且 next_retry <= now 的消息
// 重新发送到 WMS
// ============================================================================
void HttpServer::pollOutboxFullbox()
{
    if (!m_pSortingDb) return;

    QVector<OutboxRecord> pending = m_pSortingDb->getPendingOutboxFullbox(OUTBOX_POLL_BATCH_SIZE);
    if (pending.isEmpty()) return;

    HTTP_LOG_INFO("满箱回传 Outbox 重试调度（H7） 扫描到 %d 条待重试消息", pending.size());

    for (const OutboxRecord& msg : pending)
    {
        // 解析 payload
        QJsonDocument doc = QJsonDocument::fromJson(msg.payload.toUtf8());
        if (doc.isNull() || !doc.isObject())
        {
            HTTP_LOG_WARN("满箱回传 Outbox payload解析失败（H7） msgId=%s", msg.msgId.toLocal8Bit().data());
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

        HTTP_LOG_INFO("满箱回传 Outbox 重试发送（H7） msgId=%s order=%s retry=%d",
            msg.msgId.toLocal8Bit().data(), msg.orderCode.toLocal8Bit().data(),
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
//   sumLocation    = 实际使用过的格口数（去重后）
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
// 流程：
//   1. 校验 canComplete()
//   2. 状态迁移 SORTING→ENDING
//   3. 构建完结回传报文（H8）
//   4. 生成 msgId（UUID 幂等键）
//   5. 插入 Outbox 出站表
//   6. 发送到 WMS
// ============================================================================
void HttpServer::sendEnd()
{
    if (!m_pWaveMgr)
    {
        HTTP_LOG_WARN("完结回传触发失败（H8） WaveManager未初始化");
        emit logMessage("[完结回传] 触发失败: WaveManager未初始化", true);
        return;
    }

    // ── 步骤1-2: 校验可完结条件 + 状态迁移 ──
    if (!m_pWaveMgr->completeToEnding())
    {
        HTTP_LOG_WARN("完结回传触发失败（H8） 条件不满足或状态迁移拒绝 status=%d",
            m_pWaveMgr->status());
        emit logMessage(QString("[完结回传] 触发失败: 条件不满足 status=%1")
            .arg(m_pWaveMgr->status()), true);
        return;
    }

    QString orderCode = m_pWaveMgr->orderCode();
    int sumLocation = m_pWaveMgr->sumLocation();  // T-S6-05: 实际用过格口数

    // ── 步骤3: 构建完结回传报文（H8）──
    QJsonObject payload = buildEndPayload(orderCode, sumLocation);

    // ── 步骤4-5: 生成 msgId（UUID 幂等键）+ 插入 Outbox ──
    QString msgId = QString::fromUtf8(QUuid::createUuid().toByteArray().toHex());
    QString nowStr = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    QString nextRetry = QDateTime::currentDateTime().addSecs(OUTBOX_RETRY_INTERVAL_SEC)
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

    HTTP_LOG_INFO("完结回传消息已入Outbox（H8） msgId=%s order=%s sumLocation=%d",
        msgId.toLocal8Bit().data(), orderCode.toLocal8Bit().data(), sumLocation);
    emit logMessage(QString("[完结回传] 消息入Outbox msgId=%1 order=%2 sumLocation=%3")
        .arg(msgId).arg(orderCode).arg(sumLocation));

    // ── 步骤6: 发送到 WMS ──
    sendEndToWms(msgId, payload);
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
    if (!m_pSortingDb)
    {
        HTTP_LOG_WARN("完结回传结果处理（H8） SortingDatabase未初始化 msgId=%s", msgId.toLocal8Bit().data());
        return;
    }

    if (success)
    {
        // ── 成功处理（T-S6-04）────
        m_pSortingDb->markOutboxEndSuccess(msgId);

        OutboxRecord outMsg = m_pSortingDb->getOutboxEndByMsgId(msgId); // ★ S6 按 msgId 查询

        HTTP_LOG_INFO("完结回传成功（H8） msgId=%s order=%s",
            msgId.toLocal8Bit().data(), outMsg.orderCode.toLocal8Bit().data());
        emit logMessage(QString("[完结回传] 回传成功 msgId=%1 order=%2")
            .arg(msgId).arg(outMsg.orderCode));

        // 状态：ENDING → FINISHED（T-S6-04）
        if (m_pWaveMgr)
        {
            m_pWaveMgr->setState(WAVE_FINISHED);
        }
    }
    else
    {
        // ── 失败处理（T-S6-03）────
        OutboxRecord outMsg = m_pSortingDb->getOutboxEndByMsgId(msgId); // ★ S6 按 msgId 查询
        int retryCount = outMsg.retryCount;
        QString orderCode = outMsg.orderCode;

        if (retryCount >= OUTBOX_RETRY_MAX_DEFAULT)
        {
            // 重试耗尽：标记 failed，状态→HELD
            m_pSortingDb->updateOutboxEndStatus(msgId, "failed",
                QDateTime::currentDateTime().addSecs(OUTBOX_RETRY_INTERVAL_SEC)
                    .toString("yyyy-MM-dd HH:mm:ss"));

            HTTP_LOG_ERROR("完结回传失败（H8） 重试耗尽 msgId=%s order=%s retry=%d body=%s",
                msgId.toLocal8Bit().data(), orderCode.toLocal8Bit().data(),
                retryCount, body.left(200).toLocal8Bit().data());
            emit logMessage(QString("[完结回传] 重试耗尽 msgId=%1 order=%2 retry=%3")
                .arg(msgId).arg(orderCode).arg(retryCount), true);

            if (m_pWaveMgr)
            {
                m_pWaveMgr->setState(WAVE_HELD);
            }
        }
        else
        {
            // 未耗尽：更新重试信息，等待 pollOutboxEnd 调度
            HTTP_LOG_WARN("完结回传失败（H8） 将重试 msgId=%s order=%s retry=%d/%d",
                msgId.toLocal8Bit().data(), orderCode.toLocal8Bit().data(),
                retryCount + 1, OUTBOX_RETRY_MAX_DEFAULT);
            emit logMessage(QString("[完结回传] 回传失败 将重试 msgId=%1 retry=%2/%3")
                .arg(msgId).arg(retryCount + 1).arg(OUTBOX_RETRY_MAX_DEFAULT), true);
        }
    }
}

// ============================================================================
// pollOutboxEnd — 完结回传 Outbox 重试调度（H8，T-S6-03）
// 定时扫描 outbox_end 表中 status='pending' 且 next_retry <= now 的消息
// 重新发送到 WMS
// ============================================================================
void HttpServer::pollOutboxEnd()
{
    if (!m_pSortingDb) return;

    QVector<OutboxRecord> pending = m_pSortingDb->getPendingOutboxEnd(OUTBOX_POLL_BATCH_SIZE);
    if (pending.isEmpty()) return;

    HTTP_LOG_INFO("完结回传 Outbox 重试调度（H8） 扫描到 %d 条待重试消息", pending.size());

    for (const OutboxRecord& msg : pending)
    {
        // 解析 payload
        QJsonDocument doc = QJsonDocument::fromJson(msg.payload.toUtf8());
        if (doc.isNull() || !doc.isObject())
        {
            HTTP_LOG_WARN("完结回传 Outbox payload解析失败（H8） msgId=%s", msg.msgId.toLocal8Bit().data());
            if (m_pSortingDb)
                m_pSortingDb->updateOutboxEndStatus(msg.msgId, "failed",
                    QDateTime::currentDateTime().addSecs(OUTBOX_RETRY_INTERVAL_SEC)
                        .toString("yyyy-MM-dd HH:mm:ss"));
            continue;
        }

        QJsonObject payload = doc.object();

        // 更新重试信息
        QDateTime now = QDateTime::currentDateTime();
        QString nextRetry = now.addSecs(OUTBOX_RETRY_INTERVAL_SEC)
                               .toString("yyyy-MM-dd HH:mm:ss");
        m_pSortingDb->updateOutboxEndStatus(msg.msgId, "pending", nextRetry);

        HTTP_LOG_INFO("完结回传 Outbox 重试发送（H8） msgId=%s order=%s retry=%d",
            msg.msgId.toLocal8Bit().data(), msg.orderCode.toLocal8Bit().data(),
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
                WCS_INFO("[对账] 满箱回传成功（H7） msgId=%s order=%s boxcode=%s retry=%d",
                    msg.msgId.toLocal8Bit().data(), r.orderCode.toLocal8Bit().data(),
                    msg.boxcode.toLocal8Bit().data(), msg.retryCount);
            }
            else if (msg.status == "pending" || msg.status == "failed")
            {
                r.fullboxPendingCount++;
                WCS_WARN("[对账] 满箱回传未完成（H7） msgId=%s order=%s boxcode=%s status=%s retry=%d",
                    msg.msgId.toLocal8Bit().data(), r.orderCode.toLocal8Bit().data(),
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

    // 解析每条 EPC→barcode 映射，存入 EpcCache（T-S4-04 TTL缓存）
    int newCount = 0;
    QMap<QString, QString> batchMap;
    {
        for (const QJsonValue& val : dataArr)
        {
            QJsonObject item = val.toObject();
            QString epc     = item["epc"].toString().trimmed();
            QString barcode = item["barcode"].toString().trimmed();

            if (epc.isEmpty()) continue;

            if (!barcode.isEmpty())
            {
                batchMap[epc] = barcode;
                newCount++;
            }
        }
    }

    // ★ 批量写入 EpcCache（TTL 自动管理）
    if (m_pEpcCache && !batchMap.isEmpty())
    {
        m_pEpcCache->setBatch(batchMap);
    }

    HTTP_LOG_INFO("RFID查询结果已存储 context=%s total=%d new=%d",
        context.toLocal8Bit().data(), dataArr.size(), newCount);
    emit logMessage(QString("[RFID] EPC→SKU映射已存储 context=%1 返回%2条 有效%3条")
        .arg(context).arg(dataArr.size()).arg(newCount));
}

