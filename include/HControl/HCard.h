#ifndef _HCARD_H_
#define _HCARD_H_

#include "hcontrol_global.h"
#include <QWidget>

class HCONTROL_EXPORT HCard: public QWidget
{
	Q_OBJECT
signals:
	void cardClicked(QString name);

	void cardDeleteClicked(QString name);

public:
	HCard(QWidget* parent = Q_NULLPTR);

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
	// 删除按钮显示
	bool           m_bHovered;
};


#endif