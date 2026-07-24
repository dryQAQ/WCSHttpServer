#ifndef _HMETROBASE_H_
#define _HMETROBASE_H_

#include "hcontrol_global.h"
#include <QWidget>

class QVBoxLayout;
class QLabel;
class HCONTROL_EXPORT HMetroBase: public QWidget
{
	Q_OBJECT
signals:
	void clicked();
public:
	HMetroBase(QWidget* parent = Q_NULLPTR);
	~HMetroBase();

protected:
	void mousePressEvent(QMouseEvent *event);

	void enterEvent(QEvent *event);

	void leaveEvent(QEvent *event);

	void paintEvent(QPaintEvent* event);
};


#endif