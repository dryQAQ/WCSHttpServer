#ifndef _HTOOLBUTTON_H_
#define _HTOOLBUTTON_H_

#include "hcontrol_global.h"
#include <QToolButton>


class QPixmap;
class QLabel;
class QRect;


class HCONTROL_EXPORT HToolButton: public QToolButton
{
	Q_OBJECT

signals:
	void pressed();

public:
	HToolButton(
		QString normalImagePath,
		QString hoverImagePath,
		QString pressedImagePath,
		QString disableImagePath,
		QWidget* parent = Q_NULLPTR);
	~HToolButton();

	void setButtonEnabled(bool isEnabled);

protected:

	// 尺寸变化
	void resizeEvent(QResizeEvent* evnet);

	// 鼠标进入事件
	void enterEvent(QEvent* event);

	// 鼠标离开事件
	void leaveEvent(QEvent* event);

	// 鼠标点击事件
	void mousePressEvent(QMouseEvent *event);

	// 鼠标松开事件
	void mouseReleaseEvent(QMouseEvent *event);

private:

	void flushImage();


private:
	QPixmap      m_normalImage;
	QPixmap      m_hoverImage;
	QPixmap      m_pressedImage;
	QPixmap      m_disabledImage;
	QLabel*      m_pLabImage;
	HTOOLBUTTON_STATUS m_enToolButtonStatus;
};



#endif