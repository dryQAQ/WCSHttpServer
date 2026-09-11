#pragma once
// ============================================================================
// WmsGridCode.h — WMS 格口编码 ↔ 内部格口号 转换工具（2026-09-07）
//
// 背景：WMS 侧格口号使用"前缀+补零"编码（默认 前缀"22"+3位：格口号 5 → "22005"）。
//   只在「对 WMS 的报文/接口边界」做转换；系统内部（PLC 反馈 3 位 key、
//   绑定 key、数据库、UI 面板）一律保持原格口号，避免历史数据/PLC 协议错乱。
//
// 应用点：
//   · 出参：H7 满箱回传 detailList[].num = gridToWmsCode(格口号)
//   · 入参：H4 下发 items[].gridNum、H6 绑定 latticehole = parseWmsGridCodeToInt/Str()
//     （兼容带前缀 "22005" 与旧格式 "5"/"05"/"005"）
//
// 配置：config/http_server.xml → <gridCodePrefix>(默认"22") / <gridCodeWidth>(默认3)
//   前缀留空 = 关闭转换（出参仅补零、入参不剥离），可回退现网行为
// ============================================================================
#include <QString>
#include "ConfigManager.h"
#include "define.h"        // GRID_KEY_PADDING / BINDING_SLOT_COUNT

// 内部格口号(int) → WMS 格口编码（5 → "22005"；prefix 空 → 仅补零 "005"）
static inline QString gridToWmsCode(int grid)
{
    const AppConfig& cfg = ConfigManager::instance()->config();
    int width = cfg.gridCodeWidth > 0 ? cfg.gridCodeWidth : GRID_KEY_PADDING;
    return cfg.gridCodePrefix + QString("%1").arg(grid, width, 10, QChar('0'));
}

// 内部格口号字符串（"5"/"05"/"005"）→ WMS 格口编码；非数字（防御）原样返回
static inline QString gridToWmsCode(const QString& gridStr)
{
    bool ok = false;
    int grid = gridStr.trimmed().toInt(&ok);
    if (!ok) return gridStr;
    return gridToWmsCode(grid);
}

// WMS 入参格口号 → 内部格口号数字
//   兼容三种写法：带前缀 "22005"、"22005"→5；裸数字 "5"；补零 "005"
//   特殊：前缀恰为数字时（如 prefix="22" 且 WMS 发裸 "22"），回退整体解析；
//   非法输入返回 -1（由调用方按原有校验逻辑处理）
static inline int parseWmsGridCodeToInt(const QString& code)
{
    const AppConfig& cfg = ConfigManager::instance()->config();
    QString s = code.trimmed();
    if (s.isEmpty()) return -1;

    if (!cfg.gridCodePrefix.isEmpty() && s.startsWith(cfg.gridCodePrefix))
    {
        QString rest = s.mid(cfg.gridCodePrefix.length());
        bool ok = false;
        if (!rest.isEmpty())
        {
            int grid = rest.toInt(&ok);
            if (ok) return grid;        // "22005" → 5
        }
        // rest 为空或非数字（如裸 "22" 恰等于前缀）：回退整体解析
    }
    bool ok = false;
    int grid = s.toInt(&ok);
    return ok ? grid : -1;
}

// WMS 入参格口号 → 内部 3 位 key（"22005"/"5" → "005"）；非法返回空串
static inline QString parseWmsGridCodeToStr(const QString& code)
{
    int grid = parseWmsGridCodeToInt(code);
    if (grid < 0) return QString();
    return QString("%1").arg(grid, GRID_KEY_PADDING, 10, QChar('0'));
}

// ============================================================================
// ★ 2026-09-11 输入归一（查询/人工输入统一入口，全系统单一实现）
//   目的：用户输入 "7" / "007" / "22007"（WMS 编码 "22"+3位）等任意写法，
//   一律在内部统一成同一个内部格口 key（"007"）后再去查询/匹配，
//   UI 表现与查询结果完全一致（不因输入写法不同而不同）。
//
//   规则：
//     ① 先去配置前缀（<gridCodePrefix>，默认 "22"）："22007" → 7
//     ② 裸数字/补零："7" / "007" → 7
//     ③ 兜底（前缀未配置或配置不符）：结果越界（不在 1..BINDING_SLOT_COUNT）时，
//        取末位 width 位重解析："22007" → "007" → 7（格口实际只有 1..66，不会误判）
//     ④ 非数字输入（脏数据）原样返回，便于排查；空输入返回空
//   返回：内部 width 位 key（默认 3 位，如 "007"）
// ============================================================================
static inline QString normalizeGridKey(const QString& input)
{
    const AppConfig& cfg = ConfigManager::instance()->config();
    const int width = cfg.gridCodeWidth > 0 ? cfg.gridCodeWidth : GRID_KEY_PADDING;

    const QString s = input.trimmed();
    if (s.isEmpty()) return s;
    if (!s.at(0).isDigit()) return s;              // 非数字：原样（由调用方按原逻辑处理）

    bool allDigits = true;
    for (const QChar& c : s) { if (!c.isDigit()) { allDigits = false; break; } }
    if (!allDigits) return s;                       // 含非数字字符：原样

    int grid = parseWmsGridCodeToInt(s);            // ① + ②

    // ③ 兜底：越界 → 尝试末位 width 位（覆盖前缀未配置/前缀写法不同）
    if (grid < 1 || grid > BINDING_SLOT_COUNT)
    {
        if (s.length() > width)
        {
            bool ok = false;
            const int tail = s.right(width).toInt(&ok);
            if (ok && tail >= 1 && tail <= BINDING_SLOT_COUNT)
                grid = tail;
        }
    }

    if (grid < 0) return s;
    return QString("%1").arg(grid, width, 10, QChar('0'));
}
