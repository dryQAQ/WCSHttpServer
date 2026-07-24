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
    QString configPath = exePath + "/config/http_server.xml";

    if (!QFile::exists(configPath))
    {
        // 创建默认配置文件（首次运行或配置文件丢失时）
        // ──── WMS回传地址 ────
        m_config.feedbackTestUrl = "http://182.92.166.232/gids5/service/thirdPartyData/dz_bxh_wcs_cs";  // WMS测试环境回传接口
        m_config.feedbackUrl     = "http://47.93.21.77:9090/gids5/service/thirdPartyData/dz_bxh_wcs_zs"; // WMS正式环境回传接口
        // ──── WMS认证AppKey ────
        m_config.appkeyTest      = "dz_bxh_wcs_cs";  // 测试环境AppKey
        m_config.appkey          = "dz_bxh_wcs_zs";  // 正式环境AppKey
        m_config.saveToFile(configPath);
        return true;
    }

    return m_config.loadFromFile(configPath);
}

bool ConfigManager::save()
{
    QString exePath = QCoreApplication::applicationDirPath();
    return m_config.saveToFile(exePath + "/config/http_server.xml");
}
