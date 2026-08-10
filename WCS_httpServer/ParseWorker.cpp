#include "ParseWorker.h"
#include "hlog1.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QElapsedTimer>
#include "log_center.h"
#include "define.h"

// ★ 波次明细日志宏（写入 ./log/WAVE_ITEM/wave_item.log）
//   每个 items[] 子项单独记录，避免原始报文因截断而无法查看完整内容
#define WAVE_ITEM_INFO(fmt, ...) hlog_format(HLOG_LEVEL_INFO, "WAVE_ITEM", "\t" fmt, ##__VA_ARGS__)

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

        // 构建新Map
        auto* newMap = new QMap<QString, GridEntry>();
        QSet<QString> recvSet;   // 跟踪接收到的inco
        QStringList epcList;      // ★ 收集EPC用于RFID查询

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

            if (inco.isEmpty() || gridNum.isEmpty())
            {
                WCS_WARN("[Parse] 跳过无效条目 inco=%s gridNum=%s orderCode=%s",
                    inco.toLocal8Bit().data(), gridNum.toLocal8Bit().data(),
                    orderCode.toLocal8Bit().data());
                continue;
            }

            recvSet.insert(inco);

            // ★ 收集EPC（用于后续RFID查询SKU绑定）
            QString epcn = item["epcn"].toString().trimmed();
            if (!epcn.isEmpty() && !epcList.contains(epcn))
                epcList.append(epcn);

            // ★ 波次明细日志：每个 items[] 子项单独记录完整报文
            //   避免原始报文日志因截断导致无法查看完整 item 内容
            {
                QJsonObject itemLog;
                itemLog["orderCode"] = orderCode;
                itemLog["orderQty"]  = orderQty;
                QJsonArray itemArr;
                QJsonObject itemCopy;
                itemCopy["obxCode"]    = obxCode;
                itemCopy["inco"]       = inco;
                if (!epcn.isEmpty()) itemCopy["epcn"] = epcn;
                itemCopy["gridNum"]    = gridNum;
                itemCopy["gridNumber"] = gridNumber;
                itemCopy["gridType"]   = gridType;
                if (!volu.isEmpty()) itemCopy["volu"] = volu;
                itemArr.append(itemCopy);
                itemLog["items"] = itemArr;
                QByteArray itemJson = QJsonDocument(itemLog).toJson(QJsonDocument::Compact);
                WAVE_ITEM_INFO("[原始报文] %s body=%s(%d字节)",
                    task.fullUrl.toLocal8Bit().data(), itemJson.constData(), itemJson.size());
            }

            // 同品多格口合并
            if (newMap->contains(inco))
            {
                QString exist = (*newMap)[inco].gridNum;
                if (!exist.contains(gridNum))
                    (*newMap)[inco].gridNum = exist + "," + gridNum;
            }
            else
            {
                GridEntry entry;
                entry.gridNum   = gridNum;
                entry.gridType  = gridType.isEmpty() ? "普通格口" : gridType;
                entry.gridCount = gridNumber;
                entry.volu      = volu;                      // ★ 来源库位
                entry.obxCode   = obxCode;                   // ★ 容器号
                // 批次信息：每个条码都关联到所属批次
                entry.orderCode = orderCode;
                entry.orderQty  = orderQty;
                entry.skuCount  = 0;  // 循环结束后统一回填
                newMap->insert(inco, entry);
            }
        }

        // 回填 SKU 种类数到每个条目（去重后的条码种类数）
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

        LogCenter::Instance()->wcs_run_log_warn(true,
            QString("[Parse] orderCode=%1 items=%2 SKU=%3 elapsed=%4ms")
                .arg(orderCode).arg(items.size()).arg(newMap->size()).arg(elapsed));

        emit waveParsed(orderCode, newMap->size(), orderQty, elapsed, recvSet, epcList);
    }
}
