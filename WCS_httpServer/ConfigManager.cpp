#include "ConfigManager.h"
#include <QFile>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <QDir>
#include <QCoreApplication>
#include <QTimer>

ConfigManager* ConfigManager::instance()
{
    static ConfigManager mgr;
    return &mgr;
}

ConfigManager::ConfigManager()
{
    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(2000);  // 2秒聚合窗口
    connect(m_saveTimer, &QTimer::timeout, this, [this]() {
        saveNow();
    });
}

bool AppConfig::loadFromFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;

    QXmlStreamReader xml(&file);
    while (!xml.atEnd())
    {
        xml.readNext();
        if (!xml.isStartElement()) continue;

        QStringRef name = xml.name();
        if (name == "wmsListenPort")           wmsListenPort = xml.readElementText().toInt();
        else if (name == "feedbackUrl")        feedbackUrl = xml.readElementText();
        else if (name == "feedbackTestUrl")    feedbackTestUrl = xml.readElementText();
        else if (name == "appkey")             appkey = xml.readElementText();
        else if (name == "appkeyTest")         appkeyTest = xml.readElementText();
        else if (name == "useTestEnv")         useTestEnv = xml.readElementText().toInt();
        else if (name == "warehouseCode")      warehouseCode = xml.readElementText();
        else if (name == "goodsOwner")         goodsOwner = xml.readElementText();
        else if (name == "waveTimeoutMin")     waveTimeoutMin = xml.readElementText().toInt();
        else if (name == "maxRetryCount")      maxRetryCount = xml.readElementText().toInt();
        else if (name == "httpTimeoutMs")      httpTimeoutMs = xml.readElementText().toInt();
        else if (name == "waveCompleteTimeoutMs") waveCompleteTimeoutMs = xml.readElementText().toInt();
        else if (name == "plcListenPort")      plcListenPort = xml.readElementText().toInt();
        else if (name == "plcS7Ip")            plcS7Ip = xml.readElementText();
        else if (name == "plcS7Rack")          plcS7Rack = xml.readElementText().toInt();
        else if (name == "plcS7Slot")          plcS7Slot = xml.readElementText().toInt();
        else if (name == "businessPoolSize")   businessPoolSize = xml.readElementText().toInt();
        else if (name == "plcSendPoolSize")    plcSendPoolSize = xml.readElementText().toInt();
        else if (name == "logRetainDays")      logRetainDays = xml.readElementText().toInt();
        else if (name == "rfidUrl")            rfidUrl = xml.readElementText();
        else if (name == "binding")
        {
            // 容器绑定: <binding grid="00001">BOX001</binding>
            QString g = xml.attributes().value("grid").toString();
            QString box = xml.readElementText();
            if (!g.isEmpty() && !box.isEmpty())
                containerBindings[g] = box;
        }
        else if (name == "gridName")
        {
            // 格口显示名: <gridName num="1">A区退货口</gridName>
            QString num = xml.attributes().value("num").toString();
            QString display = xml.readElementText();
            if (!num.isEmpty() && !display.isEmpty())
                gridNames[num] = display;
        }
    }
    file.close();
    return !xml.hasError();
}

bool AppConfig::saveToFile(const QString& path) const
{
    QDir().mkpath(QFileInfo(path).absolutePath());

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;

    QXmlStreamWriter xml(&file);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement("config");

    xml.writeTextElement("wmsListenPort",    QString::number(wmsListenPort));
    xml.writeTextElement("feedbackUrl",      feedbackUrl);
    xml.writeTextElement("feedbackTestUrl",  feedbackTestUrl);
    xml.writeTextElement("appkey",           appkey);
    xml.writeTextElement("appkeyTest",       appkeyTest);
    xml.writeTextElement("useTestEnv",       QString::number(useTestEnv));
    xml.writeTextElement("warehouseCode",    warehouseCode);
    xml.writeTextElement("goodsOwner",       goodsOwner);
    xml.writeTextElement("waveTimeoutMin",   QString::number(waveTimeoutMin));
    xml.writeTextElement("maxRetryCount",    QString::number(maxRetryCount));
    xml.writeTextElement("httpTimeoutMs",    QString::number(httpTimeoutMs));
    xml.writeTextElement("waveCompleteTimeoutMs", QString::number(waveCompleteTimeoutMs));
    xml.writeTextElement("plcListenPort",    QString::number(plcListenPort));
    xml.writeTextElement("plcS7Ip",          plcS7Ip);
    xml.writeTextElement("plcS7Rack",        QString::number(plcS7Rack));
    xml.writeTextElement("plcS7Slot",        QString::number(plcS7Slot));
    xml.writeTextElement("businessPoolSize", QString::number(businessPoolSize));
    xml.writeTextElement("plcSendPoolSize",  QString::number(plcSendPoolSize));
    xml.writeTextElement("logRetainDays",    QString::number(logRetainDays));
    xml.writeTextElement("rfidUrl",          rfidUrl);

    // ──── 容器绑定 ────
    QMapIterator<QString, QString> it(containerBindings);
    while (it.hasNext())
    {
        it.next();
        xml.writeStartElement("binding");
        xml.writeAttribute("grid", it.key());
        xml.writeCharacters(it.value());
        xml.writeEndElement();
    }

    // ──── 格口显示名 ────
    QMapIterator<QString, QString> gn(gridNames);
    while (gn.hasNext())
    {
        gn.next();
        xml.writeStartElement("gridName");
        xml.writeAttribute("num", gn.key());
        xml.writeCharacters(gn.value());
        xml.writeEndElement();
    }

    xml.writeEndElement();
    xml.writeEndDocument();
    file.close();
    return true;
}

bool ConfigManager::load()
{
    QString exePath = QCoreApplication::applicationDirPath();
    QString configPath = exePath + "/" CONFIG_FILE;

    if (!QFile::exists(configPath))
    {
        // 配置文件不存在，使用 define.h 宏中的默认值自动创建
        // 所有 URL/AppKey/端口 默认值已在 AppConfig 结构体初始化列表中从 define.h 宏读取
        m_config.saveToFile(configPath);
        return true;
    }

    return m_config.loadFromFile(configPath);
}

bool ConfigManager::save()
{
    // ★ 延迟保存：标记脏数据，2秒内多次调用只触发一次实际写盘
    //    高并发下 bindingUpdated 每秒400+次 → 文件IO从每秒400次降为0.5次
    if (!m_saveDirty)
    {
        m_saveDirty = true;
        m_saveTimer->start();
    }
    return true;  // 调用者无需等待写盘结果
}

bool ConfigManager::saveNow()
{
    m_saveDirty = false;
    m_saveTimer->stop();
    QString exePath = QCoreApplication::applicationDirPath();
    return m_config.saveToFile(exePath + "/" CONFIG_FILE);
}
