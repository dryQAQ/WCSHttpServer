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
