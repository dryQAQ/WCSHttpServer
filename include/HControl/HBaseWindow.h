#ifndef _HBASEWINDOW_H_
#define _HBASEWINDOW_H_

#include "hcontrol_global.h"
#include <QWidget>


#define BASEWINDOW_TITLE_HEIGHT       85
#define BASEWINDOW_MENU_BAR_HEIGHT    40
#define BASEWINDOW_TOOL_BAR_HEIGHT    40
#define BASEWINDOW_SYSBUTTON_RIGHTMARGIN    10
#define BASEWINDOW_SYSBUTTON_WIDTH    40
#define BASEWINDOW_SYSBUTTON_HEIGHT   BASEWINDOW_SYSBUTTON_WIDTH
#define BASWWINDOW_TOPBORDER_LINEWIDTH 2
#define BASEWINDOW_BORDER_LINEWIDTH    1
#define BASWWINDOW_MENULEFT            100
#define BASEWINDOW_TOOLLEFT            100
#define BASEWINDOW_TOOL_TOPMARGIN      2
#define BASEWINDOW_MENUTOOLSEP_LINEWIDTH 425
#define BASEWINDOW_LOGINBUTTON_WIDTH   80
#define BASEWINDOW_LOGO_MARGIN         10

typedef enum _RESIZE_DIR_ {
	RESIZE_DIR_NODIR,
	RESIZE_DIR_TOP = 0x01,
	RESIZE_DIR_BOTTOM = 0x02,
	RESIZE_DIR_LEFT = 0x04,
	RESIZE_DIR_RIGHT = 0x08,
	RESIZE_DIR_TOPLEFT = 0x01 | 0x04,
	RESIZE_DIR_TOPRIGHT = 0x01 | 0x08,
	RESIZE_DIR_BOTTOMLEFT = 0x02 | 0x04,
	RESIZE_DIR_BOTTOMRIGHT = 0x02 | 0x08
} RESIZE_DIR;

typedef enum _LOGIN_STATUS_ {
	LOGIN_STATUS_UNLOGIN,
	LOGIN_STATUS_OPERATOR,
	LOGIN_STATUS_TECHNICIAN,
	LOGIN_STATUS_MANAGER
}LOGIN_STATUS;

class QLabel;
class QRect;
class QPaintEvent;
class QResizeEvent;
class QMouseEvent;
class QMenuBar;
class QToolBar;
class QPushButton;
class QMenu;
class HCONTROL_EXPORT HBaseWindow : public QWidget
{
	Q_OBJECT

signals:
	void sigLoginClicked();

public:
	HBaseWindow();
	virtual ~HBaseWindow();

	// 初始化
	bool init();

	// 获取中心区域
	QWidget* centralWidget();

	// 获取MenuBar
	QMenuBar* menuBar();

	// 获取toolBar
	QToolBar* toolBar();

	// 设置Logo
	void setLogo(QString path);

	// 设置是否可登陆
	void setCanLogin(bool canLogin);

	// 获取是否可登陆
	bool canLogin();

	// 是否是否可进行用户管理
	void setCanUserManagement(bool canUserManagement);

	// 获取是否可进行用户管理
	bool canUserManagement();

	// 设置登陆menubar的name
	void setLogiMenuBarnName(QString name);

	// 设置登陆按钮name
	void setLoginButtonName(QString name);

	// 设置用户管理按钮name
	void setUserManagementName(QString name);

	// 设置等处按钮name
	void setLogoutButtonName(QString name);

protected:
	// 尺寸变更事件
	void resizeEvent(QResizeEvent *event);

	// 绘制事件
	void paintEvent(QPaintEvent *event);

	// 鼠标移动事件
	void mouseMoveEvent(QMouseEvent *event);

	// 鼠标按下事件
	void mousePressEvent(QMouseEvent *event);

	// 鼠标放开事件
	void mouseReleaseEvent(QMouseEvent *event);

	// 登陆事件
	virtual void onLoginEvent();

	// 登出事件
	virtual void onLogoutEvent();

	// 进入用户管理界面
	virtual void onUserManagement();

	// 用纸用户信息
	void setUser(QString name, LOGIN_STATUS login_status);

private:

	// 初始化控件
	bool initWidgets();

	// 初始化布局
	bool initLayouts();

	// 初始化连接
	bool initSignals();

	// 释放资源
	void deInit();

	// 测试边缘
	void testEdge();

private slots:

    // 点击最小化按钮
    void onBtnMinClick();

	// 点击最大化按钮
	void onBtnMaxClick();

	// 点击关闭按钮
	void onBtnCloseClick();

	// 点击登陆区域
	void onBtnLoginClick();

	// 点击登陆按钮
	void onMenuLoginClick();

	// 点击登出按钮
	void onMenuLogoutClick();

	// 点击用户管理按钮
	void onMenuUserManagementClick();

protected:
	LOGIN_STATUS  m_enLoginStatus;

private:

	// 子控件位置信息
	QRect    m_headerRect;
	QRect    m_centralRect;
	QRect    m_menuRect;
	QRect    m_toolRect;
	QRect    m_btnMinRect;
	QRect    m_btnMaxRect;
	QRect    m_btnCloseRect;
	QRect    m_btnLoginRect;
	QRect    m_logoRect;


	// 子控件实例
	QWidget*           m_pLogo;
	QMenuBar*          m_pMenuBar;
	QToolBar*          m_pToolBar;
	QWidget*           m_centralWidget;
	QPushButton*       m_btnMin;
	QPushButton*       m_btnMax;
	QPushButton*       m_btnClose;
	QPushButton*       m_btnLogin;
	QLabel*            m_pLabLogo;

	// 边缘尺寸缩放
	RESIZE_DIR         m_enResizeDir;
	QPoint             m_dragPosition;
	int                m_nEdgeMargin;

	// 登陆项
	QMenu*             m_pLoginContextMenu;
	QAction*           m_pLoginUserManagement;
	QAction*           m_pLoginLogin;
	QAction*           m_pLoginLogout;

	// 参数
	bool               m_bCanLogin;
	bool               m_bCanUserManagement;
	QString            m_strLoginName;
	QString            m_strUserManagementButtonName;
	QString            m_strLoginButtonName;
	QString            m_strLogoutButtonName;
};


#endif