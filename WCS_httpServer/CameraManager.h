#pragma once
// ============================================================================
// CameraManager.h — 相机通信管理器（与 WCSApp DaHuaSys/Kenyence 完全一致）
//
// 职责：
//   ① 监听TCP端口，接受相机主动连接（HP-Socket CTcpServerListener）
//   ② 支持 DaHua 协议: {条码|小车号} 格式（platType=1）
//   ③ 支持 Kenyence 协议: STX{小车号:条码}ETX 格式（platType=2）
//   ④ 解析条码+小车号，通过回调函数通知上层（与WCSApp的 _cb(stCode) 一致）
//   ⑤ 管理多相机客户端连接
//   ⑥ 连接状态监控和统计
//
// 通信协议（与 WCSApp 完全兼容）：
//   DaHua:   {条码|小车号}  — 如 {ST1234567890123|001}
//           {条码}         — 仅条码，无小车号
//           {noread|001}   — 未识别到条码
//   Kenyence: STX{小车号:条码}ETX  — 如 0x02{001:ST1234567890123}0x03
//           noread/NOREAD  — 未识别到条码
//
// 回调机制（与 WCSApp 一致）：
//   相机扫描到条码后，在 HP-Socket I/O 线程中直接调用 CodeRecvCallBack 回调
//   上层（HttpServer）在回调中提交到相机数据处理专用线程池，不阻塞 I/O 线程
//   与 WCSApp 的 _cb(stCode) → FrmMainV2::OnCodeFrinDaHuaResultCallBack 路径一致
//
// 参考: WCSApp\WCSApps\DaHuaSys.h, Kenyence.h, DaHuaSys.cpp, Kenyence.cpp
// ============================================================================

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QElapsedTimer>
#include <QDateTime>
#include <QTimer>
#include <atomic>
#include <mutex>
#include <map>
#include <vector>
#include <functional>
#include "HPSocket.h"
#include "define.h"

// 相机扫描结果（与 WCSApp CodeInfo 结构体完全一致）
struct CodeInfo
{
    std::vector<QString> codes;   // 条码列表
    int    car       = 0;         // 小车号
    QString msg;                  // 原始报文
    QString srcinfo_;             // 原始数据（调试用）
};

// 相机扫描回调（与 WCSApp CodeRecvCallBack 完全一致）
// 在 HP-Socket I/O 线程中调用，上层应尽快返回或提交到线程池处理
typedef std::function<void(CodeInfo)> CodeRecvCallBack;

// 相机统计快照
struct CameraStats
{
    bool    running      = false;
    int     port         = CAMERA_LISTEN_PORT;
    int     platType     = 1;    // 1=DaHua, 2=Kenyence
    qint64  uptimeSec    = 0;
    int     clientCount  = 0;
    int64_t scanCount    = 0;
    int64_t noReadCount  = 0;  // 未识别条码数
    QString lastBarcode;
    QString lastCarNum;
    qint64  lastScanTimeMs = 0;
    QString lastIp;
    int     lastPort     = 0;
};

class CameraManager : public QObject, public CTcpServerListener
{
    Q_OBJECT

    // ---- CTcpServerListener 回调 ----
    EnHandleResult OnAccept(ITcpServer* pSender, CONNID dwConnID, UINT_PTR soClient) override;
    EnHandleResult OnSend(ITcpServer* pSender, CONNID dwConnID, const BYTE* pData, int iLength) override;
    EnHandleResult OnShutdown(ITcpServer* pSender) override;
    EnHandleResult OnReceive(ITcpServer* pSender, CONNID dwConnID, const BYTE* pData, int iLength) override;
    EnHandleResult OnClose(ITcpServer* pSender, CONNID dwConnID, EnSocketOperation enOperation, int iErrorCode) override;
    EnHandleResult OnPrepareListen(ITcpServer* pSender, SOCKET soListen) override;

public:
    explicit CameraManager(QObject* parent = nullptr);
    ~CameraManager();

    bool start(const char* ip = "0.0.0.0", int port = CAMERA_LISTEN_PORT);
    void stop();
    bool isRunning() const { return m_bRunning.load(); }

    // ★ 注册扫描结果回调（与 WCSApp DaHua::RegisterCodeResultCallBack 一致）
    void RegisterCodeResultCallBack(CodeRecvCallBack cb) { m_codeCb = std::move(cb); }

    // ★ 设置相机协议类型（与 WCSApp platType 一致）
    //    1 = DaHua  ({条码|小车号})
    //    2 = Kenyence (STX{小车号:条码}ETX)
    void setPlatType(int platType) { m_platType = platType; }
    int  platType() const { return m_platType; }

    int  connectedClientCount() const;
    CameraStats stats() const;
    qint64 uptimeSec() const;

signals:
    // ★ 仅保留 UI 相关信号（连接状态变化通知主线程 UI 更新）
    void cameraConnected(const QString& ip, int port);
    void cameraDisconnected(const QString& ip, int port);

private:
    void parseCameraData(const QByteArray& data);

    // ---- Kenyence 协议辅助（与 WCSApp Kenyence::splitData111111 完全一致） ----
    std::vector<QByteArray> splitBySTXETX(const QByteArray& data);

    // ---- TCP 通信 ----
    CTcpServerPtr m_tcpServer;

    std::atomic<bool> m_bRunning{false};
    int m_port = CAMERA_LISTEN_PORT;
    int m_platType = 1;  // ★ 相机协议类型: 1=DaHua, 2=Kenyence（与 WCSApp 一致）

    mutable std::mutex m_clientMutex;
    std::map<CONNID, std::string> m_mapClient;

    // ★ 扫描结果回调（与 WCSApp DaHua::_cb 一致，在 HP-Socket I/O 线程中调用）
    CodeRecvCallBack m_codeCb;

    // ---- 统计 ----
    std::atomic<int64_t> m_scanCount{0};
    std::atomic<int64_t> m_noReadCount{0};
    QElapsedTimer m_uptime;

    // ---- 最近数据 ----
    QString m_lastBarcode;
    QString m_lastCarNum;
    qint64  m_lastScanTimeMs = 0;
    QString m_lastIp;
    int     m_lastPort = 0;
    mutable std::mutex m_lastDataMutex;
};