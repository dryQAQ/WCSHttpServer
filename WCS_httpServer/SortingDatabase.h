#pragma once
// ============================================================================
// SortingDatabase.h — 分拣记录 SQLite 本地存储
//
// 仿照 WCSApp DataCenter 的设计，使用 Qt QSqlDatabase (SQLite 驱动)
// 存储每次 PLC 反馈落格的分拣明细，供客户查询和历史追溯。
//
// 表结构: sorting_records
//   id          INTEGER PK AUTOINCREMENT
//   order_code  TEXT    波次号
//   barcode     TEXT    条码/SKU
//   grid_num    TEXT    格口号
//   car_num     TEXT    小车号
//   grid_count  INTEGER 配货件数
//   volu        TEXT    来源库位
//   sort_time   TEXT    分拣完成时间 (yyyy-MM-dd HH:mm:ss.zzz)
//   create_time TEXT    记录创建时间
//
// 索引: idx_barcode / idx_order_code / idx_sort_time
//
// 使用方式:
//   SortingDatabase db;
//   db.open();                                    // 服务启动时调用
//   db.insertRecord(orderCode, barcode, grid, car, gridCount, volu);
//   auto records = db.queryByBarcode("SKU123");   // 按条码查询
//   auto records = db.queryByTime(t1, t2);        // 按时间范围查询
//   auto stats = db.statistics();                 // 获取统计信息
//   db.close();                                   // 服务停止时调用
// ============================================================================

#include <QObject>
#include <QString>
#include <QDateTime>
#include <QVector>
#include <QSqlDatabase>
#include <QMutex>
#include <QMutexLocker>

// ──── 分拣记录结构 ────
struct SortingRecord
{
    int     id          = 0;
    QString orderCode;          // 波次号
    QString barcode;            // 条码
    QString gridNum;            // 格口号
    QString carNum;             // 小车号
    int     gridCount   = 0;    // 配货件数
    QString volu;               // 来源库位
    QString sortTime;           // 分拣完成时间
    QString createTime;         // 记录创建时间
};

// ──── 统计信息 ────
struct SortingStatistics
{
    int totalRecords    = 0;    // 总记录数
    int todayRecords    = 0;    // 今日记录数
    int totalWaves      = 0;    // 波次数（去重）
    int totalGrids      = 0;    // 格口使用数（去重）
    QString lastSortTime;       // 最近分拣时间
};

class SortingDatabase
{
public:
    SortingDatabase();
    ~SortingDatabase();

    // ──── 生命周期 ────
    bool open(const QString& dbPath = QString());   // 打开/创建数据库
    void close();                                    // 关闭数据库
    bool isOpen() const;                             // 是否已打开

    // ──── 写入 ────
    bool insertRecord(const QString& orderCode,
                      const QString& barcode,
                      const QString& gridNum,
                      const QString& carNum,
                      int gridCount,
                      const QString& volu);

    // ──── 查询 ────
    QVector<SortingRecord> queryByBarcode(const QString& barcode, int limit = 500);
    QVector<SortingRecord> queryByTime(const QDateTime& from, const QDateTime& to,
                                       int limit = 1000);
    QVector<SortingRecord> queryByOrderCode(const QString& orderCode, int limit = 1000);
    QVector<SortingRecord> queryAll(int limit = 1000);

    // ──── 统计 ────
    SortingStatistics statistics();
    int recordCount() const;                        // 总记录数
    int todayRecordCount() const;                   // 今日记录数

    // ──── 维护 ────
    void cleanupOldRecords(int retainDays = 30);    // 清理过期记录
    QString databasePath() const { return m_dbPath; }

private:
    void createTables();                            // 建表
    QString currentTimeStr() const;                 // 当前时间字符串

    QString       m_dbPath;
    QString       m_connectionName;                 // Qt 数据库连接名
    mutable QMutex m_mutex;                          // 保护数据库操作（mutable以便在const成员函数中加锁）
    bool          m_bOpened = false;
};
