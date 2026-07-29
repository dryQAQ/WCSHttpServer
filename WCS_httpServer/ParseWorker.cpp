#include "ParseWorker.h"
#include "hlog1.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QElapsedTimer>
#include "log_center.h"

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

        // 解析JSON
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(task.rawBody, &err);
        if (doc.isNull())
        {
            LogCenter::Instance()->wcs_run_log_warn(false,
                QString("[Parse] JSON解析失败: %1").arg(err.errorString()));
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
