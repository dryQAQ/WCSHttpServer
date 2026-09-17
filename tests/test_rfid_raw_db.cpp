// ============================================================================
// test_rfid_raw_db.cpp — ★ 2026-09-15 需求验证（可执行自测，非产品代码）
//
// 验证「保留 RFID 推送的原始报文」的落库链路（需求 2 的数据库一层）：
//   · createTables 建 rfid_raw 表 + 索引
//   · insertRfidRawBatch 单事务批量写入（含 NOREAD 帧）
//   · queryRfidRawByEpc：按"识别归一后 EPC"与"识别前 EPC 原文"都能回查到同一帧整帧原文
//   · cleanupOldRfidRaw：只清理超期行，保留期内不动
//
// 构建：见 tests\run_tests.bat
// ============================================================================

#include <QCoreApplication>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QDebug>
#include <cstdio>

#include "SortingDatabase.h"

static int g_pass = 0;
static int g_fail = 0;

static void check(bool ok, const QString& what, const QString& got = QString())
{
    if (ok) { ++g_pass; std::printf("  [PASS] %s\n", what.toUtf8().constData()); }
    else
    {
        ++g_fail;
        std::printf("  [FAIL] %s%s\n", what.toUtf8().constData(),
                    got.isEmpty() ? "" : ("  ← 实际: " + got).toUtf8().constData());
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const QString EPC_RAW_LONG = "A1012501020087626498017735303032";   // 现场 32 位串（尾部多附加数据）
    const QString EPC_CUT      = "A10125010200876264980177";           // 截断后（前 24 位）
    const QString EPC_OK       = "A10126010300853174002539";           // 现场正常 24 位串
    const QString FRAME_LONG   = "{SN0098|01|" + EPC_RAW_LONG + "}0D";
    const QString FRAME_OK     = "{SN0027|01|" + EPC_OK + "}0D";
    const QString FRAME_NOREAD = "{SN0031|01|NOREAD}0D";

    const QString dbPath = QCoreApplication::applicationDirPath() + "/test_rfid_raw.db";
    QFile::remove(dbPath);
    QFile::remove(dbPath + "-wal");
    QFile::remove(dbPath + "-shm");

    SortingDatabase& db = SortingDatabase::instance();

    std::printf("== ① 打开数据库（自动建表 rfid_raw）==\n");
    check(db.open(dbPath), QString("数据库打开成功"), dbPath);
    check(db.isOpen(), QString("isOpen()=true"));

    std::printf("== ② 批量落库 3 帧（截断帧 / 正常帧 / NOREAD 帧）==\n");
    QVector<RfidRawRecord> rows;
    {
        RfidRawRecord r;   // 长串截断帧
        r.time = "2026-09-15 10:00:00"; r.epc = EPC_CUT; r.epcRaw = EPC_RAW_LONG;
        r.carNum = "98"; r.seq = "SN0098"; r.devCode = "01";
        r.rawFrame = FRAME_LONG; r.bytes = FRAME_LONG.toUtf8().size(); r.noread = false;
        rows.append(r);

        RfidRawRecord r2;  // 正常 24 位帧
        r2.time = "2026-09-15 10:00:01"; r2.epc = EPC_OK; r2.epcRaw = EPC_OK;
        r2.carNum = "27"; r2.seq = "SN0027"; r2.devCode = "01";
        r2.rawFrame = FRAME_OK; r2.bytes = FRAME_OK.toUtf8().size(); r2.noread = false;
        rows.append(r2);

        RfidRawRecord r3;  // NOREAD 帧（epc 空，仅留痕）
        r3.time = "2026-09-15 10:00:02"; r3.epc = ""; r3.epcRaw = "";
        r3.carNum = "31"; r3.seq = "SN0031"; r3.devCode = "01";
        r3.rawFrame = FRAME_NOREAD; r3.bytes = FRAME_NOREAD.toUtf8().size(); r3.noread = true;
        rows.append(r3);
    }
    const int written = db.insertRfidRawBatch(rows);
    check(written == 3, QString("insertRfidRawBatch 写入 3 行"), QString::number(written));

    std::printf("== ③ 按 EPC 回查整帧原文 ==\n");
    {
        const QVector<RfidRawRecord> q1 = db.queryRfidRawByEpc(EPC_CUT);
        check(q1.size() == 1, QString("按【截断后 EPC】查到 1 帧"), QString::number(q1.size()));
        if (!q1.isEmpty())
        {
            check(q1.first().rawFrame == FRAME_LONG, QString("整帧原文一致（含 {} 与 0D）"), q1.first().rawFrame);
            check(q1.first().epcRaw == EPC_RAW_LONG, QString("epc_raw 保留截断前原始 EPC"), q1.first().epcRaw);
            check(q1.first().seq == "SN0098" && q1.first().carNum == "98" && q1.first().devCode == "01",
                  QString("seq/carNum/devCode 留痕正确"),
                  q1.first().seq + "/" + q1.first().carNum + "/" + q1.first().devCode);
            check(q1.first().bytes == FRAME_LONG.toUtf8().size(), QString("帧字节数正确"),
                  QString::number(q1.first().bytes));
        }

        const QVector<RfidRawRecord> q2 = db.queryRfidRawByEpc(EPC_RAW_LONG);
        check(q2.size() == 1, QString("按【截断前原始 EPC】也能查到同一帧（配置调整前后都能回查）"),
              QString::number(q2.size()));

        const QVector<RfidRawRecord> q3 = db.queryRfidRawByEpc(EPC_OK);
        check(q3.size() == 1 && q3.first().rawFrame == FRAME_OK,
              QString("正常 24 位帧原文可回查"),
              q3.isEmpty() ? QString() : q3.first().rawFrame);
    }

    std::printf("== ③b 空字段帧（carNum/seq/devCode 为 null QString）也要能落库 ==\n");
    {
        // 回归用例：Qt 会把 null QString 绑定为 SQL NULL，若列是 NOT NULL 就会静默丢帧
        //（现场会出现：NOREAD 帧 epc 空、流水号无数字→carNum 空、两段帧 devCode 空）
        QVector<RfidRawRecord> bare;
        RfidRawRecord r;   // 只填 time/rawFrame，其余保持默认（null QString）
        r.time = "2026-09-15 10:00:03";
        r.rawFrame = "{|01|}0D";
        r.bytes = r.rawFrame.toUtf8().size();
        bare.append(r);
        const int w = db.insertRfidRawBatch(bare);
        check(w == 1, QString("空字段帧落库成功（NOT NULL 约束不报错）"), QString::number(w));
    }

    std::printf("== ④ NOREAD 帧同样留痕（直读库核对）==\n");
    {
        // 诊断：直读前先核对能否继续写入（WAL 下只读连接不应阻塞写入）
        QSqlDatabase v = QSqlDatabase::addDatabase("QSQLITE", "verifyConn");
        v.setDatabaseName(dbPath);
        v.setConnectOptions("QSQLITE_OPEN_READONLY");
        check(v.open(), QString("只读校验连接打开成功"));
        if (v.isOpen())
        {
            QSqlQuery q(v);
            if (q.exec("SELECT COUNT(*) FROM rfid_raw WHERE noread=1 AND epc='' AND raw_frame='" + FRAME_NOREAD + "'")
                && q.next())
                check(q.value(0).toInt() == 1, QString("NOREAD 帧（epc 空）已留痕且原文完整"),
                      QString::number(q.value(0).toInt()));
            else
                check(false, QString("NOREAD 帧查询失败"), q.lastError().text());

            if (q.exec("SELECT COUNT(*) FROM rfid_raw") && q.next())
                check(q.value(0).toInt() == 4, QString("表内共 4 行"), QString::number(q.value(0).toInt()));
            v.close();
        }
        QSqlDatabase::removeDatabase("verifyConn");
    }

    std::printf("== ⑤ 超期清理：只删保留期外的行 ==\n");
    {
        QVector<RfidRawRecord> old;
        RfidRawRecord r;   // 30 天前的旧行
        r.time = QDateTime::currentDateTime().addDays(-30).toString("yyyy-MM-dd HH:mm:ss");
        r.epc = "EPC_OLD"; r.epcRaw = "EPC_OLD"; r.rawFrame = "{SN0001|01|EPC_OLD}0D";
        r.bytes = r.rawFrame.toUtf8().size();
        std::printf("  (诊断) 旧行 time=%s 行数=%d\n", r.time.toUtf8().constData(), (int)old.size() + 1);
        old.append(r);
        const int oldWritten = db.insertRfidRawBatch(old);
        check(oldWritten == 1, QString("旧行（30 天前）已插入"), QString::number(oldWritten));

        const int deleted = db.cleanupOldRfidRaw(7);   // 保留 7 天
        check(deleted == 1, QString("清理 1 行超期原始报文（保留期内不动）"), QString::number(deleted));

        const QVector<RfidRawRecord> still = db.queryRfidRawByEpc(EPC_CUT);
        check(still.size() == 1, QString("保留期内的帧仍在（未被误删）"), QString::number(still.size()));

        // 诊断：直读总行数（应为 3 = 原始 3 行，旧行已删）
        QSqlDatabase v2 = QSqlDatabase::addDatabase("QSQLITE", "verifyConn2");
        v2.setDatabaseName(dbPath);
        v2.setConnectOptions("QSQLITE_OPEN_READONLY");
        if (v2.open())
        {
            QSqlQuery q2(v2);
            if (q2.exec("SELECT COUNT(*) FROM rfid_raw") && q2.next())
                check(q2.value(0).toInt() == 4, QString("清理后表内共 4 行"), QString::number(q2.value(0).toInt()));
            const int cOld = [&]() {
                QSqlQuery q3(v2);
                if (q3.exec("SELECT COUNT(*) FROM rfid_raw WHERE epc='EPC_OLD'") && q3.next())
                    return q3.value(0).toInt();
                return -1;
            }();
            check(cOld == 0, QString("旧行已从表中移除"), QString::number(cOld));
            v2.close();
        }
        QSqlDatabase::removeDatabase("verifyConn2");
    }

    db.close();
    std::printf("\n===== 结果：通过 %d 项，失败 %d 项 =====\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
