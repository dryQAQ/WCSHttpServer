#ifndef _HMETRO_H_
#define _HMETRO_H_

#include "hcontrol_global.h"
#include <QWidget>

class QVBoxLayout;
class QLabel;
class HCONTROL_EXPORT HMetro: public QWidget
{
	Q_OBJECT
signals:
	void clicked();
public:
	HMetro(QWidget* parent = Q_NULLPTR);
	~HMetro();

	void setIcon(QPixmap pixmap);

	void setTitle(QString title);

	void setDetail(QString detail);

protected:
	void mousePressEvent(QMouseEvent *event);

	void enterEvent(QEvent *event);

	void leaveEvent(QEvent *event);

private:
	QVBoxLayout*     m_pMainLayout;
	QLabel*          m_pLabIcon;
	QLabel*          m_pLabTitle;
	QLabel*          m_pLabDetail;
};


#endif