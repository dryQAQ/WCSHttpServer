#include "ConfigManager.h"
#include <QFile>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <QDir>
#include <QCoreApplication>

ConfigManager* ConfigManager::instance()
{
    static ConfigManager mgr;
    return &mgr;
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
        if (name == "wmsListenPort")       wmsListenPort = xml.readElementText().toInt();
        else if (name == "feedbackUrl")    feedbackUrl = xml.readElementText();
        else if (name == "feedbackTestUrl") feedbackTestUrl = xml.readElementText();
        else if (name == "appkey")         appkey = xml.readElementText();
        else if (name == "appkeyTest")     appkeyTest = xml.readElementText();
        else if (name == "useTestEnv")     useTestEnv = xml.readElementText().toInt();
        else if (name == "waveTimeoutMin") waveTimeoutMin = xml.readElementText().toInt();
        else if (name == "maxRetryCount")  maxRetryCount = xml.readElementText().toInt();
        else if (name == "logRetainDays")  logRetainDays = xml.readElementText().toInt();
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
    xml.writeTextElement("waveTimeoutMin",   QString::number(waveTimeoutMin));
    xml.writeTextElement("maxRetryCount",    QString::number(maxRetryCount));
    xml.writeTextElement("logRetainDays",    QString::number(logRetainDays));

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
    QString exePath = QCoreApplication::applicationDirPath();
    return m_config.saveToFile(exePath + "/" CONFIG_FILE);
}
