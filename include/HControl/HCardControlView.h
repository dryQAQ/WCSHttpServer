#ifndef _HCARDCONTROLVIEW_H_
#define _HCARDCONTROLVIEW_H_

#include "hcontrol_global.h"
#include <QWidget>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QTabWidget>
#include <QHBoxLayout>
#include <QPushButton>


#define MAX_LASTESTCARD_NUM    8
#define MIN_LATESTCARD_WIDTH_GEAR_HIGH    400
#define LATESTCARD_WIDTH_GEAR_HIGH_THRED  1600
#define MIN_LATESTCARD_WIDTH_GEAR_MID     350
#define LATESTCARD_WIDTH_GEAR_MID_THRED   1000
#define MIN_LATESTCARD_WIDTH_GEAR_LOW     300
#define LATESTCARD_WIDTH_GEAR_LOW_THRED   700
#define MIN_LATESTCARD_WIDTH_GEAR_LOWEST  250

class HCard;
class HLatestCard;
class HCONTROL_EXPORT HCardControlView : public QWidget
{
	Q_OBJECT
signals:
	void buttonClicked();

	void cardClicked(QString name);

	void cardDeleteClicked(QString name);

public:
	HCardControlView(
		QString title,
		QString name,
		QString tabName,
		QWidget* parent = Q_NULLPTR);

	// 新增卡片
	void addCard(QString name, QString desc);

	// 删除卡片
	void removeCard(QString name);

	// 设置最近的卡片
	void setLatestCardList(QList<QString> lstCard);

	// 新增最近的卡片
	void addLastestCard(QString name);

	// 移除最近的卡片
	void removeLatestCard(QString name);

	// 刷新卡片
	void flushCards();

protected:
	void resizeEvent(QResizeEvent* event);

private:
	void updateLatestCards();

private:
	// 标题
	QRect          m_recTitle;
	QRect          m_recBtnNewCamera;

	// 布局
	QVBoxLayout*   m_pMainLayout;
	QScrollArea*   m_pScrollView;
	QWidget*       m_pWgtScrollContent;
	QVBoxLayout*   m_pScrollLayout;
	QWidget*       m_pWgtTitle;
	QTabWidget*    m_pTabSettings;

	QWidget*       m_pWgtTabCameras;
	QVBoxLayout*   m_pWgtTabCamerasLayout;
	QHBoxLayout*   m_pLatestCardsLayout;
	QVBoxLayout*   m_pCardsLayout;

	QLabel*        m_pLabTitle;
	QPushButton*   m_pBtnNewCamera;

	QList<HCard*>  m_lstCard;
	QList<QString> m_lstLatestCard;

};

#endif