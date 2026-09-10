#include "ParseWorker.h"
#include "LogService.h"
#include "WmsGridCode.h"     // ★ 2026-09-07 WMS 格口编码(22+3位) 入参归一
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QElapsedTimer>
#include "define.h"

// ★ 波次明细日志宏（写入 ./log/WAVE_ITEM/wave_item.log）
//   每个 items[] 子项单独记录，避免原始报文因截断而无法查看完整内容
//   （宏定义已移至 LogService.h 统一管理）

ParseWorker::ParseWorker(TaskQueue* pQueue, GridBuffer* pBuffer, QObject* parent)
    : QThread(parent), m_pQueue(pQueue), m_pBuffer(pBuffer)
{
}

void ParseWorker::stop()
{
    m_bRunning = false;
    // ★ 2026-09-04 修复闪退：原 wakeAll() 唤醒后队列仍空，pop() 会再次 wait 永久阻塞，
    //   导致 wait(3s) 超时后线程仍在运行，Qt 析构 QThread 子类时 "Destroyed while thread
    //   is still running" 闪退。改为 stop() 置位停止标志，pop() 返回空任务使 run() 正常退出。
    m_pQueue->stop();
}

void ParseWorker::run()
{
    QThread::currentThread()->setPriority(QThread::LowPriority);

    while (m_bRunning)
    {
        WaveTask task = m_pQueue->pop();
        if (!m_bRunning) break;
        if (task.rawBody.isEmpty()) continue;

        QElapsedTimer timer;
        timer.start();

        // 解析JSON — 剥离 BOM 头（UTF-8 BOM: EF BB BF）
        QByteArray cleanBody = task.rawBody;
        if (cleanBody.startsWith("\xEF\xBB\xBF"))
            cleanBody.remove(0, 3);
        cleanBody.replace('\x00', "");  // 移除 null 字节
        cleanBody = cleanBody.trimmed();

        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(cleanBody, &err);
        if (doc.isNull())
        {
            LogCenter::Instance()->wcs_run_log_warn(false,
                QString("[Parse] JSON解析失败: %1 offset=%2")
                    .arg(err.errorString()).arg(err.offset));
            emit parseError(err.errorString());
            continue;
        }

        QJsonObject root   = doc.object();
        QString orderCode  = root["orderCode"].toString();
        // ★ 兼容整数和字符串两种类型: "12312" 或 12312
        int orderQty = 0;
        {
            QJsonValue v = root["orderQty"];
            orderQty = v.isString() ? v.toString().toInt() : v.toInt();
        }
        QJsonArray items   = root["items"].toArray();
        // ★ sobi 来源库位（对应满箱回传 head.fromLocation）：优先 items[].sobi，
        //   兼容根节点级 sobi（波次级来源库位）作为明细缺省兜底
        QString rootSobi   = root["sobi"].toString().trimmed();

        // 构建新Map（key=Sku编码, value=格口分配信息）
        auto* newMap = new QMap<QString, GridEntry>();
        QSet<QString> recvSet;   // 跟踪接收到的SKU
        int addCount = 0;        // 新增 SKU 计数
        int mergeCount = 0;      // 同品多格口合并数
        int duplicateCount = 0;  // 同品同格口重复行数（一SKU多行下发时的正常冗余，也需计数留痕）
        // ★ 2026-09-09 超大波次(50000item)日志节流：
        //   小波次(≤PARSE_SKU_FULL_LOG_MAX) 逐条全量输出——与原行为完全一致，现场核对无差异；
        //   超大波次才节流（首 N 条 + 每步长一条 + 结尾三类计数汇总），避免日志IO拖慢解析。
        //   全量依据：WAVE_ITEM 日志留有整条原始报文（含全部 inco/gridNum/sobi），可 grep 核对任一 SKU
        const bool bFullSkuLog = (items.size() <= PARSE_SKU_FULL_LOG_MAX);
        if (!bFullSkuLog)
        {
            WCS_INFO("[SKU映射] items=%lld 超过全量日志上限(%d)，本次节流输出（首%d条+每%d条一条，可调宏恢复全量）",
                (qint64)items.size(), PARSE_SKU_FULL_LOG_MAX, PARSE_SKU_LOG_TAIL, PARSE_SKU_LOG_STEP);
        }

        // ★ 波次明细日志：完整原始报文（大波次单条可达数MB，WAVE_ITEM 模块落盘；逐条映射日志的核对底稿）
        WAVE_ITEM_INFO("[原始报文] %s body=%s(%d字节)",
            task.fullUrl.toLocal8Bit().data(), task.rawBody.constData(), task.rawBody.size());

        for (const QJsonValue& val : items)
        {
            QJsonObject item = val.toObject();
            QString inco     = item["inco"].toString().trimmed();
            // ★ 2026-09-07 WMS 格口编码兼容：gridNum 可带前缀编码（如 "22005" = 格口号5），
            //   解析为内部格口号（归一为纯数字，后续存储/映射/校验全部用内部口径）
            QString gridNum  = item["gridNum"].toString().trimmed();
            int gridNumI     = parseWmsGridCodeToInt(gridNum);
            if (gridNumI >= 1 && gridNumI <= BINDING_SLOT_COUNT)
                gridNum = QString::number(gridNumI);   // "22005"/"05"/"5" → "5"
            QString gridType = item["gridType"].toString().trimmed();
            // ★ 2026-09-06 来源库位（对应满箱回传报文 head.fromLocation）：
            //   WMS 下发字段为 sobi（如 "H-01-AB"），旧报文兼容 volu；根节点 sobi 兜底
            QString sobi     = item["sobi"].toString().trimmed();
            QString volu     = sobi.isEmpty() ? item["volu"].toString().trimmed() : sobi;
            if (volu.isEmpty())
                volu = rootSobi;
            QString obxCode  = item["obxCode"].toString().trimmed();               // ★ 容器号
            // ★ 兼容整数和字符串两种类型
            int gridNumber = 0;
            {
                QJsonValue gv = item["gridNumber"];
                gridNumber = gv.isString() ? gv.toString().toInt() : gv.toInt();
            }

            if (inco.isEmpty() || gridNum.isEmpty() || gridNumber <= 0)
            {
                WCS_WARN("[Parse] 跳过无效条目 inco=%s gridNum=%s orderCode=%s",
                    inco.toLocal8Bit().data(), gridNum.toLocal8Bit().data(),
                    orderCode.toLocal8Bit().data());
                continue;
            }

            // ★ 格口号无效/越界检测（不阻塞波次，记录异常后跳过，便于核查和重传）
            {
                int gNum = gridNumI;   // 已在上方归一解析（无效时为 -1）
                if (gNum < 1 || gNum > BINDING_SLOT_COUNT)
                {
                    QString reason = QString("格口号无效或越界 gridNum=%1 有效范围1~%2").arg(gridNum).arg(BINDING_SLOT_COUNT);
                    WCS_WARN("[Parse] 格口号无效或越界 跳过 item inco=%s gridNum=%s orderCode=%s",
                        inco.toLocal8Bit().data(), gridNum.toLocal8Bit().data(),
                        orderCode.toLocal8Bit().data());
                    emit parseException(orderCode, inco, gridNum, reason);
                    continue;  // 跳过该 item，不加入 GridBuffer
                }
            }

            recvSet.insert(inco);

            // ★ 同品多格口合并（inco=SKU编码）
            if (newMap->contains(inco))
            {
                QString exist = (*newMap)[inco].gridNum;
                if (!exist.contains(gridNum))
                {
                    (*newMap)[inco].gridNum = exist + "," + gridNum;
                    mergeCount++;
                    if (bFullSkuLog || mergeCount <= PARSE_SKU_LOG_TAIL || (mergeCount % PARSE_SKU_LOG_STEP) == 0)
                    {
                        WCS_INFO("[SKU映射] 同品多格口合并 SKU=%s gridNum=%s (已有=%s)", 
                            inco.toLocal8Bit().data(), gridNum.toLocal8Bit().data(), exist.toLocal8Bit().data());
                    }
                }
                else
                {
                    // ★ 同品同格口重复行（一SKU多行下发）：计数 + 节流输出（不逐条刷屏）
                    duplicateCount++;
                    if (bFullSkuLog || duplicateCount <= PARSE_SKU_LOG_TAIL || (duplicateCount % PARSE_SKU_LOG_STEP) == 0)
                    {
                        WCS_INFO("[SKU映射] 重复格口跳过 SKU=%s gridNum=%s (已有=%s)", 
                            inco.toLocal8Bit().data(), gridNum.toLocal8Bit().data(), exist.toLocal8Bit().data());
                    }
                }
            }
            else
            {
                GridEntry entry;
                entry.gridNum   = gridNum;
                entry.gridType  = gridType.isEmpty() ? "0" : gridType;  // 0=分类, 1=异常, 2=发货
                entry.gridCount = gridNumber;
                entry.volu      = volu;                      // ★ 来源库位
                entry.obxCode   = obxCode;                   // ★ 容器号
                // 批次信息：每个SKU编码都关联到所属批次
                entry.orderCode = orderCode;
                entry.orderQty  = orderQty;
                entry.skuCount  = 0;  // 循环结束后统一回填
                newMap->insert(inco, entry);
                addCount++;
                // ★ 超大波次节流：全量模式(小波次)输出每条；节流模式仅首尾若干条 + 每步长一条
                if (bFullSkuLog || addCount <= PARSE_SKU_LOG_TAIL || (addCount % PARSE_SKU_LOG_STEP) == 0)
                {
                    WCS_INFO("[SKU映射] 新增 SKU=%s gridNum=%s gridType=%s gridCount=%d sobi=%s", 
                        inco.toLocal8Bit().data(), gridNum.toLocal8Bit().data(), 
                        entry.gridType.toLocal8Bit().data(), gridNumber, sobi.toLocal8Bit().data());
                }
            }
        }
        WCS_INFO("[SKU映射] 解析完成 items=%lld 新增SKU=%d 合并=%d 重复行=%d",
            (qint64)items.size(), addCount, mergeCount, duplicateCount);

        // 回填 SKU 种类数到每个条目（去重后的EPC编码种类数）
        int skuCount = newMap->size();
        for (auto it = newMap->begin(); it != newMap->end(); ++it)
        {
            it.value().skuCount = skuCount;
        }

        // 原子交换
        m_pBuffer->prepareSwap(newMap);

        qint64 elapsed = timer.elapsed();
        WCS_INFO("[Parse] orderCode=%s items=%d SKU=%d qty=%d elapsed=%lldms",
            orderCode.toLocal8Bit().data(), items.size(), newMap->size(), orderQty, elapsed);

        // ★ 输出完整 SKU→格口 映射表（方便排查 SKU 映射失败问题；sobi=WMS下发的来源库位字段）
        WCS_INFO("[SKU映射] ==== 完整映射表(orderCode=%s) ====", orderCode.toLocal8Bit().data());
        for (auto it = newMap->constBegin(); it != newMap->constEnd(); ++it)
        {
            WCS_INFO("[SKU映射] SKU=%s → gridNum=%s gridType=%s gridCount=%d sobi=%s",
                it.key().toLocal8Bit().data(), it.value().gridNum.toLocal8Bit().data(),
                it.value().gridType.toLocal8Bit().data(), it.value().gridCount, 
                it.value().volu.toLocal8Bit().data());
        }
        WCS_INFO("[SKU映射] ==== 映射表结束(共%d条) ====", newMap->size());

        LogCenter::Instance()->wcs_run_log_warn(true,
            QString("[Parse] orderCode=%1 items=%2 SKU=%3 elapsed=%4ms")
                .arg(orderCode).arg(items.size()).arg(newMap->size()).arg(elapsed));

        // ★ 2026-09-07 透传 H4 原文（供当前波次执行中排队/延迟执行；先拷贝防复用）
        QByteArray rawCopy = task.rawBody;
        emit waveParsed(orderCode, newMap->size(), orderQty, elapsed, recvSet, rawCopy);
    }
}
