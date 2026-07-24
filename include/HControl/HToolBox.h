#ifndef _HTOOLBOX_H_
#define _HTOOLBOX_H_

#include "hcontrol_global.h"
#include <QWidget>

#define MANGELIST_ITEM_WIDTH    188
#define MANGELIST_ITEM_HEIGHT   32
#define MANGE_BUTTON_WIDTH      20
#define MANGE_BUTTON_HEIGHT     MANGE_BUTTON_WIDTH
#define TOOLBOX_FIXWIDTH        200

class HEventFilter : public QObject
{
	Q_OBJECT
public:
	HEventFilter(QObject* parent = Q_NULLPTR);

protected:
	virtual bool eventFilter(QObject* watched, QEvent* event);

signals:
	void mouseEnter();
	void mouseLeave();
};

class QLabel;
class HListItemWidget : public QWidget
{
	Q_OBJECT

public:
	HListItemWidget(const QString icon1, const QString icon2, const QString itemName, QWidget* parent = Q_NULLPTR);

	void setSelected(bool selected);
signals:
	void mouseEnter();
	void mouseLeave();

protected:
	virtual void enterEvent(QEvent* event);
	virtual void leaveEvent(QEvent* event);
	virtual void paintEvent(QPaintEvent* event);

private:
	QLabel*          m_pIconLabel;
	QLabel*          m_pNameLabel;
	QLabel*          m_pPlayStatusLabel;
	bool             m_bSelected;
};

class QPushButton;
class HPopupWidgetButton;
class HOpenPlayListWidget : public QWidget
{
	Q_OBJECT

public:
	HOpenPlayListWidget(const QString title, bool canCreatePlayList, QWidget* parent);

signals:
	void showList();
	void hideList();
private:
	QPushButton*           m_pBtnTitle;
	HPopupWidgetButton*    m_pBtnCreatePlayList;
	QPushButton*           m_pBtnOpenList;
};

class QVBoxLayout;
class QListWidget;
class QListWidgetItem;
class HCONTROL_EXPORT HToolBox : public QWidget
{
	Q_OBJECT

public:
	explicit HToolBox(const QString boxTitle, bool isPlayList = false, QWidget* parent = Q_NULLPTR);
	~HToolBox();

	// 设置是否可收缩
	void setShrinkable(bool shrinkable);

	// 新增项
	void addItem(const QString itemName, const QString itemIcon1, const QString itemIcon2 = "");

	// 获取标题控件宽
	int titleWidgetWidth() const;

	// 获取列表控件宽度
	int listWidgetWidth() const;

	// 设置被选中的
	static void setItemSelected(int index);

signals:
	void ItemPressed(const int index);

private:
	bool               m_shrinkable;
	QString            m_strText;
	QVBoxLayout*       m_pMainLayout;
	QListWidget*       m_pListWidget;
	QWidget*           m_pTitleWidget;
	QList<QListWidgetItem*> m_localListItems;
	static QHash<QListWidgetItem*, HListItemWidget*> m_pListWidgets;
	static QList<QListWidgetItem*> m_pListItems;

};


#endif