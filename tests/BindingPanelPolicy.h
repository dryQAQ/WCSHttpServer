#pragma once
// ============================================================================
// BindingPanelPolicy.h — 容器绑定面板「状态判定 + 整格底色」的**纯逻辑口径**
//
// ★ 2026-09-16 现场需求②⑦：把面板的状态判定/配色/隐藏规则从 UI 代码里抽成可测的纯函数，
//   使 tests/test_binding_panel_policy.cpp 能对"配色映射、优先级、隐藏格、计数口径、文字色"
//   逐条断言（GUI 不便单测，但这条口径必须被锁定，否则现场底色与状态会悄悄漂移）。
//
// 使用方：
//   · 产品侧 MainWindow::updateBindingPanel() 与 setupUI() 建格循环（判空后隐藏异常口）；
//   · 测试侧 tests/test_binding_panel_policy.cpp。
//
// 口径（客户指定，★ 2026-09-16 更新）：
//   已绑定（有容器，未锁格未禁用）→ 绿 #26A96C（底色）+ 白字
//   满箱锁格（S7 物理锁格位）      → 橙 #F37021（底色）+ 白字
//   已解锁·待重绑（等 WMS 发 H6）  → 红 #994444（底色）+ 白字
//   未绑定                        → **不设颜色**（无色 = 默认状态，面板底色透出）+ 深灰字
//   优先级：锁格 > 待重绑 > 已绑定 > 未绑定
//   隐藏格：配置的物理异常口（exceptionGrid，现场=66）整格不渲染、不计数。
//
// ★ 2026-09-17 事故留痕：本文件在 17:05 被一次误操作（批处理命令行重定向）**截断为 0 字节**。
//   已按"编译产物 + 自测断言"逐条还原：`.build_check/test_binding_panel_policy.obj` 的符号表
//   给出全部函数签名，其 .rdata 字面量给出配色/文案/图例/tooltip 原文；
//   tests/test_binding_panel_policy.cpp 的断言锁定优先级与取值。
//   函数签名、返回文案、配色映射与优先级**与截断前一致**；注释按现场口径重写。
//   回归入口：tests\run_tests.bat 第 7 项（58 项断言）。
// ============================================================================

#include <QString>
#include "WmsGridCode.h"   // normalizeGridKey（"66"/"066"/"22066" 统一归一）

// ──── 四色常量（与产品侧 MainWindow.h 的宏保持一致，测试会断言字符串相等）────
//   ★ 2026-09-16 客户口径更新：绿改 #26A96C；**未绑定不设背景色**（无色 = 默认状态）——
//     因此未绑定的"色值"为空串，样式串里不出现 background，面板默认底色直接透出。
#define BP_COLOR_BOUND      "#26A96C"   // 绿：已绑定（★ 本次由 #4A6C5D 更新为 #26A96C）
#define BP_COLOR_LOCKED     "#F37021"   // 橙：满箱锁格
#define BP_COLOR_REBIND     "#994444"   // 红：已解锁·待重绑
#define BP_COLOR_UNBOUND    ""          // 未绑定：**无色**（不设背景，用面板默认底色）

// 未绑定态的文字色：浅色/无底色上必须用深色字，否则白字看不见
#define BP_TEXT_COLOR_ON_COLORED "#FFFFFF"   // 有底色（绿/橙/红）→ 白字
#define BP_TEXT_COLOR_PLAIN      "#333333"   // 无色（未绑定）→ 深灰字

// ──── 面板状态（互斥，四态）────
enum BindingPanelState
{
    BP_UNBOUND = 0,   // 未绑定
    BP_BOUND   = 1,   // 已绑定（有容器，未锁格未禁用）
    BP_LOCKED  = 2,   // 满箱锁格（S7 物理锁格位）
    BP_REBIND  = 3,   // 已解锁·待重绑（等 WMS 重发 H6）
};

// ============================================================================
// ① 状态判定（优先级：锁格 > 待重绑 > 已绑定 > 未绑定）
//   box     : 该格口当前容器号（空 = 无容器）
//   locked  : PLC 物理锁格中（S7 锁格位 DB77）
//   disabled: 已物理解锁但 WCS 仍禁用该格（等 WMS 重发 H6 绑定）
//   注：物理锁格**优先于一切**（无容器也报锁格，便于现场发现"空锁格"）；
//      "已解锁·待重绑"只看禁用标志（此时旧箱已归档解绑，箱号通常为空）。
// ============================================================================
static inline BindingPanelState bpStateOf(const QString& box, bool locked, bool disabled)
{
    if (locked)         return BP_LOCKED;
    if (disabled)       return BP_REBIND;
    if (!box.isEmpty()) return BP_BOUND;
    return BP_UNBOUND;
}

// ──── ② 状态 → 整格底色（未绑定 = 空串，表示"不设背景"）────
static inline QString bpColorOf(BindingPanelState state)
{
    switch (state)
    {
    case BP_BOUND:  return BP_COLOR_BOUND;
    case BP_LOCKED: return BP_COLOR_LOCKED;
    case BP_REBIND: return BP_COLOR_REBIND;
    default:        return BP_COLOR_UNBOUND;
    }
}

// ──── ③ 状态 → 格内文字色（有色底白字；无色底深灰字）────
static inline QString bpBoxTextColorOf(BindingPanelState state)
{
    const QString color = bpColorOf(state);
    return color.isEmpty() ? QString(BP_TEXT_COLOR_PLAIN) : QString(BP_TEXT_COLOR_ON_COLORED);
}

// ──── ④ 状态 → 整格外框 QSS（底色落在外框上 = 整格底色）────
//   未绑定：**不含 background**（无色，面板默认底色透出），仅保留边框让格子边界可见。
static inline QString bpFrameStyle(BindingPanelState state)
{
    const QString color = bpColorOf(state);
    if (color.isEmpty())
        return QString("QFrame { border: 1px solid #ddd; border-radius: 2px; }");
    return QString("QFrame { background: %1; border: 1px solid #ddd; border-radius: 2px; }")
        .arg(color);
}

// ──── ⑤ 状态 → 箱号/状态文字 ────
//   已绑定：箱号；锁格/待重绑：有箱号显示箱号（便于现场知道锁的是哪箱），无箱号显示状态词；
//   未绑定：`--`。
static inline QString bpBoxTextOf(BindingPanelState state, const QString& box)
{
    switch (state)
    {
    case BP_BOUND:
        return box;
    case BP_LOCKED:
        return box.isEmpty() ? QString::fromUtf8("锁格") : box;
    case BP_REBIND:
        return box.isEmpty() ? QString::fromUtf8("待重绑") : box;
    default:
        return QString("--");
    }
}

// ============================================================================
// ⑥ 异常口隐藏（★ 2026-09-16 需求②）
//   异常口（默认 66）只收超计划件、不上传 WMS、也不参与产品计划，
//   其绑定状态对操作员没有意义 → 整格不渲染、不计数。
//   exceptionGrid 支持 "66"/"066"/"22066" 任意写法（与全系统同用 normalizeGridKey 归一）；
//   填空 / "0" = 未配置 → 不隐藏任何格口（与改造前一致，零回归）。
// ============================================================================
static inline bool bpIsGridHidden(int gridNum, const QString& exceptionGrid)
{
    const QString cfg = exceptionGrid.trimmed();
    if (cfg.isEmpty() || cfg == "0") return false;
    return normalizeGridKey(QString::number(gridNum)) == normalizeGridKey(cfg);
}

// ──── ⑦ 可见格口数（计数基数：隐藏异常口后 66 → 65；未配置时 66）────
static inline int bpVisibleSlotCount(int slotCount, const QString& exceptionGrid)
{
    int visible = 0;
    for (int grid = 1; grid <= slotCount; ++grid)
    {
        if (!bpIsGridHidden(grid, exceptionGrid)) ++visible;
    }
    return visible;
}

// ============================================================================
// ⑧ 波次信息面板「状态」标签：底色 + 白字（★ 2026-09-16 本期追加需求）
//   色义（与 UI 图例一致）：
//     蓝 = 已下发（等 WMS 下发容器绑定 H6）
//     绿 = 已绑定（可点「开始分拣」）
//     橙 = 分拣中 / 满箱同步中
//     红 = 完结中 / 取消处理中 / 异常挂起（需人工关注；完结中与挂起会阻塞待执行队列）
//     灰 = 空闲 / 已取消 / 已完成（终态）
// ============================================================================
static inline QString bpWaveStatusColor(int waveStatus)
{
    switch (waveStatus)
    {
    case 1:  return "#1E6FB8";   // 已下发
    case 2:  return "#2E7D50";   // 已绑定（可开工）
    case 3:  return "#F37021";   // 分拣中
    case 4:  return "#F37021";   // 满箱同步中
    case 5:  return "#B3261E";   // 取消处理中（需关注）
    case 7:  return "#B3261E";   // 完结中（H8 未成功，会阻塞待执行队列）
    case 9:  return "#B3261E";   // 异常挂起（需人工）
    default: return "#7A7A7A";   // 空闲(0) / 已取消(6) / 已完成(8) / 未知 → 灰（终态或兜底）
    }
}

// 状态标签 QSS：白字 + 底色色块（字号与面板其它数值一致 = 15px）
static inline QString bpWaveStatusStyle(int waveStatus)
{
    return QString("font-size: 15px; font-weight: bold; color: #FFFFFF;"
                   " background-color: %1; border-radius: 4px; padding: 1px 8px;")
        .arg(bpWaveStatusColor(waveStatus));
}

// 状态色义图例（状态标签 tooltip 用；覆盖蓝/绿/橙/红/灰五种色义）
static inline QString bpWaveStatusLegend()
{
    return QString::fromUtf8(
        "状态底色口径：\n"
        "  蓝 = 已下发（等 WMS 下发容器绑定 H6）\n"
        "  绿 = 已绑定（可点「开始分拣」）\n"
        "  橙 = 分拣中 / 满箱同步中\n"
        "  红 = 完结中 / 取消处理中 / 异常挂起（需人工关注；完结中与挂起会阻塞待执行队列）\n"
        "  灰 = 空闲 / 已取消 / 已完成（终态）");
}

// ============================================================================
// ⑨ 波次列表「格口绑定」列（★ 2026-09-17 追加）
//   现场问题："该波次下的格口绑定状态以及显示并不会随切回而回溯过来，有时还会清空"
//   → 列表必须先能看出"这个波次到底有没有绑定记录"（= 切回能不能恢复出绑定）。
//   ownCnt = 按"本波次内每格最后一条绑定"取到的格口数（与切回取数、与「查看绑定」同一 SQL）。
// ============================================================================
static inline QString bpWaveBindColumnText(int ownCnt)
{
    if (ownCnt <= 0) return QString::fromUtf8("无绑定记录");
    return QString::fromUtf8("%1 格").arg(ownCnt);
}

// 0/负值（无记录）→ 红字（与「处理」列同为需关注红）；有记录 → 深灰字（正常）
static inline QString bpWaveBindColumnColor(int ownCnt)
{
    return (ownCnt <= 0) ? QString("#D32F2F") : QString("#333333");
}

// 单元格 tooltip：无记录必须点明"切回恢复不出绑定"并给出只读核查手段；
// 有记录必须给出"切回将恢复的格口数"与口径（同一 SQL，避免数字对不上）。
static inline QString bpWaveBindColumnTooltip(int ownCnt)
{
    if (ownCnt <= 0)
    {
        return QString::fromUtf8(
            "本波次**没有绑定记录**（H6 归属落空 / 从未收到 H6）：\n"
            "  · 切回本波次时**恢复不出**该波次的绑定；\n"
            "  · 若库中仍有当前活跃绑定，仅按「物理当前绑定」显示（不是本波次记录）；\n"
            "  · 处置：等 WMS 重发 H6，或用 docs/wave_bind_audit.py 只读核查历史归属。");
    }
    return QString::fromUtf8(
        "本波次绑定口径（切回时恢复什么）：\n"
        "  切回将恢复 %1 个格口（每格取**本波次内**最后一条绑定）；\n"
        "  列表数字 = 切回时会恢复的格口数（与切回取数、与「查看绑定」弹窗同一 SQL）。")
        .arg(ownCnt);
}

// 「查看绑定」弹窗表头 tooltip：说明"被更晚波次重新绑定"的覆盖数与恢复口径
//   total      : 本波次内有绑定记录的格口总数
//   confirmed  : 其中"该格全表最后一条仍属本波次"的格口数
//   activeBinds: 当前 DB 活跃绑定数（现场物理生效的绑定）
//   覆盖数 = total - confirmed（不得为负 → 兜底 0）
static inline QString bpWaveBindDetailTooltip(int total, int confirmed, int activeBinds)
{
    QString head = QString::fromUtf8("当前 DB 活跃绑定：%1 个\n").arg(activeBinds);
    if (total <= 0)
        return head + bpWaveBindColumnTooltip(0);

    const int covered = (total > confirmed) ? (total - confirmed) : 0;
    head += QString::fromUtf8("共 %1 个格口（该波次内每格最后一条绑定）：\n").arg(total);
    head += QString::fromUtf8(
                "  其中该格「全表最后一条仍属本波次」= %2 个，被更晚波次重新绑定 = %3 个；\n"
                "  被更晚波次覆盖的格口，恢复的是**本波次最后绑过的那口箱**（不是现场当前箱）。")
                .arg(confirmed).arg(covered);
    return head;
}
