#include "SortingDatabase.h"
#include "LogService.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QSqlQuery>
#include <QSqlError>
#include <QSqlRecord>
#include <QDebug>
#include "define.h"

// 数据库操作专用日志宏（写入 ./log/DataBase/DataBase.log）
// （宏定义已移至 LogService.h 统一管理）

// ============================================================================
// UI 只读查询实现（2026-09-04 P0修复）
// 与原 runOnDbThread 逻辑一致，仅将连接改为调用方传入的只读连接（WAL 并行读），
// 供 statistics/queryByBarcode/queryAllWithPending/cleanupOldRecords 复用，
// 避免万级落库占用 DB 线程时阻塞 UI 查询。
// ============================================================================
namespace {

QVector<SortingRecord> queryByBarcodeImpl(const QSqlDatabase& db, const QString& barcode, int limit)
{
    QVector<SortingRecord> result;
    if (!db.isValid() || !db.isOpen()) return result;

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
            rec.sku = q.value(3).toString();           // ★ SKU编码
            rec.gridNum = q.value(4).toString();
            rec.carNum = q.value(5).toString();
            rec.firstCar = q.value(6).toString();      // ★ 首车号
            rec.lastCar  = q.value(7).toString();      // ★ 尾车号
            rec.gridCount = q.value(8).toInt();
            rec.volu = q.value(9).toString();
            rec.sortTime = q.value(10).toString();
            rec.createTime = q.value(11).toString();
            rec.boxcode    = q.value(12).toString();   // ★ 2026-09-09 需求6：容器号
            rec.status = QString::fromUtf8("已分拣");  // 来自 sorting_records 表，PLC 已落格
            result.append(rec);
        }
    }

    // ② 查询波次计划明细（return_wave_item 表，波次下发时写入）
    //    使用 NOT EXISTS 排除已在 sorting_records 中落格的同波次同EPC编码，
    //    保证同一 (order_code, inco) 不会同时出现「已分拣」和「待分拣」两种状态
    {
        QSqlQuery q2(db);
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

    Data_INFO("[SortingDB] queryByBarcode barcode=%s limit=%d resultCount=%d",
        barcode.toLocal8Bit().data(), limit, result.size());
    for (const auto& rec : result)
    {
        Data_INFO("[SortingDB] queryByBarcode 记录 id=%d epc=%s grid=%s carNum=%s firstCar=%s lastCar=%s orderCode=%s status=%s",
            rec.id, rec.barcode.toLocal8Bit().data(), rec.gridNum.toLocal8Bit().data(),
            rec.carNum.toLocal8Bit().data(),
            rec.firstCar.isEmpty() ? "(空)" : rec.firstCar.toLocal8Bit().data(),
            rec.lastCar.isEmpty()  ? "(空)" : rec.lastCar.toLocal8Bit().data(),
            rec.orderCode.toLocal8Bit().data(), rec.status.toLocal8Bit().data());
    }
    return result;
}

QVector<SortingRecord> queryAllWithPendingImpl(const QSqlDatabase& db, int limit)
{
    QVector<SortingRecord> result;
    if (!db.isValid() || !db.isOpen()) return result;

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
                rec.sku = q.value(3).toString();           // ★ SKU编码
                rec.gridNum = q.value(4).toString();
                rec.carNum = q.value(5).toString();
                rec.firstCar = q.value(6).toString();      // ★ 首车号
                rec.lastCar  = q.value(7).toString();      // ★ 尾车号
                rec.gridCount = q.value(8).toInt();
                rec.volu = q.value(9).toString();
                rec.sortTime = q.value(10).toString();
                rec.createTime = q.value(11).toString();
            rec.boxcode    = q.value(12).toString();   // ★ 2026-09-09 需求6：容器号
                rec.status = QString::fromUtf8("已分拣");
                result.append(rec);
            }
        }
    }

    // ② 待分拣：return_wave_item 中排除已落格（NOT EXISTS）
    {
        QSqlQuery q(db);
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

    Data_INFO("[SortingDB] queryAllWithPending limit=%d resultCount=%d", limit, result.size());
    return result;
}

SortingStatistics statisticsImpl(const QSqlDatabase& db)
{
    SortingStatistics stats;
    if (!db.isValid() || !db.isOpen()) return stats;

    QSqlQuery q(db);
    if (q.exec(SQL_COUNT_ALL) && q.next())
        stats.totalRecords = q.value(0).toInt();
    else
        Data_WARN("[SortingDB] statistics SQL_COUNT_ALL 失败 err=%s", q.lastError().text().toLocal8Bit().data());

    QString today = QDate::currentDate().toString("yyyy-MM-dd");
    q.prepare(SQL_COUNT_TODAY);
    q.addBindValue(today + " 00:00:00");
    if (q.exec() && q.next())
        stats.todayRecords = q.value(0).toInt();
    else
        Data_WARN("[SortingDB] statistics SQL_COUNT_TODAY 失败 err=%s", q.lastError().text().toLocal8Bit().data());

    if (q.exec(SQL_COUNT_WAVES) && q.next())
        stats.totalWaves = q.value(0).toInt();
    else
        Data_WARN("[SortingDB] statistics SQL_COUNT_WAVES 失败 err=%s", q.lastError().text().toLocal8Bit().data());

    if (q.exec(SQL_COUNT_GRIDS) && q.next())
        stats.totalGrids = q.value(0).toInt();
    else
        Data_WARN("[SortingDB] statistics SQL_COUNT_GRIDS 失败 err=%s", q.lastError().text().toLocal8Bit().data());

    if (q.exec(SQL_LAST_SORT_TIME) && q.next())
        stats.lastSortTime = q.value(0).toString();

    Data_INFO("[SortingDB] statistics 结果: total=%d today=%d waves=%d grids=%d lastSort=%s",
        stats.totalRecords, stats.todayRecords, stats.totalWaves, stats.totalGrids,
        stats.lastSortTime.toLocal8Bit().data());
    return stats;
}

void cleanupOldRecordsImpl(const QSqlDatabase& db, int retainDays)
{
    if (!db.isValid() || !db.isOpen()) return;
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

} // namespace

// ============================================================================
// 构造 / 析构
// ============================================================================

// ============================================================================
// 单例
// ============================================================================
SortingDatabase& SortingDatabase::instance()
{
    static SortingDatabase s_instance;
    return s_instance;
}

SortingDatabase::SortingDatabase()
{
    // 单例构造，不初始化 DB 线程（open() 时创建）
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
    if (m_bOpened) return true;  // 幂等

    if (dbPath.isEmpty())
        m_dbPath = QCoreApplication::applicationDirPath() + "/" SORTING_DB_FILE;
    else
        m_dbPath = dbPath;

    QDir dir = QFileInfo(m_dbPath).absoluteDir();
    if (!dir.exists()) dir.mkpath(".");

    // ★ 创建专用 DB 线程（单连接，单缓存，零冗余）
    m_pDbThread = new QThread();
    m_pDbTarget = new QObject();
    m_pDbTarget->moveToThread(m_pDbThread);
    m_pDbThread->start();
    Data_INFO("[SortingDB] open DB线程已创建 dbTid=%llu",
        (unsigned long long)(quintptr)m_pDbThread->currentThreadId());

    // 在 DB 线程上打开数据库
    return runOnDbThread([&]() -> bool {
        Data_INFO("[SortingDB] open 开始打开数据库 path=%s", m_dbPath.toLocal8Bit().data());
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "SortingDB");
        db.setDatabaseName(m_dbPath);
        if (!db.open()) {
            Data_ERROR("[SortingDB] 数据库打开失败 path=%s errCode=%s errText=%s",
                m_dbPath.toLocal8Bit().data(),
                db.lastError().nativeErrorCode().toLocal8Bit().data(),
                db.lastError().text().toLocal8Bit().data());
            // ★ 2026-09-06 诊断增强：Driver not loaded 时输出驱动/插件环境，直接定位原因
            //   （常见：缺 sqldrivers/qsqlite.dll 或 Qt5Sql.dll、插件位数/版本不匹配、qt.conf 指向错误）
            Data_ERROR("[SortingDB] [诊断] QSQLITE驱动可用=%d 全部驱动=[%s]",
                QSqlDatabase::isDriverAvailable("QSQLITE") ? 1 : 0,
                QSqlDatabase::drivers().join(",").toLocal8Bit().constData());
            QStringList libPaths = QCoreApplication::libraryPaths();
            Data_ERROR("[SortingDB] [诊断] Qt插件搜索路径=[%s]",
                libPaths.join(";").toLocal8Bit().constData());
            QString exeDir = QCoreApplication::applicationDirPath();
            QDir sqDir(exeDir + "/sqldrivers");
            Data_ERROR("[SortingDB] [诊断] exe目录=%s sqldrivers目录存在=%d 内容=[%s]",
                exeDir.toLocal8Bit().constData(), sqDir.exists() ? 1 : 0,
                sqDir.exists() ? sqDir.entryList(QDir::Files).join(",").toLocal8Bit().constData() : "(无)");
            return false;
        }

        // WAL 模式优化
        {
            QSqlQuery q(db);
            q.exec(SQL_PRAGMA_WAL);
            q.exec(SQL_PRAGMA_SYNC);
            q.exec(SQL_PRAGMA_CACHE);
            Data_INFO("[SortingDB] open WAL模式已启用 cache_size=5000(20MB)");
        }

        createTables();
        m_bOpened = 1;
        Data_INFO("[SortingDB] 数据库已打开 path=%s connectionName=SortingDB isValid=%d",
            m_dbPath.toLocal8Bit().data(), db.isValid() ? 1 : 0);
        return true;
    });
}

void SortingDatabase::close()
{
    if (!m_pDbThread) return;
    Data_INFO("[SortingDB] close 开始关闭数据库 dbThread=%p isRunning=%d",
        (void*)m_pDbThread, m_pDbThread->isRunning() ? 1 : 0);

    // 在 DB 线程上关闭数据库
    if (m_pDbThread->isRunning()) {
        runOnDbThread([&]() {
            Data_INFO("[SortingDB] close 执行中 关闭连接...");
            m_bOpened = 0;
            {
                QSqlDatabase db = QSqlDatabase::database("SortingDB");
                if (db.isOpen()) db.close();
            }
            QSqlDatabase::removeDatabase("SortingDB");
            Data_INFO("[SortingDB] close 连接已移除");
        });

        m_pDbThread->quit();
        bool bWaitOk = m_pDbThread->wait(3000);
        Data_INFO("[SortingDB] close DB线程已退出 waitResult=%d", bWaitOk ? 1 : 0);
    }

    delete m_pDbTarget;
    m_pDbTarget = nullptr;
    delete m_pDbThread;
    m_pDbThread = nullptr;
    Data_INFO("[SortingDB] 数据库已完全关闭");
}

bool SortingDatabase::isOpen() const
{
    return m_bOpened != 0;
}

// ★ 获取当前线程的只读查询连接（懒创建，每线程独立）
//   WAL 模式下只读连接与 DB 线程的写连接并行工作：
//   落库任务（如万级波次明细 insertWaveItems）占用 DB 线程时，UI 查询走本连接并行读，不阻塞
QSqlDatabase SortingDatabase::queryDb() const
{
    if (!m_queryConns.hasLocalData())
    {
        QString connName = QString("SortingDB_Query_%1").arg((quintptr)QThread::currentThreadId());
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(m_dbPath);
        if (!db.open())
        {
            Data_ERROR("[SortingDB] 只读连接打开失败 conn=%s errText=%s",
                connName.toLocal8Bit().data(),
                db.lastError().text().toLocal8Bit().data());
        }
        else
        {
            db.exec(SQL_PRAGMA_BUSY_TIMEOUT);   // 等待写锁上限（WAL 下写不阻塞读，此处仅防极端竞争）
            Data_INFO("[SortingDB] 只读连接已创建 conn=%s tid=%llu",
                connName.toLocal8Bit().data(),
                (unsigned long long)(quintptr)QThread::currentThreadId());
        }
        m_queryConns.setLocalData(db);
    }
    return m_queryConns.localData();
}

// ============================================================================
// 建表
// ============================================================================

void SortingDatabase::createTables()
{
    QSqlDatabase db = QSqlDatabase::database("SortingDB");
    if (!db.isOpen())
    {
        Data_ERROR("[SortingDB] createTables 数据库连接不可用 errCode=%s errText=%s",
            db.lastError().nativeErrorCode().toLocal8Bit().data(),
            db.lastError().text().toLocal8Bit().data());
        return;
    }
    QSqlQuery q(db);

    Data_INFO("[SortingDB] createTables 开始建表...");
    // ──── 原有表 ────
    q.exec(SQL_CREATE_TABLE_SORTING);
    q.exec(SQL_CREATE_INDEX_BARCODE);
    q.exec(SQL_CREATE_INDEX_ORDER);
    q.exec(SQL_CREATE_INDEX_TIME);

    // ★ 迁移：为旧版 sorting_records 表添加 first_car/last_car/sku 列（V1→V2 兼容）
    //   SQLite 不支持 ALTER TABLE ADD COLUMN IF NOT EXISTS，忽略重复列错误
    q.exec(SQL_ALTER_ADD_FIRST_CAR);
    q.exec(SQL_ALTER_ADD_LAST_CAR);
    q.exec(SQL_ALTER_ADD_SKU);
    // ★ 2026-09-09 需求6：旧库加 boxcode 列（行进中换容器的记录容器号），重复列错误忽略
    q.exec(SQL_ALTER_SORTING_ADD_BOXCODE);

    // ──── S0 新增表 ────
    // 退货波次头
    q.exec(SQL_CREATE_TABLE_RETURN_WAVE);
    // ★ 2026-09-06 保险：return_wave 旧库缺列迁移
    q.exec(SQL_ALTER_ADD_RW_ORDER_QTY);
    q.exec(SQL_ALTER_ADD_RW_STATUS);
    q.exec(SQL_ALTER_ADD_RW_CREATED_AT);
    q.exec(SQL_ALTER_ADD_RW_UPDATED_AT);
    // 退货波次明细
    q.exec(SQL_CREATE_TABLE_RETURN_WAVE_ITEM);
    q.exec(SQL_CREATE_INDEX_WAVE_ITEM_ORDER);
    // ★ 2026-09-06 修复：return_wave_item 旧库缺列迁移（老库无这些列时 INSERT 报 no such column 导致
    //   波次明细落库稳定失败）。SQLite 不支持 IF NOT EXISTS，重复执行报 duplicate column，忽略即可。
    q.exec(SQL_ALTER_ADD_WI_GRID_TYPE);
    q.exec(SQL_ALTER_ADD_WI_PLAN_QTY);
    q.exec(SQL_ALTER_ADD_WI_SORTED_QTY);
    q.exec(SQL_ALTER_ADD_WI_VOLU);
    q.exec(SQL_ALTER_ADD_WI_OBX_CODE);
    // 格口容器绑定
    q.exec(SQL_CREATE_TABLE_GRID_BOX_BIND);
    q.exec(SQL_CREATE_INDEX_BIND_GRID_ACTIVE);
    // 分拣流水
    q.exec(SQL_CREATE_TABLE_SORT_TXN);
    q.exec(SQL_CREATE_INDEX_SORT_TXN_ORDER);
    q.exec(SQL_CREATE_INDEX_SORT_TXN_EPC);
    // 满箱回传出站（H7）
    q.exec(SQL_CREATE_TABLE_OUTBOX_FULLBOX);
    // ★ 2026-09-08 保险：outbox_fullbox 旧库缺 grid 列迁移（失败格口下拉读取用；重复列错误忽略）
    q.exec(SQL_ALTER_ADD_OB_GRID);
    q.exec(SQL_CREATE_INDEX_OUTBOX_ORDER);
    q.exec(SQL_CREATE_INDEX_OUTBOX_RETRY);
    // 完结回传出站（H8）
    q.exec(SQL_CREATE_TABLE_OUTBOX_END);
    // 异常记录
    q.exec(SQL_CREATE_TABLE_EXCEPTION_RECORD);
    // H4 原始报文单独落库（数据量大，独立表）
    q.exec(SQL_CREATE_TABLE_WAVE_RAW);
    // 每日峰值效率表（2026-09-07 波次面板峰值效率）
    q.exec(SQL_CREATE_TABLE_DAILY_PEAK);

    if (q.lastError().isValid())
        qWarning() << "[SortingDB] 建表失败:" << q.lastError().text();
    else
        Data_INFO("[SortingDB] createTables 建表完成（7张核心表 + wave_raw + daily_peak）");
}

// ============================================================================
// 原有接口（保留兼容）
// ============================================================================

bool SortingDatabase::insertRecord(const QString& orderCode, const QString& barcode,
                                    const QString& sku,
                                    const QString& gridNum, const QString& carNum,
                                    const QString& firstCar, const QString& lastCar,
                                    int gridCount, const QString& volu,
                                    const QString& boxcode)
{
    return runOnDbThread([&]() -> bool {
        if (!m_bOpened) return false;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
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
        q.addBindValue(sku.isEmpty() ? "" : sku);                     // ★ SKU编码
        q.addBindValue(gridNum);
        q.addBindValue(carNum);
        q.addBindValue(firstCar.isEmpty() ? "" : firstCar);           // ★ 首车号
        q.addBindValue(lastCar.isEmpty()  ? "" : lastCar);            // ★ 尾车号
        q.addBindValue(gridCount);
        q.addBindValue(volu.isEmpty() ? "--" : volu);
        q.addBindValue(now);
        q.addBindValue(now);
        q.addBindValue(boxcode);   // ★ 2026-09-09 需求6：落格容器号

        Data_INFO("[SortingDB] insertRecord barcode=%s sku=%s grid=%s carNum=%s firstCar=%s lastCar=%s gridCount=%d sobi=%s box=%s orderCode=%s",
            barcode.toLocal8Bit().data(), sku.toLocal8Bit().data(),
            gridNum.toLocal8Bit().data(),
            carNum.toLocal8Bit().data(),
            firstCar.isEmpty() ? "(空)" : firstCar.toLocal8Bit().data(),
            lastCar.isEmpty()  ? "(空)" : lastCar.toLocal8Bit().data(),
            gridCount, volu.toLocal8Bit().data(), boxcode.toLocal8Bit().data(),
            orderCode.toLocal8Bit().data());
        if (!q.exec()) {
            Data_ERROR("[SortingDB] 插入失败: %s barcode=%s",
                q.lastError().text().toLocal8Bit().data(), barcode.toLocal8Bit().data());
            return false;
        }
        Data_INFO("[SortingDB] insertRecord 成功 barcode=%s firstCar=%s lastCar=%s",
            barcode.toLocal8Bit().data(),
            firstCar.isEmpty() ? "(空)" : firstCar.toLocal8Bit().data(),
            lastCar.isEmpty()  ? "(空)" : lastCar.toLocal8Bit().data());
        return true;
    });
}

QVector<SortingRecord> SortingDatabase::queryByBarcode(const QString& barcode, int limit)
{
    // ★ UI 查询走只读连接（WAL 并行读）：万级落库占用 DB 线程时不阻塞调用线程
    QSqlDatabase readDb = queryDb();
    if (readDb.isValid() && readDb.isOpen())
        return queryByBarcodeImpl(readDb, barcode, limit);
    // 降级：只读连接不可用（DB 未初始化等）→ 走 DB 线程原路径
    return runOnDbThread([&]() -> QVector<SortingRecord> {
        return queryByBarcodeImpl(QSqlDatabase::database("SortingDB"), barcode, limit);
    });
}

QVector<SortingRecord> SortingDatabase::queryByTime(const QDateTime& from, const QDateTime& to, int limit)
{
    return runOnDbThread([&]() -> QVector<SortingRecord> {
        QVector<SortingRecord> result;
        if (!m_bOpened) return result;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
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
            rec.sku = q.value(3).toString();           // ★ SKU编码
            rec.gridNum = q.value(4).toString();
            rec.carNum = q.value(5).toString();
            rec.firstCar = q.value(6).toString();      // ★ 首车号
            rec.lastCar  = q.value(7).toString();      // ★ 尾车号
            rec.gridCount = q.value(8).toInt();
            rec.volu = q.value(9).toString();
            rec.sortTime = q.value(10).toString();
            rec.createTime = q.value(11).toString();
            rec.boxcode    = q.value(12).toString();   // ★ 2026-09-09 需求6：容器号
            result.append(rec);
        }
        Data_INFO("[SortingDB] queryByTime from=%s to=%s limit=%d resultCount=%d",
            from.toString("yyyy-MM-dd hh:mm:ss").toLocal8Bit().data(),
            to.toString("yyyy-MM-dd hh:mm:ss").toLocal8Bit().data(), limit, result.size());
        return result;
    });
}

QVector<SortingRecord> SortingDatabase::queryByOrderCode(const QString& orderCode, int limit)
{
    return runOnDbThread([&]() -> QVector<SortingRecord> {
        QVector<SortingRecord> result;
        if (!m_bOpened) return result;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
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
            rec.sku = q.value(3).toString();           // ★ SKU编码
            rec.gridNum = q.value(4).toString();
            rec.carNum = q.value(5).toString();
            rec.firstCar = q.value(6).toString();      // ★ 首车号
            rec.lastCar  = q.value(7).toString();      // ★ 尾车号
            rec.gridCount = q.value(8).toInt();
            rec.volu = q.value(9).toString();
            rec.sortTime = q.value(10).toString();
            rec.createTime = q.value(11).toString();
            rec.boxcode    = q.value(12).toString();   // ★ 2026-09-09 需求6：容器号
            result.append(rec);
        }
        Data_INFO("[SortingDB] queryByOrderCode orderCode=%s limit=%d resultCount=%d",
            orderCode.toLocal8Bit().data(), limit, result.size());
        return result;
    });
}

QVector<SortingRecord> SortingDatabase::queryAll(int limit)
{
    return runOnDbThread([&]() -> QVector<SortingRecord> {
        QVector<SortingRecord> result;
        if (!m_bOpened) return result;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
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
            rec.sku = q.value(3).toString();           // ★ SKU编码
            rec.gridNum = q.value(4).toString();
            rec.carNum = q.value(5).toString();
            rec.firstCar = q.value(6).toString();      // ★ 首车号
            rec.lastCar  = q.value(7).toString();      // ★ 尾车号
            rec.gridCount = q.value(8).toInt();
            rec.volu = q.value(9).toString();
            rec.sortTime = q.value(10).toString();
            rec.createTime = q.value(11).toString();
            rec.boxcode    = q.value(12).toString();   // ★ 2026-09-09 需求6：容器号
            result.append(rec);
        }
        Data_INFO("[SortingDB] queryAll limit=%d resultCount=%d", limit, result.size());
        return result;
    });
}

// ═════════════════════════════════════════════════════════════════════════════
// ★ 2026-09-09 需求2：按格口查询分拣明细 / 全格口分拣数量汇总
// ═════════════════════════════════════════════════════════════════════════════
QVector<SortingRecord> SortingDatabase::queryByGrid(const QString& gridNum, int limit)
{
    return runOnDbThread([&]() -> QVector<SortingRecord> {
        QVector<SortingRecord> result;
        if (!m_bOpened || gridNum.isEmpty()) return result;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen()) return result;

        QSqlQuery q(db);
        q.prepare(SQL_QUERY_BY_GRID);
        q.addBindValue(gridNum);
        q.addBindValue(limit);
        if (!q.exec()) return result;
        while (q.next()) {
            SortingRecord rec;
            rec.id = q.value(0).toInt();
            rec.orderCode = q.value(1).toString();
            rec.barcode = q.value(2).toString();
            rec.sku = q.value(3).toString();
            rec.gridNum = q.value(4).toString();
            rec.carNum = q.value(5).toString();
            rec.firstCar = q.value(6).toString();
            rec.lastCar  = q.value(7).toString();
            rec.gridCount = q.value(8).toInt();
            rec.volu = q.value(9).toString();
            rec.sortTime = q.value(10).toString();
            rec.createTime = q.value(11).toString();
            rec.boxcode    = q.value(12).toString();
            result.append(rec);
        }
        Data_INFO("[SortingDB] queryByGrid grid=%s limit=%d resultCount=%d",
            gridNum.toLocal8Bit().data(), limit, result.size());
        return result;
    });
}

QVector<GridSummaryRecord> SortingDatabase::queryGridSummary()
{
    return runOnDbThread([&]() -> QVector<GridSummaryRecord> {
        QVector<GridSummaryRecord> result;
        if (!m_bOpened) return result;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen()) return result;

        QSqlQuery q(db);
        q.prepare(SQL_QUERY_GRID_SUMMARY);
        if (!q.exec()) return result;
        while (q.next()) {
            GridSummaryRecord g;
            g.gridNum      = q.value(0).toString();
            g.sortedCount  = q.value(1).toInt();
            g.skuCount     = q.value(2).toInt();
            g.boxcode      = q.value(3).toString();
            g.lastSortTime = q.value(4).toString();
            result.append(g);
        }
        Data_INFO("[SortingDB] queryGridSummary gridCount=%d", result.size());
        return result;
    });
}

// ═════════════════════════════════════════════════════════════════════════════
// ★ queryAllWithPending — 留空查全部：已分拣 + 待分拣（UI 查询面板）
// 返回所有已分拣记录（sorting_records）和所有待分拣记录（return_wave_item，
// 用 NOT EXISTS 排除已落格的EPC编码），合并后按时间倒序（已分拣在前）
// ═════════════════════════════════════════════════════════════════════════════
QVector<SortingRecord> SortingDatabase::queryAllWithPending(int limit)
{
    // ★ UI 查询走只读连接（WAL 并行读）：万级落库占用 DB 线程时不阻塞调用线程
    QSqlDatabase readDb = queryDb();
    if (readDb.isValid() && readDb.isOpen())
        return queryAllWithPendingImpl(readDb, limit);
    // 降级：只读连接不可用（DB 未初始化等）→ 走 DB 线程原路径
    return runOnDbThread([&]() -> QVector<SortingRecord> {
        return queryAllWithPendingImpl(QSqlDatabase::database("SortingDB"), limit);
    });
}

SortingStatistics SortingDatabase::statistics()
{
    // ★ UI 查询走只读连接（WAL 并行读）：万级落库占用 DB 线程时不阻塞调用线程
    QSqlDatabase readDb = queryDb();
    if (readDb.isValid() && readDb.isOpen())
        return statisticsImpl(readDb);
    // 降级：只读连接不可用（DB 未初始化等）→ 走 DB 线程原路径
    return runOnDbThread([&]() -> SortingStatistics {
        return statisticsImpl(QSqlDatabase::database("SortingDB"));
    });
}

int SortingDatabase::recordCount()
{
    return runOnDbThread([&]() -> int {
        if (!m_bOpened) return 0;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen()) return 0;
        QSqlQuery q(db);
        q.exec(SQL_COUNT_ALL);
        if (q.next()) return q.value(0).toInt();
        return 0;
    });
}

int SortingDatabase::todayRecordCount()
{
    return runOnDbThread([&]() -> int {
        if (!m_bOpened) return 0;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen()) return 0;
        QString today = QDate::currentDate().toString("yyyy-MM-dd");
        QSqlQuery q(db);
        q.prepare(SQL_COUNT_TODAY);
        q.addBindValue(today + " 00:00:00");
        if (q.exec() && q.next()) return q.value(0).toInt();
        return 0;
    });
}

// ★ 按 SKU 查询格口分配（查询 return_wave_item 表）
QVector<ReturnWaveItemRecord> SortingDatabase::querySkuGridMapping(const QString& sku, const QString& orderCode)
{
    return runOnDbThread([&]() -> QVector<ReturnWaveItemRecord> {
        QVector<ReturnWaveItemRecord> result;
        if (!m_bOpened) return result;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen()) return result;

        QSqlQuery q(db);
        if (orderCode.isEmpty())
        {
            // 不指定波次：查询所有波次中该 SKU 的格口分配
            q.prepare(SQL_QUERY_SKU_GRID_MAPPING);
            q.addBindValue(sku);
        }
        else
        {
            // 指定波次：只查询该波次中的分配
            q.prepare(SQL_QUERY_SKU_GRID_MAPPING_BY_ORDER);
            q.addBindValue(sku);
            q.addBindValue(orderCode);
        }

        if (q.exec())
        {
            while (q.next())
            {
                ReturnWaveItemRecord rec;
                rec.id        = q.value(0).toInt();
                rec.orderCode = q.value(1).toString();
                rec.inco      = q.value(2).toString();
                rec.gridNum   = q.value(3).toString();
                rec.gridType  = q.value(4).toString();
                rec.planQty   = q.value(5).toInt();
                rec.sortedQty = q.value(6).toInt();
                rec.volu      = q.value(7).toString();
                rec.obxCode   = q.value(8).toString();
                result.append(rec);
            }
        }

        Data_INFO("[SortingDB] querySkuGridMapping sku=%s orderCode=%s resultCount=%d",
            sku.toLocal8Bit().data(),
            orderCode.isEmpty() ? "(全部波次)" : orderCode.toLocal8Bit().data(),
            result.size());
        return result;
    });
}

void SortingDatabase::cleanupOldRecords(int retainDays)
{
    // ★ UI 维护操作走只读连接（WAL 并行读）：万级落库占用 DB 线程时不阻塞调用线程
    QSqlDatabase readDb = queryDb();
    if (readDb.isValid() && readDb.isOpen())
    {
        cleanupOldRecordsImpl(readDb, retainDays);
        return;
    }
    // 降级：只读连接不可用（DB 未初始化等）→ 走 DB 线程原路径
    runOnDbThread([&]() {
        cleanupOldRecordsImpl(QSqlDatabase::database("SortingDB"), retainDays);
    });
}

// ============================================================================
// S0 新增：波次管理
// ============================================================================

bool SortingDatabase::upsertReturnWave(const QString& orderCode, int orderQty, int status)
{
    return runOnDbThread([&]() -> bool {
        if (!m_bOpened) return false;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
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
    });
}

ReturnWaveRecord SortingDatabase::getReturnWave(const QString& orderCode)
{
    return runOnDbThread([&]() -> ReturnWaveRecord {
        ReturnWaveRecord rec;
        if (!m_bOpened) return rec;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
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
    });
}

bool SortingDatabase::updateWaveStatus(const QString& orderCode, int newStatus)
{
    return runOnDbThread([&]() -> bool {
        if (!m_bOpened) return false;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen()) return false;

        QSqlQuery q(db);
        // UPDATE return_wave SET status = ?, updated_at = ? WHERE order_code = ?
        q.prepare(SQL_UPDATE_WAVE_STATUS);
        q.addBindValue(newStatus);
        q.addBindValue(currentTimeStr());
        q.addBindValue(orderCode);
        return q.exec();
    });
}

int SortingDatabase::getWaveStatus(const QString& orderCode)
{
    return runOnDbThread([&]() -> int {
        if (!m_bOpened) return -1;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen()) return -1;

        QSqlQuery q(db);
        q.prepare(SQL_SELECT_WAVE_BY_ORDER);
        q.addBindValue(orderCode);
        if (q.exec() && q.next())
            return q.value(2).toInt();
        return -1;  // 不存在
    });
}

// ★ 查询最近一条未完成波次（排除已取消=6和已完成=8），用于软件重启后恢复波次数据
ReturnWaveRecord SortingDatabase::getLatestUnfinishedWave()
{
    return runOnDbThread([&]() -> ReturnWaveRecord {
        ReturnWaveRecord rec;
        if (!m_bOpened) return rec;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
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
    });
}

// ★ 2026-09-06 查询全部已传输波次（含已完成/已取消）+ 进度计数（UI「波次数据记录」）
QVector<WaveRecordProgress> SortingDatabase::getAllWaves()
{
    return runOnDbThread([&]() -> QVector<WaveRecordProgress> {
        QVector<WaveRecordProgress> result;
        if (!m_bOpened) return result;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen()) return result;

        QSqlQuery q(db);
        q.prepare(SQL_SELECT_ALL_WAVES);
        if (!q.exec()) return result;
        while (q.next()) {
            WaveRecordProgress rec;
            rec.orderCode      = q.value(0).toString();
            rec.orderQty       = q.value(1).toInt();
            rec.status         = q.value(2).toInt();
            rec.createdAt      = q.value(3).toString();
            rec.updatedAt      = q.value(4).toString();
            rec.sortedCount    = q.value(5).toInt();
            rec.exceptionCount = q.value(6).toInt();
            result.append(rec);
        }
        return result;
    });
}

QVector<ReturnWaveRecord> SortingDatabase::getAllUnfinishedWaves()
{
    return runOnDbThread([&]() -> QVector<ReturnWaveRecord> {
        QVector<ReturnWaveRecord> result;
        if (!m_bOpened) return result;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen()) return result;

        QSqlQuery q(db);
        q.prepare(SQL_SELECT_ALL_UNFINISHED_WAVES);
        if (!q.exec()) return result;
        while (q.next()) {
            ReturnWaveRecord rec;
            rec.orderCode = q.value(0).toString();
            rec.orderQty = q.value(1).toInt();
            rec.status = q.value(2).toInt();
            rec.createdAt = q.value(3).toString();
            rec.updatedAt = q.value(4).toString();
            result.append(rec);
        }
        return result;
    });
}

// ============================================================================
// 波次恢复查询（上一波次任务恢复用）
// ============================================================================

QSet<QString> SortingDatabase::getSortedEpcsByOrder(const QString& orderCode)
{
    return runOnDbThread([&]() -> QSet<QString> {
        QSet<QString> result;
        if (!m_bOpened) return result;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen()) return result;

        QSqlQuery q(db);
        q.prepare(SQL_SELECT_SORTED_EPCS_BY_ORDER);
        q.addBindValue(orderCode);
        if (!q.exec()) return result;
        while (q.next())
            result.insert(q.value(0).toString());
        return result;
    });
}

QSet<QString> SortingDatabase::getExceptionEpcsByOrder(const QString& orderCode)
{
    return runOnDbThread([&]() -> QSet<QString> {
        QSet<QString> result;
        if (!m_bOpened) return result;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen()) return result;

        QSqlQuery q(db);
        q.prepare(SQL_SELECT_EXCEPTION_EPCS_BY_ORDER);
        q.addBindValue(orderCode);
        if (!q.exec()) return result;
        while (q.next())
            result.insert(q.value(0).toString());
        return result;
    });
}

bool SortingDatabase::hasSuccessFullbox(const QString& orderCode)
{
    return runOnDbThread([&]() -> bool {
        if (!m_bOpened) return false;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen()) return false;

        QSqlQuery q(db);
        q.prepare(SQL_SELECT_HAS_SUCCESS_FULLBOX);
        q.addBindValue(orderCode);
        if (q.exec() && q.next())
            return q.value(0).toInt() > 0;
        return false;
    });
}

void SortingDatabase::saveWaveRawPayload(const QString& orderCode, const QByteArray& body)
{
    runOnDbThread([&]() {
        if (!m_bOpened) return;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen())
        {
            Data_WARN("[SortingDB] saveWaveRawPayload 数据库不可用 order=%s", orderCode.toLocal8Bit().data());
            return;
        }

        QSqlQuery q(db);
        q.prepare(SQL_INSERT_WAVE_RAW);
        q.addBindValue(orderCode);
        q.addBindValue(QString::fromUtf8(body));
        q.addBindValue(currentTimeStr());
        if (!q.exec())
            Data_ERROR("[SortingDB] saveWaveRawPayload 失败 order=%s err=%s",
                orderCode.toLocal8Bit().data(), q.lastError().text().toLocal8Bit().data());
        else
            Data_INFO("[SortingDB] H4原始报文已落库 order=%s body=%d字节",
                orderCode.toLocal8Bit().data(), body.size());
    });
}

QByteArray SortingDatabase::getWaveRawPayload(const QString& orderCode)
{
    return runOnDbThread([&]() -> QByteArray {
        QByteArray result;
        if (!m_bOpened) return result;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen()) return result;

        QSqlQuery q(db);
        q.prepare(SQL_SELECT_WAVE_RAW);
        q.addBindValue(orderCode);
        if (q.exec() && q.next())
            result = q.value(0).toString().toUtf8();
        return result;
    });
}

// ============================================================================
// 每日峰值效率（2026-09-07）
// ============================================================================

bool SortingDatabase::saveDailyPeak(const QString& date, int peakPerMinute)
{
    return runOnDbThread([&]() -> bool {
        if (!m_bOpened) return false;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen()) return false;

        QSqlQuery q(db);
        q.prepare(SQL_INSERT_DAILY_PEAK);
        q.addBindValue(date);
        q.addBindValue(peakPerMinute);
        q.addBindValue(peakPerMinute * 60);   // peak_per_hour（展示口径）
        q.addBindValue(currentTimeStr());
        bool ok = q.exec();
        if (!ok)
            Data_ERROR("[SortingDB] saveDailyPeak 失败 date=%s peakPerMinute=%d err=%s",
                date.toLocal8Bit().data(), peakPerMinute, q.lastError().text().toLocal8Bit().data());
        return ok;
    });
}

int SortingDatabase::getDailyPeakPerMinute(const QString& date)
{
    return runOnDbThread([&]() -> int {
        if (!m_bOpened) return 0;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen()) return 0;

        QSqlQuery q(db);
        q.prepare(SQL_SELECT_DAILY_PEAK);
        q.addBindValue(date);
        if (q.exec() && q.next())
            return q.value(0).toInt();
        return 0;
    });
}

bool SortingDatabase::insertWaveItems(const QString& orderCode, const QVector<ReturnWaveItemRecord>& items)
{
    return runOnDbThread([&]() -> bool {
        if (!m_bOpened) return false;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen()) return false;

        // ★ 2026-09-09 需求4（修正）：波次明细落库使用【原子单事务】——
        //   ① 少量数据（<1000）与大量数据（30000）行为一致：DELETE+全部 INSERT 一次性 commit；
        //   ② 中途任一行失败 → 整体 rollback，不会出现"旧明细已删+新明细只写一半"的半截数据；
        //   ③ DB 写入本就在 SortingDB 专用线程串行执行（runOnDbThread 队列），分批提交并不能让
        //      并发的落格 insertRecord 插队，只会牺牲原子性——故不做分批
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

        // 批量插入新明细（单事务内循环插入）
        // ★ 2026-09-09 性能：50000 item 大波次——prepared 语句复用执行（每 WAVE_ITEM_PREP_REBUILD 行
        //   重建一次 QSqlQuery，规避历史 Qt 绑值累积问题），比"每条全新 exec 整串 SQL"快约 3~10 倍
        {
            auto esc = [](const QString& s) -> QString {
                QString r = s;
                return r.replace(QLatin1Char('\''), QLatin1String("''"));
            };
            const QString insertSql =
                "INSERT INTO return_wave_item "
                "(order_code, inco, grid_num, grid_type, plan_qty, sorted_qty, volu, obx_code) "
                "VALUES (?, ?, ?, ?, ?, 0, ?, ?)";
            int execCount = 0;
            QSqlQuery q(db);
            q.prepare(insertSql);
            for (const auto& item : items)
            {
                // 每批重建 prepared（规避 Qt 绑值累积，历史踩坑点）
                if (execCount > 0 && (execCount % WAVE_ITEM_PREP_REBUILD) == 0)
                {
                    q = QSqlQuery(db);
                    q.prepare(insertSql);
                }
                q.addBindValue(item.orderCode);
                q.addBindValue(esc(item.inco));
                q.addBindValue(esc(item.gridNum));
                q.addBindValue(item.gridType.isEmpty() ? QString("0") : item.gridType);  // 0=分类, 1=异常, 2=发货
                q.addBindValue(item.planQty);
                q.addBindValue(esc(item.volu.isNull() ? QString("") : item.volu));
                q.addBindValue(esc(item.obxCode.isNull() ? QString("") : item.obxCode));

                if (!q.exec())
                {
                    Data_WARN("[SortingDB] 插入明细失败 orderCode=%s inco=%s grid=%s err=%s",
                        item.orderCode.toLocal8Bit().data(),
                        item.inco.toLocal8Bit().data(),
                        item.gridNum.toLocal8Bit().data(),
                        q.lastError().text().toLocal8Bit().data());
                    db.rollback();
                    return false;
                }
                execCount++;
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
    });
}

QVector<ReturnWaveItemRecord> SortingDatabase::getWaveItems(const QString& orderCode)
{
    return runOnDbThread([&]() -> QVector<ReturnWaveItemRecord> {
        QVector<ReturnWaveItemRecord> result;
        if (!m_bOpened) return result;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
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
    });
}

bool SortingDatabase::incrementSortedQty(const QString& orderCode, const QString& inco, const QString& gridNum)
{
    return runOnDbThread([&]() -> bool {
        if (!m_bOpened) return false;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen()) return false;

        QSqlQuery q(db);
        // UPDATE return_wave_item SET sorted_qty = sorted_qty + 1 WHERE order_code = ? AND inco = ? AND grid_num = ?
        q.prepare(SQL_INCREMENT_SORTED_QTY);
        q.addBindValue(orderCode);
        q.addBindValue(inco);
        q.addBindValue(gridNum);
        return q.exec();
    });
}

// ============================================================================
// S0 新增：容器绑定
// ============================================================================

bool SortingDatabase::bindGridBox(const QString& gridNum, const QString& boxcode, const QString& orderCode)
{
    return runOnDbThread([&]() -> bool {
        if (!m_bOpened)
        {
            Data_ERROR("[SortingDB] bindGridBox 数据库未打开 grid=%s box=%s", gridNum.toLocal8Bit().data(), boxcode.toLocal8Bit().data());
            return false;
        }
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        Data_INFO("[SortingDB] bindGridBox 获取连接 connectionName=SortingDB isValid=%d isOpen=%d grid=%s box=%s",
            db.isValid() ? 1 : 0, db.isOpen() ? 1 : 0,
            gridNum.toLocal8Bit().data(), boxcode.toLocal8Bit().data());
        if (!db.isOpen())
        {
            Data_ERROR("[SortingDB] bindGridBox 数据库连接失败 grid=%s box=%s errCode=%s errText=%s",
                gridNum.toLocal8Bit().data(), boxcode.toLocal8Bit().data(),
                db.lastError().nativeErrorCode().toLocal8Bit().data(),
                db.lastError().text().toLocal8Bit().data());
            return false;
        }

        QString now = currentTimeStr();

        // 先归档旧绑定
        {
            QSqlQuery q(db);
            // UPDATE grid_box_bind SET active = 0, unbind_time = ? WHERE grid_num = ? AND active = 1
            q.prepare(SQL_ARCHIVE_OLD_BIND);
            q.addBindValue(now);
            q.addBindValue(gridNum);
            if (!q.exec())
            {
                Data_ERROR("[SortingDB] bindGridBox 归档旧绑定失败 grid=%s err=%s",
                    gridNum.toLocal8Bit().data(), q.lastError().text().toLocal8Bit().data());
            }
        }

        // 插入新绑定
        QSqlQuery q(db);
        // INSERT INTO grid_box_bind (grid_num, boxcode, order_code, active, bind_time, unbind_time) VALUES (?, ?, ?, 1, ?, '')
        q.prepare(SQL_INSERT_GRID_BIND);
        q.addBindValue(gridNum);
        q.addBindValue(boxcode);
        // ★ 2026-09-06 关联所属波次（切换波次时按此恢复格口绑定视图；历史行为=空串）
        q.addBindValue(orderCode);
        q.addBindValue(now);
        if (!q.exec())
        {
            Data_ERROR("[SortingDB] bindGridBox 插入失败 grid=%s box=%s err=%s",
                gridNum.toLocal8Bit().data(), boxcode.toLocal8Bit().data(),
                q.lastError().text().toLocal8Bit().data());
            return false;
        }
        return true;
    });
}

GridBoxBindRecord SortingDatabase::getActiveBind(const QString& gridNum)
{
    return runOnDbThread([&]() -> GridBoxBindRecord {
        GridBoxBindRecord rec;
        if (!m_bOpened) return rec;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
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
    });
}

bool SortingDatabase::archiveGridBinds(const QString& gridNum)
{
    return runOnDbThread([&]() -> bool {
        if (!m_bOpened) return false;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen()) return false;

        QSqlQuery q(db);
        q.prepare(SQL_ARCHIVE_OLD_BIND);
        q.addBindValue(currentTimeStr());
        q.addBindValue(gridNum);
        return q.exec();
    });
}

QVector<GridBoxBindRecord> SortingDatabase::getAllActiveBinds()
{
    return runOnDbThread([&]() -> QVector<GridBoxBindRecord> {
        QVector<GridBoxBindRecord> result;
        if (!m_bOpened) return result;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen()) return result;

        QSqlQuery q(db);
        q.prepare(SQL_SELECT_ALL_ACTIVE_BINDS);
        if (!q.exec()) return result;
        while (q.next()) {
            GridBoxBindRecord rec;
            rec.gridNum = q.value(0).toString();
            rec.boxcode = q.value(1).toString();
            rec.orderCode = q.value(2).toString();
            rec.bindTime = q.value(3).toString();
            rec.active = true;
            result.append(rec);
        }
        return result;
    });
}

// ★ 2026-09-06 按波次查询绑定快照（每格取该波次最近一条，含已归档）——波次切换恢复绑定视图用
QMap<QString, QString> SortingDatabase::getBindsByOrder(const QString& orderCode)
{
    return runOnDbThread([&]() -> QMap<QString, QString> {
        QMap<QString, QString> result;
        if (!m_bOpened || orderCode.isEmpty()) return result;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen()) return result;

        QSqlQuery q(db);
        q.prepare(SQL_SELECT_BINDS_BY_ORDER);
        q.addBindValue(orderCode);
        q.addBindValue(orderCode);
        if (!q.exec()) return result;
        while (q.next())
            result.insert(q.value(0).toString(), q.value(1).toString());
        return result;
    });
}

// ★ 2026-09-07 每格最近一次绑定记录（无论 active）——无当前绑定时"沿用上一波次绑定"用
QMap<QString, QString> SortingDatabase::getLastKnownBinds()
{
    return runOnDbThread([&]() -> QMap<QString, QString> {
        QMap<QString, QString> result;
        if (!m_bOpened) return result;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen()) return result;

        QSqlQuery q(db);
        q.prepare(SQL_SELECT_LAST_KNOWN_BINDS);
        if (!q.exec()) return result;
        while (q.next())
            result.insert(q.value(0).toString(), q.value(1).toString());
        return result;
    });
}

bool SortingDatabase::archiveAllBinds()
{
    return runOnDbThread([&]() -> bool {
        if (!m_bOpened) return false;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen()) return false;

        QSqlQuery q(db);
        q.prepare(SQL_ARCHIVE_ALL_BINDS);
        q.addBindValue(currentTimeStr());
        return q.exec();
    });
}

// ============================================================================
// S0 新增：分拣流水
// ============================================================================

bool SortingDatabase::insertSortTxn(const SortTxnRecord& txn)
{
    return runOnDbThread([&]() -> bool {
        if (!m_bOpened) return false;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen())
        {
            Data_ERROR("[SortingDB] insertSortTxn 数据库连接失败 epc=%s grid=%s errCode=%s errText=%s",
                txn.epc.toLocal8Bit().data(), txn.gridNum.toLocal8Bit().data(),
                db.lastError().nativeErrorCode().toLocal8Bit().data(),
                db.lastError().text().toLocal8Bit().data());
            return false;
        }

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
    });
}

bool SortingDatabase::isEpcAlreadySorted(const QString& orderCode, const QString& epc)
{
    return runOnDbThread([&]() -> bool {
        if (!m_bOpened) return false;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isValid() || !db.isOpen()) return false;

        QSqlQuery q(db);
        q.prepare(SQL_SELECT_TXN_BY_EPC);
        q.addBindValue(orderCode);
        q.addBindValue(epc);
        if (q.exec() && q.next())
            return true;
        return false;
    });
}

// ============================================================================
// S0 新增：Outbox 出站
// ============================================================================

bool SortingDatabase::insertOutboxFullbox(const OutboxRecord& msg)
{
    return runOnDbThread([&]() -> bool {
        if (!m_bOpened) return false;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen()) return false;

        QSqlQuery q(db);
        q.prepare(SQL_INSERT_OUTBOX_FULLBOX);
        q.addBindValue(msg.msgId);
        q.addBindValue(msg.orderCode);
        q.addBindValue(msg.boxcode);
        q.addBindValue(msg.grid);        // ★ 2026-09-08 满箱报文对应格口号（失败格口下拉直接读取）
        q.addBindValue(msg.payload);
        q.addBindValue(msg.nextRetry);
        q.addBindValue(msg.createdAt.isEmpty() ? currentTimeStr() : msg.createdAt);
        return q.exec();
    });
}

bool SortingDatabase::insertOutboxEnd(const OutboxRecord& msg)
{
    return runOnDbThread([&]() -> bool {
        if (!m_bOpened) return false;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen()) return false;

        QSqlQuery q(db);
        q.prepare(SQL_INSERT_OUTBOX_END);
        q.addBindValue(msg.msgId);
        q.addBindValue(msg.orderCode);
        q.addBindValue(msg.payload);
        q.addBindValue(msg.nextRetry);
        q.addBindValue(msg.createdAt.isEmpty() ? currentTimeStr() : msg.createdAt);
        return q.exec();
    });
}

QVector<OutboxRecord> SortingDatabase::getPendingOutboxFullbox(int limit)
{
    return runOnDbThread([&]() -> QVector<OutboxRecord> {
        QVector<OutboxRecord> result;
        if (!m_bOpened) return result;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
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
    });
}

bool SortingDatabase::updateOutboxFullboxStatus(const QString& msgId, const QString& newStatus, const QString& nextRetry)
{
    return runOnDbThread([&]() -> bool {
        if (!m_bOpened) return false;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen()) return false;

        QSqlQuery q(db);
        q.prepare(SQL_UPDATE_OUTBOX_STATUS);
        q.addBindValue(newStatus);
        q.addBindValue(nextRetry);
        q.addBindValue(msgId);
        return q.exec();
    });
}

bool SortingDatabase::markOutboxFullboxSuccess(const QString& msgId)
{
    return runOnDbThread([&]() -> bool {
        if (!m_bOpened) return false;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen()) return false;

        QSqlQuery q(db);
        q.prepare(SQL_MARK_OUTBOX_SUCCESS);
        q.addBindValue(msgId);
        return q.exec();
    });
}

// ──── 完结回传出站消息操作（H8）────

QVector<OutboxRecord> SortingDatabase::getPendingOutboxEnd(int limit)
{
    return runOnDbThread([&]() -> QVector<OutboxRecord> {
        QVector<OutboxRecord> result;
        if (!m_bOpened) return result;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
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
    });
}

bool SortingDatabase::updateOutboxEndStatus(const QString& msgId, const QString& newStatus, const QString& nextRetry)
{
    return runOnDbThread([&]() -> bool {
        if (!m_bOpened) return false;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen()) return false;

        QSqlQuery q(db);
        // UPDATE outbox_end SET status = ?, retry_count = retry_count + 1, next_retry = ? WHERE msg_id = ?
        q.prepare(SQL_UPDATE_OUTBOX_END_STATUS);
        q.addBindValue(newStatus);
        q.addBindValue(nextRetry);
        q.addBindValue(msgId);
        return q.exec();
    });
}

bool SortingDatabase::markOutboxEndSuccess(const QString& msgId)
{
    return runOnDbThread([&]() -> bool {
        if (!m_bOpened) return false;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen()) return false;

        QSqlQuery q(db);
        // UPDATE outbox_end SET status = 'success' WHERE msg_id = ?
        q.prepare(SQL_MARK_OUTBOX_END_SUCCESS);
        q.addBindValue(msgId);
        return q.exec();
    });
}

// ──── 人工重发查询 ────

QVector<OutboxRecord> SortingDatabase::getOutboxByOrderCode(const QString& orderCode)
{
    return runOnDbThread([&]() -> QVector<OutboxRecord> {
        QVector<OutboxRecord> result;
        if (!m_bOpened) return result;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
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
    });
}

QVector<OutboxRecord> SortingDatabase::getOutboxFullboxByOrder(const QString& orderCode)
{
    return runOnDbThread([&]() -> QVector<OutboxRecord> {
        QVector<OutboxRecord> result;
        if (!m_bOpened) return result;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen()) return result;

        QSqlQuery q(db);
        q.prepare(SQL_SELECT_OUTBOX_FULLBOX_BY_ORDER_ALL);
        q.addBindValue(orderCode);
        if (!q.exec()) return result;
        while (q.next()) {
            OutboxRecord rec;
            rec.msgId = q.value(0).toString();
            rec.orderCode = q.value(1).toString();
            rec.boxcode = q.value(2).toString();
            rec.grid = q.value(3).toString();       // ★ 2026-09-08 格口号
            rec.payload = q.value(4).toString();
            rec.status = q.value(5).toString();
            rec.retryCount = q.value(6).toInt();
            rec.createdAt = q.value(7).toString();
            result.append(rec);
        }
        return result;
    });
}

QVector<OutboxRecord> SortingDatabase::getOutboxEndByOrder(const QString& orderCode)
{
    return runOnDbThread([&]() -> QVector<OutboxRecord> {
        QVector<OutboxRecord> result;
        if (!m_bOpened) return result;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen()) return result;

        QSqlQuery q(db);
        q.prepare(SQL_SELECT_OUTBOX_END_BY_ORDER_ALL);
        q.addBindValue(orderCode);
        if (!q.exec()) return result;
        while (q.next()) {
            OutboxRecord rec;
            rec.msgId = q.value(0).toString();
            rec.orderCode = q.value(1).toString();
            rec.payload = q.value(2).toString();
            rec.status = q.value(3).toString();
            rec.retryCount = q.value(4).toInt();
            rec.createdAt = q.value(5).toString();
            result.append(rec);
        }
        return result;
    });
}

OutboxRecord SortingDatabase::getOutboxFullboxByMsgId(const QString& msgId)
{
    return runOnDbThread([&]() -> OutboxRecord {
        OutboxRecord rec;
        if (!m_bOpened) return rec;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen()) return rec;

        QSqlQuery q(db);
        // SELECT msg_id, order_code, boxcode, grid, payload, retry_count FROM outbox_fullbox WHERE msg_id = ?
        q.prepare(SQL_SELECT_OUTBOX_BY_MSGID);
        q.addBindValue(msgId);
        if (q.exec() && q.next()) {
            rec.msgId = q.value(0).toString();
            rec.orderCode = q.value(1).toString();
            rec.boxcode = q.value(2).toString();
            rec.grid = q.value(3).toString();       // ★ 2026-09-08 格口号
            rec.payload = q.value(4).toString();
            rec.retryCount = q.value(5).toInt();
        }
        return rec;
    });
}

// ============================================================================
// ★ 2026-09-08 UI 失败重传下拉：全部历史"重试耗尽失败 / 已取消重试"的满箱（H7）报文
//   说明：cancelled 是波次被切出后 pollOutboxFullbox 主动取消重试产生的，
//         同样属于"待人工重传"的消息，必须一并列出，否则会从界面消失
// ============================================================================
QVector<OutboxRecord> SortingDatabase::getFailedOutboxFullbox(int limit)
{
    return runOnDbThread([&]() -> QVector<OutboxRecord> {
        QVector<OutboxRecord> result;
        if (!m_bOpened) return result;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen()) return result;

        QSqlQuery q(db);
        q.prepare(SQL_SELECT_FAILED_OUTBOX_FULLBOX);
        q.addBindValue(limit);
        if (!q.exec()) return result;
        while (q.next()) {
            OutboxRecord rec;
            rec.msgId = q.value(0).toString();
            rec.orderCode = q.value(1).toString();
            rec.boxcode = q.value(2).toString();
            rec.grid = q.value(3).toString();
            rec.payload = q.value(4).toString();
            rec.status = q.value(5).toString();
            rec.retryCount = q.value(6).toInt();
            rec.createdAt = q.value(7).toString();
            result.append(rec);
        }
        return result;
    });
}

// ★ 2026-09-08 UI 失败重传下拉：全部历史"重试耗尽失败 / 已取消重试"的完结（H8）报文
QVector<OutboxRecord> SortingDatabase::getFailedOutboxEnd(int limit)
{
    return runOnDbThread([&]() -> QVector<OutboxRecord> {
        QVector<OutboxRecord> result;
        if (!m_bOpened) return result;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen()) return result;

        QSqlQuery q(db);
        q.prepare(SQL_SELECT_FAILED_OUTBOX_END);
        q.addBindValue(limit);
        if (!q.exec()) return result;
        while (q.next()) {
            OutboxRecord rec;
            rec.msgId = q.value(0).toString();
            rec.orderCode = q.value(1).toString();
            rec.payload = q.value(2).toString();
            rec.status = q.value(3).toString();
            rec.retryCount = q.value(4).toInt();
            rec.createdAt = q.value(5).toString();
            result.append(rec);
        }
        return result;
    });
}

// ============================================================================
// S0 新增：完结波次历史存档（轻量摘要，仅一份证据记录）
// ============================================================================

bool SortingDatabase::archiveWave(const QString& orderCode)
{
    return runOnDbThread([&]() -> bool {
        if (!m_bOpened) return false;

        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen()) return false;

        // ── 从主库查询摘要数据 ──
        int waveStatus = 0, orderQty = 0, totalItems = 0, sortedCount = 0, exceptionCount = 0;
        QString startTime, endTime = currentTimeStr();

        QSqlQuery q(db);

        // 波次头
        q.prepare("SELECT order_qty, status, created_at, updated_at FROM return_wave WHERE order_code = ?");
        q.addBindValue(orderCode);
        if (q.exec() && q.next()) {
            orderQty    = q.value(0).toInt();
            waveStatus  = q.value(1).toInt();
            startTime   = q.value(2).toString();
        }

        // 波次明细总数
        q.prepare("SELECT COUNT(*) FROM return_wave_item WHERE order_code = ?");
        q.addBindValue(orderCode);
        if (q.exec() && q.next()) totalItems = q.value(0).toInt();

        // 分拣完成数
        q.prepare("SELECT COUNT(*) FROM sorting_records WHERE order_code = ?");
        q.addBindValue(orderCode);
        if (q.exec() && q.next()) sortedCount = q.value(0).toInt();

        // 异常记录数
        q.prepare("SELECT COUNT(*) FROM exception_record WHERE order_code = ?");
        q.addBindValue(orderCode);
        if (q.exec() && q.next()) exceptionCount = q.value(0).toInt();

        // 满箱回传数
        int fullboxCount = 0;
        q.prepare("SELECT COUNT(*) FROM outbox_fullbox WHERE order_code = ?");
        q.addBindValue(orderCode);
        if (q.exec() && q.next()) fullboxCount = q.value(0).toInt();

        // ── 打开历史数据库写入摘要 ──
        QString historyPath = QCoreApplication::applicationDirPath() + "/" SORTING_HISTORY_DB_FILE;
        QDir dir = QFileInfo(historyPath).absoluteDir();
        if (!dir.exists()) dir.mkpath(".");

        {
            QSqlDatabase histDb = QSqlDatabase::addDatabase("QSQLITE", "ArchiveDB");
            histDb.setDatabaseName(historyPath);
            if (!histDb.open())
            {
                Data_ERROR("[SortingDB] archiveWave 历史数据库打开失败 path=%s err=%s",
                    historyPath.toLocal8Bit().data(),
                    histDb.lastError().text().toLocal8Bit().data());
                return false;
            }

            QSqlQuery hq(histDb);
            hq.exec(SQL_CREATE_TABLE_WAVE_HISTORY);

            // 状态文本
            QString statusText = QString::fromUtf8("已完成");
            if (waveStatus == 8) statusText = QString::fromUtf8("已完成");

            hq.prepare(SQL_INSERT_WAVE_HISTORY);
            hq.addBindValue(orderCode);
            hq.addBindValue(orderQty);
            hq.addBindValue(waveStatus);
            hq.addBindValue(statusText);
            hq.addBindValue(startTime);
            hq.addBindValue(endTime);
            hq.addBindValue(totalItems);
            hq.addBindValue(sortedCount);
            hq.addBindValue(exceptionCount);
            hq.addBindValue(fullboxCount);
            hq.addBindValue(QString::fromUtf8("success"));
            hq.addBindValue(currentTimeStr());

            if (!hq.exec())
            {
                Data_ERROR("[SortingDB] archiveWave 写入历史摘要失败 order=%s err=%s",
                    orderCode.toLocal8Bit().data(), hq.lastError().text().toLocal8Bit().data());
                histDb.close();
                QSqlDatabase::removeDatabase("ArchiveDB");
                return false;
            }

            Data_INFO("[SortingDB] archiveWave 波次摘要已存档 order=%s sorted=%d exc=%d fullbox=%d path=%s",
                orderCode.toLocal8Bit().data(), sortedCount, exceptionCount, fullboxCount,
                historyPath.toLocal8Bit().data());

            histDb.close();
        }
        QSqlDatabase::removeDatabase("ArchiveDB");

        return true;
    });
}

// ★ S6 完结回传按 msgId 查询（H8）
OutboxRecord SortingDatabase::getOutboxEndByMsgId(const QString& msgId)
{
    return runOnDbThread([&]() -> OutboxRecord {
        OutboxRecord rec;
        if (!m_bOpened) return rec;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen()) return rec;

        QSqlQuery q(db);
        // SELECT msg_id, order_code, payload, retry_count FROM outbox_end WHERE msg_id = ?
        q.prepare(SQL_SELECT_OUTBOX_END_BY_MSGID);
        q.addBindValue(msgId);
        if (q.exec() && q.next()) {
            rec.msgId = q.value(0).toString();
            rec.orderCode = q.value(1).toString();
            rec.payload = q.value(2).toString();
            rec.retryCount = q.value(3).toInt();
        }
        return rec;
    });
}

// ============================================================================
// S0 新增：异常记录
// ============================================================================

bool SortingDatabase::insertException(const ExceptionRecord& ex)
{
    return runOnDbThread([&]() -> bool {
        if (!m_bOpened) return false;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
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
    });
}

QVector<ExceptionRecord> SortingDatabase::getOpenExceptions(const QString& orderCode)
{
    return runOnDbThread([&]() -> QVector<ExceptionRecord> {
        QVector<ExceptionRecord> result;
        if (!m_bOpened) return result;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
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
    });
}

// ★ S7 多条件异常查询（T-S7-03）
// 支持按 orderCode、EPC、异常类型、时间范围组合查询
QVector<ExceptionRecord> SortingDatabase::queryExceptions(
    const QString& orderCode, const QString& epc,
    const QString& type, const QString& startTime,
    const QString& endTime, int limit)
{
    return runOnDbThread([&]() -> QVector<ExceptionRecord> {
        QVector<ExceptionRecord> result;
        if (!m_bOpened) return result;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
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
    });
}

// ============================================================================
// ★ 2026-09-09 需求3：批量取异常原因（epc → "type: reason"）
//   EPC 查询面板状态列对异常件显示原因用；一次查询构建 map 避免逐条 SQL
// ============================================================================
QHash<QString, QString> SortingDatabase::queryExceptionReasons(const QString& orderCode)
{
    return runOnDbThread([&]() -> QHash<QString, QString> {
        QHash<QString, QString> result;
        if (!m_bOpened) return result;
        QSqlDatabase db = QSqlDatabase::database("SortingDB");
        if (!db.isOpen()) return result;

        QString sql = "SELECT epc, type, reason FROM exception_record";
        if (!orderCode.isEmpty())
            sql += " WHERE order_code = ?";
        QSqlQuery q(db);
        q.prepare(sql);
        if (!orderCode.isEmpty())
            q.addBindValue(orderCode);
        if (!q.exec()) return result;
        while (q.next()) {
            QString epc    = q.value(0).toString();
            QString type   = q.value(1).toString();
            QString reason = q.value(2).toString();
            QString text   = type.isEmpty() ? reason : QString("%1: %2").arg(type, reason);
            if (!result.contains(epc))
                result.insert(epc, text);
        }
        return result;
    });
}

// ============================================================================
// 辅助
// ============================================================================

QString SortingDatabase::currentTimeStr() const
{
    return QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
}