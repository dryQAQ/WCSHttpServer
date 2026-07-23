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
        int orderQty       = root["orderQty"].toString().toInt();
        QJsonArray items   = root["items"].toArray();

        // 构建新Map
        auto* newMap = new QMap<QString, GridEntry>();
        QSet<QString> recvSet; // 跟踪接收到的inco

        for (const QJsonValue& val : items)
        {
            QJsonObject item = val.toObject();
            QString inco     = item["inco"].toString().trimmed();
            QString gridNum  = item["gridNum"].toString().trimmed();
            QString gridType = item["gridType"].toString().trimmed();
            int gridNumber   = item["gridNumber"].toString().toInt();

            if (inco.isEmpty() || gridNum.isEmpty()) continue;

            recvSet.insert(inco);

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
                newMap->insert(inco, entry);
            }
        }

        // 原子交换
        m_pBuffer->prepareSwap(newMap);

        qint64 elapsed = timer.elapsed();
        WCS_INFO("[Parse] orderCode=%s items=%d SKU=%d qty=%d elapsed=%lldms",
            orderCode.toLocal8Bit().data(), items.size(), newMap->size(), orderQty, elapsed);

        LogCenter::Instance()->wcs_run_log_warn(true,
            QString("[Parse] orderCode=%1 items=%2 SKU=%3 elapsed=%4ms")
                .arg(orderCode).arg(items.size()).arg(newMap->size()).arg(elapsed));

        emit waveParsed(orderCode, newMap->size(), orderQty, elapsed, recvSet);
    }
}
