#include "SortingDatabase.h"
#include "define.h"
#include <QCoreApplication>
#include <QDir>
#include <QSqlQuery>
#include <QSqlError>
#include <QSqlRecord>
#include <QUuid>
#include <QDebug>

// ============================================================================
// 构造 / 析构
// ============================================================================

SortingDatabase::SortingDatabase()
{
    // 使用 UUID 生成唯一连接名，避免多次创建实例时连接名冲突
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

    // 确定数据库文件路径：优先使用传入路径，否则使用 define.h 中的 SORTING_DB_FILE
    // SORTING_DB_FILE = "data/sorting_records.db"（exe 同目录下 data 文件夹）
    if (dbPath.isEmpty())
    {
        m_dbPath = QCoreApplication::applicationDirPath() + "/" SORTING_DB_FILE;
    }
    else
    {
        m_dbPath = dbPath;
    }

    // 确保目录存在
    QDir dir = QFileInfo(m_dbPath).absoluteDir();
    if (!dir.exists())
        dir.mkpath(".");

    // 创建 SQLite 连接
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", m_connectionName);
    db.setDatabaseName(m_dbPath);

    if (!db.open())
    {
        qWarning() << "[SortingDB] 数据库打开失败:" << db.lastError().text()
                    << "path:" << m_dbPath;
        return false;
    }

    // 启用 WAL 模式（提高并发读写性能）
    // SQL: PRAGMA journal_mode=WAL
    // SQL: PRAGMA synchronous=NORMAL
    // SQL: PRAGMA cache_size=5000
    {
        QSqlQuery q(db);
        q.exec(SQL_PRAGMA_WAL);    // PRAGMA journal_mode=WAL
        q.exec(SQL_PRAGMA_SYNC);   // PRAGMA synchronous=NORMAL
        q.exec(SQL_PRAGMA_CACHE);  // PRAGMA cache_size=5000
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
        if (db.isOpen())
            db.close();
    }
    QSqlDatabase::removeDatabase(m_connectionName);

    m_bOpened = false;
    qDebug() << "[SortingDB] 数据库已关闭";
}

bool SortingDatabase::isOpen() const
{
    return m_bOpened;
}

// ============================================================================
// 建表
// ============================================================================

void SortingDatabase::createTables()
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return;

    QSqlQuery q(db);

    // CREATE TABLE IF NOT EXISTS sorting_records (
    //   id          INTEGER PRIMARY KEY AUTOINCREMENT,
    //   order_code  TEXT    NOT NULL DEFAULT '',
    //   barcode     TEXT    NOT NULL DEFAULT '',
    //   grid_num    TEXT    NOT NULL DEFAULT '',
    //   car_num     TEXT    NOT NULL DEFAULT '1',
    //   grid_count  INTEGER NOT NULL DEFAULT 0,
    //   volu        TEXT    NOT NULL DEFAULT '',
    //   sort_time   TEXT    NOT NULL DEFAULT '',
    //   create_time TEXT    NOT NULL DEFAULT ''
    // )
    q.exec(SQL_CREATE_TABLE_SORTING);

    // CREATE INDEX IF NOT EXISTS idx_barcode    ON sorting_records(barcode)
    q.exec(SQL_CREATE_INDEX_BARCODE);
    // CREATE INDEX IF NOT EXISTS idx_order_code ON sorting_records(order_code)
    q.exec(SQL_CREATE_INDEX_ORDER);
    // CREATE INDEX IF NOT EXISTS idx_sort_time  ON sorting_records(sort_time)
    q.exec(SQL_CREATE_INDEX_TIME);

    if (q.lastError().isValid())
    {
        qWarning() << "[SortingDB] 建表失败:" << q.lastError().text();
    }
}

// ============================================================================
// 写入
// ============================================================================

bool SortingDatabase::insertRecord(const QString& orderCode,
                                    const QString& barcode,
                                    const QString& gridNum,
                                    const QString& carNum,
                                    int gridCount,
                                    const QString& volu)
{
    QMutexLocker locker(&m_mutex);

    if (!m_bOpened) return false;

    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return false;

    QString now = currentTimeStr();

    QSqlQuery q(db);
    // INSERT INTO sorting_records
    // (order_code, barcode, grid_num, car_num, grid_count, volu, sort_time, create_time)
    // VALUES (?, ?, ?, ?, ?, ?, ?, ?)
    q.prepare(SQL_INSERT_RECORD);
    q.addBindValue(orderCode);
    q.addBindValue(barcode);
    q.addBindValue(gridNum);
    q.addBindValue(carNum);
    q.addBindValue(gridCount);
    q.addBindValue(volu.isEmpty() ? "--" : volu);
    q.addBindValue(now);
    q.addBindValue(now);

    if (!q.exec())
    {
        qWarning() << "[SortingDB] 插入失败:" << q.lastError().text()
                    << "barcode:" << barcode;
        return false;
    }

    return true;
}

// ============================================================================
// 查询
// ============================================================================

QVector<SortingRecord> SortingDatabase::queryByBarcode(const QString& barcode, int limit)
{
    QMutexLocker locker(&m_mutex);
    QVector<SortingRecord> result;

    if (!m_bOpened) return result;

    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return result;

    QSqlQuery q(db);
    // SELECT id, order_code, barcode, grid_num, car_num, grid_count, volu, sort_time, create_time
    // FROM sorting_records WHERE barcode = ? ORDER BY id DESC LIMIT ?
    q.prepare(SQL_QUERY_BY_BARCODE);
    q.addBindValue(barcode);
    q.addBindValue(limit);

    if (!q.exec()) return result;

    while (q.next())
    {
        SortingRecord rec;
        rec.id         = q.value(0).toInt();
        rec.orderCode  = q.value(1).toString();
        rec.barcode    = q.value(2).toString();
        rec.gridNum    = q.value(3).toString();
        rec.carNum     = q.value(4).toString();
        rec.gridCount  = q.value(5).toInt();
        rec.volu       = q.value(6).toString();
        rec.sortTime   = q.value(7).toString();
        rec.createTime = q.value(8).toString();
        result.append(rec);
    }

    return result;
}

QVector<SortingRecord> SortingDatabase::queryByTime(const QDateTime& from,
                                                     const QDateTime& to,
                                                     int limit)
{
    QMutexLocker locker(&m_mutex);
    QVector<SortingRecord> result;

    if (!m_bOpened) return result;

    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return result;

    QSqlQuery q(db);
    // SELECT id, order_code, barcode, grid_num, car_num, grid_count, volu, sort_time, create_time
    // FROM sorting_records WHERE sort_time >= ? AND sort_time <= ? ORDER BY id DESC LIMIT ?
    q.prepare(SQL_QUERY_BY_TIME);
    q.addBindValue(from.toString("yyyy-MM-dd HH:mm:ss"));
    q.addBindValue(to.toString("yyyy-MM-dd HH:mm:ss.zzz"));
    q.addBindValue(limit);

    if (!q.exec()) return result;

    while (q.next())
    {
        SortingRecord rec;
        rec.id         = q.value(0).toInt();
        rec.orderCode  = q.value(1).toString();
        rec.barcode    = q.value(2).toString();
        rec.gridNum    = q.value(3).toString();
        rec.carNum     = q.value(4).toString();
        rec.gridCount  = q.value(5).toInt();
        rec.volu       = q.value(6).toString();
        rec.sortTime   = q.value(7).toString();
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
    // SELECT id, order_code, barcode, grid_num, car_num, grid_count, volu, sort_time, create_time
    // FROM sorting_records WHERE order_code = ? ORDER BY id DESC LIMIT ?
    q.prepare(SQL_QUERY_BY_ORDER);
    q.addBindValue(orderCode);
    q.addBindValue(limit);

    if (!q.exec()) return result;

    while (q.next())
    {
        SortingRecord rec;
        rec.id         = q.value(0).toInt();
        rec.orderCode  = q.value(1).toString();
        rec.barcode    = q.value(2).toString();
        rec.gridNum    = q.value(3).toString();
        rec.carNum     = q.value(4).toString();
        rec.gridCount  = q.value(5).toInt();
        rec.volu       = q.value(6).toString();
        rec.sortTime   = q.value(7).toString();
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
    // SELECT id, order_code, barcode, grid_num, car_num, grid_count, volu, sort_time, create_time
    // FROM sorting_records ORDER BY id DESC LIMIT ?
    q.prepare(SQL_QUERY_ALL);
    q.addBindValue(limit);

    if (!q.exec()) return result;

    while (q.next())
    {
        SortingRecord rec;
        rec.id         = q.value(0).toInt();
        rec.orderCode  = q.value(1).toString();
        rec.barcode    = q.value(2).toString();
        rec.gridNum    = q.value(3).toString();
        rec.carNum     = q.value(4).toString();
        rec.gridCount  = q.value(5).toInt();
        rec.volu       = q.value(6).toString();
        rec.sortTime   = q.value(7).toString();
        rec.createTime = q.value(8).toString();
        result.append(rec);
    }

    return result;
}

// ============================================================================
// 统计
// ============================================================================

SortingStatistics SortingDatabase::statistics()
{
    QMutexLocker locker(&m_mutex);
    SortingStatistics stats;

    if (!m_bOpened) return stats;

    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return stats;

    QSqlQuery q(db);

    // SELECT COUNT(*) FROM sorting_records
    q.exec(SQL_COUNT_ALL);
    if (q.next()) stats.totalRecords = q.value(0).toInt();

    // SELECT COUNT(*) FROM sorting_records WHERE sort_time >= ?
    QString today = QDate::currentDate().toString("yyyy-MM-dd");
    q.prepare(SQL_COUNT_TODAY);
    q.addBindValue(today + " 00:00:00");
    if (q.exec() && q.next()) stats.todayRecords = q.value(0).toInt();

    // SELECT COUNT(DISTINCT order_code) FROM sorting_records
    q.exec(SQL_COUNT_WAVES);
    if (q.next()) stats.totalWaves = q.value(0).toInt();

    // SELECT COUNT(DISTINCT grid_num) FROM sorting_records
    q.exec(SQL_COUNT_GRIDS);
    if (q.next()) stats.totalGrids = q.value(0).toInt();

    // SELECT sort_time FROM sorting_records ORDER BY id DESC LIMIT 1
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
    // SELECT COUNT(*) FROM sorting_records
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
    // SELECT COUNT(*) FROM sorting_records WHERE sort_time >= ?
    q.prepare(SQL_COUNT_TODAY);
    q.addBindValue(today + " 00:00:00");
    if (q.exec() && q.next()) return q.value(0).toInt();
    return 0;
}

// ============================================================================
// 维护
// ============================================================================

void SortingDatabase::cleanupOldRecords(int retainDays)
{
    QMutexLocker locker(&m_mutex);

    if (!m_bOpened) return;

    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return;

    QDateTime cutoff = QDateTime::currentDateTime().addDays(-retainDays);
    QString cutoffStr = cutoff.toString("yyyy-MM-dd HH:mm:ss");

    QSqlQuery q(db);
    // DELETE FROM sorting_records WHERE sort_time < ?
    q.prepare(SQL_DELETE_OLD);
    q.addBindValue(cutoffStr);

    if (!q.exec())
    {
        qWarning() << "[SortingDB] 清理旧记录失败:" << q.lastError().text();
    }
    else
    {
        int deleted = q.numRowsAffected();
        if (deleted > 0)
        {
            qDebug() << "[SortingDB] 已清理" << deleted << "条旧记录（"
                     << retainDays << "天前）";
        }
        // PRAGMA optimize（清理后优化数据库）
        q.exec(SQL_PRAGMA_OPTIMIZE);
    }
}

// ============================================================================
// 辅助
// ============================================================================

QString SortingDatabase::currentTimeStr() const
{
    return QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
}