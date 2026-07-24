#ifndef _HPOPUPWIDGETBUTTON_H_
#define _HPOPUPWIDGETBUTTON_H_

#include "hcontrol_global.h"
#include <QWidget>

//namespace PWB
//{
//	enum WidgetOrientation
//	{
//		Horizontal,
//		Vertical,
//	};
//}
//
//class QPushButton;
//class PopupWidget;
//
//class HCONTROL_EXPORT HPopupWidgetButton : public QWidget
//{
//	Q_OBJECT
//
//public:
//	explicit HPopupWidgetButton(PWB::WidgetOrientation orient, QWidget* parent = Q_NULLPTR);
//	~HPopupWidgetButton();
//
//	// 设置弹出的widget
//	void setMainWidget(QWidget* widget);
//
//	// 设置按钮objectname
//	void setButtonObjectName(const QString& name);
//
//	inline QPushButton* button()
//	{
//		return m_pButton;
//	}
//
//protected:
//	virtual bool eventFilter(QObject* watched, QEvent* event);
//
//signals:
//	void buttonClicked();
//	void othersClicked();
//
//public slots:
//    void hideWidget();
//private:
//	QPushButton*            m_pButton;
//	PWB::WidgetOrientation  m_enOrientaion;
//	PopupWidget*            m_pMainWidget;
//	bool                    m_bMainWidgetClicked;
//	static QList<PopupWidget*> m_pWidgets;
//};



#endif