#ifndef _HBASEDIALOG_H_
#define _HBASEDIALOG_H_

#include "hcontrol_global.h"
#include <QDialog>
#include <QWidget>
#include <QPushButton>
#include <QMouseEvent>

#define BASEDIALOG_BORDER_LINEWIDTH                1
#define BASEDIALOG_TOPBORDER_LINEWIDTH             2
#define BASEDIALOG_TITLE_HEIGHT                    38
#define BASEDIALOG_MENU_BAR_HEIGHT                 38
#define BASEDIALOG_SYSBUTTON_RIGHTMARGIN           10
#define BASEDIALOG_SYSBUTTON_WIDTH                 38
#define BASEDIALOG_SYSBUTTON_HEIGHT                BASEDIALOG_SYSBUTTON_WIDTH
#define BASEDIALOG_HEADER_X                        5
#define BASEDIALOG_HEADER_Y                        5

class HCONTROL_EXPORT HBaseDialog : public QDialog
{
	Q_OBJECT

public:
	HBaseDialog();
	virtual ~HBaseDialog();

	// 初始化
	bool init();

	// 获取中心区域
	QWidget* centralWidget();

	// 设置标题头
	void setHeaderText(QString header);


protected:
	// 尺寸变更事件
	void resizeEvent(QResizeEvent *event);

	// 绘制事件
	void paintEvent(QPaintEvent *event);

	// 鼠标按下事件
	void mousePressEvent(QMouseEvent *event);

	// 鼠标松开事件
	void mouseReleaseEvent(QMouseEvent *event);

	// 鼠标移动事件
	void mouseMoveEvent(QMouseEvent *event);

private:
	// 初始化控件
	bool initWidgets();

	// 初始化布局
	bool initLayouts();

	// 初始化信号槽
	bool initSignals();

	// 释放资源
	void deInit();

private slots:
    // 点击关闭按钮
    void onBtnCloseClick();

private:
	// 位置信息
	QRect          m_headerRect;
	QRect          m_headerTextRect;
	QRect          m_centralRect;
	QRect          m_btnCloseRect;

	// 子控件
	QWidget*       m_centralWidget;
	QPushButton*   m_btnClose;

	// 移动相关
	QPoint         m_stPress;
	bool           m_bLeftBtnClk;

};


#endif