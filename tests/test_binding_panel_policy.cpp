// ============================================================================
// test_binding_panel_policy.cpp — ★ 2026-09-16 现场需求②⑦（可执行自测）
//
// 需求（客户口径）：
//   ② 格口绑定面板：**隐藏 66 格口的状态显示**（66=配置的物理异常口 exceptionGrid）
//   ⑦ 用整格背景色表示绑定状态：绿=已绑定 / 橙=满箱锁格 / 红=已解锁·待重绑 / 灰=未绑定
//      并去掉状态圆点、格口号与箱号文字改白字。
//
// 本测试锁定"状态判定 + 配色 + 隐藏 + 计数"的口径（纯逻辑，不依赖 GUI）：
//   ① 四态映射到四色，与客户指定色值逐字一致
//   ② 优先级：锁格 > 待重绑 > 已绑定 > 未绑定（同格多状态并存时取高优先）
//   ③ 格口面板文案：未绑定 "--"，锁格/待重绑在无箱号时显示对应文字
//   ④ 异常口（exceptionGrid=66）整格隐藏、不计数；未配置（空/"0"）时不隐藏（零回归）
//   ⑤ 计数不变量：已绑定 + 已锁格 + 未绑定 = 可见格口数（隐藏后 65；未配置时 66）
//
// 构建：见 tests\run_tests.bat
// ============================================================================

#include <QCoreApplication>
#include <cstdio>

#include "BindingPanelPolicy.h"
#include "define.h"          // BINDING_SLOT_COUNT = 66

static int g_pass = 0;
static int g_fail = 0;

static void check(bool ok, const QString& what, const QString& got = QString())
{
    if (ok) { ++g_pass; std::printf("  [PASS] %s\n", what.toUtf8().constData()); }
    else
    {
        ++g_fail;
        std::printf("  [FAIL] %s%s\n", what.toUtf8().constData(),
                    got.isEmpty() ? "" : ("  <- 实际: " + got).toUtf8().constData());
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    std::printf("== ① 配色映射（客户指定色值，逐字一致）==\n");
    {
        check(bpColorOf(BP_BOUND)   == "#26A96C", QString("已绑定 → 绿 #26A96C"), bpColorOf(BP_BOUND));
        check(bpColorOf(BP_LOCKED)  == "#F37021", QString("满箱锁格 → 橙 #F37021"), bpColorOf(BP_LOCKED));
        check(bpColorOf(BP_REBIND)  == "#994444", QString("已解锁·待重绑 → 红 #994444"), bpColorOf(BP_REBIND));
        check(bpColorOf(BP_UNBOUND).isEmpty(),
              QString("未绑定 → **不设颜色**（无色 = 默认状态）"),
              bpColorOf(BP_UNBOUND).isEmpty() ? QString("(空)") : bpColorOf(BP_UNBOUND));

        // 有色态：底色必须落在**整格外框**（QFrame）上，而不是只改小圆点
        const QString fs = bpFrameStyle(BP_BOUND);
        check(fs.contains("QFrame") && fs.contains("#26A96C") && fs.contains("background"),
              QString("有色态样式作用于整格外框且含状态底色：%1").arg(fs), fs);

        // ★ 未绑定态：样式串里**不得出现 background**（无色 → 面板默认底色透出）
        const QString fu = bpFrameStyle(BP_UNBOUND);
        check(!fu.contains("background"),
              QString("未绑定态样式**不含 background**（无色，用面板默认底色）：%1").arg(fu), fu);
        check(fu.contains("QFrame") && fu.contains("border"),
              QString("未绑定态仍保留外框边框（格子边界可见）：%1").arg(fu), fu);
        check(fu != fs && fu != bpFrameStyle(BP_LOCKED) && fu != bpFrameStyle(BP_REBIND),
              QString("未绑定态样式与其它三态均不相同"));

        // ★ 旧灰 #7A7A7A 不得残留（本次口径：未绑定不设色）
        //   注：这里只扫"绑定格口样式串"；波次状态面板的灰（空闲/终态）是另一套口径，不受影响
        const QString allStyles = fs + bpFrameStyle(BP_LOCKED) + bpFrameStyle(BP_REBIND) + fu;
        check(!allStyles.contains("#7A7A7A"),
              QString("格口四态样式中不再出现旧灰 #7A7A7A（未绑定已改为无色）"));

        // ★ 文字色：有色底白字；无色底深灰字（否则浅底白字看不见）
        check(bpBoxTextColorOf(BP_BOUND)  == "#FFFFFF" &&
              bpBoxTextColorOf(BP_LOCKED) == "#FFFFFF" &&
              bpBoxTextColorOf(BP_REBIND) == "#FFFFFF",
              QString("绿/橙/红底 → 白字"));
        check(bpBoxTextColorOf(BP_UNBOUND) == "#333333",
              QString("未绑定（无色底）→ 深灰字 #333333（保证可读）"),
              bpBoxTextColorOf(BP_UNBOUND));
    }

    std::printf("== ② 状态优先级：锁格 > 待重绑 > 已绑定 > 未绑定 ==\n");
    {
        // 锁格优先于一切
        check(bpStateOf("H-01-0001", true,  true)  == BP_LOCKED,
              QString("锁格 + 待重绑 → 取锁格（橙）"));
        check(bpStateOf("H-01-0001", true,  false) == BP_LOCKED,
              QString("锁格 + 已绑定 → 取锁格（橙）"));
        // 待重绑优先于已绑定
        check(bpStateOf("H-01-0001", false, true)  == BP_REBIND,
              QString("待重绑 + 已绑定 → 取待重绑（红）"));
        // 已绑定
        check(bpStateOf("H-01-0001", false, false) == BP_BOUND,
              QString("有容器且未锁未禁 → 已绑定（绿）"));
        // 未绑定（含"锁格但无容器"仍按锁格展示，便于现场发现空锁格）
        check(bpStateOf("",          false, false) == BP_UNBOUND,
              QString("无容器未锁未禁 → 未绑定（无色/默认底色）"));
        check(bpStateOf("",          true,  false) == BP_LOCKED,
              QString("无容器但物理锁格 → 仍报锁格（橙）"));
    }

    std::printf("== ③ 格口面板文案（箱号 / 状态文字）==\n");
    {
        check(bpBoxTextOf(BP_BOUND,  "H-01-2066") == "H-01-2066",
              QString("已绑定显示箱号"));
        check(bpBoxTextOf(BP_LOCKED, "H-01-2066") == "H-01-2066",
              QString("锁格有箱号时显示箱号（便于现场知道锁的是哪箱）"));
        check(bpBoxTextOf(BP_LOCKED, "") == QString::fromUtf8("锁格"),
              QString("锁格无箱号时显示「锁格」"));
        check(bpBoxTextOf(BP_REBIND, "") == QString::fromUtf8("待重绑"),
              QString("待重绑无箱号时显示「待重绑」"));
        check(bpBoxTextOf(BP_UNBOUND, "") == "--",
              QString("未绑定显示 --"));
    }

    std::printf("== ④ 隐藏 66 号（异常口）==\n");
    {
        check(bpIsGridHidden(66, "66"),  QString("exceptionGrid=66 → 66 号隐藏"));
        check(bpIsGridHidden(66, "066"), QString("exceptionGrid=066（补零写法）→ 66 号同样隐藏"));
        check(bpIsGridHidden(66, "22066"), QString("exceptionGrid=22066（WMS 编码写法）→ 66 号同样隐藏"));
        check(!bpIsGridHidden(65, "66"), QString("非异常口（65 号）不隐藏"));
        check(!bpIsGridHidden(66, ""),   QString("exceptionGrid 未配置（空）→ 不隐藏任何格口（零回归）"));
        check(!bpIsGridHidden(66, "0"),  QString("exceptionGrid=0（视为未配置）→ 不隐藏任何格口"));

        // 可见格口数
        check(bpVisibleSlotCount(BINDING_SLOT_COUNT, "66") == BINDING_SLOT_COUNT - 1,
              QString("配置异常口时可见格口 = %1 - 1 = %2").arg(BINDING_SLOT_COUNT).arg(BINDING_SLOT_COUNT - 1),
              QString::number(bpVisibleSlotCount(BINDING_SLOT_COUNT, "66")));
        check(bpVisibleSlotCount(BINDING_SLOT_COUNT, "") == BINDING_SLOT_COUNT,
              QString("未配置异常口时可见格口 = %1").arg(BINDING_SLOT_COUNT),
              QString::number(bpVisibleSlotCount(BINDING_SLOT_COUNT, "")));
    }

    std::printf("== ⑤ 计数不变量：已绑定 + 已锁格 + 未绑定 = 可见格口数 ==\n");
    {
        // 模拟一份现场快照：66 号有绑定（异常口，应被隐藏且不计数）
        struct G { int num; QString box; bool lock; bool dis; };
        QVector<G> grid;
        for (int i = 1; i <= BINDING_SLOT_COUNT; ++i)
            grid.append(G{i, QString(), false, false});
        grid[65].box = "H-01-2066";                 // 66 号：异常口，有容器 → 必须被隐藏且不计入
        grid[0].box  = "H-01-2001";                 // 1 号：已绑定
        grid[1].box  = "H-01-2002"; grid[1].lock = true;    // 2 号：满箱锁格
        grid[2].box  = "H-01-2003"; grid[2].dis  = true;    // 3 号：已解锁·待重绑

        const QString excCfg = "66";
        int bound = 0, locked = 0, unbound = 0;
        int visible = 0;
        for (const G& g : grid)
        {
            if (bpIsGridHidden(g.num, excCfg)) continue;   // 面板不渲染、不计数
            ++visible;
            const BindingPanelState st = bpStateOf(g.box, g.lock, g.dis);
            if (st == BP_UNBOUND) ++unbound; else ++bound;
            if (st == BP_LOCKED)  ++locked;
        }
        check(visible == BINDING_SLOT_COUNT - 1,
              QString("面板可见格口 %1 个（66 号异常口不渲染）").arg(visible), QString::number(visible));
        check(bound + unbound == visible,
              QString("已绑定(含锁格/待重绑) %1 + 未绑定 %2 = 可见 %3").arg(bound).arg(unbound).arg(visible),
              QString("%1+%2 vs %3").arg(bound).arg(unbound).arg(visible));
        check(bound == 3 && locked == 1 && unbound == visible - 3,
              QString("现场快照：已绑定 3（其中锁格 1）/ 未绑定 %1").arg(visible - 3),
              QString("bound=%1 locked=%2 unbound=%3").arg(bound).arg(locked).arg(unbound));
        check(bound + locked + unbound - locked == visible,
              QString("「已绑定 + 已锁格 + 未绑定」三数之和口径 = 可见格口数（锁格是已绑定的子集）"));

        // 未配置异常口时：66 号照常参与计数（行为与改造前一致）
        int visibleAll = 0;
        for (const G& g : grid)
            if (!bpIsGridHidden(g.num, "")) ++visibleAll;
        check(visibleAll == BINDING_SLOT_COUNT,
              QString("未配置异常口 → 66 个格口全部可见（%1）").arg(visibleAll), QString::number(visibleAll));
    }

    std::printf("== ⑥ 波次信息面板「状态」底色 + 白字（本期追加需求）==\n");
    {
        // 逐状态锁定底色（口径见 BindingPanelPolicy.h 顶部注释）
        check(bpWaveStatusColor(0)  == "#7A7A7A", QString("空闲(0) → 灰"), bpWaveStatusColor(0));
        check(bpWaveStatusColor(1)  == "#1E6FB8", QString("已下发(1) → 蓝"), bpWaveStatusColor(1));
        check(bpWaveStatusColor(2)  == "#2E7D50", QString("已绑定(2) → 绿（可开工）"), bpWaveStatusColor(2));
        check(bpWaveStatusColor(3)  == "#F37021", QString("分拣中(3) → 橙"), bpWaveStatusColor(3));
        check(bpWaveStatusColor(4)  == "#F37021", QString("满箱同步中(4) → 橙"), bpWaveStatusColor(4));
        check(bpWaveStatusColor(5)  == "#B3261E", QString("取消处理中(5) → 红（需关注）"), bpWaveStatusColor(5));
        check(bpWaveStatusColor(6)  == "#7A7A7A", QString("已取消(6) → 灰（终态）"), bpWaveStatusColor(6));
        check(bpWaveStatusColor(7)  == "#B3261E", QString("完结中(7) → 红（H8 未成功，会阻塞队列）"), bpWaveStatusColor(7));
        check(bpWaveStatusColor(8)  == "#7A7A7A", QString("已完成(8) → 灰（终态）"), bpWaveStatusColor(8));
        check(bpWaveStatusColor(9)  == "#B3261E", QString("异常挂起(9) → 红（需人工）"), bpWaveStatusColor(9));
        check(bpWaveStatusColor(99) == "#7A7A7A", QString("未知状态 → 灰（兜底）"), bpWaveStatusColor(99));

        // 样式串：必须同时含"白字"与"底色"，且做成了色块（圆角 + 内边距）
        const QString st = bpWaveStatusStyle(3);
        check(st.contains("#FFFFFF") && st.contains("#F37021") && st.contains("background-color")
                  && st.contains("border-radius"),
              QString("状态标签样式 = 白字 + 底色色块：%1").arg(st), st);
        check(st.contains("15px"),
              QString("状态标签字号与面板其它数值一致（15px）"), st);

        // 图例文本非空且覆盖五种色义（tooltip 用）
        const QString lg = bpWaveStatusLegend();
        check(!lg.isEmpty() && lg.contains(QString::fromUtf8("蓝")) && lg.contains(QString::fromUtf8("绿"))
                  && lg.contains(QString::fromUtf8("橙")) && lg.contains(QString::fromUtf8("红"))
                  && lg.contains(QString::fromUtf8("灰")),
              QString("状态图例覆盖 蓝/绿/橙/红/灰 五种色义"));
    }

    std::printf("== ⑦ 【2026-09-17 追加】波次列表「格口绑定」列：0 → 无绑定记录（红字）==\n");
    {
        // 现场问题："该波次下的格口绑定状态以及显示并不会随切回而回溯过来，有时还会清空"
        //   → 列表必须先能看出"这个波次到底有没有绑定记录"（= 切回能不能恢复出绑定）。
        check(bpWaveBindColumnText(0) == QString::fromUtf8("无绑定记录"),
              QString("0 格 → 「无绑定记录」"), bpWaveBindColumnText(0));
        check(bpWaveBindColumnText(66) == QString::fromUtf8("66 格"),
              QString("66 格 → 「66 格」"), bpWaveBindColumnText(66));
        check(bpWaveBindColumnText(-1) == QString::fromUtf8("无绑定记录"),
              QString("异常负值 → 同样按「无绑定记录」（不显示 -1 格）"), bpWaveBindColumnText(-1));
        check(bpWaveBindColumnColor(0) == "#D32F2F",
              QString("无绑定记录 → 红字（与「处理」列同为需关注红）"), bpWaveBindColumnColor(0));
        check(bpWaveBindColumnColor(3) == "#333333",
              QString("有绑定记录 → 深灰字（正常）"), bpWaveBindColumnColor(3));

        // tooltip：无绑定必须点明"切回恢复不出绑定"；有绑定必须给出"切回将恢复的格口数"
        const QString t0 = bpWaveBindColumnTooltip(0);
        check(t0.contains(QString::fromUtf8("没有绑定记录"))
                  && t0.contains(QString::fromUtf8("恢复不出"))
                  && t0.contains("wave_bind_audit.py"),
              QString("无绑定 tooltip：说明切回恢复不出 + 给出只读核查手段"));
        const QString t3 = bpWaveBindColumnTooltip(3);
        check(t3.contains(QString::fromUtf8("3 个格口"))
                  && t3.contains(QString::fromUtf8("本波次内")),
              QString("有绑定 tooltip：给出切回将恢复的格口数与口径"));

        // 弹窗口径说明：被更晚波次重新绑定数量 = 总数 - 确认数（不得为负）
        const QString td = bpWaveBindDetailTooltip(5, 2, 66);
        check(td.contains(QString::fromUtf8("被更晚波次重新绑定 = 3")),
              QString("弹窗说明：被更晚波次覆盖格口数 = 5-2 = 3"), td);
        const QString td2 = bpWaveBindDetailTooltip(2, 5, 0);
        check(td2.contains(QString::fromUtf8("= 0")),
              QString("确认数 ≥ 总数时不出现负数（兜底 0）"));
    }

    std::printf("\n===== 结果：通过 %d 项，失败 %d 项 =====\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}