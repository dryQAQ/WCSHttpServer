#include "CameraManager.h"
#include "ConfigManager.h"
#include "LifecycleLogger.h"
#include <QRegularExpression>
#include <QRegularExpressionMatchIterator>
#include <QDebug>
#include <tchar.h>
#include <Windows.h>

CameraManager::CameraManager(QObject* parent) : QObject(parent), m_tcpServer(this) {
    AppConfig& cfg = ConfigManager::instance()->config();
    m_platType = cfg.cameraPlatType;
    CAM_LOG_INFO("CameraManager created platType=%d (1=DaHua, 2=Kenyence)", m_platType);
}

CameraManager::~CameraManager() { stop(); CAM_LOG_INFO("CameraManager destroyed"); }

bool CameraManager::start(const char* ip, int port) {
    if (m_bRunning.load()) { CAM_LOG_WARN("Camera already running"); return true; }
    m_port = port;
    if (!m_tcpServer->Start(_T("0.0.0.0"), m_port)) { CAM_LOG_ERROR("Camera start failed port=%d", m_port); return false; }
    m_bRunning.store(true); m_uptime.start(); m_scanCount.store(0); m_noReadCount.store(0);
    CAM_LOG_INFO("Camera started port=%d platType=%d", m_port, m_platType);
    return true;
}

void CameraManager::stop() {
    if (!m_bRunning.load()) return;
    { std::unique_lock<std::mutex> lock(m_clientMutex);
      for (auto& p : m_mapClient) { m_tcpServer->Disconnect(p.first, true); }
      m_mapClient.clear(); }
    m_tcpServer->Stop(); m_bRunning.store(false);
    CAM_LOG_INFO("Camera stopped");
}

std::vector<QByteArray> CameraManager::splitBySTXETX(const QByteArray& data) {
    std::vector<QByteArray> blocks; int start = -1;
    for (int i = 0; i < data.size(); ++i) {
        if (data[i] == 0x02) { start = i; }
        else if (data[i] == 0x03 && start != -1) { blocks.push_back(data.mid(start + 1, i - start - 1)); start = -1; }
    }
    return blocks;
}

int CameraManager::connectedClientCount() const { std::unique_lock<std::mutex> lock(const_cast<std::mutex&>(m_clientMutex)); return (int)m_mapClient.size(); }
CameraStats CameraManager::stats() const {
    CameraStats s; s.running = m_bRunning.load(); s.port = m_port; s.platType = m_platType;
    s.uptimeSec = uptimeSec(); s.scanCount = m_scanCount.load(); s.noReadCount = m_noReadCount.load();
    { std::lock_guard<std::mutex> lock(m_clientMutex); s.clientCount = (int)m_mapClient.size(); }
    { std::lock_guard<std::mutex> lock(m_lastDataMutex); s.lastBarcode = m_lastBarcode; s.lastCarNum = m_lastCarNum; s.lastScanTimeMs = m_lastScanTimeMs; s.lastIp = m_lastIp; s.lastPort = m_lastPort; }
    return s;
}

qint64 CameraManager::uptimeSec() const { if (!m_bRunning.load() || !m_uptime.isValid()) return 0; return m_uptime.elapsed() / 1000; }

EnHandleResult CameraManager::OnPrepareListen(ITcpServer* pSender, SOCKET soListen) { CAM_LOG_INFO("Camera listen ready port=%d platType=%d", m_port, m_platType); return HR_OK; }

EnHandleResult CameraManager::OnAccept(ITcpServer* pSender, CONNID dwConnID, UINT_PTR soClient) {
    TCHAR szIp[24] = {0}; int ipLen = 24; USHORT port = 0; pSender->GetRemoteAddress(dwConnID, szIp, ipLen, port);
    { std::unique_lock<std::mutex> lock(m_clientMutex); m_mapClient.insert(std::make_pair(dwConnID, "camera")); }
    std::string sip = (char*)szIp; { std::lock_guard<std::mutex> lock(m_lastDataMutex); m_lastIp = QString::fromStdString(sip); m_lastPort = port; }
    CAM_LOG_INFO("Camera connected conn=%llu ip=%s", (unsigned long long)dwConnID, sip.c_str());
    emit cameraConnected(QString::fromStdString(sip), port);
    return HR_OK;
}

EnHandleResult CameraManager::OnSend(ITcpServer*, CONNID, const BYTE*, int) { return HR_OK; }

EnHandleResult CameraManager::OnReceive(ITcpServer*, CONNID, const BYTE* pData, int iLength) {
    QByteArray rawData((const char*)pData, iLength); parseCameraData(rawData); return HR_OK;
}

EnHandleResult CameraManager::OnClose(ITcpServer* pSender, CONNID dwConnID, EnSocketOperation, int) {
    TCHAR szIp[24] = {0}; int ipLen = 24; USHORT port = 0; pSender->GetRemoteAddress(dwConnID, szIp, ipLen, port);
    std::string sip = (char*)szIp; { std::unique_lock<std::mutex> lock(m_clientMutex); m_mapClient.erase(dwConnID); }
    CAM_LOG_INFO("Camera disconnected conn=%llu ip=%s", (unsigned long long)dwConnID, sip.c_str());
    emit cameraDisconnected(QString::fromStdString(sip), port);
    return HR_OK;
}

EnHandleResult CameraManager::OnShutdown(ITcpServer*) { CAM_LOG_INFO("Camera shutdown"); return HR_OK; }
void CameraManager::parseCameraData(const QByteArray& rawData) {
    if (m_platType == 2) {
        QString qdata = QString::fromLocal8Bit(rawData);
        if (qdata.contains("noread") || qdata.contains("NOREAD")) {
            CodeInfo stCode; stCode.codes.push_back("noread"); stCode.srcinfo_ = qdata; stCode.msg = qdata;
            m_noReadCount.fetch_add(1); CAM_LOG_WARN("Camera noread(Kenyence) data=%s", qdata.toLocal8Bit().data());
            if (m_codeCb) m_codeCb(stCode);
            return;
        }
        std::vector<QByteArray> vecdata = splitBySTXETX(rawData);
        if (vecdata.empty()) {
            QString tem = qdata; tem.replace(',', '_');
            QStringList lt = tem.split(':'); if (lt.size() >= 2) {
                CodeInfo stCode; stCode.srcinfo_ = qdata; stCode.msg = QString("{%1|%2}").arg(lt[1], lt[0]);
                stCode.codes.push_back(lt[1]); bool ok = false; stCode.car = lt[0].trimmed().toInt(&ok);
                CAM_LOG_INFO("Camera scan(Kenyence) code=%s car=%d", lt[1].toLocal8Bit().data(), stCode.car);
                { std::lock_guard<std::mutex> lock(m_lastDataMutex); m_lastBarcode = lt[1]; m_lastCarNum = lt[0]; m_lastScanTimeMs = QDateTime::currentMSecsSinceEpoch(); }
                m_scanCount.fetch_add(1); if (m_codeCb) m_codeCb(stCode);
            } return;
        }
        for (int a = 0; a < (int)vecdata.size(); a++) {
            QString tem = QString::fromLocal8Bit(vecdata[a]); tem.replace(',', '_');
            QStringList lt = tem.split(':'); if (lt.size() >= 2) {
                CodeInfo stCode; stCode.srcinfo_ = qdata; stCode.msg = QString("{%1|%2}").arg(lt[1], lt[0]);
                stCode.codes.push_back(lt[1]); bool ok = false; stCode.car = lt[0].trimmed().toInt(&ok);
                CAM_LOG_INFO("Camera scan(Kenyence) code=%s car=%d", lt[1].toLocal8Bit().data(), stCode.car);
                { std::lock_guard<std::mutex> lock(m_lastDataMutex); m_lastBarcode = lt[1]; m_lastCarNum = lt[0]; m_lastScanTimeMs = QDateTime::currentMSecsSinceEpoch(); }
                m_scanCount.fetch_add(1); if (m_codeCb) m_codeCb(stCode);
            } } return;
    }
    QString qdata = QString::fromLocal8Bit(rawData);
    if (qdata.contains("noread") || qdata.contains("NoRead")) {
        CodeInfo stCode; stCode.codes.push_back("noread"); stCode.srcinfo_ = qdata; stCode.msg = qdata;
        m_noReadCount.fetch_add(1); CAM_LOG_WARN("Camera noread(DaHua) data=%s", qdata.toLocal8Bit().data());
        if (m_codeCb) m_codeCb(stCode); return;
    }
    QRegularExpression re("\\{(.*?)\\}"); QRegularExpressionMatchIterator it = re.globalMatch(qdata);
    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next(); QString content = match.captured(1).trimmed();
        if (content.isEmpty()) continue; QStringList parts = content.split('|');
        CodeInfo stCode; stCode.srcinfo_ = qdata; stCode.msg = QString("{%1}").arg(content);
        if (parts.size() >= 2) { stCode.codes.push_back(parts[0].trimmed()); bool ok = false; stCode.car = parts[1].trimmed().toInt(&ok); }
        else if (parts.size() == 1) { stCode.codes.push_back(parts[0].trimmed()); stCode.car = 0; }
        else continue;
        if (stCode.codes.empty() || stCode.codes[0].isEmpty()) continue;
        { std::lock_guard<std::mutex> lock(m_lastDataMutex); m_lastBarcode = stCode.codes[0]; m_lastCarNum = QString::number(stCode.car); m_lastScanTimeMs = QDateTime::currentMSecsSinceEpoch(); }
        m_scanCount.fetch_add(1); CAM_LOG_INFO("Camera scan(DaHua) code=%s car=%d", stCode.codes[0].toLocal8Bit().data(), stCode.car);
        if (m_codeCb) m_codeCb(stCode);
    }
}