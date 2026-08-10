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
        else if (name == "rfidQueryUrl")     rfidQueryUrl = xml.readElementText();  // ★ RFID SKU-EPC 绑定查询 URL
        else if (name == "warehouseCode")      warehouseCode = xml.readElementText();
        else if (name == "goodsOwner")         goodsOwner = xml.readElementText();
        else if (name == "waveTimeoutMin")     waveTimeoutMin = xml.readElementText().toInt();
        else if (name == "maxRetryCount")      maxRetryCount = xml.readElementText().toInt();
        else if (name == "expectedBindCount")  expectedBindCount = xml.readElementText().toInt();
        else if (name == "httpTimeoutMs")      httpTimeoutMs = xml.readElementText().toInt();
        else if (name == "waveCompleteTimeoutMs") waveCompleteTimeoutMs = xml.readElementText().toInt();
        else if (name == "plcListenPort")      plcListenPort = xml.readElementText().toInt();
        else if (name == "plcS7Ip")            plcS7Ip = xml.readElementText();
        else if (name == "plcS7Rack")          plcS7Rack = xml.readElementText().toInt();
        else if (name == "plcS7Slot")          plcS7Slot = xml.readElementText().toInt();
        
        else if (name == "businessPoolSize")   businessPoolSize = xml.readElementText().toInt();
        else if (name == "plcSendPoolSize")    plcSendPoolSize = xml.readElementText().toInt();
        else if (name == "plcRecvPoolSize")    plcRecvPoolSize = xml.readElementText().toInt();
        
        else if (name == "logRetainDays")      logRetainDays = xml.readElementText().toInt();
        else if (name == "apiInsertWaveInfo")    apiInsertWaveInfo = xml.readElementText();
        else if (name == "apiBindingLatticePort") apiBindingLatticePort = xml.readElementText();
        else if (name == "apiInsertWaveIn")      apiInsertWaveIn = xml.readElementText();
        else if (name == "apiRfidCarNumReport") apiRfidCarNumReport = xml.readElementText();
        else if (name == "configVersion")     configVersion = xml.readElementText().toInt();
        else if (name == "h7FromLocationSource")    fullboxFromLocationSource = xml.readElementText();
        else if (name == "h7DefaultFromLocation") fullboxDefaultFromLocation = xml.readElementText();
        // ──── S7 分拣增强配置 ────
        // bool 类型：XML 中写 "true"/"false"，读取时转为小写比较
        else if (name == "sortingEpcDedup")         sortingEpcDedup = (xml.readElementText().trimmed().toLower() == "true");
        else if (name == "sortingGridCapPolicy")    sortingGridCapPolicy = xml.readElementText();
        else if (name == "sortingOrderQtyValidate") sortingOrderQtyValidate = xml.readElementText();
        else if (name == "sortingConflictPolicy")   sortingConflictPolicy = xml.readElementText();
        else if (name == "sortingAllowOverrecv")    sortingAllowOverrecv = (xml.readElementText().trimmed().toLower() == "true");
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

    // ──── 配置版本号 ────
    xml.writeComment(" 配置文件版本号，与软件版本匹配检查 ");
    xml.writeTextElement("configVersion",    QString::number(configVersion));

    // ──── 服务配置 ────
    xml.writeComment(" WMS 推送监听端口（WMS → WCS，修改后需重启服务） ");
    xml.writeTextElement("wmsListenPort",    QString::number(wmsListenPort));

    // ──── 回传配置（WCS → WMS）────
    xml.writeComment(" 正式环境回传 URL（WCS 满箱/完结回传 WMS 的地址） ");
    xml.writeTextElement("feedbackUrl",      feedbackUrl);
    xml.writeComment(" 测试环境回传 URL ");
    xml.writeTextElement("feedbackTestUrl",  feedbackTestUrl);
    xml.writeComment(" 正式环境 AppKey（HTTP Header 鉴权） ");
    xml.writeTextElement("appkey",           appkey);
    xml.writeComment(" 测试环境 AppKey ");
    xml.writeTextElement("appkeyTest",       appkeyTest);
    xml.writeComment(" 是否使用测试环境（1=测试, 0=正式） ");
    xml.writeTextElement("useTestEnv",       QString::number(useTestEnv));

    // ──── RFID 查询配置 ────
    xml.writeComment(" RFID SKU-EPC 绑定查询 URL（WCS 查询 RFID 获取 EPC→barcode 映射） ");
    xml.writeTextElement("rfidQueryUrl",     rfidQueryUrl);

    // ──── WMS 业务参数 ────
    xml.writeComment(" 仓库编码 ");
    xml.writeTextElement("warehouseCode",    warehouseCode);
    xml.writeComment(" 货主编码 ");
    xml.writeTextElement("goodsOwner",       goodsOwner);

    // ──── 波次配置 ────
    xml.writeComment(" 波次超时时间（分钟，0=不超时） ");
    xml.writeTextElement("waveTimeoutMin",   QString::number(waveTimeoutMin));
    xml.writeComment(" 异常 SKU 最大重试次数 ");
    xml.writeTextElement("maxRetryCount",    QString::number(maxRetryCount));
    xml.writeComment(" 期望绑定数量（波次下发校验用，默认66） ");
    xml.writeTextElement("expectedBindCount", QString::number(expectedBindCount));

    // ──── 网络超时 ────
    xml.writeComment(" HTTP 回传请求超时（毫秒） ");
    xml.writeTextElement("httpTimeoutMs",    QString::number(httpTimeoutMs));
    xml.writeComment(" 波次完成回传超时（毫秒） ");
    xml.writeTextElement("waveCompleteTimeoutMs", QString::number(waveCompleteTimeoutMs));

    // ──── PLC 通信 ────
    xml.writeComment(" PLC TCP 监听端口（PLC 主动连接此端口，修改后需重启服务） ");
    xml.writeTextElement("plcListenPort",    QString::number(plcListenPort));
    xml.writeComment(" S7 PLC IP 地址 ");
    xml.writeTextElement("plcS7Ip",          plcS7Ip);
    xml.writeComment(" S7 机架号 ");
    xml.writeTextElement("plcS7Rack",        QString::number(plcS7Rack));
    xml.writeComment(" S7 槽位号 ");
    xml.writeTextElement("plcS7Slot",        QString::number(plcS7Slot));

    // ──── 线程池 ────
    xml.writeComment(" 业务线程池大小 ");
    xml.writeTextElement("businessPoolSize", QString::number(businessPoolSize));
    xml.writeComment(" PLC 发送线程池大小 ");
    xml.writeTextElement("plcSendPoolSize",  QString::number(plcSendPoolSize));
    xml.writeComment(" PLC 反馈接收线程池大小 ");
    xml.writeTextElement("plcRecvPoolSize",   QString::number(plcRecvPoolSize));

    // ──── 日志 ────
    xml.writeComment(" 日志保留天数 ");
    xml.writeTextElement("logRetainDays",    QString::number(logRetainDays));

    // ──── 满箱回传配置 ────
    xml.writeComment(" fromLocation 取值来源（config=使用固定值, volu=使用波次明细中的 volu 字段） ");
    xml.writeTextElement("h7FromLocationSource",   fullboxFromLocationSource);
    xml.writeComment(" 来源库位默认值（config 模式时使用） ");
    xml.writeTextElement("h7DefaultFromLocation",  fullboxDefaultFromLocation);

    // ──── S7 分拣增强配置 ────
    xml.writeComment(" EPC 任务内防重（true/false） ");
    xml.writeTextElement("sortingEpcDedup",          sortingEpcDedup ? "true" : "false");
    xml.writeComment(" 格口上限策略（reject=拒收, exception=入异常口, allow=允许超收） ");
    xml.writeTextElement("sortingGridCapPolicy",     sortingGridCapPolicy);
    xml.writeComment(" orderQty 校验模式（strict=严格, loose=宽松） ");
    xml.writeTextElement("sortingOrderQtyValidate",  sortingOrderQtyValidate);
    xml.writeComment(" 冲突策略（STRICT_EXCEPTION=入异常口, LOOSE_FIRST=取首个匹配） ");
    xml.writeTextElement("sortingConflictPolicy",   sortingConflictPolicy);
    xml.writeComment(" 是否允许超收（true/false） ");
    xml.writeTextElement("sortingAllowOverrecv",     sortingAllowOverrecv ? "true" : "false");

    // ──── API 路由 ────
    xml.writeComment(" ① 波次下发（WMS → WCS，H4 推送波次数据） ");
    xml.writeTextElement("apiInsertWaveInfo",     apiInsertWaveInfo);
    xml.writeComment(" ② 容器绑定（WMS → WCS，H6 格口容器绑定） ");
    xml.writeTextElement("apiBindingLatticePort", apiBindingLatticePort);
    xml.writeComment(" ③ 波次取消（WMS → WCS，H5 退货任务取消） ");
    xml.writeTextElement("apiInsertWaveIn",       apiInsertWaveIn);
    xml.writeComment(" ④ RFID 小车号推送（RFID → WCS，EPC+barcode+carNum 映射） ");
    xml.writeTextElement("apiRfidCarNumReport",   apiRfidCarNumReport);

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
