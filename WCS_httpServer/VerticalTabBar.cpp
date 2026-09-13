#include "VerticalTabBar.h"

#include <QPainter>
#include <QStylePainter>
#include <QStyleOptionTabBarBase>
#include <QFontMetrics>

// ── 外观常量（★ 2026-09-13 客户要求"各项之间区分明显一些"：改用高对比配色）──
//   选中：深蓝底 + 白字 + 左侧白色强调条；未选中：浅灰底 + 深灰字 + 清晰边框；
//   标签之间留间距（见 paintEvent 的 inset），避免连成一片看不清分界。
namespace {
const QColor kSelBg    (0x15, 0x65, 0xC0);   // 选中底：深蓝（与样式表 #1565C0 一致）
const QColor kSelFg    (0xFF, 0xFF, 0xFF);   // 选中字：白
const QColor kSelEdge  (0x0D, 0x47, 0xA1);   // 选中边框：更深蓝
const QColor kSelMark  (0xFF, 0xFF, 0xFF);   // 选中左侧强调条：白
const QColor kUnselBg  (0xEC, 0xEF, 0xF1);   // 未选中底：浅灰蓝
const QColor kUnselFg  (0x37, 0x47, 0x4F);   // 未选中字：深灰（提高对比度）
const QColor kUnselEdge(0xCF, 0xD8, 0xDC);   // 未选中边框：可见的浅灰
const QColor kBarBg    (0xE0, 0xE4, 0xE7);   // 标签栏留白底色（比未选中略深，突出分界）
const QColor kDisabled (0xAA, 0xAA, 0xAA);

// 标签内缩：上下各留 4px → 相邻标签之间出现 8px 间隙（区分更明显）
const int kTabInsetY = 4;
// 左侧强调条宽度（仅选中标签绘制）
const int kMarkW = 5;
}

VerticalTabBar::VerticalTabBar(QWidget* parent)
    : QTabBar(parent)
{
}

bool VerticalTabBar::isVerticalSide() const
{
    const QTabBar::Shape s = shape();
    return s == QTabBar::RoundedWest || s == QTabBar::TriangularWest ||
           s == QTabBar::RoundedEast || s == QTabBar::TriangularEast;
}

// ----------------------------------------------------------------------------
// 逐字竖排时的"单字实际高度"与"行距"
//   ★ 2026-09-13 修正：原来用 QFontMetrics::height()（≈1.4 倍字号）当单字高度，
//     8 个字的标签要 200+px，5 个标签合计 1100px > 窗口可用高度（约 900px）导致标签被压缩。
//     中文竖排只需"字面高度 + 小行距"，故改用 tightBoundingRect 取字面高度。
// ----------------------------------------------------------------------------
namespace {
// 取一个汉字（无则退回任意字符）的字面高度；异常时退回 height()
int tightCharHeight(const QFontMetrics& fm)
{
    QRect ink = fm.tightBoundingRect(QString::fromUtf8("国"));
    if (ink.height() <= 0)
        ink = fm.tightBoundingRect(QStringLiteral("A"));
    return ink.height() > 0 ? ink.height() : fm.height();
}
// 行距：字面高度的 1/5（竖排紧排，兼顾可读与紧凑）
int tightGap(int inkH) { return qMax(2, inkH / 5); }
}

// ----------------------------------------------------------------------------
// 逐字竖排后，标签文字需要的总高度（用紧凑行高，保证 5 个标签能放进窗口）
// ----------------------------------------------------------------------------
int VerticalTabBar::stackedTextHeight(int index) const
{
    const QString text = tabText(index);
    if (text.isEmpty())
        return 0;

    const QFontMetrics fm(font());
    const int inkH = tightCharHeight(fm);
    const int gap  = tightGap(inkH);
    return text.size() * inkH + qMax(0, text.size() - 1) * gap;
}

// ----------------------------------------------------------------------------
// 标签尺寸：宽度沿用基类（样式表 min-width 等约束都保留）；
//   ★ 2026-09-13 客户要求"各项之间的区分明显一些"：
//     高度统一取"最长标签所需高度"——5 个标签等高排列，边界整齐、分界清楚
//     （此前各标签按自身字数变高变矮，长短不一反而显得零乱）。
// ----------------------------------------------------------------------------
QSize VerticalTabBar::tabSizeHint(int index) const
{
    QSize sz = QTabBar::tabSizeHint(index);
    if (!isVerticalSide())
        return sz;

    // 所有标签统一高度 = 最长标签的竖排文字高度（+ 上下留白，供边框/间隙使用）
    int maxTextH = 0;
    for (int i = 0; i < count(); ++i)
        maxTextH = qMax(maxTextH, stackedTextHeight(i));

    const int needH = maxTextH + 2 * kTabInsetY + 8;
    if (sz.height() < needH)
        sz.setHeight(needH);
    return sz;
}

// ----------------------------------------------------------------------------
// 自绘标签：底/边框/文字全手绘，文字逐字竖向居中，不旋转。
// ----------------------------------------------------------------------------
void VerticalTabBar::paintEvent(QPaintEvent* event)
{
    // ── 非竖排方向（今后若改回 North/South）：完全沿用基类绘制 ──
    if (!isVerticalSide())
    {
        QTabBar::paintEvent(event);
        return;
    }

    // 与 tabSizeHint / stackedTextHeight 使用同一套紧凑行高，避免"量出来放得下、画出来被裁"
    const QFontMetrics fm(font());
    const int charH = qMax(1, tightCharHeight(fm));
    const int gap   = tightGap(charH);

    // ★ 空间自检：任一个标签装不下整列文字时，整体退回基类绘制
    //   （唯一一次调用，避免在绘制过程中重复绘制；正常情况不会走到这里，
    //    因为 tabSizeHint 已按竖排高度兜底）
    for (int i = 0; i < count(); ++i)
    {
        const QRect r = tabRect(i);
        if (!r.isValid())
            continue;
        const int n = tabText(i).size();
        if (n <= 0)
            continue;
        if (n * charH + (n - 1) * gap > r.height() + 2)
        {
            QTabBar::paintEvent(event);
            return;
        }
    }

    QStylePainter p(this);

    // 1) 标签栏底色（标签之间的间隙由此露出 → 分界清晰），与未选中标签形成层次
    p.fillRect(rect(), kBarBg);

    for (int i = 0; i < count(); ++i)
    {
        const QRect tabRectRaw = tabRect(i);
        if (!tabRectRaw.isValid())
            continue;
        // ★ 上下内缩，使相邻标签之间留出可见间隙（客户要求"每一项之间的区分明显一些"）
        const QRect r = tabRectRaw.adjusted(0, kTabInsetY, 0, -kTabInsetY);

        // ── 2) 标签底 + 边框（选中/未选中高对比）──
        const bool sel = (i == currentIndex());
        const bool en  = isTabEnabled(i);

        p.fillRect(r.adjusted(1, 1, -1, -1), sel ? kSelBg : kUnselBg);
        p.setPen(sel ? kSelEdge : kUnselEdge);
        p.drawRect(r.adjusted(0, 0, -1, -1));
        // 选中项：左侧强调条（一眼看出"当前在哪一页"）
        if (sel)
        {
            p.fillRect(QRect(r.left() + 1, r.top() + 1, kMarkW, r.height() - 1), kSelMark);
        }

        // ── 3) 逐字竖排文字 ──
        const QString text = tabText(i);
        if (text.isEmpty())
            continue;

        p.setPen(en ? (sel ? kSelFg : kUnselFg) : kDisabled);

        // 单字宽度上限：左右各留 6px（选中项左侧再让出强调条），保证字符主体完整且不越出标签
        const int leftPad  = sel ? (kMarkW + 4) : 6;
        const int maxCharW = qMax(1, r.width() - leftPad - 6);
        const int n        = text.size();
        const int totH     = n * charH + (n - 1) * gap;

        // 整列文字竖向居中（中文竖排阅读顺序：自上而下）
        int y = r.top() + qMax(0, (r.height() - totH) / 2);

        for (int k = 0; k < n; ++k)
        {
            const QString ch = text.mid(k, 1);
            const QRect cell(r.left() + leftPad, y, r.width() - leftPad - 6, charH);

            if (fm.horizontalAdvance(ch) <= maxCharW)
            {
                // 常见情况：单字放得下 → 水平居中单字
                p.drawText(cell, Qt::AlignHCenter | Qt::AlignVCenter, ch);
            }
            else
            {
                // 极端窄标签：把该字等比压到可用宽度内，保证"可见"而不是被裁掉
                p.save();
                p.setClipRect(cell);
                QFont f = font();
                const int need = qMax(1, fm.horizontalAdvance(ch));
                f.setPointSizeF(qMax(4.0, font().pointSizeF() * double(maxCharW) / double(need)));
                p.setFont(f);
                p.drawText(cell, Qt::AlignHCenter | Qt::AlignVCenter, ch);
                p.restore();
            }

            y += charH + gap;
        }
    }

    // 4) 标签栏基线（与页面 pane 边框视觉衔接）
    QStyleOptionTabBarBase baseOpt;
    baseOpt.initFrom(this);
    baseOpt.rect         = rect();
    baseOpt.shape        = shape();
    baseOpt.tabBarRect   = rect();
    baseOpt.documentMode = false;
    p.drawPrimitive(QStyle::PE_FrameTabBarBase, baseOpt);
}
