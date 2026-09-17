#pragma once
// ============================================================================
// EpcCode.h — RFID EPC 码识别（★ 2026-09-15 需求；单一实现源，多处共用）
//
// 客户口径（2026-09-15 现场确认）：
//   「不管开头是不是 A101，只认识 A + 23 位数字组成的 EPC 码」
//   → EPC 码形态 = 大写字母 'A' + (长度-1) 位数字；长度 = XML rfidEpcTruncateLen，
//     默认 24（即 'A' + 23 位数字）。
//   → 在 RFID 推送串中**定位**该形态，而不是假定它就在开头、也不是简单"取前 N 位"：
//        · 尾部多出附加数据（现场样例 A10125010200876264980177 + 35303032）→ 正确取出前 24 位
//        · 头部多出杂串（如 0000A10125010200876264980177）        → 也能定位到真实 EPC
//        · 与"开头是不是 A101"无关：A101 只是现场当前的号段前缀，不作判定依据
//   → 未识别到该形态时**不猜**：调用方按原文继续处理并告警；
//     识别前的原文（epcRaw）与整帧原文一律留痕（run.log / 界面 / rfid_raw 表）。
//
// 长度取值：len < 2 视为不启用识别（保留原文）——至少要 'A' + 1 位数字才有意义。
// ============================================================================

#include <QString>

namespace EpcCode
{
// 识别 EPC：在 raw 中查找首个「'A' + (len-1) 位数字」形态（ASCII 数字，非 Unicode 数字）
//   入参 raw    — RFID 推送串中的 EPC 字段原文（调用方已 trim）
//        len    — EPC 码长度（默认 24；< 2 = 不识别）
//   出参 epcOut — 识别到的 len 位 EPC（返回 true 时有效）
//        pos    — 该 EPC 在 raw 中的起始下标（可传 nullptr；>0 = 原文前面还有多余字符）
//   返回 true = 识别到；false = 未识别（raw 为空 / len < 2 / 串中不含该形态）
inline bool extract(const QString& raw, int len, QString& epcOut, int* pos = nullptr)
{
    epcOut.clear();
    if (pos) *pos = -1;
    if (raw.isEmpty() || len < 2 || raw.size() < len)
        return false;

    const int lastStart = raw.size() - len;        // 起始下标上限（含）
    for (int i = 0; i <= lastStart; ++i)
    {
        if (raw.at(i) != QLatin1Char('A'))
            continue;                              // 锚点必须是 'A'（客户口径：A + N-1 位数字）

        bool allDigits = true;
        for (int j = i + 1; j < i + len; ++j)
        {
            const QChar c = raw.at(j);
            if (c < QLatin1Char('0') || c > QLatin1Char('9'))
            {
                allDigits = false;
                break;
            }
        }
        if (!allDigits)
            continue;

        epcOut = raw.mid(i, len);
        if (pos) *pos = i;
        return true;
    }
    return false;
}

// 便捷包装：识别成功返回识别结果，否则原样返回 raw
//   （调用方用返回值 == 原串 即可判断"是否发生归一"）
inline QString recognize(const QString& raw, int len)
{
    QString epc;
    return extract(raw, len, epc) ? epc : raw;
}

} // namespace EpcCode
