#include "HttpServer.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QDateTime>
#include <QThread>
#include <QCoreApplication>
#include <QUrlQuery>
#include <algorithm>
#include <climits>
#include <cstring>
#include "ParseWorker.h"
#include "HttpClient.h"
#include "LogService.h"
#include "ConfigManager.h"
#include "EpcCode.h"         // ★ 2026-09-15 EPC 码识别（A + N-1 位数字；与推送侧同源）
#include "WmsGridCode.h"     // ★ 2026-09-07 WMS 格口编码(22+3位) 转换工具
#include <QThread>
#include <QCoreApplication>
#include <QUuid>
#include <cstring>
#include <algorithm>   // ★ 2026-09-13 性能分位采样：std::sort
#include <cmath>       // ★ 2026-09-13 性能分位采样：std::ceil
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

    // ★ 2026-09-17 本次会话启动时刻（与 SortingDatabase::currentTimeStr 同格式，可字符串比较）：
    //   绑定"归属补齐"只处理 bind_time >= 本时刻的行 —— 上一会话/历史遗留的空归属行永不改判
    //   （"历史数据不得被粘贴进其它波次"铁律）。必须在任何 H6/H4 处理之前赋值。
    m_sessionStartTime = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");

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

    // ★ 2026-09-07 效率峰值统计初始化：加载数据库中的当日峰值（重启后延续当天显示）
    m_peakDate = QDateTime::currentDateTime().toString("yyyy-MM-dd");
    if (m_pSortingDb && m_pSortingDb->isOpen())
        m_peakPerMinuteToday = m_pSortingDb->getDailyPeakPerMinute(m_peakDate);

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

    // ★ 2026-09-14 设置「计划分配」查询回调：同品多格口按计划件数分配（选格依据）
    //   ★ 入参两个都是识别码（=EPC）：SKU 由本回调内部按 EPC 从 EpcCache 解析，
    //     这样 PlcManager 侧无需知道 EPC→SKU 的映射来源。
    m_pPlcMgr->setPlanAllocCallback([this](const QString& epc, const QString& sku, bool bClaim) -> PlcPlanAllocInfo {
        return planAllocOf(epc, sku, bClaim);
    });

    // ★ 2026-09-26：计划缺口搬迁回调（setMoveGapCallback / moveAllocGap）**已整体删除**。
    //   现场口径：不允许因锁格或其它原因搬迁计划额度 —— 计划是"（SKU,格口,分拣类型）多少件就落多少件、
    //   不能多"。不可用单元的额度留在原单元，件改用其它单元自身剩余额度，都没有则改投异常口或不发指令。
    //   （删除的是"搬迁能力"本身，不是默认关掉开关：避免以后有人再打开或重写实现。）
    //   搬迁的巡检红线保留：PlanAllocTable::audit() ⑤ planQty==planQtyH4 / ⑥ landed≤planQtyH4 /
    //   ⑦ 每 (SKU,类型) Σlanded≤ΣplanQtyH4 —— 一旦有人重新引入额度搬动，30s 巡检即报违规并写异常表。

    // ★ 2026-09-14 选格成功日志节流回调（日万级件下控制日志量；异常/超计划/搬迁仍逐条保留）
    m_pPlcMgr->setSelectLogCallback([this]() -> bool { return allocShouldLogSelect(); });

    // ★ 2026-09-14 单条下发结果回调：失败只**入队**，由主线程释放额度
    //   （分配表写操作统一主线程独占，避免 sendPool 线程直接改表引入状态错乱）
    //   ★ 2026-09-20 现场问题④：增补"最终下发格口"出参 → 记录"本件本要发往哪个格口"，
    //     供件落入异常口时区分"软件主动改投"与"PLC 自己改投/偏投"（锁格时 PLC 会把在途件强制送 66）。
    m_pPlcMgr->setSendResultCallback([this](const QString& epc, int finalGrid, bool success) {
        if (success && finalGrid > 0)
        {
            std::lock_guard<std::mutex> lk(m_lastSentGridMutex);
            m_lastSentGrid.insert(epc, finalGrid);
        }
        if (success) return;
        std::lock_guard<std::mutex> lk(m_allocPendingReleaseMutex);
        m_allocPendingRelease.insert(epc);
    });

    // ★ 2026-09-20 现场问题④：下发前置条件「已解锁 且 已绑定容器」的判据来源
    //   （内存绑定表为准，不做 DB 兜底 —— 详见 hasBoundContainer 注释）
    m_pPlcMgr->setGridBoundCallback([this](int grid) -> bool {
        return hasBoundContainer(grid);
    });

    // ★ 2026-09-20 现场问题④：改投异常口原因通知（发送侧打标 + UI 告警；落格侧据此写唯一一条留痕）
    m_pPlcMgr->setExcRouteCallback([this](const QString& epc, const QString& reason) {
        noteExcRoute(epc, reason);
    });

    // ★ 2026-09-26：缺口搬迁能力已整体删除（见上）。旧 XML 若仍写着 true，ConfigManager 已逐键 WARN
    //   "已删除、不生效"；这里不再重复提示（避免同一件事两条告警）。

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

    // ★ 2026-09-14 落格即计时归零：PLC 反馈线程池内判定"已落格"的 EPC → 回主线程执行归零
    //   （在途语义容器 m_sentEpcs/m_rescanResendTimes 仅主线程访问，不能在线程池里直接改）
    connect(this, &HttpServer::epcsLanded, this,
        [this](const QStringList& epcs) {
            for (const QString& epc : epcs)
                noteEpcLanded(epc);
        }, Qt::QueuedConnection);

    // ★ 2026-09-26 额度即时归还（现场口径：额度消耗与归还均以 **PLC 落格反馈**为准）
    //   件没进计划格口（落异常口 66 / PLC 报 status=2 无格口、status=3 信息不全 / 落错格）时：
    //   该件不计分拣、不消耗计划额度 ⇒ 认领额度**当场归还**，不再等 30s 超时清扫。
    //   ★ 必须排在 epcsLanded 之后（同一线程队列按发出顺序处理）：先清"在途"标志，
    //     归还逻辑才能区分"这个 EPC 是刚落地的那件"还是"已被重新下发的新件"。
    connect(this, &HttpServer::allocClaimReleaseRequested, this,
        &HttpServer::releaseAllocClaimsOnMissedLanding, Qt::QueuedConnection);

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

            // ★ 2026-09-07 波次到达终态（已完结/已取消）且有待执行波次 → 尝试接替下一波次
            //   （H8 成功/失败耗尽/会话超时兜底、H5 取消 全部经由此处触发，主线程串行安全）
            //   ★ 2026-09-17 现场要求：**能否真的接替由会话状态决定**——
            //     接收中且本次会话未点过「结束任务」才出队；否则队列冻结保留
            //     （见 maybeStartPendingWave / PendingWaveQueuePolicy.h）。
            //     点「结束任务」后本波次变已完结（H8 成功）正是**不允许**接替的典型场景。
            if (newStatus == WAVE_FINISHED || newStatus == WAVE_CANCELLED)
                maybeStartPendingWave();

            // ★ 2026-09-08 恢复分拣 → 补发挂起任务（RFID发送不阻塞保障）：
            //   非分拣中已就绪（SKU+小车号齐）的 EPC → 补发 PLC 指令
            //   （满箱回传已解耦：不再迁移状态，无需挂起重放）
            if (newStatus == WAVE_SORTING)
                replayPendingRfidPlcEpcs();
        });

    // 显式指定 Qt::QueuedConnection：ParseWorker::run() 在独立线程中运行，
    // 使用 AutoConnection 时因 sender/receiver 的 thread() 都在主线程，
    // 但 emit 发生在工作线程，导致信号跨线程传递异常。
    connect(m_pWorker, &ParseWorker::waveParsed, this,
        [this](const QString& orderCode, int skuCount, int orderQty, qint64, const QSet<QString>& recvSet,
               const QByteArray& rawBody,
               const QVector<QPair<QString, QVector<PlanGridInput>>>& skuPlans, int planSum) {
            // ════════════════════════════════════════════════════════════════
            // ★ 2026-09-17 会话闸门（现场缺陷"跳过一个波次"修复，见 PendingWaveQueuePolicy.h）
            //   旧实现：`!m_receiving && !m_replayingPending` → 直接 **丢弃** 该波次
            //     （重放标志只为"自动开始下一波次"开路，冻结态下还会把重放波次注册进内存）。
            //   新口径：**注册当前波次的前提 = 接收中 且 本次会话没点过「结束任务」**；
            //     不满足时**绝不丢弃**——按来源放回待执行队列，等下次「开始接收任务」执行：
            //       · 来自队列重放 → 放回**队首**（它就是队首，放回后不跳号）；
            //       · 来自 HTTP 新到达 → 追加**队尾**（它比队列里的都新）。
            //     这样"H4 已被 WMS 下发（界面已回 200 successed）"的波次要么成为当前波次、
            //     要么留在队列里可见可执行，**不会静默消失**（现场"波次数据被跳过"的另一来源）。
            // ════════════════════════════════════════════════════════════════
            const bool bFromReplay = (m_pendingReplayInFlight.load() > 0);
            if (bFromReplay)
                m_pendingReplayInFlight.fetch_sub(1);   // 本次解析结果已到达，配对出队

            if (!m_receiving.load() || m_endRequested.load())
            {
                const QString why = !m_receiving.load()
                    ? QString::fromUtf8("当前未接收任务")
                    : QString::fromUtf8("本次会话已点「结束任务」");
                HTTP_LOG_WARN("会话已冻结（%s），不注册波次 orderCode=%s 来源=%s → 放回待执行队列%s",
                    why.toLocal8Bit().data(), orderCode.toLocal8Bit().data(),
                    bFromReplay ? "队列重放" : "HTTP新到达", bFromReplay ? "队首" : "队尾");
                enqueuePendingWave(orderCode, orderQty, rawBody,
                                   QDateTime::currentMSecsSinceEpoch(), bFromReplay,
                                   QString::fromUtf8("会话已冻结（%1）%2")
                                       .arg(why, bFromReplay ? QString::fromUtf8("·重放回滚")
                                                             : QString::fromUtf8("·WMS 已下发")));
                return;
            }

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
                    // 入队（去重/SKU 提取/落库/UI 刷新/留痕统一在 enqueuePendingWave 内）
                    const bool queued = enqueuePendingWave(orderCode, orderQty, rawBody,
                                                           QDateTime::currentMSecsSinceEpoch(), false,
                                                           QString::fromUtf8("当前波次作业中"));
                    if (!queued)
                    {
                        HTTP_LOG_WARN("波次已在待执行队列，忽略重复 orderCode=%s", orderCode.toLocal8Bit().data());
                        emit logMessage(QString("[波次] %1 已在待执行队列，忽略重复下发").arg(orderCode), true);
                        return;
                    }
                    // ★ 2026-09-17 提示语修正：旧文案承诺"当前波次结束后自动开始"，
                    //   而实际口径是"队列只在**点了「开始接收任务」**时才出队"（本次现场要求）。
                    //   点「结束任务」结束当前波次后，队列**冻结保留**，不会被自动消费。
                    HTTP_LOG_INFO("新波次排队 orderCode=%s qty=%d 当前波次=%s 队列=%d",
                        orderCode.toLocal8Bit().data(), orderQty, curOrder.toLocal8Bit().data(),
                        m_pendingWaveQueue.size());
                    emit logMessage(QString("[波次] 新波次 %1 已排队（待执行 %2 个，当前波次 %3 作业中）——"
                                            "点「结束任务」后队列保留不自动执行，"
                                            "再点「开始接收任务」时按顺序执行队首；"
                                            "可用「查看接收波次队列」查看")
                        .arg(orderCode).arg(m_pendingWaveQueue.size()).arg(curOrder));
                    return;
                }
            }

            m_pWaveMgr->setWaveData(orderCode, orderQty, skuCount);
            m_pWaveMgr->setRecvSet(recvSet);

            // ════════════════════════════════════════════════════════════════
            // ★ 2026-09-14 编译「计划分配表」（本波次唯一一次，主线程）
            //   客户口径落地：每个格口有对应这个产品的数量（正常分拣格口/发货格口
            //   各一份），按各格口数量分件；已落+在途 ≥ 计划 → 改投异常口。
            //   ★ 只在主线程编译：表的写操作统一由主线程独占（选格链路同线程），
            //     PLC 反馈线程池只通过 commitOnLanded 读改，由 m_allocMutex 串行化。
            //   ★ 编译失败/开关关闭 → valid=false，全系统退回改造前老逻辑（不阻塞投线）。
            // ════════════════════════════════════════════════════════════════
            buildPlanAllocTable(orderCode, orderQty, planSum, skuPlans);

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

            // ★ 2026-09-11 重扫重投：PLC 落格反馈已到达 → 该 EPC 退出"在途"集合。
            //   覆盖本批次所有分支（成功/无匹配/无绑定/冲突/status 2·3），即"本次投递已结束"，
            //   之后该件再被 RFID 读到（拿起重新上料）即可按原格口映射重新下发。
            //   注：此处为反馈批处理的主线程入口（PlcManager 的 QTimer flushFeedbackBatch 同线程），
            //       与 m_sentEpcs 的其它访问点同线程，无需加锁。
            //   同时快照出"本次落格属于重扫重投"的 EPC，供线程池内判断（避免跨线程访问成员容器）
            QSet<QString> rescanEpcs;
            // ★ 2026-09-20 现场问题④：反馈到达时仍记为"在途"的 EPC 快照
            //   必须在 clearEpcInFlight 之前取：本件若确实处于在途（已下发、尚未反馈），
            //   说明"下发时格口状态是好的"，是落格前才失去容器绑定 —— 即本次整改后的兜底窗口，
            //   需要在异常留痕里标注出来（与"下发时就无绑定"区分开，后者改造后不应再发生）。
            QSet<QString> inFlightEpcs;
            for (const PlcFeedbackEntry& e : entries)
            {
                if (m_sentEpcs.contains(e.code)) inFlightEpcs.insert(e.code);
                clearEpcInFlight(e.code);
                if (m_rescanResendTimes.contains(e.code))
                    rescanEpcs.insert(e.code);
            }

            // ★ 提交到 PLC 反馈接收专用线程池（不阻塞主线程）
            //
            // ★★ landedEpcs 必须【按值捕获】**副本** ★★
            //   ThreadPool::commitNoWait 只做"入队 + notify_one"就返回（见 ThreadPool.h 第 82-99 行），
            //   任务由池内线程**稍后**执行。若按引用捕获栈上局部变量，本函数返回后该对象即析构，
            //   工作线程随后访问就是 use-after-free（表现为随机崩溃 C0000005）。
            //   按值捕获后，任务写入的是它自己那份副本 → 因此所有 landedEpcs.insert(...)
            //   都必须发生在任务 lambda 内部（下面的代码正是如此），外层变量仅作为传参载体。
            //   此前的注释"commitNoWait 保证本帧内执行完"与实现不符，已一并纠正。
            {
            QSet<QString> landedEpcs;
            // ★ 2026-09-26：本批"件没进计划格口"的 EPC（落异常口 66 / PLC 报 status=2,3 / 落错格）
            //   —— 这些件不计分拣、不消耗计划额度 ⇒ 批末回主线程**即时归还**认领额度
            //   （现场口径：额度消耗与归还均以 PLC 真实落格反馈为准，不再等 30s 超时清扫）。
            QStringList missedPlanLandingEpcs;
            if (m_pPlcRecvPool)
            {
                m_pPlcRecvPool->commitNoWait([this, entries, rescanEpcs, inFlightEpcs, landedEpcs, missedPlanLandingEpcs]() mutable {
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

                        // ──── ★ 2026-09-13 物理异常口反馈识别（必须先于"正常落格处理"）────
                        //   超计划件被改投异常格口后，PLC 会按异常格号反馈落格；
                        //   若不在此拦下，它会被当成正常落格：计件、写内存明细、进而进 H7 报文，
                        //   反而制造新的账实不符与上传污染。
                        //   ★ 必须带 status 条件：现场 PLC 自己也会用同一格号报"无格口"
                        //     （历史日志：grid=066 且 status=2 的反馈共 35 条，涉及 35 个 EPC，
                        //      其中 30 个随后在正常格口成功落格 → 066 是 PLC 的异常/无格口落点）。
                        //     若不加此条件，会把"PLC 判定落格失败"误标成"超计划件已入异常口"，
                        //     掩盖真实原因、污染异常账。
                        //   判定：grid==异常格号 且 status≠2/3（真实落格）→ 我方改投的异常件。
                        {
                            const QString excGridCfg = ConfigManager::instance()->config().exceptionGrid.trimmed();
                            const bool bIsExcGrid = !excGridCfg.isEmpty() && excGridCfg != "0" &&
                                                    normalizeGridKey(e.grid) == normalizeGridKey(excGridCfg);
                            const bool bPlcFail = (e.status == 2 || e.status == 3);   // PLC 判定失败（无格口/信息不全）
                            if (bIsExcGrid && !bPlcFail)
                            {
                                const QString skuExc = m_pEpcCache ? m_pEpcCache->get(e.code) : QString();
                                // 异常口当前容器（人工对账/清出用；无绑定则记"—"）
                                QString excBox = currentBoxOfGrid(normalizeGridKey(e.grid));
                                if (excBox.isEmpty()) excBox = QString::fromUtf8("未绑定容器");
                                const int excPlanQty = planQtyOfGrid(skuExc, normalizeGridKey(e.grid));

                                // ── ★ 2026-09-20 现场问题④：区分"谁把这件弄进异常口的" ──
                                //   ① 软件主动改投（发送侧已打标）→ 原因取自打标（本次新增：格口未绑定容器）；
                                //   ② 未打标但本件在途且"本要发往的格口 ≠ 异常口"→ PLC 自己改投/偏投
                                //      （典型：锁格瞬间 PLC 把在途件强制送 66，客户 2026-09-20 确认）；
                                //   ③ 其余 → 既有"超计划"口径（人工多投/同时两件在线）。
                                //   三者都要在日志与异常表里可区分，现场才知道该等绑定、还是该核对人工多投。
                                const QString tagReason = takeExcRoute(e.code);
                                int lastSent = 0;
                                {
                                    std::lock_guard<std::mutex> lkSent(m_lastSentGridMutex);
                                    lastSent = m_lastSentGrid.value(e.code, 0);
                                }
                                const bool bPlcRedirect = tagReason.isEmpty() && lastSent > 0 &&
                                                          normalizeGridKey(QString::number(lastSent)) != normalizeGridKey(e.grid);
                                QString excType, excCauseDesc;
                                if (!tagReason.isEmpty())
                                {
                                    excType      = QString::fromUtf8("改投异常口(%1)").arg(tagReason);
                                    excCauseDesc = QString::fromUtf8("软件改投（原因：%1）").arg(tagReason);
                                }
                                else if (bPlcRedirect)
                                {
                                    excType      = QString::fromUtf8("异常口件(PLC改投/偏投)");
                                    excCauseDesc = QString::fromUtf8("PLC改投/偏投（本件本要发往格口%1）")
                                                       .arg(PlanAllocTable::gridKeyOf((qint16)lastSent));
                                }
                                else
                                {
                                    excType      = QString::fromUtf8("超计划入异常口");
                                    excCauseDesc = QString::fromUtf8("超计划件按策略改投异常口");
                                }

                                HTTP_LOG_WARN("[异常口] %s已真实落入异常口 epc=%s sku=%s grid=%s 容器=%s "
                                              "status=%d 该SKU在本口计划=%d件 本件下发目标=%d —— 不计已分拣、不写箱内明细、不进 H7 报文，请人工清出",
                                    excCauseDesc.toLocal8Bit().data(),
                                    e.code.toLocal8Bit().data(), skuExc.toLocal8Bit().data(),
                                    e.grid.toLocal8Bit().data(), excBox.toLocal8Bit().data(), e.status, excPlanQty, lastSent);
                                emit logMessage(QString::fromUtf8(
                                    "[异常口] %1已入异常口：EPC %2（SKU %3）格口%4 容器%5 —— 请现场清出")
                                    .arg(excCauseDesc).arg(e.code).arg(skuExc).arg(normalizeGridKey(e.grid)).arg(excBox), true);
                                if (m_pWaveMgr)
                                    m_pWaveMgr->markException(e.code);
                                if (m_pSortingDb && m_pSortingDb->isOpen())
                                {
                                    ExceptionRecord exExc;
                                    exExc.type      = excType;
                                    exExc.orderCode = m_pWaveMgr ? m_pWaveMgr->orderCode() : QString();
                                    exExc.epc       = e.code;
                                    exExc.sku       = skuExc;
                                    exExc.reason    = QString::fromUtf8(
                                        "%1并已落格（容器%2，不计已分拣、不写箱内明细、不上传WMS，请人工清出）")
                                                          .arg(excCauseDesc).arg(excBox);
                                    exExc.time      = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
                                    m_pSortingDb->insertException(exExc);
                                }
                                // 计时/在途复位（与其它异常分支一致，便于现场重新投放）
                                // ★ 2026-09-16 改为 resetCycle（作废本轮，下次推送按新件起算）
                                //   + 打"异常终态"根因标记（供后续超时守卫沿用根因）
                                markEpcTerminalException(e.code, excType);
                                if (m_pEpcCache) m_pEpcCache->resetCycle(e.code);
                                landedEpcs.insert(e.code);   // ★ 异常口件也已落格 → 一并归零在途/重发计数
                                // ★ 2026-09-26：件进了异常口 ⇒ 没进计划格口，不计分拣、不消耗计划额度
                                //   → 批末回主线程即时归还该件认领的额度（此前要等 30s 超时清扫）
                                missedPlanLandingEpcs << e.code;
                                continue;   // ★ 不再进入正常落格处理
                            }
                        }

                        // ──── PLC 5字段反馈状态过滤（1=成功, 2=无格口, 3=信息不全）────
                        // 3字段格式无状态字段（status=0），5字段格式 status=1/2/3
                        // PLC 判定失败（2/3）的反馈不得计入成功分拣，转为异常记录
                        if (e.status == 2 || e.status == 3)
                        {
                            // ★ 2026-09-22 现场要求：状态必须说清"到底什么信息不全"，不能只写"信息不全"。
                            //   5 字段格式 = {EPC|格口号|首车|尾车|status}；status=3 由 PLC 判定为"信息不全"，
                            //   缺失字段直接由本条反馈的字段内容推断（原始报文可对照
                            //   log/PLC/PLC.log 的「PLC反馈(5字段) raw={...}」行）。
                            QStringList missing;
                            if (e.code.isEmpty())                        missing << QString::fromUtf8("EPC/条码");
                            if (e.grid.isEmpty() || e.grid.toInt() == 0) missing << QString::fromUtf8("格口号");
                            if (e.firstCar.isEmpty() && e.car.isEmpty()) missing << QString::fromUtf8("小车号");
                            if (e.lastCar.isEmpty())                     missing << QString::fromUtf8("尾车号");

                            const QString fEpc  = e.code.isEmpty()     ? QString::fromUtf8("(空)") : e.code;
                            const QString fGrid = e.grid.isEmpty()     ? QString::fromUtf8("(空)") : e.grid;
                            const QString fCar1 = e.firstCar.isEmpty() ? QString::fromUtf8("(空)") : e.firstCar;
                            const QString fCar2 = e.lastCar.isEmpty()  ? QString::fromUtf8("(空)") : e.lastCar;

                            QString statusDesc;
                            QString reason;
                            if (e.status == 2)
                            {
                                statusDesc = QString::fromUtf8("无格口");
                                reason = QString::fromUtf8("PLC 反馈 status=2（无格口）：格口号=%1 —— PLC 未给出可落格口，"
                                                           "本件不计入成功分拣（可重投，重投成功即闭环）").arg(fGrid);
                            }
                            else
                            {
                                statusDesc = QString::fromUtf8("信息不全");
                                reason = missing.isEmpty()
                                    ? QString::fromUtf8("PLC 反馈 status=3（信息不全）：本条字段看似齐全"
                                                        "（EPC=%1 格口号=%2 首车=%3 尾车=%4）—— 具体缺失项由 PLC 侧判定，"
                                                        "请对照 log/PLC/PLC.log 的 raw={...} 行核对；本件不计入成功分拣")
                                          .arg(fEpc, fGrid, fCar1, fCar2)
                                    : QString::fromUtf8("PLC 反馈 status=3（信息不全）：缺少 %1"
                                                        "（EPC=%2 格口号=%3 首车=%4 尾车=%5），本件不计入成功分拣")
                                          .arg(missing.join(QString::fromUtf8("、")), fEpc, fGrid, fCar1, fCar2);
                            }

                            HTTP_LOG_WARN("PLC反馈状态异常 code=%s grid=%s status=%d(%s) 原因=%s",
                                e.code.toLocal8Bit().data(), e.grid.toLocal8Bit().data(),
                                e.status, statusDesc.toLocal8Bit().data(), reason.toLocal8Bit().data());
                            if (m_pWaveMgr)
                                m_pWaveMgr->markException(e.code);
                            if (m_pSortingDb && m_pSortingDb->isOpen())
                            {
                                ExceptionRecord exRec;
                                // 库内 type 仍是稳定代号（处理数统计口径依赖它，界面统一翻译成中文）
                                exRec.type      = (e.status == 2) ? "plc_no_grid" : "plc_info_incomplete";
                                exRec.orderCode = m_pWaveMgr ? m_pWaveMgr->orderCode() : QString();
                                exRec.epc       = e.code;
                                exRec.sku       = m_pEpcCache ? m_pEpcCache->get(e.code) : QString();
                                exRec.reason    = reason;
                                exRec.time      = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
                                m_pSortingDb->insertException(exRec);
                            }
                            // ★ 2026-09-09 需求7：PLC 判定失败的件入异常，计时归0——二次上传重新计时
                            // ★ 2026-09-16 改为 resetCycle（作废本轮）+ 打根因标记
                            // ★ 2026-09-22 根因标记带上"缺什么"（现场看「处理明细/实时面板」即可定位）
                            markEpcTerminalException(e.code,
                                (e.status == 2)
                                    ? QString::fromUtf8("PLC无格口")
                                    : (missing.isEmpty()
                                           ? QString::fromUtf8("PLC信息不全")
                                           : QString::fromUtf8("PLC信息不全（缺%1）").arg(missing.join(QString::fromUtf8("、")))));
                            if (m_pEpcCache)
                                m_pEpcCache->resetCycle(e.code);
                            // ★ 2026-09-26：PLC 报"无格口/信息不全" ⇒ 本件没落进计划格口
                            //   （不计已分拣）→ 批末即时归还其认领额度，重投按新件判定
                            missedPlanLandingEpcs << e.code;
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
                            // ★ 2026-09-09 口径调整（客户确认"以实时记录PLC分拣数量为准"）：
                            //   PLC 报成功即计"已分拣"，不再因 WCS 侧未查到 SKU 而挪进异常；
                            //   异常仅保留 PLC 主动报失败的（status=2/3）。本条仍写异常表留痕可查
                            HTTP_LOG_WARN("PLC反馈识别码无匹配 code=%s grid=%s sku=%s 计为已分拣(PLC报成功)，异常表留痕",
                                e.code.toLocal8Bit().data(), e.grid.toLocal8Bit().data(), sku.toLocal8Bit().data());
                            if (m_pWaveMgr)
                            {
                                // ★ 2026-09-13 异常及时清理：先判"是否仍在异常口"，再计已分拣
                                //   （markSorted 内部也会移除异常集合，顺序颠倒会导致判定恒为 false）
                                const bool wasExc = m_pWaveMgr->removeExceptionOnSorted(e.code);
                                m_pWaveMgr->markSorted(e.code);
                                landedEpcs.insert(e.code);   // ★ 2026-09-14 已落格 → 批处理后归零超时计时/在途
                                if (wasExc)
                                {
                                    HTTP_LOG_WARN("[异常清理] code=%s 已成功落格(无匹配留痕) → 处理/异常口 -1", e.code.toLocal8Bit().data());
                                    emit logMessage(QString::fromUtf8("[异常清理] EPC %1 已成功落格到格口%2 → 已从「处理/异常口」中减去")
                                        .arg(e.code).arg(e.grid));
                                    if (m_pSortingDb && m_pSortingDb->isOpen())
                                        m_pSortingDb->markExceptionResolved(m_pWaveMgr->orderCode(), e.code);
                                }
                            }
                            // 写入异常记录（仅留痕，不增加面板异常计数）
                            if (m_pSortingDb)
                            {
                                ExceptionRecord exRec;
                                exRec.type      = "no_match";
                                exRec.orderCode = m_pWaveMgr->orderCode();
                                exRec.epc       = e.code;
                                exRec.sku       = "";
                                exRec.reason    = QString("格口%1：当前波次中不存在该识别码（PLC报成功，计已分拣，仅留痕）").arg(e.grid);
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
                                // ──── ★ 2026-09-20 现场问题④：兜底窗口（指令发出后、落地前绑定被拿走）────
                                //   改造后"下发前置条件 = 已解锁且已绑定容器"，指令不会再发给无容器格口，
                                //   因此本分支只剩兜底场景：指令发出时格口还是好的，件在途中格口失去绑定
                                //   （唯一高频来源是人工点「清空格口绑定」，已加二次确认 + 在途件提示）。
                                //   口径（客户确认 2026-09-20：**暂按现状处理，后续再定**）：
                                //     · 落格照实（PLC 已把件带过去，软件无法拦）；
                                //     · 仍计已分拣（既有"以实时 PLC 分拣数量为准"口径，本次不动）；
                                //     · 只写 no_bind 留痕；**不写落格明细** ⇒ 永不进任何容器的 H7；
                                //     · **不消耗额度**：认领不提交，≤allocClaimTimeoutMs 由
                                //       sweepPlanAllocClaims 归还（额度随认领超时回滚，不占计划）；
                                //     · 面板「处理/异常口」不 +1（本分支不调 markException，与 66 号件区分）。
                                //   本次新增：UI 红色告警 + 异常原因写明上述口径 + 在途标记（现场可见、可对账）。
                                const bool bWasInFlight = inFlightEpcs.contains(e.code);
                                const QString skuNoBind = m_pEpcCache ? m_pEpcCache->get(e.code) : QString();
                                HTTP_LOG_WARN("PLC反馈格口无绑定 code=%s grid=%s sku=%s 在途件=%d "
                                              "计为已分拣(PLC报成功)，异常表留痕；该件未进任何容器、不进任何 H7、不消耗额度",
                                    e.code.toLocal8Bit().data(), e.grid.toLocal8Bit().data(),
                                    skuNoBind.toLocal8Bit().data(), bWasInFlight ? 1 : 0);
                                emit logMessage(QString::fromUtf8(
                                    "[格口未绑定] EPC %1（SKU %2）落格时格口%3 无容器绑定%4 —— 该件已落格但**未进任何容器**："
                                    "不计入任何容器的 H7 报文、不消耗计划额度（认领将超时归还）；"
                                    "请人工清出/核对，并确认该格口是否需要重新绑定容器")
                                    .arg(e.code)
                                    .arg(skuNoBind.isEmpty() ? QString::fromUtf8("未知") : skuNoBind)
                                    .arg(e.grid)
                                    .arg(bWasInFlight
                                             ? QString::fromUtf8("（在途件：下发时格口可用，落格前失去绑定）")
                                             : QString::fromUtf8("（非在途件：下发时该格口即未绑定，请检查 Exe 是否为本次改造后版本）")),
                                    true);
                                if (m_pWaveMgr)
                                {
                                    // ★ 2026-09-13 异常及时清理（同"无匹配"分支：先判定再计件）
                                    const bool wasExc = m_pWaveMgr->removeExceptionOnSorted(e.code);
                                    m_pWaveMgr->markSorted(e.code);
                                    landedEpcs.insert(e.code);   // ★ 2026-09-14 已落格 → 批处理后归零超时计时/在途
                                    if (wasExc)
                                    {
                                        HTTP_LOG_WARN("[异常清理] code=%s 已成功落格(无绑定留痕) → 处理/异常口 -1", e.code.toLocal8Bit().data());
                                        emit logMessage(QString::fromUtf8("[异常清理] EPC %1 已成功落格到格口%2 → 已从「处理/异常口」中减去")
                                            .arg(e.code).arg(e.grid));
                                        if (m_pSortingDb && m_pSortingDb->isOpen())
                                            m_pSortingDb->markExceptionResolved(m_pWaveMgr->orderCode(), e.code);
                                    }
                                }
                                if (m_pSortingDb)
                                {
                                    ExceptionRecord exRec;
                                    exRec.type      = "no_bind";
                                    exRec.orderCode = m_pWaveMgr->orderCode();
                                    exRec.epc       = e.code;
                                    exRec.sku       = skuNoBind;
                                    exRec.reason    = QString::fromUtf8(
                                        "格口%1：落格时无活跃容器绑定（%2）；PLC报成功、计已分拣、仅留痕；"
                                        "该件不属于任何容器 ⇒ 不进任何 H7 报文；计划额度未消耗（认领超时归还）；请人工清出/核对")
                                                          .arg(e.grid)
                                                          .arg(bWasInFlight
                                                                   ? QString::fromUtf8("在途件，下发时格口可用")
                                                                   : QString::fromUtf8("非在途件（下发时即无绑定）"));
                                    exRec.time      = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
                                    m_pSortingDb->insertException(exRec);
                                }
                                continue;
                            }
                        }

                        // ──── 落格与计划的一致性校验（替代原"分类vs发货冲突"判定）────
                        // ★ 2026-09-14 口径修正：客户确认 WMS 会下发 gridType（0=分类/正常分拣、2=发货），
                        //   且同一个 SKU 可以按「分类格口 + 发货格口」分别计划（每格口各一份数量）。
                        //   原判定在 activeMap 内用 it.key()==e.code 比较 entry.gridType，恒为同一条目
                        //   → 永不成立（等于没校验）；而合并后的 gridType 只保留最后一行，也不可靠。
                        //   现按"每格口保存的 gridTypePerGrid"判定：
                        //     · 落格口在本 SKU 计划内（planQtyPerGrid 命中）→ 正常落格，不记冲突；
                        //     · 落格口不在计划内 → 真"落错格"，按 STRICT_EXCEPTION 策略留痕（仍计已分拣）。
                        {
                            const QString curGridKey = normalizeGridKey(e.grid);
                            const bool bInPlan = entry.planQtyPerGrid.contains(curGridKey);
                            if (bInPlan)
                            {
                                const QString plannedType = entry.gridTypePerGrid.value(curGridKey, entry.gridType);
                                HTTP_LOG_INFO("落格校验 计划内格口 code=%s sku=%s grid=%s(类型%s,计划%d件) —— 正常，非冲突",
                                    e.code.toLocal8Bit().data(), sku.toLocal8Bit().data(),
                                    curGridKey.toLocal8Bit().data(), plannedType.toLocal8Bit().data(),
                                    entry.planQtyPerGrid.value(curGridKey));
                            }
                            else if (!sku.isEmpty() && !entry.gridNum.isEmpty())
                            {
                                HTTP_LOG_WARN("落格校验 实际格口不在该SKU计划内 code=%s sku=%s 实际=%s 计划格口=[%s]（PLC报成功，计已分拣）",
                                    e.code.toLocal8Bit().data(), sku.toLocal8Bit().data(),
                                    curGridKey.toLocal8Bit().data(), entry.gridNum.toLocal8Bit().data());
                                if (QString(cfg.sortingConflictPolicy) == "STRICT_EXCEPTION")
                                {
                                    // ★ 2026-09-13 异常及时清理（同"无匹配/无绑定"分支：先判定再计件）
                                    const bool wasExc = m_pWaveMgr->removeExceptionOnSorted(e.code);
                                    m_pWaveMgr->markSorted(e.code);
                                    landedEpcs.insert(e.code);   // ★ 2026-09-14 已落格 → 批处理后归零超时计时/在途
                                    if (wasExc)
                                    {
                                        HTTP_LOG_WARN("[异常清理] code=%s 已成功落格(落错格留痕) → 处理/异常口 -1", e.code.toLocal8Bit().data());
                                        emit logMessage(QString::fromUtf8("[异常清理] EPC %1 已成功落格到格口%2 → 已从「处理/异常口」中减去")
                                            .arg(e.code).arg(e.grid));
                                        if (m_pSortingDb && m_pSortingDb->isOpen())
                                            m_pSortingDb->markExceptionResolved(m_pWaveMgr->orderCode(), e.code);
                                    }
                                    if (m_pSortingDb)
                                    {
                                        ExceptionRecord exRec;
                                        exRec.type      = "wrong_grid";
                                        exRec.orderCode = m_pWaveMgr->orderCode();
                                        exRec.epc       = e.code;
                                        exRec.sku       = sku;
                                        exRec.reason    = QString("落错格：实际格口%1 不在该SKU计划内（计划格口=[%2]）；PLC报成功，计已分拣，仅留痕")
                                                              .arg(curGridKey).arg(entry.gridNum);
                                        exRec.time      = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
                                        m_pSortingDb->insertException(exRec);
                                    }
                                    continue;
                                }
                            }
                        }

                        // ──── EPC 任务内防重（orderCode+epc）────
                        // ★ 2026-09-09 需求8：计数以 PLC 实时反馈为准——重复反馈也计 1 件（markSorted 累计+1）
                        // ★ 2026-09-11 重扫重投：该 EPC 被 WCS 重新下发（操作员拿起重新上料）→ 落格的是
                        //   **同一实物件** → 不重复计件，保持"箱内 1 件 = 账 1 件"
                        // ★ 2026-09-14 落格明细去重（客户确认）：同一 EPC 同波次同格口只保留 1 条落格明细。
                        //   改法：此处只负责"计数"，**不再 continue 跳过明细**，统一交给下方
                        //   「按格口记录分拣明细」的 (格口,EPC) 去重把关 —— 换箱后重投回同一格口
                        //   不再产生第二条明细（这正是 09-13 波次数量对不上的根因）。
                        bool bDedupLanded = false;   // 重复类落格（重扫重投/重复反馈/DB防重）：计数已完成，明细交由下方去重把关
                        // ★ 2026-09-26 单元归属诊断（同一 (SKU,格口) 可有多类型单元）——作用域覆盖本件全流程：
                        //   lateStubUsed = 认领已被释放、按归属存根记回了**原认领单元**（属性未串）；
                        //   unitGuessed  = 该格口多单元却无法判定归属 → 按"首个有余量单元"记账，需留痕；
                        //   landedType   = 本次真正记账的单元类型（落格明细持久化；0xFF = 本件未进计划单元）
                        bool   lateStubUsed = false;
                        bool   unitGuessed  = false;
                        quint8 landedType   = 0xFF;
                        // ★ 2026-09-21 硬上限：本件是否突破"H4 该格口计划件数"（突破 ⇒ 不写落格明细 ⇒ 不进 H7）
                        //   声明在这里（与 bDedupLanded 同级）：明细写入点在该块之外，需跨块传递。
                        bool bOverHardCeil = false;
                        if (cfg.sortingEpcDedup && m_pWaveMgr->isCodeSorted(e.code))
                        {
                            if (rescanEpcs.contains(e.code))
                            {
                                const QString orderCodeNow = m_pWaveMgr->orderCode();
                                // ★ 2026-09-13 异常及时清理：该 EPC 此前掉入异常口、本次被重新投递并落格
                                //   → 无论落在首落格口还是别的格口，都算"落格操作成功"，异常数立即减去
                                const bool wasExcRescan = m_pWaveMgr->removeExceptionOnSorted(e.code);
                                if (wasExcRescan)
                                {
                                    HTTP_LOG_INFO("[异常清理] code=%s 重投后已落格 → 处理/异常口 -1", e.code.toLocal8Bit().data());
                                    emit logMessage(QString::fromUtf8("[异常清理] EPC %1 重投后已落格到格口%2 → 已从「处理/异常口」中减去")
                                        .arg(e.code).arg(e.grid));
                                    if (m_pSortingDb && m_pSortingDb->isOpen())
                                        m_pSortingDb->markExceptionResolved(orderCodeNow, e.code);
                                }
                                // 首落格口优先用内存去重集合（已写过明细的格口），DB 查询仅作兜底
                                QString firstGrid = lastDetailGridOf(e.code);
                                if (firstGrid.isEmpty() && m_pSortingDb)
                                    firstGrid = m_pSortingDb->getFirstSortedGrid(orderCodeNow, e.code);
                                HTTP_LOG_INFO("[重扫] 重扫落格（不重复计件）code=%s order=%s 本次落格=%s 首落=%s sorted=%d",
                                    e.code.toLocal8Bit().data(), orderCodeNow.toLocal8Bit().data(),
                                    e.grid.toLocal8Bit().data(),
                                    firstGrid.isEmpty() ? "(无记录)" : firstGrid.toLocal8Bit().data(),
                                    m_pWaveMgr->sorted());

                                const bool bSameGrid = !firstGrid.isEmpty() &&
                                    parseWmsGridCodeToInt(firstGrid) == parseWmsGridCodeToInt(e.grid);
                                if (bSameGrid)
                                {
                                    // 同一格口再次落格 = 同一物理件 → 明细已存在，本次不再新增（去重）
                                    bDedupLanded = true;
                                    HTTP_LOG_INFO("[重扫] 已重新落格到原格口%s（件数不重复计，明细不重复记）epc=%s",
                                        normalizeGridKey(e.grid).toLocal8Bit().data(), e.code.toLocal8Bit().data());
                                    emit logMessage(QString::fromUtf8("[重扫] EPC %1 已重新落格到原格口%2（件数不重复计、明细不重复记）")
                                        .arg(e.code).arg(e.grid));
                                }
                                else if (!firstGrid.isEmpty())
                                {
                                    // 重扫后落到**不同格口** → 该格口此前没有本件明细，允许补记一条（口径调整）
                                    HTTP_LOG_WARN("[重扫] 重扫落格号与首落不一致 code=%s order=%s 首落=%s 本次=%s"
                                                  "—— 本次按实际落格格口补记明细（同一 EPC 在不同格口各 1 条）",
                                        e.code.toLocal8Bit().data(), orderCodeNow.toLocal8Bit().data(),
                                        firstGrid.toLocal8Bit().data(), e.grid.toLocal8Bit().data());
                                    emit logMessage(QString::fromUtf8("[重扫] EPC %1 重扫落格到格口%2，与首落格口%3 不同"
                                                                      "—— 将在格口%2 补记 1 条明细，请现场核查")
                                        .arg(e.code).arg(e.grid).arg(firstGrid), true);
                                    if (m_pSortingDb && m_pSortingDb->isOpen())
                                    {
                                        ExceptionRecord exRe;
                                        exRe.type      = QString::fromUtf8("重扫格口不一致");
                                        exRe.orderCode = orderCodeNow;
                                        exRe.epc       = e.code;
                                        exRe.sku       = sku;
                                        exRe.reason    = QString::fromUtf8("重扫落格号=%1 与首落格号=%2 不一致（按实际格口补记明细）")
                                                             .arg(e.grid).arg(firstGrid);
                                        exRe.time      = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
                                        m_pSortingDb->insertException(exRe);
                                    }
                                }
                                else
                                {
                                    // 无首落记录（如明细表缺失）→ 按正常落格处理并补记明细
                                    bDedupLanded = true;
                                    HTTP_LOG_WARN("[重扫] 无首落记录 epc=%s grid=%s —— 按本次实际落格补记明细",
                                        e.code.toLocal8Bit().data(), e.grid.toLocal8Bit().data());
                                }
                            }
                            else
                            {
                                HTTP_LOG_WARN("EPC防重拦截 code=%s order=%s 重复反馈计件（明细按 (格口,EPC) 去重）",
                                    e.code.toLocal8Bit().data(),
                                    m_pWaveMgr->orderCode().toLocal8Bit().data());
                                // ★ 2026-09-13 异常及时清理（先判定再计件，理由同"正常落格"分支）
                                const bool wasExcDup = m_pWaveMgr->removeExceptionOnSorted(e.code);
                                m_pWaveMgr->markSorted(e.code); // 重复反馈也计 1 件（以PLC实时记录为准）
                                landedEpcs.insert(e.code);   // ★ 2026-09-14 已落格 → 批处理后归零超时计时/在途
                                if (wasExcDup)
                                {
                                    HTTP_LOG_WARN("[异常清理] code=%s 重复反馈落格 → 处理/异常口 -1", e.code.toLocal8Bit().data());
                                    if (m_pSortingDb && m_pSortingDb->isOpen())
                                        m_pSortingDb->markExceptionResolved(m_pWaveMgr->orderCode(), e.code);
                                }
                                bDedupLanded = true;
                            }
                        }
                        // 数据库级防重（sort_txn 是否有记录）：同上——只判计数，明细统一由 (格口,EPC) 去重把关
                        if (cfg.sortingEpcDedup && bDedupLanded == false &&
                            m_pSortingDb && m_pSortingDb->isEpcAlreadySorted(m_pWaveMgr->orderCode(), e.code))
                        {
                            HTTP_LOG_WARN("EPC防重拦截(DB) code=%s order=%s 数据库已有记录（明细按 (格口,EPC) 去重）",
                                e.code.toLocal8Bit().data(),
                                m_pWaveMgr->orderCode().toLocal8Bit().data());
                            // ★ 2026-09-13 异常及时清理（同上）
                            const bool wasExcDb = m_pWaveMgr->removeExceptionOnSorted(e.code);
                            m_pWaveMgr->markSorted(e.code); // 同步内存状态（该反馈计 1 件）
                            landedEpcs.insert(e.code);   // ★ 2026-09-14 已落格 → 批处理后归零超时计时/在途
                            if (wasExcDb && m_pSortingDb->isOpen())
                                m_pSortingDb->markExceptionResolved(m_pWaveMgr->orderCode(), e.code);
                            bDedupLanded = true;
                        }
                        // 重复类落格（重扫/重复反馈/DB防重）已在此处完成计数与异常清理
                        // → 跳过"正常落格"的计件与超计划预警，直达下方"按格口记录分拣明细"（由 (格口,EPC) 去重把关）
                        if (!bDedupLanded)
                        {

                        // ──── 正常落格处理 ────
                        // ★ 2026-09-07 客户确认：不做格口计划上限限制，只如实记录落格已分拣数量。
                        //   原「格口达计划上限 拒收/入异常口」策略已移除——多SKU共格时按SKU计划数误拒
                        //   （mock 实测 6/12），且职责上只需记录已分拣数量，超不超由 WMS 计划侧把握。
                        //   ★ 2026-09-21 硬上限补强（客户口径：格口计划多少件就只分多少件）：
                        //     下发侧已按"H4计划 − 已落 − 在途 > 0"硬拦（含回退路径），正常不会超；
                        //     但 PLC 偏投/人工硬塞仍可能让件物理落入已满额格口 —— 此时：
                        //       · 照实登记已分拣（账实优先，与既有口径一致）；
                        //       · 写异常表 `超计划落格(SKU×格口×属性)` + ERROR（现场立即可见）；
                        //       · **不写落格明细** ⇒ 不进 H7 报文（上传数量绝不超过计划，避免 WMS 驳回）。

                        // ★ 2026-09-13 计划数封顶：真实落格计数 + 超计划预警登记
                        //   计数口径 = PLC 确认真正落入该格口的去重 EPC 数（额度消耗源，见 clearBoxLandedCount 注释）
                        //   ⑤ 超出计划时不改任何上传数据（乙方案下由 clampFullboxQtyToPlan 在报文构建时裁剪），
                        //     这里只登记预警，供波次面板「预警」数字与「查看」弹窗、以及异常表留痕。
                        {
                            const QString curSku = m_pEpcCache ? m_pEpcCache->get(e.code) : QString();
                            if (!curSku.isEmpty())
                            {
                                // 本件落格容器（仅用于日志/预警展示，与下方 rec.boxcode 同口径）
                                QString curBox;
                                {
                                    std::lock_guard<std::mutex> lockBind(m_containerMutex);
                                    curBox = m_containerBindings.value(e.grid);
                                    if (curBox.isEmpty())
                                    {
                                        bool okB = false;
                                        const int gB = e.grid.toInt(&okB);
                                        if (okB && gB >= 1)
                                            curBox = m_containerBindings.value(
                                                QString("%1").arg(gB, GRID_KEY_PADDING, 10, QChar('0')));
                                    }
                                }

                                const QString gridKey = normalizeGridKey(e.grid);
                                // ★ 2026-09-21 硬上限：本件落入之前该 (SKU,格口) 的已落去重件数
                                //   （在 noteGridLanded 登记本件之前取，用来判定"本件是超出的第几件"）
                                const int landedBefore = landedCountOf(gridKey, curSku);
                                const int planH4 = h4PlanQtyOfGrid(curSku, gridKey);

                                // ★ 2026-09-14 计划分配表：**唯一权威**的计划/已落/在途来源
                                //   commitLandedAlloc 在锁内一次完成：
                                //     ① 已落格登记（同 EPC 去重，不变量①）
                                //     ② 释放该件在途认领（与已落格解耦）
                                //     ③ 返回本格口计划件数 + 本格口已落件数（供超计划判定）
                                //   ★ 计划件数不再用"SKU 计划总数"兜底：多格口场景下会把每个格口
                                //     都误判为不超计划（这是现场"人工多投没被拦住"的根因）。
                                int landedNow = 0, planQtyNow = 0;
                                bool claimMismatch = false;
                                // ★ 2026-09-26：lateStubUsed / unitGuessed / landedType 在"本件全流程"作用域声明
                                //   （见 bDedupLanded 处的说明）——此处只做赋值，供下方落格明细持久化使用。
                                commitLandedAlloc(curSku, gridKey, e.code, 0,
                                                  &claimMismatch, &landedNow, &planQtyNow,
                                                  &lateStubUsed, &unitGuessed, 0xFF, &landedType);
                                if (lateStubUsed) m_allocLateStubCnt.fetch_add(1);
                                if (unitGuessed)  m_allocUnitGuessCnt.fetch_add(1);
                                // 分配表未生效（valid=false）时退回老口径（保持改造前行为，不误报）
                                const int planQty = m_allocValid.load() ? planQtyNow : 0;

                                // ★ 预警面板计数（m_boxLandedEpcs）仍按"PLC 实测已落入的 EPC"登记：
                                //   与分配表解耦 → 即使分配表失效，波次面板「预警」数字依然可用（稳定性）
                                noteGridLanded(curSku, gridKey, e.code);

                                // 本格口类型（0=正常分拣, 1=异常, 2=发货）——便于人工判断该口性质
                                const QString gridTypeNow = QString::number(gridTypeOfGridInPlan(curSku, gridKey));

                                // ★★ 2026-09-21 硬上限红线：本件落入前该格口已达/超过 H4 计划 ⇒ 本件是"超出件"
                                //    判定基准：H4 计划（不可变快照）。分配表计划的漂移不再影响这条红线。
                                if (planH4 > 0 && landedBefore >= planH4)
                                {
                                    bOverHardCeil = true;
                                    const QString typeName = (gridTypeNow == "2") ? QString::fromUtf8("发货")
                                                           : (gridTypeNow == "1") ? QString::fromUtf8("异常")
                                                                                  : QString::fromUtf8("分类");
                                    int sentGrid = 0;
                                    {
                                        std::lock_guard<std::mutex> lkSent(m_lastSentGridMutex);
                                        sentGrid = m_lastSentGrid.value(e.code, 0);
                                    }
                                    HTTP_LOG_ERROR("[计划上限] 落格突破 H4 计划：SKU=%s 格口%s(%s) 计划%d件 "
                                                   "本件之前已落%d件（本件为超出件）epc=%s 容器=%s 本件下发目标=%d "
                                                   "—— 已照实计已分拣、写异常表，但**不写落格明细**（不进 H7）",
                                        curSku.toLocal8Bit().data(), gridKey.toLocal8Bit().data(),
                                        typeName.toLocal8Bit().data(), planH4, landedBefore,
                                        e.code.toLocal8Bit().data(), curBox.toLocal8Bit().data(), sentGrid);
                                    emit logMessage(QString::fromUtf8(
                                        "[计划上限] SKU %1 格口%2(%3) 计划%4件已满 —— EPC %5 仍落入该格口（本件之前已落%6件）："
                                        "该件不会进入 H7 报文，请现场取出并核对（下发侧已按计划硬拦，出现多半是 PLC 偏投/人工硬塞）")
                                        .arg(curSku).arg(gridKey).arg(typeName).arg(planH4).arg(e.code).arg(landedBefore), true);
                                    if (m_pSortingDb && m_pSortingDb->isOpen())
                                    {
                                        ExceptionRecord exHard;
                                        exHard.type      = QString::fromUtf8("超计划落格(%1)").arg(typeName);
                                        exHard.orderCode = m_pWaveMgr ? m_pWaveMgr->orderCode() : QString();
                                        exHard.epc       = e.code;
                                        exHard.sku       = curSku;
                                        exHard.reason    = QString::fromUtf8(
                                            "格口%1(%2) H4计划%3件，本件落格前已落%4件 ⇒ 超出计划；"
                                            "容器%5，本件下发目标格口=%6；已计已分拣、不写落格明细、不进 H7，请人工取出核对")
                                            .arg(gridKey).arg(typeName).arg(planH4).arg(landedBefore)
                                            .arg(curBox).arg(sentGrid);
                                        exHard.time      = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
                                        m_pSortingDb->insertException(exHard);
                                    }
                                }

                                if (claimMismatch)
                                {
                                    // 认领不匹配（人工硬塞 / 认领已被超时释放）→ 账实照记，另留痕
                                    // ★ 2026-09-26 追加**单元归属**说明（同一 (SKU,格口) 可有多类型单元）：
                                    //   · 命中归属存根 → 记回原认领单元，属性未串（预期路径，无需人工干预）；
                                    //   · 走启发式     → 多单元却无法判定，已按首个有余量单元记账（需人工核对）。
                                    //   ★ 只追加说明、不改异常类型名（`分配表认领不匹配` 是既有健康检查判据）。
                                    const QString attrNote =
                                        lateStubUsed
                                            ? QString::fromUtf8("【归属已恢复】按原认领单元%1|%2记账（认领已释放，属性未串）")
                                                  .arg(gridKey).arg(PlanAllocTable::typeNameOf(landedType))
                                            : (unitGuessed
                                                   ? QString::fromUtf8("【属性归属未知】该格口有多类型单元，"
                                                                       "已按首个有余量单元%1|%2记账，请人工核对")
                                                         .arg(gridKey).arg(PlanAllocTable::typeNameOf(landedType))
                                                   : QString());
                                    HTTP_LOG_WARN("[分配表] 认领不匹配 epc=%s sku=%s grid=%s 计划%d件 已落%d件 "
                                                  "—— 落格计数照实登记（账实优先），请核对是否人工放置 %s",
                                        e.code.toLocal8Bit().data(), curSku.toLocal8Bit().data(),
                                        gridKey.toLocal8Bit().data(), planQtyNow, landedNow,
                                        attrNote.toLocal8Bit().data());
                                    if (m_pSortingDb && m_pSortingDb->isOpen())
                                    {
                                        ExceptionRecord exMm;
                                        exMm.type      = QString::fromUtf8("分配表认领不匹配");
                                        exMm.orderCode = m_pWaveMgr ? m_pWaveMgr->orderCode() : QString();
                                        exMm.epc       = e.code;
                                        exMm.sku       = curSku;
                                        exMm.reason    = QString::fromUtf8(
                                            "格口%1 容器%2 计划%3件 已落%4件 —— 该件无在途认领或认领已超时释放；"
                                            "落格计数已照实登记，请核对是否人工放置/是否有未下发件落入")
                                            .arg(gridKey).arg(curBox).arg(planQtyNow).arg(landedNow)
                                            + (attrNote.isEmpty() ? QString() : QString::fromUtf8("；") + attrNote);
                                        exMm.time      = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
                                        m_pSortingDb->insertException(exMm);
                                    }
                                }
                                else if (unitGuessed)
                                {
                                    // 无在途认领（人工硬塞 / PLC 偏投 / 切回重建）且该格口有多个单元
                                    //   → 无法判定属性归属：只做节流日志留痕，不写异常表
                                    //   （切回重建会逐件走这条路径，按件写异常表会把表刷爆）
                                    const int nGuess = m_allocUnitGuessCnt.load();
                                    if (nGuess <= 20 || (nGuess % 100) == 0)
                                        HTTP_LOG_WARN("[分配表] 属性归属未知（无在途认领）epc=%s sku=%s grid=%s "
                                                      "→ 已按首个有余量单元%1|%2记账（第%d件；前20件+每100件打印）",
                                            e.code.toLocal8Bit().data(), curSku.toLocal8Bit().data(),
                                            gridKey.toLocal8Bit().data(),
                                            PlanAllocTable::typeNameOf(landedType).toLocal8Bit().data(),
                                            nGuess);
                                }

                                // ★ 2026-09-21：超计划预警的判定基准 = min(分配表计划, H4 计划)，
                                //   与"硬上限"和"H7 裁剪"完全同一口径（分配表漂移不再放大上限）
                                const int planForWarn = (planH4 > 0 && planQty > 0) ? qMin(planH4, planQty)
                                                       : (planH4 > 0 ? planH4 : planQty);
                                const bool bOverPlan = (m_allocValid.load() && planForWarn > 0 && landedNow > planForWarn);
                                if (bOverPlan)
                                {
                                    const QString orderNow = m_pWaveMgr ? m_pWaveMgr->orderCode() : QString();
                                    const int overQty = landedNow - planForWarn;
                                    const QString typeNameW = (gridTypeNow == "2") ? QString::fromUtf8("发货")
                                                            : (gridTypeNow == "1") ? QString::fromUtf8("异常")
                                                                                   : QString::fromUtf8("分类");
                                    HTTP_LOG_WARN("[超计划-预警] 格口%s(%s) 容器%s SKU=%s EPC=%s 本格口计划%d件 "
                                                  "实际落格%d件 多余%d件 → 已登记预警；该多余件不进入上传报文，请现场取出",
                                        gridKey.toLocal8Bit().data(), typeNameW.toLocal8Bit().data(),
                                        curBox.toLocal8Bit().data(),
                                        curSku.toLocal8Bit().data(), e.code.toLocal8Bit().data(),
                                        planForWarn, landedNow, overQty);
                                    emit logMessage(QString::fromUtf8(
                                        "[超计划] 格口%1(%2) 容器%3 SKU %4：计划%5件 实际落格%6件 多余%7件"
                                        "（已在波次面板登记预警，多余件不进入上传报文，请现场取出）")
                                        .arg(gridKey).arg(typeNameW).arg(curBox).arg(curSku)
                                        .arg(planForWarn).arg(landedNow).arg(overQty), true);

                                    if (m_pSortingDb && m_pSortingDb->isOpen())
                                    {
                                        ExceptionRecord exOver;
                                        // ★ 区分语义：有计划但已满额 = "错投超计划"（人工多放/同时两件在线）；
                                        //   原 type=超计划多入 保留（既有核对脚本与异常面板口径不变）
                                        exOver.type      = QString::fromUtf8("错投超计划");
                                        exOver.orderCode = orderNow;
                                        exOver.epc       = e.code;
                                        exOver.sku       = curSku;
                                        exOver.reason    = QString::fromUtf8(
                                            "格口%1(%2) 容器%3 本格口计划%4件 实际落格%5件 多余%6件（本件为多余件之一）；"
                                            "该多余件不进入上传报文，请现场取出")
                                            .arg(gridKey).arg(typeNameW).arg(curBox).arg(planForWarn)
                                            .arg(landedNow).arg(overQty);
                                        exOver.time      = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
                                        m_pSortingDb->insertException(exOver);
                                    }
                                }
                            }
                        }

                        // ★ 2026-09-13 异常及时清理（客户要求"异常要能及时清理，不是一直保留"）：
                        //   先判"该 EPC 是否仍在异常口"，再计已分拣——
                        //   顺序很关键：markSorted 内部也会移除异常集合（防双计），
                        //   若先 markSorted，这里的判定就永远为 false，异常留痕无法闭环。
                        const bool wasExcNormal = m_pWaveMgr->removeExceptionOnSorted(e.code);
                        m_pWaveMgr->markSorted(e.code);
                        landedEpcs.insert(e.code);   // ★ 2026-09-14 已落格 → 批处理后归零超时计时/在途
                        if (wasExcNormal)
                        {
                            // 处理/异常口数量已 −1（二者同源）；把该 EPC 的异常留痕归档为"已处理"
                            HTTP_LOG_WARN("[异常清理] code=%s 已成功落格(正常分拣) → 处理/异常口 -1 当前=%d order=%s",
                                e.code.toLocal8Bit().data(), m_pWaveMgr->exception(),
                                m_pWaveMgr->orderCode().toLocal8Bit().data());
                            emit logMessage(QString::fromUtf8("[异常清理] EPC %1 已成功落格到格口%2 → 已从「处理/异常口」中减去")
                                .arg(e.code).arg(e.grid));
                            if (m_pSortingDb && m_pSortingDb->isOpen())
                                m_pSortingDb->markExceptionResolved(m_pWaveMgr->orderCode(), e.code);
                        }

                        // ★ 记录该格口已分拣件数（只计数，不做上限拦截）
                        {
                            bool ok = false;
                            int gn = e.grid.toInt(&ok);
                            QString gridKey = ok ? QString::number(gn) : e.grid;
                            std::lock_guard<std::mutex> lock(m_gridCountMutex);
                            m_gridSortedCount[gridKey]++;
                        }
                        }   // ← if (!bDedupLanded) 结束：以上为"正常落格"的计件与超计划预警

                        // ──── 按格口记录分拣明细（锁格/满箱时回传 WMS 用）────
                        // ★ 2026-09-14 落错格拦截（客户确认：落错格的件不进 H7 明细）：
                        //   件落到的格口不在该 SKU 计划内（人工硬塞/塞错箱子）→ **不写落格明细**，
                        //   因此不会出现在该格口的 H7 报文里。理由：该格口对这个 SKU 没有计划数量，
                        //   一旦上报，WMS 侧出现"该格口上报了它无计划的 SKU"，可能按格口校验而整条驳回。
                        //   该件仍计已分拣（PLC 报成功口径不变），异常表留痕 + UI 提示人工取出。
                        //   ★ 边界：异常口(66) 是系统主动改投，不属于"落错格"（其件在更上方分支已被拦下，不会走到这里）。
                        const bool bWrongGrid = m_allocValid.load() && !isGridInPlanOf(sku, e.grid)
                                                && !isExceptionGridKey(normalizeGridKey(e.grid));
                        if (bWrongGrid)
                        {
                            // 计划格口清单（分配表口径：格口(类型):件数），便于现场直接放回
                            QStringList planList;
                            if (m_allocValid.load())
                            {
                                std::lock_guard<std::mutex> lk(m_allocMutex);
                                const QVector<qint16> grids = m_alloc.gridsOf(sku);
                                for (qint16 g : grids)
                                {
                                    const QString gk = PlanAllocTable::gridKeyOf(g);
                                    planList << QString("%1(%2):%3件").arg(gk)
                                                    .arg(PlanAllocTable::typeNameOf(
                                                        (quint8)m_alloc.gridTypeOf(sku, gk)))
                                                    .arg(m_alloc.planQtyOf(sku, gk));
                                }
                            }
                            HTTP_LOG_WARN("[落错格] 件未写入落格明细（不进 H7）epc=%s sku=%s 实际格口=%s 计划格口=[%s] "
                                          "—— 已计已分拣并留痕，请人工取出并放回计划格口",
                                e.code.toLocal8Bit().data(), sku.toLocal8Bit().data(),
                                normalizeGridKey(e.grid).toLocal8Bit().data(),
                                planList.join(QString::fromUtf8(",")).toLocal8Bit().data());
                            emit logMessage(QString::fromUtf8(
                                "[落错格] EPC %1（SKU %2）落到了无计划的格口%3 —— 该件不上传WMS，请人工取出放回计划格口[%4]")
                                .arg(e.code).arg(sku).arg(normalizeGridKey(e.grid))
                                .arg(planList.join(QString::fromUtf8(","))), true);

                            // 异常表留痕（type=错投无计划格口；与原 wrong_grid 并存，便于归类统计）
                            if (m_pSortingDb && m_pSortingDb->isOpen())
                            {
                                ExceptionRecord exWg;
                                exWg.type      = QString::fromUtf8("错投无计划格口");
                                exWg.orderCode = m_pWaveMgr ? m_pWaveMgr->orderCode() : QString();
                                exWg.epc       = e.code;
                                exWg.sku       = sku;
                                exWg.reason    = QString::fromUtf8(
                                    "实际落格%1，但该 SKU 在 %1 无计划（计划格口=[%2]）；该件不进入上传报文，请人工取出放回")
                                    .arg(normalizeGridKey(e.grid)).arg(planList.join(QString::fromUtf8(",")));
                                exWg.time      = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
                                m_pSortingDb->insertException(exWg);
                            }
                            // ★ 2026-09-26：件落到"该 SKU 无计划"的格口 ⇒ 没进计划格口，
                            //   其原本认领的额度不该被消耗 → 即时归还（重投按新件判定）
                            missedPlanLandingEpcs << e.code;
                        }
                        // ★ 2026-09-14 去重把关（客户确认：同一 EPC 同波次同格口只记 1 条）：
                        //   重复类落格（重扫重投 / 重复反馈 / DB防重）只要该 (格口,EPC) 已有明细，
                        //   就不再新增第二条 —— 否则同一物理件会在换箱前后各写一条，
                        //   H7 按容器聚合上报时"旧箱+新箱各 1 件"，WMS 侧数量对不上并整条驳回。
                        if (bWrongGrid)
                        {
                            // 落错格 → 不写明细（既不留内存、也不落库），仅上述告警与异常留痕
                        }
                        else if (bOverHardCeil)
                        {
                            // ★ 2026-09-21 硬上限：本件突破了"H4 该格口计划件数"→ **不写落格明细**
                            //   ⇒ 不进该格口任何容器的 H7 报文（上传数量严格不超过计划），
                            //   异常表已写 `超计划落格(属性)`、UI 已告警，请现场取出。
                            HTTP_LOG_WARN("[计划上限] 落格明细不写入（不进 H7）epc=%s grid=%s sku=%s "
                                          "—— 该 (SKU,格口) 已达 H4 计划件数，本件为超出件（已计已分拣、已留痕）",
                                e.code.toLocal8Bit().data(), normalizeGridKey(e.grid).toLocal8Bit().data(),
                                sku.toLocal8Bit().data());
                        }
                        else if (isLandingDetailRecorded(normalizeGridKey(e.grid), e.code))
                        {
                            HTTP_LOG_WARN("[落格明细去重] 同一 EPC 在本波次该格口已有明细，本次不重复记录 "
                                          "epc=%s grid=%s sku=%s（件仍只有 1 件，账实保持 1:1；已分拣计数照计）",
                                e.code.toLocal8Bit().data(), normalizeGridKey(e.grid).toLocal8Bit().data(),
                                sku.toLocal8Bit().data());
                        }
                        else
                        {
                            // ★ 2026-09-26：重复类落格（重扫重投/重复反馈/DB 防重）不走 commitOnLanded，
                            //   单元类型从分配表反查 ⇒ 落格明细的 grid_type 与"该件实际记账的单元"一致，
                            //   切回/断电重建才能按类型精确归属（查不到 = 本件未进计划单元 → 留空）。
                            if (landedType == 0xFF && m_allocValid.load() && !sku.isEmpty())
                            {
                                std::lock_guard<std::mutex> lk(m_allocMutex);
                                const int t = m_alloc.landedTypeOfEpc(sku, normalizeGridKey(e.grid), e.code);
                                if (t >= 0 && t <= 2) landedType = (quint8)t;
                            }
                            markLandingDetailRecorded(normalizeGridKey(e.grid), e.code);
                            {
                            std::lock_guard<std::mutex> lock(m_gridRecordMutex);
                            GridSortRecord rec;
                            rec.inco   = e.code;                                  // EPC（链路主键）
                            rec.sku    = m_pEpcCache ? m_pEpcCache->get(e.code) : QString();  // ★ SKU（EPC→SKU 映射，落格时固化，供 WMS 报文 sku 字段）
                            rec.car    = e.car;
                            rec.timeMs = e.timestampMs;
                            // ★ 2026-09-09 需求6：落格时固化当前容器号——物件行进中换绑，按落格时刻的新绑定记录
                            {
                                std::lock_guard<std::mutex> lockBind(m_containerMutex);
                                rec.boxcode = m_containerBindings.value(e.grid);
                                if (rec.boxcode.isEmpty())
                                {
                                    bool okN = false;
                                    int gN = e.grid.toInt(&okN);
                                    if (okN && gN >= 1)
                                        rec.boxcode = m_containerBindings.value(
                                            QString("%1").arg(gN, GRID_KEY_PADDING, 10, QChar('0')));
                                }
                            }
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
                                QString skuRec = m_pEpcCache ? m_pEpcCache->get(e.code) : "";
                                m_pSortingDb->insertRecord(
                                    m_pWaveMgr->orderCode(),
                                    e.code, skuRec, e.grid, e.car,
                                    e.firstCar, e.lastCar,
                                    1 /* ★ 2026-09-07 每条落格记录=1件（与 rec.gridCount 口径一致，不再写 SKU 计划数）*/,
                                    entry.volu,
                                    rec.boxcode, /* ★ 2026-09-09 需求6：落格容器号 */
                                    /* ★ 2026-09-26：落格记账单元类型（0/1/2）——切回/断电重建按类型精确归属；
                                       该件未进任何计划单元时 landedType=0xFF，落空串（不臆造类型） */
                                    (landedType <= 2) ? QString::number((int)landedType) : QString());
                            }
                            }   // ← 记录明细（去重后仅首次写入）
                        }       // ← else：本 (格口,EPC) 尚未记录过
                    }

                    if (entries.size() == 1)
                    {
                        HTTP_LOG_INFO("PLC反馈自动分拣 code=%s grid=%s",
                            entries[0].code.toLocal8Bit().data(),
                            entries[0].grid.toLocal8Bit().data());
                    }

                    // ★ 2026-09-14 本批已落格的 EPC → 回主线程做"计时归零/在途清零"（见 noteEpcLanded 说明）
                    if (!landedEpcs.isEmpty())
                        emit epcsLanded(QStringList(landedEpcs.constBegin(), landedEpcs.constEnd()));

                    // ★ 2026-09-26 额度即时归还：必须在 epcsLanded **之后**发（先清在途标志，
                    //   归还逻辑才能区分"刚落地这件"与"已被重新下发的新件"，避免误释放新认领）
                    if (!missedPlanLandingEpcs.isEmpty())
                        emit allocClaimReleaseRequested(missedPlanLandingEpcs);
                });
            }
            }   // ← 结束"按值捕获 landedEpcs 副本"的作用域
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
            //
            // ★ 2026-09-22 现场需求（第2条）：锁格 = 满箱 = 容器即将离场。
            //   先把箱号存进"离场快照"（供报文重建/留痕），再发 H7（报文用该箱号），
            //   然后**立即**清掉该格活跃绑定 + DB 归档 —— **不等 H7 回传结果**：
            //     · 清理后 hasBoundContainer()==false ⇒ 判定期即把件改投异常口，
            //       不会再出现"格口没有容器、衣服照样落进去"；
            //     · H7 报文与箱号已持久保留在 outbox_fullbox（失败自动重试，耗尽留痕等人工重传），
            //       所以"不等回执"不会丢数据。
            if (!boxCode.isEmpty())
                markBoxSnapshot(grid, boxCode);

            HTTP_LOG_INFO("[锁格→满箱] 满箱回传（H7） grid=%s box=%s", grid.toLocal8Bit().data(), boxCode.toLocal8Bit().data());
            sendFullbox(grid);

            if (ConfigManager::instance()->config().sortingClearBoxOnFullbox)
            {
                detachContainerOnFullbox(grid, boxCode, QString::fromUtf8("锁格满箱(H7待回传)"));
            }
            else
            {
                HTTP_LOG_INFO("[锁格→满箱] 容器号保留（sortingClearBoxOnFullbox=false，等H7成功才解绑） grid=%s box=%s",
                    grid.toLocal8Bit().data(), boxCode.toLocal8Bit().data());
            }

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

    // ★ 2026-09-09 需求1/现场修复：PLC 解锁（S7 边沿）——**不解除 WCS 满箱禁用**（客户口径 B：
    //   仍等 WMS 重发 H6 绑定新容器后才恢复分配），但要让操作员能区分"物理锁格"与"已解锁待重绑"：
    //   · 刷新绑定面板（该格由黄"锁格"→ 橙"已解锁·待重绑"）
    // ★ 2026-09-26 现场口径（B 方案，接在下面 1348 行说明）：解锁边沿是"恢复收件"的时刻 ——
    //   若该格已绑新箱（H6 早于解锁到达，当时被推迟恢复）则在此解除禁用、转绿；
    //   仍未绑容器的格口保持禁用（红"已解锁·待重绑"，等 WMS 重发 H6）。
    // ★ 2026-09-25 现场口径更正：**解锁不发送满箱回传（H7）——只有锁格才发满箱回传**。
    //   原实现在解锁时若发现该格留有未上传记录（锁格时波次非执行态被跳过），会按当前绑定补发一次 H7；
    //   现场明确要求取消：解锁只做状态刷新 + 留痕提示，**绝不产生任何回传报文**
    //   （解锁时点上按"当前绑定"补发，存在把报文挂到非本波次/新箱号上的风险，且会与锁格报文重复）。
    //   遗留记录不会丢：仍留在内存，可经「重锁该格」（锁格才发）、「一键满箱回传」/
    //   「重传满箱切换(H7)」手输格口，或点「结束任务」时的统一补发（flushUnreportedFullboxes）上传。
    connect(m_pPlcMgr, &PlcManager::gridUnlocked, this,
        [this](const QString& grid) {
            bool okG = false;
            const int gNum = grid.toInt(&okG);
            if (!okG) return;   // 非法格口号：不做任何状态处理（保持既有行为）

            // ★ 2026-09-26 现场口径（B 方案）：**解锁边沿 = 该格真正可以收件的时刻**。
            //   若该格"H6 已绑新箱、但当时 PLC 还锁着"（H6 处理处会把恢复收件推迟到这里），
            //   此刻补做恢复：解除满箱锁格禁用 → 面板由橙"满箱锁格"转绿"已绑定"，恢复可下发。
            //   ★ 只在"已绑定容器"时恢复：没换箱（未绑定）的格口保持红"已解锁·待重绑"，
            //     等 WMS 重发 H6 再恢复 —— 否则面板会丢掉"待重绑"这个现场需要的信号。
            bool bRestored = false;
            if (m_pPlcMgr && m_pPlcMgr->isGridDisabled(gNum) && hasBoundContainer(gNum))
            {
                m_pPlcMgr->enableGrid(gNum);
                bRestored = true;
                HTTP_LOG_INFO("[解锁] 格口%d 已解锁且已绑定容器 → 恢复收件（解除满箱锁格禁用；"
                              "H6 早于解锁到达，恢复推迟到此，见 H6 处理处的 B 方案口径）", gNum);
            }

            bool bStillDisabled = m_pPlcMgr && m_pPlcMgr->isGridDisabled(gNum);
            HTTP_LOG_INFO("[解锁] PLC解锁信号 grid=%s WCS禁用=%d（%s）",
                grid.toLocal8Bit().data(), bStillDisabled ? 1 : 0,
                bStillDisabled ? "等待 WMS 重发 H6 绑定后恢复分配" : "已可用");
            emit logMessage(bRestored
                ? QString("[S7] 解锁 grid=%1 —— 已绑定容器，格口已恢复收件").arg(grid)
                : (bStillDisabled
                      ? QString("[S7] 解锁 grid=%1 —— 等待 WMS 重绑(H6) 后恢复分配").arg(grid)
                      : QString("[S7] 解锁 grid=%1 —— 格口已可用").arg(grid)));
            emit bindingUpdated();

            // ★ 2026-09-25 现场口径：解锁**不补发**满箱回传（H7）——只有锁格才发送满箱回传。
            //   这里只做留痕：提示该格仍有未上传记录及可用的人工/锁格补传入口，
            //   不发起任何满箱回传（本 lambda 内不出现满箱回传发送调用，契约见 tests ⑮）。
            int pendingRecords = 0;
            {
                std::lock_guard<std::mutex> lock(m_gridRecordMutex);
                auto it = m_gridSortRecords.find(grid);
                if (it != m_gridSortRecords.end())
                    pendingRecords = it.value().size();
            }
            if (pendingRecords > 0)
            {
                HTTP_LOG_WARN("[解锁] 格口%s 有 %d 条未上传的落格记录 —— 按现场口径解锁**不发送**满箱回传（H7），"
                              "只有锁格才发送；记录保留在内存，可用「一键满箱回传」/「重传满箱切换(H7)」手输格口补传，"
                              "或该格再次锁格、点「结束任务」时统一补发",
                    grid.toLocal8Bit().data(), pendingRecords);
                emit logMessage(QString::fromUtf8(
                    "[解锁] 格口%1 有 %2 条未上传记录：解锁不发送满箱回传（只有锁格才发）——"
                    "记录已保留，请用「一键满箱回传」/「重传满箱切换(H7)」补传，或结束任务时统一补发")
                    .arg(grid).arg(pendingRecords), true);
            }
        }, Qt::QueuedConnection);

    // ★ 2026-09-26 兜底对账（B 方案配套）见 reconcileStrandedDisabledGrids()：**不用 s7Connected 触发**
    //   —— S7 刚连上时 PlcManager 的锁格快照还是掉线前的旧值（轮询 1s 后才刷新），此刻判"锁格位=0"
    //   会读到 stale=true 而漏救。改为挂在 10s 健康检查周期上（见 logHealthStatus 末尾），
    //   无论"边沿丢失"还是"快照过期"，最迟 10 秒内自愈。

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

    // ★ 2026-09-22 现场口径（最终定稿）：满箱回传（H7）统一补发完成后，固定延迟 END_REPORT_DELAY_MS
    //   （默认 1 秒）再发送完结回传（H8）—— **不论 H7 是否成功**（结果与完结回传完全分开）。
    m_endDelayTimer = new QTimer(this);
    m_endDelayTimer->setSingleShot(true);
    connect(m_endDelayTimer, &QTimer::timeout, this, &HttpServer::onEndDelayTimeout);

    // ★ 2026-09-02 修复"结束任务卡死/闪退"：H8 完结回传会话兜底定时器（单次）
    //   sendEnd() 启动；成功/耗尽/超时任一结束点都会 stop 并发出 endReportFinished，
    //   保证 MainWindow 的等待必然结束（不卡死），服务停止后软件保持运行
    m_endSessionTimer = new QTimer(this);
    m_endSessionTimer->setSingleShot(true);
    connect(m_endSessionTimer, &QTimer::timeout, this, &HttpServer::onEndSessionTimeout);

    // ★ 2026-09-14 计划分配表：认领超时清扫 + 不变量巡检 + 下发失败待释放队列处理
    //   周期 30s（可配 allocAuditIntervalMs）。只做内存遍历（O(在途)）+ 一次 O(计划单元)
    //   巡检，不查 DB、不发网络请求，主线程开销可控。
    //   生命周期沿用本类既有定时器模式（new QTimer(this) + stopDevices() 内 stop）。
    m_allocSweepTimer = new QTimer(this);
    connect(m_allocSweepTimer, &QTimer::timeout, this, [this]() {
        drainAllocPendingRelease();   // 先处理发送失败（尽快归还额度）
        sweepPlanAllocClaims();       // 再清扫超时认领 + 不变量巡检
    });
    m_allocSweepTimer->start(ConfigManager::instance()->config().allocAuditIntervalMs > 0
                                 ? ConfigManager::instance()->config().allocAuditIntervalMs
                                 : ALLOC_AUDIT_INTERVAL_MS);

    // ★ 2026-09-15 RFID 原始报文留痕：攒批落库定时器（业务入口只入队，此处落盘，不阻塞主链路）
    m_rfidRawFlushTimer = new QTimer(this);
    connect(m_rfidRawFlushTimer, &QTimer::timeout, this, &HttpServer::flushRfidRawRows);
    m_rfidRawFlushTimer->start(RFID_RAW_FLUSH_INTERVAL_MS);

    // ★ 2026-09-15 原始报文超期清理：启动清一次（startDevices 内触发）；跨天运行在"队列空"的空闲拍再清
    m_rfidRawCleanTimer = new QTimer(this);
    connect(m_rfidRawCleanTimer, &QTimer::timeout, this, [this]() { cleanupRfidRawIfNeeded(false); });
    m_rfidRawCleanTimer->start(60 * 1000);   // 每分钟检查是否已跨天

    // ★ 2026-09-15 切出中的波次号到期自动清空（避免长期保留把新波次的 H6 记到旧波次）
    m_switchingOutTimer = new QTimer(this);
    m_switchingOutTimer->setSingleShot(true);
    connect(m_switchingOutTimer, &QTimer::timeout, this, [this]() {
        if (!m_switchingOutOrderCode.isEmpty())
        {
            HTTP_LOG_INFO("切出中的波次号 %s 保留窗口到期，自动清空（其后 H6 不再归入该波次）",
                m_switchingOutOrderCode.toLocal8Bit().data());
            m_switchingOutOrderCode.clear();
        }
    });

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
    // ★ 2026-09-15 RFID 原始报文：设备已停 → 补写残留队列（数据不丢），再关库
    if (m_rfidRawFlushTimer) m_rfidRawFlushTimer->stop();
    if (m_rfidRawCleanTimer) m_rfidRawCleanTimer->stop();
    {
        int pending = 0;
        {
            std::lock_guard<std::mutex> lock(m_rfidRawMutex);
            pending = m_rfidRawPending.size();
        }
        if (pending > 0)
        {
            LOG_INFO("[析构] step2f RFID原始报文残留队列补写 pending=%d", pending);
            flushRfidRawRows();
        }
        HTTP_LOG_INFO("RFID原始报文留痕统计 累计落库=%lld 帧 队列丢弃=%lld 帧 补写前残留=%d 帧",
            (long long)m_rfidRawFlushed, (long long)m_rfidRawDropped, pending);
    }
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
        // ★ 2026-09-15 EPC 识别长度（XML rfidEpcTruncateLen，默认 24）：解析线程即用此口径
        m_pRfidPush->setEpcTruncateLen(cfg.rfidEpcTruncateLen);
        {
            const QString ruleText = (cfg.rfidEpcTruncateLen < 2)
                ? QString::fromUtf8("不识别，整串原样使用（回退改造前行为）")
                : QString::fromUtf8("只识别『A + %1 位数字』共 %2 位")
                      .arg(cfg.rfidEpcTruncateLen - 1).arg(cfg.rfidEpcTruncateLen);
            HTTP_LOG_INFO("EPC识别口径已下发 len=%d（%s）",
                cfg.rfidEpcTruncateLen, ruleText.toLocal8Bit().data());
        }
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

    // ★ 2026-09-15 RFID 原始报文：启动清一次超期数据（保留期 RFID_RAW_RETAIN_DAYS 天）
    cleanupRfidRawIfNeeded(true);
    return true;
}

// ============================================================================
// ★ 2026-09-15 RFID 原始推送报文留痕（三层留痕的数据库一层）
//   入队（业务入口，O(1) 不写盘） → 定时/满批单事务落库 → 退出补写
//   设计约束：绝不反压 RFID→PLC 热路径；DB 写不动时丢最旧并告警
// ============================================================================
void HttpServer::noteRfidRawFrame(const QString& epc, const QString& epcRaw,
                                  const QString& carNum, const QString& seq,
                                  const QString& devCode, const QString& rawFrame,
                                  bool noread)
{
    RfidRawRecord r;
    r.time     = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    r.epc      = epc;
    r.epcRaw   = epcRaw.isEmpty() ? epc : epcRaw;   // 未归一/未识别时与 epc 相同
    r.carNum   = carNum;
    r.seq      = seq;
    r.devCode  = devCode;
    r.rawFrame = rawFrame;
    r.bytes    = rawFrame.toUtf8().size();
    r.noread   = noread;

    bool needFlush = false;
    {
        std::lock_guard<std::mutex> lock(m_rfidRawMutex);
        if (m_rfidRawPending.size() >= RFID_RAW_PENDING_MAX_ROWS)
        {
            m_rfidRawPending.removeFirst();          // 丢最旧，保证内存有界
            m_rfidRawDropped++;
            if (m_rfidRawDropped % 100 == 1)
                HTTP_LOG_WARN("RFID原始报文待写队列超限(%d)，已丢弃最旧帧 累计丢弃=%lld（DB 写入跟不上推送）",
                    RFID_RAW_PENDING_MAX_ROWS, (long long)m_rfidRawDropped);
        }
        m_rfidRawPending.append(r);
        needFlush = (m_rfidRawPending.size() >= RFID_RAW_FLUSH_MAX_ROWS);
    }
    if (needFlush)
        flushRfidRawRows();                          // 满批立即落库（单事务）
}

void HttpServer::flushRfidRawRows()
{
    if (!m_pSortingDb || !m_pSortingDb->isOpen()) return;

    QVector<RfidRawRecord> batch;
    {
        std::lock_guard<std::mutex> lock(m_rfidRawMutex);
        if (m_rfidRawPending.isEmpty()) return;
        batch.swap(m_rfidRawPending);
    }
    const int written = m_pSortingDb->insertRfidRawBatch(batch);
    m_rfidRawFlushed += written;
    if (written != batch.size())
    {
        HTTP_LOG_WARN("RFID原始报文落库不完整 待写=%d 成功=%d（详见 DataBase 日志；已写入的帧不重复）",
            (int)batch.size(), written);
    }
    else if (written > 0)
    {
        HTTP_LOG_INFO("RFID原始报文落库 %d 帧（累计 %lld 帧）", written, (long long)m_rfidRawFlushed);
    }
}

void HttpServer::cleanupRfidRawIfNeeded(bool force)
{
    if (!m_pSortingDb || !m_pSortingDb->isOpen()) return;

    const QString today = QDateTime::currentDateTime().toString("yyyy-MM-dd");
    if (!force && m_rfidRawCleanDate == today) return;   // 每天最多清一次

    // 跨天空闲拍清理：待写队列非空时先落库，避免"刚写进去就被判超期"（时间口径一致性）
    if (!force)
    {
        {
            std::lock_guard<std::mutex> lock(m_rfidRawMutex);
            if (!m_rfidRawPending.isEmpty()) return;
        }
    }
    m_rfidRawCleanDate = today;
    const int deleted = m_pSortingDb->cleanupOldRfidRaw(RFID_RAW_RETAIN_DAYS);
    HTTP_LOG_INFO("RFID原始报文超期清理完成 保留%d天 删除=%d 行（表内共 %d 行）",
        RFID_RAW_RETAIN_DAYS, deleted, m_pSortingDb->countRfidRaw());
}

// ★ 2026-09-15 EPC 识别长度下发（启动注入 + 配置热生效共用）
void HttpServer::setEpcTruncateLen(int len)
{
    if (m_pRfidPush)
        m_pRfidPush->setEpcTruncateLen(len);
    HTTP_LOG_INFO("EPC识别长度更新 len=%d（%s）", len,
        len < 2 ? "不识别，整串原样使用" : "只识别 A + N-1 位数字形态");
}

int HttpServer::epcTruncateLen() const
{
    return m_pRfidPush ? m_pRfidPush->epcTruncateLen() : RFID_EPC_TRUNCATE_LEN;
}

int HttpServer::rfidRawPendingRows() const
{
    std::lock_guard<std::mutex> lock(m_rfidRawMutex);
    return m_rfidRawPending.size();
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
    // ★ 2026-09-17 现场要求（"跳过一个波次"缺陷）：点「开始接收任务」= 解开「结束任务」
    //   造成的队列冻结；本函数下方的 maybeStartPendingWave() 因而成为**唯一的出队时机**。
    //   （冻结置位点：sendEnd()——见那里的注释与 PendingWaveQueuePolicy.h）
    m_endRequested.store(false);

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

    // ★ 2026-09-17 现场要求：「开始接收任务」也要像「新任务」一样，把**波次信息面板数据清空**
    //   （此前 stop → start 一轮后，面板仍显示上一波次的号/件数/已分拣等旧数据）
    //   口径与「新任务」完全一致：当前波次切出（内存清空；进度/明细/计划/落格/异常保留于 DB，
    //   可随时从「波次数据历史记录」切回继续）；绑定已在上面的 restoreWaveFromDB() 里清空并归档。
    //   注意顺序：必须在 restoreWaveFromDB() 之后调用（先清绑定，再切波次），并早于下面的队列执行逻辑。
    resetWavePanelToIdle(QString::fromUtf8("开始接收任务"));

    // ★ 2026-09-07 若存在"执行中排队"的待执行波次，开启接收后执行队首
    //   ★ 2026-09-17 现场要求：**这里是待执行队列唯一的出队时机**——
    //     "只有点了「开始接收任务」才开始接收队列里的任务"（点「结束任务」后队列冻结保留，
    //     不会再有终态回调把队首偷偷执行掉；判据见 PendingWaveQueuePolicy.h）。
    //     上面的 m_endRequested.store(false) 已解开冻结，因此本行的 maybeStartPendingWave()
    //     会真正取队首执行。
    // ★ 2026-09-08：若内存波次处于"分拣已结束但回传未完成"（完结中/异常挂起），先切出——
    //   数据与失败报文保留在 DB（可随时从列表切回或在下拉中重传），避免旧波次阻塞队列执行
    if (!m_pendingWaveQueue.isEmpty() && m_pWaveMgr)
    {
        int st = m_pWaveMgr->status();
        if (st == WAVE_ENDING || st == WAVE_HELD)
        {
            QString oldOrder = m_pWaveMgr->orderCode();
            HTTP_LOG_INFO("开始接收：旧波次 %s 回传未完成 status=%d，先切出再执行待执行队列 queue=%d",
                oldOrder.toLocal8Bit().data(), st, m_pendingWaveQueue.size());
            emit logMessage(QString("[波次] 旧波次 %1 回传未完成（%2），已切出（数据/失败报文保留，"
                                    "可切回或在下拉中重传）；开始执行队列下一波次")
                .arg(oldOrder).arg(WaveSnapshot::statusToString(st)));
            switchAwayCurrentWave();
        }
    }
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
    // ★ 2026-09-07 退出前把当日峰值效率落库（当天最终最大值）
    persistDailyPeak();

    // ★ 2026-09-07 防析构竞态：先断开设备 → 本对象的所有信号连接，
    //   避免 stop() 期间设备 emit（plcFeedbackBusinessBatch/plcConnected 等）打到正在析构的本对象 lambda
    if (m_pPlcMgr)   m_pPlcMgr->disconnect(this);
    if (m_pRfidPush) m_pRfidPush->disconnect(this);

    // 停止 Outbox/H8 会话定时器（进程退出时；接收停止不调用本方法）
    if (m_outboxEndTimer)     m_outboxEndTimer->stop();
    if (m_outboxFullboxTimer) m_outboxFullboxTimer->stop();
    if (m_endSessionTimer)    m_endSessionTimer->stop();
    if (m_endDelayTimer)      m_endDelayTimer->stop();     // ★ 2026-09-22 完结回传固定延迟发送器
    // ★ 2026-09-14 计划分配表清扫定时器（停止设备后不再触发，避免析构期间回调）
    if (m_allocSweepTimer)    m_allocSweepTimer->stop();
    // ★ 2026-09-15 RFID 原始报文定时器（残留队列由析构补写，此处只停触发）
    if (m_rfidRawFlushTimer)  m_rfidRawFlushTimer->stop();
    if (m_rfidRawCleanTimer)  m_rfidRawCleanTimer->stop();
    if (m_switchingOutTimer)  m_switchingOutTimer->stop();
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

// ============================================================================
// ★ 2026-09-15 启动口径（客户需求）：**每次开启软件 = 新任务状态**
//   ① 一律不把 DB 里的绑定装载到内存/界面 —— 容器绑定面板全为"未绑定"；
//   ② 把启动瞬间的 DB active 绑定全部归档（active=0 + unbind_time 留痕）：
//      · 若存在未结束波次（上次关闭切出/异常关闭遗留）→ 通知"可切回继续"，
//        切回时按 grid_box_bind.order_code 取回该波次自己的绑定（见 resumeUnfinishedWave 步骤6）；
//      · 若无未结束波次 → 属旧库遗留 active 行，同样归档（界面保持未绑定）。
//   ③ 只做提示，不自动装载任何进度 —— 等 WMS 下发新波次，或人工从「波次数据记录」切回旧波次。
//   正常退出路径已在 MainWindow::closeEvent → switchOutWaveForExit() 切出并归档；
//   本函数是崩溃/断电/强杀（未走正常退出）的兜底，保证绑定始终"已归档、波次可切回"。
// ============================================================================
void HttpServer::restoreWaveFromDB()
{
    if (!m_pSortingDb || !m_pSortingDb->isOpen()) return;

    // ① 清空内存绑定（含 setupCore 从 XML 兜底预绑定的部分）→ 界面显示"未绑定"
    {
        std::lock_guard<std::mutex> lock(m_containerMutex);
        if (!m_containerBindings.isEmpty())
        {
            for (auto it = m_containerBindings.constBegin(); it != m_containerBindings.constEnd(); ++it)
            {
                HTTP_LOG_INFO("[解绑留痕] grid=%s 旧箱=%s 原因=启动新任务状态（内存绑定清空）",
                    it.key().toLocal8Bit().data(), it.value().toLocal8Bit().data());
            }
            m_containerBindings.clear();
            m_boxSnapshot.clear();   // ★ 2026-09-22 离场箱号快照随绑定一并清空
        }
    }

    // ② 归档 DB 中的 active 绑定（行保留，含波次归属与解绑时间，供切回恢复与追溯）
    QVector<GridBoxBindRecord> actives = m_pSortingDb->getAllActiveBinds();
    for (const GridBoxBindRecord& b : actives)
    {
        HTTP_LOG_INFO("[解绑留痕] grid=%s 旧箱=%s order=%s bindTime=%s 原因=启动新任务状态（DB绑定归档）",
            b.gridNum.toLocal8Bit().data(), b.boxcode.toLocal8Bit().data(),
            b.orderCode.toLocal8Bit().data(), b.bindTime.toLocal8Bit().data());
    }
    bool archived = true;
    if (!actives.isEmpty())
    {
        archived = m_pSortingDb->archiveAllBinds();
        HTTP_LOG_INFO("启动归档 active 绑定 count=%d 结果=%s（历史行保留，仅 active=0 + unbind_time 留痕；"
                      "**不装载到界面** → 面板为新任务状态）",
            (int)actives.size(), archived ? "成功" : "失败");
    }

    // ③ 未结束波次提示（不装载进度，仅告知可切回）
    //   ★ 2026-09-16 需求④：本函数是"开机后绑定显示必然为空"的保障 —— 内存已在①清空、
    //     DB active 行已在②归档，且**任何来源的绑定都不会在这里被装载到界面**。
    ReturnWaveRecord wave = m_pSortingDb->getLatestUnfinishedWave();
    if (wave.orderCode.isEmpty())
    {
        HTTP_LOG_INFO("启动为新任务状态（未绑定）：无未结束波次，DB 遗留 active 绑定=%d 个已归档；等待 WMS 下发新波次",
            (int)actives.size());
        emit logMessage(QString::fromUtf8("[容器绑定] 启动为新任务状态（全部格口未绑定）——等待 WMS 下发新波次"
                                          "（历史绑定已在数据库归档留痕，可追溯）"));
        return;
    }

    QVector<ReturnWaveItemRecord> items = m_pSortingDb->getWaveItems(wave.orderCode);
    HTTP_LOG_INFO("启动为新任务状态（未绑定）：检测到未结束波次 order=%s status=%d(%s) items=%d updatedAt=%s "
                  "DB active 绑定 %d 个已归档保留 —— 切回该波次时随波次一起恢复",
        wave.orderCode.toLocal8Bit().data(), wave.status,
        WaveSnapshot::statusToString(wave.status).toLocal8Bit().data(), items.size(),
        wave.updatedAt.toLocal8Bit().data(), (int)actives.size());
    emit logMessage(QString::fromUtf8(
        "[容器绑定] 启动为新任务状态（全部格口未绑定）；检测到未结束波次 %1（%2，明细 %3 条）——"
        "进度与格口绑定已归档保留于数据库，可在「波次数据记录」切回继续（切换时绑定与进度一并恢复）")
        .arg(wave.orderCode).arg(WaveSnapshot::statusToString(wave.status)).arg(items.size()),
        !archived);
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
    // ★ 2026-09-09（客户要求）：清理旧绑定【之前】逐格留痕日志（格口号/旧箱号），
    //   确保"清空"动作后仍能追溯每个格口原先绑的是什么箱子
    {
        std::lock_guard<std::mutex> lock(m_containerMutex);
        if (!m_containerBindings.isEmpty())
        {
            for (auto it = m_containerBindings.constBegin(); it != m_containerBindings.constEnd(); ++it)
            {
                HTTP_LOG_INFO("解绑留痕 grid=%s 旧箱=%s 原因=人工清空格口绑定 动作=DB归档+内存清空",
                    it.key().toLocal8Bit().data(), it.value().toLocal8Bit().data());
                LOG_INFO("[解绑留痕] grid=%s 旧箱=%s 原因=人工清空绑定",
                    it.key().toLocal8Bit().data(), it.value().toLocal8Bit().data());
            }
        }
    }

    int cleared = 0;
    {
        std::lock_guard<std::mutex> lock(m_containerMutex);
        cleared = m_containerBindings.size();
        m_containerBindings.clear();
        m_boxSnapshot.clear();   // ★ 2026-09-22 离场箱号快照随绑定一并清空
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

void HttpServer::onOutboxResendReply(const QString& msgId, bool isH7, bool success,
                                     int httpStatus, const QString& body, const QString& note)
{
    if (!m_pSortingDb || !m_pSortingDb->isOpen()) return;

    // ★ 2026-09-22 现场需求①：先把本次重传的**完整响应**留痕（全量落库，不截断），
    //   再做状态处理 —— 保证"查看 → 双击"看到的一定是最近一次回传的真实响应。
    m_pSortingDb->saveOutboxResponse(msgId, isH7, httpStatus, body, note);
    emit outboxResponseSaved();

    QString orderCode;
    if (isH7)
    {
        OutboxRecord rec = m_pSortingDb->getOutboxFullboxByMsgId(msgId);
        orderCode = rec.orderCode;
        if (success)
        {
            m_pSortingDb->markOutboxFullboxSuccess(msgId);   // 成功只改状态，不计数
            // ★ 2026-09-22 现场口径：**人工重传结果同样与容器绑定解耦** ——
            //   只更新这条报文的状态（补传成功即完成），不改任何格口的容器绑定：
            //   容器号只在锁格/完结补发那一刻清理；期间若已换绑新容器，绝不受本条回执影响。
            HTTP_LOG_INFO("未完成波次面板 人工重传成功（H7）msgId=%s grid=%s box=%s —— 仅更新报文状态（容器绑定不受影响）",
                msgId.toLocal8Bit().data(), rec.grid.toLocal8Bit().data(), rec.boxcode.toLocal8Bit().data());
        }
        else
        {
            // ★ 2026-09-22 现场需求（每次失败回传独立消耗次数）：**人工重传失败也消耗一次**
            //   —— 保持原状态不变，只把 retry_count + 1（旧实现失败时什么都不写，
            //      导致"手动补传失败多少次都不计数"，既看不出尝试次数也无法据上限收口）。
            const QString keepStatus = rec.status.isEmpty() ? QStringLiteral("failed") : rec.status;
            m_pSortingDb->updateOutboxFullboxStatus(msgId, keepStatus,
                QDateTime::currentDateTime().addSecs(OUTBOX_RETRY_INTERVAL_SEC)
                    .toString("yyyy-MM-dd HH:mm:ss"));
            HTTP_LOG_WARN("未完成波次面板 人工重传失败（H7）→ 消耗一次重试次数 msgId=%s status=%s retry=%d/%d",
                msgId.toLocal8Bit().data(), keepStatus.toLocal8Bit().data(),
                rec.retryCount + 1, OUTBOX_RETRY_MAX_H7);
        }
    }
    else
    {
        OutboxRecord rec = m_pSortingDb->getOutboxEndByMsgId(msgId);
        orderCode = rec.orderCode;
        if (success)
        {
            m_pSortingDb->markOutboxEndSuccess(msgId);       // 成功只改状态，不计数
        }
        else
        {
            const QString keepStatus = rec.status.isEmpty() ? QStringLiteral("failed") : rec.status;
            m_pSortingDb->updateOutboxEndStatus(msgId, keepStatus,
                QDateTime::currentDateTime().addSecs(OUTBOX_RETRY_INTERVAL_SEC)
                    .toString("yyyy-MM-dd HH:mm:ss"));
            HTTP_LOG_WARN("未完成波次面板 人工重传失败（H8）→ 消耗一次重试次数 msgId=%s status=%s retry=%d/%d",
                msgId.toLocal8Bit().data(), keepStatus.toLocal8Bit().data(),
                rec.retryCount + 1, OUTBOX_RETRY_MAX_DEFAULT);
        }
    }

    HTTP_LOG_INFO("未完成波次面板 重传结果 kind=%s msgId=%s success=%d http=%d respLen=%d",
        isH7 ? "H7" : "H8", msgId.toLocal8Bit().data(), success ? 1 : 0,
        httpStatus, body.size());
    emit outboxResendResult(orderCode, isH7 ? "fullbox" : "end", msgId, success);
    emit outboxFailedChanged();   // ★ 2026-09-08 重传成功 → 刷新失败重传下拉（条目自动消失）
    // ★ 2026-09-22 现场口径（最终定稿）：人工重传 H7 的结果**不影响**完结回传（H8）——
    //   两者完全分开：完结回传只在「结束任务」后的固定 1 秒到点发送，这里不做任何屏障复检。
}

// ============================================================================
// ★ 2026-09-08 UI 失败重传下拉（H7 失败格口 / H8 失败波次）
//   数据源：outbox 表中 status='failed'（重试耗尽）与 'cancelled'（波次切出后取消重试）；
//   仅"查询 + 重发"，不改波次状态、不阻塞主流程（重发结果走既有 outboxResendResult 回执链路）
// ============================================================================

// 从 H7 满箱报文 payload 提取格口号（内部号）——仅用于老库 grid 列为空时的兜底
static QString extractGridFromFullboxPayload(const QString& payload)
{
    QJsonDocument doc = QJsonDocument::fromJson(payload.toUtf8());
    if (!doc.isObject()) return QString();
    QJsonArray dl = doc.object().value("head").toObject().value("detailList").toArray();
    if (dl.isEmpty()) return QString();
    QString num = dl.first().toObject().value("num").toString();   // WMS 编码（前缀22+3位）
    if (num.isEmpty()) return QString();
    return parseWmsGridCodeToStr(num);                              // → 内部格口号
}

QVector<HttpServer::FailedFullboxItem> HttpServer::getFailedFullboxItemsByOrder(const QString& orderCode, int limit)
{
    QVector<FailedFullboxItem> out;
    if (!m_pSortingDb || !m_pSortingDb->isOpen() || orderCode.isEmpty()) return out;

    // ★ 2026-09-16 需求⑤：数据源改为"只取本波次"的失败/已取消满箱报文
    return aggregateFailedFullbox(m_pSortingDb->getFailedOutboxFullboxByOrder(orderCode, limit));
}

// ★ 2026-09-16 聚合抽出：按 (波次, 格口) 归并失败/已取消 H7 报文
//   口径与原 getFailedFullboxItems 完全一致（老数据 grid 缺失时从 payload 反解，仍缺则 "?"；
//   failCount = 该(波次,格口)下报文条数；failed/cancelled 混合时 status = "mixed"），
//   这样"按波次"与"全部历史"两条数据源共用同一段聚合逻辑，不存在口径漂移。
QVector<HttpServer::FailedFullboxItem> HttpServer::aggregateFailedFullbox(const QVector<OutboxRecord>& rows)
{
    QVector<FailedFullboxItem> out;
    QMap<QString, int> index;   // orderCode + 分隔符 + grid → out 下标
    for (const OutboxRecord& r : rows)
    {
        QString grid = r.grid;
        if (grid.isEmpty())
        {
            grid = extractGridFromFullboxPayload(r.payload);
            if (grid.isEmpty()) grid = "?";
        }
        const QString key = r.orderCode + QChar(0x1f) + grid;
        if (!index.contains(key))
        {
            FailedFullboxItem item;
            item.orderCode = r.orderCode;
            item.grid      = grid;
            item.lastTime  = r.createdAt;
            item.status    = r.status;
            out.append(item);
            index.insert(key, out.size() - 1);
        }
        FailedFullboxItem& item = out[index.value(key)];
        item.msgIds << r.msgId;
        ++item.failCount;
        if (item.status != r.status) item.status = "mixed";   // 同组内 failed/cancelled 混合
    }
    return out;
}

QVector<HttpServer::FailedEndItem> HttpServer::getFailedEndItems(int limit)
{
    QVector<FailedEndItem> out;
    if (!m_pSortingDb || !m_pSortingDb->isOpen()) return out;

    QVector<OutboxRecord> rows = m_pSortingDb->getFailedOutboxEnd(limit);
    QMap<QString, int> index;   // orderCode → out 下标
    for (const OutboxRecord& r : rows)
    {
        if (!index.contains(r.orderCode))
        {
            FailedEndItem item;
            item.orderCode = r.orderCode;
            item.lastTime  = r.createdAt;
            item.status    = r.status;
            out.append(item);
            index.insert(r.orderCode, out.size() - 1);
        }
        FailedEndItem& item = out[index.value(r.orderCode)];
        item.msgIds << r.msgId;
        ++item.failCount;
        if (item.status != r.status) item.status = "mixed";
    }
    return out;
}

// 精确重传：某波次 + 某格口的全部失败/已取消 H7 报文
bool HttpServer::resendFailedFullboxGrid(const QString& orderCode, const QString& grid)
{
    if (!m_pSortingDb || !m_pSortingDb->isOpen() || orderCode.isEmpty() || grid.isEmpty())
        return false;

    QVector<OutboxRecord> rows = m_pSortingDb->getOutboxFullboxByOrder(orderCode);
    int sent = 0;
    for (const OutboxRecord& r : rows)
    {
        if (r.status == "success") continue;
        QString g = r.grid;
        if (g.isEmpty()) g = extractGridFromFullboxPayload(r.payload);
        if (g != grid) continue;

        QJsonDocument doc = QJsonDocument::fromJson(r.payload.toUtf8());
        if (doc.isNull() || !doc.isObject())
        {
            HTTP_LOG_WARN("失败格口重传 payload 解析失败 order=%s grid=%s msgId=%s",
                orderCode.toLocal8Bit().data(), grid.toLocal8Bit().data(),
                r.msgId.toLocal8Bit().data());
            continue;
        }
        emit outboxResendReady("fullbox", doc.object(), r.msgId);
        ++sent;
    }

    HTTP_LOG_INFO("失败格口手动重传 order=%s grid=%s 发送=%d status=%s",
        orderCode.toLocal8Bit().data(), grid.toLocal8Bit().data(), sent,
        sent > 0 ? "已提交" : "无可用报文");
    if (sent == 0)
        emit logMessage(QString("[重传] 格口%1（波次%2）没有可重传的失败满箱报文").arg(grid).arg(orderCode), true);
    else
        emit logMessage(QString("[重传] 满箱切换(H7) 按格口补发 order=%1 grid=%2 报文数=%3（不影响主流程）")
            .arg(orderCode).arg(grid).arg(sent));
    return sent > 0;
}

// 精确重传：某波次的全部失败/已取消 H8 报文
bool HttpServer::resendFailedEnd(const QString& orderCode)
{
    if (!m_pSortingDb || !m_pSortingDb->isOpen() || orderCode.isEmpty())
        return false;

    QVector<OutboxRecord> rows = m_pSortingDb->getOutboxEndByOrder(orderCode);
    int sent = 0;
    for (const OutboxRecord& r : rows)
    {
        if (r.status == "success") continue;
        QJsonDocument doc = QJsonDocument::fromJson(r.payload.toUtf8());
        if (doc.isNull() || !doc.isObject())
        {
            HTTP_LOG_WARN("失败波次重传 payload 解析失败 order=%s msgId=%s",
                orderCode.toLocal8Bit().data(), r.msgId.toLocal8Bit().data());
            continue;
        }
        emit outboxResendReady("end", doc.object(), r.msgId);
        ++sent;
    }

    HTTP_LOG_INFO("失败波次手动重传 order=%s 发送=%d", orderCode.toLocal8Bit().data(), sent);
    if (sent == 0)
        emit logMessage(QString("[重传] 波次%1 没有可重传的失败完结报文").arg(orderCode), true);
    else
        emit logMessage(QString("[重传] 任务完结(H8) 按波次补发 order=%1 报文数=%2（不影响主流程）")
            .arg(orderCode).arg(sent));
    return sent > 0;
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
    // ★ 2026-09-17 格口绑定摘要（切回前就能判断"这个波次有没有绑定记录"）：
    //   bindOwn  = 该波次自己绑过且箱号非空的格口数（= 切回时能恢复的格口数，主口径）
    //   bindConfirmed = 其中"该格全表最后一条仍属本波次"的格口数（供现场判断是否被更晚波次覆盖）
    //   bindActive    = DB 当前活跃绑定数（仅兜底显示用，非本波次记录）
    if (m_pSortingDb && m_pSortingDb->isOpen())
    {
        const QMap<QString, QString> own = m_pSortingDb->getBindsByOrder(orderCode);
        const QMap<QString, QString> cnf = m_pSortingDb->getBindsByOrderActive(orderCode);
        summary["bindOwn"]       = (int)own.size();
        summary["bindConfirmed"] = (int)cnf.size();
        int activeCnt = 0;
        for (const GridBoxBindRecord& b : m_pSortingDb->getAllActiveBinds())
            if (!b.gridNum.isEmpty() && !b.boxcode.isEmpty()) ++activeCnt;
        summary["bindActive"] = activeCnt;
    }
    else
    {
        summary["bindOwn"]       = 0;
        summary["bindConfirmed"] = 0;
        summary["bindActive"]    = 0;
    }
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

    // ══════════════════════════════════════════════════════════════════════════
    // ★ 2026-09-16 现场需求①：**不允许切回已完成/已取消的波次任务** —— 入口守卫
    //
    //   为什么必须放在函数最前（且在 switchAwayCurrentWave 之前）：
    //     本函数下面的"校验1"会先把当前波次切出（switchAwayCurrentWave），
    //     若等到"校验2"才发现目标波次是终态，当前任务就已经被切出去了 ——
    //     等于"拒绝切回"这个动作本身产生了副作用。因此终态判定必须在任何副作用之前完成：
    //     拒绝路径**只打日志**，不切出当前波次、不动绑定、不动任何内存结构。
    //
    //   判定来源（两处都查，避免"内存状态与 DB 状态不一致"时漏拦）：
    //     ① 内存当前波次就是目标且已终态（同一次运行内刚完结/取消）→ 直接拒绝；
    //     ② DB 波次状态 ∈ {WAVE_CANCELLED(6), WAVE_FINISHED(8)} → 直接拒绝。
    //   终态波次不再提供"载入查看"：报文补发请用「重传满箱切换(H7)/重传任务完结(H8)」。
    // ══════════════════════════════════════════════════════════════════════════
    {
        const int curStatus = m_pWaveMgr->status();
        const bool bMemTerminal =
            (!m_pWaveMgr->orderCode().isEmpty() && m_pWaveMgr->orderCode() == orderCode &&
             (curStatus == WAVE_FINISHED || curStatus == WAVE_CANCELLED));
        const int dbStatus = waveDbStatus(orderCode);
        const bool bDbTerminal = (dbStatus == WAVE_FINISHED || dbStatus == WAVE_CANCELLED);

        if (bMemTerminal || bDbTerminal)
        {
            const int shown = bDbTerminal ? dbStatus : curStatus;
            HTTP_LOG_WARN("切回拒绝 波次已终态 order=%s status=%d(%s) 来源=%s（按需求：不允许切回已完成/已取消波次）",
                orderCode.toLocal8Bit().data(), shown,
                WaveSnapshot::statusToString(shown).toLocal8Bit().data(),
                bDbTerminal ? "数据库" : "内存");
            emit logMessage(QString::fromUtf8(
                "[切回] 波次 %1（%2）已完成/已取消：按需求**不允许切回**，当前任务未受影响；"
                "如需补发报文请用「重传满箱切换(H7) / 重传任务完结(H8)」")
                .arg(orderCode).arg(WaveSnapshot::statusToString(shown)), true);
            return false;
        }
    }

    int curStatus = m_pWaveMgr->status();

    // ── 同波次快路径：选中的就是当前内存中的波次（同一次运行内挂起，未重启）──
    //   内存数据（GridBuffer/进度集合）齐全，仅需按映射调整状态（如 异常挂起→已绑定 人工闭环），无需重建
    {
        QString curOrder = m_pWaveMgr->orderCode();
        if (!curOrder.isEmpty() && curOrder == orderCode)
        {
            // ★ 2026-09-16 需求①：终态波次不允许切回 —— 本分支已由入口守卫拦掉，
            //   此处再做一次防御（若入口守卫被误删，这里必须仍然拒绝，而不是走旧的"面板即其数据"）
            if (curStatus == WAVE_FINISHED || curStatus == WAVE_CANCELLED)
            {
                HTTP_LOG_WARN("切回拒绝(同会话防御) 波次已终态 order=%s status=%d(%s)",
                    orderCode.toLocal8Bit().data(), curStatus,
                    WaveSnapshot::statusToString(curStatus).toLocal8Bit().data());
                emit logMessage(QString::fromUtf8(
                    "[切回] 波次 %1（%2）已完成/已取消：按需求不允许切回；如需补发 H7/H8 请用「重传」按钮")
                    .arg(orderCode).arg(WaveSnapshot::statusToString(curStatus)), true);
                return false;
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
    // ★ 2026-09-16 需求①：已完结/已取消（终态）**不再提供"载入查看"** —— 入口守卫已拦掉，
    //   这里保留防御式拒绝（若守卫被误删，仍必须拒绝，而不是把终态波次载入内存）
    case WAVE_FINISHED:
    case WAVE_CANCELLED:
        HTTP_LOG_WARN("切回拒绝(防御) 终态波次不允许切回 order=%s status=%d(%s)",
            orderCode.toLocal8Bit().data(), wave.status,
            WaveSnapshot::statusToString(wave.status).toLocal8Bit().data());
        emit logMessage(QString::fromUtf8(
            "[切回] 波次 %1（%2）已完成/已取消：按需求不允许切回；如需补发 H7/H8 请用「重传」按钮")
            .arg(orderCode).arg(WaveSnapshot::statusToString(wave.status)), true);
        return false;
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
    // ★ 2026-09-06 降级恢复：完结中(ENDING)/异常挂起(HELD)/取消处理中(CANCEL_PENDING)等波次的明细
    //   可能为空（下发时落库失败/历史数据缺失），此时仍应允许切换——按 DB 恢复进度与状态，
    //   GridBuffer 置空，仅支持「补发 H7/H8」，无法继续分拣（无格口映射）。否则仍拒绝。
    //   ★ 2026-09-16：终态（FINISHED/CANCELLED）已不允许切回，故不再列入本降级清单。
    bool allowEmptyItems = (wave.status == WAVE_ENDING || wave.status == WAVE_HELD ||
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
        // ★ 2026-09-14 同品多格口：恢复时同样保留「格口→计划件数」明细
        //   （DB return_wave_item 一行 = 一个 (SKU,格口) 对该格口的计划数），
        //   否则恢复后 planQtyPerGrid 为空，planAllocOf 会退化成"总数全给首个格口"，
        //   多格口 SKU 在恢复波次里又回到"全落第一个格口"的老问题。
        const QString gKey = normalizeGridKey(it.gridNum);
        if (newMap->contains(it.inco))
        {
            GridEntry& e = (*newMap)[it.inco];
            QStringList grids = e.gridNum.split(',', Qt::SkipEmptyParts);
            if (!grids.contains(it.gridNum))
                e.gridNum = e.gridNum.isEmpty() ? it.gridNum : e.gridNum + "," + it.gridNum;
            // ★ 同格口多行**累加**（与解析期 ParseWorker 完全同口径）：同一 (SKU,格口) 出现多行，
            //   语义是"这个产品在这个格口一共计划几件"。取大者会让计划数偏小，且与解析期
            //   不一致 → 同一波次"恢复前/恢复后"结果不同（不可追溯）。
            //   ★ 2026-09-26：累加改在**单元**粒度（(SKU,格口,类型)）—— return_wave_item 每行都带
            //     grid_type，故"同格口分类行 + 发货行"恢复后仍是两个各自保额的单元，不再被合并。
            e.planQtyPerGrid[gKey] += it.planQty;
            // ★ 每格口类型同样保存（同品可同时计划到"正常分拣(分类)"与"发货"格口）
            if (!it.gridType.isEmpty())
                e.gridTypePerGrid.insert(gKey, it.gridType);
            {
                const QString ck = makeCellKey(gKey, it.gridType);
                e.planQtyPerCell[ck] += it.planQty;
                e.gridTypePerCell.insert(ck, cellKeyTypeOf(ck));
            }
            int sum = 0;
            for (auto pit = e.planQtyPerCell.constBegin(); pit != e.planQtyPerCell.constEnd(); ++pit)
                sum += pit.value();
            e.gridCount = sum;
        }
        else
        {
            GridEntry entry;
            entry.gridNum   = it.gridNum;
            entry.gridType  = it.gridType.isEmpty() ? "0" : it.gridType;
            entry.gridCount = it.planQty;
            entry.volu      = it.volu;
            entry.obxCode   = it.obxCode;
            entry.planQtyPerGrid.insert(gKey, it.planQty);   // ★ 分格口计划
            entry.gridTypePerGrid.insert(gKey, entry.gridType);   // ★ 分格口类型
            entry.orderCode = orderCode;
            entry.orderQty  = wave.orderQty;
            entry.skuCount  = 0;
            newMap->insert(it.inco, entry);
        }
    }

    // ★ 恢复场景同样重置按格口落格计数：否则残留上一个波次的计数会让本波次分配被打偏
    clearGridLandedCount();
    {
        auto typeNameR = [](const QString& t) -> QString {
            if (t == "1") return QString::fromUtf8("异常");
            if (t == "2") return QString::fromUtf8("发货");
            return QString::fromUtf8("分类");
        };
        int multiSku = 0;
        QStringList detail;
        for (auto mit = newMap->constBegin(); mit != newMap->constEnd(); ++mit)
        {
            const GridEntry& e = mit.value();
            // ★ 2026-09-26：按**单元数**判定（同格口两类型 = 2 个单元也要列出）
            const int unitCnt = e.planQtyPerCell.isEmpty() ? e.planQtyPerGrid.size() : e.planQtyPerCell.size();
            if (unitCnt < 2) continue;
            ++multiSku;
            if (detail.size() >= 20) continue;
            QStringList one;
            if (!e.planQtyPerCell.isEmpty())
            {
                for (auto cit = e.planQtyPerCell.constBegin(); cit != e.planQtyPerCell.constEnd(); ++cit)
                    one << QString("%1(%2):%3件").arg(cellKeyGridOf(cit.key()))
                               .arg(typeNameR(cellKeyTypeOf(cit.key()))).arg(cit.value());
            }
            else
            {
                for (auto pit = e.planQtyPerGrid.constBegin(); pit != e.planQtyPerGrid.constEnd(); ++pit)
                    one << QString("%1(%2):%3件").arg(pit.key())
                               .arg(typeNameR(e.gridTypePerGrid.value(pit.key(), e.gridType)))
                               .arg(pit.value());
            }
            detail << QString("%1→[%2]").arg(mit.key()).arg(one.join("+"));
        }
        if (multiSku > 0)
            HTTP_LOG_INFO("恢复波次 同品多格口分配 order=%s 涉及SKU=%d 明细(格口(类型):件数): %s",
                orderCode.toLocal8Bit().data(), multiSku, detail.join("; ").toLocal8Bit().data());
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

    // ★ 2026-09-14 计划分配表：恢复后**重新编译**（计划来自 DB return_wave_item 的
    //   (SKU,格口,计划件数,类型) 行）。已落格计数不进表（DB 是唯一持久权威，落格明细在
    //   sorting_records）；恢复后的余量从"计划全额"起算，与既有口径一致（docs 用例 7 已声明）。
    //   ★ 2026-09-26 现场口径：**同一 (SKU,格口) 可以存在多种 grid_type** ⇒ 恢复也必须按
    //     **单元 (SKU,格口,类型)** 编译（planQtyPerCell，上面刚按行累加好），否则同格口的
    //     "分类 + 发货"会被合并成一个单元、属性额度互相顶账（原实现只读格口级 planQtyPerGrid）。
    //     编译口径与解析期（ParseWorker）统一走 PlanAllocTable::inputsFromCells。
    {
        QVector<QPair<QString, QVector<PlanGridInput>>> skuPlans;
        skuPlans.reserve(newMap->size());
        for (auto it = newMap->constBegin(); it != newMap->constEnd(); ++it)
        {
            const GridEntry& e = it.value();
            if (e.planQtyPerGrid.isEmpty() && e.planQtyPerCell.isEmpty()) continue;
            const QVector<PlanGridInput> gs =
                PlanAllocTable::inputsFromCells(e.planQtyPerCell, e.planQtyPerGrid,
                                                e.gridTypePerGrid, e.gridType);
            if (!gs.isEmpty()) skuPlans.append(qMakePair(it.key(), gs));
        }
        // 恢复场景先清旧表（可能残留上一个波次的计划），再按本波次明细编译
        clearPlanAllocTable(QString::fromUtf8("波次恢复-重建前"));
        if (!skuPlans.isEmpty())
        {
            buildPlanAllocTable(orderCode, wave.orderQty, 0, skuPlans);
            reportPlanAlloc(QString::fromUtf8("波次恢复"));
        }
    }

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

    // 步骤6：★ 2026-09-15 恢复该波次的格口绑定（客户需求：切回波次即带回该波次的绑定关系）
    //   ★★ 2026-09-17 口径修正（现场取证，docs/格口绑定波次归属_根因与修复_20260917.md）★★
    //   主口径 = **本波次自己的记录** `getBindsByOrder()`（每格取"本波次内"最后一条，boxcode 非空）。
    //   为什么不再用 `getBindsByOrderActive()`（每格"全表最后一条"且必须属本波次）当主口径：
    //     · 实测真实库 14 个历史波次里 13 个返回 0 行（该格口只要被更晚波次绑过就不再命中）；
    //     · 它一旦**部分命中**，旧实现就整体替换内存绑定（loadContainerBindings 是赋值不是合并）
    //       → 其余格口全部变成"未绑定"，现场看到"切回后面板被清空"。
    //     · 数学关系：每格"全表最后一条属本波次" ⇒ 该行必然是"本波次内最后一条"，
    //       即 getBindsByOrderActive ⊂ getBindsByOrder → 先取后者**绝不会漏格口**。
    //   兜底：本波次确实没有任何绑定记录 → ③ 用 DB 当前 active 绑定仅作**显示**（不写库），
    //         并明确告警"非本波次记录"，避免被误当成该波次的绑定；两者都空 → 保持未绑定 + 告警。
    //   ★ 全程**只恢复内存/界面显示，一律不写库**（铁律：历史不得粘贴进其它波次；
    //     绑定行只由当时的 H6 写入，归属补齐只由 H4 到达时的 attributePendingBindsToWave 完成）。
    //   · ★ 2026-09-16 需求①：终态（已完成/已取消）波次已不允许切回（入口守卫），
    //     因此本分支只会对"未结束波次"执行；isWaveTerminal 保留作防御性双保险。
    if (!isWaveTerminal(orderCode) && m_pSortingDb)
    {
        int physCount = 0;
        {
            std::lock_guard<std::mutex> lock(m_containerMutex);
            physCount = m_containerBindings.size();
        }

        // 主口径：本波次自己的绑定（每格本波次内最后一条）
        QMap<QString, QString> ownBinds = m_pSortingDb->getBindsByOrder(orderCode);
        // 确认口径：其中"该格全表最后一条仍属本波次"的格口数（只用于日志标签）
        const QMap<QString, QString> confirmed = m_pSortingDb->getBindsByOrderActive(orderCode);

        if (!ownBinds.isEmpty())
        {
            const int coveredByLater = ownBinds.size() - confirmed.size();
            const QString src = (coveredByLater > 0)
                ? QString::fromUtf8("本波次的绑定记录（其中 %1 个格口已被更晚波次重新绑定，按其本波次最后一条恢复）")
                      .arg(coveredByLater)
                : QString::fromUtf8("本波次的绑定记录");
            applyWaveBinds(orderCode, ownBinds, src);
            HTTP_LOG_INFO("切回波次恢复容器绑定 order=%s count=%d（来源=本波次的绑定记录；其中被更晚波次覆盖=%d 个；恢复前物理绑定=%d 个）",
                orderCode.toLocal8Bit().data(), (int)ownBinds.size(), coveredByLater, physCount);
        }
        else
        {
            // ③ DB 中当前 active 的绑定（物理当前绑定）——**只恢复显示、不写库**：
            //    写库会把"现场当前这几个箱"固化成该波次的绑定记录
            QMap<QString, QString> activeBinds;
            for (const GridBoxBindRecord& b : m_pSortingDb->getAllActiveBinds())
                if (!b.gridNum.isEmpty() && !b.boxcode.isEmpty())
                    activeBinds.insert(b.gridNum, b.boxcode);

            if (!activeBinds.isEmpty())
            {
                applyWaveBinds(orderCode, activeBinds,
                               QString::fromUtf8("DB 当前活跃绑定（物理当前绑定，非本波次记录）"));
                HTTP_LOG_WARN("切回波次恢复容器绑定 order=%s count=%d（来源=DB 当前活跃绑定：本波次无绑定记录，"
                              "按物理当前绑定显示以免无法分拣；仅显示不写库，WMS 下发新 H6 时自动覆盖）",
                    orderCode.toLocal8Bit().data(), (int)activeBinds.size());
                emit logMessage(QString::fromUtf8(
                    "[切换] 波次 %1 **没有任何绑定记录**（H6 归属落空/当时未绑定）——已按现场当前物理绑定显示 %2 个"
                    "（**不是本波次的记录**，仅显示不写库）；WMS 下发新 H6 时自动覆盖")
                    .arg(orderCode).arg(activeBinds.size()), true);
            }
            else
            {
                HTTP_LOG_WARN("切回波次无任何绑定可恢复 order=%s（DB 无绑定记录/无活跃绑定）——保持未绑定，等 WMS 下发 H6",
                    orderCode.toLocal8Bit().data());
                emit logMessage(QString::fromUtf8("[切换] 波次 %1 数据库无绑定记录（从未绑定过）——"
                                                  "保持未绑定，等待 WMS 下发容器绑定(H6)").arg(orderCode), true);
            }
        }
    }
    else
    {
        emit bindingUpdated();
    }

    // 步骤7：★ 2026-09-16 按 DB 重建"已落格进度"（断电/关闭重启后继续分拣的依据）
    //   为什么必须在步骤6之后：① "H7 明细只补仍属当前绑定容器的件"，需要已恢复的当前容器号；
    //   ② 额度/去重与该波次绑定无关，但同批次重建更易核对。
    //   现场含义：断电重启→切回后，该格口满箱时 H7 报文**包含重启前已落的件**，
    //             且继续投件不会突破"每格口计划件数"（已落件不再被当成 0）。
    restoreLandedProgress(orderCode);

    HTTP_LOG_INFO("恢复波次成功 order=%s target=%d(%s) qty=%d SKU=%d sorted=%d exc=%d fullbox=%d",
        orderCode.toLocal8Bit().data(), targetStatus,
        WaveSnapshot::statusToString(targetStatus).toLocal8Bit().data(),
        wave.orderQty, skuCount, (int)sortedEpcs.size(), (int)exceptionEpcs.size(), hasFullbox ? 1 : 0);
    // ★ 2026-09-16 需求①：终态不再可切回 → 这里只剩"进入上次任务"一种成功口径
    emit logMessage(QString("[恢复] 已进入上次任务 order=%1 状态=%2 已分拣=%3 异常=%4")
        .arg(orderCode).arg(WaveSnapshot::statusToString(targetStatus))
        .arg(sortedEpcs.size()).arg(exceptionEpcs.size()));

    // ★ 2026-09-06 切回波次自动补发 → ★ 2026-09-26 现场口径修订：**不再自动补发满箱回传（H7）**。
    //   现场口径：满箱回传只在四种情形发送（锁格 / 结束任务统一补发 / 「一键满箱回传」/
    //   「重传满箱切换(H7)」），切回波次属"自动发送"、不在其列，故此处只补发 H8（完结回传）；
    //   该波次未成功的 H7 仍完整保留在 outbox_fullbox，由操作员在「重传满箱切换(H7)」下拉里人工重传。
    //   ★ 2026-09-16：终态波次已不允许切回，故此处不再需要"终态不补发"的特例判断
    {
        QStringList h7PendingDesc;
        int h7Pending = 0;
        if (m_pSortingDb && m_pSortingDb->isOpen())
        {
            const QVector<OutboxRecord> fs = m_pSortingDb->getOutboxFullboxByOrder(orderCode);
            for (const OutboxRecord& r : fs)
            {
                if (r.status == "success") continue;
                ++h7Pending;
                if (h7PendingDesc.size() < 20)
                    h7PendingDesc << QString("%1(%2,%3)").arg(r.grid, r.boxcode, r.status);
            }
        }
        resendOutbox(orderCode, /*resendH7=*/false, /*resendH8=*/true);
        if (h7Pending > 0)
        {
            HTTP_LOG_WARN("[切回] 波次 %s 仍有未成功的满箱回传（H7）%d 条 —— 按现场口径**不自动补发**，"
                          "请用「重传满箱切换(H7)」人工重传：%s",
                orderCode.toLocal8Bit().data(), h7Pending, h7PendingDesc.join(" ").toLocal8Bit().data());
            emit logMessage(QString::fromUtf8(
                "[切回] 波次 %1 未成功的满箱回传（H7）%2 条：%3 —— 系统不自动补发，"
                "请在「重传满箱切换(H7)」下拉中选择该波次人工重传（报文与箱号都在）")
                .arg(orderCode).arg(h7Pending).arg(h7PendingDesc.join(QString::fromUtf8(" "))), true);
        }
        else
        {
            HTTP_LOG_INFO("[切回] 波次 %s 无未成功的满箱回传（H7）；仅补发完结回传（H8）", orderCode.toLocal8Bit().data());
        }
    }

    // ★ 2026-09-07 人工切换到待执行队列中的波次 → 从队列移除，防止自动重复执行
    for (int i = 0; i < m_pendingWaveQueue.size(); ++i)
    {
        if (m_pendingWaveQueue[i].orderCode == orderCode)
        {
            HTTP_LOG_INFO("人工切换已接管待执行波次 orderCode=%s（从队列移除）", orderCode.toLocal8Bit().data());
            m_pendingWaveQueue.removeAt(i);
            emit pendingWavesChanged();   // ★ 2026-09-08 UI 队列/波次列表刷新
            break;
        }
    }

    emit waveResumed(orderCode, targetStatus);
    return true;
}

// ============================================================================
// ★ 2026-09-13 计划数封顶 + 超计划件改投异常口
//   背景（现场 034 格口事件）：某 SKU 计划 2 件，人工多放 1 件，3 件被分落两个容器上报，
//   合计 3 件 > 计划 2 件 → WMS 回 [2107632]…无法分配 并**整条驳回**，连带同报文其它
//   正常件一起不落账。本机制按「格口+SKU」的计划件数封顶，超出者改投物理异常口。
//
//   额度消耗源 = **PLC 确认真正落入该格口的去重 EPC 数**（不是下发数、不是放置数）：
//     · 下发失败 / 落进异常口 → 不消耗额度 → "缺件由人工重投异常件补上"成立；
//     · 同一 EPC 只计 1 次 → 重复反馈不会把额度刷高。
//   口径 key = 格口号 + "\n" + SKU（★ 按格口而非容器：跨容器累计、换箱不重置额度）
// ============================================================================
void HttpServer::clearBoxLandedCount()
{
    std::lock_guard<std::mutex> lock(m_boxLandedMutex);
    m_boxLandedEpcs.clear();
    m_epcBoundException.clear();
}

// 判定：本件是否允许投"期望格口"（返回 false = 已达计划，应改投异常口）
//   counted = 该 EPC 此前已计入本格口本 SKU（= 已正确落入过的件被拿出重投）
bool HttpServer::allowIntoPlanGrid(const QString& gridKey, const QString& sku, const QString& epc,
                                   int planQty, bool& counted) const
{
    counted = false;
    const QString g = gridKey.trimmed();
    if (g.isEmpty() || sku.isEmpty() || epc.isEmpty())
        return true;                 // 信息不全 → 不在此处拦截，交原有校验链路处理

    std::lock_guard<std::mutex> lock(m_boxLandedMutex);
    const QString key = g + "\n" + sku;

    // ③ 已正确落入过该格口的件被拿出重投 → 允许回该格口，且不重复计数
    if (m_boxLandedEpcs.value(key).contains(epc))
    {
        counted = true;
        return true;
    }

    // 计划件数缺失/为 0 → 不做封顶（保持原行为，避免把无计划 SKU 全打进异常口）
    if (planQty <= 0)
        return true;

    // ② 未达计划 → 允许（含"异常件被重投回来补缺口"）
    // ① 已达计划 → 拒绝（改投异常口）
    return m_boxLandedEpcs.value(key).size() < planQty;
}

// 计数：落格反馈确认成功后调用；返回 true = 本次落格后该格口该 SKU 超出计划（登记预警）
bool HttpServer::noteLandedIntoPlanGrid(const QString& gridKey, const QString& sku, const QString& epc,
                                        int planQty, int& landedNow)
{
    landedNow = 0;
    const QString g = gridKey.trimmed();
    if (g.isEmpty() || sku.isEmpty() || epc.isEmpty())
        return false;

    std::lock_guard<std::mutex> lock(m_boxLandedMutex);
    const QString key = g + "\n" + sku;

    // ④ 同一 EPC 只计 1 次（重复反馈/迟到反馈不得把额度刷高）
    QSet<QString>& landed = m_boxLandedEpcs[key];
    if (!landed.contains(epc))
        landed.insert(epc);

    landedNow = landed.size();
    return planQty > 0 && landedNow > planQty;       // ⑤ 超出计划 → 登记预警
}

int HttpServer::landedCountOf(const QString& gridKey, const QString& sku) const
{
    std::lock_guard<std::mutex> lock(m_boxLandedMutex);
    return m_boxLandedEpcs.value(gridKey.trimmed() + "\n" + sku).size();
}

// ============================================================================
// ★ 2026-09-14 同品多格口「按计划件数分配」：选格依据 + 落格计数
//   现场背景：H4 允许同一 SKU 按多格口分别计划
//     （如 106101134113101 → 22034 计划 1 件 + 22048 计划 3 件），
//   但旧选格逻辑恒取「首个格口」，导致 4 件全落在 034、计划中的 048 落空，
//   满箱上报的格口分布与计划不符（WMS 虽按 SKU 总量校验通过，但账实分布已错）。
//   现口径（客户确认）：
//     · 前 N 件按计划分流到各格口（N 取该格口计划件数）；
//     · 各计划格口满额后的多余件 → 一律投异常口，并在日志标注（不再占用计划格口）。
//   计数口径（客户确认）：以 PLC 反馈「落格成功」为准 —— 故在落格反馈处登记，
//   同一 EPC 只计一次；跨换箱持续累计（H6 重绑不清零，仅 H4 新波次/波次清理时清零）。
// ============================================================================
PlcPlanAllocInfo HttpServer::planAllocOf(const QString& epc, const QString& skuIn, bool bClaim)
{
    PlcPlanAllocInfo info;
    info.orderCode = m_allocOrderCode;

    // 1) 超计划兜底格口（配置项 exceptionGrid；未配置 = -1 表示不做异常口改投）
    {
        const QString excCfg = ConfigManager::instance()->config().exceptionGrid.trimmed();
        bool okExc = false;
        const int exc = excCfg.toInt(&okExc);
        info.excGrid = (okExc && exc > 0 && exc <= BINDING_SLOT_COUNT) ? exc : -1;
    }

    // SKU 解析：调用方可能只给 EPC（识别码即 EPC），此处按 EPC 从 EpcCache 取 SKU
    //   （EpcCache 自带互斥量，可跨线程调用；与落格链路 getSkuByEpc 同一口径）
    QString sku = skuIn;
    if (sku.isEmpty() || sku == epc)
        sku = skuOfEpcForAlloc(epc);

    if (sku.isEmpty()) return info;

    // ════════════════════════════════════════════════════════════════════════
    // ★ 2026-09-21 硬上限：**只读预查也总是返回** H4 计划 / 每格口属性 / 已落 / 在途。
    //
    //   为什么必须在"分配表未生效"时也返回：发送侧的上限判据（PlcManager 候选过滤）要用它，
    //   而"分配表一失效就没上限"正是现场"某格口超计划那么多"的结构性漏洞之一
    //   （另一处是把 entry.gridCount = SKU 计划总数 当成本格口上限）。
    //
    //   口径（客户确认 2026-09-21）：
    //     · 额度以 **(SKU, 格口, 分拣属性)** 为单位，**严格不大于 H4 该格口计划件数**；
    //     · 分类(0)与发货(2)各自独立封顶，互不借用（不许用"SKU 总数"当某一格口/属性的上限）；
    //     · 已落计入 = PLC 确认落入该格口的去重 EPC 数；在途 = 已下发未反馈的件数。
    // ════════════════════════════════════════════════════════════════════════
    if (m_pBuffer)
    {
        const GridEntry ent = m_pBuffer->get(sku);
        if (!ent.planQtyPerCell.isEmpty())
        {
            // ★ 2026-09-26：**单元口径**（(SKU,格口,类型)）—— 逐单元给出计划；同格口两类型各自一份。
            for (auto cit = ent.planQtyPerCell.constBegin(); cit != ent.planQtyPerCell.constEnd(); ++cit)
            {
                const QString gk   = normalizeGridKey(cellKeyGridOf(cit.key()));
                const QString type = cellKeyTypeOf(cit.key());
                if (gk.isEmpty()) continue;
                info.planQtyPerCell.insert(makeCellKey(gk, type), cit.value());
                info.gridTypePerCell.insert(makeCellKey(gk, type), type);
            }
            // 格口级视图（各类型求和）：发送侧的上限判据/日志/报表仍按格口读这张表
            for (auto pit = ent.planQtyPerGrid.constBegin(); pit != ent.planQtyPerGrid.constEnd(); ++pit)
            {
                const QString gk = normalizeGridKey(pit.key());
                if (gk.isEmpty()) continue;
                info.planQtyPerGrid.insert(gk, pit.value());
                info.gridTypePerGrid.insert(gk, ent.gridTypePerGrid.value(gk, ent.gridType.isEmpty() ? "0" : ent.gridType));
            }
        }
        else
        {
            for (auto pit = ent.planQtyPerGrid.constBegin(); pit != ent.planQtyPerGrid.constEnd(); ++pit)
            {
                const QString gk = normalizeGridKey(pit.key());
                if (gk.isEmpty()) continue;
                info.planQtyPerGrid.insert(gk, pit.value());                       // ★ H4 计划（权威上限）
                info.gridTypePerGrid.insert(gk, ent.gridTypePerGrid.value(gk, ent.gridType.isEmpty() ? "0" : ent.gridType));
            }
        }
    }

    if (!m_allocValid.load() || !ConfigManager::instance()->config().allocEnabled)
    {
        // 分配表未启用/未编译 → 用 H4 计划 + 落格计数（m_boxLandedEpcs）+ 按格口在途 组合同样的上限判据
        for (auto pit = info.planQtyPerGrid.constBegin(); pit != info.planQtyPerGrid.constEnd(); ++pit)
        {
            const QString gk = pit.key();
            info.landedNum.insert(gk, landedCountOf(gk, sku));
            info.reservNum.insert(gk, inFlightCountOfGrid(sku, gk));
        }
        return info;   // valid=false：调用方按既有回退逻辑选格，但**仍受上面的上限约束**
    }

    // ── ★ 2026-09-20 现场问题④：本次"不可分配"格口掩码（已解锁且未绑定容器）──
    //   ★★ 必须在 m_allocMutex **之外**构建（allocBlockedMask 内部要取 m_containerMutex 与
    //      S7 锁格状态锁）—— 否则形成 alloc→container 锁嵌套，加锁顺序与其它路径不一致（死锁风险）。
    //   ★ 只有 bClaim=true 才需要（只读预查不认领，无需掩码）；开关关闭时掩码全 false。
    //   ★ 掩码只影响"认领哪一个格口"，**不动任何额度**：被跳过的格口额度原样保留，
    //     WMS 重发 H6 绑定容器后自动恢复分配（这就是现场要的"不消耗额度"）。
    PlanAllocTable::GridMask blocked{};
    if (bClaim) blocked = allocBlockedMask();

    // 2) 分配表：计划/类型/已落/在途 + ★ 在同一次加锁内完成额度认领
    //    ★ 必须"判定 + 改数"在同一把锁内：这是修复"同时两件在线 → 两件都判未满额
    //      → 都发同一格口 → 箱内实落超计划"的关键（改造前是两次独立读改）。
    {
        std::lock_guard<std::mutex> lock(m_allocMutex);
        if (!m_alloc.skuHasPlan(sku))
            return info;   // 该 SKU 本波次无计划 → 退回老逻辑

        const QVector<qint16> grids = m_alloc.gridsOf(sku);
        for (qint16 g : grids)
        {
            const QString gk = PlanAllocTable::gridKeyOf(g);
            // ★ 2026-09-21 上限一律取 **H4 计划快照**（planQtyH4）：
            //   即使历史上有人搬动过额度（planQty 被抬高），发送侧上限仍不会被放大；
            //   二者不等时巡检⑤会报违规并写异常表。
            info.planQtyPerGrid.insert(gk, m_alloc.planQtyH4Of(sku, gk));
            info.landedNum.insert(gk, m_alloc.landedOf(sku, gk));
            info.reservNum.insert(gk, m_alloc.reservOf(sku, gk));
            // 每格口类型按格口取（不会像合并后的 gridType 那样只剩最后一行）
            info.gridTypePerGrid.insert(gk, QString::number(m_alloc.gridTypeOf(sku, gk)));
        }
        info.valid = !info.planQtyPerGrid.isEmpty();
        info.skuIdx = m_alloc.skuIndexOf(sku);
        if (!info.valid)
            return info;

        // bClaim=false：只读预查（供选格先判断是否需要缺口搬迁），**不扣额度、不登记认领**
        if (!bClaim)
            return info;

        // ── 认领额度 ──
        //   ① 该件此前已正确落入过原格口（件被拿出重投）→ 放行且不占用新额度，
        //      保持"重扫重投仍回原格口"的既有口径（见 docs 用例 9/10）。
        //      ★ 但该格口若已解锁且未绑定容器 → 不再回它（件会落进无容器格口），
        //        继续走 ② 认领别的格口 / 改投异常口。
        for (qint16 g : grids)
        {
            const QString gk = PlanAllocTable::gridKeyOf(g);
            if (g >= 0 && g < (int)blocked.size() && blocked[(size_t)g]) continue;   // ★ 不可分配 → 跳过
            if (m_alloc.epcLandedIn(sku, gk, epc))
            {
                info.claimOk      = true;
                info.claimGrid    = g;
                info.claimPlanIdx = -1;          // 未新增认领 → 无需释放/提交
                info.claimId      = 0;
                info.allEpcsLanded = true;
                return info;
            }
        }

        //   ② 正常认领：按计划单元顺序取首个仍有额度的单元（额度 = 计划−已落−在途）
        //      ★ 掩码内（锁格 / 已解锁且未绑定容器 / 禁用）的格口直接跳过，额度保留
        qint16 claimGrid = -1, planIdx = -1;
        quint64 claimId = 0;
        quint8  claimType = 0;   // ★ 2026-09-26：认领单元的（格口,类型）
        if (m_alloc.claim(sku, &claimGrid, &claimId, &planIdx, &blocked, &claimType))
        {
            info.claimOk      = true;
            info.claimGrid    = claimGrid;
            info.claimType    = claimType;
            info.claimPlanIdx = planIdx;
            info.claimId      = claimId;
            // ③ 认领登记（同一次锁内）：落格提交/发送失败释放都按 EPC 反查
            bool conflict = false;
            m_alloc.noteIssued(epc, info.skuIdx, planIdx, claimId,
                               QDateTime::currentMSecsSinceEpoch(), &conflict);
            if (conflict)
            {
                // 同一 EPC 已有在途认领（重复下发）→ 保留原认领，本次额度立即释放，防泄漏
                m_alloc.releaseEpc(epc);
                if (!m_alloc.noteIssued(epc, info.skuIdx, planIdx, claimId,
                                        QDateTime::currentMSecsSinceEpoch(), nullptr))
                {
                    HTTP_LOG_WARN("[分配表] 认领登记失败（已在途）epc=%s sku=%s grid=%s —— 立即释放本次额度",
                        epc.toLocal8Bit().data(), sku.toLocal8Bit().data(),
                        PlanAllocTable::gridKeyOf(claimGrid).toLocal8Bit().data());
                    m_alloc.releaseByClaimId(claimId);
                    info.claimOk = false;
                }
            }
            // ④ 认领号登记（供落格反馈提交用；与在途认领同生命周期）
            if (info.claimOk)
            {
                std::lock_guard<std::mutex> lk(m_allocClaimIdMutex);
                m_allocClaimIds.insert(epc, claimId);
            }
        }
    }

    return info;
}

// ★ 取「该 SKU 在某格口的类型数字」（0=正常分拣 1=异常 2=发货）；不在计划内返回 0
int HttpServer::gridTypeOfGridInPlan(const QString& sku, const QString& gridKey) const
{
    std::lock_guard<std::mutex> lock(m_allocMutex);
    return m_alloc.gridTypeOf(sku, gridKey);
}

void HttpServer::noteGridLanded(const QString& sku, const QString& gridKey, const QString& epc)
{
    const QString g = gridKey.trimmed();
    if (sku.isEmpty() || g.isEmpty() || epc.isEmpty())
        return;

    std::lock_guard<std::mutex> lock(m_gridLandedMutex);
    m_gridLandedNum[sku][g].insert(epc);   // 去重：同一 EPC 重复反馈不重复计数
}

void HttpServer::clearGridLandedCount()
{
    std::lock_guard<std::mutex> lock(m_gridLandedMutex);
    m_gridLandedNum.clear();
}

// ============================================================================
// ★ 2026-09-14 落格明细去重：同一 EPC「同波次同格口」只记 1 条落格明细
//   现场根因（09-13 波次 PP202600000580）：同一件货被重扫重投后又落回原格口，
//   在同一次换箱前后各写了一条落格明细（同一物理件 = 2 条）；H7 按容器聚合上报时
//   同一件在"旧箱"与"新箱"各计 1 件 → WMS 侧数量对不上并整条驳回
//   （[2107632]转移库存产品编码[115101001502703],库位[H-T0131],数量[1]无法分配）。
//   口径（客户确认）：同一 EPC 在同一波次内落到同一格口只保留一条落格明细；
//   若本次落格晚于已记录的那条（件被拿出后又被系统送回该格口），
//   仅打日志、不重复记账（箱内该件仍只有 1 件，账实保持 1:1）。
//   不变的部分：已分拣计数仍按 PLC 实测逐次累加（原有口径），本机制只约束"落格明细"。
//   线程安全：反馈处理运行在 PLC 接收线程池内，故用独立互斥量保护。
// ============================================================================
bool HttpServer::isLandingDetailRecorded(const QString& gridKey, const QString& epc) const
{
    if (gridKey.isEmpty() || epc.isEmpty())
        return false;
    std::lock_guard<std::mutex> lock(m_landingDetailMutex);
    return m_landingDetailKeys.contains(gridKey + "\n" + epc);
}

void HttpServer::markLandingDetailRecorded(const QString& gridKey, const QString& epc)
{
    if (gridKey.isEmpty() || epc.isEmpty())
        return;
    std::lock_guard<std::mutex> lock(m_landingDetailMutex);
    m_landingDetailKeys.insert(gridKey + "\n" + epc,
        QDateTime::currentMSecsSinceEpoch());
    m_lastDetailGridByEpc[epc] = gridKey;
}

void HttpServer::noteBoxLandedEpcs(const QString& sku, const QString& gridKey, const QString& epc)
{
    // 与"每格口已落格计数"同源：同一 EPC 只计一次，供计划额度判定（allowIntoPlanGrid）使用
    noteGridLanded(sku, gridKey, epc);
}

bool HttpServer::hasLandingDetail(const QString& epc) const
{
    if (epc.isEmpty())
        return false;
    std::lock_guard<std::mutex> lock(m_landingDetailMutex);
    return m_lastDetailGridByEpc.contains(epc);
}

QString HttpServer::lastDetailGridOf(const QString& epc) const
{
    if (epc.isEmpty())
        return QString();
    std::lock_guard<std::mutex> lock(m_landingDetailMutex);
    return m_lastDetailGridByEpc.value(epc);
}

// ★ 2026-09-25 去重实物件数（现场反馈："历史记录里的已分拣总比波次面板少几件"）
//   根因：面板「分拣件数」原先取 WaveManager::sorted()（PLC 反馈**件次**累计），
//   而「波次数据历史记录」页的「已分拣」列取 sorting_records 的 COUNT(DISTINCT barcode)
//   —— 两者口径不同：件次含重复反馈/重投，且含"计了件但没写明细"的四类件
//   （no_bind / no_match / 落错格 / 超计划超出件），故面板 ≥ 历史列，差额就是这几件。
//   本函数给出与历史列**同源**的实时值：m_lastDetailGridByEpc 的 key 集合正是
//   "写进过落格明细的 EPC"，与 sorting_records 去重计数一一对应（多格口同件仍算 1 件）。
//   注：本值只读内存，不加 DB 查询 —— 面板每秒渲染一次，不能引入同步查询（见 updateWavePanel 说明）。
int HttpServer::sortedDetailCount() const
{
    std::lock_guard<std::mutex> lock(m_landingDetailMutex);
    return m_lastDetailGridByEpc.size();
}

void HttpServer::clearLandingDedup()
{
    std::lock_guard<std::mutex> lock(m_landingDetailMutex);
    const int n = m_landingDetailKeys.size();
    m_landingDetailKeys.clear();
    m_lastDetailGridByEpc.clear();
    if (n > 0)
        HTTP_LOG_INFO("落格明细去重集合已清空 keys=%d（波次切换/新波次/完结清理）", n);
}

// ════════════════════════════════════════════════════════════════════════════
// ★ 2026-09-14 计划分配表（落格结构优化）——编译 / 认领 / 落格 / 释放 / 巡检 / 报告
//
// 客户口径（本块代码要落地的不变量）：
//   ① 每个格口都有"对应这个产品的数量"——一个 SKU 可同时计划到「正常分拣格口」
//      与「发货格口」，各格口各一份数量；落格按各格口数量分（不再全投第一个）。
//   ② 人工失误 / 同时两件在线造成"箱内实落 > 计划"必须被拦住：
//      已落格 + 在途认领 ≥ 计划件数 → 后续件一律改投异常口（66 号）。
//
// 为什么能拦住（改造前的根因）：改造前选格时"查已落数"与"登记已落数"是两次独立
//   读改，同时两件在线时两件都读到"已落 0 < 计划 1"→ 两件都发同一格口。
//   本表把**在途认领**也算占用额度，且"判定 + 扣减"在同一把锁内完成 → 第二件在下发
//   时刻就看到额度为 0，直接改投异常口。
// ════════════════════════════════════════════════════════════════════════════

// ──── 编译（波次解析完成时调用一次；仅主线程）────
bool HttpServer::buildPlanAllocTable(const QString& orderCode, int orderQty, int planSum,
                                     const QVector<QPair<QString, QVector<PlanGridInput>>>& skuPlans)
{
    AppConfig& cfg = ConfigManager::instance()->config();

    // ★ 2026-09-14 应急处置开关：环境变量 WCS_ALLOC_OFF=1 时强制关闭本特性
    //   （优先级高于 XML 配置；用于现场/联调时"不改配置文件即可让行为回到改造前"）
    const bool bAllocOn = cfg.allocEnabled && !qEnvironmentVariableIsSet("WCS_ALLOC_OFF");

    // 开关关闭 → 明确失效，全系统退回改造前行为（可配置回退，无需回滚可执行文件）
    if (!bAllocOn)
    {
        m_allocValid.store(false);
        std::lock_guard<std::mutex> lock(m_allocMutex);
        m_alloc.clear();
        m_allocPlanLogCnt = 0;
        HTTP_LOG_WARN("[计划分配表] 已关闭(allocEnabled=false) order=%s —— 选格退回改造前'恒取首个格口'行为",
            orderCode.toLocal8Bit().data());
        return false;
    }

    QElapsedTimer timer;
    timer.start();

    std::lock_guard<std::mutex> lock(m_allocMutex);
    m_alloc.clear();
    m_allocPlanLogCnt = 0;
    // ★ 2026-09-26 单元归属诊断计数：新波次从零开始
    m_allocLateStubCnt.store(0);
    m_allocUnitGuessCnt.store(0);
    m_allocReleasedOnMissedCnt.store(0);

    // 异常口（66）不属于任何 SKU 的产品计划：若 H4 把它写进计划，编译期剔除，
    // 否则"计划件数"会把本该改投异常口的件算成"计划内"，多余件就漏判了。
    const QString excCfg = cfg.exceptionGrid.trimmed();
    bool okExc = false;
    const int excGrid = excCfg.toInt(&okExc);
    const bool hasExc = (okExc && excGrid > 0 && excGrid <= BINDING_SLOT_COUNT);

    QVector<QPair<QString, QVector<PlanGridInput>>> cleaned;
    cleaned.reserve(skuPlans.size());
    int droppedExc = 0;
    for (const auto& kv : skuPlans)
    {
        QVector<PlanGridInput> gs;
        gs.reserve(kv.second.size());
        for (const PlanGridInput& g : kv.second)
        {
            if (hasExc && g.grid == (qint16)excGrid) { ++droppedExc; continue; }
            gs.append(g);
        }
        if (!gs.isEmpty()) cleaned.append(qMakePair(kv.first, gs));
    }
    if (droppedExc > 0)
    {
        HTTP_LOG_WARN("[计划分配] H4 计划中含异常格口%s 的条目共%d 行 —— 已从计划中剔除，该格口只收超计划件",
            PlanAllocTable::gridKeyOf((qint16)excGrid).toLocal8Bit().data(), droppedExc);
    }

    const bool ok = m_alloc.build(BINDING_SLOT_COUNT, cleaned);
    if (!ok)
    {
        m_allocValid.store(false);
        HTTP_LOG_ERROR("[计划分配表] 编译结果为空 order=%s SKU条目=%d —— 分配表失效，选格退回老逻辑（不影响投线）",
            orderCode.toLocal8Bit().data(), skuPlans.size());
        return false;
    }

    m_allocOrderCode = orderCode;
    m_allocValid.store(true);

    const PlanStats st = m_alloc.stats();
    const qint64 ms = timer.elapsed();
    HTTP_LOG_INFO("[计划分配表] 构建 order=%s SKU=%d 计划单元=%d 多格口SKU=%d 计划件数合计=%d（H4 orderQty=%d %s）耗时=%lldms 内存≈%dKB",
        orderCode.toLocal8Bit().data(), st.skuCount, st.cellCount, st.multiSkuCnt,
        st.planTotal, orderQty,
        (st.planTotal == orderQty ? "一致" : (st.planTotal == planSum ? "与Σitems一致但不等于orderQty" : "不一致")),
        ms, (int)((st.cellCount * 24 + st.skuCount * 80) / 1024));

    if (st.planTotal != orderQty)
    {
        HTTP_LOG_WARN("[计划分配表] 计划件数校验不一致 order=%s Σ每格口计划=%d orderQty=%d 差=%d "
                      "（按 H4 明细为准分配；请核对 WMS 计划）",
            orderCode.toLocal8Bit().data(), st.planTotal, orderQty, st.planTotal - orderQty);
    }

    // 多格口明细入日志（现场核对"哪个产品分到哪些格口、各几件"的底稿）
    {
        QStringList detail;
        const int n = qMin(50, st.multiSkuCnt > 0 ? st.multiSkuCnt : 0);
        int shown = 0;
        QVector<PlanAllocTable::UiRow> rows;
        m_alloc.snapshot(&rows);
        for (const auto& r : rows)
        {
            if (r.cells.size() < 2) continue;
            if (shown >= n) break;
            QStringList one;
            for (const auto& c : r.cells)
                one << QString("%1(%2):%3件").arg(PlanAllocTable::gridKeyOf(c.grid))
                           .arg(PlanAllocTable::typeNameOf(c.gridType)).arg(c.planQty);
            detail << QString("%1→[%2]").arg(r.sku).arg(one.join("+"));
            ++shown;
        }
        if (!detail.isEmpty())
        {
            HTTP_LOG_INFO("[计划分配表] 多格口分配 order=%s 涉及SKU=%d 明细(格口(类型):件数): %s%s",
                orderCode.toLocal8Bit().data(), st.multiSkuCnt,
                detail.join("; ").toLocal8Bit().data(),
                st.multiSkuCnt > shown ? " …（其余见「计划分配表」页）" : "");
        }
    }
    return true;
}

// ──── 认领（主线程）────
//   ★ 2026-09-20 现场问题④：本包装**没有调用点**（历史遗留），保留仅为兼容；
//     真正的认领入口是 planAllocOf()（它会在分配表锁之外构建"不可分配格口掩码"并传入，
//     保证已解锁且未绑定容器的格口永不被认领、额度原样保留）。
//     ★ 后续若有人启用本函数：必须同样先算掩码再传入，否则会绕过现场问题④的整改口径。
bool HttpServer::claimAlloc(const QString& sku, qint16* claimGrid, quint64* claimId, qint16* planIdx)
{
    std::lock_guard<std::mutex> lock(m_allocMutex);
    return m_alloc.claim(sku, claimGrid, claimId, planIdx);
}

void HttpServer::noteAllocIssued(const QString& epc, int skuIdx, qint16 planIdx, quint64 claimId)
{
    std::lock_guard<std::mutex> lock(m_allocMutex);
    m_alloc.noteIssued(epc, skuIdx, planIdx, claimId, QDateTime::currentMSecsSinceEpoch(), nullptr);
}

// ★ 2026-09-26（现场口径：额度消耗与归还均以 **PLC 真实落格反馈**为准）
//   件没进计划格口 → 认领额度当场归还，不等 30s 超时清扫。三条来源（反馈线程收集 → 本槽主线程执行）：
//     · 落异常口 66（PLC 把在途件强制送 66 / 软件改投后 PLC 反馈落格）
//     · PLC 报 status=2（无格口）/ status=3（信息不全）
//     · 落错格（落到该 SKU 无计划的格口）
//   ★ 守卫 isEpcInFlight：该 EPC 此刻若正被**重新下发**（新认领在途），绝不能释放新认领
//     （否则会把重投件的额度还掉 → 同一单元被超发）。epcsLanded 先清在途标志，故本槽在其后执行。
//   ★ stashAttribution=false：这些件"确定没进计划格口"，留存根只会让后续查无实据的反馈记错属性。
void HttpServer::releaseAllocClaimsOnMissedLanding(const QStringList& epcs)
{
    if (epcs.isEmpty()) return;

    int released = 0, skippedInflight = 0;
    for (const QString& epc : epcs)
    {
        if (epc.isEmpty()) continue;
        if (isEpcInFlight(epc)) { ++skippedInflight; continue; }   // 已被重新下发 → 保留新认领

        bool ok = false;
        {
            std::lock_guard<std::mutex> lock(m_allocMutex);
            ok = m_alloc.releaseEpc(epc, QDateTime::currentMSecsSinceEpoch(), /*stashAttribution=*/false);
        }
        if (ok) ++released;
        {
            std::lock_guard<std::mutex> lk(m_allocClaimIdMutex);
            m_allocClaimIds.remove(epc);
        }
    }

    if (released > 0 || skippedInflight > 0)
    {
        m_allocReleasedOnMissedCnt.fetch_add(released);
        HTTP_LOG_WARN("[分配表] 件未进计划格口（异常口/PLC无格口·信息不全/落错格）→ 按 PLC 反馈**即时归还**额度 "
                      "%d 条（跳过仍在途=%d；旧逻辑要等认领超时清扫，最多 %dms）—— 该件不计分拣、不消耗计划额度",
            released, skippedInflight, ConfigManager::instance()->config().allocClaimTimeoutMs);
    }
}

bool HttpServer::epcLandedInPlan(const QString& sku, const QString& gridKey, const QString& epc) const
{
    std::lock_guard<std::mutex> lock(m_allocMutex);
    return m_alloc.epcLandedIn(sku, gridKey, epc);
}

// ──── 落格登记（唯一跨线程入口：PLC 反馈线程池）────
bool HttpServer::commitLandedAlloc(const QString& sku, const QString& gridKey, const QString& epc,
                                   quint64 claimId, bool* mismatchOut, int* landedNowOut, int* planQtyOut,
                                   bool* lateStubUsedOut, bool* unitGuessedOut,
                                   quint8 gridTypeHint, quint8* landedTypeOut)
{
    // 认领号反查（反馈线程读；主线程认领时写入）→ 独立小锁，避免与分配表锁交叉
    quint64 cid = claimId;
    if (cid == 0 && !epc.isEmpty())
    {
        std::lock_guard<std::mutex> lk(m_allocClaimIdMutex);
        cid = m_allocClaimIds.value(epc, 0);
    }

    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(m_allocMutex);
        ok = m_alloc.commitOnLanded(sku, gridKey, epc, cid, mismatchOut, landedNowOut, planQtyOut,
                                    lateStubUsedOut, unitGuessedOut, gridTypeHint, landedTypeOut);
    }
    // 认领已消费（无论成功与否）→ 清登记，避免无界增长（在途认领本身已在表内释放）
    if (!epc.isEmpty())
    {
        std::lock_guard<std::mutex> lk(m_allocClaimIdMutex);
        m_allocClaimIds.remove(epc);
    }
    return ok;
}

// ──── 发送失败待释放队列（主线程处理；sendPool 线程只入队）────
void HttpServer::drainAllocPendingRelease()
{
    QSet<QString> pending;
    {
        std::lock_guard<std::mutex> lk(m_allocPendingReleaseMutex);
        if (m_allocPendingRelease.isEmpty()) return;
        pending = m_allocPendingRelease;
        m_allocPendingRelease.clear();
    }
    int released = 0;
    {
        std::lock_guard<std::mutex> lock(m_allocMutex);
        for (const QString& epc : pending)
        {
            if (m_alloc.releaseEpc(epc)) ++released;
        }
    }
    {
        std::lock_guard<std::mutex> lk(m_allocClaimIdMutex);
        for (const QString& epc : pending)
            m_allocClaimIds.remove(epc);
    }
    if (released > 0)
    {
        HTTP_LOG_WARN("[分配表] 下发失败已释放认领 %d 条（额度归还，件可按原计划重投）pending=%d",
            released, pending.size());
    }
}

// ──── 缺口搬迁（**已于 2026-09-26 整体删除**）────
//   现场口径：不允许因锁格或其它原因搬迁计划额度；计划是"（SKU,格口,分拣类型）多少件就落多少件、不能多"。
//   删除内容：HttpServer::moveAllocGap()、构造函数里的 setMoveGapCallback 接线、
//   PlcManager 的搬迁块与 PlcMoveGapCallback、PlanAllocTable::moveGap()。
//   现场实例（SKU 105301083212803，H4：034=2件 + 040=176件）：两个箱子轮流满箱 → 搬迁把额度
//   在两格口之间来回搬（净搬入 034 81 件）→ 查询页出现"计划2 / 分拣记录83"，且是跨类型搬迁。
//   现行口径：不可用单元的额度留在原单元；件改用其它单元自身剩余额度，都没有则改投异常口 66 或不发指令。
//   防回归：PlanAllocTable::audit() ⑤⑥⑦ + 每 30s 巡检（本文件 sweepPlanAllocClaims → audit）
//   一旦发现 planQty 偏离 planQtyH4（有人重新引入额度搬动）即报违规并写异常表。

// ──── 认领超时清扫 + 在途上限保护（每 30s，主线程）────
void HttpServer::sweepPlanAllocClaims()
{
    AppConfig& cfg = ConfigManager::instance()->config();
    if (!m_allocValid.load()) return;

    QElapsedTimer timer;
    timer.start();

    int releasedCnt = 0;
    int prunedStubCnt = 0;   // ★ 2026-09-26 归属存根剪枝条数
    int stubCnt = 0;         // ★ 2026-09-26 剪枝后剩余存根条数
    {
        std::lock_guard<std::mutex> lock(m_allocMutex);

        // ① 在途上限保护：超过上限说明有认领泄漏（下发后长期无落格反馈），强制清扫
        const int inflight = m_alloc.inflightCount();
        if (cfg.allocMaxInflight > 0 && inflight > cfg.allocMaxInflight)
        {
            HTTP_LOG_ERROR("[分配表] 在途认领超上限 inflight=%d > %d —— 强制清扫防止额度泄漏（请查现场落格反馈是否正常）",
                inflight, cfg.allocMaxInflight);
            const QStringList forced = m_alloc.sweepExpiredClaims(
                QDateTime::currentMSecsSinceEpoch(), 1000);   // 1s 即视为超时，全部释放
            releasedCnt += forced.size();
        }

        // ② 正常超时清扫（下发后 30s 无落格反馈 → 释放额度，让其它件可用）
        const QStringList released = m_alloc.sweepExpiredClaims(
            QDateTime::currentMSecsSinceEpoch(), cfg.allocClaimTimeoutMs);
        for (const QString& epc : released)
        {
            HTTP_LOG_WARN("[分配表] 认领超时释放 epc=%s（下发后 %dms 无落格反馈 → 额度归还，件仍可按原计划重投）",
                epc.toLocal8Bit().data(), cfg.allocClaimTimeoutMs);
        }
        releasedCnt += released.size();

        // ③ ★ 2026-09-26 归属存根剪枝：认领被释放后保留"该件原本认领哪个单元"的存根，
        //   供迟到反馈记回原单元（同一 (SKU,格口) 多类型时防止串属性）。
        //   超过保留期（kOrphanKeepMs，10 分钟）即清 —— 更晚到的反馈只能走启发式并留痕。
        prunedStubCnt = m_alloc.pruneOrphanClaims(QDateTime::currentMSecsSinceEpoch(),
                                                  PlanAllocTable::kOrphanKeepMs);
        stubCnt = m_alloc.orphanCount();
    }

    if (releasedCnt > 0)
        HTTP_LOG_INFO("[分配表] 认领清扫完成 released=%d 耗时=%lldms", releasedCnt, timer.elapsed());
    if (prunedStubCnt > 0)
        HTTP_LOG_INFO("[分配表] 归属存根剪枝 清理=%d（超 %lldms；剩余存根=%d）",
            prunedStubCnt, (long long)PlanAllocTable::kOrphanKeepMs, stubCnt);

    auditPlanAlloc();
}

// ──── 不变量巡检（每 30s）────
void HttpServer::auditPlanAlloc()
{
    if (!m_allocValid.load()) return;

    QStringList bad;
    {
        std::lock_guard<std::mutex> lock(m_allocMutex);
        bad = m_alloc.audit();
    }
    m_allocAuditBad.store(bad.size());

    if (bad.isEmpty())
    {
        HTTP_LOG_INFO("[分配表] 不变量巡检通过 order=%s", m_allocOrderCode.toLocal8Bit().data());
        return;
    }

    HTTP_LOG_ERROR("[分配表] 不变量巡检发现 %d 处违规 order=%s —— 明细见下（已写异常表，请人工核对账实）",
        bad.size(), m_allocOrderCode.toLocal8Bit().data());
    for (const QString& line : bad)
        HTTP_LOG_ERROR("[分配表] 违规: %s", line.toLocal8Bit().data());

    if (m_pSortingDb && m_pSortingDb->isOpen())
    {
        ExceptionRecord ex;
        ex.type      = QString::fromUtf8("分配表不变量违反");
        ex.orderCode = m_allocOrderCode;
        ex.reason    = QString::fromUtf8("巡检发现 %1 处违规：%2")
                           .arg(bad.size()).arg(bad.mid(0, 5).join(QString::fromUtf8("; ")));
        ex.time      = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
        m_pSortingDb->insertException(ex);
    }
}

// ──── 波次报告（完结/切出/恢复/开工各一条聚合日志，不是每件）────
void HttpServer::reportPlanAlloc(const QString& tag)
{
    if (!m_allocValid.load())
    {
        HTTP_LOG_INFO("[计划分配表] 报告 tag=%s order=%s 分配表未生效（valid=false，选格走老逻辑）",
            tag.toLocal8Bit().data(), m_allocOrderCode.toLocal8Bit().data());
        return;
    }

    PlanStats st;
    QStringList multiDetail;
    QString typeSummary;      // ★ 2026-09-21：按分拣属性汇总（分类/发货各自独立封顶，现场按属性对账）
    {
        std::lock_guard<std::mutex> lock(m_allocMutex);
        st = m_alloc.stats();
        QVector<PlanAllocTable::UiRow> rows;
        m_alloc.snapshot(&rows);

        // 属性合计：计划/已落/余量（H4 计划快照口径，与巡检⑤⑥⑦同一来源）
        {
            struct Agg { int plan = 0, landed = 0, remain = 0; };
            QMap<QString, Agg> byType;
            for (const auto& r : rows)
            {
                for (const auto& c : r.cells)
                {
                    const QString tn = PlanAllocTable::typeNameOf(c.gridType);
                    Agg& a = byType[tn];
                    a.plan   += c.planQtyH4;
                    a.landed += c.landed;
                    a.remain += c.remain;
                }
            }
            QStringList parts;
            for (auto it = byType.constBegin(); it != byType.constEnd(); ++it)
                parts << QString("%1: 计划%2/已落%3/余%4")
                             .arg(it.key()).arg(it.value().plan).arg(it.value().landed).arg(it.value().remain);
            typeSummary = parts.join(QString::fromUtf8(" ｜ "));
        }

        if (st.multiSkuCnt > 0)
        {
            int shown = 0;
            for (const auto& r : rows)
            {
                if (r.cells.size() < 2 || shown >= 30) continue;
                QStringList one;
                for (const auto& c : r.cells)
                {
                    // ★ 显示 H4 计划（= 硬上限）；若当前额度与之不等（历史上被搬动过）一并标出
                    one << QString("%1(%2):%3件/已落%4%5").arg(PlanAllocTable::gridKeyOf(c.grid))
                               .arg(PlanAllocTable::typeNameOf(c.gridType))
                               .arg(c.planQtyH4).arg(c.landed)
                               .arg(c.planQty != c.planQtyH4
                                        ? QString::fromUtf8("⚠额度被改(%1)").arg(c.planQty) : QString());
                }
                multiDetail << QString("%1→[%2]").arg(r.sku).arg(one.join(" | "));
                ++shown;
            }
        }
    }

    HTTP_LOG_INFO("[计划分配表] 报告 tag=%s order=%s SKU=%d 计划单元=%d 计划件数=%d 已落=%d 在途=%d 余量=%d 多格口SKU=%d",
        tag.toLocal8Bit().data(), m_allocOrderCode.toLocal8Bit().data(),
        st.skuCount, st.cellCount, st.planTotal, st.landedTotal,
        st.reservTotal, st.remainTotal, st.multiSkuCnt);
    // ★ 2026-09-26 单元归属诊断（同一 (SKU,格口) 可有多类型单元时才有意义）：
    //   迟到恢复 = 认领已释放但按存根记回原单元（属性未串）；属性未知 = 多单元无法判定、已按首个有余量单元记账；
    //   即时归还 = 件没进计划格口（66/无格口/落错格）按 PLC 反馈当场归还的额度条数。
    {
        const int nLate = m_allocLateStubCnt.load();
        const int nGuess = m_allocUnitGuessCnt.load();
        const int nBack  = m_allocReleasedOnMissedCnt.load();
        if (nLate > 0 || nGuess > 0 || nBack > 0)
            HTTP_LOG_INFO("[计划分配表] 单元归属 order=%s 迟到反馈按存根归属=%d 件 属性归属未知=%d 件 "
                          "件未进计划格口即时归还额度=%d 条",
                m_allocOrderCode.toLocal8Bit().data(), nLate, nGuess, nBack);
    }
    if (!multiDetail.isEmpty())
    {
        HTTP_LOG_INFO("[计划分配表] 多格口执行情况 order=%s（格口(类型):计划件数/已落件数）: %s",
            m_allocOrderCode.toLocal8Bit().data(), multiDetail.join("; ").toLocal8Bit().data());
    }
    // ★ 2026-09-21 分拣属性分账：分类/发货各自独立封顶 —— 现场按属性核对"计划 vs 实落"
    //   （硬上限 = H4 计划快照；两条红线：每格口 已落 ≤ 计划、每属性 Σ已落 ≤ Σ计划）
    if (!typeSummary.isEmpty())
    {
        HTTP_LOG_INFO("[计划分配表] 属性分账 order=%s（H4计划/已落/余量，按分拣属性隔离）: %s",
            m_allocOrderCode.toLocal8Bit().data(), typeSummary.toLocal8Bit().data());
    }
}

// ──── 选格成功日志节流（性能：5 万件波次下由 10 万行降到 1 万行量级）────
bool HttpServer::allocShouldLogSelect()
{
    AppConfig& cfg = ConfigManager::instance()->config();
    // ★ 计数与判定的读改写都在同一把分配表锁内完成 —— 本函数可能从 PLC 发送线程池
    //   调用（sendBatchCodesWithEpcCache 在主线程或发送池内都可能执行），加锁保证节流
    //   计数不会因并发而产生数据竞争（计数只影响日志，不影响任何分配判定）。
    std::lock_guard<std::mutex> lock(m_allocMutex);
    m_allocPlanLogCnt = (m_allocPlanLogCnt >= INT_MAX - 1) ? 1 : m_allocPlanLogCnt + 1;
    if (cfg.allocPlanLogTail <= 0) return true;                       // 0 = 全部逐条
    if (m_allocPlanLogCnt <= cfg.allocPlanLogTail) return true;
    if (cfg.allocPlanLogStep <= 0) return false;
    return (m_allocPlanLogCnt % cfg.allocPlanLogStep) == 0;
}

// ──── 清空分配表（五处清零点统一入口）────
void HttpServer::clearPlanAllocTable(const QString& reason)
{
    {
        std::lock_guard<std::mutex> lock(m_allocMutex);
        const int inflight = m_alloc.inflightCount();
        if (inflight > 0)
        {
            // 在途认领残留 = 有件已下发但未落格；清表前留痕，便于现场核对（不变量④）
            HTTP_LOG_WARN("[分配表] 清空时有在途认领 %d 条（未收到落格反馈）reason=%s order=%s —— 已随表清空",
                inflight, reason.toLocal8Bit().data(), m_allocOrderCode.toLocal8Bit().data());
        }
        m_alloc.clear();
        m_allocPlanLogCnt = 0;
    }
    m_allocValid.store(false);
    m_allocOrderCode.clear();
    m_allocAuditBad.store(0);
    // ★ 2026-09-26 单元归属诊断计数随波次清零（避免跨波次串账）
    m_allocLateStubCnt.store(0);
    m_allocUnitGuessCnt.store(0);
    m_allocReleasedOnMissedCnt.store(0);
}

// ──── UI 只读快照（主线程；锁内拷内存，不查 DB）────
PlanAllocSnapshot HttpServer::planAllocSnapshot() const
{
    PlanAllocSnapshot snap;
    {
        std::lock_guard<std::mutex> lock(m_allocMutex);   // m_allocOrderCode 也在此锁内读
        snap.orderCode = m_allocOrderCode;
    }
    snap.valid     = m_allocValid.load();
    snap.warnCount = overplanWarningCount();
    snap.auditBad  = m_allocAuditBad.load();

    QVector<PlanAllocTable::UiRow> rows;
    QVector<qint16> grids;
    PlanStats st;
    {
        std::lock_guard<std::mutex> lock(m_allocMutex);
        snap.version = m_alloc.version();
        m_alloc.snapshot(&rows);
        grids = m_alloc.allPlanGrids();
        st    = m_alloc.stats();
    }

    snap.skuCount    = st.skuCount;
    snap.cellCount   = st.cellCount;
    snap.planTotal   = st.planTotal;
    snap.landedTotal = st.landedTotal;
    snap.reservTotal = st.reservTotal;
    snap.remainTotal = st.remainTotal;
    snap.inflightCnt = st.inflightCnt;
    snap.multiSkuCnt = st.multiSkuCnt;

    for (qint16 g : grids)
        snap.gridColumns << PlanAllocTable::gridKeyOf(g);

    // 格口实时状态（容器绑定/禁用/锁格）——现场看这一页即可判断"能不能落"
    QMap<QString, QString> binds = getContainerBindings();
    QMap<QString, QString> bindNorm;
    for (auto it = binds.constBegin(); it != binds.constEnd(); ++it)
    {
        const QString k = normalizeGridKey(it.key());
        if (!k.isEmpty() && !it.value().isEmpty() && !bindNorm.contains(k))
            bindNorm.insert(k, it.value());
    }

    snap.rows.reserve(rows.size());
    for (const auto& r : rows)
    {
        PlanAllocRow row;
        row.sku         = r.sku;
        row.planTotal   = r.planTotal;
        row.landedTotal = r.landedTotal;
        row.reservTotal = r.reservTotal;
        row.remainTotal = r.remainTotal;
        row.cells.reserve(r.cells.size());
        for (const auto& c : r.cells)
        {
            PlanAllocCell cell;
            cell.gridKey   = PlanAllocTable::gridKeyOf(c.grid);
            cell.gridType  = QString::number(c.gridType);
            cell.planQty   = c.planQty;
            cell.landedQty = c.landed;
            cell.reservQty = c.reserv;
            cell.remainQty = c.remain;
            cell.boxcode   = bindNorm.value(cell.gridKey);
            cell.bound     = !cell.boxcode.isEmpty();
            if (m_pPlcMgr)
            {
                cell.disabled = m_pPlcMgr->isGridDisabled(c.grid);
                cell.locked   = m_pPlcMgr->isGridLocked(c.grid);
            }
            row.cells.append(cell);
        }
        snap.rows.append(row);
    }

    // 按计划件数降序（现场最关心"件数多的产品"，也让分页首屏最有价值）
    std::sort(snap.rows.begin(), snap.rows.end(),
              [](const PlanAllocRow& a, const PlanAllocRow& b) {
                  if (a.planTotal != b.planTotal) return a.planTotal > b.planTotal;
                  return a.sku < b.sku;
              });
    return snap;
}

// ============================================================================
// ★ 乙方案：H7 报文裁剪 —— 把某格口各 SKU 行的 qty 裁剪到"该格口计划件数"
//   为什么需要：正常路径下超计划件已改投异常口、不会进箱；但"同时两件在线"等情形
//   仍可能让箱内实落数超过计划，此时若按实落数上报，WMS 会回
//   [2107632]…无法分配 并**整条驳回**，牵连同报文其它正常件一起不落账（现场 034 格口事件）。
//   ★ 2026-09-14 口径修正（配合"同品多格口按计划件数分配"）：
//     封顶基准必须是 **本格口的计划件数**（GridEntry::planQtyPerGrid[本格口]），
//     而不是该 SKU 的计划总数。否则同一 SKU 拆到多格口时（如 34 计划 1 件 + 48 计划 3 件），
//     每个格口都会被 4 件"总数"误判为不超，裁剪形同失效。
//     无分格口计划时（旧数据/单格口）退回该 SKU 计划总数。
//   裁剪策略：qty' = min(qty, 本格口计划件数)（无计划的行不裁剪）；只封顶不补足，
//   并且同一 SKU 只输出一行（重复行合并），保证报文自洽。
//   裁剪掉的多余件不进入上传数据，但已计入"超计划预警"并在 UI 可查、异常表有留痕。
// ============================================================================
int HttpServer::clampFullboxQtyToPlan(const QString& gridKey, QJsonArray& detailList,
                                      QStringList& trimLog) const
{
    trimLog.clear();
    if (!m_pBuffer || detailList.isEmpty())
        return 0;

    // 1) 先按 SKU 合并（同一 SKU 出现多行时合并 qty，防止裁剪后报文自相矛盾）
    QMap<QString, int> skuQty;
    QStringList order;
    for (const QJsonValue& v : detailList)
    {
        const QJsonObject o = v.toObject();
        const QString sku = o.value("sku").toString();
        const int qty = o.value("qty").toString().toInt();
        if (sku.isEmpty()) continue;
        if (!skuQty.contains(sku)) order << sku;
        skuQty[sku] += qty;
    }

    // 2) 逐 SKU 按「本格口计划件数」封顶，重建 detailList（保留原行的 num/targetLocation 字段）
    const QJsonObject first = detailList.first().toObject();
    const QString num = first.value("num").toString();
    const QString box = first.value("targetLocation").toString();
    const QString gKey = normalizeGridKey(gridKey);

    QJsonArray rebuilt;
    int trimmedTotal = 0;
    for (const QString& sku : order)
    {
        const int raw = skuQty.value(sku);
        // ★ 2026-09-14 落格结构优化：封顶基准**只认分配表**（该 SKU 在该格口的计划件数）。
        //   去掉"退回 SKU 计划总数"的兜底 —— 多格口 SKU（34 计划1件 + 48 计划3件，总数4）
        //   在 34 会被 4 件"总数"误判为不超计划，裁剪形同失效，WMS 侧实报超计划整条驳回。
        //   分配表未生效（valid=false）时取 0 = 不加封顶（保持改造前不裁剪的老行为）。
        const int planQty = planQtyOfGrid(sku, gKey);
        int qty = raw;
        if (planQty > 0 && raw > planQty)
        {
            qty = planQty;
            trimmedTotal += (raw - planQty);
            trimLog << QString::fromUtf8("%1 本格口计划%2件 实落%3件 → 报%4件（多余%5件不进入上传报文）")
                           .arg(sku).arg(planQty).arg(raw).arg(qty).arg(raw - planQty);
            HTTP_LOG_WARN("[H7裁剪] 格口%s SKU=%s 本格口计划%d件 实落%d件 → 只报%d件（多余%d件不上传，已登记预警）",
                gKey.toLocal8Bit().data(), sku.toLocal8Bit().data(), planQty, raw, qty, raw - planQty);
        }
        QJsonObject item;
        item["num"]            = num;
        item["qty"]            = QString::number(qty);
        item["sku"]            = sku;
        item["targetLocation"] = box;
        rebuilt.append(item);
    }
    detailList = rebuilt;
    return trimmedTotal;
}

// ★ 2026-09-14 判定某格口是否为配置的物理异常口（超计划件落点）
bool HttpServer::isExceptionGridKey(const QString& gridKey) const
{
    const QString excCfg = ConfigManager::instance()->config().exceptionGrid.trimmed();
    if (excCfg.isEmpty() || excCfg == "0")
        return false;
    return normalizeGridKey(gridKey) == normalizeGridKey(excCfg);
}

// ★ 2026-09-14 取「该 SKU 在该格口的计划件数」：
//   ★ 2026-09-21 硬上限口径：**以 H4 计划为准**（`GridEntry::planQtyPerGrid`），
//     分配表有效时再与 `planQtyH4Of` 取**较小值** —— 保证 H7 报文裁剪/预警判定
//     一律不会超过 WMS 下发的该格口计划件数（现场"计划 2 件、实拣 83 件"的整改闭环）。
//   ★★ 为什么不能用"SKU 计划总数"兜底：多格口时（如 34 计划 2 件 + 40 计划 176 件，总数 178），
//      把总数当作每个格口的计划，会让每个格口都被 178 件"误判为不超计划" → 裁剪失效、
//      预警不触发、WMS 侧实报超计划被整条驳回（[2107632]…无法分配）。
int HttpServer::planQtyOfGrid(const QString& sku, const QString& gridKey) const
{
    if (sku.isEmpty()) return 0;
    const QString gk = normalizeGridKey(gridKey);
    int planH4 = 0;
    if (m_pBuffer)
    {
        const GridEntry e = m_pBuffer->get(sku);
        planH4 = e.planQtyPerGrid.value(gk, 0);
    }
    if (planH4 > 0) return planH4;          // ★ H4 计划优先（权威上限）

    // 兜底：H4 无分格口计划（旧数据/极特殊波次）→ 退回分配表的快照；两者都没有则 0（不裁剪）
    std::lock_guard<std::mutex> lock(m_allocMutex);
    return m_alloc.planQtyH4Of(sku, gk);
}

// ★ 2026-09-14 判定「该格口是否在该 SKU 的计划内」
//   口径（客户确认）：落到的格口若不在该 SKU 的计划里 → 属"落错格"，
//   **该件不写入落格明细**（因而不会出现在该格口的 H7 报文里），只留异常留痕 + UI 提示人工取出。
//   为什么这样处理：该格口对这个 SKU 没有计划数量，一旦上报，WMS 侧会出现
//   "格口 034 上报了它没有计划的 SKU"，可能按格口校验数量而整条驳回
//   （[2107632]…无法分配），牵连同报文其它正常件一起不落账。
//   ★ 边界：分配表未生效（valid=false）或该 SKU 不在本波次映射 → 返回 true（视为计划内，不拦），
//     避免把"信息缺失"误判成"落错格"而误拦正常件；此时由老逻辑分支处理。
bool HttpServer::isGridInPlanOf(const QString& sku, const QString& gridKey) const
{
    if (sku.isEmpty()) return true;                  // 信息不全 → 不拦
    if (m_allocValid.load())
    {
        std::lock_guard<std::mutex> lock(m_allocMutex);
        if (m_alloc.skuHasPlan(sku))
            return m_alloc.inPlanOf(sku, gridKey);   // 有计划 → 严格按计划判定
        // 不在分配表 → 落回下方老口径（不拦）
    }
    // ── 老口径（分配表未生效/该 SKU 无计划时）──
    if (!m_pBuffer) return true;
    const GridEntry entry = m_pBuffer->get(sku);
    if (entry.gridNum.isEmpty())
        return true;                        // 该 SKU 不在本波次映射 → 由其它分支处理，不在此拦
    const QString g = normalizeGridKey(gridKey);
    if (!entry.planQtyPerGrid.isEmpty())
        return entry.planQtyPerGrid.contains(g);
    for (const QString& t : entry.gridNum.split(',', Qt::SkipEmptyParts))
    {
        if (normalizeGridKey(t) == g)
            return true;
    }
    return false;
}

int HttpServer::overplanWarningCount() const
{
    // ★ 单次遍历统计，不调用 overplanWarnings()（避免同一把锁被重复获取 + 构造无用明细）
    std::lock_guard<std::mutex> lock(m_boxLandedMutex);
    int n = 0;
    for (auto it = m_boxLandedEpcs.constBegin(); it != m_boxLandedEpcs.constEnd(); ++it)
    {
        const QString key = it.key();                 // "格口号\nSKU"
        const int sep = key.indexOf('\n');
        if (sep <= 0) continue;
        const QString g   = key.left(sep);
        const QString sku = key.mid(sep + 1);
        if (isExceptionGridKey(g)) continue;          // 异常口是"主动改投"，不计入预警
        const int planQty = planQtyOfGrid(sku, g);    // ★ 按本格口计划（多格口各自计划不同）
        if (planQty > 0 && it.value().size() > planQty) ++n;
    }
    return n;
}

QVector<HttpServer::OverplanWarning> HttpServer::overplanWarnings() const
{
    QVector<OverplanWarning> out;
    std::lock_guard<std::mutex> lock(m_boxLandedMutex);

    for (auto it = m_boxLandedEpcs.constBegin(); it != m_boxLandedEpcs.constEnd(); ++it)
    {
        const QString key = it.key();                 // "格口号\nSKU"
        const int sep = key.indexOf('\n');
        if (sep <= 0) continue;

        OverplanWarning w;
        w.gridKey   = key.left(sep);
        w.sku       = key.mid(sep + 1);
        w.landedQty = it.value().size();

        // ★ 异常口（改投超计划件）不作为"预警"列出：它本就是设计落点，由日志/异常表留痕
        if (isExceptionGridKey(w.gridKey))
            continue;

        // 计划件数：取该 SKU 在**本格口**的计划数（同品多格口时各格口计划不同）
        //   ★ 2026-09-21：基准 = H4 计划（planQtyOfGrid 内部已改为 H4 优先），保证"计划 vs 实落"
        //     的预警口径与硬上限、H7 裁剪三者完全一致（分配表漂移不再影响预警判定）。
        w.planQty = planQtyOfGrid(w.sku, w.gridKey);
        {
            const GridEntry e = m_pBuffer ? m_pBuffer->get(w.sku) : GridEntry();
            w.skuPlanQty = e.gridCount;
            w.gridType   = e.gridTypePerGrid.value(normalizeGridKey(w.gridKey), e.gridType);
        }

        if (w.planQty <= 0 || w.landedQty <= w.planQty)
            continue;                                 // 未超计划 → 不列入预警

        w.overQty = w.landedQty - w.planQty;

        // 多余件 EPC 清单 = 已落格 EPC 中"本格口计划件数之外"的那些
        //   （QSet 无序，故清单顺序不代表落格先后；仅用于现场核对与取出）
        int idx = 0;
        for (const QString& e : it.value())
        {
            if (++idx > w.planQty) w.epcs << e;
        }
        out.append(w);
    }
    return out;
}

// ============================================================================
// ★ 2026-09-15 切回波次时恢复格口绑定的统一入口（**只读恢复，绝不写库**）
//
//   ★★ 铁律（客户口径）：历史数据不得被粘贴/复制进其它波次任务，格口绑定状态尤其如此 ★★
//     `grid_box_bind.order_code` = 那次绑定**当时所属的波次**，是该行的永久归属，只能由
//     "当次 H6 下发"写入；任何"把 A 波次的绑定写进 B 波次"的动作都被禁止。
//     因此本函数只恢复内存与界面显示，**不产生任何新的绑定行**：
//       · 恢复出的绑定是"显示态"（面板/分拣依据），其记录仍属于原波次；
//       · 该波次自己的绑定行只由它当时的 H6 写入，天然完整、可切回、可回溯。
//     （历史教训：曾用"沿用/补记"把绑定复制到新波次名下，导致面板多绑一堆、
//       甚至把早期联调预置的 H65 个旧箱全部复活 —— 该做法已彻底删除。）
// ============================================================================
void HttpServer::applyWaveBinds(const QString& orderCode, const QMap<QString, QString>& binds,
                               const QString& source)
{
    if (binds.isEmpty()) return;

    loadContainerBindings(binds);
    emit bindingUpdated();

    QStringList detail;
    for (auto it = binds.constBegin(); it != binds.constEnd(); ++it)
    {
        if (detail.size() >= 12) { detail << "..."; break; }
        detail << QString("%1→%2").arg(it.key()).arg(it.value());
    }
    HTTP_LOG_INFO("恢复容器绑定(仅显示不写库) order=%s count=%d 来源=%s 明细=%s",
        orderCode.toLocal8Bit().data(), (int)binds.size(), source.toLocal8Bit().data(),
        detail.join(",").toLocal8Bit().data());
    emit logMessage(QString::fromUtf8("[切换] 已恢复该波次的格口绑定 %1 个（来源：%2）")
        .arg(binds.size()).arg(source));
}

// ============================================================================
// ★ 2026-09-16 切回波次时按数据库重建"已落格进度"（断电/关闭重启后"继续分拣"的唯一依据）
//
//   为什么必须有（docs/意外关闭重启_继续上次任务.md §3.6）：进程重启后下列内存结构是空的，
//   不重建就会在"继续分拣"时暴露成账实不符：
//     · m_gridSortRecords（H7 满箱报文明细来源）→ 该格口再满箱时"无分拣记录 跳过"
//       ⇒ 箱内重启前已落的件永远不会上报 ⇒ WMS 数量对不上（现场曾回 [2107632] 整条驳回）；
//     · 计划额度（分配表 landed / m_gridLandedNum）→ 已落件被当成 0 ⇒ 继续投件突破
//       "每格口计划件数"，多余件被改投异常口、箱内少账；
//     · 落格明细去重集合 → 继续分拣时同一件重复记账（换箱前后同一件报两次）。
//
//   取数：SortingDatabase::getLandingRecordsForWave(orderCode)（sorting_records 按落格先后正序，
//         带落格当时的容器号/SKU/库位/时间）= 已落格进度的唯一持久权威。
//   三处口径与实时落格路径**逐条对齐**：
//     ① H7 明细：**只补"仍属于当前绑定容器"的件** —— 换箱前旧箱里的件已在旧箱 H7 里报过，
//        再补进新箱就是同一件报两次（WMS 侧数量对不上）；旧箱件不进明细、但仍进额度（见②）。
//     ② 计划额度：用**整波次全部落格**（跨容器累计，换箱不重置额度），按 (格口,SKU,EPC) 去重。
//     ③ 去重集合：**全部落格件**都登记（含旧箱件），保证继续分拣时同一件不再多写一条明细。
//   ★ 只读 DB、只写内存与界面，**不产生任何数据库写入**（"切回不写库"铁律不变）。
//   ★ 调用时机：必须在"步骤6 恢复绑定"之后（①要靠已恢复的当前容器号做过滤）。
// ============================================================================
void HttpServer::restoreLandedProgress(const QString& orderCode)
{
    if (orderCode.isEmpty() || !m_pSortingDb || !m_pSortingDb->isOpen()) return;

    const QVector<LandedRecord> landed = m_pSortingDb->getLandingRecordsForWave(orderCode);
    if (landed.isEmpty())
    {
        HTTP_LOG_INFO("切回波次无需重建已落格进度 order=%s（库中无该波次落格明细）",
            orderCode.toLocal8Bit().data());
        return;
    }

    int quotaCount   = 0;    // 计入计划额度/封顶计数/去重集合的**行数**（重复行不增加额度）
    int detailAdded  = 0;    // 补进 H7 明细分录的件数（仍属当前绑定容器）
    int skipOldBox   = 0;    // 换箱前旧容器的件（已在旧箱上报过 → 不重复进明细）
    int skipNoSku    = 0;    // 历史行缺 SKU（进明细会让 H7 整单中止 → 只计额度）
    int skipNoBox    = 0;    // 该格口当前无绑定容器（无从归属 → 不入明细）
    int guessNoType  = 0;    // ★ 2026-09-26：多单元格口 + 旧数据无 grid_type ⇒ 属性归属靠启发式（需留痕）
    QMap<QString, int> detailPerGrid;

    for (const LandedRecord& r : landed)
    {
        const QString epc = r.epc.trimmed();
        if (epc.isEmpty()) continue;
        const QString gridKey = normalizeGridKey(r.gridNum);
        if (gridKey.isEmpty()) continue;
        const QString sku = r.sku.trimmed();

        // ── ② 计划额度 / 封顶计数（跨容器累计；同一 EPC 只计 1 次 —— 与实时落格同口径）──
        if (!sku.isEmpty())
        {
            bool mismatch = false; int landedNow = 0, planQty = 0;
            bool lateStub = false, unitGuessed = false; quint8 landedType = 0xFF;
            // ★ 2026-09-26：按落格明细里持久化的**单元类型**精确归属。
            //   业务确认同一 (SKU,格口) 可有多种 grid_type ⇒ 不按类型归属就会落到"首个有余量单元"，
            //   把两个属性的余量填错位（该类型可能被多收、另一类型被饿死）。
            //   旧数据无该列（空串/越界）→ 0xFF（无提示）→ 走启发式，并由 unitGuessed 计数留痕。
            quint8 hint = 0xFF;
            {
                const QString t = r.gridType.trimmed();
                bool okT = false;
                const int ti = t.toInt(&okT);
                if (okT && ti >= 0 && ti <= 2) hint = (quint8)ti;
            }
            commitLandedAlloc(sku, gridKey, epc, 0 /*无在途认领：重启后一律按"账实优先"登记已落*/,
                              &mismatch, &landedNow, &planQty, &lateStub, &unitGuessed, hint, &landedType);
            if (unitGuessed) ++guessNoType;
            noteGridLanded(sku, gridKey, epc);
        }
        else
        {
            ++skipNoSku;
        }
        ++quotaCount;

        // ── ③ 落格明细去重集合：不论此刻容器是否相同都登记（继续分拣时同一件不重复记账）──
        if (isLandingDetailRecorded(gridKey, epc))
            continue;
        markLandingDetailRecorded(gridKey, epc);

        // ── ① H7 满箱明细分录：只补"仍属于当前绑定容器"的件 ──
        if (sku.isEmpty())       continue;                  // 缺 SKU 的件进明细会让 H7 整单中止
        if (isExceptionGridKey(gridKey)) continue;           // 异常口件不上传 WMS（同实时落格口径）
        const QString curBox = currentBoxOfGrid(gridKey);
        if (curBox.isEmpty())             { ++skipNoBox;  continue; }
        if (r.boxcode.trimmed() != curBox) { ++skipOldBox; continue; }

        GridSortRecord rec;
        rec.inco      = epc;
        rec.sku       = sku;
        rec.car       = QString();          // 小车号为实时链路字段，重启后 DB 不保留 → 留空（H7 不用）
        rec.boxcode   = curBox;
        rec.gridCount = 1;                  // 每条落格记录 = 1 件（与实时落格口径一致）
        rec.volu      = r.volu.isEmpty() ? QString("--") : r.volu;
        rec.timeMs    = r.timeMs;
        {
            std::lock_guard<std::mutex> lock(m_gridRecordMutex);
            m_gridSortRecords[gridKey].append(rec);
        }
        ++detailAdded;
        detailPerGrid[gridKey] += 1;
    }

    QStringList perGrid;
    for (auto it = detailPerGrid.constBegin(); it != detailPerGrid.constEnd(); ++it)
    {
        if (perGrid.size() >= 20) { perGrid << "..."; break; }
        perGrid << QString("%1:%2件").arg(it.key()).arg(it.value());
    }

    HTTP_LOG_INFO("切回波次已落格进度重建 order=%s 落格明细=%d 行 额度/去重登记=%d 行 "
                  "H7明细补入=%d 件[%s] 跳过(旧容器=%d 缺SKU=%d 无绑定容器=%d) 属性归属未知=%d "
                  "—— 只重建内存与界面，未写库",
        orderCode.toLocal8Bit().data(), (int)landed.size(), quotaCount,
        detailAdded, perGrid.join(",").toLocal8Bit().data(),
        skipOldBox, skipNoSku, skipNoBox, guessNoType);
    emit logMessage(QString::fromUtf8(
        "[切换] 已按数据库重建本波次已落格进度：落格明细 %1 件（计划额度/落格去重已恢复）；"
        "其中 %2 件补入当前容器的 H7 明细，%3 件属换箱前旧容器（不重复上报）%4")
        .arg(landed.size()).arg(detailAdded).arg(skipOldBox)
        .arg(guessNoType > 0
                 ? QString::fromUtf8("；另有 %1 件因旧数据未记录单元类型、属性归属按首个有余量单元推断，请核对")
                       .arg(guessNoType)
                 : QString()),
        skipNoSku > 0 || guessNoType > 0);
}

// ============================================================================
// ★ 2026-09-06 挂起切出当前波次（供「切换波次」与「新任务」共用）：
//   清空内存（WaveManager/格口映射/格口运行数据），状态与进度保留于 DB；
//   未成功的 H7/H8 报文保留 outbox（切回该波次时自动补发 / 可手动重传）
// ============================================================================

void HttpServer::switchAwayCurrentWave()
{
    if (!m_pWaveMgr) return;

    // ★ 2026-09-22 完结回传（固定延迟发送）：波次被切出（新任务/切回其它波次）→ 取消尚未到点的
    //   延迟发送，避免把 H8 误发给"已经不在内存里"的波次（报文仍在 Outbox，可由面板补传）。
    cancelPendingEndReport(QString::fromUtf8("波次切出"));

    // ★ 2026-09-15 切出留痕：把"切出的是哪个波次"连同绑定数量一并打印，
    //   并记录切出中的波次号——切出后内存波次为空，此期间到达的 H6 不应记到空波次
    //   （否则 H6 绑定的波次归属丢失 → 该波次切回时按波次取不到绑定）。
    const QString outOrder  = m_pWaveMgr->orderCode();
    const int     outStatus = m_pWaveMgr->status();
    if (!outOrder.isEmpty())
    {
        int bindCnt = 0;
        {
            std::lock_guard<std::mutex> lock(m_containerMutex);
            bindCnt = m_containerBindings.size();
        }
        HTTP_LOG_INFO("切出波次 order=%s status=%d(%s) 绑定 %d 个：绑定记录保留于 DB（按波次可回溯），切回时恢复",
            outOrder.toLocal8Bit().data(), outStatus,
            WaveSnapshot::statusToString(outStatus).toLocal8Bit().data(), bindCnt);
        emit logMessage(QString::fromUtf8(
            "[波次] 已切出波次 %1（%2）——进度与格口绑定（%3 个）保留于数据库，切回该波次时自动恢复")
            .arg(outOrder).arg(WaveSnapshot::statusToString(outStatus)).arg(bindCnt));
        m_switchingOutOrderCode = outOrder;
        // ★ 2026-09-17 记录切出时刻：H4 到达时用它判定"哪些行是本窗口内产生的误归属"，
        //   只对窗口内的行做纠偏（窗口外的已归属行永不改判）。
        m_switchOutTime = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
    }

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
    clearBoxLandedCount();   // ★ 2026-09-13 计划数封顶计数随波次切出清空
    clearGridLandedCount();  // ★ 2026-09-14 按格口落格计数（多格口分配依据）随波次切出清空
    clearLandingDedup();     // ★ 2026-09-14 落格明细去重集合随波次切出清空
    // ★ 2026-09-14 计划分配表：切出前先出报告（现场核对"每个格口计划几件、实际落几件"），再清表
    reportPlanAlloc(QString::fromUtf8("波次切出"));
    clearPlanAllocTable(QString::fromUtf8("波次切出"));
    m_pendingSkuQuery.clear();
    m_skuQueryRetryCount.clear();
    m_notReadyRetryCount.clear();
    // ★ 2026-09-11 在途/冷却/重发计数一并清空
    clearAllEpcRuntimeState();
    // ★ 满箱锁格禁用的格口保持禁用（物理状态未变），该波次重新落库/恢复时统一处理

    // ★ 2026-09-15 切出中的波次号保留一小段时间：期间若 WMS 送来 H6 绑定，
    //   记到刚切出的波次（而不是空波次），避免绑定归属丢失导致切回取不回绑定。
    if (!m_switchingOutOrderCode.isEmpty() && m_switchingOutTimer)
        m_switchingOutTimer->start(WAVE_SWITCH_OUT_KEEP_MS);   // 到期自动清空
}

// ============================================================================
// ★ 2026-09-15 关闭软件时的「切出当前波次」（客户需求①）
//   语义：每次关闭都把当前波次任务切出，保留全部信息与格口绑定状态信息到数据库便于回溯；
//         下次开启软件 = 新任务状态（无格口绑定）。
//   本方法在 MainWindow::closeEvent 中、停止接收之前调用（正常退出路径）。
//   异常关闭（崩溃/断电/强杀）不会走到这里 —— 由 restoreWaveFromDB() 在下次启动时兜底归档，
//   因此任一时刻数据库都处于"绑定已归档、波次可切回"的一致状态。
//   ① 波次进度/计划/明细/落格/异常：早已在 DB（落库即写），无需额外动作，状态保持原值（保留切回时的空）；
//   ② 未成功的 H7/H8：保留在 outbox（切回该波次时自动补发，也可手动重传）；
//   ③ 格口绑定：归档（active=0 + unbind_time）并逐格留痕 —— 行不删除，切回时按波次取回；
//   ④ 内存清空（SKU 映射/格口记录/在途/分配表等），面板回到"未绑定"。
// ============================================================================
void HttpServer::switchOutWaveForExit()
{
    const QString orderCode = m_pWaveMgr ? m_pWaveMgr->orderCode() : QString();
    const int     status    = m_pWaveMgr ? m_pWaveMgr->status() : WAVE_IDLE;
    const bool    hasWave   = (!orderCode.isEmpty() && status != WAVE_IDLE);

    // ── ① 绑定归档 + 逐格留痕（先归档再清内存，保证"归档的是即将被清掉的那份映射"）──
    int bindCount = 0;
    {
        std::lock_guard<std::mutex> lock(m_containerMutex);
        bindCount = m_containerBindings.size();
        for (auto it = m_containerBindings.constBegin(); it != m_containerBindings.constEnd(); ++it)
        {
            HTTP_LOG_INFO("[解绑留痕] grid=%s 旧箱=%s order=%s 原因=关闭软件切出波次 动作=DB归档(active=0)+内存清空",
                it.key().toLocal8Bit().data(), it.value().toLocal8Bit().data(),
                orderCode.toLocal8Bit().data());
            LOG_INFO("[解绑留痕] grid=%s 旧箱=%s order=%s 原因=关闭软件切出波次",
                it.key().toLocal8Bit().data(), it.value().toLocal8Bit().data(),
                orderCode.toLocal8Bit().data());
        }
    }
    bool archived = true;
    if (m_pSortingDb && m_pSortingDb->isOpen() && bindCount > 0)
        archived = m_pSortingDb->archiveAllBinds();

    // ── ② 切出波次（清内存；DB 状态/进度/明细保持 → 列表可见、可切回）──
    if (hasWave)
        switchAwayCurrentWave();
    if (m_pPlcMgr)
        m_pPlcMgr->enableAllGrids();

    // ── ③ 内存绑定清空 → 面板回到"未绑定"（下次启动也是新任务状态）──
    {
        std::lock_guard<std::mutex> lock(m_containerMutex);
        m_containerBindings.clear();
        m_boxSnapshot.clear();   // ★ 2026-09-22 离场箱号快照随绑定一并清空
    }
    emit bindingUpdated();

    // ── ④ 可读摘要（现场核对"关掉了什么、留下了什么"）──
    int h7Pend = 0, h8Pend = 0;
    if (m_pSortingDb && m_pSortingDb->isOpen() && !orderCode.isEmpty())
    {
        for (const OutboxRecord& r : m_pSortingDb->getOutboxFullboxByOrder(orderCode))
            if (r.status != "success") ++h7Pend;
        for (const OutboxRecord& r : m_pSortingDb->getOutboxEndByOrder(orderCode))
            if (r.status != "success") ++h8Pend;
    }
    if (hasWave)
    {
        const QString line = QString::fromUtf8(
            "[切出] 关闭软件：已切出波次 %1（%2）——进度/明细/计划/落格记录保留于数据库；"
            "格口绑定 %3 个已归档留痕（%4）；未成功报文 满箱%5 完结%6 保留待补发；"
            "下次开启为新任务状态（未绑定），可从「波次数据记录」切回继续")
            .arg(orderCode).arg(WaveSnapshot::statusToString(status)).arg(bindCount)
            .arg(archived ? QString::fromUtf8("归档成功") : QString::fromUtf8("归档失败，见日志"))
            .arg(h7Pend).arg(h8Pend);
        HTTP_LOG_INFO("关闭切出波次 order=%s status=%d(%s) 绑定=%d 归档=%s H7待发=%d H8待发=%d",
            orderCode.toLocal8Bit().data(), status,
            WaveSnapshot::statusToString(status).toLocal8Bit().data(), bindCount,
            archived ? "成功" : "失败", h7Pend, h8Pend);
        LOG_INFO("[切出] 关闭软件切出波次 order=%s status=%d 绑定归档=%d 结果=%s",
            orderCode.toLocal8Bit().data(), status, bindCount, archived ? "成功" : "失败");
        emit logMessage(line, !archived);
    }
    else
    {
        if (bindCount > 0)
        {
            HTTP_LOG_INFO("关闭：当前无进行中波次，归档残留绑定 %d 个 结果=%s（历史行保留）",
                bindCount, archived ? "成功" : "失败");
            emit logMessage(QString::fromUtf8("[切出] 关闭软件：当前无进行中波次，已归档残留格口绑定 %1 个"
                                              "（历史记录保留于数据库，可追溯）").arg(bindCount),
                            !archived);
        }
        else
        {
            HTTP_LOG_INFO("关闭：无进行中波次、无待归档绑定 —— 无需切出");
        }
    }
}

// ============================================================================
// ★ 2026-09-17 现场要求：把「波次信息面板」复位为"等待 WMS 下发新波次"（= 新任务状态）
//
//   现场问题：停止接收 → 再次点「开始接收任务」后，波次信息面板仍显示上一波次的
//   波次号/状态/计划件数/已分拣/异常/H7/H8/时间等**旧数据**，看起来像"任务还在"。
//   现场要求：与「新任务」按钮一样，**点击开始接收任务就把波次信息面板数据清空**。
//
//   语义（与「新任务」切出完全一致，复用同一条切出链路 switchAwayCurrentWave）：
//     · 当前波次**切出**：进度/明细/计划/落格/异常/未成功报文全部保留于 DB（状态保持原值）；
//     · 内存清空（WaveManager→IDLE、SKU 映射、格口记录、在途、分配表…）→ 面板回到"等待下发"；
//     · 该波次仍可从「波次数据历史记录」**切回继续**（切回时进度与绑定一并恢复）。
//   绑定清除不在此函数内：清空/归档由调用方已有链路负责
//   （开始接收 = restoreWaveFromDB() 清内存+归档 DB；新任务 = archiveAttributedBinds()+清内存）。
//
//   ★ 刻意不自动装载任何波次：与"启动=新任务状态"口径一致（WMS 重新下发才开工）。
// ============================================================================
bool HttpServer::resetWavePanelToIdle(const QString& reason)
{
    if (!m_pWaveMgr) return false;

    const QString oldOrder = m_pWaveMgr->orderCode();
    const int     oldStatus = m_pWaveMgr->status();
    const QString tag = reason.isEmpty() ? QString::fromUtf8("切出") : reason;

    if (oldOrder.isEmpty() || oldStatus == WAVE_IDLE)
    {
        HTTP_LOG_INFO("%s：当前无进行中波次，波次信息面板已是初始状态（等待 WMS 下发新波次）",
            tag.toLocal8Bit().data());
        return false;
    }

    if (oldStatus == WAVE_ENDING || oldStatus == WAVE_FULLBOX_SYNC)
    {
        HTTP_LOG_INFO("%s：当前波次处于 %s，仍安全切出（未成功 H7/H8 保留，切回该波次时自动补发）",
            tag.toLocal8Bit().data(), WaveSnapshot::statusToString(oldStatus).toLocal8Bit().data());
    }

    switchAwayCurrentWave();   // 内存清空；DB 状态/进度/明细/报文保留（可切回）

    HTTP_LOG_INFO("%s：已切出波次 order=%s status=%d(%s) -> 空闲（波次信息面板复位为等待下发；"
                  "数据保留于 DB，可随时从「波次数据历史记录」切回）",
        tag.toLocal8Bit().data(), oldOrder.toLocal8Bit().data(), oldStatus,
        WaveSnapshot::statusToString(oldStatus).toLocal8Bit().data());
    emit logMessage(QString::fromUtf8(
        "[波次] %1：已切出波次 %2（%3）——波次信息面板已复位为「等待 WMS 下发新波次」，"
        "进度与数据保留于数据库，可从「波次数据历史记录」切回继续")
        .arg(tag).arg(oldOrder).arg(WaveSnapshot::statusToString(oldStatus)));

    // 通知 UI 刷新波次面板（waveResumed 的既有处理就是 updateWavePanel()）
    emit waveResumed(QString(), WAVE_IDLE);
    return true;
}

// ★ 2026-09-15 该波次是否已终态（已完成/已取消）
//   ★ 2026-09-16 需求①：终态波次**一律不允许切回**（见 resumeUnfinishedWave 入口守卫），
//     本接口供 UI 判定"该行不可切回"（禁用按钮/双击时提示），不再用于"载入查看"分支。
bool HttpServer::isWaveTerminal(const QString& orderCode)
{
    const int st = waveDbStatus(orderCode);
    return (st == WAVE_FINISHED || st == WAVE_CANCELLED);
}

// ★ 2026-09-16 需求①：取波次 DB 状态（不存在/库未开 → -1，供 UI 判定与日志）
int HttpServer::waveDbStatus(const QString& orderCode)
{
    if (orderCode.isEmpty() || !m_pSortingDb || !m_pSortingDb->isOpen()) return -1;
    const int st = m_pSortingDb->getWaveStatus(orderCode);
    return (st <= 0) ? -1 : st;   // 无记录返回 0 → 归一为 -1（"未知/不存在"）
}

// ============================================================================
// ★ 2026-09-06 新任务：当前波次进度/数据保留于 DB（可切换回来继续），
//   内存清空回到空闲（IDLE + 空 SKU 映射），等待 WMS 下发新波次。
//   说明：不落库状态（DB 保留切出前状态=列表显示）；H7/H8 后台补发定时器保持运行
//   （pollOutbox* 只重试当前内存波次的消息，切出波次的未成功报文在切换回时自动补发/可手动重传）
//
// ★ 2026-09-17 现场要求：**开始新任务时清空格口绑定**（原实现是"绑定留用、等新 H6 覆盖"）
//   动作 = ①逐格解绑留痕日志 ②DB 归档（active=0 + unbind_time，行与归属永不改动）
//          ③内存绑定清空（面板全部回到「未绑定」）④恢复满箱锁格禁用的格口
//   口径与「关闭软件切出」「启动=新任务状态」一致（见 docs/意外关闭重启_继续上次任务.md）：
//     · 该波次的绑定记录仍在库里 → **切回该波次时按记录逐格恢复**（不影响本次的切回修复）；
//     · 只归档"已归属"的行，order_code='' 的待补齐行**保留 active** —— 否则 H4 到达时
//       归属补齐不到它们，新波次又会 0 绑定（切回无绑定可恢复，即现场老问题回归）。
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

    // ── ★ 2026-09-17 清空格口绑定（现场要求）──
    //   ① 先逐格留痕（保证留痕的就是"清空那一刻"的映射），再归档，最后清内存
    int clearedBinds = 0;
    {
        std::lock_guard<std::mutex> lock(m_containerMutex);
        clearedBinds = m_containerBindings.size();
        for (auto it = m_containerBindings.constBegin(); it != m_containerBindings.constEnd(); ++it)
        {
            HTTP_LOG_INFO("[解绑留痕] grid=%s 旧箱=%s order=%s 原因=新任务清空格口绑定 动作=DB归档(active=0)+内存清空",
                it.key().toLocal8Bit().data(), it.value().toLocal8Bit().data(),
                oldOrder.toLocal8Bit().data());
            LOG_INFO("[解绑留痕] grid=%s 旧箱=%s order=%s 原因=新任务清空绑定",
                it.key().toLocal8Bit().data(), it.value().toLocal8Bit().data(),
                oldOrder.toLocal8Bit().data());
        }
    }
    bool archived = true;
    if (m_pSortingDb && m_pSortingDb->isOpen())
        archived = m_pSortingDb->archiveAttributedBinds();   // 只归档已归属行；待补齐行保留
    else
        archived = false;
    {
        std::lock_guard<std::mutex> lock(m_containerMutex);
        m_containerBindings.clear();
        m_boxSnapshot.clear();   // ★ 2026-09-22 离场箱号快照随绑定一并清空
    }

    // ★ 2026-09-07 新任务：恢复满箱禁用格口（物理已处理或 WMS 将重新绑定）、刷新绑定面板
    if (m_pPlcMgr)
        m_pPlcMgr->enableAllGrids();
    emit bindingUpdated();

    HTTP_LOG_INFO("新任务清空格口绑定 cleared=%d 归档=%s（历史行保留、归属不变，切回该波次时按记录恢复）",
        clearedBinds, archived ? "成功" : "失败，见 DataBase 日志");
    emit logMessage(QString::fromUtf8(
        "[新任务] 格口容器绑定已清空：面板 %1 个绑定全部复位为「未绑定」（DB 已归档留痕，"
        "未成功归档请查日志）；原记录保留在数据库，切回该波次时会逐格恢复%2")
        .arg(clearedBinds)
        .arg(archived ? QString() : QString::fromUtf8("（**归档失败，详见日志**）")), !archived);

    // ★ 2026-09-08 需求修正（7a）：「新任务」**不再读取/执行剩余待执行波次**——
    //   只把当前波次切出并回到空闲，直接等待 WMS 下发新的波次；
    //   剩余队列原样保留（可在「查看接收波次队列」中查看，或在下次「开始接收任务」时自动执行队首）
    if (!m_pendingWaveQueue.isEmpty())
    {
        HTTP_LOG_INFO("新任务：待执行队列保留 %d 个（不自动执行，等待 WMS 下发新波次/下次开始接收时执行）",
            m_pendingWaveQueue.size());
        emit logMessage(QString("[新任务] 已回到空闲：等待 WMS 下发新波次；"
                                "剩余待执行波次 %1 个已保留（不自动执行，可点「查看接收波次队列」查看）")
            .arg(m_pendingWaveQueue.size()));
    }
    else
    {
        emit logMessage("[新任务] 已回到空闲：等待 WMS 下发新波次；旧波次可随时从「波次数据记录」切换回来");
    }

    emit waveResumed(QString(), WAVE_IDLE);
    return true;
}

// ============================================================================
// ★ 2026-09-17 待执行队列「入队唯一入口」：新到达排队 / 冻结回滚 / 队首回滚共用一条链路，
//   保证去重、SKU 集合、落库、UI 刷新、留痕日志在任何一条路径上都不遗漏。
//   front=true 插队首（"重放结果被拦下"的回滚：它本就是队首，放回后不跳号）。
//   返回 true=本次确实入队；false=队列中已有同单（忽略重复下发，未改动队列）。
// ============================================================================
bool HttpServer::enqueuePendingWave(const QString& orderCode, int orderQty,
                                   const QByteArray& rawBody, qint64 recvTimeMs,
                                   bool front, const QString& reason)
{
    if (orderCode.isEmpty()) return false;

    // 防重复排队：队列中已有同单 → 忽略本次（重放回滚同理：已在队列里的就是它自己）
    for (const PendingWave& p : m_pendingWaveQueue)
    {
        if (p.orderCode == orderCode)
        {
            HTTP_LOG_WARN("波次已在待执行队列，忽略重复 orderCode=%s 原因=%s",
                orderCode.toLocal8Bit().data(), reason.toLocal8Bit().data());
            return false;
        }
    }

    PendingWave pw;
    pw.orderCode = orderCode;
    pw.orderQty  = orderQty;      // ★ 2026-09-08 队列弹窗展示件数
    pw.rawBody   = rawBody;
    pw.recvTime  = (recvTimeMs > 0) ? recvTimeMs : QDateTime::currentMSecsSinceEpoch();
    // ★ 2026-09-14 波次隔离：SKU 集合从 H4 轻量提取（只读 items[].inco，不做映射/校验）
    pw.skuSet    = extractPendingWaveSkuSet(rawBody);
    if (pw.skuSet.isEmpty())
    {
        HTTP_LOG_WARN("待执行波次排队：SKU 集合提取为空 orderCode=%s（不影响执行，仅波次隔离判定退化为按状态判定）",
            orderCode.toLocal8Bit().data());
    }

    if (front) m_pendingWaveQueue.prepend(pw);
    else       m_pendingWaveQueue.append(pw);
    emit pendingWavesChanged();   // ★ 2026-09-08 UI 队列/波次列表刷新
    // ★ 落波次头（波次记录列表可见，状态=已下发/待执行）；明细待执行时随格口映射一起落库
    if (m_pSortingDb)
        m_pSortingDb->upsertReturnWave(orderCode, orderQty, WAVE_CREATED);
    HTTP_LOG_INFO("待执行波次入队 orderCode=%s qty=%d %s 队列=%d 原因=%s",
        orderCode.toLocal8Bit().data(), orderQty, front ? "(队首)" : "(队尾)",
        m_pendingWaveQueue.size(), reason.toLocal8Bit().data());
    return true;
}

// ★ 2026-09-14 波次隔离：从 H4 原始报文提取本波次涉及的 SKU（仅 inco 字段，不做校验）
QSet<QString> HttpServer::extractPendingWaveSkuSet(const QByteArray& rawBody)
{
    QSet<QString> skus;
    QJsonParseError perr;
    const QJsonDocument pdoc = QJsonDocument::fromJson(rawBody, &perr);
    if (perr.error) return skus;
    const QJsonArray pitems = pdoc.object().value("items").toArray();
    for (const QJsonValue& v : pitems)
    {
        const QString inco = v.toObject().value("inco").toString().trimmed();
        if (!inco.isEmpty()) skus.insert(inco);
    }
    return skus;
}

// ============================================================================
// ★ 2026-09-07 待执行队列出队：取队首重放给 ParseWorker 解析注册。
//
// ★★ 2026-09-17 现场缺陷"跳过一个波次"修复（详见 PendingWaveQueuePolicy.h）：
//   出队**只允许**发生在"接收中(本次会话已开启) 且 本次会话没点过「结束任务」"时。
//   旧实现只要波次走到终态就出队，但点「结束任务」后 H8 回调里波次就变已完结，
//   而接收层要等 H8 回调走完才收尾（stopReceive 在 endReportFinished 之后），
//   于是**界面已显示「开始接收任务」，队首却已被重放注册**；操作员再点「开始接收任务」
//   时 startReceive() 又把这一波切出并执行下一个队首 → 中间那一波被跳过。
//   现在：冻结态下**只记日志、原样保留队列**，等点「开始接收任务」时再按队首顺序执行。
//
//   仍保留：接收中波次被 WMS 取消（H5 终态）后自动接替下一波（test/e2e_pending_replay.py 依赖）。
//   仅当内存空闲（IDLE/终态）时执行。
// ============================================================================
void HttpServer::maybeStartPendingWave()
{
    if (m_pendingWaveQueue.isEmpty()) return;
    if (!m_pWaveMgr || !m_pQueue) return;

    // ── 出队时机闸门（现场要求：只有点「开始接收任务」才开始接收队列里的任务）──
    const PendingDequeueDecision decision =
        pendingDequeueDecision(m_receiving.load(), m_endRequested.load(), m_pendingWaveQueue.size());
    if (decision != PDQ_ALLOW)
    {
        if (decision != PDQ_QUEUE_EMPTY)
        {
            HTTP_LOG_INFO("待执行波次不出队：%s（队列 %d 个原样保留，点「开始接收任务」后按队首顺序执行）",
                pendingDequeueDecisionText(decision), m_pendingWaveQueue.size());
            emit logMessage(QString::fromUtf8(
                "[波次] 待执行队列 %1 个已保留：%2——点「开始接收任务」后才会依次执行队首")
                .arg(m_pendingWaveQueue.size())
                .arg(QString::fromUtf8(pendingDequeueDecisionText(decision))));
        }
        return;
    }

    int st = m_pWaveMgr->status();
    if (!(st == WAVE_IDLE || st == WAVE_CANCELLED || st == WAVE_FINISHED))
        return;   // 当前波次仍进行中，不插队

    // ★★ 2026-09-17 悬垂引用修复（既有缺陷）：
    //   旧实现 `const PendingWave& pw = first(); ...; removeFirst(); ... pw.orderCode ...`
    //   —— removeFirst() 后 `pw` 引用的是**已被搬移/释放的元素**：日志里打印的是"队首之后
    //   那一波的波次号"（现场据此误判"跳了一个波次"），且属 use-after-free（UB）。
    //   现场实录：16:48:22.308 实放 test-002，日志却打 `orderCode=test-003`。
    //   现在先整体取出（值拷贝），push 失败再放回队首，日志一律用副本。
    const PendingWave pw = m_pendingWaveQueue.takeFirst();

    // 队首重放（push 成功才真正出队；水位满则放回队首，下次触发再试）
    WaveTask task;
    task.rawBody  = pw.rawBody;
    task.fullUrl  = pw.fullUrl.isEmpty() ? QString("queue://replay") : pw.fullUrl;
    task.recvTime = pw.recvTime;
    if (!m_pQueue->push(task))
    {
        m_pendingWaveQueue.prepend(pw);   // 回滚：仍在队首，不跳号
        HTTP_LOG_WARN("待执行波次入队失败(队列满) orderCode=%s，已放回队首，稍后自动重试",
            pw.orderCode.toLocal8Bit().data());
        return;
    }
    m_pendingReplayInFlight.fetch_add(1);   // 重放已交给 ParseWorker，等解析回调配对出队
    emit pendingWavesChanged();   // ★ 2026-09-08 UI 队列/波次列表刷新
    HTTP_LOG_INFO("开始执行待执行波次 orderCode=%s 来源=待执行队列 队列剩余=%d",
        pw.orderCode.toLocal8Bit().data(), m_pendingWaveQueue.size());
    emit logMessage(QString("[波次] 开始执行队首波次 %1（待执行剩 %2 个）——解析完成后进入该任务")
        .arg(pw.orderCode).arg(m_pendingWaveQueue.size()));
}

// ============================================================================
// ★ 2026-09-17 归属补齐/纠偏：H4 到达（波次落库完成）时，把"本会话内、当前物理生效、
//   尚未归属本波次"的绑定行改判给本波次。
//
//   ★★ 为什么必须有（现场取证，docs/格口绑定波次归属_根因与修复_20260917.md §二）：
//     H6（BindingLatticePort）**不含波次号**，归属只能取"内存当前波次"，于是：
//       ① H6 早于 H4（WMS 先发绑定再发波次）→ 内存无波次 → 归属写成空串
//          → 切回该波次时按波次取不到绑定 → 现场看到"切回去绑定没回来/被清空"（波次 789）；
//       ② 切出/新任务后 120s"切出窗口"内到达的 H6 被记到**刚切出的上一个波次**
//          → 新波次零绑定、旧波次被污染（09-15 23:53 那 66 条 H6 实为 456456 的绑定，
//            却挂在 6565656 名下；切回 456456 时"无任何绑定可恢复"）。
//     两类都只能在"H4 到达"这一刻判定：此刻"自上次波次以来收到的绑定"就是本次投递的绑定。
//
//   ★ 与"历史不得粘贴进其它波次"铁律共存的约束（实现在 SQL_UPDATE_BINDS_ATTRIBUTE_TO_WAVE）：
//     ① 只处理 active=1 且 bind_time >= 本次会话启动时刻的行 → 历史行永不改判；
//     ② 只处理"空归属"或"切出窗口内、属于本次投递的误归属"两类 → 已归属且不在窗口内的行永不改判；
//     ③ 只改 order_code；不新增/不删除行；该格口已有本波次记录则跳过。
//   ★ 返回改判行数；>0 时刷新界面（面板显示的是内存映射，不受影响，但列表「格口绑定」列要变）。
int HttpServer::attributePendingBindsToWave(const QString& orderCode)
{
    if (orderCode.isEmpty() || !m_pSortingDb || !m_pSortingDb->isOpen())
        return 0;

    const int changed = m_pSortingDb->attributeBindsToWave(
        orderCode, m_sessionStartTime, m_switchingOutOrderCode, m_switchOutTime);

    if (changed <= 0)
    {
        HTTP_LOG_INFO("绑定归属补齐 order=%s 改判=0 行（本会话无未归属/待纠偏的活跃绑定）",
            orderCode.toLocal8Bit().data());
        return 0;
    }

    HTTP_LOG_INFO("绑定归属补齐 order=%s 改判=%d 行（来源：空归属 或 切出窗口内的误归属；"
                  "只改 order_code，未增删行）sessionStart=%s 切出窗口=%s@%s",
        orderCode.toLocal8Bit().data(), changed,
        m_sessionStartTime.toLocal8Bit().data(),
        m_switchingOutOrderCode.isEmpty() ? "(无)" : m_switchingOutOrderCode.toLocal8Bit().data(),
        m_switchOutTime.toLocal8Bit().data());
    emit logMessage(QString::fromUtf8(
        "[绑定归属] 波次 %1：已把 %2 个格口的绑定补记到本波次（H6 早于 H4 的归属落空 / 切出窗口内的误归属）"
        "——切回本波次时即可恢复这些绑定；仅补归属，未新增或删除任何绑定行")
        .arg(orderCode).arg(changed));
    emit bindingUpdated();

    // 归属已补上，切出窗口的"待纠偏"使命完成 → 立即收窗，避免继续影响后续波次
    if (!m_switchingOutOrderCode.isEmpty() && m_switchingOutOrderCode != orderCode)
    {
        HTTP_LOG_INFO("归属补齐后关闭切出窗口 order=%s（原窗口波次 %s @ %s）",
            orderCode.toLocal8Bit().data(), m_switchingOutOrderCode.toLocal8Bit().data(),
            m_switchOutTime.toLocal8Bit().data());
        m_switchingOutOrderCode.clear();
        m_switchOutTime.clear();
        if (m_switchingOutTimer) m_switchingOutTimer->stop();
    }
    return changed;
}

// ★ 2026-09-17 某波次绑定明细（只读）：每格取"该波次内"最后一条 —— 与切回恢复取数同一口径
QVector<GridBoxBindRecord> HttpServer::getWaveBindDetail(const QString& orderCode)
{
    if (orderCode.isEmpty() || !m_pSortingDb || !m_pSortingDb->isOpen())
        return QVector<GridBoxBindRecord>();
    return m_pSortingDb->getLastBindsByOrder(orderCode);
}

// ============================================================================
// 绑定沿用（新波次注册后、且当前内存无任何绑定时）
//
//   ★★ 铁律（客户口径）：历史数据不得被粘贴进其它波次任务，格口绑定状态尤其如此 ★★
//   因此本函数**只把"现场当前正在用的绑定"显示到面板**（供分拣继续），
//   **绝不写 `grid_box_bind`**：
//     · 该波次自己的绑定行，只由它当时的 H6 下发写入（归属唯一、可回溯）；
//     · 未下发 H6 时，本波次在库里就是"无绑定记录"（这是事实，不做任何复制）；
//     · WMS 下发 H6 后，绑定自然以本波次号落库。
//   来源口径：内存绑定 → 否则 DB 中 active=1 的绑定（异常关闭后重启）→ 都没有则不臆造。
//   （历史教训：曾把沿用的绑定"重建为本波次的行"，导致面板多绑一堆、
//     甚至把早期联调预置的 65 个旧箱全部复活 —— 该做法已彻底删除。）
// ============================================================================
void HttpServer::restoreBindsIfEmpty(const QString& orderCode)
{
    if (!m_pSortingDb || !m_pSortingDb->isOpen() || orderCode.isEmpty()) return;
    {
        std::lock_guard<std::mutex> lock(m_containerMutex);
        if (!m_containerBindings.isEmpty()) return;   // 已有绑定（含沿用/新 H6）则不动
    }

    // 现场当前正在用的绑定：内存 → DB active → 空
    QMap<QString, QString> cur;
    for (const GridBoxBindRecord& b : m_pSortingDb->getAllActiveBinds())
        if (!b.gridNum.isEmpty() && !b.boxcode.isEmpty())
            cur.insert(b.gridNum, b.boxcode);

    if (cur.isEmpty())
    {
        HTTP_LOG_INFO("沿用绑定跳过 orderCode=%s：现场当前无活跃绑定（内存为空且 DB 无 active 绑定）"
                      "——保持未绑定，等 WMS 下发 H6",
            orderCode.toLocal8Bit().data());
        return;
    }

    applyWaveBinds(orderCode, cur, QString::fromUtf8("现场当前活跃绑定（仅显示，不写库）"));
    HTTP_LOG_INFO("沿用绑定(仅显示) orderCode=%s binds=%d（本波次未收到 H6；不写库，等 WMS 下发 H6 后按本波次落库）",
        orderCode.toLocal8Bit().data(), cur.size());
    emit logMessage(QString("[绑定] 波次 %1 无新绑定下发——已显示当前绑定 %2 个（**未写入本波次记录**，"
                            "等 WMS 下发 H6）；WMS 下发后自动覆盖")
        .arg(orderCode).arg(cur.size()));
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
            const GridEntry& e = it.value();   // ★ 用引用：避免每个 SKU 深拷两个 QMap（万级 SKU 时开销可观）
            ReturnWaveItemRecord rec;
            rec.orderCode = orderCode;
            rec.inco      = it.key();  // inco=SKU编码

            // ★ 2026-09-14 按「每格口计划」逐格口写行（落格结构优化的持久化侧）
            //   口径（客户确认）：一个 SKU 可同时计划到「正常分拣格口」与「发货格口」，
            //   **每个格口各有一份数量**，DB 一行 = 一个 (SKU,格口) 对该格口的计划件数。
            //   ★ 修复的缺陷：改造前这里写的是 it.value().gridCount（该 SKU 的**计划总数**）
            //     与合并后只剩最后一行的 gridType → return_wave_item 无法回答"这个格口计划
            //     几件"，波次恢复后计划被污染，且"计划 vs 实际"对账失去依据（追溯链断裂）。
            //   ★ 2026-09-26 现场口径：一行 = 一个 **(SKU,格口,分拣类型)** 单元
            //     （同格口的分类行与发货行各写一行；表已有 grid_type 列且无唯一约束，无需改表）。
            if (!e.planQtyPerCell.isEmpty())
            {
                for (auto cit = e.planQtyPerCell.constBegin(); cit != e.planQtyPerCell.constEnd(); ++cit)
                {
                    ReturnWaveItemRecord r2 = rec;
                    r2.gridNum  = cellKeyGridOf(cit.key());        // 内部 3 位 key（"034"）
                    r2.gridType = cellKeyTypeOf(cit.key());        // 0=分类 / 1=异常 / 2=发货
                    r2.planQty  = cit.value();                     // ★ 本单元计划件数（不是 SKU 总数）
                    r2.volu     = e.volu;
                    r2.obxCode  = e.obxCode;
                    items.append(r2);
                }
                continue;
            }
            if (!e.planQtyPerGrid.isEmpty())
            {
                for (auto pit = e.planQtyPerGrid.constBegin(); pit != e.planQtyPerGrid.constEnd(); ++pit)
                {
                    ReturnWaveItemRecord r2 = rec;
                    r2.gridNum  = pit.key();     // 内部 3 位 key（"034"），与绑定/PLC/落格库口径一致
                    r2.planQty  = pit.value();   // ★ 本格口计划件数（不是 SKU 总数）
                    r2.gridType = e.gridTypePerGrid.value(pit.key(), e.gridType.isEmpty() ? "0" : e.gridType);
                    r2.volu     = e.volu;
                    r2.obxCode  = e.obxCode;
                    items.append(r2);
                }
                continue;
            }

            // 兜底（旧数据/无分格口计划）：按候选串拆分，计划数记为该 SKU 总数
            //   —— 与改造前行为一致，保证老数据仍可落库、可恢复
            QStringList grids = e.gridNum.split(',', Qt::SkipEmptyParts);
            for (const QString& g : grids)
            {
                rec.gridNum   = g.trimmed();
                rec.gridType  = e.gridType.isEmpty() ? "0" : e.gridType;  // 0=正常分拣, 1=异常, 2=发货
                rec.planQty   = e.gridCount;
                rec.volu      = e.volu;
                rec.obxCode   = e.obxCode;     // 容器号
                items.append(rec);
            }
        }
    }
    return items;
}

// ============================================================================
// ★ 2026-09-14 「计划格口 vs 容器绑定/类型」预检（新波次开工前）
//   客户口径：**每个格口都有对应这个产品的数量**（正常分拣/发货格口各自一份计划），
//   按这些数量分配的前提是——计划里的每个格口都必须有容器可落。
//   因此开工前把"计划里有件、但当前没绑定容器"的格口一次性列出来，
//   避免件被发到无容器格口（PLC 回报"无格口"→落不进箱→人工翻找）。
//   只告警、不阻塞（绑定可能随后由 WMS 的 H6 补上）。
// ============================================================================
void HttpServer::precheckPlanGridBindings(const QString& orderCode)
{
    if (!m_pBuffer || !m_pPlcMgr)
        return;

    // 1) 计划：格口号 → (所属SKU数、计划件数合计、类型)
    //   ★ 2026-09-26：按**单元** (SKU,格口,分拣类型) 汇总后再落到格口（同格口的分类/发货各自计数，
    //     类型取该格口首个单元的类型用于显示）。
    QMap<QString, int> gridQty;      // 格口 → 计划件数合计（各单元之和）
    QMap<QString, int> gridSkuCnt;   // 格口 → 涉及 SKU 数
    QMap<QString, QString> gridType; // 格口 → 类型（取首个非空）
    {
        const QMap<QString, GridEntry>* pMap = m_pBuffer->activeMap();
        if (!pMap)
            return;
        for (auto it = pMap->constBegin(); it != pMap->constEnd(); ++it)
        {
            const GridEntry& e = it.value();
            if (!e.planQtyPerCell.isEmpty())
            {
                for (auto cit = e.planQtyPerCell.constBegin(); cit != e.planQtyPerCell.constEnd(); ++cit)
                {
                    if (cit.value() <= 0) continue;
                    const QString gk = cellKeyGridOf(cit.key());
                    const QString tp = cellKeyTypeOf(cit.key());
                    gridQty[gk] += cit.value();
                    gridSkuCnt[gk] += 1;
                    if (!gridType.contains(gk)) gridType.insert(gk, tp);
                }
                continue;
            }
            for (auto pit = e.planQtyPerGrid.constBegin(); pit != e.planQtyPerGrid.constEnd(); ++pit)
            {
                if (pit.value() <= 0) continue;
                gridQty[pit.key()] += pit.value();
                gridSkuCnt[pit.key()] += 1;
                if (!gridType.contains(pit.key()))
                    gridType.insert(pit.key(), e.gridTypePerGrid.value(pit.key(), e.gridType));
            }
        }
    }
    if (gridQty.isEmpty())
    {
        HTTP_LOG_INFO("计划格口预检 order=%s 无分格口计划数据，跳过", orderCode.toLocal8Bit().data());
        return;
    }

    // 2) 当前绑定（内存绑定表，key 统一为内部 3 位 key）
    QMap<QString, QString> binds;
    {
        std::lock_guard<std::mutex> lockBind(m_containerMutex);
        for (auto it = m_containerBindings.constBegin(); it != m_containerBindings.constEnd(); ++it)
        {
            const QString k = normalizeGridKey(it.key());
            if (!k.isEmpty() && !it.value().isEmpty() && !binds.contains(k))
                binds.insert(k, it.value());
        }
    }

    auto typeName = [](const QString& t) -> QString {
        if (t == "1") return QString::fromUtf8("异常");
        if (t == "2") return QString::fromUtf8("发货");
        return QString::fromUtf8("分类");
    };

    // 3) 逐格口核对
    //   ★ 2026-09-26：**不再打印"计划单元明细(格口|类型:件数)"** —— 该明细会进入界面运行日志，
    //     现场不需要（单元口径的计划/已落/余量请在「计划分配表」页查看，或看分配表巡检/异常表留痕）。
    int planGrids = 0, unbound = 0, disabled = 0, excSkipped = 0, locked = 0;
    QStringList unboundDesc, disabledDesc, typeSummary, lockedDesc;
    for (auto it = gridQty.constBegin(); it != gridQty.constEnd(); ++it)
    {
        const QString g = it.key();
        ++planGrids;
        const bool bIsExc = isExceptionGridKey(g);
        if (bIsExc) { ++excSkipped; continue; }   // 异常口不在计划内（planAllocOf 已剔除），此处跳过

        const QString t = typeName(gridType.value(g));
        bool okG = false;
        const int gInt = g.toInt(&okG);
        if (okG && gInt > 0 && m_pPlcMgr && m_pPlcMgr->isGridDisabled(gInt))
        {
            ++disabled;
            if (disabledDesc.size() < 20)
                disabledDesc << QString("%1(%2,%3件)").arg(g).arg(t).arg(it.value());
        }
        // ★ 2026-09-26 现场口径：**锁格（含锁格状态未知）的计划格口也不收件** —— 开工前单独列出，
        //   让现场知道"这个格口的计划件落不进去，原因是 PLC 还锁着（满箱未换箱/未复位），不是没绑容器"。
        if (okG && gInt > 0 && m_pPlcMgr
            && (!m_pPlcMgr->isLockStateKnown() || m_pPlcMgr->isGridLocked(gInt)))
        {
            ++locked;
            if (lockedDesc.size() < 20)
                lockedDesc << QString("%1(%2,%3件)").arg(g).arg(t).arg(it.value());
        }
        if (!binds.contains(g))
        {
            ++unbound;
            if (unboundDesc.size() < 20)
                unboundDesc << QString("%1(%2,%3件)").arg(g).arg(t).arg(it.value());
        }
    }

    // 4) 类型分布摘要（证明"分类口 + 发货口"各自的计划都被保留）
    {
        QMap<QString, int> cntByType;
        for (auto it = gridQty.constBegin(); it != gridQty.constEnd(); ++it)
            cntByType[typeName(gridType.value(it.key()))] += 1;
        for (auto it = cntByType.constBegin(); it != cntByType.constEnd(); ++it)
            typeSummary << QString("%1格口%2个").arg(it.key()).arg(it.value());
    }

    HTTP_LOG_INFO("计划格口预检 order=%s 计划格口=%d（%s）已绑定=%d 未绑定=%d 已禁用=%d 锁格中=%d 异常口跳过=%d",
        orderCode.toLocal8Bit().data(), planGrids, typeSummary.join("+").toLocal8Bit().data(),
        planGrids - unbound, unbound, disabled, locked, excSkipped);

    if (locked > 0)
    {
        // ★ 2026-09-26 现场口径：锁格（含锁格状态未知）的计划格口一律不收件 ——
        //   现场动作 = 完成换箱并让 PLC 复位锁格位（下降沿）；额度保留在原单元，解锁后自动恢复分配。
        HTTP_LOG_WARN("计划格口预检 锁格中的计划格口 %d 个：%s —— 这些格口不收件（等 PLC 解锁）；"
                      "件改用同 SKU 其它格口自身额度，都没有则改投异常口或不下发；额度保留在原格口",
            locked, lockedDesc.join(" ").toLocal8Bit().data());
        emit logMessage(QString::fromUtf8(
            "[计划预检] 有 %1 个计划格口正处于「满箱锁格」（PLC 锁格位=1）：%2 —— 这些格口**不收件**"
            "（锁格不落件，无论什么情况）；请完成换箱让 PLC 复位锁格位，解锁后自动恢复分配。"
            "额度保留在原格口，件改用同 SKU 其它格口自身额度、都没有则改投异常口%3")
            .arg(locked).arg(lockedDesc.join(" "))
            .arg(ConfigManager::instance()->config().exceptionGrid), true);
    }
    if (unbound > 0)
    {
        // ★ 2026-09-20 现场问题④：未绑定容器的格口不再"照发落件"，而是**下发前就被拦下**
        //   （改投异常口、不计已分拣、不消耗计划额度）。因此这里的告警必须写明后果与动作，
        //   否则现场会以为"照旧能落、只是不进箱"。
        HTTP_LOG_WARN("计划格口预检 未绑定容器的计划格口 %d 个：%s%s —— 件不会被下发到这些格口（改投异常口），请先下发 H6 绑定",
            unbound, unboundDesc.join(" ") .toLocal8Bit().data(),
            unbound > 20 ? " …（其余见绑定表）" : "");
        emit logMessage(QString::fromUtf8(
            "[计划预检] 有 %1 个计划格口尚未绑定容器：%2 —— 这些格口的计划件**不再落格**（判定期即拦下并改投异常口%3），"
            "请先让 WMS 下发容器绑定（H6），绑定后额度仍在、自动恢复分配")
            .arg(unbound).arg(unboundDesc.join(" "))
            .arg(ConfigManager::instance()->config().exceptionGrid), true);
    }
    if (disabled > 0)
    {
        // ★ 2026-09-26：缺口搬迁能力已删除（恒不搬迁）—— 文案固定按"额度留在原单元"表述。
        HTTP_LOG_WARN("计划格口预检 已禁用(满箱未重绑)的计划格口 %d 个：%s —— %s",
            disabled, disabledDesc.join(" ").toLocal8Bit().data(),
            "该格口不再收件（件改用同 SKU 其它格口自身额度，都没有则改投异常口66或不发指令）；"
            "额度保留在原格口，换箱重绑后自动恢复");
        emit logMessage(QString::fromUtf8(
            "[计划预检] 有 %1 个计划格口处于满箱未重绑(禁用)状态：%2 —— "
            "该格口不再收件：件改用同 SKU 其它格口自身额度，都没有则改投异常口或不发指令；"
            "额度保留在原格口（**不搬迁**），换箱并重绑容器后自动恢复分配")
            .arg(disabled).arg(disabledDesc.join(" ")), true);
    }

    // ════════════════════════════════════════════════════════════════════════
    // ★ 2026-09-14 分配表口径预检（落格结构优化新增三项）
    //   ① Σ每格口计划件数 与 H4 orderQty 是否一致（不一致说明计划本身有问题）
    //   ② 已禁用计划格口按 SKU 列出件数缺口（并确认会转给谁）
    //   ③ 无可用计划格口的 SKU 清单（这些 SKU 的件将全部改投异常口）
    //   只告警不阻塞（除非 allocRequirePlanValid=true —— 由调用方决定是否拒绝开工）
    //   ★ 2026-09-20 现场问题④：③ 的"可用"判据与发送侧口径对齐 ——
    //     增加"已解锁且未绑定容器"（这次整改后同样下发不出去）。
    //     ★ 掩码必须在 m_allocMutex **之外**先算好（allocBlockedMask 内部要取绑定表锁）。
    // ════════════════════════════════════════════════════════════════════════
    if (m_allocValid.load())
    {
        PlanStats st;
        QStringList noneAvailSkus;
        int noneAvailCnt = 0;
        const PlanAllocTable::GridMask blockedMask = allocBlockedMask();   // ★ 锁外快照
        {
            std::lock_guard<std::mutex> lk(m_allocMutex);
            st = m_alloc.stats();
            if (m_pPlcMgr)
            {
                const int skuN = m_alloc.skuCount();
                for (int si = 0; si < skuN; ++si)
                {
                    const QString sku = m_alloc.skuNameAt(si);
                    if (sku.isEmpty()) continue;
                    const QVector<qint16> gs = m_alloc.gridsOf(sku);
                    bool anyAvail = false;
                    for (qint16 g : gs)
                    {
                        if (m_pPlcMgr->isGridDisabled(g) || m_pPlcMgr->isGridLocked(g)) continue;
                        // ★ 已解锁且未绑定容器 → 同样不可下发（本次整改口径）
                        if (g >= 0 && g < (int)blockedMask.size() && blockedMask[(size_t)g]) continue;
                        anyAvail = true;
                        break;
                    }
                    if (!anyAvail)
                    {
                        ++noneAvailCnt;
                        if (noneAvailSkus.size() < 20) noneAvailSkus << sku;
                    }
                }
            }
        }

        const int waveQty = m_pWaveMgr ? m_pWaveMgr->orderQty() : 0;
        emit logMessage(QString::fromUtf8(
            "[计划预检] 分配表：计划单元%1 个｜计划件数合计%2｜多格口SKU %3 个%4")
            .arg(st.cellCount).arg(st.planTotal).arg(st.multiSkuCnt)
            .arg(waveQty > 0 && st.planTotal != waveQty
                     ? QString::fromUtf8("｜⚠Σ计划(%1)≠orderQty(%2)，差%3")
                           .arg(st.planTotal).arg(waveQty).arg(st.planTotal - waveQty)
                     : QString()),
            waveQty > 0 && st.planTotal != waveQty);

        if (noneAvailCnt > 0)
        {
            HTTP_LOG_WARN("计划格口预检 无可用计划单元的 SKU %d 个：%s —— 这些 SKU 的件将全部改投异常口或不发指令",
                noneAvailCnt, noneAvailSkus.join(" ").toLocal8Bit().data());
            emit logMessage(QString::fromUtf8(
                "[计划预检] 有 %1 个产品的计划单元全部不可用（锁格/禁用/未绑定容器）：%2 —— 这些产品的件将"
                "改投异常口%3或不发指令；请先处理格口（完成换箱解锁 / 绑定容器 / 重绑）")
                .arg(noneAvailCnt).arg(noneAvailSkus.join(" "))
                .arg(ConfigManager::instance()->config().exceptionGrid), true);
        }
    }
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

            // ★ 2026-09-17 归属补齐/纠偏（**必须在 restoreBindsIfEmpty 之前**）：
            //   H6 报文不含波次号，归属只能靠"内存当前波次"猜 → 两类现场故障（都表现为切回取不到绑定）：
            //     ① H6 早于 H4 → 归属空串；② 切出窗口内的 H6 被记到刚切出的上一个波次。
            //   H4 到达（本波次落库完成）时，"自上次波次以来收到的绑定"就是本次投递的绑定 → 此刻改判最准。
            //   补齐后本波次已有自己的绑定行，下面的 restoreBindsIfEmpty（借显示）自然少触发。
            attributePendingBindsToWave(orderCode);

            // ★ 2026-09-07 绑定沿用：无任何 active 绑定（如上一波次完结已归档）时，恢复最近绑定
            restoreBindsIfEmpty(orderCode);

            // ★ 2026-09-13 自动化验证钩子（仅当进程环境变量 WCS_E2E_AUTOSORT=1 时生效）：
            //   无人值守跑 mock/E2E 时无法人工点「开始分拣」，此处自动执行一次同样的动作。
            //   生产默认不设置该环境变量 → 行为与以往完全一致（仍需人工点击）。
            if (qEnvironmentVariableIsSet("WCS_E2E_AUTOSORT") ||
                qEnvironmentVariableIsSet("WCS_E2E_AUTOSORT_ANY"))   // ANY=任意路径均自动开工（含"成为当前波次"）
            {
                if (m_pWaveMgr->startSorting())
                {
                    HTTP_LOG_WARN("测试钩子：自动开工(WCS_E2E_AUTOSORT=1) BOUND→SORTING orderCode=%s",
                        orderCode.toLocal8Bit().data());
                    emit logMessage(QString("[测试钩子] 自动开工（E2E）：orderCode=%1 已进入分拣中").arg(orderCode));
                    precheckPlanGridBindings(orderCode);   // ★ 2026-09-14 计划格口 vs 容器绑定预检
                }
            }
        }
        // ★ 2026-09-14 自动化验证钩子（补）：待执行波次被"自动开始"接替为当前波次时，
        //   其推进路径与上面不同（不经由 CREATED→BOUND 的那条分支），
        //   为让"挂起件补发"可在无人值守下验证，这里再补一次。
        //   仅当环境变量 WCS_E2E_AUTOSORT_ANY=1 时生效；生产不设置 → 行为与以往完全一致。
        if (qEnvironmentVariableIsSet("WCS_E2E_AUTOSORT_ANY") && m_pWaveMgr &&
            m_pWaveMgr->orderCode() == orderCode && m_pWaveMgr->status() == WAVE_BOUND)
        {
            if (m_pWaveMgr->startSorting())
            {
                HTTP_LOG_WARN("测试钩子：自动开工(WCS_E2E_AUTOSORT_ANY=1) BOUND→SORTING orderCode=%s",
                    orderCode.toLocal8Bit().data());
                emit logMessage(QString("[测试钩子] 自动开工（E2E/ANY）：orderCode=%1 已进入分拣中").arg(orderCode));
                precheckPlanGridBindings(orderCode);
            }
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
    clearBoxLandedCount();   // ★ 2026-09-13 计划数封顶计数随新波次清空
    clearGridLandedCount();  // ★ 2026-09-14 按格口落格计数（多格口分配依据）随新波次清空
    clearLandingDedup();     // ★ 2026-09-14 落格明细去重集合随新波次清空
    // ★ 2026-09-14 计划分配表：本回调在 waveParsed 之后异步到达，此时分配表**刚为该波次编译好**，
    //   因此只有"表属于别的波次"（同波次重下发/未走解析链）时才清，避免把当前波次的计划误清空。
    if (m_allocOrderCode != orderCode)
    {
        reportPlanAlloc(QString::fromUtf8("新波次落库完成"));
        clearPlanAllocTable(QString::fromUtf8("新波次(表属于旧波次)"));
    }
    // ★ 新波次到来，清空旧波次相关数据（EpcCache 保留，RFID 推送独立于波次生命周期）
    // ★ 纠正5: 新波次开始时恢复所有禁用格口
    if (m_pPlcMgr)
        m_pPlcMgr->enableAllGrids();
    // ★ 新波次开始时清空 SKU 查询防重标记和重试计数
    m_pendingSkuQuery.clear();
    m_skuQueryRetryCount.clear();
    m_notReadyRetryCount.clear();
    clearAllEpcRuntimeState();   // ★ 2026-09-11 在途/冷却/重发计数一并清空
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

        // ★ 2026-09-17 可选：H6 报文自带波次号（协议增强，WMS 不改也能跑）
        //   背景：H6 本身不含波次号，归属只能靠"内存当前波次"猜，才会出现
        //   "H6 早于 H4 归属落空""切出窗口内 H6 记到上一个波次"两类现场故障。
        //   若 WMS 后续在 H6 上带上波次号（orderCode/order_code/waveCode，query 或 body 均可），
        //   本系统**直接采用报文自带的归属**，不再依赖任何推断。
        QString bodyOrderCode = q.queryItemValue("orderCode").trimmed();
        if (bodyOrderCode.isEmpty()) bodyOrderCode = q.queryItemValue("order_code").trimmed();
        if (bodyOrderCode.isEmpty()) bodyOrderCode = q.queryItemValue("waveCode").trimmed();

        // 如果 queryString 为空，尝试从 Body JSON 解析
        if (latticehole.isEmpty() || boxcode.isEmpty() || bodyOrderCode.isEmpty())
        {
            QJsonDocument d = QJsonDocument::fromJson(st.body);
            QJsonObject obj = d.object();
            if (latticehole.isEmpty())   latticehole = obj["latticehole"].toString().trimmed();
            if (boxcode.isEmpty())       boxcode     = obj["boxcode"].toString().trimmed();
            if (bodyOrderCode.isEmpty()) bodyOrderCode = obj["orderCode"].toString().trimmed();
            if (bodyOrderCode.isEmpty()) bodyOrderCode = obj["order_code"].toString().trimmed();
            if (bodyOrderCode.isEmpty()) bodyOrderCode = obj["waveCode"].toString().trimmed();
        }

        QJsonObject result = handleBindingLatticePort(latticehole, boxcode);
        sendJsonResponse(pSender, dwConnID, result);

        // ★ 异步持久化到数据库（HTTP 响应已返回，不阻塞 WMS 等待）
        //   bindGridBox 内部有 QMutexLocker 保护，线程安全
        if (m_pSortingDb && !latticehole.isEmpty() && !boxcode.isEmpty())
        {
            // ★ 2026-09-07 WMS 格口编码兼容：latticehole 可带前缀编码（如 "22005" = 格口号5）
            int gridNum = parseWmsGridCodeToInt(latticehole);
            if (gridNum >= 1 && gridNum <= BINDING_SLOT_COUNT)
            {
                QString normalizedGrid = QString("%1").arg(gridNum, GRID_KEY_PADDING, 10, QChar('0'));
                // ★ 2026-09-06 绑定关联所属波次（提交时刻快照），供波次切换恢复格口绑定视图
                //   ★ 2026-09-15 内存波次为空但处于"刚切出"窗口时，仍归入刚切出的波次：
                //     否则该 H6 绑定落到空波次 → 该波次的绑定归属丢失 → 切回时按波次取不回绑定，
                //     现场表现为"切走再切回，格口绑定被清理了、恢复不了、无法分拣"。
                //   ★ 2026-09-17 现场取证（docs/格口绑定波次归属_根因与修复_20260917.md §二）：
                //     ① 该窗口**确实救回过真实数据**（09-15 23:53 有 66 条 H6 靠它落库）→ 保留；
                //     ② 但它也会把"下一个波次的 H6"记到刚切出的上一个波次（同批 66 条 H6 实为
                //        456456 的绑定，却挂到 6565656 名下）→ 因此在 H4 到达时做**归属纠偏**
                //        （attributePendingBindsToWave，见 onWavePersistenceFinished）。
                //   归属优先级：H6 报文自带波次号 > 内存当前波次 > 切出窗口内的刚切出波次 > 空串（待补齐）
                QString bindOrderCode;
                QString bindSrc;
                if (!bodyOrderCode.isEmpty())
                {
                    bindOrderCode = bodyOrderCode;
                    bindSrc = QString::fromUtf8("报文自带波次号");
                }
                else if (m_pWaveMgr && !m_pWaveMgr->orderCode().isEmpty())
                {
                    bindOrderCode = m_pWaveMgr->orderCode();
                    bindSrc = QString::fromUtf8("内存当前波次");
                }
                else if (!m_switchingOutOrderCode.isEmpty())
                {
                    bindOrderCode = m_switchingOutOrderCode;
                    bindSrc = QString::fromUtf8("切出窗口（待 H4 到达时纠偏）");
                    HTTP_LOG_WARN("BindingLatticePort 内存波次已切出，H6 绑定暂归入刚切出的波次 order=%s grid=%s box=%s"
                                  "（若随后到达的 H4 不是该波次，将在 H4 落库时纠偏）",
                        bindOrderCode.toLocal8Bit().data(), normalizedGrid.toLocal8Bit().data(),
                        boxcode.toLocal8Bit().data());
                }
                else
                {
                    // ★ 2026-09-16：归属确实落空（内存无波次、且不在切出窗口内 —— 例如开机后
                    //   WMS 还没下发 H4 就直接送 H6）。按口径**不猜测归属**：该行以 order_code=''
                    //   保留（可作"当前活跃绑定"显示/回溯），H4 到达时由归属补齐改判给该波次。
                    bindSrc = QString::fromUtf8("归属待补齐（H4 到达时改判）");
                    HTTP_LOG_WARN("BindingLatticePort H6 归属波次为空（内存无波次且不在切出窗口内）"
                                  "grid=%s box=%s —— 该行按 order_code='' 保留，待 H4 到达时归属补齐",
                        normalizedGrid.toLocal8Bit().data(), boxcode.toLocal8Bit().data());
                }
                HTTP_LOG_INFO("BindingLatticePort 提交数据库写入 grid=%s box=%s order=%s 归属来源=%s",
                    normalizedGrid.toLocal8Bit().data(), boxcode.toLocal8Bit().data(),
                    bindOrderCode.toLocal8Bit().data(), bindSrc.toLocal8Bit().data());

                if (m_pBusinessPool)
                {
                    m_pBusinessPool->commitNoWait([this, normalizedGrid, boxcode, bindOrderCode]() {
                        HTTP_LOG_INFO("BindingLatticePort 数据库写入开始 grid=%s box=%s order=%s",
                            normalizedGrid.toLocal8Bit().data(), boxcode.toLocal8Bit().data(), bindOrderCode.toLocal8Bit().data());
                        bool ok = m_pSortingDb->bindGridBox(normalizedGrid, boxcode, bindOrderCode);
                        if (ok)
                            HTTP_LOG_INFO("BindingLatticePort 数据库写入成功 grid=%s box=%s", normalizedGrid.toLocal8Bit().data(), boxcode.toLocal8Bit().data());
                        else
                        {
                            HTTP_LOG_ERROR("BindingLatticePort 数据库写入失败 grid=%s box=%s (详见DataBase日志)", normalizedGrid.toLocal8Bit().data(), boxcode.toLocal8Bit().data());
                            // ★ 2026-09-17 失败必须让现场看见：此前只写 data.log，
                            //   表现为"面板显示已绑定、库里一行都没有"（切回该波次无绑定可恢复）
                            emit bindPersistFailed(normalizedGrid, boxcode, bindOrderCode);
                        }
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
                    {
                        HTTP_LOG_ERROR("BindingLatticePort 同步写入失败 grid=%s box=%s", normalizedGrid.toLocal8Bit().data(), boxcode.toLocal8Bit().data());
                        emit bindPersistFailed(normalizedGrid, boxcode, bindOrderCode);
                    }
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

    // ──── 步骤2: 格口号范围校验（★ 2026-09-07 兼容 WMS 前缀编码 "22005" = 格口号5）────
    int gridNum = parseWmsGridCodeToInt(latticehole);
    if (gridNum < 1 || gridNum > BINDING_SLOT_COUNT)
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

    // ──── ★ 2026-09-14 异常口绑定：允许（原 09-13 的"拒绝绑定"已撤销）────
    //   为什么必须允许：异常口 66 是超计划件的物理落点，现场要在此处放一个容器收件、人工清出；
    //   若拒绝绑定（H6 被回 500），该口无容器 → 件落在异常口但没有箱号可查，人工无从对账。
    //   防污染不靠"拒绝绑定"，而靠"异常口的件不进 H7"：
    //     · 超计划件由本系统按 exceptionGrid 主动改投该口，并在落格反馈最前面被识别（见
    //       "物理异常口反馈识别"），不计已分拣、不写箱内明细、不进 H7 报文；
    //     · 该口本身不进入任何 SKU 的计划（planAllocOf 会把计划里出现的异常格口剔除）。
    {
        const QString excGridBind = ConfigManager::instance()->config().exceptionGrid.trimmed();
        if (!excGridBind.isEmpty() && excGridBind != "0" &&
            normalizeGridKey(QString::number(gridNum)) == normalizeGridKey(excGridBind))
        {
            HTTP_LOG_INFO("BindingLatticePort 异常口绑定 latticehole=%s box=%s（exceptionGrid=%s）"
                          "—— 允许绑定用于收集超计划件；该口落格件不入 H7 报文、需人工清出",
                latticehole.toLocal8Bit().data(), boxcode.toLocal8Bit().data(),
                excGridBind.toLocal8Bit().data());
            emit logMessage(QString::fromUtf8(
                "[容器绑定] 格口%1 是异常口/强排口，已绑定容器%2（该口只收超计划件，需人工清出、不上传WMS）")
                .arg(QString("%1").arg(gridNum, GRID_KEY_PADDING, 10, QChar('0'))).arg(boxcode));
        }
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
            // ★ 2026-09-09（客户要求）：覆盖旧绑定前留痕（格口号/旧箱/新箱/波次/原因）
            HTTP_LOG_INFO("解绑留痕 grid=%s 旧箱=%s 新箱=%s order=%s 原因=WMS重发H6换箱 动作=覆盖内存绑定（旧绑定DB归档）",
                normalizedGrid.toLocal8Bit().data(),
                it.value().toLocal8Bit().data(),
                boxcode.toLocal8Bit().data(),
                orderCode.toLocal8Bit().data());
            LOG_INFO("[解绑留痕] grid=%s 旧箱=%s 新箱=%s order=%s 原因=H6换箱",
                normalizedGrid.toLocal8Bit().data(),
                it.value().toLocal8Bit().data(),
                boxcode.toLocal8Bit().data(),
                orderCode.toLocal8Bit().data());
            HTTP_LOG_INFO("BindingLatticePort 切箱 latticehole=%s old=%s new=%s",
                normalizedGrid.toLocal8Bit().data(),
                it.value().toLocal8Bit().data(),
                boxcode.toLocal8Bit().data());
            emit logMessage(QString("[容器绑定] 格口%1 切箱: %2 → %3")
                .arg(normalizedGrid).arg(it.value()).arg(boxcode));
        }

        m_containerBindings[normalizedGrid] = boxcode;
        // ★ 2026-09-22 现场需求（第2条）：新容器到场 → 清掉该格"离场箱号快照"，
        //   门禁 hasBoundContainer() 与落格写库立即恢复按"已绑定"处理。
        if (m_boxSnapshot.remove(normalizedGrid) > 0)
            HTTP_LOG_INFO("[容器到场] grid=%s box=%s 原因=H6绑定（离场快照已清除：绑定层面已恢复；"
                          "能否下发另见锁格/禁用判据，★2026-09-26 起 PLC 仍锁格时暂不收件）",
                normalizedGrid.toLocal8Bit().data(), boxcode.toLocal8Bit().data());
    }

    // ★ 纠正5: H6 绑定时恢复格口（满箱锁格后WMS重新绑定，格口恢复正常分拣）
    // ★ 2026-09-26 现场口径（B 方案）：「H6 到场」≠「马上就能收件」——
    //   PLC 若仍处于满箱锁格（S7 锁格位=1，即换箱动作还没完成/PLC 尚未复位），
    //   **暂不解除该格禁用**，把"恢复收件"推迟到解锁边沿（`gridUnlocked`）到达时再做。
    //   现场依据（08:26 实例）：H6 在 55.023 到、锁格位到 56.497 才复位 —— 原实现"H6 立即恢复"
    //   使这 ≈1.5 秒窗口内件照落该格（下发判据对"锁格"是照发：PlcManager::isGridDispatchable）。
    //   窗口内的效果：该格仍属"禁用"⇒ 不进候选（件优先用同 SKU 其它格口的剩余额度；
    //   都没有则按既有规则"不发指令"+异常表留痕），额度仍保留在该格口、解锁后自动恢复。
    if (m_pPlcMgr)
    {
        if (m_pPlcMgr->isGridLocked(gridNum))
        {
            HTTP_LOG_WARN("H6绑定 格口%d 已绑新箱 box=%s，但 PLC 仍为满箱锁格（锁格位=1）—— "
                          "暂不恢复收件，等解锁边沿（下降沿）到达后自动恢复（本窗口该格不收件）",
                gridNum, boxcode.toLocal8Bit().data());
            emit logMessage(QString::fromUtf8(
                "[容器绑定] 格口%1 已绑新箱 %2，但 PLC 仍处「满箱锁格」（换箱动作未结束）—— "
                "解锁后自动恢复收件；这期间该格不收件，件改用其它格口额度或按既有规则处置")
                .arg(gridNum).arg(boxcode), true);
        }
        else
        {
            m_pPlcMgr->enableGrid(gridNum);
            HTTP_LOG_INFO("H6绑定 格口恢复 grid=%d box=%s", gridNum, boxcode.toLocal8Bit().data());
        }
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

    // ★ 2026-09-08：波次已进入完结流程（ENDING 且已存在 H8 报文，含回传失败待重传）时，
    //   按"已终态"幂等处理（不再允许取消，也不报 500），避免 WMS 侧因状态语义变化出现新报错
    auto isEndingWithH8 = [this](const QString& oc) -> bool {
        if (!m_pSortingDb || !m_pSortingDb->isOpen() || oc.isEmpty()) return false;
        return !m_pSortingDb->getOutboxEndByOrder(oc).isEmpty();
    };

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
            if (dbStatus == WAVE_CANCELLED || dbStatus == WAVE_FINISHED ||
                (dbStatus == WAVE_ENDING && isEndingWithH8(orderCode)))   // ★ 2026-09-08 完结流程按终态幂等
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
    // ★ 2026-09-08：波次已进入完结流程（ENDING 且有 H8 报文）同样按终态幂等返回
    if (currentStatus == WAVE_CANCELLED || currentStatus == WAVE_FINISHED ||
        (currentStatus == WAVE_ENDING && isEndingWithH8(orderCode)))
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
        // ★ 2026-09-14 自动化验证钩子（仅 WCS_E2E_ALLOW_CANCEL_SORTING=1 时生效）：
        //   生产语义不变（分拣中不可取消）。无人值守的 E2E 需要一个"结束当前波次"的动作，
        //   才能验证"接替下一波次 → 挂起件补发"这条链路；这里只放行**取消**，
        //   其余流程与人工取消完全一致。
        if (!qEnvironmentVariableIsSet("WCS_E2E_ALLOW_CANCEL_SORTING"))
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
        HTTP_LOG_WARN("测试钩子：放行『取消分拣中的波次』(WCS_E2E_ALLOW_CANCEL_SORTING=1) "
                      "orderCode=%s status=%d sorted=%d —— 仅用于无人值守 E2E，生产不得开启",
            orderCode.toLocal8Bit().data(), m_pWaveMgr->status(), m_pWaveMgr->sorted());
        // ★ 必须真正完成状态迁移，否则后面的清理/接替链路不会执行（此前只"放行"是错的）
        if (!m_pWaveMgr->forceCancelForE2E())
        {
            QJsonObject r;
            r["code"] = "500";
            r["message"] = "E2E 强制取消失败（状态迁移被拒）";
            r["cancellable"] = false;
            return r;
        }
    }

    // ──── 步骤5: 取消后清理（T-S3-02）────
    // tryCancelWave() 已原子完成状态迁移，此处执行清理操作
    // 清理容器绑定（内存清空 + DB 归档留史，历史行保留可追溯/可沿用）、分拣记录、GridBuffer、WaveManager

    // 清理容器绑定（内存 + 数据库归档：★ 2026-09-07 清空当前绑定但历史记录保留在 DB）
    {
        std::lock_guard<std::mutex> lock(m_containerMutex);
        int count = m_containerBindings.size();
        // ★ 2026-09-09（客户要求）：清理前逐格留痕（格口号/旧箱号/波次/原因）
        for (auto it = m_containerBindings.constBegin(); it != m_containerBindings.constEnd(); ++it)
        {
            HTTP_LOG_INFO("解绑留痕 grid=%s 旧箱=%s order=%s 原因=波次取消(H5)清理 动作=DB归档+内存清空",
                it.key().toLocal8Bit().data(), it.value().toLocal8Bit().data(),
                orderCode.toLocal8Bit().data());
            LOG_INFO("[解绑留痕] grid=%s 旧箱=%s order=%s 原因=波次取消",
                it.key().toLocal8Bit().data(), it.value().toLocal8Bit().data(),
                orderCode.toLocal8Bit().data());
        }
        m_containerBindings.clear();
        m_boxSnapshot.clear();   // ★ 2026-09-22 离场箱号快照随绑定一并清空
        HTTP_LOG_INFO("CancelWave 已清空容器绑定(内存) count=%d orderCode=%s（DB历史已归档留档）",
            count, orderCode.toLocal8Bit().data());
    }
    if (m_pSortingDb)
        m_pSortingDb->archiveAllBinds();
    emit bindingUpdated();

    // ★ 2026-09-22 完结回传（固定延迟发送）：波次被取消（H5）→ 取消尚未到点的延迟发送
    //   （已发出的 H7/H8 报文仍在 Outbox，可由面板补发）
    cancelPendingEndReport(QString::fromUtf8("波次取消(H5)"));

    // 清理格口分拣记录
    {
        std::lock_guard<std::mutex> lock(m_gridRecordMutex);
        m_gridSortRecords.clear();
    }

    // ★★ 2026-09-14 顺序修正（数据完整性）：先重置 WaveManager（→ 可能自动开始待执行波次），
    //   再清 GridBuffer。
    //
    //   为什么必须这个顺序：
    //     本函数前面的 `tryCancelWave()` 已把状态原子迁移到 CANCELLED 并发出 `waveStatusChanged`，
    //     该回调里会调用 `maybeStartPendingWave()` —— 也就是**把排队中的下一波次交给
    //     ParseWorker 重放解析**（异步线程）。
    //     （`clearWave()`（→IDLE）本身**不**触发出队：★ 2026-09-17 起只有"接收中且本次会话
    //       未点过「结束任务」"的终态才出队，口径见 PendingWaveQueuePolicy.h。）
    //     若先清 GridBuffer，则下一波次解析完成后、其落库回调 `buildWaveItems()` 去读
    //     `m_pBuffer->activeMap()` 时映射已被清空 → **落库 items=0** →
    //     该波次在 DB 里没有任何明细 → 之后"恢复波次"会因明细为空被拒绝（只能重下发）。
    //     实测（test/e2e_pending_replay.py）曾复现 `波次明细写入成功 … count=0`。
    //
    //   先重置再清空后：下一波次的解析结果在本次清空之后写入缓冲，
    //   落库读到的就是它自己的映射；本次清空只影响"已取消的旧波次"。
    // ★ 重置 WaveManager 为 IDLE（含清空 orderCode），使 H4/H6 不再校验旧波次
    if (m_pWaveMgr)
    {
        m_pWaveMgr->clearWave();
        HTTP_LOG_INFO("CancelWave 已重置WaveManager orderCode=%s", orderCode.toLocal8Bit().data());
    }

    // ★ 清理 GridBuffer（双缓冲置空），避免 H6 绑定校验旧波次数据
    if (m_pBuffer)
    {
        m_pBuffer->prepareSwap(nullptr);
        HTTP_LOG_INFO("CancelWave 已清理GridBuffer orderCode=%s", orderCode.toLocal8Bit().data());
    }

    // ★ 2026-09-14 计划分配表：取消波次同样出报告并清表（与"切出/新波次/完结"同一批清零点）
    //   报告留在日志里，便于事后核对"取消时各格口的计划/实际"；在途认领随表清空（有残留会 WARN）
    reportPlanAlloc(QString::fromUtf8("波次取消"));
    clearPlanAllocTable(QString::fromUtf8("波次取消"));

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
        // ★ 2026-09-07 WMS 格口编码兼容：gridNum 可带前缀编码（如 "22005" = 格口号5），解析为内部号再校验
        {
            int gNum = parseWmsGridCodeToInt(gridNum);
            if (gNum >= 1 && gNum <= BINDING_SLOT_COUNT)
                gridNum = QString::number(gNum);   // 归一为内部格口号（后续逻辑均用内部号）
            else
            {
                HTTP_WARN("InsertWaveInfo 格口号无效或越界 orderCode=%s items[%d] gridNum=%s range=1..%d",
                    orderCode.toLocal8Bit().data(), i, gridNum.toLocal8Bit().data(), BINDING_SLOT_COUNT);
                emit logMessage(QString("[WMS] 格口号无效或越界 items[%1] gridNum=%2 (有效范围: 1~%3)，已记录异常")
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
    // ★ 2026-09-07 周期（60s）把当日峰值效率落库（断电/崩溃兜底，最终值见退出/跨日结转）
    persistDailyPeak();

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

    // ★ 2026-09-13 性能核验：把"实时面板是否影响分拣主流程"变成可读数字
    //   rfid→plc  = 收到 RFID 推送 → 下发 PLC 指令的耗时（现场硬窗口 PLC_SEND_TIMEOUT_MS = 1000ms）
    //   fb延迟    = 下发 → 落格反馈（含设备处理时间，非软件瓶颈但可用于异常排查）
    //   eventLag  = RFID 帧解析 → 业务入口（GUI 线程被 UI 绘制拖慢时此值会明显上升）
    {
        const PerfSnapshot ps = perfSnapshot();
        const int waveSorted = m_pWaveMgr ? m_pWaveMgr->sorted() : 0;
        const int waveExc    = m_pWaveMgr ? m_pWaveMgr->exception() : 0;   // = 界面「处理」/「异常口」
        HTTP_INFO("[性能] rfid→plc p50=%d p95=%d p99=%d ms(n=%d) | 反馈时延 p95=%d ms | 主线程事件滞后 p95=%d ms "
                  "| RFID扫描累计=%llu | 波次已分拣=%d 处理/异常口=%d",
            ps.rfidToPlcP50, ps.rfidToPlcP95, ps.rfidToPlcP99, ps.samples,
            ps.fbLatencyP95, ps.eventLagP95,
            (unsigned long long)m_rfidPushTotal.load(std::memory_order_relaxed),
            waveSorted, waveExc);
        // 超阈值告警：p99 接近 1s 硬窗口说明主线程已被拖慢（UI 面板/DB 阻塞等）
        if (ps.rfidToPlcP99 >= PLC_SEND_TIMEOUT_MS)
        {
            HTTP_WARN("[性能] rfid→plc p99=%dms 已达/超过 %dms 硬窗口，主线程存在阻塞（UI面板/日志/DB查询）",
                ps.rfidToPlcP99, PLC_SEND_TIMEOUT_MS);
        }
    }

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

    // ★ 2026-09-26 兜底对账（B 方案配套）：10 秒一次把"已绑容器 + 已解锁 + 仍被禁用"的格口恢复收件。
    //   放在健康检查周期内的理由：① 它是纯状态对账、无外部副作用；② 不依赖任何边沿信号，
    //   所以"掉线期间丢失解锁边沿"或"锁格快照过期"都能自愈；③ 频率足够低（格口卡住只影响收件，
    //   不影响已发出去的指令与报文）。
    reconcileStrandedDisabledGrids();
}

// ============================================================================
// reconcileStrandedDisabledGrids — ★ 2026-09-26 现场口径（B 方案）兜底对账
//
//   背景：H6 绑定到达时若 PLC 仍为满箱锁格（锁格位=1），恢复收件被**推迟**到解锁边沿
//   （见 handleBindingLatticePort 与 gridUnlocked 的处理）。若这次解锁边沿丢失，
//   该格会永远卡在"已绑新箱但仍被禁用"⇒ 不收件、面板橙/红，直到新任务或人工处理。
//   边沿丢失的真实路径：S7 掉线期间现场完成换箱解锁 —— PlcManager 在 s7Connected 里
//   memset 掉"上次锁格状态"，重连后的第一次轮询读到 0 与 0 比较 ⇒ 不产生下降沿。
//
//   本函数用**轮询到的当前值**做对账（不依赖边沿）：满足下面三条才恢复
//     ① 该格仍被禁用（m_disabledGrids）        ② 锁格位=0（PLC 已复位，快照为最新轮询值）
//     ③ 该格已绑定容器（有箱才恢复；没换箱的保持红"已解锁·待重绑"，等 WMS 重发 H6）
//   返回恢复的格口数。调用点：logHealthStatus（每 HEALTH_CHECK_INTERVAL_MS = 10s）。
// ============================================================================
int HttpServer::reconcileStrandedDisabledGrids()
{
    if (!m_pPlcMgr) return 0;

    int restored = 0;
    QStringList grids;
    for (int g = 1; g <= BINDING_SLOT_COUNT; ++g)
    {
        if (!m_pPlcMgr->isGridDisabled(g)) continue;   // 未禁用：无需处理
        if (m_pPlcMgr->isGridLocked(g))    continue;   // 仍锁格：等解锁边沿（正常路径）
        if (!hasBoundContainer(g))         continue;   // 未换箱：保持"已解锁·待重绑"
        m_pPlcMgr->enableGrid(g);
        ++restored;
        grids << QString::number(g);
        HTTP_LOG_WARN("[格口恢复] 兜底对账：格口%d 已绑定容器且锁格位=0，但此前因满箱锁格仍被禁用 "
                      "→ 恢复收件（H6 早于解锁到达；解锁边沿可能因 S7 掉线丢失）", g);
    }
    if (restored > 0)
    {
        emit bindingUpdated();
        emit logMessage(QString::fromUtf8(
            "[格口恢复] 兜底对账：格口[%1] 已解锁且已绑定容器（此前等待解锁边沿）→ 已恢复收件")
            .arg(grids.join(QString::fromUtf8(","))), true);
    }
    return restored;
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

    // ── ★ 2026-09-14 异常口兜底拦截（客户确认：66 号的件不上传 WMS）──
    //   正常路径下 66 号的件在落格反馈最前面就被识别为异常件（不进 m_gridSortRecords），
    //   且 sendFullbox / sendFullboxForGrid / manualFullbox 三处入口都会跳过异常口。
    //   这里再在**报文构建的唯一出口**兜一道：即使将来新增调用路径，异常口也不可能被上传，
    //   避免"实报数超计划 → WMS 回 [2107632] 整条驳回、牵连同批正常件不落账"。
    if (isExceptionGridKey(grid))
    {
        HTTP_LOG_ERROR("满箱回传报文（H7）拦截异常口 grid=%s box=%s 记录=%d —— 异常口件不上传WMS（请人工清出）",
            grid.toLocal8Bit().data(), boxCode.toLocal8Bit().data(), records.size());
        emit logMessage(QString::fromUtf8(
            "[满箱回传] 拦截：格口%1 是异常口，该口件不上传WMS（请人工清出）").arg(normalizeGridKey(grid)), true);
        QJsonObject blocked;
        blocked["blocked"]   = QString::fromUtf8("异常口不上传");
        blocked["orderCode"] = orderCode;
        blocked["grid"]      = normalizeGridKey(grid);
        return blocked;   // 标记报文：调用方识别 blocked 字段后跳过发送（不会产生 H7）
    }

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
    // ★ 2026-09-14 报文层兜底去重：同一 EPC 在本格口只计 1 件
    //   （落格侧已按 (格口,EPC) 去重，此处再兜一道：即使上游出现重复记录，
    //     也不会把同一物理件在 H7 里算成 2 件 → WMS 侧数量对不上而整条驳回）
    QJsonArray detailList;
    QMap<QString, int> skuQtyMap;      // sku → 该格实分件数
    QStringList skuOrder;              // 保持出现顺序
    QSet<QString> seenEpcsInGrid;      // 本格口已计数的 EPC（去重兜底）
    int dupSkipped = 0;
    for (const GridSortRecord& rec : records)
    {
        const QString epcKey = rec.inco.trimmed();
        if (!epcKey.isEmpty())
        {
            if (seenEpcsInGrid.contains(epcKey))
            {
                ++dupSkipped;
                continue;   // 同一 EPC 重复记录 → 本格口该件只算 1 件
            }
            seenEpcsInGrid.insert(epcKey);
        }
        // ★ 2026-09-06 客户确认：sku 字段必须是 SKU 编码（EPC→SKU 绑定查询结果，落格时已固化 rec.sku），
        //   不能是 EPC 码、也不能发"未找到sku"占位串；缺 SKU 的情形已在 sendFullbox 前置整单拦截
        QString sku = rec.sku.isEmpty() ? getSkuByEpc(rec.inco) : rec.sku;
        if (sku.isEmpty())
            continue;   // 防御：前置已整单拦截，正常不会走到
        if (!skuQtyMap.contains(sku))
            skuOrder.append(sku);
        skuQtyMap[sku] += rec.gridCount;   // 每条落格记录=1件
    }
    if (dupSkipped > 0)
        HTTP_LOG_WARN("满箱回传报文（H7） 报文层去重 grid=%s box=%s 跳过同一EPC重复记录=%d 条（本格口每件只计一次）",
            grid.toLocal8Bit().data(), boxCode.toLocal8Bit().data(), dupSkipped);
    for (const QString& sku : skuOrder)
    {
        QJsonObject item;
        // ★ 2026-09-07 WMS 格口编码：num = 格口号编码（前缀22+3位，格口5 → "22005"；内部号仅此处瞬时转换）
        item["num"]            = gridToWmsCode(grid);
        item["targetLocation"] = boxCode.isEmpty() ? cfg.fullboxDefaultTargetLocation : boxCode;  // 目标库位 = 容器号，无容器号时兜底
        item["sku"]            = sku;
        item["qty"]            = QString::number(skuQtyMap.value(sku));
        detailList.append(item);
    }
    head["detailList"] = detailList;

    // ★ 2026-09-13 乙方案：按计划件数裁剪该格口各 SKU 的 qty（先按 SKU 合并、再封顶）
    //   目的：正常路径下超计划件已改投异常口、不会进箱；但"同时两件在线"等情形仍可能让箱内
    //   实落数 > 计划数，若照实上报会被 WMS 判"无法分配"并整条驳回，牵连同批正常件不落账。
    //   裁剪只封顶不补足；被裁掉的多余件仍留在本地记录与「超计划预警」中可查。
    {
        QStringList trimLog;
        const int trimmed = clampFullboxQtyToPlan(grid, detailList, trimLog);
        if (trimmed > 0)
        {
            HTTP_LOG_WARN("满箱回传报文（H7） 按计划件数裁剪 qty grid=%s box=%s 裁剪件数=%d（多余件不进入上传报文）",
                grid.toLocal8Bit().data(), boxCode.toLocal8Bit().data(), trimmed);
            for (const QString& line : trimLog)
                HTTP_LOG_WARN("满箱回传报文（H7） 裁剪明细 %s", line.toLocal8Bit().data());
        }
    }

    // ★ 2026-09-06：满箱总件数日志（Σqty 应=该格实分件数，供对账）
    {
        int totalQty = 0;
        for (const QJsonValue& v : detailList)
            totalQty += v.toObject().value("qty").toString().toInt();
        HTTP_LOG_INFO("满箱回传报文（H7） Σqty=%d 聚合sku行=%d 原始落格记录=%d grid=%s",
            totalQty, detailList.size(), records.size(), grid.toLocal8Bit().data());
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
//   1. 校验波次处于执行态（SORTING/FULLBOX_SYNC；非执行态不发送，记录保留由兜底补发处理）
//   2. 锁容器明细（获取当前容器绑定和分拣记录）
//   3. 构建满箱回传报文（H7）
//   4. 生成 msgId（UUID 幂等键）
//   5. 插入 Outbox 出站表
//   6. 发送到 WMS（异步，失败由 Outbox 定时器重试）
// ★ 2026-09-08 解耦（客户确认）：满箱回传不再迁移波次状态（取消 SORTING→FULLBOX_SYNC 单例限制）。
//   多格口同时/先后满箱互不阻塞——每个锁格各自直接入 Outbox 队列异步发送，
//   与分拣落格、RFID 绑定查询天然并行（满箱回传走 WMS 地址，查询走 RFID 地址，端口不同互不影响）
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

    // ── 步骤1: 执行态校验（不做状态迁移，满箱回传与分拣并行）──
    {
        int st = m_pWaveMgr->status();
        if (st != WAVE_SORTING && st != WAVE_FULLBOX_SYNC)
        {
            HTTP_LOG_INFO("满箱回传（H7）跳过 波次非执行态 grid=%s status=%d(%s)——记录保留内存，由完结前兜底补发/人工处理",
                grid.toLocal8Bit().data(), st, WaveSnapshot::statusToString(st).toLocal8Bit().data());
            return;
        }
    }

    // ── ★ 2026-09-14 异常口不做 H7 满箱回传 ──
    //   异常口 66 收的是超计划件（本就不属于 WMS 计划），一旦按容器上报，
    //   WMS 侧该波次"实报数量"就会超过计划总数 → 回 [2107632] 无法分配并整条驳回，
    //   牵连同报文其它正常格口一起不落账。故此处直接跳过：异常口件只留本地异常账 + UI 预警，人工清出。
    {
        const QString excGridCfg = ConfigManager::instance()->config().exceptionGrid.trimmed();
        if (!excGridCfg.isEmpty() && excGridCfg != "0" &&
            normalizeGridKey(grid) == normalizeGridKey(excGridCfg))
        {
            HTTP_LOG_WARN("满箱回传（H7）跳过 格口%s 为异常口(exceptionGrid=%s)：异常口件不入上传报文，请人工清出",
                grid.toLocal8Bit().data(), excGridCfg.toLocal8Bit().data());
            emit logMessage(QString::fromUtf8(
                "[满箱回传] 跳过格口%1（异常口）：该口只收超计划件、不上传WMS，请人工清出")
                .arg(normalizeGridKey(grid)));
            return;
        }
    }

    // ── 步骤3: 获取容器绑定（内存快速路径 → 数据库现查兜底）──
    QString boxCode = lookupGridBoxCode(grid);

    if (boxCode.isEmpty())
    {
        HTTP_LOG_WARN("满箱回传触发失败（H7） 格口 %s 无容器绑定", grid.toLocal8Bit().data());
        emit logMessage(QString("[满箱回传] 触发失败: 格口%1 无容器绑定").arg(grid), true);
        return;   // 记录保留内存，由完结前兜底补发/人工处理（不迁移状态）
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
        return;   // 无可发送内容（不迁移状态）
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
            return;   // 缺SKU已写异常表+保留记录（兜底补发/人工重锁格重发；不迁移状态）
        }
    }

    qint64 t1 = funcTimer.elapsed();  // ★ 耗时：前置校验

    // ── 步骤4: 构建满箱回传报文（H7）──
    QJsonObject payload = buildFullboxPayload(orderCode, grid, boxCode, records);
    // ★ 2026-09-14 兜底：异常口被拦截时不产生任何 H7（记录保留内存，人工清出即可）
    if (payload.contains("blocked"))
    {
        HTTP_LOG_WARN("满箱回传（H7）中止：格口%s 为异常口（报文构建层拦截），记录不上传 grid=%s",
            normalizeGridKey(grid).toLocal8Bit().data(), grid.toLocal8Bit().data());
        return;
    }
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
    outMsg.grid      = grid;      // ★ 2026-09-08 记录格口号（失败格口下拉直接读取，免解析 payload）
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
// replayPendingRfidPlcEpcs — 恢复分拣(SORTING)后重放挂起的 RFID EPC（2026-09-08）
// 场景：RFID 推送到达时处于非分拣中状态（如满箱同步中），EPC 已存储+已查绑定但未发送 PLC；
//       状态恢复 SORTING 后自动补发 PLC 指令，杜绝"扫了但不分拣"
// 仅主线程调用（waveStatusChanged 钩子）；集合受 m_pendingRfidMutex 保护
//   （handleRfidCarNumReport 在业务线程池/主线程两条路径均可能入队）
// ============================================================================
void HttpServer::replayPendingRfidPlcEpcs()
{
    QList<QString> epcs;
    {
        std::lock_guard<std::mutex> lock(m_pendingRfidMutex);
        if (m_pendingRfidPlcEpcs.isEmpty())
            return;
        epcs = m_pendingRfidPlcEpcs.values();
        m_pendingRfidPlcEpcs.clear();   // 先清空：逐条补发，成功的即完成
    }
    HTTP_LOG_INFO("RFID挂起EPC补发 恢复分拣 count=%d", epcs.size());
    emit logMessage(QString("[RFID] 恢复分拣——自动补发挂起的 EPC %1 条").arg(epcs.size()));
    for (const QString& epc : epcs)
    {
        // 缓存已过期/未就绪（如 carNum 未到、TTL 清理）→ 跳过（未就绪路径有独立重试）
        if (!m_pEpcCache || !m_pEpcCache->isReadyForPlc(epc))
        {
            HTTP_LOG_INFO("RFID挂起补发跳过（缓存过期/未就绪） epc=%s", epc.toLocal8Bit().data());
            continue;
        }
        // 仍在途（已下发未落格）→ 跳过（防重复指令）；已落格的件允许重扫重投
        if (isEpcInFlight(epc))
        {
            HTTP_LOG_INFO("RFID挂起补发跳过（在途） epc=%s", epc.toLocal8Bit().data());
            continue;
        }
        if (trySendToPlcForEpcInternal(epc, true /*bReplayed：跳过波次隔离，按当前波次最新计划选格*/))
            HTTP_LOG_INFO("RFID挂起补发成功 epc=%s（按当前波次最新计划选格）", epc.toLocal8Bit().data());
        else
            HTTP_LOG_WARN("RFID挂起补发失败（格口禁用/无映射/PLC未连接） epc=%s——由异常/未就绪链路跟踪",
                epc.toLocal8Bit().data());
    }
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
    // ★ 2026-09-22 现场需求（第2条）：活跃绑定被"锁格/完结补发"清掉后，改用**离场箱号快照**，
    //   保证后续 H7 报文重建（缺SKU/Outbox失败后补发、手动满箱、一键满箱、完结前兜底补发）
    //   仍用锁格那一刻的箱号（★ 2026-09-25：解锁已不再触发 H7，不在此列）。
    if (boxCode.isEmpty())
        boxCode = boxSnapshotOf(grid);

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
// currentBoxOfGrid — 取某格口当前绑定的容器号（const 版本，供 UI 弹窗只读调用）
//   ★ 与 lookupGridBoxCode 的差别：只查内存绑定表，不查数据库、不写日志——
//     因为它是 const 方法且会被 UI 刷新链路高频调用，不引入 DB 访问与日志噪音。
//     UI 弹窗场景下（程序正在运行）内存绑定表就是权威来源；若内存为空则返回空串，
//     弹窗会显示"—"（仅用于现场定位，不影响任何账务判定）。
// ============================================================================
QString HttpServer::currentBoxOfGrid(const QString& grid) const
{
    std::lock_guard<std::mutex> lock(m_containerMutex);
    QString boxCode = m_containerBindings.value(grid);
    if (boxCode.isEmpty())
    {
        bool ok = false;
        const int g = grid.toInt(&ok);
        if (ok && g >= 1)
            boxCode = m_containerBindings.value(QString("%1").arg(g, GRID_KEY_PADDING, 10, QChar('0')));
    }
    return boxCode;
}

// ============================================================================
// ★ 2026-09-20 现场问题④（no_bind）：下发前置条件的判据来源与打标
// ============================================================================

// hasBoundContainer — 某格口当前是否有活跃容器绑定（只查内存绑定表；线程安全）
//
//   为什么只认内存绑定表、不做数据库兜底（与 lookupGridBoxCode 的差别）：
//     DB 里的 active 绑定可能是"已被现场取走的旧箱"（PLC 已解锁、WMS 还没发 H6 的中间态），
//     若拿它当"已绑定"，就会把件重新导向一个**没有箱子**的格口 —— 正是本次整改要消除的
//     账实不符来源（现场 034 格口：新容器满箱回传成功、人工复核多出一件）。
//     内存绑定表由 H6 绑定 / 波次恢复 / 归档清空三条链路维护，是"当前有效绑定"的唯一真相。
bool HttpServer::hasBoundContainer(int grid) const
{
    if (grid < 1 || grid > BINDING_SLOT_COUNT) return false;
    const QString key = QString("%1").arg(grid, GRID_KEY_PADDING, 10, QChar('0'));
    std::lock_guard<std::mutex> lock(m_containerMutex);
    if (!m_containerBindings.value(key).isEmpty()) return true;
    // 兼容历史/异形 key（如 DB 恢复时的 "34"/"034" 混写）：逐项归一后比对（绑定表 ≤66 项，开销可忽略）
    for (auto it = m_containerBindings.constBegin(); it != m_containerBindings.constEnd(); ++it)
    {
        if (it.value().isEmpty()) continue;
        bool ok = false;
        const int g = normalizeGridKey(it.key()).toInt(&ok);
        if (ok && g == grid) return true;
    }
    return false;
}

// ════════════════════════════════════════════════════════════════════════════
// ★ 2026-09-22 现场需求（第2条）：锁格 = 满箱 = 容器即将离场
//   「容器离场」= ①箱号快照 ②立即清掉该格活跃绑定（内存 erase + DB active=0/unbind_time）
//   **不等 H7 回传结果**；③ H7 报文与箱号持久保留在 outbox_fullbox（失败自动重试、
//   重试耗尽留痕并进入「重传满箱切换(H7)」等人工回传）。
//   清理后 hasBoundContainer() 立即为 false ⇒ 下发前置条件判为"未绑定" ⇒ 件在判定期改投异常口，
//   不会再落进"没有箱子的格口"，也不会把件记到已被取走的旧箱号上（串箱/账实不符）。
// ════════════════════════════════════════════════════════════════════════════
void HttpServer::markBoxSnapshot(const QString& grid, const QString& box)
{
    if (grid.isEmpty() || box.isEmpty()) return;
    const QString key = normalizeGridKey(grid);
    std::lock_guard<std::mutex> lock(m_containerMutex);
    m_boxSnapshot[key] = box;
}

QString HttpServer::boxSnapshotOf(const QString& grid) const
{
    if (grid.isEmpty()) return QString();
    const QString key = normalizeGridKey(grid);
    std::lock_guard<std::mutex> lock(m_containerMutex);
    QString box = m_boxSnapshot.value(key);
    if (box.isEmpty())
    {
        bool ok = false;
        const int g = key.toInt(&ok);
        if (ok && g >= 1)
            box = m_boxSnapshot.value(QString("%1").arg(g, GRID_KEY_PADDING, 10, QChar('0')));
    }
    return box;
}

void HttpServer::clearBoxSnapshot(const QString& grid)
{
    if (grid.isEmpty()) return;
    const QString key = normalizeGridKey(grid);
    std::lock_guard<std::mutex> lock(m_containerMutex);
    m_boxSnapshot.remove(key);
    bool ok = false;
    const int g = key.toInt(&ok);
    if (ok && g >= 1)
        m_boxSnapshot.remove(QString("%1").arg(g, GRID_KEY_PADDING, 10, QChar('0')));
}

void HttpServer::clearAllBoxSnapshots()
{
    std::lock_guard<std::mutex> lock(m_containerMutex);
    m_boxSnapshot.clear();
}

bool HttpServer::detachContainerOnFullbox(const QString& grid, const QString& box, const QString& reason)
{
    if (grid.isEmpty()) return false;

    // ① 箱号快照：即使随后活跃绑定被清，H7 报文重建（补发/手动满箱）仍能取到正确箱号
    if (!box.isEmpty())
        markBoxSnapshot(grid, box);

    // ② 清内存活跃绑定（兼容补零/异形 key：逐个 remove + 值匹配兜底）
    bool bCleared = false;
    {
        std::lock_guard<std::mutex> lock(m_containerMutex);
        const QString key = normalizeGridKey(grid);
        if (m_containerBindings.remove(key) > 0) bCleared = true;
        bool ok = false;
        const int g = key.toInt(&ok);
        if (ok && g >= 1)
        {
            if (m_containerBindings.remove(QString("%1").arg(g, GRID_KEY_PADDING, 10, QChar('0'))) > 0)
                bCleared = true;
        }
        if (!box.isEmpty())
        {
            for (auto it = m_containerBindings.begin(); it != m_containerBindings.end(); )
            {
                if (it.value() == box) { it = m_containerBindings.erase(it); bCleared = true; }
                else ++it;
            }
        }
    }

    const QString orderCode = m_pWaveMgr ? m_pWaveMgr->orderCode() : QString();

    // ③ 解绑留痕（清理动作留痕：格口号/旧箱号/波次/原因/动作）
    if (bCleared || !box.isEmpty())
    {
        HTTP_LOG_INFO("解绑留痕 grid=%s 旧箱=%s order=%s 原因=%s 动作=DB归档(active=0)+内存清空 后续=等待WMS重发H6绑定新箱",
            grid.toLocal8Bit().data(), box.toLocal8Bit().data(), orderCode.toLocal8Bit().data(),
            reason.toLocal8Bit().data());
        LOG_INFO("[解绑留痕] grid=%s 旧箱=%s order=%s 原因=%s 动作=DB归档+内存清空",
            grid.toLocal8Bit().data(), box.toLocal8Bit().data(), orderCode.toLocal8Bit().data(),
            reason.toLocal8Bit().data());
    }

    // ④ DB 归档（active=0 + unbind_time；行不删，历史可追溯/可沿用）
    if (m_pSortingDb && m_pSortingDb->isOpen())
    {
        const QString key = normalizeGridKey(grid);
        m_pSortingDb->archiveGridBinds(key);
        HTTP_LOG_INFO("[容器离场] grid=%s box=%s 原因=%s 动作=DB归档(active=0)完成 箱号快照=已保存"
                      "（H7报文与箱号保留在Outbox，失败自动重试/耗尽后人工重传）",
            key.toLocal8Bit().data(), box.toLocal8Bit().data(), reason.toLocal8Bit().data());
    }
    else
    {
        HTTP_LOG_WARN("[容器离场] grid=%s box=%s 原因=%s DB未打开：仅清内存绑定（重启由启动归档兜底）",
            grid.toLocal8Bit().data(), box.toLocal8Bit().data(), reason.toLocal8Bit().data());
    }

    emit bindingUpdated();
    return bCleared;
}

// ════════════════════════════════════════════════════════════════════════════
// ★ 2026-09-22 现场口径（最终）：**满箱回传结果与容器绑定彻底解耦**
//   · 容器号**只在锁格（=满箱）/完结补发那一刻清理**（见 detachContainerOnFullbox）——
//     那才是"箱子离场"的物理时点；
//   · H7 的成功/失败/重试/人工重传**一律不改容器绑定**：不查绑定、不写 grid_box_bind、
//     不因"成功"解绑，也不会因迟到回执清掉期间换绑好的新容器（因此"回传成功后兜底解绑"
//     这条链路已整体删除，代码中不再存在能因回执改动容器绑定的路径）；
//   · 回传结果只做三件事：① 更新该条报文状态（成功/失败/重试次数）② 落响应留痕
//     ③ 刷新失败重传下拉 + 完结屏障复检。
//   容器状态变化只有三个来源：H6 绑定（新容器到场）／锁格·完结补发（容器离场）／
//   启动归档·清空格口绑定·关闭软件切出·新任务·波次取消（批量清空）。
// ════════════════════════════════════════════════════════════════════════════

// isGridDispatchableHere — 单格口版"是否可下发"（判据唯一来源 = PlcManager::isGridDispatchable）
//   已禁用(满箱未重绑) → false；锁格 → true（按原有逻辑）；开关关闭 → 只看禁用；
//   其余（已解锁）→ 必须有容器绑定。
//   ★ 与 allocBlockedMask() 同源：掩码中 true 的格口 == 本函数返回 false 的格口。
//   ★ 设备层未就绪（m_pPlcMgr==nullptr）时返回 true（不拦，退回改造前行为）。
bool HttpServer::isGridDispatchableHere(int grid) const
{
    if (!m_pPlcMgr) return true;
    return m_pPlcMgr->isGridDispatchable(grid);
}

// allocBlockedMask — 本次"不可认领/不可下发"格口掩码（判据与发送侧完全同源）
//
//   ★★ 必须在 m_allocMutex **之外**调用：内部要取 m_containerMutex（绑定表）与
//      PlcManager 的锁格/禁用状态锁，若在分配表锁内调用会形成 alloc→container 嵌套，
//      与其它路径的加锁顺序不一致（PlanAllocTable 的线程约定也要求"锁外准备入参"）。
//
//   ★ 2026-09-20 口径（现场问题④ + 多格口额度承接）：
//     **只有"可下发格口"能被认领** —— 即 `!isGridDispatchable(g)`：
//       · 已解锁且未绑定容器 → 不可认领（本次整改新增）；
//       · 满箱未重绑(禁用)    → 不可认领（本次同步收紧：禁用格口同样是"不可用"的落点，
//                               把件认领过去会落进已上报过的满箱/无容器处，制造新的账实不符）；
//       · 锁格                → 可认领（按原有逻辑，不拦）；
//       · 开关关闭            → 全 false（等价不传掩码，逐字回退改造前行为）。
//   ⇒ 多格口 SKU 的效果正是客户 2026-09-20 追加的口径：
//      **优先用"其它可用计划格口"的剩余额度承接**；若其它格口都没有可用额度（只剩不可用格口上的额度），
//      认领会失败，由 PlcManager 按根因改投异常口（额度仍原样留在原格口，绑定/换箱后自动恢复分配）。
PlanAllocTable::GridMask HttpServer::allocBlockedMask() const
{
    PlanAllocTable::GridMask blocked{};   // 值初始化 = 全 false = 全部可认领
    if (!m_pPlcMgr) return blocked;       // 设备层未就绪 → 不拦（退回改造前行为）

    for (int g = 1; g <= BINDING_SLOT_COUNT; ++g)
    {
        if (!m_pPlcMgr->isGridDispatchable(g)) blocked[(size_t)g] = true;
    }
    return blocked;
}

// noteExcRoute — 改投异常口的"原因打标"（发送侧，主线程）
//   ★ 发送侧只打标 + UI 告警，**不写数据库留痕**：留痕统一在件真正落入异常口时写
//     （一次事件一条记录），避免"指令发出但发送失败/未落格"时留下误导性的异常记录。
void HttpServer::noteExcRoute(const QString& epc, const QString& reason)
{
    if (epc.isEmpty()) return;
    {
        std::lock_guard<std::mutex> lk(m_excRouteMutex);
        m_excRouteMark.insert(epc, qMakePair(reason, QDateTime::currentMSecsSinceEpoch()));
    }
    const QString sku = getSkuByEpc(epc);
    HTTP_LOG_WARN("[不可落格] epc=%s sku=%s 原因=%s → 改投异常口%s（本件不落计划格口、不计已分拣、不消耗计划额度，请人工清出）",
        epc.toLocal8Bit().data(), sku.toLocal8Bit().data(), reason.toLocal8Bit().data(),
        ConfigManager::instance()->config().exceptionGrid.toLocal8Bit().data());
    emit logMessage(QString::fromUtf8(
        "[不可落格] EPC %1（SKU %2）%3 → 已改投异常口%4（不计已分拣、不消耗计划额度；额度保留在原格口，"
        "绑定容器后自动恢复分配，请到异常口人工清出）")
        .arg(epc).arg(sku.isEmpty() ? QString::fromUtf8("未知") : sku).arg(reason)
        .arg(ConfigManager::instance()->config().exceptionGrid), true);
}

// clearExcRoute — 清除旧标（每轮投递入口调用）
//   为什么每轮都要清：一件货可能被反复投放（从异常口取出重投），若上一轮的打标残留，
//   会把后续**真正因为超计划**落入 66 的同一件误标成"未绑定容器"（原因张冠李戴）。
void HttpServer::clearExcRoute(const QString& epc)
{
    if (epc.isEmpty()) return;
    std::lock_guard<std::mutex> lk(m_excRouteMutex);
    m_excRouteMark.remove(epc);
}

// takeExcRoute — 取用并消费打标（落格线程池）
//   返回空 = 无标（该件落入 66 属超计划 / PLC 改投，按既有口径处理）；
//   命中但超过保鲜期同样视为无标（防残留）。
QString HttpServer::takeExcRoute(const QString& epc)
{
    if (epc.isEmpty()) return QString();
    std::lock_guard<std::mutex> lk(m_excRouteMutex);
    auto it = m_excRouteMark.find(epc);
    if (it == m_excRouteMark.end()) return QString();
    const QPair<QString, qint64> v = it.value();
    m_excRouteMark.erase(it);
    const qint64 ageMs = QDateTime::currentMSecsSinceEpoch() - v.second;
    if (ageMs > EPC_TERMINAL_EXCEPTION_KEEP_MS) return QString();   // 保鲜期外 → 视为无标
    return v.first;
}

// ============================================================================
// sendFullboxForGrid — 单格口 H7 满箱补发（「完结前兜底补发」与「手动满箱切换」共用）
// 不动波次状态机、不禁用格口；无容器号/缺SKU/Outbox失败 时返回 false（记录保留内存）
// ============================================================================
// ★ 2026-09-16 需求④：返回值改为"本次生成的 H7 msgId"（成功=非空），失败返回空串并把原因写入 *reasonOut。
//   本函数是"补发/手动满箱"的唯一实现；sendFullboxForGrid() 是它的 bool 薄封装（既有调用点零改动）。
QString HttpServer::sendFullboxForGridDetailed(const QString& orderCode, const QString& grid,
                                              QVector<GridSortRecord> records, QString* reasonOut)
{
    auto fail = [reasonOut](const QString& r) -> QString {
        if (reasonOut) *reasonOut = r;
        return QString();
    };

    if (records.isEmpty()) return fail(QString::fromUtf8("无待上传的分拣记录"));

    // 0. ★ 2026-09-14 异常口不回传（同 sendFullbox：异常口件不属于 WMS 计划，上报会使实报数超计划被整条驳回）
    {
        const QString excGridCfg = ConfigManager::instance()->config().exceptionGrid.trimmed();
        if (!excGridCfg.isEmpty() && excGridCfg != "0" &&
            normalizeGridKey(grid) == normalizeGridKey(excGridCfg))
        {
            HTTP_LOG_WARN("满箱补发 跳过格口%s（异常口，件不上传WMS，请人工清出）", grid.toLocal8Bit().data());
            return fail(QString::fromUtf8("异常口不上传WMS"));
        }
    }

    // 1. 容器号
    QString boxCode = lookupGridBoxCode(grid);
    if (boxCode.isEmpty())
    {
        HTTP_LOG_WARN("满箱补发 格口 %s 无容器绑定，跳过（记录保留内存，可人工核对）",
            grid.toLocal8Bit().data());
        emit logMessage(QString("[满箱补发] 格口%1 无容器绑定，跳过（记录保留内存，可人工核对）").arg(grid), true);
        return fail(QString::fromUtf8("无容器绑定"));
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
            return fail(QString::fromUtf8("有 %1 条EPC未查到SKU").arg(missingSkuEpcs.size()));
        }
    }

    // 3. 构建 H7 报文 + 入 Outbox + 清理该格记录 + 发送
    QJsonObject payload = buildFullboxPayload(orderCode, grid, boxCode, records);
    // ★ 2026-09-14 兜底：异常口被拦截时不产生任何 H7（记录保留内存，人工清出即可）
    if (payload.contains("blocked"))
    {
        HTTP_LOG_WARN("满箱补发 中止：格口%s 为异常口（报文构建层拦截）", normalizeGridKey(grid).toLocal8Bit().data());
        return fail(QString::fromUtf8("报文构建层拦截（异常口）"));
    }
    QString msgId = QString::fromUtf8(QUuid::createUuid().toByteArray().toHex());
    QString nowStr = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    QString nextRetry = QDateTime::currentDateTime().addSecs(OUTBOX_RETRY_INTERVAL_SEC)
                            .toString("yyyy-MM-dd HH:mm:ss");

    OutboxRecord outMsg;
    outMsg.msgId      = msgId;
    outMsg.orderCode  = orderCode;
    outMsg.boxcode    = boxCode;
    outMsg.grid       = grid;     // ★ 2026-09-08 记录格口号（失败格口下拉直接读取，免解析 payload）
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
        return fail(QString::fromUtf8("Outbox 写入失败"));
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
    if (reasonOut) reasonOut->clear();   // 成功无原因
    return msgId;
}

// ★ 2026-09-16 需求④：bool 薄封装（既有调用点零改动），语义与改造前完全一致
bool HttpServer::sendFullboxForGrid(const QString& orderCode, const QString& grid,
                                    QVector<GridSortRecord> records)
{
    return !sendFullboxForGridDetailed(orderCode, grid, std::move(records), nullptr).isEmpty();
}

// ============================================================================
// reportFullboxAllBoundGrids — ★ 2026-09-26 现场口径：「一键满箱回传」的**唯一实现**
//
//   口径（按钮与「结束任务」完全同一套，杜绝两处实现漂移）：
//     · 遍历**当前所有已绑定容器**（内存绑定表），逐个按 H7 满箱回传；
//     · **有分拣记录才生成报文**；无记录的格口跳过并计数（不产生空报文）；
//     · 箱号取**当前绑定**（离场后的格口不在本集合内，见 flushUnreportedFullboxes 的遗留留痕）；
//     · 异常口 66 一律跳过（该口件不上传 WMS，请人工清出）；
//     · 不动波次状态机、不禁用格口；报文入 Outbox 后失败/超时由既有重试与人工重传处理。
//   detachOnSuccess=true 时（仅"结束任务"用）：报文一发出即视为"容器离场"，
//     立即清该格活跃绑定 + 归档留痕（2026-09-22 口径）；按钮版传 false（不改绑定）。
//
//   入参 reasonTag 仅用于日志区分（"一键满箱" / "结束任务补发"）。
// ============================================================================
QVector<FullboxBatchEntry> HttpServer::reportFullboxAllBoundGrids(const QString& reasonTag,
                                                                 bool detachOnSuccess)
{
    QVector<FullboxBatchEntry> out;
    const QMap<QString, QString> binds = getContainerBindings();
    for (auto it = binds.constBegin(); it != binds.constEnd(); ++it)
    {
        FullboxBatchEntry e;
        e.grid = normalizeGridKey(it.key());
        e.box  = it.value();

        QString reason;
        e.msgId = manualFullbox(e.grid, &reason);   // 与「手动满箱/重传 H7」同一实现
        if (e.msgId.isEmpty())
            e.reason = reason;

        if (!e.msgId.isEmpty() && detachOnSuccess
            && ConfigManager::instance()->config().sortingClearBoxOnFullbox)
        {
            // 完结补发与锁格同口径：满箱报文一发出即视为"容器离场"，立即清掉活跃绑定（不等回执）。
            // 依据：036/052 曾因"未锁格 + 完结补发被 WMS 拒"⇒ 绑定残留 ⇒ 现场已取箱而 WCS 仍认为
            // 已绑定 ⇒ 件继续被投进没有箱子的格口。H7 报文与箱号保留在 Outbox 可重试。
            detachContainerOnFullbox(e.grid, e.box, QString::fromUtf8("完结补发(H7待回传)"));
        }
        out.append(e);
    }

    int okCnt = 0, emptyCnt = 0, failCnt = 0;
    QStringList failDesc;
    for (const FullboxBatchEntry& e : out)
    {
        if (!e.msgId.isEmpty()) { ++okCnt; continue; }
        if (e.reason == QString::fromUtf8("无待上传的分拣记录")
            || e.reason == QString::fromUtf8("异常口不上传WMS")) { ++emptyCnt; continue; }
        ++failCnt;
        if (failDesc.size() < 20) failDesc << QString("%1(%2)").arg(e.grid, e.reason);
    }
    HTTP_LOG_INFO("%s 已绑定容器逐个 H7：总数=%d 成功=%d 无记录跳过=%d 失败=%d"
                  "（与「一键满箱回传」同一实现；detachOnSuccess=%d）",
        reasonTag.toLocal8Bit().data(), out.size(), okCnt, emptyCnt, failCnt,
        detachOnSuccess ? 1 : 0);
    if (failCnt > 0)
    {
        HTTP_LOG_WARN("%s：%d 个已绑定格口未能生成 H7 —— %s（不阻塞后续流程，可用「重传满箱切换(H7)」补传）",
            reasonTag.toLocal8Bit().data(), failCnt, failDesc.join(" ").toLocal8Bit().data());
    }
    return out;
}

// ============================================================================
// flushUnreportedFullboxes — 「结束任务」时的统一满箱回传（2026-09-07 引入；2026-09-26 按现场口径改写）
//
//   ★ 现场口径（2026-09-26）：点「结束任务」= **对当前所有已绑定容器做一次「一键满箱回传」**
//     （与按钮同一实现 reportFullboxAllBoundGrids → manualFullbox；有记录才发报文，无记录跳过），
//     随后固定延迟 endReportDelayMs（默认 2000ms）+ **无条件**发送完结回传（H8，见 sendEnd）。
//   ★ 未绑定容器但**仍有未上传落格记录**的格口：本函数**不发报文**，改为：
//       · http.log 告警 + 界面提示（不静默丢弃）；
//       · 写一条 exception_record（type=满箱回传未完成）留痕：格口号 + 未上传条数 +
//         "用「重传满箱切换(H7)」手输格口号补传"指引（手动满箱可用离场快照箱号，件不会永远报不上去）。
//   ★ 不动波次状态机、不禁用格口；返回本次实际生成的 H7 报文条数。
// ============================================================================
int HttpServer::flushUnreportedFullboxes(const QString& orderCode)
{
    // ── ① 主集合：当前所有已绑定容器（一键满箱口径；成功者按完结口径清绑定）──
    const QVector<FullboxBatchEntry> entries =
        reportFullboxAllBoundGrids(QString::fromUtf8("结束任务补发"), /*detachOnSuccess=*/true);

    int flushed = 0, emptyCnt = 0, failedCnt = 0;
    QStringList failedDesc;
    for (const FullboxBatchEntry& e : entries)
    {
        if (!e.msgId.isEmpty()) { ++flushed; continue; }
        if (e.reason == QString::fromUtf8("无待上传的分拣记录")
            || e.reason == QString::fromUtf8("异常口不上传WMS"))
        {
            ++emptyCnt;   // 正常跳过：无记录 / 异常口
            continue;
        }
        ++failedCnt;
        if (failedDesc.size() < 20)
            failedDesc << QString("%1(%2)").arg(e.grid, e.reason);
    }

    // ── ② 遗留：未绑定容器但仍有未上传记录（离场旧箱）→ 跳过 + 留痕（等人工手输格口号补传）──
    QStringList leftoverDesc;
    int leftoverRecords = 0;
    {
        QMap<QString, QVector<GridSortRecord>> pending;
        {
            std::lock_guard<std::mutex> lock(m_gridRecordMutex);
            pending = m_gridSortRecords;
        }
        for (auto it = pending.constBegin(); it != pending.constEnd(); ++it)
        {
            if (it.value().isEmpty()) continue;
            const QString grid = it.key();
            if (isExceptionGridKey(grid)) continue;      // 异常口件不上传 WMS（人工清出）
            bool okG = false;
            const int g = normalizeGridKey(grid).toInt(&okG);
            if (okG && g > 0 && hasBoundContainer(g)) continue;   // 已在 ① 里处理
            leftoverRecords += it.value().size();
            if (leftoverDesc.size() < 20)
                leftoverDesc << QString("%1(%2条)").arg(normalizeGridKey(grid)).arg(it.value().size());
        }
    }

    HTTP_LOG_INFO("结束任务满箱补发完成 order=%s 已绑定容器=%d 已发=%d 无记录跳过=%d 失败=%d "
                  "未绑定容器但有未上传记录(跳过)=%d个格口/%d条",
        orderCode.toLocal8Bit().data(), entries.size(), flushed, emptyCnt, failedCnt,
        leftoverDesc.size(), leftoverRecords);

    if (failedCnt > 0)
    {
        HTTP_LOG_WARN("结束任务满箱补发：%d 个已绑定格口未能生成 H7 —— %s（不阻塞完结回传，可用"
                      "「重传满箱切换(H7)」补传）",
            failedCnt, failedDesc.join(" ").toLocal8Bit().data());
        emit logMessage(QString::fromUtf8(
            "[一键满箱] 结束任务补发：%1 个已绑定格口未生成报文（%2）—— 不影响完结回传，可稍后补传")
            .arg(failedCnt).arg(failedDesc.join(QString::fromUtf8(" "))), true);
    }

    if (!leftoverDesc.isEmpty())
    {
        const QString detail = QString::fromUtf8("未绑定容器但仍有未上传落格记录：%1 —— 共 %2 条，"
                                                 "本次**不发报文**（无当前容器号）；请用「重传满箱切换(H7)」"
                                                 "手输格口号补传（手动满箱按离场快照箱号上报）")
                                   .arg(leftoverDesc.join(QString::fromUtf8(" "))).arg(leftoverRecords);
        HTTP_LOG_WARN("结束任务满箱补发：%s", detail.toLocal8Bit().data());
        emit logMessage(QString::fromUtf8("[满箱回传] %1").arg(detail), true);
        if (m_pSortingDb && m_pSortingDb->isOpen() && !orderCode.isEmpty())
        {
            ExceptionRecord ex;
            ex.type      = QString::fromUtf8("满箱回传未完成");
            ex.orderCode = orderCode;
            ex.epc       = "";
            ex.sku       = "";
            ex.reason    = detail;
            ex.time      = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
            m_pSortingDb->insertException(ex);
        }
    }

    return flushed;
}

// ============================================================================
// manualFullbox — 手动满箱切换（2026-09-07）
// UI 在「重传满箱切换」旁输入格口号后点击：读取该格口当前分拣记录与容器号，
// 立即按 H7 满箱回传上传（替代人工 PLC 锁格的补传手段，不动波次状态机）
//   ★ 2026-09-16 需求④：返回值改为本次生成的 H7 msgId；失败返回空串并回填可读原因，
//     供「一键满箱回传」计数（成功=已入 Outbox 的报文件数；失败=未生成报文的原因）。
//     ★ 日志与业务行为与改造前逐行一致（仅补返回值/原因）。
// ============================================================================
QString HttpServer::manualFullbox(const QString& grid, QString* reasonOut)
{
    auto fail = [reasonOut](const QString& r) -> QString {
        if (reasonOut) *reasonOut = r;
        return QString();
    };

    if (!m_pWaveMgr)
    {
        emit logMessage("[手动满箱] 服务未就绪", true);
        return fail(QString::fromUtf8("服务未就绪"));
    }
    if (grid.isEmpty())
    {
        emit logMessage("[手动满箱] 请先输入格口号", true);
        return fail(QString::fromUtf8("未输入格口号"));
    }

    // ★ 2026-09-14 异常口不参与满箱回传（异常口件不属于 WMS 计划，上报会使实报超计划被整条驳回）
    if (isExceptionGridKey(grid))
    {
        HTTP_LOG_INFO("手动满箱 跳过格口%s（异常口/强排口，件不上传WMS）", grid.toLocal8Bit().data());
        emit logMessage(QString::fromUtf8("[手动满箱] 格口%1 是异常口：该口只收超计划件、不上传WMS，请人工清出")
            .arg(normalizeGridKey(grid)));
        return fail(QString::fromUtf8("异常口不上传WMS"));
    }

    QString orderCode = m_pWaveMgr->orderCode();
    if (orderCode.isEmpty())
    {
        emit logMessage("[手动满箱] 当前无运行波次，无法执行手动满箱", true);
        return fail(QString::fromUtf8("当前无运行波次"));
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
        return fail(QString::fromUtf8("无待上传的分拣记录"));
    }

    QString innerReason;
    QString msgId = sendFullboxForGridDetailed(orderCode, grid, records, &innerReason);
    if (!msgId.isEmpty())
    {
        // 成功：清空原因（调用方按 msgId 判定成功）
        if (reasonOut) reasonOut->clear();
        emit logMessage(QString("[手动满箱] 格口%1 已按 H7 满箱回传上传（order=%2）").arg(grid).arg(orderCode));
        return msgId;
    }
    return fail(innerReason.isEmpty() ? QString::fromUtf8("满箱回传未生成报文") : innerReason);
}

// ============================================================================
// RFID 推送吞吐/峰值统计（★ 2026-09-07 效率与峰值显示）
// 口径：每收到一条含 EPC 的 RFID 推送记为 1 件；
//   · recordRfidPush：滑动 60s 时间戳（实时"效率"）+ 每分钟分桶（统计图/当日峰值）
//   · 跨日：日期变化时先把前一天最终峰值落库，再清零统计
// ============================================================================
void HttpServer::recordRfidPush()
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    std::lock_guard<std::mutex> lock(m_rfidPushMutex);

    // 跨日结转：日期变化 → 前一天最终峰值落库 + 清零
    QString today = QDateTime::currentDateTime().toString("yyyy-MM-dd");
    if (m_peakDate.isEmpty())
        m_peakDate = today;
    if (m_peakDate != today)
    {
        if (m_peakPerMinuteToday > 0 && m_pSortingDb && m_pSortingDb->isOpen())
        {
            m_pSortingDb->saveDailyPeak(m_peakDate, m_peakPerMinuteToday);
            HTTP_LOG_INFO("效率统计 跨日结转 date=%s peakPerMinute=%d", m_peakDate.toLocal8Bit().data(), m_peakPerMinuteToday);
        }
        m_peakDate = today;
        m_peakPerMinuteToday = 0;
        m_rfidMinuteCount.clear();
        m_rfidPushTimes.clear();
    }

    // 1) 滑动 60 秒窗口（实时效率）
    m_rfidPushTimes.push_back(nowMs);
    const qint64 cutoff = nowMs - 60 * 1000;
    while (!m_rfidPushTimes.empty() && m_rfidPushTimes.front() < cutoff)
        m_rfidPushTimes.pop_front();

    // 2) 每分钟分桶（统计图 / 当日峰值）
    const qint64 epochMin = nowMs / 60000;
    int& bucket = m_rfidMinuteCount[epochMin];
    ++bucket;
    if (bucket > m_peakPerMinuteToday)
        m_peakPerMinuteToday = bucket;

    // 3) ★ 2026-09-13 波次面板「RFID扫描次数」：本次运行累计推送 EPC 次数
    //    口径与上面完全同源（一条推送一个非空 EPC 记 1 次；重复 EPC 逐次累加；跨波次不清零）
    m_rfidPushTotal.fetch_add(1, std::memory_order_relaxed);
}

// ============================================================================
// ★ 2026-09-13 性能核验：环形采样实现（无动态分配、无锁、常数开销）
//   写侧：主线程（RFID 推送 / 落格反馈 / PLC 发送）单点写入，游标原子自增
//   读侧：健康日志每 60s 复制快照后排序取分位（256 槽，开销可忽略）
// ============================================================================
void HttpServer::perfSampleRfidToPlc(int ms)
{
    if (ms < 0) return;
    const int i = m_perfIdx.fetch_add(1, std::memory_order_relaxed);
    m_perfRfidToPlc[i % PERF_RING_SLOTS] = ms;
    m_perfRfidCount.fetch_add(1, std::memory_order_relaxed);
}

void HttpServer::perfSampleFbLatency(int ms)
{
    if (ms < 0) return;
    const int i = m_perfIdx.fetch_add(1, std::memory_order_relaxed);
    m_perfFbLatency[i % PERF_RING_SLOTS] = ms;
    m_perfFbCount.fetch_add(1, std::memory_order_relaxed);
}

void HttpServer::perfSampleEventLag(int ms)
{
    if (ms < 0) return;
    const int i = m_perfIdx.fetch_add(1, std::memory_order_relaxed);
    m_perfEventLag[i % PERF_RING_SLOTS] = ms;
}

int HttpServer::perfPercentile(const int* ring, int validCount, double pct)
{
    if (validCount <= 0) return -1;
    if (validCount > PERF_RING_SLOTS) validCount = PERF_RING_SLOTS;
    QVector<int> v;
    v.reserve(validCount);
    for (int i = 0; i < validCount; ++i)
        v.append(ring[i]);
    std::sort(v.begin(), v.end());
    int idx = qBound(0, (int)std::ceil(pct * v.size()) - 1, v.size() - 1);
    return v.at(idx);
}

HttpServer::PerfSnapshot HttpServer::perfSnapshot() const
{
    PerfSnapshot s;
    const quint64 rfidN = m_perfRfidCount.load(std::memory_order_relaxed);
    const quint64 fbN   = m_perfFbCount.load(std::memory_order_relaxed);
    s.samples      = (int)qMin<quint64>(rfidN, PERF_RING_SLOTS);
    s.rfidToPlcP50 = perfPercentile(m_perfRfidToPlc, s.samples, 0.50);
    s.rfidToPlcP95 = perfPercentile(m_perfRfidToPlc, s.samples, 0.95);
    s.rfidToPlcP99 = perfPercentile(m_perfRfidToPlc, s.samples, 0.99);
    const int fbValid = (int)qMin<quint64>(fbN, PERF_RING_SLOTS);
    s.fbLatencyP95 = perfPercentile(m_perfFbLatency, fbValid, 0.95);
    // 事件滞后采样与上面共用游标，槽内数据按"最近 PERF_RING_SLOTS 次任一事件"计，取分位仍具参考性
    s.eventLagP95  = perfPercentile(m_perfEventLag, PERF_RING_SLOTS, 0.95);
    return s;
}

QString HttpServer::containerForGrid(const QString& grid) const
{
    if (grid.isEmpty()) return QString();
    std::lock_guard<std::mutex> lock(m_containerMutex);
    QString box = m_containerBindings.value(grid);
    if (box.isEmpty())
    {
        // 兼容 "7" / "007" 两种写法（与落格时取容器号的兜底口径一致）
        bool ok = false;
        const int g = grid.toInt(&ok);
        if (ok && g >= 1)
            box = m_containerBindings.value(QString("%1").arg(g, GRID_KEY_PADDING, 10, QChar('0')));
    }
    return box;
}

bool HttpServer::isPlcSendInFlight(const QString& epc) const
{
    return isEpcInFlightReadOnly(epc);
}

// ★ 2026-09-20 现场问题④：当前在途件数（只读，不改任何状态）
//   口径与 isEpcInFlightReadOnly 完全一致（含 plcInFlightTimeoutMs 超时判定）——
//   超时的件不再算"在途"，因此该数与实时面板的"待落格"口径一致。
//   用途：MainWindow「清空格口绑定」二次确认（分拣中清空会让这些件落到无容器格口）。
int HttpServer::inFlightCount() const
{
    int n = 0;
    for (const QString& epc : m_sentEpcs)
        if (isEpcInFlightReadOnly(epc)) ++n;
    return n;
}

// ★ 2026-09-21 硬上限：某 (SKU,格口) 当前在途件数（只读）
//   为什么需要：分配表未生效（allocEnabled=false / 编译失败）时没有"在途认领"账，
//   若只按"已落格数"判上限，"两件同时在飞"就会双双放行 → 该格口超计划。
//   数据源：m_lastSentGrid（发送结果回调记录"本件发往哪个格口"，带锁）+ m_sentEpcs（主线程）。
//   注：分配表有效时不要用本函数（表内 reserv 就是权威在途账），避免重复计数。
int HttpServer::inFlightCountOfGrid(const QString& sku, const QString& gridKey) const
{
    if (sku.isEmpty() || gridKey.isEmpty()) return 0;
    const QString want = normalizeGridKey(gridKey);
    int n = 0;
    for (const QString& epc : m_sentEpcs)
    {
        if (!isEpcInFlightReadOnly(epc)) continue;
        int sentGrid = 0;
        {
            std::lock_guard<std::mutex> lk(m_lastSentGridMutex);
            sentGrid = m_lastSentGrid.value(epc, 0);
        }
        if (sentGrid <= 0) continue;
        if (normalizeGridKey(QString::number(sentGrid)) != want) continue;
        if (getSkuByEpc(epc) != sku) continue;      // 同格口可能混着别的 SKU 的件
        ++n;
    }
    return n;
}

// ★ 2026-09-21 硬上限：某 (SKU,格口) 的 **H4 计划件数**（权威上限，与分配表是否生效无关）
//   来源：H4 解析后的 GridEntry::planQtyPerGrid（同格口多行累加）；无该格口计划 → 0（视为不可投）
int HttpServer::h4PlanQtyOfGrid(const QString& sku, const QString& gridKey) const
{
    if (sku.isEmpty() || !m_pBuffer) return 0;
    const GridEntry e = m_pBuffer->get(sku);
    if (e.planQtyPerGrid.isEmpty()) return 0;
    return e.planQtyPerGrid.value(normalizeGridKey(gridKey), 0);
}

// ★ 2026-09-21 硬上限：某 (SKU,格口) 当前是否还有可用额度（严格口径）
//   used = 已落（分配表权威去重；表未生效时用 m_boxLandedEpcs）+ 在途认领
//   ⇒ remaining = H4计划 − used > 0 才允许把件投到该格口
bool HttpServer::hasGridQuotaLeft(const QString& sku, const QString& gridKey) const
{
    const int planH4 = h4PlanQtyOfGrid(sku, gridKey);
    if (planH4 <= 0) return false;                     // 该格口没有计划数量 → 不可投（进 66）
    const QString gk = normalizeGridKey(gridKey);
    int used = 0;
    if (m_allocValid.load() && ConfigManager::instance()->config().allocEnabled)
    {
        std::lock_guard<std::mutex> lock(m_allocMutex);
        used = m_alloc.landedOf(sku, gk) + m_alloc.reservOf(sku, gk);
    }
    else
    {
        used = landedCountOf(gk, sku) + inFlightCountOfGrid(sku, gk);
    }
    return used < planH4;
}

int HttpServer::rescanResendTimes(const QString& epc) const
{
    return m_rescanResendTimes.value(epc, 0);
}

// ============================================================================
// ★ 2026-09-14 落格即计时归零（统一入口，覆盖全部落格分支）
//   为什么需要：EpcCache 的「RFID推送→PLC发送」超时窗口（PLC_SEND_TIMEOUT_MS=1s）以
//   receivedAt 为起点。件已真实落格后，这一轮计时就没有意义了；若不清零，
//   该 EPC 之后被重新推送（二次上传/重扫重投）时会沿用旧的 receivedAt，
//   在重投链路里被立刻判"发送超时"→ 本该重新下发的件反而进不了格口。
//   （重扫重投在 RFID 推送入口已有一次归零，但落格本身此前没有归零。）
//   线程安全（重要）：本方法**只在主线程执行**（由 epcsLanded 信号驱动）——
//     m_sentEpcs / m_rescanResendTimes / m_lastPlcSendMs 等"在途语义"容器仅在主线程访问，
//     落格分支运行在 PLC 反馈接收线程池内，不能直接读写它们。
//   影响面（客户关注"会不会影响下一波次"）：**不会**。
//     · 只改这一个 EPC 的条目，不触碰其它 EPC、不改波次状态、不改绑定、不改计数；
//     · EpcCache 条目本就有 TTL，跨波次会自然过期；H4 新波次还会清空在途/冷却/重发计数；
//     · 同一 EPC 在下一波次再次出现时按新条目重新起算，行为与首次一致。
// ============================================================================
void HttpServer::noteEpcLanded(const QString& epc)
{
    if (epc.isEmpty())
        return;

    const bool wasInFlight = m_sentEpcs.contains(epc);
    if (m_pEpcCache)
        m_pEpcCache->resetTiming(epc);     // receivedAt=now、sentAt 清空 → 超时窗口重新起算
    m_rescanResendTimes.remove(epc);       // 该件本轮投递已结束，重发次数清零
    clearEpcInFlight(epc);

    HTTP_LOG_INFO("[落格归零] epc=%s 已落格 → 超时计时归0、在途/重发计数清零（%s）",
        epc.toLocal8Bit().data(),
        wasInFlight ? "本次确为在途件" : "本次未记为在途(可能已由批处理入口清理)");
}

bool HttpServer::isEpcInFlightReadOnly(const QString& epc) const
{
    if (epc.isEmpty() || !m_sentEpcs.contains(epc)) return false;
    const AppConfig& cfg = ConfigManager::instance()->config();
    const int ttlMs = cfg.plcInFlightTimeoutMs > 0 ? cfg.plcInFlightTimeoutMs : PLC_INFLIGHT_TIMEOUT_MS;
    const qint64 sentAt = m_sentAtMs.value(epc, 0);
    if (sentAt <= 0) return true;
    return (QDateTime::currentMSecsSinceEpoch() - sentAt) <= ttlMs;
}


int HttpServer::rfidPushPerMinute() const
{
    std::lock_guard<std::mutex> lock(m_rfidPushMutex);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 cutoff = now - 60 * 1000;
    // 清理窗口外的时间戳（与写入侧都清理，窗口始终 ≤ 1 分钟数据）
    while (!m_rfidPushTimes.empty() && m_rfidPushTimes.front() < cutoff)
        m_rfidPushTimes.pop_front();
    return (int)m_rfidPushTimes.size();
}

int HttpServer::peakPerMinuteToday() const
{
    std::lock_guard<std::mutex> lock(m_rfidPushMutex);
    return m_peakPerMinuteToday;
}

void HttpServer::persistDailyPeak()
{
    std::lock_guard<std::mutex> lock(m_rfidPushMutex);
    if (m_peakPerMinuteToday > 0 && m_pSortingDb && m_pSortingDb->isOpen())
    {
        m_pSortingDb->saveDailyPeak(m_peakDate, m_peakPerMinuteToday);
        HTTP_LOG_INFO("效率统计 峰值落库 date=%s peakPerMinute=%d",
            m_peakDate.toLocal8Bit().data(), m_peakPerMinuteToday);
    }
}

void HttpServer::efficiencySeries(int lastMinutes, QVector<int>* pLastMinute,
                                  QVector<int>* pHourPeaks) const
{
    std::lock_guard<std::mutex> lock(m_rfidPushMutex);

    if (pLastMinute)
    {
        pLastMinute->clear();
        const qint64 nowMin = QDateTime::currentMSecsSinceEpoch() / 60000;
        // 不足 N 个桶（凌晨/开机初期）前补 0，保证 x 轴刻度稳定
        for (qint64 m = nowMin - lastMinutes + 1; m <= nowMin; ++m)
            pLastMinute->append(m_rfidMinuteCount.value(m, 0));
    }

    if (pHourPeaks)
    {
        pHourPeaks->fill(0, 24);
        for (auto it = m_rfidMinuteCount.constBegin(); it != m_rfidMinuteCount.constEnd(); ++it)
        {
            int hour = QDateTime::fromMSecsSinceEpoch(it.key() * 60000).time().hour();  // 本地小时 0..23
            if (hour < 0 || hour > 23) continue;
            if (it.value() > (*pHourPeaks)[hour])
                (*pHourPeaks)[hour] = it.value();
        }
    }
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
void HttpServer::onFullboxReplyFinished(const QString& msgId, bool success, int httpStatus,
                                        const QString& body, const QString& note)
{
    // ★ 2026-09-22 现场需求①：入口即把本次回传的**完整响应**留痕到 outbox_fullbox.resp_*
    //   （全量落库，不截断）；放在状态处理之前，保证"查看 → 双击"看到的一定是最近一次响应。
    if (m_pSortingDb && m_pSortingDb->isOpen())
    {
        m_pSortingDb->saveOutboxResponse(msgId, /*isH7=*/true, httpStatus, body, note);
        emit outboxResponseSaved();
    }

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

        // 3. ★ 2026-09-22 现场口径（最终）：**满箱回传结果与容器绑定彻底解耦（各自独立）** ——
        //   容器号**只在锁格（=满箱）/完结补发那一刻清理**（那才是"箱子离场"的时点），
        //   回传成功/失败/重试**一律不改容器绑定**：不改内存绑定、不写 grid_box_bind、
        //   不因"成功"去解绑、也不会因迟到回执清掉期间换绑好的新容器。
        //   回传结果只做三件事：① 更新该条报文状态（成功/失败/重试次数）② 落响应留痕
        //   ③ 刷新失败重传下拉与屏障复检。容器状态变化只有三条来源：H6 绑定（新容器到场）、
        //   锁格/完结补发（容器离场）、启动归档/清空绑定/新任务/波次取消（批量清空）。
        if (bIsCurrent)
        {
            HTTP_LOG_INFO("满箱回传（H7） 成功：结果仅更新报文状态（容器绑定不受影响；容器号已在锁格/补发时清理） order=%s grid=%s box=%s msgId=%s",
                orderCode.toLocal8Bit().data(), outMsg.grid.toLocal8Bit().data(),
                boxCode.toLocal8Bit().data(), msgId.toLocal8Bit().data());
        }

        // 4. 状态恢复：FULLBOX_SYNC → SORTING（T-S5-05）—— 仅当前波次
        //    ★ 2026-09-08 幂等：满箱回传已解耦状态机（不再进入 FULLBOX_SYNC），
        //      多格口并发满箱时回执乱序到达也安全（状态已是 SORTING 时直接成功）
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
            // ★ 2026-09-22 计数口径：终态标记**不再消耗重试次数**（retry_count 已由每次重发尝试 +1，
            //   这里只是把耗尽状态落库；旧实现会再加 1 → 出现"重试2/2、计数=3"的漂移）
            m_pSortingDb->setOutboxFullboxStatus(msgId, "failed",
                QDateTime::currentDateTime().addSecs(OUTBOX_RETRY_INTERVAL_SEC)
                    .toString("yyyy-MM-dd HH:mm:ss"));

            HTTP_LOG_ERROR("满箱回传失败（H7） 重试耗尽 order=%s grid=%s retry=%d body=%s",
                outMsg.orderCode.toLocal8Bit().data(),
                outMsg.grid.toLocal8Bit().data(),
                outMsg.retryCount, body.left(200).toLocal8Bit().data());
            emit logMessage(QString("[满箱回传] 重试耗尽 order=%1 格口%2 retry=%3"
                                    "（失败报文可在「重传满箱切换(H7)」下拉中选择重传，分拣继续）")
                .arg(outMsg.orderCode)
                .arg(outMsg.grid.isEmpty() ? QString::fromUtf8("?") : outMsg.grid)
                .arg(outMsg.retryCount), true);
            emit outboxFailedChanged();   // ★ 2026-09-08 UI 刷新失败格口下拉
            // ★ 2026-09-16 需求④：「一键满箱回传」计数归因 —— 只对"本次一键生成的 msgId"计失败，
            //   其它来源（自动满箱/手动重传）由 UI 只刷新"本波次"总数，不污染本次一键数字
            emit fullboxMessageFailed(msgId, outMsg.orderCode, outMsg.grid);

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
            // ★ 2026-09-06 归属防护：该波次已非当前内存波次时不动状态（切回时按 DB 状态恢复）
            // ★ 2026-09-08 解耦后使用幂等 resumeSorting()：多格口并发满箱时状态可能已是 SORTING
            bool bIsCurrent = m_pWaveMgr && m_pWaveMgr->orderCode() == outMsg.orderCode;
            if (bIsCurrent && m_pWaveMgr)
            {
                if (m_pWaveMgr->resumeSorting())
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

    // ★ 2026-09-22 现场口径（最终定稿）：H7 的结果**不影响**完结回传（H8）——
    //   满箱回传成功/失败只更新它自己那条报文（状态/响应留痕/失败下拉）；
    //   完结回传由「结束任务」后的固定 1 秒到点强制发送，与本回执无关（结果与完结回传分开）。
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
        // ★ 2026-09-22 现场需求（R2）：**不再**因"非当前波次"把待重试消息标记 cancelled ——
        //   满箱回传报文与箱号必须保留：失败自动重试、重试耗尽才落 failed 并进入
        //   「重传满箱切换(H7)」等人工回传（上限判定见 onFullboxReplyFinished，超限即转 failed，不会无限重发）。
        if (!currentOrderCode.isEmpty() && msg.orderCode != currentOrderCode)
        {
            HTTP_LOG_INFO("满箱回传 Outbox 重试历史波次消息（H7） msgOrder=%s currentOrder=%s retry=%d/%d（保留可重试，不再取消）",
                msg.orderCode.toLocal8Bit().data(), currentOrderCode.toLocal8Bit().data(),
                msg.retryCount, OUTBOX_RETRY_MAX_H7);
        }

        // ★ 2026-09-22 现场需求：**上限兜底**（"每次失败回传独立消耗次数"必须真的能收口）——
        //   上限判定原先只在"收到回执"时做；若回执丢失（超时无回调/进程被强杀在途），
        //   消息会永远 pending 并被反复重发。这里补上：已达上限 → 直接落 failed + 留痕，不再重发。
        if (msg.retryCount >= OUTBOX_RETRY_MAX_H7)
        {
            HTTP_LOG_ERROR("满箱回传 Outbox 重试次数已达上限（H7）→ 置 failed（等人工重传） msgId=%s order=%s grid=%s retry=%d/%d",
                msg.msgId.toLocal8Bit().data(), msg.orderCode.toLocal8Bit().data(),
                msg.grid.toLocal8Bit().data(), msg.retryCount, OUTBOX_RETRY_MAX_H7);
            m_pSortingDb->setOutboxFullboxStatus(msg.msgId, "failed",
                QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
            if (m_pSortingDb)
            {
                ExceptionRecord ex;
                ex.type      = QString::fromUtf8("满箱回传失败");
                ex.orderCode = msg.orderCode;
                ex.epc       = "";
                ex.sku       = "";
                ex.reason    = QString("满箱回传（H7）重试次数达上限(%1)仍未成功 → 置失败，等待人工重传 grid=%2 box=%3")
                                   .arg(OUTBOX_RETRY_MAX_H7).arg(msg.grid).arg(msg.boxcode);
                ex.time      = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
                m_pSortingDb->insertException(ex);
            }
            emit outboxFailedChanged();
            continue;
        }

        // 解析 payload
        QJsonDocument doc = QJsonDocument::fromJson(msg.payload.toUtf8());
        if (doc.isNull() || !doc.isObject())
        {
            HTTP_LOG_WARN("满箱回传 Outbox payload解析失败（H7）");
            // 标记为 failed（★ 非"重发尝试"，不消耗重试次数）
            if (m_pSortingDb)
                m_pSortingDb->setOutboxFullboxStatus(msg.msgId, "failed",
                    QDateTime::currentDateTime().addSecs(OUTBOX_RETRY_INTERVAL_SEC)
                        .toString("yyyy-MM-dd HH:mm:ss"));
            continue;
        }

        QJsonObject payload = doc.object();

        // 更新重试信息（★ 本次确实要"重发一次" → 消耗一次重试次数，retry_count + 1）
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

    // ★ 2026-09-17 现场要求（"跳过一个波次"缺陷）：点「结束任务」= 本次接收会话开始收尾，
    //   在此**立即冻结待执行队列**——从这一刻起队首不再出队。必须在这里置位，因为接收层
    //   还要等 H8 回传结果才 stopReceive()（此间 m_receiving 仍为 true），而 H8 成功回调里
    //   本波次就会变**已完结**；旧实现正是借此把队首重放注册，造成"界面显示开始接收任务、
    //   系统却已自动接收下一波"，操作员再点开始又执行下一个队首 → 中间那一波被跳过。
    //   解冻的唯一入口：下一次 startReceive()（点「开始接收任务」）。
    //   判据实现见 PendingWaveQueuePolicy.h / HttpServer::maybeStartPendingWave。
    m_endRequested.store(true);
    if (!m_pendingWaveQueue.isEmpty())
    {
        HTTP_LOG_INFO("点「结束任务」：待执行队列 %d 个已冻结（本次会话不再出队；点「开始接收任务」后按队首顺序执行）",
            m_pendingWaveQueue.size());
        emit logMessage(QString::fromUtf8(
            "[波次] 待执行队列 %1 个已冻结——点「结束任务」后不再自动执行，"
            "点「开始接收任务」才会依次执行队首").arg(m_pendingWaveQueue.size()));
    }

    // ★ 2026-09-22 幂等：本波次的"固定延迟发送"已在排队（1 秒窗口内又点了一次「结束任务」）——
    //   直接返回 true，不重复补发 H7、不重复登记，也不打"触发失败"的误导日志。
    if (m_endDelayTimer && m_endDelayTimer->isActive()
        && m_endDelayOrderCode == m_pWaveMgr->orderCode())
    {
        HTTP_LOG_INFO("点「结束任务」重复触发：波次 %s 的完结回传已在固定延迟排队中（%dms），忽略本次",
            m_endDelayOrderCode.toLocal8Bit().data(),
            ConfigManager::instance()->config().endReportDelayMs);
        emit logMessage(QString("[完结回传] 波次 %1 的完结回传已在排队（满箱回传补发后 %2 秒发送），"
                                "本次重复点击已忽略").arg(m_endDelayOrderCode)
                            .arg(ConfigManager::instance()->config().endReportDelayMs / 1000.0, 0, 'f', 1));
        return true;
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

    // ★ 2026-09-08 需求4配套（防重复 H8）：波次已处于「完结中(ENDING)」时，
    //   若该波次已存在未成功的 H8 报文（FAILED 重试耗尽 / pending 在途 / cancelled），
    //   本次不再新建第二条 H8（否则会向 WMS 重复回传同一波次的完结报文）。
    //   返回 false → 调用方（MainWindow）立即收尾停止接收；失败报文用「重传任务完结(H8)」下拉补发。
    if (status == WAVE_ENDING && m_pSortingDb && m_pSortingDb->isOpen())
    {
        QVector<OutboxRecord> endMsgs = m_pSortingDb->getOutboxEndByOrder(m_pWaveMgr->orderCode());
        bool hasUnfinished = false;
        QString unfinishedStatus;
        for (const OutboxRecord& r : endMsgs)
        {
            if (r.status != "success") { hasUnfinished = true; unfinishedStatus = r.status; break; }
        }
        if (hasUnfinished)
        {
            HTTP_LOG_WARN("完结回传（H8） 已有未成功报文，跳过重复发送 order=%s status=%s",
                m_pWaveMgr->orderCode().toLocal8Bit().data(), unfinishedStatus.toLocal8Bit().data());
            emit logMessage(QString("[完结回传] 波次 %1 已有未成功的完结回传报文（%2），本次不再重复发送——"
                                    "可在「重传任务完结(H8)」下拉中选择该波次补发")
                .arg(m_pWaveMgr->orderCode()).arg(unfinishedStatus), true);
            emit outboxFailedChanged();
            return false;
        }
    }

    // ★ 2026-09-07 「结束任务」统一满箱回传（2026-09-26 口径）：对**当前所有已绑定容器**做一次
    //   「一键满箱回传」（有分拣记录才生成报文；未绑定但有未上传记录的格口跳过并留痕），再发 H8
    {
        int flushedGrids = flushUnreportedFullboxes(m_pWaveMgr->orderCode());
        if (flushedGrids > 0)
        {
            emit logMessage(QString("[完结前补发] 已对已绑定容器完成满箱回传（H7）：本次生成 %1 条报文，"
                                    "随后发送完结回传（H8）")
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

    // ════════════════════════════════════════════════════════════════════════
    // ★ 2026-09-26 现场口径（定稿）：**已绑定容器统一做一遍满箱回传 → 固定 2 秒后强制发送完结回传**
    //   ① 上面 flushUnreportedFullboxes() 已对**当前所有已绑定容器**做了一遍 H7
    //      （= 「一键满箱回传」同一实现；报文+箱号入 outbox_fullbox 并立即异步发出，不看结果）；
    //   ② 这里启动**单次**定时器：END_REPORT_DELAY_MS（默认 2000ms，配置 endReportDelayMs）后，
    //      **不论 H7 是否成功**，都生成并发送 H8；
    //      （为什么 2 秒：补发范围从"有记录的格口"扩到"全部已绑定容器"，报文最多 66 条，1 秒窗口偏紧）
    //   ③ **结果与完结回传分开**：H7 的成功/失败/重试/人工补传只影响它自己那条报文，
    //      全程不阻塞、不改写、不取消 H8（H8 自己按 outbox_end 重试）。
    //   顺序保证：H7 全部先发出 → 隔 2 秒 → H8 ⇒ H8 仍是本波次最后一条报文。
    // ════════════════════════════════════════════════════════════════════════
    const QString endOrder = m_pWaveMgr->orderCode();
    const int delayMs = ConfigManager::instance()->config().endReportDelayMs;   // 0=补发完立即发

    if (delayMs > 0 && m_endDelayTimer)
    {
        m_endDelayOrderCode = endOrder;
        m_endDelayTimer->start(delayMs);
        HTTP_LOG_INFO("完结回传（H8）已按固定延迟排队 order=%s delay=%dms（不论 H7 是否成功均会发送）",
            endOrder.toLocal8Bit().data(), delayMs);
        emit logMessage(QString("[完结回传] 满箱回传已统一补发，将在 %1 秒后发送完结回传（H8）"
                                "—— 不论满箱回传是否成功，H7 结果与完结回传互不影响")
            .arg(delayMs / 1000.0, 0, 'f', 1));
        return true;   // 已进入完结流程（延迟到点即发送 H8）
    }

    return emitEndReportNow();
}

// ============================================================================
// onEndDelayTimeout — 固定延迟到点：留痕（如有未成功的 H7）+ **强制发送**完结回传（H8）
//   ★ 现场口径：不论 H7 是否成功都发；未成功清单只写异常留痕 + 界面提示，不做等待/拦截/弹窗。
// ============================================================================
void HttpServer::onEndDelayTimeout()
{
    const QString order = m_endDelayOrderCode;
    m_endDelayOrderCode.clear();
    if (order.isEmpty()) return;

    // 归属防护：这一秒内若已切出/新任务/取消，则不再补发该波次的 H8（报文仍在 Outbox，可面板补传）
    if (!m_pWaveMgr || m_pWaveMgr->orderCode() != order)
    {
        HTTP_LOG_WARN("完结回传（H8）延迟到点但该波次已非当前内存波次 order=%s current=%s —— 不再发送"
                      "（可用「重传任务完结(H8)」补发）",
            order.toLocal8Bit().data(),
            (m_pWaveMgr ? m_pWaveMgr->orderCode() : QString()).toLocal8Bit().data());
        return;
    }
    if (m_pWaveMgr->status() != WAVE_ENDING)
    {
        HTTP_LOG_WARN("完结回传（H8）延迟到点但波次状态已非「完结中」 order=%s status=%d —— 不再发送",
            order.toLocal8Bit().data(), m_pWaveMgr->status());
        return;
    }

    // ★ 结果与完结回传分开：这里**只看**"有没有未成功的 H7"，用于留痕/提示；**不等待、不拦截**
    QString detail;
    const int unfinished = collectUnfinishedFullbox(order, &detail);
    if (unfinished > 0)
    {
        HTTP_LOG_WARN("完结回传（H8）发送前：仍有未成功的满箱回传（H7）%d 项 —— 照常发送 H8（结果分开）明细=%s",
            unfinished, detail.toLocal8Bit().data());
        emit logMessage(QString("[完结回传] 仍有未成功的满箱回传（H7）：%1 —— 照常发送完结回传（H8）；"
                                "这些报文可在「重传满箱切换(H7)」稍后补传")
            .arg(detail), true);
        writeUnfinishedFullboxNotice(order, detail);
    }

    HTTP_LOG_INFO("完结回传（H8）固定延迟到点 → 发送 order=%s 未成功H7=%d", 
        order.toLocal8Bit().data(), unfinished);
    emitEndReportNow();
}

// ============================================================================
// cancelPendingEndReport — 取消尚未到点的延迟发送（波次切出/取消(H5)/新任务时调用）
//   注意：已经发出的 H7 报文与箱号不受影响；已入库的 H8 报文也可由面板补发。
// ============================================================================
void HttpServer::cancelPendingEndReport(const QString& why)
{
    if (!m_endDelayTimer || !m_endDelayTimer->isActive())
    {
        m_endDelayOrderCode.clear();
        return;
    }
    m_endDelayTimer->stop();
    const QString order = m_endDelayOrderCode;
    m_endDelayOrderCode.clear();
    HTTP_LOG_WARN("完结回传（H8）延迟发送已取消（%s） order=%s（未发送；可由「重传任务完结(H8)」补发）",
        why.toLocal8Bit().data(), order.toLocal8Bit().data());
    emit logMessage(QString("[完结回传] 波次 %1 的延迟发送已取消（%2）—— 未发送完结回传，"
                            "如需完结请在面板用「重传任务完结(H8)」补发").arg(order, why), true);
}

// ============================================================================
// emitEndReportNow — 完结回传（H8）的**唯一**生成 + 发送出口
//   调用点：① sendEnd()（延迟为 0 时直发）；② onEndDelayTimeout()（固定延迟到点，**不论 H7 是否成功**）。
//   保证：H8 一定在所有满箱回传（H7）报文之后生成/发送（最后一条报文）；
//         且**不读取任何 H7 状态** ⇒ 个别格口一直失败也挡不住完结回传（结果与完结回传分开）。
// ============================================================================
bool HttpServer::emitEndReportNow()
{
    QElapsedTimer funcTimer;
    funcTimer.start();
    qint64 epochMs = QDateTime::currentMSecsSinceEpoch();

    if (!m_pWaveMgr)
    {
        HTTP_LOG_WARN("完结回传触发失败（H8） WaveManager未初始化");
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

    // ★ 2026-09-22：H8 已生成并发出（本波次最后一条报文）；无需再收敛任何"屏障"状态
    //   （完结回传已与满箱回传结果完全分开）。
    return true;   // ★ 2026-09-02：已成功进入完结回传流程
}

// ════════════════════════════════════════════════════════════════════════════
// ★ 2026-09-22 现场口径（最终定稿）：未成功的满箱回传（H7）**统计 + 留痕**（仅供展示/追溯）
//   注意：这两个函数**不参与任何等待或拦截** —— 完结回传（H8）由「结束任务」后的固定延迟强制发送，
//   与这里的统计结果无关（结果与完结回传分开）。
// ════════════════════════════════════════════════════════════════════════════

// collectUnfinishedFullbox — 返回未成功的 H7 条数（含"报文未生成"的内存记录），明细写入 *detailOut
int HttpServer::collectUnfinishedFullbox(const QString& orderCode, QString* detailOut)
{
    QStringList parts;      // 明细片段（格口/箱号/状态/次数）—— 现场要求"提示对应格口号"
    int nInflight = 0, nFailed = 0, nUnbuilt = 0;

    if (m_pSortingDb && m_pSortingDb->isOpen() && !orderCode.isEmpty())
    {
        const QVector<OutboxRecord> rows = m_pSortingDb->getOutboxFullboxByOrder(orderCode);
        for (const OutboxRecord& r : rows)
        {
            if (r.status == "success") continue;
            const QString gridKey = r.grid.isEmpty() ? QString::fromUtf8("?") : normalizeGridKey(r.grid);
            const QString item = QString::fromUtf8("格口%1(%2,%3,%4/%5)")
                                     .arg(gridKey, r.boxcode.isEmpty() ? QString::fromUtf8("?") : r.boxcode,
                                          r.status)
                                     .arg(r.retryCount)
                                     .arg(OUTBOX_RETRY_MAX_H7);
            if (r.status == "pending")
            {
                ++nInflight;
                parts << QString::fromUtf8("[重试中]") + item;
            }
            else
            {
                ++nFailed;
                parts << QString::fromUtf8("[未成功]") + item;
            }
        }
    }

    // 报文未生成（记录仍留在内存）：缺 SKU / 无容器绑定 / Outbox 写失败等
    {
        std::lock_guard<std::mutex> lock(m_gridRecordMutex);
        for (auto it = m_gridSortRecords.constBegin(); it != m_gridSortRecords.constEnd(); ++it)
        {
            if (it.value().isEmpty()) continue;
            ++nUnbuilt;
            parts << QString::fromUtf8("[未生成报文]格口%1(%2件)")
                         .arg(normalizeGridKey(it.key())).arg(it.value().size());
        }
    }

    if (detailOut)
    {
        if (parts.isEmpty())
            detailOut->clear();
        else
            detailOut->append(parts.join(QString::fromUtf8("；"))
                              + QString::fromUtf8("（重试中%1/未成功%2/未生成%3）")
                                    .arg(nInflight).arg(nFailed).arg(nUnbuilt));
    }

    // ★ 纯统计：返回"未成功"的条数（重试中 + 未成功 + 未生成报文）—— **不参与任何等待/拦截**
    return nInflight + nFailed + nUnbuilt;
}

// writeUnfinishedFullboxNotice — 留痕：发 H8 前把"未成功的满箱回传清单"写异常表（记录用，不改变发送与否）
void HttpServer::writeUnfinishedFullboxNotice(const QString& orderCode, const QString& detail)
{
    if (!m_pSortingDb || !m_pSortingDb->isOpen() || orderCode.isEmpty()) return;
    ExceptionRecord ex;
    ex.type      = QString::fromUtf8("满箱回传未完成");
    ex.orderCode = orderCode;
    ex.epc       = "";
    ex.sku       = "";
    ex.reason    = QString::fromUtf8("完结回传（H8）发送时仍有未成功的满箱回传：%1 —— "
                                     "已按现场口径照常发送完结回传（结果与完结回传分开）；"
                                     "未成功报文与箱号保留在 Outbox，可用「重传满箱切换(H7)」补传")
                       .arg(detail.isEmpty() ? QString::fromUtf8("(无明细)") : detail);
    ex.time      = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    m_pSortingDb->insertException(ex);
    HTTP_LOG_WARN("[完结回传] 已留痕（未成功的满箱回传）order=%s 明细=%s",
        orderCode.toLocal8Bit().data(), detail.toLocal8Bit().data());
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

    // ★ 2026-09-08 需求4修正：超时兜底**不再把波次置为「已完成」**——
    //   仅有 H8 成功回执才置 FINISHED（见成功分支）；此处只结束"等待会话"，H8 未确认报文
    //   仍在 outbox_end 由定时器继续自动重试，或人工在「重传任务完结(H8)」下拉中补发。
    //   ★ 2026-09-06 防护：若内存波次已不在 ENDING（等待期间执行了「新任务/切换」），不做任何状态处理
    if (m_pWaveMgr && m_pWaveMgr->status() == WAVE_ENDING)
    {
        HTTP_LOG_WARN("完结回传（H8） 会话超时 波次保持 ENDING 不变（未置已完成、未归档）order=%s"
                      "（H8保留outbox继续重试/可下拉补发）",
            orderCode.toLocal8Bit().data());
        emit logMessage(QString("[完结回传] 等待超时：波次 %1 保持「完结中」未置已完成"
                                "（H8 继续自动重试，或可在下拉中手动重传）")
            .arg(orderCode), true);
    }
    else
    {
        HTTP_LOG_WARN("完结回传（H8） 会话超时但内存波次已不在完结等待（可能已切出/新任务），跳过状态处理 order=%s current=%d",
            orderCode.toLocal8Bit().data(),
            m_pWaveMgr ? m_pWaveMgr->status() : -1);
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
void HttpServer::onEndReplyFinished(const QString& msgId, bool success, int httpStatus,
                                    const QString& body, const QString& note)
{
    // ★ 2026-09-22 现场需求①：入口即把本次回传的**完整响应**留痕到 outbox_end.resp_*
    //   （全量落库，不截断）；放在状态处理之前，保证"查看 → 双击"看到的一定是最近一次响应。
    if (m_pSortingDb && m_pSortingDb->isOpen())
    {
        m_pSortingDb->saveOutboxResponse(msgId, /*isH7=*/false, httpStatus, body, note);
        emit outboxResponseSaved();
    }

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

        // ★ 2026-09-22：H8 已成功（本波次最后一条报文）—— 无屏障需要收敛；延迟发送器也已自然结束
        if (m_endDelayTimer && m_endDelayTimer->isActive())
            m_endDelayTimer->stop();
        m_endDelayOrderCode.clear();

        // ★ 完结回传成功后停止 Outbox 重试定时器，新波次下发时重新启动
        if (m_outboxEndTimer)     m_outboxEndTimer->stop();
        // ★ 2026-09-22 现场需求（R2）：H7 报文与箱号必须保留可重试 —— 若该波次仍有未成功的满箱回传，
        //   **不停止** H7 重试轮询（否则失败报文永远等不到自动重试/人工回传）。
        bool bHasUnfinishedH7 = false;
        if (m_pSortingDb && m_pSortingDb->isOpen())
        {
            const QVector<OutboxRecord> rows = m_pSortingDb->getOutboxFullboxByOrder(outMsg.orderCode);
            for (const OutboxRecord& r : rows)
            {
                if (r.status != "success") { bHasUnfinishedH7 = true; break; }
            }
        }
        if (m_outboxFullboxTimer)
        {
            if (bHasUnfinishedH7)
            {
                HTTP_LOG_WARN("完结回传（H8） 该波次仍有未成功的满箱回传（H7）——保留重试轮询继续自动重试 order=%s",
                    outMsg.orderCode.toLocal8Bit().data());
                if (!m_outboxFullboxTimer->isActive())
                    m_outboxFullboxTimer->start(OUTBOX_POLL_INTERVAL_SEC * 1000);
            }
            else
            {
                m_outboxFullboxTimer->stop();
                HTTP_LOG_INFO("完结回传（H8） Outbox重试定时器已停止 order=%s（满箱回传已全部成功）",
                    outMsg.orderCode.toLocal8Bit().data());
            }
        }

        // ★ 波次完结后清理运行时状态（与 H4 新波次下发的重置逻辑一致）
        {
            std::lock_guard<std::mutex> lock(m_gridRecordMutex);
            m_gridSortRecords.clear();
        }
        {
            std::lock_guard<std::mutex> lock(m_gridCountMutex);
            m_gridSortedCount.clear();
        }
        clearBoxLandedCount();   // ★ 2026-09-13 计划数封顶计数随波次清理
        clearGridLandedCount();  // ★ 2026-09-14 按格口落格计数（多格口分配依据）随波次清理
        clearLandingDedup();     // ★ 2026-09-14 落格明细去重集合随波次清理
        // ★ 2026-09-14 计划分配表：完结前出报告（本波次"每格口计划/实际"终版对账），再清表
        reportPlanAlloc(QString::fromUtf8("波次完结"));
        clearPlanAllocTable(QString::fromUtf8("波次完结"));
        if (m_pPlcMgr)
            m_pPlcMgr->enableAllGrids();
        m_pendingSkuQuery.clear();
        m_skuQueryRetryCount.clear();
        m_notReadyRetryCount.clear();
        clearAllEpcRuntimeState();   // ★ 2026-09-11 在途/冷却/重发计数一并清空
        // ★ 2026-09-07 客户确认：完结后不清空容器绑定——绑定信息保留在内存/数据库，
        //   供追溯与切回查看；下一波次由 H6 按格口重新绑定覆盖
        HTTP_LOG_INFO("波次完结清理完成 order=%s（格口记录/计数/待查SKU/重试/在途EPC 清空；容器绑定保留）",
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
        // ★ 2026-09-08：先记录"本结果是否属于正在等待的会话"，供非当前波次分支决定是否通知 UI 收尾
        const bool wasWaitingSession = (!m_endSessionOrderCode.isEmpty() && m_endSessionOrderCode == orderCode);
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
            // ★ 2026-09-22 计数口径：终态标记不消耗重试次数（retry_count 已由每次重发 +1）
            m_pSortingDb->setOutboxEndStatus(msgId, "failed",
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

            // ★ 2026-09-08 需求4修正：该波次已非当前内存波次（已切出）→ **不做任何状态终态化/归档**，
            //   仅记录失败事实（流水/日志），波次保持原状态，可在「重传任务完结(H8)」下拉中按波次补发
            bool bIsCurrent = m_pWaveMgr && m_pWaveMgr->orderCode() == orderCode;
            if (!bIsCurrent)
            {
                HTTP_LOG_WARN("完结回传失败耗尽（H8） 且该波次已非当前内存波次 order=%s"
                              "（保持原状态不变，未终态化/未归档；可用下拉手动补发）",
                    orderCode.toLocal8Bit().data());
                emit logMessage(QString("[完结回传] order=%1 补发失败（该波次已切出，状态保持不变；"
                                        "可在「重传任务完结(H8)」下拉中重传）")
                    .arg(orderCode), true);
                emit outboxFailedChanged();   // ★ UI 刷新失败波次下拉
                // ★ 属于正在等待的会话 → 仍需通知 UI 收尾（否则「结束任务」会等满 30s 兜底）
                if (wasWaitingSession)
                    emit endReportFinished();
                return;
            }

            // ★ 2026-09-08 需求4修正：完结回传失败 → **保持原状态（完结中 ENDING）不变，不再推到「已完成」**。
            //   只有 H8 成功回执才置 FINISHED + 归档（见成功分支）；
            //   failed 报文保留在 outbox_end，可用「重传任务完结(H8)」下拉按波次手动补发；
            //   重传成功后仍走成功分支 → FINISHED + archiveWave。
            //   注意：不清理内存运行态（便于人工核查/手输格口满箱），也不 archiveWave。
            HTTP_LOG_WARN("完结回传失败（H8） 重试耗尽，波次状态保持 %s 不变（不置已完成）order=%s"
                          "（failed报文可用「重传任务完结(H8)」下拉补发）",
                WaveSnapshot::statusToString(m_pWaveMgr ? m_pWaveMgr->status() : WAVE_ENDING).toLocal8Bit().data(),
                orderCode.toLocal8Bit().data());
            emit logMessage(QString("[完结回传] 重试耗尽 order=%1——波次保持「%2」未置已完成；"
                                    "可在「重传任务完结(H8)」下拉中选择该波次补发")
                .arg(orderCode)
                .arg(WaveSnapshot::statusToString(m_pWaveMgr ? m_pWaveMgr->status() : WAVE_ENDING)), true);
            emit outboxFailedChanged();   // ★ UI 刷新失败波次下拉

            // ★ 2026-09-02：H8 会话已结束，停止兜底定时器
            if (m_endSessionTimer) m_endSessionTimer->stop();

            // ★ 重试耗尽后停止 H8 完结回传定时器（已标记 failed，不再需要轮询；手动补发走下拉/面板）
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
        // ★ 2026-09-22 现场需求：上限兜底（回执丢失时不能无限重发）—— 已达上限直接落 failed + 留痕
        if (msg.retryCount >= OUTBOX_RETRY_MAX_DEFAULT)
        {
            HTTP_LOG_ERROR("完结回传 Outbox 重试次数已达上限（H8）→ 置 failed（等人工重传） msgId=%s order=%s retry=%d/%d",
                msg.msgId.toLocal8Bit().data(), msg.orderCode.toLocal8Bit().data(),
                msg.retryCount, OUTBOX_RETRY_MAX_DEFAULT);
            m_pSortingDb->setOutboxEndStatus(msg.msgId, "failed",
                QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
            if (m_pSortingDb)
            {
                ExceptionRecord ex;
                ex.type      = QString::fromUtf8("完结回传失败");
                ex.orderCode = msg.orderCode;
                ex.epc       = "";
                ex.sku       = "";
                ex.reason    = QString("完结回传（H8）重试次数达上限(%1)仍未成功 → 置失败，等待人工重传")
                                   .arg(OUTBOX_RETRY_MAX_DEFAULT);
                ex.time      = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
                m_pSortingDb->insertException(ex);
            }
            emit outboxFailedChanged();
            continue;
        }

        // ★ 非当前波次的消息：标记为 cancelled，跳过重试
        //   防止软件重启后发送上一波次未回传的完结数据
        //   （★ 2026-09-22：终态标记不消耗重试次数）
        if (currentOrderCode.isEmpty() || msg.orderCode != currentOrderCode)
        {
            HTTP_LOG_INFO("完结回传 Outbox 跳过非当前波次消息（H8） msgOrder=%s currentOrder=%s",
                msg.orderCode.toLocal8Bit().data(),
                currentOrderCode.toLocal8Bit().data());
            if (m_pSortingDb)
                m_pSortingDb->setOutboxEndStatus(msg.msgId, "cancelled",
                    QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
            emit outboxFailedChanged();   // ★ 2026-09-08 cancelled 同样进入失败重传下拉
            continue;
        }

        // 解析 payload
        QJsonDocument doc = QJsonDocument::fromJson(msg.payload.toUtf8());
        if (doc.isNull() || !doc.isObject())
        {
            HTTP_LOG_WARN("完结回传 Outbox payload解析失败（H8）");
            if (m_pSortingDb)
                m_pSortingDb->setOutboxEndStatus(msg.msgId, "failed",
                    QDateTime::currentDateTime().addSecs(OUTBOX_RETRY_INTERVAL_SEC)
                        .toString("yyyy-MM-dd HH:mm:ss"));
            continue;
        }

        QJsonObject payload = doc.object();

        // 更新重试信息（★ 2026-09-02：H8 用快速重试间隔 5s，保证失败后尽快重发，
        //   避免旧逻辑 +30s 造成"结束任务"等待窗口内长时间无重试）
        //   ★ 2026-09-22：本次确实要"重发一次" → 消耗一次重试次数（retry_count + 1）
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
    // ★ 2026-09-13 性能核验：主线程事件滞后采样
    //   RfidPushClient 在 HP-Socket 工作线程解析帧时把解析时刻写进 body.recvMs；
    //   本函数在主线程执行，两者之差 = "业务入口等主线程"的滞后（UI 变慢会体现在这里）
    {
        const qint64 recvMs = (qint64)body.value("recvMs").toDouble(0);
        if (recvMs > 0)
            perfSampleEventLag((int)(QDateTime::currentMSecsSinceEpoch() - recvMs));
    }

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
    // ★ 2026-09-08 可发送执行态：SORTING 或 FULLBOX_SYNC（满箱同步期间分拣/下发照常，与落格反馈口径一致）
    bool bSorting = (waveStatus == WAVE_SORTING || waveStatus == WAVE_FULLBOX_SYNC);
    if (!bSorting)
    {
        // ★ 2026-09-08 说明：RFID 接收/存储/SKU 绑定查询不受任何状态影响（一直接收、一直通畅）；
        //   仅"向 PLC 发送指令"限分拣中（安全设计：非分拣状态无格口映射/格口禁用，下发会错乱）。
        //   已就绪的 EPC 将挂起，恢复分拣后自动补发 PLC——不会不处理、不会丢数据
        HTTP_LOG_INFO("RFID推送 已接收(非SORTING) status=%d(%s) orderCode=%s——已存储+已查绑定，未发送PLC(恢复分拣后自动补发)",
            waveStatus, WaveSnapshot::statusToString(waveStatus).toLocal8Bit().data(),
            m_pWaveMgr ? m_pWaveMgr->orderCode().toLocal8Bit().data() : "(null)");
        emit logMessage(QString("[RFID] 非分拣中(status=%1)：已接收并存储、绑定查询已提交；未发送PLC（恢复分拣后自动补发）")
            .arg(WaveSnapshot::statusToString(waveStatus)));
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
        // ★ 2026-09-15 原始报文留痕字段（TCP 推送入口带；HTTP 直推入口天然为空）
        const QString epcRaw   = item["epcRaw"].toString().trimmed();
        const QString devCode  = item["devCode"].toString().trimmed();
        const QString rawFrame = item["raw"].toString();
        const bool    noread   = item["noread"].toBool();

        // ★ 2026-09-15 EPC 识别（同一口径，两个入口一致）：
        //   · TCP 推送入口：RfidPushClient 已识别归一（此处为幂等复核，不会二次改写）
        //   · HTTP 直推入口：原始串在此识别，保证与推送侧同口径
        //   · 未识别（形态不符）→ 不猜，按原文继续并告警（原文在 rfid_raw 表可查）
        const int epcLen = epcTruncateLen();
        if (!epc.isEmpty() && epcLen >= 2 && !noread)
        {
            QString cut;
            if (EpcCode::extract(epc, epcLen, cut) && cut != epc)
            {
                HTTP_LOG_WARN("EPC识别归一(HTTP直推) seq=%s 原文(%d位)=%s → EPC(%d位)=%s",
                    seq.toLocal8Bit().data(), epc.size(), epc.toLocal8Bit().data(),
                    cut.size(), cut.toLocal8Bit().data());
                epc = cut;
            }
        }

        // ★ 2026-09-15 逐帧原始报文入队（O(1)，由定时器/满批单事务落库到 rfid_raw 表）
        //   NOREAD 帧同样留痕（epc 空 + noread=1），但不进入分拣
        //   注：HTTP 直推入口不带整帧原文 → rawFrame 留空（不臆造），仅按结构化字段留痕
        noteRfidRawFrame(epc, epcRaw, carNum, seq, devCode, rawFrame, noread);

        if (epc.isEmpty()) continue;   // 空 EPC（NOREAD 帧/无 EPC 推送）→ 已留痕，不进入分拣
        recordRfidPush();   // ★ 2026-09-07 效率统计：每收到一件(RFID含EPC推送)记一次，供 1 分钟滑动窗口吞吐显示
        // ★ 2026-09-07 需求：把 RFID 推送数据帧实时显示到 UI 运行日志（逐帧一行）
        //   ★ 2026-09-15 行内带识别前原文与整帧原文（现场无需翻日志即可核对"丢弃了什么"）
        {
            const QString rawNote = rawFrame.isEmpty()
                ? QString()
                : QString(" 原始报文=%1").arg(rawFrame);
            const QString rawEpcNote = (!epcRaw.isEmpty() && epcRaw != epc)
                ? QString("（原文 %1）").arg(epcRaw)
                : QString();
            emit logMessage(QString("RFID数据帧 seq=%1 car=%2 epc=%3%4%5")
                .arg(seq).arg(carNum).arg(epc).arg(rawEpcNote).arg(rawNote));
        }
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

        // ★ 非分拣中状态：仅存储 + 查询，不发送 PLC（已就绪 EPC 挂起，恢复分拣后自动补发）
        if (!bSorting)
        {
            HTTP_LOG_INFO("RFID推送 已存储未发送(非分拣中) epc=%s carNum=%s seq=%s status=%d %s",
                epc.toLocal8Bit().data(), carNum.toLocal8Bit().data(),
                seq.toLocal8Bit().data(), waveStatus,
                needSkuQuery ? "(SKU绑定查询已提交/排队)" : "(SKU已绑定，恢复分拣后自动补发)");
        }
    }

    // ★ 2026-09-11 重扫重投：本批中"已经下发过 PLC 指令"的 EPC 再次推送 = 二次上传（拿起重新上料）
    //   → 先把计时起点归零，使 1s「RFID推送→PLC发送」超时窗口从**本次重扫**起算。
    //   否则会沿用上一次推送的 receivedAt（例如上一次被冷却拦截、未真正下发时），
    //   本次重扫会被误判"发送超时"而入异常口，导致重投失败。
    if (m_pEpcCache)
    {
        for (auto it3 = batchMap.constBegin(); it3 != batchMap.constEnd(); ++it3)
        {
            if (m_lastPlcSendMs.contains(it3.key()))   // 本波次已下发过 → 二次上传
            {
                m_pEpcCache->resetTiming(it3.key());
                HTTP_LOG_INFO("重扫重投 二次上传计时归零 epc=%s（1s 超时窗口从本次重扫起算）",
                    it3.key().toLocal8Bit().data());
            }
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
    {        HTTP_LOG_INFO("RFID推送 触发SKU查询 epcCount=%d epcList=[%s]",
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
        // ★ 非分拣中状态：已就绪（SKU+carNum 齐）的 EPC 挂起，恢复分拣后自动补发；不发送 PLC
        if (!bSorting)
        {
            if (m_pEpcCache && m_pEpcCache->isReadyForPlc(epc))
            {
                std::lock_guard<std::mutex> lock(m_pendingRfidMutex);
                m_pendingRfidPlcEpcs.insert(epc);
                HTTP_LOG_INFO("RFID推送 非分拣中已就绪→挂起待补发 epc=%s status=%d pending=%d",
                    epc.toLocal8Bit().data(), waveStatus, m_pendingRfidPlcEpcs.size());
            }
            continue;
        }

        bool isReady = m_pEpcCache && m_pEpcCache->isReadyForPlc(epc);
        
        if (!isReady)
        {
            skuNotFoundCount++;
            HTTP_LOG_WARN("RFID推送 EPC已到但SKU未绑定 epc=%s barcode=%s carNum=%s (等待RFID SKU查询结果)", 
                epc.toLocal8Bit().data(), 
                it.value().first.toLocal8Bit().data(),
                it.value().second.toLocal8Bit().data());
        }

        // ★ 防重复指令：仍在途（已下发、未收到落格反馈）时跳过 —— 防 RFID 双读/抖动；
        //   ★ 2026-09-11 重扫重投：已落格（反馈已到 → 出在途）的件再次推送到这里时不再跳过，
        //   由 trySendToPlcForEpc 按原 SKU→格口映射重新下发同一格口
        if (isEpcInFlight(epc))
        {
            HTTP_LOG_INFO("RFID推送 在途跳过 epc=%s (已下发待落格反馈，防重复指令)",
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
// 在途 EPC（m_sentEpcs：已下发未落格）跳过，防重复指令
// ★ 2026-09-11 重扫重投：已落格（出在途）的 EPC 不再跳过，由 trySendToPlcForEpc 重新下发原格口
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
        // ★ 防重复：仍在途（已下发、未收到落格反馈）时跳过
        if (isEpcInFlight(epc))
        {
            HTTP_LOG_INFO("未就绪重试 在途跳过 epc=%s (已下发待落格反馈)",
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
// ============================================================================
// ★ 2026-09-11 同波次「重扫重投」——在途集合管理
//   在途 = 已向 PLC 下发、尚未收到该件落格反馈；用途：防重复指令（RFID 双读/抖动）。
//   反馈到达即"出在途" → 该件被拿起重新上料（再次 RFID 推送）时，允许按原 SKU→格口
//   映射重新下发**同一格口**（不再被旧的"本波次已发送过"永久去重拦住）。
//   计数防重（isCodeSorted）保持不变：同波次同 EPC 仍只计 1 件、不新增流水、不进 H7 明细。
//   线程：以上集合仅在主线程访问（RFID 推送 / QTimer 重试 / 反馈批处理入口 lambda）
// ============================================================================
void HttpServer::markEpcInFlight(const QString& epc)
{
    if (epc.isEmpty()) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_sentEpcs.insert(epc);
    m_sentAtMs[epc]      = now;
    m_lastPlcSendMs[epc] = now;
}

void HttpServer::clearEpcInFlight(const QString& epc)
{
    if (epc.isEmpty()) return;
    if (m_sentEpcs.remove(epc))   // Qt5 QSet::remove 返回 bool
    {
        // ★ 2026-09-13 性能核验：下发→落格反馈时延采样（在途时刻 → 现在）
        const qint64 sentAt = m_sentAtMs.value(epc, 0);
        if (sentAt > 0)
            perfSampleFbLatency((int)(QDateTime::currentMSecsSinceEpoch() - sentAt));
        HTTP_LOG_INFO("在途解除（已收到PLC落格反馈）epc=%s——之后可重扫重投",
            epc.toLocal8Bit().data());
    }
    m_sentAtMs.remove(epc);

    // ★ 2026-09-13 需求：只要发生了落格操作（成功落格 **或** 掉入异常口），
    //   该 EPC 的超时相关限制当场归 0 ——
    //     · receivedAt 重置为当前时刻 → RFID推送→PLC发送的 1s 超时窗口（PLC_SEND_TIMEOUT_MS）重新起算
    //     · sentAt 清空           → "开始处理→发送"耗时窗口重新起算
    //   本函数由落格反馈批处理入口对**每条**反馈调用（覆盖成功/无匹配/无绑定/冲突/status 2·3 全部分支），
    //   因此"再次投放"不会再沿用上一次投递的旧计时而被误判超时。
    if (m_pEpcCache)
        m_pEpcCache->resetTiming(epc);

    // ★ 未就绪重试计数一并清理（否则跨多次投递累积，达到 NOT_READY_RETRY_MAX 后误判放弃）
    m_notReadyRetryCount.remove(epc);
}

bool HttpServer::isEpcInFlight(const QString& epc)
{
    if (epc.isEmpty() || !m_sentEpcs.contains(epc)) return false;

    const AppConfig& cfg = ConfigManager::instance()->config();
    const int ttlMs = cfg.plcInFlightTimeoutMs > 0 ? cfg.plcInFlightTimeoutMs : PLC_INFLIGHT_TIMEOUT_MS;
    const qint64 sentAt = m_sentAtMs.value(epc, 0);
    if (sentAt <= 0) return true;   // 无下发时刻记录 → 保守按在途处理（优先防重复指令）

    const qint64 ageMs = QDateTime::currentMSecsSinceEpoch() - sentAt;
    if (ageMs <= ttlMs) return true;

    // 在途超时（PLC 迟迟未反馈）→ 视为本次投递结束，允许重扫重投，避免永久锁死
    m_sentEpcs.remove(epc);
    m_sentAtMs.remove(epc);
    HTTP_LOG_WARN("在途超时解除 epc=%s 已过%lldms > %dms（PLC未反馈，允许重扫重投）",
        epc.toLocal8Bit().data(), (long long)ageMs, ttlMs);
    return false;
}

// ============================================================================
// ★ 2026-09-16 「根因优先」标记：本轮已入异常终态（供超时守卫避免覆盖真实根因）
//   用例：格口满箱未重绑(禁用) → 发送失败记"无可用格口"；142s 后同一 EPC 再推送，
//         旧 receivedAt 使 elapsed=142457ms → 超时守卫本会再记一条"发送超时"覆盖根因。
//   时效：EPC_TERMINAL_EXCEPTION_KEEP_MS 内有效，超期自动失效（防止长期运行无限累积）。
//   线程：仅主线程调用（与 m_sentEpcs / m_lastPlcSendMs 同线程约定）。
// ============================================================================
void HttpServer::markEpcTerminalException(const QString& epc, const QString& type)
{
    if (epc.isEmpty()) return;
    m_epcTerminalException.insert(epc, qMakePair(type, QDateTime::currentMSecsSinceEpoch()));
    HTTP_LOG_INFO("异常终态打标 epc=%s type=%s 保鲜期=%dms（后续同件超时将沿用该根因）",
        epc.toLocal8Bit().data(), type.toLocal8Bit().data(), EPC_TERMINAL_EXCEPTION_KEEP_MS);
}

void HttpServer::clearEpcTerminalException(const QString& epc)
{
    if (epc.isEmpty()) return;
    if (m_epcTerminalException.remove(epc) > 0)
        HTTP_LOG_INFO("异常终态解除 epc=%s（本轮已成功下发/清理）", epc.toLocal8Bit().data());
}

QString HttpServer::terminalExceptionType(const QString& epc) const
{
    if (epc.isEmpty()) return QString();
    auto it = m_epcTerminalException.constFind(epc);
    if (it == m_epcTerminalException.constEnd()) return QString();
    const qint64 ageMs = QDateTime::currentMSecsSinceEpoch() - it.value().second;
    if (ageMs > EPC_TERMINAL_EXCEPTION_KEEP_MS) return QString();   // 超期失效
    return it.value().first;
}

void HttpServer::clearAllEpcRuntimeState()
{
    const int inFlight = m_sentEpcs.size();
    const int termMark = m_epcTerminalException.size();
    m_sentEpcs.clear();
    m_sentAtMs.clear();
    m_lastPlcSendMs.clear();
    m_rescanResendTimes.clear();
    m_epcTerminalException.clear();   // ★ 2026-09-16 根因标记随波次切换一并清理（避免跨波次误用）
    HTTP_LOG_INFO("在途/冷却/重发计数已清空 inFlightBefore=%d 异常终态标记=%d（波次切换或完结清理）",
        inFlight, termMark);
}

bool HttpServer::trySendToPlcForEpc(const QString& epc)
{
    return trySendToPlcForEpcInternal(epc, false);
}

// bReplayed=true：本件来自"挂起件补发"（replayPendingRfidPlcEpcs）
//   → 跳过波次隔离判定（该判定只用于"新到货件"分流，不适用于已判定归属的补发件）；
//   → 其余校验与选格完全一致，因此**始终按当前波次的最新计划选格**：
//     同一 SKU 在不同波次的格口不同时，补发件落的是新波次的格口。
bool HttpServer::trySendToPlcForEpcInternal(const QString& epc, bool bReplayed)
{
    if (!m_pEpcCache || !m_pPlcMgr || !m_pBuffer)
    {
        HTTP_LOG_WARN("trySendToPlcForEpc 前置条件不满足 epc=%s EpcCache=%d PlcMgr=%d Buffer=%d",
            epc.toLocal8Bit().data(), m_pEpcCache!=nullptr, m_pPlcMgr!=nullptr, m_pBuffer!=nullptr);
        return false;
    }

    // ★ 2026-09-20 现场问题④：本轮投递开始 → 先清掉上一轮的"改投异常口原因"打标。
    //   为什么必须清：同一件货可能被反复投放（从异常口取出重投），若旧标残留，
    //   会把后续**真正因超计划**落入 66 的同一件误标成"未绑定容器"（原因张冠李戴）。
    //   打标只由本轮改投分支写入，落下 66 时取用并消费（一次事件一条异常记录）。
    clearExcRoute(epc);

    // ════════════════════════════════════════════════════════════════════════
    // ★★ 2026-09-14 波次隔离判定（**必须放在所有其它闸门之前**）★★
    //
    // 客户口径（两条，已确认）：
    //   ① **波次与波次之间相互隔离、互不影响**；现场作业方式为
    //      "同一时刻只有一个波次的货上线"。
    //   ② 若**同一 SKU 同时出现在两个波次**，且当前波次已下发到该 SKU
    //      → **按当前波次处理即可**（不做特殊区分，避免把正常件误挂起）。
    //
    // 因此"下一波次的货"只在**一个明确的、无歧义的条件**下才成立：
    //     该 SKU **完全不在当前波次的计划里**（不是它的货）
    //     且 该 SKU 出现在某个排队波次的 SKU 集合里
    //   → 判为下一波次的货 → 挂起等待其所属波次。
    //   反之，只要当前波次计划里有这个 SKU（无论还有没有余量），一律按当前波次处理：
    //     · 有余量 → 正常按计划分配；
    //     · 无余量（多投/超计划）→ 维持既有"超计划改投异常口 66"语义。
    //
    // 为什么必须最先判：既有"非执行态"闸门对 CREATED/BOUND 状态会**广播**下发
    //   （历史实现：不选格口就发指令）→ PLC 可能把件导向任意格口。
    //   若下一波次的货在此时上线，就会被这条广播链误送。
    // ════════════════════════════════════════════════════════════════════════
    if (!bReplayed && !m_pendingWaveQueue.isEmpty())
    {
        const int stIso = m_pWaveMgr ? m_pWaveMgr->status() : -1;
        const QString skuIso = getSkuByEpc(epc);
        if (!skuIso.isEmpty() && skuBelongsToPendingWave(skuIso))
        {
            // 本 SKU 是否属于**当前波次**的计划（不看余量，只看"是不是它的货"）
            bool inCurWave = false;
            {
                std::lock_guard<std::mutex> lk(m_allocMutex);
                inCurWave = m_alloc.skuHasPlan(skuIso);
            }
            if (!inCurWave)
            {
                bool readyIso = m_pEpcCache->isReadyForPlc(epc);
                if (readyIso)
                {
                    std::lock_guard<std::mutex> lock(m_pendingRfidMutex);
                    m_pendingRfidPlcEpcs.insert(epc);
                }
                HTTP_LOG_WARN("trySendToPlcForEpc 波次隔离：本件挂起（不按当前波次计划分配、不投异常口、不广播）"
                              "epc=%s sku=%s status=%d(%s) 原因=SKU 不在当前波次计划内且属于排队中的下一波次 "
                              "待执行队列=%d %s",
                    epc.toLocal8Bit().data(), skuIso.toLocal8Bit().data(), stIso,
                    stIso >= 0 ? WaveSnapshot::statusToString(stIso).toLocal8Bit().data() : "?",
                    m_pendingWaveQueue.size(),
                    readyIso ? "(已就绪→挂起，该波次开始分拣后自动补发)" : "(未就绪，走正常重试)");
                emit logMessage(QString::fromUtf8(
                    "[波次隔离] EPC %1（SKU %2）不属于当前波次、属于排队中的下一波次，暂不下发 —— "
                    "待该波次开始分拣后按其计划补发；不会误投异常口、也不会被广播到任意格口")
                    .arg(epc).arg(skuIso));
                return false;
            }
        }
    }

    // ★ 2026-09-05：发送 PLC 仅限执行态（与主线一致）；SKU 绑定查询不受此限（接收即查已提前完成），
    //   此处兜底所有调用路径（含异步查询回调）。
    // ★ 2026-09-08 口径统一：SORTING 与 FULLBOX_SYNC 均可发送——满箱同步期间其他格口落格照常，
    //   向 PLC 下发指令同样照常（与落格反馈处理口径一致），满箱上传与分拣互不阻塞
    {
        int st = m_pWaveMgr ? m_pWaveMgr->status() : -1;
        if (st != WAVE_SORTING && st != WAVE_FULLBOX_SYNC)
        {
            // ★ 2026-09-08 不丢弃保障：已就绪（SKU+carNum 齐）仅因非执行态未发送 → 挂起，
            //   恢复分拣后由 replayPendingRfidPlcEpcs 自动补发；未就绪的走既有未就绪重试链路
            bool readyNow = m_pEpcCache && m_pEpcCache->isReadyForPlc(epc);
            if (readyNow)
            {
                std::lock_guard<std::mutex> lock(m_pendingRfidMutex);
                m_pendingRfidPlcEpcs.insert(epc);
            }
            HTTP_LOG_INFO("trySendToPlcForEpc 非执行态不发送 epc=%s status=%d(%s) %s",
                epc.toLocal8Bit().data(), st,
                st >= 0 ? WaveSnapshot::statusToString(st).toLocal8Bit().data() : "?",
                readyNow ? "(已就绪→挂起，恢复分拣后自动补发)" : "(未就绪，走正常重试)");
            return false;
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // ★ 2026-09-14 注：早期版本这里还有一段"待执行队列非空 + 当前波次非执行态 → 挂起"的重复守卫。
    //   现已被函数开头的「波次隔离判定」统一取代（放在所有闸门之前，覆盖面更广：
    //   既覆盖非执行态、也覆盖"当前波次仍在分拣但该 SKU 已无额度"的情形）。
    //   保留此注释以免后人误以为漏了判断。

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

    // ★ 检查 PLC 连接
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

    // ★★★ 2026-09-13 计划数封顶闸门（客户四轮确认口径）★★★
    //   在"RFID 拿到 SKU、查到格口映射"之后判定：该 SKU 在该格口的**真实落格件数**是否已达计划件数。
    //   额度消耗源 = PLC 确认真正落入该格口的去重 EPC（见 clearBoxLandedCount 注释），因此：
    //     · 未达计划  → 允许投期望格口（含此前被判去异常口的件被人工重投回来补缺口）
    //     · 已达计划  → 判为多余件，按策略改投**物理异常口**（无论重投多少次）
    //     · 该 EPC 已正确落入过该格口（被拿出重投）→ 允许回原格口，且不重复计数
    {
        const AppConfig& cfgOp = ConfigManager::instance()->config();

        // ★★ 2026-09-21 硬上限修正：封顶基准必须是 **本格口(本属性)的计划件数**，不能用 SKU 计划总数。
        //   原实现取 `entry.gridCount`（= 该 SKU 各格口计划之和，见 ParseWorker 的累加口径）——
        //   多格口时（现场实例：034(发货) 计划 2 件 + 040(分类) 176 件，总数 178）
        //   等于把本格口上限放大到 178，且**让分类与发货互相顶账**（正是"某格口超计划那么多"的结构性成因）。
        //   映射串为多格口（"34,40"）时取**最严**：只要本件可能落到的任一格口已达其计划，即判超计划。
        int planQtyOp = 0;
        {
            QStringList gs = entry.gridNum.split(',', Qt::SkipEmptyParts);
            if (gs.isEmpty()) gs << entry.gridNum;
            for (const QString& gs1 : gs)
            {
                const QString gk1 = normalizeGridKey(gs1);
                const int plan1 = entry.planQtyPerGrid.value(gk1, 0);
                if (planQtyOp == 0 || (plan1 > 0 && plan1 < planQtyOp))
                    planQtyOp = plan1;                  // 取最小（最严）计划件数
            }
        }
        const QString gridKeyOp = normalizeGridKey(entry.gridNum);

        bool countedBefore = false;
        const bool allowToPlanGrid = allowIntoPlanGrid(gridKeyOp, sku, epc, planQtyOp, countedBefore);

        if (countedBefore)
        {
            // 规则③：已正确落入该格口的件被拿出重投 → 回原格口，不占新额度
            HTTP_LOG_INFO("[计划封顶] EPC=%s 已正确落入过格口%s（SKU=%s）→ 允许重投回原格口，不重复计数",
                epc.toLocal8Bit().data(), gridKeyOp.toLocal8Bit().data(), sku.toLocal8Bit().data());
        }
        else if (!allowToPlanGrid)
        {
            // 规则①：已达计划 → 多余件
            QString excGrid = cfgOp.exceptionGrid.trimmed();
            if (excGrid == "0") excGrid.clear();                  // "0" 视为未配置

            const QString orderOp = m_pWaveMgr ? m_pWaveMgr->orderCode() : QString();
            const int landedOp = landedCountOf(gridKeyOp, sku);
            // ★ 2026-09-21：日志/异常 reason 带上**分拣属性**，现场一眼看清是"分类"还是"发货"额度用尽
            const QString typeOp = entry.gridTypePerGrid.value(gridKeyOp,
                                     entry.gridType.isEmpty() ? QStringLiteral("0") : entry.gridType);
            const QString typeNameOp = (typeOp == "2") ? QString::fromUtf8("发货")
                                     : (typeOp == "1") ? QString::fromUtf8("异常")
                                                       : QString::fromUtf8("分类");
            const QString reasonOp = QString::fromUtf8(
                "本格口%1(%2)计划%3件 已真实落格%4件，本件为超出计划的多余件（格口映射=[%5]）")
                .arg(gridKeyOp).arg(typeNameOp).arg(planQtyOp).arg(landedOp).arg(entry.gridNum);

            if (cfgOp.sortingOverplanPolicy == "exception" && !excGrid.isEmpty())
            {
                // ① 改投物理异常口：指令目标格口由 entry.gridNum 换成异常格号
                codeGridMap[epc] = excGrid;
                HTTP_LOG_WARN("[超计划] epc=%s sku=%s 格口%s(%s) 本格口计划%d件 已实落%d件 → 改投异常口%s（严格不大于计划件数，本件不再进入期望格口）",
                    epc.toLocal8Bit().data(), sku.toLocal8Bit().data(), gridKeyOp.toLocal8Bit().data(),
                    typeNameOp.toLocal8Bit().data(), planQtyOp, landedOp, excGrid.toLocal8Bit().data());
                emit logMessage(QString::fromUtf8(
                    "[超计划] SKU %1 格口%2(%3)计划%4件已满（已实落%5件）——EPC %6 改投异常口%7")
                    .arg(sku).arg(gridKeyOp).arg(typeNameOp).arg(planQtyOp).arg(landedOp).arg(epc).arg(excGrid), true);
            }
            else
            {
                // ② 未配置异常口（或策略为 block）→ 不发任何指令，降级为人工处理
                HTTP_LOG_WARN("[超计划] epc=%s sku=%s 格口%s(%s) 本格口计划%d件 已实落%d件，"
                              "且未配置异常口(exceptionGrid)或策略为%s → 不下发指令，请人工取出",
                    epc.toLocal8Bit().data(), sku.toLocal8Bit().data(), gridKeyOp.toLocal8Bit().data(),
                    typeNameOp.toLocal8Bit().data(), planQtyOp, landedOp, cfgOp.sortingOverplanPolicy.toLocal8Bit().data());
                emit logMessage(QString::fromUtf8(
                    "[超计划] SKU %1 格口%2(%3)计划%4件已满——EPC %5 未下发指令（未配置异常口），请人工取出")
                    .arg(sku).arg(gridKeyOp).arg(typeNameOp).arg(planQtyOp).arg(epc), true);

                if (m_pSortingDb && m_pSortingDb->isOpen())
                {
                    ExceptionRecord exOp;
                    exOp.type      = QString::fromUtf8("超计划未投放");
                    exOp.orderCode = orderOp;
                    exOp.epc       = epc;
                    exOp.sku       = sku;
                    exOp.reason    = reasonOp + QString::fromUtf8("；未配置异常口，未下发指令，请人工取出");
                    exOp.time      = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
                    m_pSortingDb->insertException(exOp);
                }
                return false;
            }

            // 异常留痕 + 异常口径登记（仅日志/统计，非永久黑名单：缺口出现时仍可重投补上）
            if (m_pSortingDb && m_pSortingDb->isOpen())
            {
                ExceptionRecord exOp;
                exOp.type      = QString::fromUtf8("超计划改投异常口");
                exOp.orderCode = orderOp;
                exOp.epc       = epc;
                exOp.sku       = sku;
                exOp.reason    = reasonOp + QString::fromUtf8("；已改投异常口%1").arg(excGrid);
                exOp.time      = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
                m_pSortingDb->insertException(exOp);
            }
            if (m_pWaveMgr)
                m_pWaveMgr->markException(epc);
            {
                std::lock_guard<std::mutex> lkOp(m_boxLandedMutex);
                m_epcBoundException.insert(epc);
            }
        }
    }

    // ★ 超时检查：从 RFID 推送到达起算，超过 PLC_SEND_TIMEOUT_MS 则入异常格口
    if (m_pEpcCache && m_pEpcCache->isSendTimeout(epc))
    {
        qint64 elapsed = m_pEpcCache->getElapsedMs(epc);

        // ★ 2026-09-16 根因优先：若该 EPC 最近已因发送失败入异常终态（如"无可用格口"），
        //   本次超时**不是**新问题，而是同一根因的延续 —— 异常记录沿用原根因，避免覆盖。
        //   实测案例：格口满箱未重绑 → 记"无可用格口"；142s 后同件再上线，
        //   elapsed=142457ms（=两次推送间隔）本会再记一条"发送超时"，掩盖真实原因。
        const QString priorRoot = terminalExceptionType(epc);
        const bool hasPrior = !priorRoot.isEmpty();
        const QString exType = hasPrior
            ? QString::fromUtf8("发送超时(前序:%1)").arg(priorRoot)
            : QString::fromUtf8("发送超时");
        const QByteArray priorNote = hasPrior
            ? QString::fromUtf8("（沿用前序根因：%1）").arg(priorRoot).toLocal8Bit()
            : QByteArray();

        HTTP_LOG_WARN("PLC发送超时 epc=%s sku=%s grid=%s seq=%s elapsed=%lldms 超时阈值=%dms 入异常格口%s",
            epc.toLocal8Bit().data(), sku.toLocal8Bit().data(),
            entry.gridNum.toLocal8Bit().data(), seq.toLocal8Bit().data(), elapsed, PLC_SEND_TIMEOUT_MS,
            priorNote.constData());
        emit logMessage(QString("[异常] 发送超时 epc=%1 耗时%2ms 入异常格口%3")
            .arg(epc).arg(elapsed)
            .arg(hasPrior ? QString::fromUtf8("（前序根因：%1）").arg(priorRoot) : QString()));

        // 写入异常记录表（发送超时，入异常格口）
        if (m_pSortingDb)
        {
            ExceptionRecord ex;
            ex.type      = exType;
            ex.orderCode = m_pWaveMgr->orderCode();
            ex.epc       = epc;
            ex.sku       = sku;
            ex.reason    = hasPrior
                ? QString::fromUtf8("本轮投递前序已因「%1」发送失败；本次计时%2ms（含两次推送间隔）超过阈值%3ms，"
                                    "入异常格口。根因为前序失败，非下发耗时问题")
                      .arg(priorRoot).arg(elapsed).arg(PLC_SEND_TIMEOUT_MS)
                : QString::fromUtf8("RFID推送→PLC发送耗时%1ms，超过阈值%2ms，入异常格口")
                      .arg(elapsed).arg(PLC_SEND_TIMEOUT_MS);
            m_pSortingDb->insertException(ex);
        }
        // ★ 2026-09-09 需求7：入异常口后计时归0——二次上传（RFID重推）重新计时，不再立即超时
        // ★ 2026-09-16 改为 resetCycle：作废本轮，使下次推送按新件起算（理由同上）
        markEpcTerminalException(epc, exType);
        if (m_pEpcCache)
            m_pEpcCache->resetCycle(epc);
        return false;
    }

    // ★ 2026-09-11 重扫重投保护（仅对"非首次下发"生效）：总开关 + 冷却 + 每波次重发次数上限
    //   场景：操作员把已落格的件拿起重新上料 → 允许按原格口映射重投
    // ★ 2026-09-13 调整（客户要求：已落格后的物件再次投放，WCS 依旧处理）：
    //   · rescanResendMaxTimes = 0（默认）→ **不限制重投次数**，只要重新上料就重发原格口；
    //     >0 时才启用上限保护（超限拦截 + 异常表"重扫超限"留痕）
    //   · rescanResendCooldownMs = 0（默认）→ 不做冷却拦截；>0 时恢复防连读限频
    // ★ 2026-09-20 现场问题④：把"是否重投 / 计数基准"提到块外 —— 计数器改为**真正下发成功后才 +1**
    bool bRescanSend     = false;
    int  rescanTimesBefore = 0;
    {
        const AppConfig& cfg = ConfigManager::instance()->config();
        const qint64 nowMs  = QDateTime::currentMSecsSinceEpoch();
        const qint64 lastMs = m_lastPlcSendMs.value(epc, 0);

        if (lastMs > 0)   // 该 EPC 本波次已下发过 → 本次属于"重扫重投"
        {
            if (!cfg.rescanResendEnabled)
            {
                HTTP_LOG_INFO("重扫重投已关闭（rescanResendEnabled=false）epc=%s 不再下发", epc.toLocal8Bit().data());
                return false;
            }
            // 冷却：默认 0 = 不限制（XML/宏取当前配置值，0 即关闭冷却拦截）
            const int cooldownMs = cfg.rescanResendCooldownMs;
            if (cooldownMs > 0 && nowMs - lastMs < cooldownMs)
            {
                HTTP_LOG_INFO("重扫重投 冷却中跳过 epc=%s 距上次下发%lldms < %dms",
                    epc.toLocal8Bit().data(), (long long)(nowMs - lastMs), cooldownMs);
                return false;
            }
            // 次数上限：默认 0 = 不限制（仅当配置 >0 时才做上限保护）
            const int maxTimes = cfg.rescanResendMaxTimes;
            const int times = m_rescanResendTimes.value(epc, 0);
            if (maxTimes > 0 && times >= maxTimes)
            {
                HTTP_LOG_WARN("重扫重投 已达上限 epc=%s times=%d/%d 不再下发（人工处理）",
                    epc.toLocal8Bit().data(), times, maxTimes);
                emit logMessage(QString::fromUtf8("[重扫] EPC %1 重发次数已达上限 %2，不再下发（请人工处理）")
                    .arg(epc).arg(maxTimes), true);
                if (m_pSortingDb && m_pSortingDb->isOpen())
                {
                    ExceptionRecord ex;
                    ex.type      = QString::fromUtf8("重扫超限");
                    ex.orderCode = m_pWaveMgr ? m_pWaveMgr->orderCode() : QString();
                    ex.epc       = epc;
                    ex.sku       = sku;
                    ex.reason    = QString::fromUtf8("同一 EPC 本波次重扫重投已达 %1 次上限（映射=[%2]），不再下发")
                                       .arg(maxTimes).arg(entry.gridNum);
                    ex.time      = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
                    m_pSortingDb->insertException(ex);
                }
                return false;
            }

            // ★ 2026-09-20 现场问题④：计数器只在**真正下发成功**后 +1（见下方 sendOk 分支）。
            //   为什么移位：原实现在"下发动作之前"就 +1，于是"格口未绑定/满箱未重绑 → 根本没发出指令"
            //   的反复重投也会吃掉次数配额 —— 现场把这件货拿出异常口再投第 4 次时会被判"重扫超限"
            //   而彻底锁死（即使格口随后已绑定容器）。下发失败不是这件货的问题，不应计入重投上限。
            //   rescanResendMaxTimes 的原有语义（防同一件反复真实下发）完全保留。
            bRescanSend      = true;
            rescanTimesBefore = times;
            const QString rescanPrior = terminalExceptionType(epc);   // 前序根因（发送成功后才被清除）
            const QString rescanLog = maxTimes > 0
                ? QString::fromUtf8("[重扫] EPC=%1 已落格/已入异常口→本次为第%2次重投尝试（上限%3）grid=%4%5")
                      .arg(epc).arg(times + 1).arg(maxTimes).arg(entry.gridNum)
                      .arg(rescanPrior.isEmpty() ? QString()
                                                 : QString::fromUtf8("（前序：%1 → 本次为异常口取回/重投，按新件重新判定格口与额度）").arg(rescanPrior))
                : QString::fromUtf8("[重扫] EPC=%1 已落格/已入异常口→本次为第%2次重投尝试（不限次）grid=%3%4")
                      .arg(epc).arg(times + 1).arg(entry.gridNum)
                      .arg(rescanPrior.isEmpty() ? QString()
                                                 : QString::fromUtf8("（前序：%1 → 本次为异常口取回/重投，按新件重新判定格口与额度）").arg(rescanPrior));
            HTTP_LOG_INFO("%s", rescanLog.toLocal8Bit().data());
            emit logMessage(rescanLog, false);
        }
    }

    // ★ 计时终点：发送动作执行后即结束 PLC_SEND_TIMEOUT_MS 计时（无论成败）
    //   1s 限制衡量的是「收到→发出PLC指令」的延迟，发出则计时结束；
    //   发送结果（成功/失败）由 sendOk 独立判断，与计时互不干扰
    bool sendOk = m_pPlcMgr->sendBatchCodesWithEpcCache(codeGridMap);
    if (m_pEpcCache) m_pEpcCache->markSent(epc);   // ★ 记录 PLC 发送指令时间（计时终点，无论成败）
    // ★ 2026-09-20 现场问题④：重投次数只在"真正下发成功"后计数（见上方重扫重投保护的说明）
    if (sendOk && bRescanSend)
        m_rescanResendTimes[epc] = rescanTimesBefore + 1;
    qint64 sendElapsed = m_pEpcCache ? m_pEpcCache->getHandleSendMs(epc) : -1;   // 开始处理→发送 耗时
    // ★ 2026-09-13 性能核验：RFID推送→PLC下发 时延采样（现场 1s 硬窗口的关键指标）
    if (sendElapsed >= 0) perfSampleRfidToPlc((int)sendElapsed);
    if (!sendOk)
    {
        // ★ 发送失败：可能原因 ① 无可下发格口（未绑定容器/满箱未重绑(禁用)）→ 按客户口径不发；
        //   ② PLC 未连接。不标记为已发送，并写异常表留痕（便于现场核对"这件为什么没发指令"）
        HTTP_LOG_WARN("PLC发送未成功 epc=%s sku=%s grid=%s carNum=%s seq=%s elapsed=%lldms "
                      "(未绑定容器/格口满箱未重绑/物理锁格/PLC未连接，不标记已发送)",
            epc.toLocal8Bit().data(), sku.toLocal8Bit().data(),
            entry.gridNum.toLocal8Bit().data(), carNum.toLocal8Bit().data(),
            seq.toLocal8Bit().data(), sendElapsed);
        emit logMessage(QString("[格口] 无可用格口，未发指令 epc=%1 sku=%2 映射=[%3] 小车%4 seq=%5"
                                "（未绑定容器/满箱未重绑；等到 WMS 重发H6绑定或人工处理）")
            .arg(epc).arg(sku).arg(entry.gridNum).arg(carNum).arg(seq), true);

        // ★ 异常留痕（type=无可用格口）：现场可在"分拣记录查询"看到原因
        if (m_pSortingDb && m_pSortingDb->isOpen())
        {
            ExceptionRecord ex;
            ex.type      = QString::fromUtf8("无可用格口");
            ex.orderCode = m_pWaveMgr ? m_pWaveMgr->orderCode() : QString();
            ex.epc       = epc;
            ex.sku       = sku;
            ex.reason    = QString::fromUtf8("映射=[%1] 内格口未绑定容器/满箱未重绑(禁用)/物理锁格，未发送PLC指令；"
                                             "等待WMS重发H6绑定容器或人工处理（本件不计已分拣、不消耗额度）")
                               .arg(entry.gridNum);
            ex.time      = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
            m_pSortingDb->insertException(ex);
        }

        // ★ 2026-09-09 需求7：入异常口后计时归0——二次上传重新计时
        // ★ 2026-09-16 改为 resetCycle：作废本轮而非"把起点设为失败时刻"。
        //   原因：失败件缓存条目会长时间留存（TTL 300s），若起点=失败时刻，
        //   下一次推送的 elapsed 会从失败时刻累积（实测 142457ms 误报"发送超时"，
        //   掩盖真实根因"无可用格口"）。resetCycle 使下次推送按新件起算。
        // ★ 2026-09-16 根因优先：标记本轮已入异常终态，防止后续推送被记成"发送超时"覆盖根因
        markEpcTerminalException(epc, QString::fromUtf8("无可用格口"));
        if (m_pEpcCache)
            m_pEpcCache->resetCycle(epc);
        return false;
    }

    HTTP_LOG_INFO("PLC发送成功 epc=%s sku=%s grid=%s carNum=%s seq=%s elapsed=%lldms",
        epc.toLocal8Bit().data(), sku.toLocal8Bit().data(),
        entry.gridNum.toLocal8Bit().data(), carNum.toLocal8Bit().data(),
        seq.toLocal8Bit().data(), sendElapsed);
    emit logMessage(QString("[PLC] 发送 %1 → 格口%2 小车%3 (sku=%4 seq=%5) 耗时%6ms")
        .arg(epc).arg(entry.gridNum).arg(carNum).arg(sku).arg(seq).arg(sendElapsed));

    // ★ 标记在途（已下发、待落格反馈）——反馈到达即由 clearEpcInFlight 解除，之后允许重扫重投
    markEpcInFlight(epc);

    // ★ 2026-09-16 本轮已成功下发 → 解除"异常终态"根因标记（下一次失败会重新打标）
    clearEpcTerminalException(epc);

    return true;
}

