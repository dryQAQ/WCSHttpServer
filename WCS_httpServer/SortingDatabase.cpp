#include "SortingDatabase.h"
#include "LogService.h"
#include <QCoreApplication>
#include <QDir>
#include <QSqlQuery>
#include <QSqlError>
#include <QSqlRecord>
#include <QUuid>
#include <QDebug>
#include "define.h"

// 数据库操作专用日志宏（写入 ./log/DataBase/DataBase.log）
// （宏定义已移至 LogService.h 统一管理）

// ============================================================================
// 构造 / 析构
// ============================================================================

SortingDatabase::SortingDatabase()
{
    m_connectionName = "SortingDB_" + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

SortingDatabase::~SortingDatabase()
{
    close();
}

// ============================================================================
// 生命周期
// ============================================================================

bool SortingDatabase::open(const QString& dbPath)
{
    QMutexLocker locker(&m_mutex);
    if (m_bOpened) return true;

    if (dbPath.isEmpty())
        m_dbPath = QCoreApplication::applicationDirPath() + "/" SORTING_DB_FILE;
    else
        m_dbPath = dbPath;

    QDir dir = QFileInfo(m_dbPath).absoluteDir();
    if (!dir.exists()) dir.mkpath(".");

    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", m_connectionName);
    db.setDatabaseName(m_dbPath);

    if (!db.open()) {
        qWarning() << "[SortingDB] 数据库打开失败:" << db.lastError().text() << "path:" << m_dbPath;
        return false;
    }

    // WAL 模式优化
    {
        QSqlQuery q(db);
        q.exec(SQL_PRAGMA_WAL);
        q.exec(SQL_PRAGMA_SYNC);
        q.exec(SQL_PRAGMA_CACHE);
    }

    createTables();
    m_bOpened = true;
    qDebug() << "[SortingDB] 数据库已打开:" << m_dbPath;
    return true;
}

void SortingDatabase::close()
{
    QMutexLocker locker(&m_mutex);
    if (!m_bOpened) return;
    {
        QSqlDatabase db = QSqlDatabase::database(m_connectionName);
        if (db.isOpen()) db.close();
    }
    QSqlDatabase::removeDatabase(m_connectionName);
    m_bOpened = false;
    qDebug() << "[SortingDB] 数据库已关闭";
}

bool SortingDatabase::isOpen() const
{
    return m_bOpened;
}

// ★ 跨线程安全：在调用线程中按需创建数据库连接
// Qt SQLite 连接是线程隔离的，每个线程需要独立创建 addDatabase
// 否则在 PlcRecvPool 等线程池中调用 database() 会返回无效连接
QSqlDatabase SortingDatabase::ensureConnection()
{
    // 如果当前线程已有同名连接，直接返回
    if (QSqlDatabase::contains(m_connectionName))
    {
        QSqlDatabase db = QSqlDatabase::database(m_connectionName);
        if (db.isOpen()) return db;
    }

    // 在当前线程创建新连接
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", m_connectionName);
    db.setDatabaseName(m_dbPath);
    if (!db.open())
    {
        Data_ERROR("[SortingDB] 线程数据库打开失败 path=%s err=%s",
            m_dbPath.toLocal8Bit().data(),
            db.lastError().text().toLocal8Bit().data());
        return QSqlDatabase();
    }

    // 跨线程连接也启用 WAL 模式
    {
        QSqlQuery q(db);
        q.exec(SQL_PRAGMA_WAL);
        q.exec(SQL_PRAGMA_SYNC);
        q.exec(SQL_PRAGMA_CACHE);
    }

    return db;
}

// ============================================================================
// 建表
// ============================================================================

void SortingDatabase::createTables()
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return;
    QSqlQuery q(db);

    // ──── 原有表 ────
    q.exec(SQL_CREATE_TABLE_SORTING);
    q.exec(SQL_CREATE_INDEX_BARCODE);
    q.exec(SQL_CREATE_INDEX_ORDER);
    q.exec(SQL_CREATE_INDEX_TIME);

    // ──── S0 新增表 ────
    // 退货波次头
    q.exec(SQL_CREATE_TABLE_RETURN_WAVE);
    // 退货波次明细
    q.exec(SQL_CREATE_TABLE_RETURN_WAVE_ITEM);
    q.exec(SQL_CREATE_INDEX_WAVE_ITEM_ORDER);
    // 格口容器绑定
    q.exec(SQL_CREATE_TABLE_GRID_BOX_BIND);
    q.exec(SQL_CREATE_INDEX_BIND_GRID_ACTIVE);
    // 分拣流水
    q.exec(SQL_CREATE_TABLE_SORT_TXN);
    q.exec(SQL_CREATE_INDEX_SORT_TXN_ORDER);
    q.exec(SQL_CREATE_INDEX_SORT_TXN_EPC);
    // 满箱回传出站（H7）
    q.exec(SQL_CREATE_TABLE_OUTBOX_FULLBOX);
    q.exec(SQL_CREATE_INDEX_OUTBOX_ORDER);
    q.exec(SQL_CREATE_INDEX_OUTBOX_RETRY);
    // 完结回传出站（H8）
    q.exec(SQL_CREATE_TABLE_OUTBOX_END);
    // 异常记录
    q.exec(SQL_CREATE_TABLE_EXCEPTION_RECORD);

    if (q.lastError().isValid())
        qWarning() << "[SortingDB] 建表失败:" << q.lastError().text();
}

// ============================================================================
// 原有接口（保留兼容）
// ============================================================================

bool SortingDatabase::insertRecord(const QString& orderCode, const QString& barcode,
                                    const QString& gridNum, const QString& carNum,
                                    int gridCount, const QString& volu)
{
    QMutexLocker locker(&m_mutex);
    if (!m_bOpened) return false;
    QSqlDatabase db = ensureConnection();  // ★ 跨线程安全
    if (!db.isValid() || !db.isOpen())
    {
        Data_ERROR("[SortingDB] insertRecord 数据库不可用 barcode=%s", barcode.toLocal8Bit().data());
        return false;
    }

    QString now = currentTimeStr();
    QSqlQuery q(db);
    q.prepare(SQL_INSERT_RECORD);
    q.addBindValue(orderCode);
    q.addBindValue(barcode);
    q.addBindValue(gridNum);
    q.addBindValue(carNum);
    q.addBindValue(gridCount);
    q.addBindValue(volu.isEmpty() ? "--" : volu);
    q.addBindValue(now);
    q.addBindValue(now);
    if (!q.exec()) {
        Data_ERROR("[SortingDB] 插入失败: %s barcode=%s",
            q.lastError().text().toLocal8Bit().data(), barcode.toLocal8Bit().data());
        return false;
    }
    return true;
}

QVector<SortingRecord> SortingDatabase::queryByBarcode(const QString& barcode, int limit)
{
    QMutexLocker locker(&m_mutex);
    QVector<SortingRecord> result;
    if (!m_bOpened) return result;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return result;

    // ① 查询实际分拣记录（sorting_records 表，PLC 落格反馈写入）
    QSqlQuery q(db);
    q.prepare(SQL_QUERY_BY_BARCODE);
    q.addBindValue(barcode);
    q.addBindValue(limit);
    if (q.exec()) {
        while (q.next()) {
            SortingRecord rec;
            rec.id = q.value(0).toInt();
            rec.orderCode = q.value(1).toString();
            rec.barcode = q.value(2).toString();
            rec.gridNum = q.value(3).toString();
            rec.carNum = q.value(4).toString();
            rec.gridCount = q.value(5).toInt();
            rec.volu = q.value(6).toString();
            rec.sortTime = q.value(7).toString();
            rec.createTime = q.value(8).toString();
            rec.status = QString::fromUtf8("已分拣");  // 来自 sorting_records 表，PLC 已落格
            result.append(rec);
        }
    }

    // ② 查询波次计划明细（return_wave_item 表，波次下发时写入）
    //    使用 NOT EXISTS 排除已在 sorting_records 中落格的同波次同EPC编码，
    //    保证同一 (order_code, inco) 不会同时出现「已分拣」和「待分拣」两种状态
    {
        QSqlQuery q2(db);
        // SQL_QUERY_PENDING_BY_BARCODE:
        //   SELECT i.order_code, i.inco, i.grid_num, i.plan_qty, i.volu
        //   FROM return_wave_item i
        //   WHERE i.inco = ? AND NOT EXISTS (
        //     SELECT 1 FROM sorting_records s
        //     WHERE s.barcode = i.inco AND s.order_code = i.order_code)
        //   ORDER BY i.id DESC LIMIT ?
        q2.prepare(SQL_QUERY_PENDING_BY_BARCODE);
        q2.addBindValue(barcode);
        q2.addBindValue(limit);
        if (q2.exec()) {
            while (q2.next()) {
                SortingRecord rec;
                rec.id = 0;  // 计划明细无自增ID
                rec.orderCode = q2.value(0).toString();
                rec.barcode = q2.value(1).toString();
                rec.gridNum = q2.value(2).toString();
                rec.carNum = "1";  // 计划明细无小车号，默认=1
                rec.gridCount = q2.value(3).toInt();  // plan_qty
                rec.volu = q2.value(4).toString();
                rec.sortTime = "";   // 尚未分拣，无分拣时间
                rec.createTime = ""; // 计划明细无创建时间
                rec.status = QString::fromUtf8("待分拣");  // 来自 return_wave_item 表，尚未落格
                result.append(rec);
            }
        }
    }

    return result;
}

QVector<SortingRecord> SortingDatabase::queryByTime(const QDateTime& from, const QDateTime& to, int limit)
{
    QMutexLocker locker(&m_mutex);
    QVector<SortingRecord> result;
    if (!m_bOpened) return result;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return result;

    QSqlQuery q(db);
    q.prepare(SQL_QUERY_BY_TIME);
    q.addBindValue(from.toString("yyyy-MM-dd HH:mm:ss"));
    q.addBindValue(to.toString("yyyy-MM-dd HH:mm:ss.zzz"));
    q.addBindValue(limit);
    if (!q.exec()) return result;
    while (q.next()) {
        SortingRecord rec;
        rec.id = q.value(0).toInt();
        rec.orderCode = q.value(1).toString();
        rec.barcode = q.value(2).toString();
        rec.gridNum = q.value(3).toString();
        rec.carNum = q.value(4).toString();
        rec.gridCount = q.value(5).toInt();
        rec.volu = q.value(6).toString();
        rec.sortTime = q.value(7).toString();
        rec.createTime = q.value(8).toString();
        result.append(rec);
    }
    return result;
}

QVector<SortingRecord> SortingDatabase::queryByOrderCode(const QString& orderCode, int limit)
{
    QMutexLocker locker(&m_mutex);
    QVector<SortingRecord> result;
    if (!m_bOpened) return result;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return result;

    QSqlQuery q(db);
    q.prepare(SQL_QUERY_BY_ORDER);
    q.addBindValue(orderCode);
    q.addBindValue(limit);
    if (!q.exec()) return result;
    while (q.next()) {
        SortingRecord rec;
        rec.id = q.value(0).toInt();
        rec.orderCode = q.value(1).toString();
        rec.barcode = q.value(2).toString();
        rec.gridNum = q.value(3).toString();
        rec.carNum = q.value(4).toString();
        rec.gridCount = q.value(5).toInt();
        rec.volu = q.value(6).toString();
        rec.sortTime = q.value(7).toString();
        rec.createTime = q.value(8).toString();
        result.append(rec);
    }
    return result;
}

QVector<SortingRecord> SortingDatabase::queryAll(int limit)
{
    QMutexLocker locker(&m_mutex);
    QVector<SortingRecord> result;
    if (!m_bOpened) return result;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return result;

    QSqlQuery q(db);
    q.prepare(SQL_QUERY_ALL);
    q.addBindValue(limit);
    if (!q.exec()) return result;
    while (q.next()) {
        SortingRecord rec;
        rec.id = q.value(0).toInt();
        rec.orderCode = q.value(1).toString();
        rec.barcode = q.value(2).toString();
        rec.gridNum = q.value(3).toString();
        rec.carNum = q.value(4).toString();
        rec.gridCount = q.value(5).toInt();
        rec.volu = q.value(6).toString();
        rec.sortTime = q.value(7).toString();
        rec.createTime = q.value(8).toString();
        result.append(rec);
    }
    return result;
}

// ═════════════════════════════════════════════════════════════════════════════
// ★ queryAllWithPending — 留空查全部：已分拣 + 待分拣（UI 查询面板）
// 返回所有已分拣记录（sorting_records）和所有待分拣记录（return_wave_item，
// 用 NOT EXISTS 排除已落格的EPC编码），合并后按时间倒序（已分拣在前）
// ═════════════════════════════════════════════════════════════════════════════
QVector<SortingRecord> SortingDatabase::queryAllWithPending(int limit)
{
    QMutexLocker locker(&m_mutex);
    QVector<SortingRecord> result;
    if (!m_bOpened) return result;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return result;

    // ① 已分拣：sorting_records 全部记录
    {
        QSqlQuery q(db);
        q.prepare(SQL_QUERY_ALL);
        q.addBindValue(limit);
        if (q.exec()) {
            while (q.next()) {
                SortingRecord rec;
                rec.id = q.value(0).toInt();
                rec.orderCode = q.value(1).toString();
                rec.barcode = q.value(2).toString();
                rec.gridNum = q.value(3).toString();
                rec.carNum = q.value(4).toString();
                rec.gridCount = q.value(5).toInt();
                rec.volu = q.value(6).toString();
                rec.sortTime = q.value(7).toString();
                rec.createTime = q.value(8).toString();
                rec.status = QString::fromUtf8("已分拣");
                result.append(rec);
            }
        }
    }

    // ② 待分拣：return_wave_item 中排除已落格（NOT EXISTS）
    {
        QSqlQuery q(db);
        // SQL_QUERY_ALL_PENDING:
        //   SELECT i.order_code, i.inco, i.grid_num, i.plan_qty, i.volu
        //   FROM return_wave_item i
        //   WHERE NOT EXISTS (SELECT 1 FROM sorting_records s
        //     WHERE s.barcode = i.inco AND s.order_code = i.order_code)
        //   ORDER BY i.id DESC LIMIT ?
        q.prepare(SQL_QUERY_ALL_PENDING);
        q.addBindValue(limit);
        if (q.exec()) {
            while (q.next()) {
                SortingRecord rec;
                rec.id = 0;
                rec.orderCode = q.value(0).toString();
                rec.barcode = q.value(1).toString();
                rec.gridNum = q.value(2).toString();
                rec.carNum = "1";
                rec.gridCount = q.value(3).toInt();
                rec.volu = q.value(4).toString();
                rec.sortTime = "";
                rec.createTime = "";
                rec.status = QString::fromUtf8("待分拣");
                result.append(rec);
            }
        }
    }

    return result;
}

SortingStatistics SortingDatabase::statistics()
{
    QMutexLocker locker(&m_mutex);
    SortingStatistics stats;
    if (!m_bOpened) return stats;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return stats;

    QSqlQuery q(db);
    q.exec(SQL_COUNT_ALL);
    if (q.next()) stats.totalRecords = q.value(0).toInt();
    QString today = QDate::currentDate().toString("yyyy-MM-dd");
    q.prepare(SQL_COUNT_TODAY);
    q.addBindValue(today + " 00:00:00");
    if (q.exec() && q.next()) stats.todayRecords = q.value(0).toInt();
    q.exec(SQL_COUNT_WAVES);
    if (q.next()) stats.totalWaves = q.value(0).toInt();
    q.exec(SQL_COUNT_GRIDS);
    if (q.next()) stats.totalGrids = q.value(0).toInt();
    q.exec(SQL_LAST_SORT_TIME);
    if (q.next()) stats.lastSortTime = q.value(0).toString();
    return stats;
}

int SortingDatabase::recordCount() const
{
    QMutexLocker locker(&m_mutex);
    if (!m_bOpened) return 0;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return 0;
    QSqlQuery q(db);
    q.exec(SQL_COUNT_ALL);
    if (q.next()) return q.value(0).toInt();
    return 0;
}

int SortingDatabase::todayRecordCount() const
{
    QMutexLocker locker(&m_mutex);
    if (!m_bOpened) return 0;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return 0;
    QString today = QDate::currentDate().toString("yyyy-MM-dd");
    QSqlQuery q(db);
    q.prepare(SQL_COUNT_TODAY);
    q.addBindValue(today + " 00:00:00");
    if (q.exec() && q.next()) return q.value(0).toInt();
    return 0;
}

void SortingDatabase::cleanupOldRecords(int retainDays)
{
    QMutexLocker locker(&m_mutex);
    if (!m_bOpened) return;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return;
    QDateTime cutoff = QDateTime::currentDateTime().addDays(-retainDays);
    QSqlQuery q(db);
    q.prepare(SQL_DELETE_OLD);
    q.addBindValue(cutoff.toString("yyyy-MM-dd HH:mm:ss"));
    if (q.exec()) {
        int deleted = q.numRowsAffected();
        if (deleted > 0)
            qDebug() << "[SortingDB] 已清理" << deleted << "条旧记录（" << retainDays << "天前）";
        q.exec(SQL_PRAGMA_OPTIMIZE);
    }
}

// ============================================================================
// S0 新增：波次管理
// ============================================================================

bool SortingDatabase::upsertReturnWave(const QString& orderCode, int orderQty, int status)
{
    QMutexLocker locker(&m_mutex);
    if (!m_bOpened) return false;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return false;

    QString now = currentTimeStr();
    QSqlQuery q(db);
    // INSERT OR REPLACE INTO return_wave (order_code, order_qty, status, created_at, updated_at) VALUES (?, ?, ?, ?, ?)
    q.prepare(SQL_UPSERT_RETURN_WAVE);
    q.addBindValue(orderCode);
    q.addBindValue(orderQty);
    q.addBindValue(status);
    q.addBindValue(now);
    q.addBindValue(now);
    return q.exec();
}

ReturnWaveRecord SortingDatabase::getReturnWave(const QString& orderCode)
{
    QMutexLocker locker(&m_mutex);
    ReturnWaveRecord rec;
    if (!m_bOpened) return rec;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return rec;

    QSqlQuery q(db);
    q.prepare(SQL_SELECT_WAVE_BY_ORDER);
    q.addBindValue(orderCode);
    if (q.exec() && q.next()) {
        rec.orderCode = q.value(0).toString();
        rec.orderQty = q.value(1).toInt();
        rec.status = q.value(2).toInt();
        rec.createdAt = q.value(3).toString();
        rec.updatedAt = q.value(4).toString();
    }
    return rec;
}

bool SortingDatabase::updateWaveStatus(const QString& orderCode, int newStatus)
{
    QMutexLocker locker(&m_mutex);
    if (!m_bOpened) return false;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return false;

    QSqlQuery q(db);
    // UPDATE return_wave SET status = ?, updated_at = ? WHERE order_code = ?
    q.prepare(SQL_UPDATE_WAVE_STATUS);
    q.addBindValue(newStatus);
    q.addBindValue(currentTimeStr());
    q.addBindValue(orderCode);
    return q.exec();
}

int SortingDatabase::getWaveStatus(const QString& orderCode)
{
    QMutexLocker locker(&m_mutex);
    if (!m_bOpened) return -1;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return -1;

    QSqlQuery q(db);
    q.prepare(SQL_SELECT_WAVE_BY_ORDER);
    q.addBindValue(orderCode);
    if (q.exec() && q.next())
        return q.value(2).toInt();
    return -1;  // 不存在
}

// ★ 查询最近一条未完成波次（排除已取消=6和已完成=8），用于软件重启后恢复波次数据
ReturnWaveRecord SortingDatabase::getLatestUnfinishedWave()
{
    QMutexLocker locker(&m_mutex);
    ReturnWaveRecord rec;
    if (!m_bOpened) return rec;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return rec;

    QSqlQuery q(db);
    q.prepare(SQL_SELECT_LATEST_UNFINISHED_WAVE);
    if (q.exec() && q.next()) {
        rec.orderCode = q.value(0).toString();
        rec.orderQty = q.value(1).toInt();
        rec.status = q.value(2).toInt();
        rec.createdAt = q.value(3).toString();
        rec.updatedAt = q.value(4).toString();
    }
    return rec;
}

bool SortingDatabase::insertWaveItems(const QString& orderCode, const QVector<ReturnWaveItemRecord>& items)
{
    QMutexLocker locker(&m_mutex);
    if (!m_bOpened) return false;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return false;

    // ★ 使用事务包裹，保证原子性 + 性能
    db.transaction();

    // 先删除旧明细（覆盖重下）
    {
        QSqlQuery q(db);
        q.prepare("DELETE FROM return_wave_item WHERE order_code = ?");
        q.addBindValue(orderCode);
        if (!q.exec())
        {
            Data_WARN("[SortingDB] 删除旧明细失败 orderCode=%s err=%s",
                orderCode.toLocal8Bit().data(), q.lastError().text().toLocal8Bit().data());
            db.rollback();
            return false;
        }
    }

    // 批量插入新明细（使用事务包裹）
    // ★ 使用直接SQL构造替代 prepare/bindValue，彻底避开 Qt 绑值累积问题
    //   转义单引号防止 SQL 注入（数据来自 ParseWorker 解析的 JSON，可信但做防御）
    {
        auto esc = [](const QString& s) -> QString {
            QString r = s;
            return r.replace(QLatin1Char('\''), QLatin1String("''"));
        };
        for (const auto& item : items)
        {
            QString sql = QString(
                "INSERT INTO return_wave_item "
                "(order_code, inco, grid_num, grid_type, plan_qty, sorted_qty, volu, obx_code) "
                "VALUES ('%1', '%2', '%3', '%4', %5, 0, '%6', '%7')")
                .arg(esc(item.orderCode),
                     esc(item.inco),
                     esc(item.gridNum),
                     esc(item.gridType.isEmpty() ? QString("0") : item.gridType),  // 0=分类, 1=异常, 2=发货
                     QString::number(item.planQty),
                     esc(item.volu.isNull() ? QString("") : item.volu),
                     esc(item.obxCode.isNull() ? QString("") : item.obxCode));

            QSqlQuery q(db);
            if (!q.exec(sql))
            {
                Data_WARN("[SortingDB] 插入明细失败 orderCode=%s inco=%s grid=%s err=%s",
                    item.orderCode.toLocal8Bit().data(),
                    item.inco.toLocal8Bit().data(),
                    item.gridNum.toLocal8Bit().data(),
                    q.lastError().text().toLocal8Bit().data());
                db.rollback();
                return false;
            }
        }
    }

    if (!db.commit())
    {
        Data_WARN("[SortingDB] 事务提交失败 orderCode=%s err=%s",
            orderCode.toLocal8Bit().data(), db.lastError().text().toLocal8Bit().data());
        db.rollback();
        return false;
    }

    Data_INFO("[SortingDB] 波次明细写入成功 orderCode=%s count=%d",
        orderCode.toLocal8Bit().data(), items.size());
    return true;
}

QVector<ReturnWaveItemRecord> SortingDatabase::getWaveItems(const QString& orderCode)
{
    QMutexLocker locker(&m_mutex);
    QVector<ReturnWaveItemRecord> result;
    if (!m_bOpened) return result;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return result;

    QSqlQuery q(db);
    q.prepare(SQL_SELECT_WAVE_ITEMS);
    q.addBindValue(orderCode);
    if (!q.exec()) return result;
    while (q.next()) {
        ReturnWaveItemRecord rec;
        rec.id = q.value(0).toInt();
        rec.orderCode = q.value(1).toString();
        rec.inco = q.value(2).toString();
        rec.gridNum = q.value(3).toString();
        rec.gridType = q.value(4).toString();
        rec.planQty = q.value(5).toInt();
        rec.sortedQty = q.value(6).toInt();
        rec.volu = q.value(7).toString();
        rec.obxCode = q.value(8).toString();  // ★ 容器号
        result.append(rec);
    }
    return result;
}

bool SortingDatabase::incrementSortedQty(const QString& orderCode, const QString& inco, const QString& gridNum)
{
    QMutexLocker locker(&m_mutex);
    if (!m_bOpened) return false;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return false;

    QSqlQuery q(db);
    // UPDATE return_wave_item SET sorted_qty = sorted_qty + 1 WHERE order_code = ? AND inco = ? AND grid_num = ?
    q.prepare(SQL_INCREMENT_SORTED_QTY);
    q.addBindValue(orderCode);
    q.addBindValue(inco);
    q.addBindValue(gridNum);
    return q.exec();
}

// ============================================================================
// S0 新增：容器绑定
// ============================================================================

bool SortingDatabase::bindGridBox(const QString& gridNum, const QString& boxcode, const QString& orderCode)
{
    QMutexLocker locker(&m_mutex);
    if (!m_bOpened) return false;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return false;

    QString now = currentTimeStr();

    // 先归档旧绑定
    {
        QSqlQuery q(db);
        // UPDATE grid_box_bind SET active = 0, unbind_time = ? WHERE grid_num = ? AND active = 1
        q.prepare(SQL_ARCHIVE_OLD_BIND);
        q.addBindValue(now);
        q.addBindValue(gridNum);
        q.exec();
    }

    // 插入新绑定
    QSqlQuery q(db);
    // INSERT INTO grid_box_bind (grid_num, boxcode, order_code, active, bind_time, unbind_time) VALUES (?, ?, ?, 1, ?, '')
    q.prepare(SQL_INSERT_GRID_BIND);
    q.addBindValue(gridNum);
    q.addBindValue(boxcode);
    q.addBindValue(orderCode);
    q.addBindValue(now);
    return q.exec();
}

GridBoxBindRecord SortingDatabase::getActiveBind(const QString& gridNum)
{
    QMutexLocker locker(&m_mutex);
    GridBoxBindRecord rec;
    if (!m_bOpened) return rec;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return rec;

    QSqlQuery q(db);
    q.prepare(SQL_SELECT_ACTIVE_BIND);
    q.addBindValue(gridNum);
    if (q.exec() && q.next()) {
        rec.gridNum = gridNum;
        rec.boxcode = q.value(0).toString();
        rec.orderCode = q.value(1).toString();
        rec.bindTime = q.value(2).toString();
        rec.active = true;
    }
    return rec;
}

bool SortingDatabase::archiveGridBinds(const QString& gridNum)
{
    QMutexLocker locker(&m_mutex);
    if (!m_bOpened) return false;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return false;

    QSqlQuery q(db);
    q.prepare(SQL_ARCHIVE_OLD_BIND);
    q.addBindValue(currentTimeStr());
    q.addBindValue(gridNum);
    return q.exec();
}

// ============================================================================
// S0 新增：分拣流水
// ============================================================================

bool SortingDatabase::insertSortTxn(const SortTxnRecord& txn)
{
    QMutexLocker locker(&m_mutex);
    if (!m_bOpened) return false;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return false;

    QSqlQuery q(db);
    q.prepare(SQL_INSERT_SORT_TXN);
    q.addBindValue(txn.orderCode);
    q.addBindValue(txn.epc);
    q.addBindValue(txn.sku);
    q.addBindValue(txn.gridNum);
    q.addBindValue(txn.boxcode);
    q.addBindValue(txn.result);
    q.addBindValue(txn.reason);
    q.addBindValue(txn.sortTime.isEmpty() ? currentTimeStr() : txn.sortTime);
    return q.exec();
}

bool SortingDatabase::isEpcAlreadySorted(const QString& orderCode, const QString& epc)
{
    QMutexLocker locker(&m_mutex);
    if (!m_bOpened) return false;
    QSqlDatabase db = ensureConnection();  // ★ 跨线程安全
    if (!db.isValid() || !db.isOpen()) return false;

    QSqlQuery q(db);
    q.prepare(SQL_SELECT_TXN_BY_EPC);
    q.addBindValue(orderCode);
    q.addBindValue(epc);
    if (q.exec() && q.next())
        return true;
    return false;
}

// ============================================================================
// S0 新增：Outbox 出站
// ============================================================================

bool SortingDatabase::insertOutboxFullbox(const OutboxRecord& msg)
{
    QMutexLocker locker(&m_mutex);
    if (!m_bOpened) return false;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return false;

    QSqlQuery q(db);
    q.prepare(SQL_INSERT_OUTBOX_FULLBOX);
    q.addBindValue(msg.msgId);
    q.addBindValue(msg.orderCode);
    q.addBindValue(msg.boxcode);
    q.addBindValue(msg.payload);
    q.addBindValue(msg.nextRetry);
    q.addBindValue(msg.createdAt.isEmpty() ? currentTimeStr() : msg.createdAt);
    return q.exec();
}

bool SortingDatabase::insertOutboxEnd(const OutboxRecord& msg)
{
    QMutexLocker locker(&m_mutex);
    if (!m_bOpened) return false;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return false;

    QSqlQuery q(db);
    q.prepare(SQL_INSERT_OUTBOX_END);
    q.addBindValue(msg.msgId);
    q.addBindValue(msg.orderCode);
    q.addBindValue(msg.payload);
    q.addBindValue(msg.nextRetry);
    q.addBindValue(msg.createdAt.isEmpty() ? currentTimeStr() : msg.createdAt);
    return q.exec();
}

QVector<OutboxRecord> SortingDatabase::getPendingOutboxFullbox(int limit)
{
    QMutexLocker locker(&m_mutex);
    QVector<OutboxRecord> result;
    if (!m_bOpened) return result;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return result;

    QSqlQuery q(db);
    q.prepare(SQL_SELECT_OUTBOX_PENDING);
    q.addBindValue(currentTimeStr());
    q.addBindValue(limit);
    if (!q.exec()) return result;
    while (q.next()) {
        OutboxRecord rec;
        rec.msgId = q.value(0).toString();
        rec.orderCode = q.value(1).toString();
        rec.boxcode = q.value(2).toString();
        rec.payload = q.value(3).toString();
        rec.retryCount = q.value(4).toInt();
        result.append(rec);
    }
    return result;
}

bool SortingDatabase::updateOutboxFullboxStatus(const QString& msgId, const QString& newStatus, const QString& nextRetry)
{
    QMutexLocker locker(&m_mutex);
    if (!m_bOpened) return false;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return false;

    QSqlQuery q(db);
    q.prepare(SQL_UPDATE_OUTBOX_STATUS);
    q.addBindValue(newStatus);
    q.addBindValue(nextRetry);
    q.addBindValue(msgId);
    return q.exec();
}

bool SortingDatabase::markOutboxFullboxSuccess(const QString& msgId)
{
    QMutexLocker locker(&m_mutex);
    if (!m_bOpened) return false;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return false;

    QSqlQuery q(db);
    q.prepare(SQL_MARK_OUTBOX_SUCCESS);
    q.addBindValue(msgId);
    return q.exec();
}

// ──── 完结回传出站消息操作（H8）────

QVector<OutboxRecord> SortingDatabase::getPendingOutboxEnd(int limit)
{
    QMutexLocker locker(&m_mutex);
    QVector<OutboxRecord> result;
    if (!m_bOpened) return result;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return result;

    QSqlQuery q(db);
    // SELECT msg_id, order_code, payload, retry_count FROM outbox_end WHERE status = 'pending' AND next_retry <= ? ORDER BY created_at ASC LIMIT ?
    q.prepare(SQL_SELECT_OUTBOX_END_PENDING);
    q.addBindValue(currentTimeStr());
    q.addBindValue(limit);
    if (!q.exec()) return result;
    while (q.next()) {
        OutboxRecord rec;
        rec.msgId = q.value(0).toString();
        rec.orderCode = q.value(1).toString();
        rec.payload = q.value(2).toString();
        rec.retryCount = q.value(3).toInt();
        result.append(rec);
    }
    return result;
}

bool SortingDatabase::updateOutboxEndStatus(const QString& msgId, const QString& newStatus, const QString& nextRetry)
{
    QMutexLocker locker(&m_mutex);
    if (!m_bOpened) return false;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return false;

    QSqlQuery q(db);
    // UPDATE outbox_end SET status = ?, retry_count = retry_count + 1, next_retry = ? WHERE msg_id = ?
    q.prepare(SQL_UPDATE_OUTBOX_END_STATUS);
    q.addBindValue(newStatus);
    q.addBindValue(nextRetry);
    q.addBindValue(msgId);
    return q.exec();
}

bool SortingDatabase::markOutboxEndSuccess(const QString& msgId)
{
    QMutexLocker locker(&m_mutex);
    if (!m_bOpened) return false;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return false;

    QSqlQuery q(db);
    // UPDATE outbox_end SET status = 'success' WHERE msg_id = ?
    q.prepare(SQL_MARK_OUTBOX_END_SUCCESS);
    q.addBindValue(msgId);
    return q.exec();
}

// ──── 人工重发查询 ────

QVector<OutboxRecord> SortingDatabase::getOutboxByOrderCode(const QString& orderCode)
{
    QMutexLocker locker(&m_mutex);
    QVector<OutboxRecord> result;
    if (!m_bOpened) return result;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return result;

    QSqlQuery q(db);
    // SELECT msg_id, order_code, boxcode, payload, retry_count FROM outbox_fullbox WHERE order_code = ? AND status = 'pending'
    q.prepare(SQL_SELECT_OUTBOX_BY_ORDER);
    q.addBindValue(orderCode);
    if (!q.exec()) return result;
    while (q.next()) {
        OutboxRecord rec;
        rec.msgId = q.value(0).toString();
        rec.orderCode = q.value(1).toString();
        rec.boxcode = q.value(2).toString();
        rec.payload = q.value(3).toString();
        rec.retryCount = q.value(4).toInt();
        result.append(rec);
    }
    return result;
}

OutboxRecord SortingDatabase::getOutboxFullboxByMsgId(const QString& msgId)
{
    QMutexLocker locker(&m_mutex);
    OutboxRecord rec;
    if (!m_bOpened) return rec;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return rec;

    QSqlQuery q(db);
    // SELECT msg_id, order_code, boxcode, payload, retry_count FROM outbox_fullbox WHERE msg_id = ?
    q.prepare(SQL_SELECT_OUTBOX_BY_MSGID);
    q.addBindValue(msgId);
    if (q.exec() && q.next()) {
        rec.msgId = q.value(0).toString();
        rec.orderCode = q.value(1).toString();
        rec.boxcode = q.value(2).toString();
        rec.payload = q.value(3).toString();
        rec.retryCount = q.value(4).toInt();
    }
    return rec;
}

// ★ S6 完结回传按 msgId 查询（H8）
OutboxRecord SortingDatabase::getOutboxEndByMsgId(const QString& msgId)
{
    QMutexLocker locker(&m_mutex);
    OutboxRecord rec;
    if (!m_bOpened) return rec;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return rec;

    QSqlQuery q(db);
    // SELECT msg_id, order_code, boxcode, payload, retry_count FROM outbox_end WHERE msg_id = ?
    q.prepare(SQL_SELECT_OUTBOX_END_BY_MSGID);
    q.addBindValue(msgId);
    if (q.exec() && q.next()) {
        rec.msgId = q.value(0).toString();
        rec.orderCode = q.value(1).toString();
        rec.boxcode = q.value(2).toString();
        rec.payload = q.value(3).toString();
        rec.retryCount = q.value(4).toInt();
    }
    return rec;
}

// ============================================================================
// S0 新增：异常记录
// ============================================================================

bool SortingDatabase::insertException(const ExceptionRecord& ex)
{
    QMutexLocker locker(&m_mutex);
    if (!m_bOpened) return false;
    QSqlDatabase db = ensureConnection();  // ★ 跨线程安全
    if (!db.isValid() || !db.isOpen()) return false;

    QSqlQuery q(db);
    q.prepare(SQL_INSERT_EXCEPTION);
    q.addBindValue(ex.type);
    q.addBindValue(ex.orderCode);
    q.addBindValue(ex.epc);
    q.addBindValue(ex.sku);
    q.addBindValue(ex.reason);
    q.addBindValue(ex.time.isEmpty() ? currentTimeStr() : ex.time);
    return q.exec();
}

QVector<ExceptionRecord> SortingDatabase::getOpenExceptions(const QString& orderCode)
{
    QMutexLocker locker(&m_mutex);
    QVector<ExceptionRecord> result;
    if (!m_bOpened) return result;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return result;

    QSqlQuery q(db);
    q.prepare(SQL_SELECT_OPEN_EXCEPTIONS);
    q.addBindValue(orderCode);
    if (!q.exec()) return result;
    while (q.next()) {
        ExceptionRecord rec;
        rec.id = q.value(0).toInt();
        rec.type = q.value(1).toString();
        rec.orderCode = q.value(2).toString();
        rec.epc = q.value(3).toString();
        rec.sku = q.value(4).toString();
        rec.reason = q.value(5).toString();
        rec.time = q.value(6).toString();
        result.append(rec);
    }
    return result;
}

// ★ S7 多条件异常查询（T-S7-03）
// 支持按 orderCode、EPC、异常类型、时间范围组合查询
QVector<ExceptionRecord> SortingDatabase::queryExceptions(
    const QString& orderCode, const QString& epc,
    const QString& type, const QString& startTime,
    const QString& endTime, int limit)
{
    QMutexLocker locker(&m_mutex);
    QVector<ExceptionRecord> result;
    if (!m_bOpened) return result;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return result;

    // 动态构建 SQL：WHERE 条件按传入参数拼接
    QString sql = "SELECT id, type, order_code, epc, sku, reason, time FROM exception_record WHERE 1=1";
    QVector<QPair<QString, QVariant>> bindings;

    if (!orderCode.isEmpty()) {
        sql += " AND order_code = ?";
        bindings.append({"order_code", orderCode});
    }
    if (!epc.isEmpty()) {
        sql += " AND epc = ?";
        bindings.append({"epc", epc});
    }
    if (!type.isEmpty()) {
        sql += " AND type = ?";
        bindings.append({"type", type});
    }
    if (!startTime.isEmpty()) {
        sql += " AND time >= ?";
        bindings.append({"startTime", startTime});
    }
    if (!endTime.isEmpty()) {
        sql += " AND time <= ?";
        bindings.append({"endTime", endTime});
    }
    sql += " ORDER BY time DESC LIMIT ?";
    bindings.append({"limit", limit});

    QSqlQuery q(db);
    q.prepare(sql);
    for (const auto& b : bindings)
        q.addBindValue(b.second);
    if (!q.exec()) return result;
    while (q.next()) {
        ExceptionRecord rec;
        rec.id = q.value(0).toInt();
        rec.type = q.value(1).toString();
        rec.orderCode = q.value(2).toString();
        rec.epc = q.value(3).toString();
        rec.sku = q.value(4).toString();
        rec.reason = q.value(5).toString();
        rec.time = q.value(6).toString();
        result.append(rec);
    }
    return result;
}

// ============================================================================
// 辅助
// ============================================================================

QString SortingDatabase::currentTimeStr() const
{
    return QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
}
