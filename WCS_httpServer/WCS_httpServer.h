#pragma once

#include <QtWidgets/QMainWindow>
#include "ui_WCS_httpServer.h"

class WCS_httpServer : public QMainWindow
{
    Q_OBJECT

public:
    WCS_httpServer(QWidget *parent = Q_NULLPTR);

private:
    Ui::WCS_httpServerClass ui;
};
