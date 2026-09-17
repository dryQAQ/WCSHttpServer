// ============================================================================
// test_epc_recognize.cpp — ★ 2026-09-15 需求验证（可执行自测，非产品代码）
//
// 需求 1：RFID 推送的 EPC 码按配置长度识别（rfidEpcTruncateLen，默认 24 位字符）
//         客户口径：**不管开头是不是 A101，只认识「A + 23 位数字」组成的 EPC 码**
//         → 在推送串中"定位"该形态（附加数据在前/在后都能取出），不是简单取前 N 位
// 需求 2：保留 RFID 推送的原始报文（整帧原文不因识别/归一丢失）
//
// 做法：直接用真实类 RfidPushClient::OnReceive 喂入现场帧字节，检查其 emit 的 JSON：
//   · epc        = 识别归一后的 EPC（A + N-1 位数字）
//   · epcRaw     = 识别前的 EPC 原文
//   · raw        = 整帧原文（含 {} 与字面帧尾 0D）
//   · normalized = 是否发生归一（原文与识别结果不同）
//   · noread     = NOREAD 帧标记（仅留痕，不进入分拣）
// 覆盖：尾部附加数据 / 头部杂串 / 正常件 / 非该形态原样保留 / NOREAD / TCP 分包 / 粘包多帧 / 关闭(0) / 长度可配
//
// 构建：见 tests\run_tests.bat
// ============================================================================

#include <QCoreApplication>
#include <QJsonObject>
#include <QJsonArray>
#include <QString>
#include <QVector>
#include <QDebug>
#include <cstdio>

#include "RfidPushClient.h"

// 把 protected 的 OnReceive 提升为 public，便于直接喂字节
class TestRfidPush : public RfidPushClient
{
public:
    using RfidPushClient::OnReceive;
};

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

// 收集本次 OnReceive 触发的所有帧
struct Frame
{
    QString epc, epcRaw, carNum, seq, devCode, raw;
    bool normalized = false;
    bool noread = false;
};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    TestRfidPush client;
    QVector<Frame> frames;
    QObject::connect(&client, &RfidPushClient::rfidPushReceived,
        [&frames](const QJsonObject& body) {
            const QJsonArray arr = body.value("data").toArray();
            for (const QJsonValue& v : arr)
            {
                const QJsonObject o = v.toObject();
                Frame f;
                f.epc        = o.value("epc").toString();
                f.epcRaw     = o.value("epcRaw").toString();
                f.carNum     = o.value("carNum").toString();
                f.seq        = o.value("seq").toString();
                f.devCode    = o.value("devCode").toString();
                f.raw        = o.value("raw").toString();
                f.normalized = o.value("normalized").toBool();
                f.noread     = o.value("noread").toBool();
                frames.append(f);
            }
        });

    auto feed = [&client](const QByteArray& bytes) {
        client.OnReceive(nullptr, 1, (const BYTE*)bytes.constData(), bytes.size());
    };

    // ── 现场样例（客户提供）──
    const QByteArray EPC_OK   = "A10126010300853174002539";           // 24 位：A + 23 位数字（正常件）
    const QByteArray EPC_BAD  = "A1012501020087626498017735303032";   // 32 位：尾部多 8 位附加数据（失败件）
    const QByteArray EPC_REAL = "A10125010200876264980177";           // EPC_BAD 中符合规则的真实 EPC

    std::printf("== ① 默认配置（24 位 = A+23 位数字）喂入 32 位长串（尾部附加数据）==\n");
    {
        client.setEpcTruncateLen(24);
        frames.clear();
        QByteArray f = "{SN0098|01|" + EPC_BAD + "}0D";
        feed(f);

        check(frames.size() == 1, QString("解析出 1 帧"), QString::number(frames.size()));
        if (!frames.isEmpty())
        {
            const Frame& x = frames.first();
            check(x.epc == QString::fromLatin1(EPC_REAL),
                  QString("识别出真实 EPC（A+23 位数字）"), x.epc);
            check(x.epcRaw == QString::fromLatin1(EPC_BAD),
                  QString("epcRaw 保留识别前原文"), x.epcRaw);
            check(x.normalized, QString("normalized=true"), x.normalized ? "true" : "false");
            check(x.raw == QString::fromLatin1(f),
                  QString("raw = 整帧原文（含 {} 与字面帧尾 0D）"), x.raw);
            check(x.seq == "SN0098" && x.carNum == "98" && x.devCode == "01",
                  QString("seq/carNum/devCode 解析正确"),
                  x.seq + "/" + x.carNum + "/" + x.devCode);
        }
    }

    std::printf("== ② 正常 24 位 EPC：不归一、原文照留 ==\n");
    {
        client.setEpcTruncateLen(24);
        frames.clear();
        QByteArray f = "{SN0027|01|" + EPC_OK + "}0D";
        feed(f);

        check(frames.size() == 1, QString("解析出 1 帧"), QString::number(frames.size()));
        if (!frames.isEmpty())
        {
            const Frame& x = frames.first();
            check(x.epc == QString::fromLatin1(EPC_OK), QString("epc 原样保留"), x.epc);
            check(!x.normalized, QString("normalized=false"), x.normalized ? "true" : "false");
            check(x.raw == QString::fromLatin1(f), QString("raw 保留整帧原文"), x.raw);
        }
    }

    std::printf("== ③ 前端杂串：EPC 不在开头也能定位（与开头是否为 A101 无关）==\n");
    {
        client.setEpcTruncateLen(24);
        frames.clear();
        QByteArray payload = "0000" + EPC_REAL + "9999";
        feed("{SN0033|01|" + payload + "}0D");

        check(frames.size() == 1, QString("解析出 1 帧"), QString::number(frames.size()));
        if (!frames.isEmpty())
        {
            const Frame& x = frames.first();
            check(x.epc == QString::fromLatin1(EPC_REAL),
                  QString("串中定位到 A+23 位数字 → 取出真实 EPC"), x.epc);
            check(x.normalized && x.epcRaw == QString::fromLatin1(payload),
                  QString("原文完整留痕（含前后杂串）"), x.epcRaw);
        }
    }

    std::printf("== ④ 非该形态（EPC001 / 纯数字）：不猜、原样保留 ==\n");
    {
        client.setEpcTruncateLen(24);
        frames.clear();
        feed("{SN0041|01|EPC001}0D");
        feed("{SN0042|01|123456789012345678901234567890}0D");

        check(frames.size() == 2, QString("解析出 2 帧"), QString::number(frames.size()));
        if (frames.size() == 2)
        {
            check(frames[0].epc == "EPC001" && !frames[0].normalized,
                  QString("EPC001：原样保留（不误判）"), frames[0].epc);
            check(frames[1].epc == "123456789012345678901234567890" && !frames[1].normalized,
                  QString("纯 30 位数字（无 A 锚点）：原样保留"), frames[1].epc);
        }
    }

    std::printf("== ⑤ NOREAD 帧：仅留痕（epc 空、原始报文保留）==\n");
    {
        frames.clear();
        QByteArray f = "{SN0031|01|NOREAD}0D";
        feed(f);

        check(frames.size() == 1, QString("NOREAD 帧同样上报（供原文留痕）"), QString::number(frames.size()));
        if (!frames.isEmpty())
        {
            const Frame& x = frames.first();
            check(x.epc.isEmpty(), QString("epc 为空（不进入分拣）"), x.epc);
            check(x.noread, QString("noread=true"), x.noread ? "true" : "false");
            check(x.raw == QString::fromLatin1(f), QString("NOREAD 帧原始报文保留"), x.raw);
        }
    }

    std::printf("== ⑥ TCP 分包：半帧 + 半帧 → 合成 1 帧，原文完整 ==\n");
    {
        frames.clear();
        QByteArray f = "{SN0055|01|" + EPC_BAD + "}0D";
        feed(f.left(12));
        check(frames.isEmpty(), QString("半帧不解析（等更多数据）"), QString::number(frames.size()));
        feed(f.mid(12));
        check(frames.size() == 1, QString("补全后解析出 1 帧"), QString::number(frames.size()));
        if (!frames.isEmpty())
            check(frames.first().raw == QString::fromLatin1(f),
                  QString("分包后整帧原文仍完整"), frames.first().raw);
    }

    std::printf("== ⑦ TCP 粘包：一包两帧 → 解析 2 帧，各自原文正确 ==\n");
    {
        frames.clear();
        QByteArray a = "{SN0061|01|" + EPC_OK + "}0D";
        QByteArray b = "{SN0062|01|" + EPC_BAD + "}0D";
        feed(a + b);

        check(frames.size() == 2, QString("解析出 2 帧"), QString::number(frames.size()));
        if (frames.size() == 2)
        {
            check(frames[0].raw == QString::fromLatin1(a) && frames[0].epc == QString::fromLatin1(EPC_OK),
                  QString("第 1 帧 原文/EPC 正确"), frames[0].raw);
            check(frames[1].raw == QString::fromLatin1(b)
                      && frames[1].epc == QString::fromLatin1(EPC_REAL),
                  QString("第 2 帧 原文保留且 EPC 识别正确"), frames[1].raw);
        }
    }

    std::printf("== ⑧ 关闭识别（rfidEpcTruncateLen=0）：整串原样使用 ==\n");
    {
        client.setEpcTruncateLen(0);
        frames.clear();
        QByteArray f = "{SN0071|01|" + EPC_BAD + "}0D";
        feed(f);

        check(frames.size() == 1, QString("解析出 1 帧"), QString::number(frames.size()));
        if (!frames.isEmpty())
        {
            const Frame& x = frames.first();
            check(x.epc == QString::fromLatin1(EPC_BAD), QString("epc=整串（回退改造前行为）"), x.epc);
            check(!x.normalized, QString("normalized=false"), x.normalized ? "true" : "false");
        }
        client.setEpcTruncateLen(24);
    }

    std::printf("== ⑨ 长度可配：N=20 时识别『A + 19 位数字』 ==\n");
    {
        client.setEpcTruncateLen(20);
        frames.clear();
        feed("{SN0081|01|" + EPC_OK + "}0D");

        check(frames.size() == 1 && frames.first().epc == QString::fromLatin1(EPC_OK).left(20),
              QString("epc 按配置长度识别"), frames.isEmpty() ? QString() : frames.first().epc);
        client.setEpcTruncateLen(24);
    }

    std::printf("\n===== 结果：通过 %d 项，失败 %d 项 =====\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

