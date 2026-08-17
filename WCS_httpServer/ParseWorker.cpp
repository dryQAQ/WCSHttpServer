#include "ParseWorker.h"
#include "LogService.h"
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
    m_pQueue->wakeAll();
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

        // 构建新Map（key=Sku编码, value=格口分配信息）
        auto* newMap = new QMap<QString, GridEntry>();
        QSet<QString> recvSet;   // 跟踪接收到的SKU

        // ★ 波次明细日志：只记录 WMS 原始报文，一行搞定
        WAVE_ITEM_INFO("[原始报文] %s body=%s(%d字节)",
            task.fullUrl.toLocal8Bit().data(), task.rawBody.constData(), task.rawBody.size());

        for (const QJsonValue& val : items)
        {
            QJsonObject item = val.toObject();
            QString inco     = item["inco"].toString().trimmed();
            QString gridNum  = item["gridNum"].toString().trimmed();
            QString gridType = item["gridType"].toString().trimmed();
            QString volu     = item["volu"].toString().trimmed();   // ★ 来源库位
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

            // ★ 格口号越界检测（不阻塞波次，记录异常后跳过，便于核查和重传）
            {
                bool ok = false;
                int gNum = gridNum.toInt(&ok);
                if (ok && (gNum < 1 || gNum > BINDING_SLOT_COUNT))
                {
                    QString reason = QString("格口号越界 gridNum=%1 有效范围1~%2").arg(gNum).arg(BINDING_SLOT_COUNT);
                    WCS_WARN("[Parse] 格口号越界 跳过 item inco=%s gridNum=%s orderCode=%s",
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
                    WCS_INFO("[SKU映射] 同品多格口合并 SKU=%s gridNum=%s (已有=%s)", 
                        inco.toLocal8Bit().data(), gridNum.toLocal8Bit().data(), exist.toLocal8Bit().data());
                }
                else
                {
                    WCS_INFO("[SKU映射] 重复格口跳过 SKU=%s gridNum=%s (已有=%s)", 
                        inco.toLocal8Bit().data(), gridNum.toLocal8Bit().data(), exist.toLocal8Bit().data());
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
                WCS_INFO("[SKU映射] 新增 SKU=%s gridNum=%s gridType=%s gridCount=%d volu=%s", 
                    inco.toLocal8Bit().data(), gridNum.toLocal8Bit().data(), 
                    entry.gridType.toLocal8Bit().data(), gridNumber, volu.toLocal8Bit().data());
            }
        }

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

        // ★ 输出完整 SKU→格口 映射表（方便排查 SKU 映射失败问题）
        WCS_INFO("[SKU映射] ==== 完整映射表(orderCode=%s) ====", orderCode.toLocal8Bit().data());
        for (auto it = newMap->constBegin(); it != newMap->constEnd(); ++it)
        {
            WCS_INFO("[SKU映射] SKU=%s → gridNum=%s gridType=%s gridCount=%d volu=%s",
                it.key().toLocal8Bit().data(), it.value().gridNum.toLocal8Bit().data(),
                it.value().gridType.toLocal8Bit().data(), it.value().gridCount, 
                it.value().volu.toLocal8Bit().data());
        }
        WCS_INFO("[SKU映射] ==== 映射表结束(共%d条) ====", newMap->size());

        LogCenter::Instance()->wcs_run_log_warn(true,
            QString("[Parse] orderCode=%1 items=%2 SKU=%3 elapsed=%4ms")
                .arg(orderCode).arg(items.size()).arg(newMap->size()).arg(elapsed));

        emit waveParsed(orderCode, newMap->size(), orderQty, elapsed, recvSet);
    }
}
