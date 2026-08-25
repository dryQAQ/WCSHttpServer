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
    m_pSortingDb = &SortingDatabase::instance(); // ★ 分拣记录数据库（单例）
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
        HTTP_INFO("业务线程池已创建 threads=%d", cfg.businessPoolSize);

        // ★ 专用业务线程池（参考WCSApp分类线程池设计，不同业务用独立池，方便管理内存）

        m_pPlcRecvPool = new Hanchine::ThreadPool(cfg.plcRecvPoolSize);

        HTTP_INFO("PLC反馈接收专用线程池已创建 threads=%d", cfg.plcRecvPoolSize);

        
    }

    // 显式指定 Qt::QueuedConnection：ParseWorker::run() 在独立线程中运行，
    // 使用 AutoConnection 时因 sender/receiver 的 thread() 都在主线程，
    // 但 emit 发生在工作线程，导致信号跨线程传递异常。
    connect(m_pWorker, &ParseWorker::waveParsed, this,
        [this](const QString& orderCode, int skuCount, int orderQty, qint64, const QSet<QString>& recvSet) {
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

            // ★ S1 新增：波次数据落库（T-S1-01/03）
            //   持久化 ReturnWave 头 + ReturnWaveItem 明细到 SQLite
            if (m_pSortingDb)
            {
                // 波次头（UPSERT，未分拣时允许覆盖，使用实际波次状态）
                m_pSortingDb->upsertReturnWave(orderCode, orderQty, m_pWaveMgr->status());

                // 波次明细（从 GridBuffer 读取全量 SKU→格口映射）
                QVector<ReturnWaveItemRecord> items;
                const QMap<QString, GridEntry>* pMap = m_pBuffer->activeMap();
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
                bool bOk = m_pSortingDb->insertWaveItems(orderCode, items);
                if (bOk)
                    HTTP_INFO("波次数据已落库 orderCode=%s items=%d skuCount=%d orderQty=%d",
                        orderCode.toLocal8Bit().data(), items.size(), skuCount, orderQty);
                else
                    HTTP_ERROR("波次数据落库失败 orderCode=%s items=%d",
                        orderCode.toLocal8Bit().data(), items.size());
            }
            // ★ 数据库写入完成后，默认已绑定，直接推进到 BOUND 状态
            if (m_pWaveMgr->status() == WAVE_CREATED)
            {
                m_pWaveMgr->setState(WAVE_BOUND);
                HTTP_LOG_INFO("波次自动推进 CREATED→BOUND orderCode=%s (数据库已落库，默认已绑定，等待手动开始分拣)",
                    orderCode.toLocal8Bit().data());
                emit logMessage(QString("[波次] 自动推进: 已下发→已绑定 orderCode=%1 (等待手动开始分拣)").arg(orderCode));
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
            // ★ SKU-EPC 绑定不再在 H4 下发时批量预查询，改为 RFID 推送 EPC 时实时查询
            // ★ 纠正5: 新波次开始时恢复所有禁用格口
            if (m_pPlcMgr)
                m_pPlcMgr->enableAllGrids();
            // ★ 新波次开始时清空 SKU 查询防重标记和重试计数
            m_pendingSkuQuery.clear();
            m_skuQueryRetryCount.clear();
            m_notReadyRetryCount.clear();
            m_sentEpcs.clear();
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
                        // 非 SORTING 状态拒绝处理（BOUND 等待手动开始分拣，落格反馈暂不处理）
                        if (waveStatus != WAVE_SORTING)
                        {
                            HTTP_LOG_WARN("PLC反馈被拒绝 非SORTING状态 orderCode=%s code=%s status=%d",
                                m_pWaveMgr->orderCode().toLocal8Bit().data(),
                                e.code.toLocal8Bit().data(), waveStatus);
                            continue; // 跳过本条
                        }

                        // ──── T-S4-08: 异常处理 ────
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
                        // ──── 格口达计划上限策略────
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
                                // ★ 从 EpcCache 获取 SKU 编码
                                QString sku = m_pEpcCache ? m_pEpcCache->get(e.code) : "";
                                m_pSortingDb->insertRecord(
                                    m_pWaveMgr->orderCode(),
                                    e.code, sku, e.grid, e.car,
                                    e.firstCar, e.lastCar,
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
    m_outboxFullboxTimer = new QTimer(this);
    connect(m_outboxFullboxTimer, &QTimer::timeout, this, &HttpServer::pollOutboxFullbox);
    m_outboxFullboxTimer->start(OUTBOX_POLL_INTERVAL_SEC * 1000);

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
    // ★ 关闭分拣记录数据库（单例，仅关闭不删除）
    if (m_pSortingDb) {
        m_pSortingDb->close();
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

    // ★ 启动时从数据库恢复未完成波次（软件重启后继续处理同一批次数据）
    restoreWaveFromDB();

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

// ★ 启动时检查数据库中的未完成波次（仅日志提示，不恢复到当前任务流）
//   上一波次未完成不会影响本次任务，用户可重新下发新波次
void HttpServer::restoreWaveFromDB()
{
    if (!m_pSortingDb || !m_pSortingDb->isOpen()) return;

    ReturnWaveRecord wave = m_pSortingDb->getLatestUnfinishedWave();
    if (wave.orderCode.isEmpty()) return;

    // 加载波次明细
    QVector<ReturnWaveItemRecord> items = m_pSortingDb->getWaveItems(wave.orderCode);

    HTTP_LOG_INFO("检测到上一波次未完成 orderCode=%s status=%d items=%d updatedAt=%s（不恢复到当前任务流）",
        wave.orderCode.toLocal8Bit().data(), wave.status, items.size(),
        wave.updatedAt.toLocal8Bit().data());
    emit logMessage(QString::fromUtf8("[提示] 检测到上一波次未完成: orderCode=%1 状态=%2 明细数=%3 更新时间=%4（已跳过，不恢复）")
        .arg(wave.orderCode)
        .arg(WaveSnapshot::statusToString(wave.status))
        .arg(items.size())
        .arg(wave.updatedAt));
}

void HttpServer::stop()
{
    // ★ 停止 Outbox 重试定时器，防止服务停止后继续重试
    if (m_outboxEndTimer)     m_outboxEndTimer->stop();
    if (m_outboxFullboxTimer) m_outboxFullboxTimer->stop();

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
                HTTP_LOG_INFO("BindingLatticePort 提交数据库写入 grid=%s box=%s", normalizedGrid.toLocal8Bit().data(), boxcode.toLocal8Bit().data());

                if (m_pBusinessPool)
                {
                    m_pBusinessPool->commitNoWait([this, normalizedGrid, boxcode]() {
                        HTTP_LOG_INFO("BindingLatticePort 数据库写入开始 grid=%s box=%s", normalizedGrid.toLocal8Bit().data(), boxcode.toLocal8Bit().data());
                        bool ok = m_pSortingDb->bindGridBox(normalizedGrid, boxcode);
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
                    bool ok = m_pSortingDb->bindGridBox(normalizedGrid, boxcode);
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

    // ═══════════════════════════════════════════════════════════════════════
    // 路由4: RFID 小车号推送（RFID 主动推送 → WCS 被动接收）
    // 调用方: RFID 读卡服务
    // 报文: POST /api/rfid/carNumReport
    // 功能: RFID 扫描到EPC编码后推送 EPC→barcode+carNum 映射，WCS 存入 EpcCache
    // 需求: §7 RFID小车号（T-S4-02 由RFID主动推送，不在波次下发时主动查询）
    // ═══════════════════════════════════════════════════════════════════════
    if (st.path == m_apiRfidCarNumReport && st.method == "POST")
    {
        QJsonDocument d = QJsonDocument::fromJson(st.body);
        QJsonObject result = handleRfidCarNumReport(d.object());
        sendJsonResponse(pSender, dwConnID, result);
        HTTP_INFO("RFID小车号推送 body=%d elapsed=%lldms", st.body.size(), reqTimer.elapsed());
        return;
    }


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
    // 容器绑定在波次下发之前执行，IDLE 状态允许绑定（预绑定）
    // 仅 CANCELLED/FINISHED/HELD 终态拒绝绑定
    if (waveStatus == WAVE_CANCELLED || waveStatus == WAVE_FINISHED || waveStatus == WAVE_HELD)
    {
        HTTP_LOG_WARN("BindingLatticePort 终态拒绝 latticehole=%s orderCode=%s status=%d",
            latticehole.toLocal8Bit().data(), orderCode.toLocal8Bit().data(), waveStatus);
        emit logMessage(QString("[容器绑定] 波次已终态，拒绝绑定 latticehole=%1 orderCode=%2 status=%3")
            .arg(latticehole).arg(orderCode).arg(waveStatus), true);
        QJsonObject r;
        r["code"] = "500";
        r["message"] = QString("波次已终态(status=%1)，不允许绑定新容器").arg(waveStatus);
        return r;
    }

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
    // 清理容器绑定、分拣记录、GridBuffer、WaveManager，使系统可接收新波次

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
// 报文格式（仅含 WMS 所需字段，不发送多余字段）：
//   head:
//     orderCode      = 波次下发编号（H4）
//     orderType      = 固定 "02"（WMS_ORDER_TYPE）
//     warehouseCode  = 配置项（默认 "A"）
//     goodsOwner     = 配置项（默认 "BXH_ZS"）
//     fromLocation   = 任务下发时的 volu 字段值
//     createDate     = 当前时间
//     createUserCode = 固定 "admin"
//     createUserName = 固定 "管理员"
//   detailList:
//     num            = 格口号
//     targetLocation = 目标库位编码（格口绑定的容器号）
//     sku            = SKU 编码（通过 RFID EPC→SKU 映射获取，无映射时回退 EPC）
//     qty            = 该容器内实分数量
// ============================================================================
QJsonObject HttpServer::buildFullboxPayload(const QString& orderCode, const QString& grid,
                                              const QString& boxCode, const QVector<GridSortRecord>& records)
{
    AppConfig& cfg = ConfigManager::instance()->config();

    // ── fromLocation 取值：使用任务下发时的 volu 字段值 ──
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

    QJsonArray detailList;
    for (const GridSortRecord& rec : records)
    {
        QJsonObject item;
        // item["lineNum"]        = QString::number(lineNum++);
        item["num"]            = grid;                              // 格口号
        item["targetLocation"] = boxCode.isEmpty() ? cfg.fullboxDefaultTargetLocation : boxCode;  // 目标库位 = 容器号，无容器号时兜底

        // ★ SKU 取值：通过 RFID EPC→SKU 映射获取，无映射时回退到 EPC 编码
        QString sku = getSkuByEpc(rec.inco);
        item["sku"]            = sku.isEmpty() ? QString::fromUtf8("未找到sku") : sku;
        item["qty"]            = QString::number(rec.gridCount);
        // item["batchCode"]      = orderCode;
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

    if (m_pSortingDb)
    {
        m_pSortingDb->insertOutboxFullbox(outMsg);
    }
    qint64 t3 = funcTimer.elapsed();  // ★ 耗时：Outbox写入

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

    // ★ 原始报文：满箱回传（H7）写入 run.log
    LOG_INFO("[原始报文] [满箱回传] body=%s(%d字节)",
        outMsg.payload.constData(), outMsg.payload.size());

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
        LOG_INFO("[耗时] [满箱回传] 网络往返=%lldms success=0",
            roundTripMs);

        // 查询当前重试次数
        OutboxRecord outMsg = m_pSortingDb->getOutboxFullboxByMsgId(msgId);

        if (outMsg.retryCount >= OUTBOX_RETRY_MAX_H7)
        {
            // 重试耗尽：标记 failed，记录异常，状态→IDLE（不阻塞新波次）
            m_pSortingDb->updateOutboxFullboxStatus(msgId, "failed",
                QDateTime::currentDateTime().addSecs(OUTBOX_RETRY_INTERVAL_SEC)
                    .toString("yyyy-MM-dd HH:mm:ss"));

            HTTP_LOG_ERROR("满箱回传失败（H7） 重试耗尽 order=%s retry=%d body=%s",
                outMsg.orderCode.toLocal8Bit().data(),
                outMsg.retryCount, body.left(200).toLocal8Bit().data());
            emit logMessage(QString("[满箱回传] 重试耗尽 order=%1 retry=%2")
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

            // ★ 状态→IDLE，不阻塞新波次下发
            if (m_pWaveMgr)
            {
                m_pWaveMgr->setState(WAVE_IDLE);
                HTTP_LOG_INFO("满箱回传失败（H7） 状态已重置为IDLE order=%s 可接收新波次",
                    outMsg.orderCode.toLocal8Bit().data());
            }

            // ★ 重试耗尽后停止 H7 满箱回传定时器（已标记 failed，不再需要轮询）
            if (m_outboxFullboxTimer) m_outboxFullboxTimer->stop();
            HTTP_LOG_INFO("满箱回传失败（H7） Outbox重试定时器(H7)已停止 order=%s",
                outMsg.orderCode.toLocal8Bit().data());
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
void HttpServer::sendEnd()
{
    QElapsedTimer funcTimer;
    funcTimer.start();
    qint64 epochMs = QDateTime::currentMSecsSinceEpoch();

    if (!m_pWaveMgr)
    {
        HTTP_LOG_WARN("完结回传触发失败（H8） WaveManager未初始化");
        emit logMessage("[完结回传] 触发失败: WaveManager未初始化", true);
        return;
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

    // ★ 原始报文：完结回传（H8）写入 run.log
    LOG_INFO("[原始报文] [完结回传] body=%s(%d字节)",
        outMsg.payload.constData(), outMsg.payload.size());

    // ── 步骤6: 发送到 WMS ──
    sendEndToWms(msgId, payload);
    qint64 t3 = funcTimer.elapsed();  // ★ 耗时：信号发送

    // ★ 耗时汇总日志（不包含网络往返，仅本地处理）
    LOG_INFO("[耗时] [完结回传] order=%s payload构建=%lldms Outbox写入=%lldms 信号发送=%lldms 本地总计=%lldms",
        orderCode.toLocal8Bit().data(),
        t1, t2 - t1, t3 - t2, t3);
    HTTP_LOG_INFO("完结回传耗时（H8） order=%s payload构建=%lldms Outbox=%lldms 信号发送=%lldms 总计=%lldms",
        orderCode.toLocal8Bit().data(), t1, t2 - t1, t3 - t2, t3);
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

        HTTP_LOG_INFO("完结回传成功（H8） order=%s 网络往返=%lldms",
            outMsg.orderCode.toLocal8Bit().data(), roundTripMs);
        LOG_INFO("[耗时] [完结回传] 网络往返=%lldms order=%s success=1",
            roundTripMs, outMsg.orderCode.toLocal8Bit().data());
        emit logMessage(QString("[完结回传] 回传成功 order=%1")
            .arg(outMsg.orderCode));

        // 状态：ENDING → FINISHED（已完成，终态）
        // ★ 纠正：回传成功后状态变为已完成，后续可接收新波次（FINISHED→IDLE）
        if (m_pWaveMgr)
        {
            m_pWaveMgr->setState(WAVE_FINISHED);
        }

        // ★ 波次完结后存档到历史数据库（方便追溯查找）
        if (m_pSortingDb)
        {
            m_pSortingDb->archiveWave(outMsg.orderCode);
            HTTP_LOG_INFO("完结回传（H8） 波次历史已存档 order=%s", outMsg.orderCode.toLocal8Bit().data());
        }

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
        HTTP_LOG_INFO("波次完结清理完成 order=%s（格口记录/计数/待查SKU/重试/已发送EPC 已清空）",
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

        if (retryCount >= OUTBOX_RETRY_MAX_DEFAULT)
        {
            // 重试耗尽：标记 failed，记录异常，状态→IDLE（不阻塞新波次）
            m_pSortingDb->updateOutboxEndStatus(msgId, "failed",
                QDateTime::currentDateTime().addSecs(OUTBOX_RETRY_INTERVAL_SEC)
                    .toString("yyyy-MM-dd HH:mm:ss"));

            HTTP_LOG_ERROR("完结回传失败（H8） 重试耗尽 order=%s retry=%d body=%s",
                orderCode.toLocal8Bit().data(),
                retryCount, body.left(200).toLocal8Bit().data());
            emit logMessage(QString("[完结回传] 重试耗尽 order=%1 retry=%2")
                .arg(orderCode).arg(retryCount), true);

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

            // ★ 状态→IDLE，不阻塞新波次下发
            if (m_pWaveMgr)
            {
                m_pWaveMgr->setState(WAVE_IDLE);
                HTTP_LOG_INFO("完结回传失败（H8） 状态已重置为IDLE order=%s 可接收新波次",
                    orderCode.toLocal8Bit().data());
            }

            // ★ 重试耗尽后停止 H8 完结回传定时器（已标记 failed，不再需要轮询）
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
            emit logMessage(QString("[完结回传] 回传失败 将重试 order=%1 retry=%2/%3")
                .arg(orderCode).arg(retryCount + 1).arg(OUTBOX_RETRY_MAX_DEFAULT), true);
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

        // 更新重试信息
        QDateTime now = QDateTime::currentDateTime();
        QString nextRetry = now.addSecs(OUTBOX_RETRY_INTERVAL_SEC)
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

    // ★ 仅在分拣中状态接受 RFID 推送（点击「开始分拣」后才开始接收）
    int waveStatus = m_pWaveMgr ? m_pWaveMgr->status() : -1;
    if (waveStatus != WAVE_SORTING)
    {
        HTTP_LOG_WARN("RFID推送 被拒绝 非SORTING状态 status=%d(%s) orderCode=%s",
            waveStatus, WaveSnapshot::statusToString(waveStatus).toLocal8Bit().data(),
            m_pWaveMgr ? m_pWaveMgr->orderCode().toLocal8Bit().data() : "(null)");
        QJsonObject r;
        r["code"] = "500";
        r["message"] = QString::fromUtf8("当前波次状态不允许接收RFID推送，请先点击开始分拣。当前状态=%1")
            .arg(WaveSnapshot::statusToString(waveStatus));
        return r;
    }

    // 解析每条 EPC→barcode+carNum 映射，存入 EpcCache（T-S4-04 TTL缓存）
    int newCount = 0;
    int carCount = 0;
    int skuNeedQueryCount = 0;  // ★ 需要查询 SKU 的 EPC 数量
    QStringList epcNeedSkuQuery;  // ★ 需要查询 SKU 的 EPC 列表
    QMap<QString, QPair<QString, QString>> batchMap;  // epc → {barcode, carNum}
    for (const QJsonValue& val : dataArr)
    {
        QJsonObject item = val.toObject();
        QString epc     = item["epc"].toString().trimmed();
        QString barcode = item["barcode"].toString().trimmed();  // ★ barcode=SKU编码（客户确认 2026-08-14）
        QString carNum  = item["carNum"].toString().trimmed();   // ★ RFID 小车号

        if (epc.isEmpty()) continue;

        // ★ 纠正: 接受无 barcode 的 EPC+carNum（RFID 只推送 EPC+小车号时 barcode 为空）
        //   无论 barcode 是否有值，都存入 EpcCache
        batchMap[epc] = {barcode, carNum};
        newCount++;
        if (!carNum.isEmpty() && carNum != CAR_NUM_STR(DEFAULT_CAR_NUM)) carCount++;

        // ★ 检查是否需要触发 SKU 查询（barcode 为空 或 EpcCache 中 skuBound=false）
        bool needSkuQuery = false;
        if (barcode.isEmpty())
        {
            needSkuQuery = true;
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
                HTTP_LOG_INFO("RFID推送 SKU查询入队 epc=%s carNum=%s pendingSize=%d",
                    epc.toLocal8Bit().data(), carNum.toLocal8Bit().data(),
                    m_pendingSkuQuery.size());
            }
            else
            {
                // ★ 日志: 记录被防重拦截的 EPC，方便排查重复推送
                int retryCount = m_skuQueryRetryCount.value(epc, 0);
                HTTP_LOG_INFO("RFID推送 SKU查询已提交跳过(防重) epc=%s carNum=%s retryCount=%d pendingSize=%d",
                    epc.toLocal8Bit().data(), carNum.toLocal8Bit().data(),
                    retryCount, m_pendingSkuQuery.size());
            }
        }
        else
        {
            // ★ 日志: 记录不需要 SKU 查询的 EPC（barcode 已有 or skuBound 已 true）
            HTTP_LOG_INFO("RFID推送 SKU已就绪无需查询 epc=%s barcode=%s carNum=%s",
                epc.toLocal8Bit().data(), barcode.toLocal8Bit().data(), carNum.toLocal8Bit().data());
        }
    }

    // ★ 批量写入 EpcCache（含 carNum，TTL 自动管理，保留已有 SKU 绑定数据）
    if (m_pEpcCache && !batchMap.isEmpty())
    {
        m_pEpcCache->setBatchWithCar(batchMap);
    }

    // ★ 立即触发 SKU 查询（使用新线程异步执行，不阻塞 RFID 推送响应）
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

    HTTP_LOG_INFO("RFID推送已存储 total=%d new=%d carNum=%d sent=%d skuNotFound=%d skuQuery=%d",
        dataArr.size(), newCount, carCount, sentCount, skuNotFoundCount, skuNeedQueryCount);
    emit logMessage(QString("[RFID] 小车号推送 接收%1条 有效%2条 小车号%3条 已发送PLC%4条 SKU未绑定%5条 触发SKU查询%6条")
        .arg(dataArr.size()).arg(newCount).arg(carCount).arg(sentCount).arg(skuNotFoundCount).arg(skuNeedQueryCount));

    QJsonObject r;
    r["code"] = "200";
    r["message"] = "OK";
    r["stored"] = newCount;
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
                ex.type      = QString::fromUtf8("发送超时");
                ex.orderCode = m_pWaveMgr->orderCode();
                ex.epc       = epc;
                ex.sku       = epc;
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
        HTTP_LOG_WARN("PLC发送超时 epc=%s sku=%s grid=%s elapsed=%lldms 超时阈值=%dms 入异常格口",
            epc.toLocal8Bit().data(), sku.toLocal8Bit().data(),
            entry.gridNum.toLocal8Bit().data(), elapsed, PLC_SEND_TIMEOUT_MS);
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

    bool sendOk = m_pPlcMgr->sendBatchCodesWithEpcCache(codeGridMap);
    if (!sendOk)
    {
        // ★ 发送失败（可能因格口禁用或PLC未连接），不标记为已发送
        HTTP_LOG_WARN("PLC发送失败 epc=%s sku=%s grid=%s carNum=%s (格口禁用或PLC未连接，不标记已发送)",
            epc.toLocal8Bit().data(), sku.toLocal8Bit().data(),
            entry.gridNum.toLocal8Bit().data(), carNum.toLocal8Bit().data());
        return false;
    }

    HTTP_LOG_INFO("PLC发送成功 epc=%s sku=%s grid=%s carNum=%s elapsed=%lldms",
        epc.toLocal8Bit().data(), sku.toLocal8Bit().data(),
        entry.gridNum.toLocal8Bit().data(), carNum.toLocal8Bit().data(),
        m_pEpcCache ? m_pEpcCache->getElapsedMs(epc) : -1);
    emit logMessage(QString("[PLC] 发送 %1 → 格口%2 小车%3 (sku=%4)")
        .arg(epc).arg(entry.gridNum).arg(carNum).arg(sku));

    // ★ 标记已发送，防止重复发送
    m_sentEpcs.insert(epc);

    return true;
}

