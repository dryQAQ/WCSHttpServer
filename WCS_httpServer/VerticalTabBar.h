#pragma once
// ============================================================================
// VerticalTabBar.h — 左侧标签页的"中文竖排"标签栏（★ 2026-09-13 UI修正）
//
// 背景：
//   QTabWidget 置于左侧（QTabWidget::West）时，Qt 默认会把标签文字**整体旋转 90°**
//   贴到侧边。英文旋转后仍可顺读，中文被旋转后则是"拧着"的，现场反馈不可读。
//
// 解决：
//   本类只改写两件事——
//     ① tabSizeHint：按"逐字竖排"计算标签高度（N 个字 = N 行），
//        宽度仍沿用基类（受样式表 min-width 控制），因此标签栏几何、命中测试不变；
//     ② paintEvent：自己画标签底/边框/文字，文字按**每个字符一行**竖向居中排布，
//        不做任何旋转。中文即"从上到下逐字读"，符合中文竖排习惯。
//
// 说明：
//   · 未依赖任何私有 API，纯 Qt5 公开接口（QStyle::drawControl 取标签形状与调色）；
//   · 仅在 West/East 方向生效；若将来把标签页改回 North/South，自动回落到基类绘制；
//   · 标签宽度若不足以容纳（例如被压得很窄），自动退回"沿用基类"以保证可读性。
// ============================================================================

#include <QTabBar>
#include <QTabWidget>

class VerticalTabBar : public QTabBar
{
    Q_OBJECT
public:
    explicit VerticalTabBar(QWidget* parent = nullptr);

protected:
    QSize tabSizeHint(int index) const override;
    void  paintEvent(QPaintEvent* event) override;

private:
    // 是否处于"竖排"方向（West/East）
    bool isVerticalSide() const;

    // 逐字竖排后，标签文字需要的总高度（像素）
    int stackedTextHeight(int index) const;
};

// ============================================================================
// VerticalTabWidget — 仅用于把 VerticalTabBar 装进 QTabWidget
//   原因：QTabWidget::setTabBar() 是 protected，外部无法直接调用；
//   本类只提供一个公开的 installVerticalTabBar()，不改变 QTabWidget 任何行为，
//   因此 MainWindow 中已有的 m_tabMain 接口、样式表选择器等都不受影响。
// ============================================================================
class VerticalTabWidget : public QTabWidget
{
    Q_OBJECT
public:
    explicit VerticalTabWidget(QWidget* parent = nullptr)
        : QTabWidget(parent)
    {
        // 必须在 addTab 之前替换：QTabWidget 会按标签栏类型缓存尺寸
        setTabBar(new VerticalTabBar(this));
    }
};
