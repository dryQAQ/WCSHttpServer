#ifndef _HLATESTCARD_H_
#define _HLATESTCARD_H_

#include "hcontrol_global.h"
#include <QWidget>

class HCONTROL_EXPORT HLatestCard: public QWidget
{
	Q_OBJECT
signals:
	void cardClicked(QString name);

	void cardDeleteClicked(QString name);
public:
	HLatestCard(QWidget* parent = Q_NULLPTR);

	// 设置标题
	void setTitle(const QString& title);

	// 获取标题
	QString getTitle();

	// 设置描述
	void setDescription(const QString& description);

	// 获取描述
	QString getDescription();

protected:
	void paintEvent(QPaintEvent* event);

	void enterEvent(QEvent *event);

	void leaveEvent(QEvent *event);

	void resizeEvent(QResizeEvent* event);

	void mousePressEvent(QMouseEvent *event);

private:
	QString        m_strTitle;
	QString        m_strDescription;

	// 位置
	QRect          m_recIcon;
	QRect          m_recTitle;
	QRect          m_recDesc;
	QRect          m_recDeleteIcon;

	QPixmap        m_pixDelete;
	bool           m_bHover;
};


#endif