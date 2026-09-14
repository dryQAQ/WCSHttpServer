#include "ConfigManager.h"
#include "LogService.h"   // ★ 2026-09-04：启动/配置日志（LOG_INFO 写入 run.log）
#include <QFile>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <QDir>
#include <QCoreApplication>
#include <QTimer>
#include <QCryptographicHash>

// ★ 2026-09-07 配置文件内容哈希（外部修改检测）
static QByteArray configFileHash(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    QByteArray data = f.readAll();
    f.close();
    return QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex();
}

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
        else if (name == "feedbackEndUrl")     feedbackEndUrl = xml.readElementText();     // ★ H8 完结回传 URL
        else if (name == "feedbackEndTestUrl") feedbackEndTestUrl = xml.readElementText(); // ★ H8 完结回传测试 URL
        else if (name == "feedbackMethod")    feedbackMethod = xml.readElementText();     // ★ 2026-09-06 满箱/锁格/波次完成回传 method
        else if (name == "feedbackEndMethod") feedbackEndMethod = xml.readElementText();  // ★ 2026-09-06 完结回传(H8) method
        else if (name == "appkey")             appkey = xml.readElementText();
        else if (name == "appkeyTest")         appkeyTest = xml.readElementText();
        else if (name == "useTestEnv")         useTestEnv = xml.readElementText().toInt();
        else if (name == "rfidQueryUrl")     rfidQueryUrl = xml.readElementText();  // ★ RFID SKU-EPC 绑定查询 URL
        else if (name == "rfidAppkey")       rfidAppkey = xml.readElementText();   // ★ 2026-09-04 RFID 查询鉴权 AppKey
        else if (name == "rfidPushServerIp") rfidPushServerIp = xml.readElementText();  // ★ 2026-09-04 RFID 推送服务端 IP
        else if (name == "rfidPushServerPort") rfidPushServerPort = xml.readElementText().toInt();  // ★ 2026-09-04 RFID 推送服务端端口
        else if (name == "rfidHeartbeatEnable") rfidHeartbeatEnable = xml.readElementText().toInt(); // ★ 2026-09-05 心跳开关(1=发送 0=不发送)
        else if (name == "rfidHeartbeatIntervalMs") rfidHeartbeatIntervalMs = xml.readElementText().toInt(); // ★ 2026-09-05 心跳间隔(ms)
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
        else if (name == "configVersion")     configVersion = xml.readElementText().toInt();
        else if (name == "h7FromLocationSource")    fullboxFromLocationSource = xml.readElementText();
        else if (name == "h7DefaultFromLocation") fullboxDefaultFromLocation = xml.readElementText();
        else if (name == "h7DefaultTargetLocation") fullboxDefaultTargetLocation = xml.readElementText();
        else if (name == "gridCodePrefix")      gridCodePrefix = xml.readElementText();
        else if (name == "gridCodeWidth")       gridCodeWidth = xml.readElementText().toInt();
        // ──── S7 分拣增强配置 ────
        // bool 类型：XML 中写 "true"/"false"，读取时转为小写比较
        else if (name == "sortingEpcDedup")         sortingEpcDedup = (xml.readElementText().trimmed().toLower() == "true");
        else if (name == "sortingGridCapPolicy")    sortingGridCapPolicy = xml.readElementText();
        // ★ 2026-09-13 计划数封顶：超计划件改投异常口
        else if (name == "sortingOverplanPolicy")   sortingOverplanPolicy = xml.readElementText().trimmed();
        else if (name == "exceptionGrid")           exceptionGrid = xml.readElementText().trimmed();
        else if (name == "sortingOrderQtyValidate") sortingOrderQtyValidate = xml.readElementText();
        else if (name == "sortingConflictPolicy")   sortingConflictPolicy = xml.readElementText();
        else if (name == "sortingAllowOverrecv")    sortingAllowOverrecv = (xml.readElementText().trimmed().toLower() == "true");
        // ★ 2026-09-11 同波次重扫重投
        else if (name == "rescanResendEnabled")     rescanResendEnabled = (xml.readElementText().trimmed().toLower() == "true");
        else if (name == "rescanResendCooldownMs")  rescanResendCooldownMs = xml.readElementText().toInt();
        else if (name == "rescanResendMaxTimes")    rescanResendMaxTimes = xml.readElementText().toInt();
        else if (name == "plcInFlightTimeoutMs")    plcInFlightTimeoutMs = xml.readElementText().toInt();
        // ──── ★ 2026-09-14 计划分配表（落格结构优化）────
        else if (name == "allocEnabled")            allocEnabled = (xml.readElementText().trimmed().toLower() == "true");
        else if (name == "allocRequirePlanValid")   allocRequirePlanValid = (xml.readElementText().trimmed().toLower() == "true");
        else if (name == "allocGapMoveOnDisabled")  allocGapMoveOnDisabled = (xml.readElementText().trimmed().toLower() == "true");
        else if (name == "allocGapMoveOnLocked")    allocGapMoveOnLocked = (xml.readElementText().trimmed().toLower() == "true");
        else if (name == "allocMaxInflight")        allocMaxInflight = xml.readElementText().toInt();
        else if (name == "allocAuditIntervalMs")    allocAuditIntervalMs = xml.readElementText().toInt();
        else if (name == "allocClaimTimeoutMs")     allocClaimTimeoutMs = xml.readElementText().toInt();
        else if (name == "allocPlanLogTail")        allocPlanLogTail = xml.readElementText().toInt();
        else if (name == "allocPlanLogStep")        allocPlanLogStep = xml.readElementText().toInt();
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
    xml.writeComment(" 正式环境满箱回传 URL（H7 满箱同步到WMS） ");
    xml.writeTextElement("feedbackUrl",      feedbackUrl);
    xml.writeComment(" 测试环境满箱回传 URL（H7） ");
    xml.writeTextElement("feedbackTestUrl",  feedbackTestUrl);
    xml.writeComment(" 正式环境完结回传 URL（H8 波次完结通知WMS） ");
    xml.writeTextElement("feedbackEndUrl",   feedbackEndUrl);
    xml.writeComment(" 测试环境完结回传 URL（H8） ");
    xml.writeTextElement("feedbackEndTestUrl", feedbackEndTestUrl);
    xml.writeComment(" ★ 满箱/锁格/波次完成回传 method（URL 参数 method，WMS 网关接口标识） ");
    xml.writeTextElement("feedbackMethod",    feedbackMethod);
    xml.writeComment(" ★ 完结回传(H8) method（URL 参数 method，WMS 网关接口标识） ");
    xml.writeTextElement("feedbackEndMethod", feedbackEndMethod);
    xml.writeComment(" 正式环境 AppKey（HTTP Header + URL 参数 appkey） ");
    xml.writeTextElement("appkey",           appkey);
    xml.writeComment(" 测试环境 AppKey ");
    xml.writeTextElement("appkeyTest",       appkeyTest);
    xml.writeComment(" 是否使用测试环境（1=测试, 0=正式） ");
    xml.writeTextElement("useTestEnv",       QString::number(useTestEnv));

    // ──── RFID 查询配置 ────
    xml.writeComment(" RFID SKU-EPC 绑定查询 URL（WCS 查询 RFID 获取 EPC→barcode 映射） ");
    xml.writeTextElement("rfidQueryUrl",     rfidQueryUrl);
    xml.writeComment(" RFID 查询 Authorization 头完整值（形如 APP_KEYS <key>，整串使用；空=不发送） ");
    xml.writeTextElement("rfidAppkey",       rfidAppkey);
    xml.writeComment(" RFID 推送服务端地址（WCS 作为 TCP 客户端主动连接接收 EPC+carNum） ");
    xml.writeTextElement("rfidPushServerIp",   rfidPushServerIp);
    xml.writeTextElement("rfidPushServerPort", QString::number(rfidPushServerPort));
    xml.writeComment(" 是否向 RFID 服务端发送心跳包（1=发送 0=不发送，默认1） ");
    xml.writeTextElement("rfidHeartbeatEnable", QString::number(rfidHeartbeatEnable));
    xml.writeComment(" 心跳发送间隔（毫秒，默认2000=2秒） ");
    xml.writeTextElement("rfidHeartbeatIntervalMs", QString::number(rfidHeartbeatIntervalMs));

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
    xml.writeComment(" 期望绑定数量（波次下发校验用，默认1） ");
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
    xml.writeComment(" fromLocation 取值来源（config=使用固定值, sobi=使用波次明细 sobi 来源库位） ");
    xml.writeTextElement("h7FromLocationSource",   fullboxFromLocationSource);
    xml.writeComment(" 来源库位默认值（config 模式时使用） ");
    xml.writeTextElement("h7DefaultFromLocation",  fullboxDefaultFromLocation);
    xml.writeComment(" 目标库位默认值（无容器号时兜底） ");
    xml.writeTextElement("h7DefaultTargetLocation", fullboxDefaultTargetLocation);

    // ──── WMS 格口编码配置 ────
    xml.writeComment(" WMS 格口编码：对外格口号 = 前缀 + 格口号补零（默认 22+3位：格口号5 → 22005）；前缀留空=关闭转换 ");
    xml.writeTextElement("gridCodePrefix",   gridCodePrefix);
    xml.writeComment(" 格口号补零宽度（默认 3，与内部/PLC 3位一致） ");
    xml.writeTextElement("gridCodeWidth",    QString::number(gridCodeWidth));

    // ──── ★ 2026-09-14 计划分配表（落格结构优化）────
    xml.writeComment(" ★ 计划分配表总开关（true=按各格口计划数量分配；false=回退改造前'恒取首个格口'行为） ");
    xml.writeTextElement("allocEnabled",            allocEnabled ? "true" : "false");
    xml.writeComment(" true=开工预检发现『Σ每格口计划 ≠ H4 orderQty』即拒绝开工（默认 false 仅告警） ");
    xml.writeTextElement("allocRequirePlanValid",   allocRequirePlanValid ? "true" : "false");
    xml.writeComment(" 计划格口满箱未重绑(禁用)时，把未完成计划件转给同 SKU 其它可用计划格口（true/false） ");
    xml.writeTextElement("allocGapMoveOnDisabled",  allocGapMoveOnDisabled ? "true" : "false");
    xml.writeComment(" 计划格口物理锁格时同样转移（true/false；false 时锁格格口不转移） ");
    xml.writeTextElement("allocGapMoveOnLocked",    allocGapMoveOnLocked ? "true" : "false");
    xml.writeComment(" 在途认领上限（超出即强制清扫+异常留痕，防认领泄漏） ");
    xml.writeTextElement("allocMaxInflight",        QString::number(allocMaxInflight));
    xml.writeComment(" 不变量巡检 + 认领超时清扫周期(ms) ");
    xml.writeTextElement("allocAuditIntervalMs",    QString::number(allocAuditIntervalMs));
    xml.writeComment(" 认领超时(ms)：下发后迟迟无落格反馈则释放额度 ");
    xml.writeTextElement("allocClaimTimeoutMs",     QString::number(allocClaimTimeoutMs));
    xml.writeComment(" 选格成功日志前 N 条逐条输出（0=全部逐条） ");
    xml.writeTextElement("allocPlanLogTail",        QString::number(allocPlanLogTail));
    xml.writeComment(" 之后每 N 条输出一条（异常/超计划/搬迁/兜底日志一律逐条保留） ");
    xml.writeTextElement("allocPlanLogStep",        QString::number(allocPlanLogStep));

    // ──── S7 分拣增强配置 ────
    xml.writeComment(" EPC 任务内防重（true/false） ");
    xml.writeTextElement("sortingEpcDedup",          sortingEpcDedup ? "true" : "false");
    xml.writeComment(" 【已废弃】格口上限策略（历史遗留，程序只读写不使用） ");
    xml.writeTextElement("sortingGridCapPolicy",     sortingGridCapPolicy);
    xml.writeComment(" ★ 超计划处置：exception=超计划件改投异常口 / block=超计划件不发指令并标注 / allow=仅标注 ");
    xml.writeTextElement("sortingOverplanPolicy",    sortingOverplanPolicy);
    xml.writeComment(" ★ 物理异常格口号（3位内部号如 066；留空或 0 = 未配置，则超计划件不发指令并告警） ");
    xml.writeTextElement("exceptionGrid",            exceptionGrid);
    xml.writeComment(" orderQty 校验模式（strict=严格, loose=宽松） ");
    xml.writeTextElement("sortingOrderQtyValidate",  sortingOrderQtyValidate);
    xml.writeComment(" 冲突策略（STRICT_EXCEPTION=入异常口, LOOSE_FIRST=取首个匹配） ");
    xml.writeTextElement("sortingConflictPolicy",   sortingConflictPolicy);
    xml.writeComment(" 是否允许超收（true/false） ");
    xml.writeTextElement("sortingAllowOverrecv",     sortingAllowOverrecv ? "true" : "false");
    // ──── ★ 2026-09-11 同波次重扫重投 ────
    xml.writeComment(" 重扫重投开关（true=已落格件重新上料仍按原格口下发；false=旧行为不再下发） ");
    xml.writeTextElement("rescanResendEnabled",      rescanResendEnabled ? "true" : "false");
    xml.writeComment(" 同一 EPC 两次下发最小间隔(ms)，防 RFID 连读 ");
    xml.writeTextElement("rescanResendCooldownMs",   QString::number(rescanResendCooldownMs));
    xml.writeComment(" 同一 EPC 每波次最多重投次数（超限写异常表；首投不计入） ");
    xml.writeTextElement("rescanResendMaxTimes",     QString::number(rescanResendMaxTimes));
    xml.writeComment(" PLC 在途超时(ms)：未收到落格反馈超时后允许重投（防永久锁死） ");
    xml.writeTextElement("plcInFlightTimeoutMs",     QString::number(plcInFlightTimeoutMs));

    // ──── API 路由 ────
    xml.writeComment(" ① 波次下发（WMS → WCS，H4 推送波次数据） ");
    xml.writeTextElement("apiInsertWaveInfo",     apiInsertWaveInfo);
    xml.writeComment(" ② 容器绑定（WMS → WCS，H6 格口容器绑定） ");
    xml.writeTextElement("apiBindingLatticePort", apiBindingLatticePort);
    xml.writeComment(" ③ 波次取消（WMS → WCS，H5 退货任务取消） ");
    xml.writeTextElement("apiInsertWaveIn",       apiInsertWaveIn);

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
    // ★ 配置文件固定放在 exe 同目录的 config 子目录下：{exe}/config/http_server.xml
    QString configPath = exePath + "/" CONFIG_FILE;

    if (!QFile::exists(configPath))
    {
        // 配置文件不存在，使用 define.h 宏中的默认值自动创建
        // 所有 URL/AppKey/端口 默认值已在 AppConfig 结构体初始化列表中从 define.h 宏读取
        LOG_WARN("[配置] 配置文件不存在: %s（将按默认值自动创建）", configPath.toLocal8Bit().constData());
        m_config.saveToFile(configPath);
        LOG_INFO("[配置] 默认配置文件已创建: %s", configPath.toLocal8Bit().constData());
        m_seenHash = configFileHash(configPath);   // ★ 记录自写后哈希
        return true;
    }

    bool ok = m_config.loadFromFile(configPath);
    m_seenHash = configFileHash(configPath);       // ★ 记录本次加载所见文件内容
    LOG_INFO("[配置] 配置文件已加载: %s（%s）", configPath.toLocal8Bit().constData(), ok ? "成功" : "失败");
    return ok;
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
    QString configPath = exePath + "/" CONFIG_FILE;

    // ★ 2026-09-07 防"手改配置被自动保存覆盖"：
    //   保存前比对磁盘文件哈希——若文件被外部（手工/编辑工具/其它实例）改动过，
    //   说明本程序内存里是旧值，直接整写会用旧值覆盖手改（如测试满箱回传地址被还原）。
    //   处理：先重新加载（保留外部改动到内存）再写盘；解析失败则放弃本次保存并告警。
    QByteArray diskHash = configFileHash(configPath);
    if (!diskHash.isEmpty() && m_seenHash != diskHash)
    {
        LOG_WARN("[配置] 检测到配置文件已被外部修改（非本程序写入）——先重新加载再保存，避免覆盖手改内容");
        if (!m_config.loadFromFile(configPath))
        {
            LOG_ERROR("[配置] 外部修改的配置文件解析失败，本次自动保存已取消（保留文件现状，请人工检查XML）");
            return false;
        }
    }

    bool ok = m_config.saveToFile(configPath);
    if (ok)
        m_seenHash = configFileHash(configPath);   // ★ 记录自写后哈希
    return ok;
}
