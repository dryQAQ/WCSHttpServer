# -*- coding: utf-8 -*-
"""
mock_app.py — 窄带分拣 WCS 本地 Mock 联调仿真台（tkinter GUI，零第三方依赖）

启动: python mock_app.py   （或双击 run_mock.bat）
角色（全部依据 release_WcsHttpServer/config/http_server.xml 里的地址工作）:
  RFID推送服务端(监听 rfidPushServerIp:Port) / PLC客户端(连 plcListenPort)
  S7锁格模拟(102, DB77) / WMS下发客户端(连 wmsListenPort)
  WMS网关接收(H7/H8回传, 8099等) / RFID查询服务(rfidQueryUrl 端口)
"""
import collections
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time
import tkinter as tk
import tkinter.font as tkfont
import urllib.error
import urllib.request
from tkinter import messagebox, ttk

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import mock_cfg
import mock_channels
import mock_logstore
import mock_proto
import mock_scenario
import mock_s7comm
import mock_swap_config

HERE = os.path.dirname(os.path.abspath(__file__))


def local_bind_host(cfg_host, store=None):
    """绑定地址：配置为 127.0.0.1/localhost 用之；否则回退 127.0.0.1 并提示"""
    h = (cfg_host or "").strip()
    if h in ("127.0.0.1", "localhost", "0.0.0.0", "::1", ""):
        return "127.0.0.1" if h != "0.0.0.0" else h
    return "127.0.0.1"


def url_host_port(url, def_port):
    m = re.match(r"^https?://([^/:]+)(?::(\d+))?", url or "")
    if not m:
        return "127.0.0.1", def_port
    return m.group(1), (int(m.group(2)) if m.group(2) else def_port)


# ── run_mock.bat 传入的“预期端口”（仅供『配置』页对照参考，
#    仿真台实际仍按 http_server.xml 工作，绝不自动改写配置文件） ──
EXPECTED_PORT_ENV = {
    "wms": "MOCK_WMS_PORT", "plc": "MOCK_PLC_PORT", "rfid": "MOCK_RFID_PORT",
    "gw": "MOCK_GW_PORT", "query": "MOCK_QUERY_PORT",
}
PORT_REF_ROWS = [
    # (说明, xml 节点/位置, 端口key)
    ("WMS 软件下发", "wmsListenPort", "wms"),
    ("PLC 指令通道", "plcListenPort", "plc"),
    ("RFID 推送服务端", "rfidPushServerPort", "rfid"),
    ("H7/H8 回传网关", "URL feedbackTestUrl / feedbackEndTestUrl", "gw"),
    ("RFID 查询服务", "URL rfidQueryUrl", "query"),
    ("S7 锁格", "snap7 协议固定", None),
]


def env_expected_ports(env=None):
    """读取外部环境给出的预期端口（key: wms/plc/rfid/gw/query）；无效/缺省项忽略"""
    env = os.environ if env is None else env
    out = {}
    for key, var in EXPECTED_PORT_ENV.items():
        raw = (env.get(var) or "").strip()
        try:
            n = int(raw)
        except Exception:
            continue
        if 1 <= n <= 65535:
            out[key] = n
    return out


def argv_expected_ports(argv=None):
    """解析 run_mock.bat 命令行参数: wms=9001 plc=2500 ...（大小写不敏感，无效值忽略）"""
    argv = sys.argv[1:] if argv is None else argv
    out = {}
    for raw in argv:
        if "=" not in raw:
            continue
        k, _, v = raw.partition("=")
        k = k.strip().lower()
        if k not in EXPECTED_PORT_ENV:
            continue
        try:
            n = int(v.strip())
        except Exception:
            continue
        if 1 <= n <= 65535:
            out[k] = n
    return out


# 远程拓扑参数（键 → 环境变量）: wcs=WCS所在IP, bind=仿真台监听绑定地址
NET_ARG_ENV = {"wcs": "MOCK_WCS_IP", "bind": "MOCK_BIND_IP"}

# 通道停用开关（run_mock.bat 直接传，如: run_mock.bat noplc nos7 nogw noquery）
CHANNEL_SWITCHES = ("norfid", "noplc", "nos7", "nogw", "noquery")
CHANNEL_SWITCH_BY_NAME = {"RFID推送服务端": "norfid", "PLC客户端": "noplc",
                          "S7锁格模拟": "nos7", "WMS网关接收": "nogw",
                          "RFID查询服务": "noquery"}


def disabled_channels(argv=None):
    """解析停用通道开关（大小写不敏感）"""
    argv = sys.argv[1:] if argv is None else argv
    return {a.strip().lower() for a in argv if a.strip().lower() in CHANNEL_SWITCHES}


def net_args(argv=None, env=None):
    """解析远程拓扑参数: wcs=目标WCS的IP / bind=监听绑定地址(如0.0.0.0)。
    命令行优先于环境变量；无效(空)值忽略。"""
    argv = sys.argv[1:] if argv is None else argv
    env = os.environ if env is None else env
    out = {}
    for key, var in NET_ARG_ENV.items():
        v = (env.get(var) or "").strip()
        if v:
            out[key] = v
    for raw in argv:
        if "=" in raw:
            k, _, v = raw.partition("=")
            k = k.strip().lower()
            if k in NET_ARG_ENV and v.strip():
                out[k] = v.strip()
    return out


def resolve_expected_ports():
    """期望端口最终值 = 环境变量 ∪ 命令行参数（run_mock.bat 传入）"""
    out = env_expected_ports()
    out.update(argv_expected_ports())
    return out


def port_reference_lines(cfg, expected=None):
    """『配置』页端口对照行: [(tag, line)]，tag ∈ head/ok/bad/gray"""
    expected = dict(expected or {})
    cur = cfg.port_summary() if cfg.ok else {}
    lines = [("head", "端口预期参考 —— run_mock.bat 指定 vs http_server.xml 当前值（手工改文件用，本台不自动改配置）"),
             ("gray", "注: WCS 监听项改后须重启 WCS_httpServer.exe；仿真台监听项改后重启本台即生效；S7=102 固定")]
    marks = "①②③④⑤⑥"
    for i, (name, node, key) in enumerate(PORT_REF_ROWS):
        if key is None:
            lines.append(("gray", f"  {marks[i]} {name}   {node}  → 固定 102"))
            continue
        cur_v = cur.get(key, "-")
        if key in expected:
            exp_v = expected[key]
            same = (exp_v == cur_v)
            lines.append((("ok" if same else "bad"),
                          f"  {marks[i]} {name}   {node}   期望 {exp_v}   当前 {cur_v}   "
                          f"{'✓ 一致' if same else '✗ 不一致'}{'' if same else '（改 xml）'}"))
        else:
            lines.append(("gray",
                          f"  {marks[i]} {name}   {node}   期望 —   当前 {cur_v}"))
    return lines


class MockConsole(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("WCS 窄带分拣 Mock 联调仿真台")
        self.geometry("1440x900")
        self.minsize(1180, 720)

        self._closing = False
        self._tick_after = None
        self.ui_queue = collections.deque()
        self.wcs_proc_detect = False
        self.wcs_proc_check_at = 0.0
        self.last_scen_report = ""

        # ── 报文留存 ──
        logdir = os.path.join(HERE, "logs")
        self.store = mock_logstore.MessageStore(max_keep=60000, log_dir=logdir)

        # ── 配置 ──
        self.cfg = mock_cfg.WcsConfig().load()
        if not self.cfg.ok:
            self.store.add_event(f"配置加载失败: {self.cfg.error}")
        # run_mock.bat 指定的“预期端口”（仅配置页对照展示，不影响运行行为）
        self.expected_ports = resolve_expected_ports()

        # ── 服务实例（地址全部取自配置文件） ──
        # 远程拓扑覆盖参数（run_mock.bat 传 wcs=/bind= 或环境变量）:
        #   wcs=WCS所在IP   -> PLC客户端 / WMS下发客户端 的连接目标（默认127.0.0.1）
        #   bind=监听地址    -> RFID推送/S7/网关/查询 等监听服务的绑定地址（默认按配置=回环；
        #                       仿真台与WCS异机时填 0.0.0.0 或本机网卡IP）
        net = net_args()
        wcs_ip = net.get("wcs") or "127.0.0.1"
        bind_host = net.get("bind") or ""
        self.wcs_ip = wcs_ip
        self.bind_host = bind_host
        rfid_host = bind_host or local_bind_host(self.cfg.rfid_server_ip, self.store)
        plc_host = wcs_ip
        qhost, qport = url_host_port(self.cfg.rfid_query_url, 9100)
        gwhost, gwport = url_host_port(self.cfg.feedback_url_h7, 8099)
        s7host = bind_host or local_bind_host(self.cfg.plc_s7_ip, self.store)
        if bind_host:
            qhost = gwhost = bind_host
        else:
            qhost = "127.0.0.1" if qhost not in ("127.0.0.1", "localhost", "0.0.0.0") else qhost
            gwhost = "127.0.0.1" if gwhost not in ("127.0.0.1", "localhost", "0.0.0.0") else gwhost

        self.rfid = mock_channels.RfidServer(rfid_host, self.cfg.rfid_server_port, self.store)
        self.plc = mock_channels.PlcClient(plc_host, self.cfg.plc_listen_port, self.store)
        self.gw = mock_channels.HttpGatewayServer(gwhost, gwport, self.store, self.cfg)
        self.query = mock_channels.RfidQueryServer(qhost, qport, self.store, self.cfg)
        self.s7 = mock_s7comm.S7Server(s7host, 102, self.store)
        self.push = mock_channels.HttpWmsPush(f"http://{wcs_ip}:{self.cfg.wms_listen_port}",
                                              self.store)

        self.services = [("RFID推送服务端", self.rfid), ("PLC客户端", self.plc),
                         ("WMS网关接收", self.gw), ("RFID查询服务", self.query),
                         ("S7锁格模拟", self.s7)]

        # ── PLC 自动回执设置（场景/页面共用） ──
        self.auto_ack_on = False
        self.auto_ack_status = "1"
        self.auto_ack_5field = True
        self.auto_ack_scenario = False   # ★ 场景控制标志：不受界面刷新覆盖（修复:场景中途自动回执被UI tick关闭）
        self.auto_ack_delay_ms = 0       # ★ PLC 自动回执延迟(ms)，模拟 PLC 响应耗时
        self.plc_seen = collections.deque(maxlen=8000)   # (epc, time, text)
        self.piece_grid = {}                             # epc -> grid（场景登记）
        self.plc.on_cmd = self._plc_on_cmd

        # ── 场景检查点 ──
        self._cp_ev = threading.Event()
        self._cp_ev.set()
        self._cp_text = ""
        self._scen_abort = threading.Event()

        # ── RFID 自动推送状态 ──
        self.rfid_auto_job = None
        self.rfid_auto_epcs = []
        self.rfid_seq_start = 1
        self.rfid_seq_auto = True

        # ── UI ──
        self._build_ui()
        self._apply_initial_mapping()
        self._ov_at = 0.0
        self._refresh_cfg_view()

        # 启动服务（可用 noplc/nos7/nogw/noquery/norfid 停用个别通道：
        # 如现场真 PLC 已连 WCS 时用 noplc，仿真台只作注入源、不冒充 PLC）
        self.disabled_channels = disabled_channels()
        for name, svc in self.services:
            if name in CHANNEL_SWITCH_BY_NAME and CHANNEL_SWITCH_BY_NAME[name] in self.disabled_channels:
                self.store.add_event(f"{name} 已按启动参数停用（{CHANNEL_SWITCH_BY_NAME[name]}），不在本次启动之列")
                continue
            try:
                svc.start()
            except Exception as e:
                self.store.add_event(f"{name} 启动异常: {e}")
        if self.disabled_channels:
            self.store.add_event("本次停用通道: " + " ".join(sorted(self.disabled_channels)))
        self.store.add_event("仿真台已启动。当前配置文件: " + self.cfg.path)
        self.store.add_event("提示：请先确认 WCS 已启用 MOCK 配置并启动 WCS_httpServer.exe（本台信息见 状态总览 页）")

        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.after(300, self._tick)

    # ======================================================================
    # UI 骨架
    # ======================================================================
    def _build_ui(self):
        style = ttk.Style(self)
        try:
            style.theme_use("vista")
        except Exception:
            pass
        default_font = tkfont.nametofont("TkDefaultFont")
        default_font.configure(family="Microsoft YaHei UI", size=9)
        tkfont.nametofont("TkTextFont").configure(family="Consolas", size=9)

        head = ttk.Frame(self, padding=(8, 6))
        head.pack(fill="x")
        self.lbl_title = tk.Label(head, text="◆ 窄带分拣 WCS × 三方设备 Mock 联调台",
                                  font=("Microsoft YaHei UI", 12, "bold"))
        self.lbl_title.grid(row=0, column=0, sticky="w")
        self.btn_all_start = ttk.Button(head, text="▶ 全部启动服务", command=self._all_start)
        self.btn_all_start.grid(row=0, column=1, padx=6)
        self.btn_all_stop = ttk.Button(head, text="■ 全部停止服务", command=self._all_stop)
        self.btn_all_stop.grid(row=0, column=2)
        self.lbl_status = tk.Label(head, text="", fg="#333333", anchor="w", justify="left")
        self.lbl_status.grid(row=1, column=0, columnspan=6, sticky="we", pady=(4, 0))
        head.columnconfigure(0, weight=1)

        nb = ttk.Notebook(self)
        nb.pack(fill="both", expand=True, padx=4, pady=(0, 4))
        self.nb = nb
        pages = [
            ("状态总览", self._pg_overview), ("报文总览", self._pg_msg),
            ("RFID推送", self._pg_rfid), ("PLC", self._pg_plc),
            ("软件下发WMS→WCS", self._pg_wms), ("WMS网关收H7/H8", self._pg_gw),
            ("RFID查询服务", self._pg_query), ("S7锁格", self._pg_s7),
            ("场景与演练", self._pg_scen), ("配置", self._pg_cfg),
        ]
        for title, builder in pages:
            f = ttk.Frame(nb)
            nb.add(f, text=title)
            builder(f)

        self.bind_all("<Control-q>", lambda e: self._on_close())

    # ------------------------------------------------------------------ 页: 状态总览
    def _pg_overview(self, parent):
        wrap = ttk.Frame(parent)
        wrap.pack(fill="both", expand=True)
        left = ttk.LabelFrame(wrap, text="依据配置文件的工作地址", padding=6)
        left.pack(side="left", fill="both", expand=True, padx=4, pady=4)
        right = ttk.LabelFrame(wrap, text="测试步骤速查（对照 12 项需求）", padding=6)
        right.pack(side="left", fill="both", expand=True, padx=4, pady=4)

        self.ov_cfg = tk.Text(left, height=16, width=74, state="disabled", font=("Consolas", 9))
        self.ov_cfg.pack(fill="both", expand=True)

        right_txt = (
            "1) RFID推送  {SNxxxx|01|EPC}0D      → 页『RFID推送』发送；识别为{EPC|小车号}\n"
            "2) 任务下发(H4 波次)                 → 页『软件下发』样例/模板推送\n"
            "3) 格口容器绑定(H6)                  → 页『软件下发』批量绑定\n"
            "4) 逐条操纵报文                       → 各通道页均可编辑后手动发送/定时推送\n"
            "5) WCS→PLC {EPC|格口|小车}           → 页『PLC』观察并回执\n"
            "6) PLC落格反馈 {EPC|格口|小车|小车+2|状态} → 页『PLC』自动/手动回执\n"
            "7) PLC锁格(满箱)                     → 页『S7锁格』点格口置位 → WCS检测DB77上升沿→H7\n"
            "8) 重新绑定格口容器                   → H6 再次下发同/新容器(自动恢复满箱禁用格口)\n"
            "9/10) 满箱/完结回传应答               → 页『WMS网关』自动按样例应答 success\n"
            "11) appkey/method 校验                → WMS网关页醒目提示 URL参数/Header 是否齐全一致\n"
            "12) 保留报文                          → 页『报文总览』全部通道原始收发，可过滤/导出\n"
            "\n【WCS 界面人工步骤(模拟器无法代点)】\n"
            "  ① 启动 WCS_httpServer.exe(须已启用MOCK配置)\n"
            "  ② 点击『开始接收任务』 ③ 波次到达后点『开始分拣』\n"
            "  ④ 结束后点『结束任务』(触发 H8)\n"
            "\n【快速全流程】→ 页『场景与演练』→ 运行标准全流程\n")
        self.ov_hint = tk.Text(right, height=16, width=74, state="disabled", font=("Microsoft YaHei UI", 9))
        self._txt(self.ov_hint, right_txt)

        bottom = ttk.LabelFrame(wrap, text="通道与服务状态（每2秒刷新）", padding=6)
        bottom.pack(side="bottom", fill="x", padx=4)
        self.ov_svc = tk.Text(bottom, height=8, state="disabled", font=("Consolas", 9))
        self.ov_svc.pack(fill="both", expand=True)
        wrap.columnconfigure(0, weight=1)

    # ------------------------------------------------------------------ 页: 报文总览
    def _pg_msg(self, parent):
        top = ttk.Frame(parent, padding=4)
        top.pack(fill="x")
        ttk.Label(top, text="通道:").pack(side="left")
        self.f_ch = ttk.Combobox(top, width=8, state="readonly",
                                 values=("全部",) + tuple(mock_logstore.CHANNELS))
        self.f_ch.current(0)
        self.f_ch.pack(side="left", padx=(2, 8))
        ttk.Label(top, text="方向:").pack(side="left")
        self.f_dir = ttk.Combobox(top, width=6, state="readonly", values=("全部", "收", "发", "事件"))
        self.f_dir.current(0)
        self.f_dir.pack(side="left", padx=(2, 8))
        ttk.Label(top, text="关键字:").pack(side="left")
        self.f_kw = ttk.Entry(top, width=26)
        self.f_kw.pack(side="left", padx=2)
        self.f_kw.bind("<Return>", lambda e: self._msg_rebuild())
        ttk.Button(top, text="应用过滤", command=self._msg_rebuild).pack(side="left", padx=4)
        ttk.Button(top, text="清空显示", command=self._msg_clear).pack(side="left", padx=4)
        ttk.Button(top, text="导出报文...", command=self._msg_export).pack(side="left", padx=4)
        self.lbl_msgcnt = tk.Label(top, text="", fg="#666")
        self.lbl_msgcnt.pack(side="right")

        mid = ttk.Frame(parent)
        mid.pack(fill="both", expand=True)
        cols = ("time", "ch", "dir", "summary", "ref")
        self.msg_tree = ttk.Treeview(mid, columns=cols, show="headings", height=14)
        widths = {"time": 150, "ch": 70, "dir": 42, "summary": 620, "ref": 130}
        heads = {"time": "时间", "ch": "通道", "dir": "方向", "summary": "摘要", "ref": "关联"}
        for c in cols:
            self.msg_tree.heading(c, text=heads[c])
            self.msg_tree.column(c, width=widths[c], anchor="w")
        ys = ttk.Scrollbar(mid, orient="vertical", command=self.msg_tree.yview)
        self.msg_tree.configure(yscrollcommand=ys.set)
        self.msg_tree.pack(side="left", fill="both", expand=True)
        ys.pack(side="right", fill="y")
        self.msg_tree.bind("<<TreeviewSelect>>", self._msg_onselect)

        detail = ttk.LabelFrame(parent, text="报文详情（原始+解析+HEX）", padding=4)
        detail.pack(fill="x", pady=(2, 2))
        self.msg_detail = tk.Text(detail, height=7, state="disabled", font=("Consolas", 9),
                                  bg="#f7f7f2")
        self.msg_detail.pack(fill="x")

        self._msg_iids = []          # 保持窗口行序
        self._msg_applied_seq = 0
        self._msg_auto_scroll = tk.BooleanVar(value=True)
        ttk.Checkbutton(top, text="自动滚动", variable=self._msg_auto_scroll).pack(side="left")

    def _msg_rebuild(self, *_):
        rows = self.store.snapshot(channel=self.f_ch.get(), direction=self.f_dir.get(),
                                   keyword=self.f_kw.get().strip(), last_n=3000)
        self.msg_tree.delete(*self.msg_tree.get_children())
        self._msg_iids = []
        for r in rows:
            self._msg_insert(r)
        self._msg_applied_seq = self.store.last_seq()
        self._msg_cnt_label()

    def _msg_insert(self, r):
        iid = self.msg_tree.insert("", "end", iid=str(r["seq"]),
                                   values=(r["time_str"], r["channel"], r["dir"], r["summary"], r["ref"]))
        self._msg_iids.append(iid)
        while len(self._msg_iids) > 4000:
            old = self._msg_iids.pop(0)
            if self.msg_tree.exists(old):
                self.msg_tree.delete(old)
        if self._msg_auto_scroll.get():
            self.msg_tree.see(iid)

    def _msg_clear(self):
        self.msg_tree.delete(*self.msg_tree.get_children())
        self._msg_iids = []
        self._msg_applied_seq = self.store.last_seq()
        self._msg_cnt_label()

    def _msg_cnt_label(self):
        try:
            total = self.store.count()
            shown = len(self.msg_tree.get_children())
            self.lbl_msgcnt.config(text=f"显示 {shown} / 共 {total}")
        except Exception:
            pass

    def _msg_onselect(self, _e=None):
        sel = self.msg_tree.selection()
        if not sel:
            return
        try:
            seq = int(sel[0])
        except Exception:
            return
        rows = self.store.snapshot(last_n=None)
        for r in reversed(rows):
            if r["seq"] == seq:
                break
        else:
            return
        txt = f"── {r['time_str']} [{r['channel']}] {r['dir']}  {r['summary']}\n"
        if r["raw_text"]:
            txt += r["raw_text"]
        if r["raw_hex"]:
            txt += "\nHEX: " + r["raw_hex"]
        if r["parsed"]:
            txt += "\n解析: " + r["parsed"]
        self._txt(self.msg_detail, txt)

    def _msg_export(self):
        from tkinter import filedialog
        path = filedialog.asksaveasfilename(defaultextension=".txt",
                                            initialfile=f"报文_%s.txt" % time.strftime("%Y%m%d_%H%M%S"),
                                            filetypes=[("文本", "*.txt")])
        if not path:
            return
        rows = self.store.snapshot(channel=self.f_ch.get(), direction=self.f_dir.get(),
                                   keyword=self.f_kw.get().strip())
        with open(path, "w", encoding="utf-8") as f:
            f.write(f"# WCS Mock 报文导出 {time.strftime('%Y-%m-%d %H:%M:%S')}  共{len(rows)}条\n")
            for r in rows:
                f.write(mock_logstore.MessageStore._fmt_line(r) + "\n")
        messagebox.showinfo("导出完成", f"已导出 {len(rows)} 条报文到\n{path}")

    # ------------------------------------------------------------------ 页: RFID推送
    def _pg_rfid(self, parent):
        left = ttk.LabelFrame(parent, text="服务状态", padding=6)
        left.pack(side="left", fill="y", padx=4, pady=4)
        self.rfid_st = tk.Label(left, text="", justify="left", font=("Consolas", 9), fg="#222")
        self.rfid_st.pack(anchor="w")

        mid = ttk.LabelFrame(parent, text="单帧发送（格式 {流水号|设备码|EPC} 帧尾）", padding=6)
        mid.pack(side="left", fill="both", expand=True, padx=4, pady=4)
        r = 0
        ttk.Label(mid, text="流水号:").grid(row=r, column=0, sticky="e")
        self.rfid_seq = ttk.Entry(mid, width=10)
        self.rfid_seq.insert(0, "SN0027")
        self.rfid_seq.grid(row=r, column=1, sticky="w")
        self.rfid_seq_inc = tk.BooleanVar(value=True)
        ttk.Checkbutton(mid, text="发送后自动+1(循环SN0001~SN0099)", variable=self.rfid_seq_inc).grid(row=r, column=2, sticky="w")
        r += 1
        ttk.Label(mid, text="设备码:").grid(row=r, column=0, sticky="e")
        self.rfid_dev = ttk.Entry(mid, width=10)
        self.rfid_dev.insert(0, "01")
        self.rfid_dev.grid(row=r, column=1, sticky="w")
        ttk.Label(mid, text="（多设备可用 02/03，WCS 不参与业务）").grid(row=r, column=2, sticky="w")
        r += 1
        ttk.Label(mid, text="EPC:").grid(row=r, column=0, sticky="ne")
        self.rfid_epc = ttk.Entry(mid, width=52)
        self.rfid_epc.insert(0, "A10126000900009285552527")
        self.rfid_epc.grid(row=r, column=1, columnspan=2, sticky="we", pady=2)
        r += 1
        ttk.Label(mid, text="帧尾:").grid(row=r, column=0, sticky="e")
        self.rfid_tail = ttk.Combobox(mid, width=16, state="readonly",
                                      values=[t[0] for t in mock_proto.TAIL_CHOICES])
        self.rfid_tail.current(0)
        self.rfid_tail.grid(row=r, column=1, sticky="w")
        self.rfid_tail.bind("<<ComboboxSelected>>",
                            lambda e: self.store.add_event(
                                "RFID帧尾切换: " + self.rfid_tail.get()))
        ttk.Label(mid, text="（现场=字面0D；CR/无 仅兼容测试）").grid(row=r, column=2, sticky="w")
        r += 1
        btns = ttk.Frame(mid)
        btns.grid(row=r, column=0, columnspan=3, pady=6)
        ttk.Button(btns, text="➤ 发送一帧", command=self._rfid_send_once).pack(side="left")
        ttk.Button(btns, text="NOREAD帧", command=self._rfid_send_noread).pack(side="left", padx=6)
        ttk.Button(btns, text="心跳帧(服务端发)", command=self._rfid_send_heart).pack(side="left")
        r += 1
        self.rfid_preview = tk.Label(mid, text="", fg="#00695c", justify="left", font=("Consolas", 9))
        self.rfid_preview.grid(row=r, column=0, columnspan=3, sticky="w")
        r += 1
        self.rfid_note = tk.Label(mid, text="识别: {流水号|设备码|EPC} → {EPC|小车号}；SN0027→小车27→PLC指令段 027", fg="#888")
        self.rfid_note.grid(row=r, column=0, columnspan=3, sticky="w", pady=(4, 0))

        right = ttk.LabelFrame(parent, text="EPC列表连续推送（模拟产线节奏）", padding=6)
        right.pack(side="left", fill="both", expand=True, padx=4, pady=4)
        ttk.Label(right, text="每行一个 EPC（留空行自动跳过）；流水号按 小车1~99 轮转分配").pack(anchor="w")
        self.rfid_list = tk.Text(right, height=12, width=40, font=("Consolas", 9))
        self.rfid_list.pack(fill="both", expand=True)
        f = ttk.Frame(right)
        f.pack(fill="x", pady=4)
        ttk.Label(f, text="间隔ms:").pack(side="left")
        self.rfid_pace = ttk.Entry(f, width=7)
        self.rfid_pace.insert(0, "350")
        self.rfid_pace.pack(side="left", padx=2)
        ttk.Label(f, text="轮数(0=无限):").pack(side="left")
        self.rfid_rounds = ttk.Entry(f, width=5)
        self.rfid_rounds.insert(0, "1")
        self.rfid_rounds.pack(side="left", padx=2)
        self.btn_rfid_auto = ttk.Button(f, text="▶ 开始连续推送", command=self._rfid_auto_start)
        self.btn_rfid_auto.pack(side="left", padx=4)
        self.btn_rfid_auto2 = ttk.Button(f, text="■ 停止", command=self._rfid_auto_stop)
        self.btn_rfid_auto2.pack(side="left")
        self.rfid_auto_st = tk.Label(right, text="", fg="#666")
        self.rfid_auto_st.pack(anchor="w")
        tip = ("提示：EPC 需在『RFID查询服务』页有映射且波次处于分拣中，WCS 才会向 PLC 发落格指令；\n"
               "同一 EPC 5 分钟内重复推送不会重复处理（缓存+防重），可用『清空』重建环境。")
        ttk.Label(right, text=tip, justify="left").pack(anchor="w")

    def _rfid_tail_kind(self):
        sel = self.rfid_tail.get() or ""
        for label, key in mock_proto.TAIL_CHOICES:
            if sel == label:
                return key
        return "LIT0D"

    def _rfid_next_seq(self):
        if self.rfid_seq_inc.get():
            cur = self.rfid_seq.get().strip()
            m = re.search(r"(\d+)", cur)
            if m:
                n = (int(m.group(1)) % 99) + 1
                return "SN%04d" % n
        return self.rfid_seq.get().strip() or "SN0001"

    def _rfid_preview_label(self, seq, epc):
        car = mock_proto.car_num_from_seq(seq)
        self.rfid_preview.config(
            text=f"帧预览: {{ {seq}|{self.rfid_dev.get().strip()}|{epc} }}{'0D' if self._rfid_tail_kind() == 'LIT0D' else ''}  →  小车号={car or '?'}(PLC段={mock_proto.pad3(car) if car else '?'})")

    def _rfid_send_once(self):
        epc = self.rfid_epc.get().strip()
        seq = self.rfid_seq.get().strip() or "SN0001"
        raw = mock_proto.make_rfid_frame(seq, self.rfid_dev.get().strip() or "01", epc,
                                         self._rfid_tail_kind())
        ok, note = self.rfid.send_frame(raw)
        if ok and self.rfid_seq_inc.get():
            self.rfid_seq.delete(0, "end")
            self.rfid_seq.insert(0, self._rfid_next_seq())
        self._preview_note(note)

    def _rfid_send_noread(self):
        raw = mock_proto.make_rfid_frame(self.rfid_seq.get().strip() or "SN0001",
                                         self.rfid_dev.get().strip() or "01", "NOREAD",
                                         self._rfid_tail_kind())
        ok, note = self.rfid.send_frame(raw, note="NOREAD")
        self._preview_note(note)

    def _rfid_send_heart(self):
        ok, note = self.rfid.send_frame(b"{HEARTBEAT}0D", note="服务端心跳(可选)")
        self._preview_note(note)

    def _preview_note(self, note):
        self.rfid_note.config(fg="#00695c", text=note)

    def _rfid_auto_start(self):
        if self.rfid_auto_job:
            return
        txt = self.rfid_list.get("1.0", "end")
        epcs = [ln.strip() for ln in txt.splitlines() if ln.strip()]
        if not epcs:
            messagebox.showwarning("无EPC", "请在右侧列表输入至少一个 EPC（或从场景页自动生成）")
            return
        try:
            pace = max(int(self.rfid_pace.get()), 50)
            rounds = int(self.rfid_rounds.get())
        except Exception:
            pace, rounds = 350, 1
        self.rfid_auto_epcs = epcs
        self.rfid_auto_rounds = rounds
        self.rfid_auto_pace = pace
        self.rfid_auto_total = 0
        self.rfid_auto_job = "running"
        self.store.add_event(f"RFID连续推送开始 {len(epcs)}个EPC 间隔{pace}ms 轮数{rounds}")
        self._rfid_auto_step(0, 0)

    def _rfid_auto_step(self, idx, rnd):
        if self.rfid_auto_job != "running":
            self.rfid_auto_job = None
            return
        if not self.rfid.connected_client_count():
            self.rfid_auto_st.config(text="⚠ 无 WCS 连接，停止推送", fg="red")
            self.rfid_auto_job = None
            return
        epcs = self.rfid_auto_epcs
        epc = epcs[idx]
        seq = "SN%04d" % (self.rfid_seq_start if self.rfid_seq_start else 1)
        n = ((self.rfid_seq_start - 1) + self.rfid_auto_total) % 99 + 1
        seq = "SN%04d" % n
        raw = mock_proto.make_rfid_frame(seq, self.rfid_dev.get().strip() or "01", epc,
                                         self._rfid_tail_kind())
        ok, note = self.rfid.send_frame(raw)
        self.rfid_auto_total += 1
        self.rfid_auto_st.config(
            text=f"已推送 {self.rfid_auto_total} 帧（当前 {seq}）{'OK' if ok else '发失败'}")
        nidx = idx + 1
        nrnd = rnd
        if nidx >= len(epcs):
            nidx = 0
            nrnd += 1
            if 0 < self.rfid_auto_rounds <= nrnd:
                self.rfid_auto_st.config(text=f"连续推送完成 共{self.rfid_auto_total}帧")
                self.store.add_event(f"RFID连续推送完成 共{self.rfid_auto_total}帧")
                self.rfid_auto_job = None
                return
        self.rfid_auto_job = self.after(self.rfid_auto_pace,
                                        lambda: self._rfid_auto_step(nidx, nrnd))

    def _rfid_auto_stop(self):
        self.rfid_auto_job = None
        self.rfid_auto_st.config(text="已停止")

    # ------------------------------------------------------------------ 页: PLC
    def _pg_plc(self, parent):
        left = ttk.LabelFrame(parent, text="连接状态", padding=6)
        left.pack(side="left", fill="y", padx=4, pady=4)
        self.plc_st = tk.Label(left, text="", justify="left", font=("Consolas", 9), fg="#222")
        self.plc_st.pack(anchor="w")
        ttk.Button(left, text="重新连接", command=lambda: self.plc.restart()).pack(anchor="w", pady=6)

        mid = ttk.LabelFrame(parent, text="WCS→PLC 落格指令（最近）与自动回执", padding=6)
        mid.pack(side="left", fill="both", expand=True, padx=4, pady=4)
        self.plc_last = tk.Text(mid, height=7, state="disabled", font=("Consolas", 9), bg="#eef7ef")
        self.plc_last.pack(fill="x")
        f = ttk.Frame(mid)
        f.pack(fill="x", pady=4)
        self.auto_ack_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(f, text="收到落格指令自动回执", variable=self.auto_ack_var,
                        command=self._plc_apply_auto).pack(side="left")
        ttk.Label(f, text="状态码:").pack(side="left", padx=(10, 2))
        self.plc_status_cb = ttk.Combobox(f, width=22, state="readonly",
                                          values=("1 成功", "2 无格口(异常)", "3 信息不全(异常)", "0 其它(按成功)"))
        self.plc_status_cb.current(0)
        self.plc_status_cb.pack(side="left")
        self.plc_status_cb.bind("<<ComboboxSelected>>", lambda e: self._plc_apply_auto())
        self.plc_5field = tk.BooleanVar(value=True)
        ttk.Checkbutton(f, text="5字段", variable=self.plc_5field,
                        command=self._plc_apply_auto).pack(side="left", padx=6)
        ttk.Label(f, text="回执延迟ms:").pack(side="left", padx=(10, 2))
        self.plc_delay = ttk.Entry(f, width=6)
        self.plc_delay.insert(0, "0")
        self.plc_delay.pack(side="left")
        f2 = ttk.Frame(mid)
        f2.pack(fill="x", pady=2)
        ttk.Button(f2, text="{start}", command=lambda: self.plc.send_text("{start}", "发PLC")).pack(side="left")
        ttk.Button(f2, text="{stop}", command=lambda: self.plc.send_text("{stop}", "发PLC")).pack(side="left", padx=6)
        self.lbl_plc_ack_now = tk.Label(f2, text="自动回执当前: 关", fg="#666")
        self.lbl_plc_ack_now.pack(side="right")

        right = ttk.LabelFrame(parent, text="手动回执 / 自定义帧", padding=6)
        right.pack(side="left", fill="both", expand=True, padx=4, pady=4)
        ttk.Label(right, text="5字段反馈示例 {EPC|格口号|小车号|小车号+2|状态码} —— 回车号自动+2").pack(anchor="w")
        f3 = ttk.Frame(right)
        f3.pack(fill="x", pady=3)
        ttk.Label(f3, text="EPC:").pack(side="left")
        self.plc_epc = ttk.Entry(f3, width=30)
        self.plc_epc.pack(side="left", padx=2)
        ttk.Label(f3, text="格口:").pack(side="left")
        self.plc_grid = ttk.Entry(f3, width=5)
        self.plc_grid.insert(0, "001")
        self.plc_grid.pack(side="left", padx=2)
        ttk.Label(f3, text="小车:").pack(side="left")
        self.plc_car = ttk.Entry(f3, width=5)
        self.plc_car.insert(0, "027")
        self.plc_car.pack(side="left", padx=2)
        ttk.Button(f3, text="回执成功(状态1)", command=lambda: self._plc_reply("1")).pack(side="left", padx=4)
        ttk.Button(f3, text="回执异常(状态2)", command=lambda: self._plc_reply("2")).pack(side="left")
        ttk.Label(right, text="任意原文（如 {EPC|005|027|029|1}，支持多帧一包）:").pack(anchor="w", pady=(6, 0))
        self.plc_raw = tk.Text(right, height=6, font=("Consolas", 9))
        self.plc_raw.pack(fill="x")
        self.plc_raw.insert("1.0", "{A10126000900009285552527|001|027|029|1}")
        ttk.Button(right, text="发送原文", command=self._plc_send_raw).pack(anchor="w", pady=4)
        tk.Label(right, text="说明：WCS 只认 {start}/{stop}/3字段/5字段；状态码仅 2/3 判异常，"
                 "第4字段只存档不校验；指令帧无帧尾。", fg="#999", justify="left",
                 wraplength=430).pack(anchor="w")

    def _plc_apply_auto(self):
        # 界面勾选 OR 场景控制（场景期间不被界面刷新覆盖）
        ui_on = self.auto_ack_var.get()
        self.auto_ack_on = bool(ui_on or self.auto_ack_scenario)
        try:
            self.auto_ack_delay_ms = max(int(self.plc_delay.get()), 0)
        except Exception:
            self.auto_ack_delay_ms = 0
        if self.auto_ack_scenario:
            # 场景控制时状态码以场景设定为准（不覆盖界面下拉）
            s = self.auto_ack_status
            tag = "场景控制"
        else:
            s = self.plc_status_cb.get().split(" ")[0]
            self.auto_ack_status = s
            tag = "手动勾选" if ui_on else "关"
        self.auto_ack_5field = self.plc_5field.get()
        delay_txt = f" 延迟{self.auto_ack_delay_ms}ms" if self.auto_ack_delay_ms else ""
        self.lbl_plc_ack_now.config(
            text=f"自动回执当前: {tag + '-开(状态=' + s + ')' + delay_txt if (ui_on or self.auto_ack_scenario) else '关'}")

    def _plc_status_from_cb(self):
        return self.plc_status_cb.get().split(" ")[0] if self.plc_status_cb.get() else "1"

    def _plc_on_cmd(self, fields):
        """PLC线程回调：返回应答帧列表"""
        if len(fields) == 3:
            epc, grid, car = fields[0], fields[1], fields[2]
            self.plc_seen.append((epc, time.time(), "|".join(fields)))
        elif len(fields) == 5:
            self.plc_seen.append((fields[0], time.time(), "|".join(fields)))
        replies = []
        if self.auto_ack_on and len(fields) >= 3:
            status = self.auto_ack_status
            if self.auto_ack_5field:
                replies.append(mock_proto.make_plc_feedback(fields[0], fields[1], fields[2], status).decode("ascii"))
            else:
                replies.append("{%s|%s|%s}" % (fields[0], fields[1], fields[2]))
        if replies and self.auto_ack_delay_ms:
            time.sleep(self.auto_ack_delay_ms / 1000.0)   # ★ 模拟 PLC 响应延迟(界面可调)
        return replies

    def _plc_reply(self, status):
        epc = self.plc_epc.get().strip() or "A10126000900009285552527"
        grid = self.plc_grid.get().strip() or "001"
        car = self.plc_car.get().strip() or "027"
        if self.plc_5field.get():
            raw = mock_proto.make_plc_feedback(epc, grid, car, status)
        else:
            raw = ("{%s|%s|%s}" % (epc, grid, car)).encode("ascii")
        self.plc.send_bytes(raw, note=f"手动回执状态{status}")

    def _plc_send_raw(self):
        txt = self.plc_raw.get("1.0", "end").strip()
        if not txt:
            return
        for seg in re.findall(r"\{[^{}]*\}", txt) or ([txt] if txt else []):
            self.plc.send_text(seg, "原文")

    # ------------------------------------------------------------------ 页: 软件下发
    def _pg_wms(self, parent):
        left = ttk.LabelFrame(parent, text="模板选择与校验", padding=6)
        left.pack(side="left", fill="both", expand=True, padx=4, pady=4)
        f = ttk.Frame(left)
        f.pack(fill="x")
        ttk.Label(f, text="接口:").pack(side="left")
        self.wm_api = ttk.Combobox(f, state="readonly", width=34,
                                   values=[("H4 波次下发 " + self.cfg.api_insert_wave),
                                           ("H6 容器绑定 " + self.cfg.api_binding),
                                           ("H5 退货取消 " + self.cfg.api_cancel)])
        self.wm_api.current(0)
        self.wm_api.pack(side="left", padx=4)
        self.wm_api.bind("<<ComboboxSelected>>", lambda e: self._wm_load_template())
        ttk.Button(f, text="载入模板→编辑区", command=self._wm_load_template).pack(side="left")
        f2 = ttk.Frame(left)
        f2.pack(fill="x")
        ttk.Label(f2, text="模板:").pack(side="left")
        self.wm_tpl = ttk.Combobox(f2, state="readonly", width=46)
        self.wm_tpl.pack(side="left", padx=4)
        ttk.Label(f2, text="orderCode:").pack(side="left")
        self.wm_oc = ttk.Entry(f2, width=20)
        self.wm_oc.pack(side="left")
        self._wm_fill_tpl_list()
        self.wm_tpl.current(0)
        ttk.Label(left, text="请求体 JSON（可自由编辑）：").pack(anchor="w")
        self.wm_body = tk.Text(left, height=16, font=("Consolas", 9))
        self.wm_body.pack(fill="both", expand=True)
        f3 = ttk.Frame(left)
        f3.pack(fill="x", pady=4)
        ttk.Button(f3, text="校验(求和/字段/JSON)", command=self._wm_validate).pack(side="left")
        ttk.Button(f3, text="➤ 发送到 WCS", command=self._wm_send).pack(side="left", padx=8)
        self.lbl_wm_warn = tk.Label(f3, text="", fg="#b26a00")
        self.lbl_wm_warn.pack(side="left")
        tk.Label(left, text="最近响应：", fg="#666").pack(anchor="w")
        self.wm_resp = tk.Text(left, height=6, state="disabled", font=("Consolas", 9), bg="#eef3fb")
        self.wm_resp.pack(fill="x")

        right = ttk.LabelFrame(parent, text="H6 批量绑定 / 便捷工具", padding=6)
        right.pack(side="left", fill="both", expand=True, padx=4, pady=4)
        ttk.Label(right, text="批量 H6（逐条发送，间隔150ms）").pack(anchor="w")
        fb = ttk.Frame(right)
        fb.pack(fill="x", pady=3)
        ttk.Label(fb, text="格口范围:").pack(side="left")
        self.bind_from = ttk.Entry(fb, width=5)
        self.bind_from.insert(0, "1")
        self.bind_from.pack(side="left")
        ttk.Label(fb, text="~").pack(side="left")
        self.bind_to = ttk.Entry(fb, width=5)
        self.bind_to.insert(0, "6")
        self.bind_to.pack(side="left")
        ttk.Label(fb, text="容器前缀:").pack(side="left", padx=(8, 2))
        self.bind_pre = ttk.Entry(fb, width=10)
        self.bind_pre.insert(0, "H-01-2")
        self.bind_pre.pack(side="left")
        ttk.Button(fb, text="批量绑定", command=self._wm_batch_bind).pack(side="left", padx=6)
        self.lbl_bind_st = tk.Label(right, text="", fg="#666")
        self.lbl_bind_st.pack(anchor="w")
        tk.Label(right, text="（示例: 前缀 H-01-2 + 三位序号 H-01-2001 ↔ 格口1；\n重新绑定同一格口=换箱并自动解除满箱禁用，见需求8）",
                 fg="#999", justify="left").pack(anchor="w", pady=6)
        ttk.Separator(right, orient="horizontal").pack(fill="x", pady=6)
        tk.Label(right, text="关键应答格式速查：", fg="#555").pack(anchor="w")
        info = ("H4成功: {\"code\":\"200\",\"message\":\"successed.\",\"sentTime\":\"...\"}\n"
                "H6成功: {\"code\":\"200\",\"message\":\"收到信息\"}\n"
                "H5成功: {\"code\":\"200\",\"message\":\"取消成功\",\"cancellable\":true}\n"
                "未点[开始接收任务]→HTTP500 {\"code\":\"500\",\"message\":\"未开始接收任务...\"}\n"
                "H4 strict校验: orderQty 必须 = Σitems.gridNumber，否则 500")
        tk.Label(right, text=info, justify="left", fg="#444", font=("Consolas", 9)).pack(anchor="w")
        self.push.on_result = self._wm_on_result

    def _wm_fill_tpl_list(self):
        self.wm_tpl.configure(values=[
            "① 用户样例波次 PP202600000030 (32明细/sobi)",
            "② 干净波次 6格口×1SKU×2件",
            "③ 干净波次 3格口×2SKU×2件",
            "④ 容器绑定单条模板(格口6)",
            "⑤ 取消波次模板",
        ])

    def _wm_tpl_index(self):
        try:
            return self.wm_tpl.current()
        except Exception:
            return 0

    def _wm_load_template(self, *_):
        api = self.wm_api.get()
        tpl = self._wm_tpl_index()
        oc = self.wm_oc.get().strip() or ("PP2026%08d" % (time.time() % 100000000))
        if api.startswith("H4"):
            if tpl == 0:
                obj = mock_proto.sample_wave(oc)
            elif tpl == 1:
                w, _m = mock_proto.build_clean_wave(oc, grids=6, per_grid=2)
                obj = w
            else:
                w, _m, _p = mock_proto.build_count_wave(oc, grids=3, skus_per_grid=2, qty=2)
                obj = w
            text = json.dumps(obj, ensure_ascii=False, indent=1)
        elif api.startswith("H6"):
            obj = {"boxcode": "%s%03d" % (self.bind_pre.get().strip() or "H-01-2", 6),
                   "latticehole": "6"}
            text = json.dumps(obj, ensure_ascii=False, indent=1)
        else:
            obj = mock_proto.cancel_payload(oc)
            text = json.dumps(obj, ensure_ascii=False, indent=1)
        self._txt_ed(self.wm_body, text)
        if not oc:
            self.wm_oc.insert(0, obj.get("orderCode", ""))

    def _wm_current_api_path(self):
        api = self.wm_api.get()
        if api.startswith("H4"):
            return self.cfg.api_insert_wave
        if api.startswith("H6"):
            return self.cfg.api_binding
        return self.cfg.api_cancel

    def _wm_validate(self):
        body = self.wm_body.get("1.0", "end")
        try:
            obj = json.loads(body)
        except Exception as e:
            self.lbl_wm_warn.config(text=f"JSON错误: {e}", fg="red")
            return None
        warns = []
        if self._wm_current_api_path() == self.cfg.api_insert_wave:
            items = obj.get("items", [])
            total = sum(int(it.get("gridNumber", 0)) for it in items if isinstance(it, dict))
            qty = obj.get("orderQty")
            try:
                qty = int(qty) if qty is not None else None
            except Exception:
                qty = None
            if qty is not None and total != qty:
                warns.append(f"⚠orderQty={qty} ≠ ΣgridNumber={total}（strict模式将500拒收）")
            bad = [it.get("gridNum") for it in items
                   if mock_proto.grid_code_parse(it.get("gridNum")) is None]
            if bad:
                warns.append(f"⚠gridNum 越界/非法: {bad}（应为 22001~22066 或 1~66）")
            if not items:
                warns.append("⚠items 为空")
            if not warns:
                warns.append(f"✓ 校验通过: items={len(items)} ΣgridNumber={total}")
        elif self._wm_current_api_path() == self.cfg.api_binding:
            try:
                h = int(obj.get("latticehole"))
                if not 1 <= h <= 66:
                    warns.append("⚠格口号越界 1~66")
            except Exception:
                warns.append("⚠缺少 latticehole")
            if not obj.get("boxcode"):
                warns.append("⚠缺少 boxcode")
            if not warns:
                warns.append(f"✓ 绑定格口{obj.get('latticehole')} → {obj.get('boxcode')}")
        else:
            if not obj.get("orderCode"):
                warns.append("⚠缺少 orderCode")
            if not warns:
                warns.append("✓ 取消请求格式正确")
        self.lbl_wm_warn.config(text=" | ".join(warns), fg="#b26a00")
        return obj

    def _wm_send(self):
        obj = self._wm_validate()
        if obj is None:
            return
        self.push.send(self._wm_current_api_path(), json.dumps(obj, ensure_ascii=False))
        self._txt_ed(self.wm_resp, "（发送中…响应到达后显示于此并进入报文总览）")

    def _wm_on_result(self, text):
        self.ui_queue.append(("wm_resp", text))

    def _wm_batch_bind(self):
        try:
            b0, b1 = int(self.bind_from.get()), int(self.bind_to.get())
        except Exception:
            messagebox.showwarning("参数", "格口范围须为数字")
            return
        pre = self.bind_pre.get().strip() or "H-01-2"
        if not 1 <= b0 <= b1 <= 66:
            messagebox.showwarning("参数", "格口范围须在 1~66 内")
            return
        payloads = mock_proto.binding_payloads(pre, b0, b1)
        self.lbl_bind_st.config(text=f"开始批量绑定 {len(payloads)} 个...")
        threading.Thread(target=self._batch_bind_worker, args=(payloads,), daemon=True).start()

    def _batch_bind_worker(self, payloads):
        ok = 0
        for i, p in enumerate(payloads):
            if self._closing:
                break
            self.push.send(self.cfg.api_binding, json.dumps(p, ensure_ascii=False),
                           note=f"批量{i + 1}/{len(payloads)}")
            time.sleep(0.15)
            ok += 1
        self.ui_queue.append(("bind_done", f"批量绑定请求已全部发出（{ok}条）→ 响应见报文总览/最近响应"))

    # ------------------------------------------------------------------ 页: WMS网关
    def _pg_gw(self, parent):
        left = ttk.LabelFrame(parent, text="应答策略", padding=6)
        left.pack(side="left", fill="y", padx=4, pady=4)
        self.gw_mode = tk.StringVar(value="auto")
        for txt, val in (("自动-成功(样例应答)", "auto"),
                         ("自动-失败(success=false)", "auto_fail"),
                         ("自定义应答体", "custom"),
                         ("人工应答(进队列)", "manual")):
            ttk.Radiobutton(left, text=txt, value=val, variable=self.gw_mode,
                            command=self._gw_apply).pack(anchor="w")
        f = ttk.Frame(left)
        f.pack(fill="x", pady=4)
        ttk.Label(f, text="延迟ms:").pack(side="left")
        self.gw_delay = ttk.Entry(f, width=7)
        self.gw_delay.insert(0, "0")
        self.gw_delay.pack(side="left")
        tk.Label(f, text="(>3000 测WCS超时)", fg="#999").pack(side="left")
        ttk.Button(left, text="应用策略", command=self._gw_apply).pack(fill="x", pady=4)
        ttk.Label(left, text="校验基准（按配置文件）:").pack(anchor="w")
        chk = (f"appkey = {self.cfg.active_appkey}\n"
               f"method H7 = {self.cfg.method_h7}\n"
               f"method H8 = {self.cfg.method_h8}\n"
               f"监听 {self.gw.host}:{self.gw.port}\n"
               f"期望 Header: AppKey 与 URL参数 appkey 一致")
        tk.Label(left, text=chk, justify="left", font=("Consolas", 8), fg="#444").pack(anchor="w")
        self.gw_st = tk.Label(left, text="", justify="left", fg="#222")
        self.gw_st.pack(anchor="w", pady=6)
        self.gw_h78 = tk.Label(left, text="H7:0  H8:0", font=("Consolas", 10, "bold"), fg="#00695c")
        self.gw_h78.pack(anchor="w")

        right = ttk.LabelFrame(parent, text="收到回传与应答", padding=6)
        right.pack(side="left", fill="both", expand=True, padx=4, pady=4)
        ttk.Label(right, text="最近收到的报文（原文+校验结果已入报文总览）：").pack(anchor="w")
        self.gw_last_body = tk.Text(right, height=13, state="disabled", font=("Consolas", 9))
        self.gw_last_body.pack(fill="both", expand=True)
        ttk.Label(right, text="自定义应答体 JSON:").pack(anchor="w")
        self.gw_custom = tk.Text(right, height=5, font=("Consolas", 9))
        self.gw_custom.insert("1.0", json.dumps({"success": True, "body": "自定义", "ts": ""},
                                                ensure_ascii=False))
        self.gw_custom.pack(fill="x")
        bottom = ttk.Frame(right)
        bottom.pack(fill="x", pady=2)
        ttk.Label(bottom, text="人工应答队列:").pack(side="left")
        self.gw_q_list = tk.Listbox(bottom, height=5)
        self.gw_q_list.pack(side="left", fill="both", expand=True, padx=4)
        fq = ttk.Frame(bottom)
        fq.pack(side="left")
        ttk.Button(fq, text="答成功", command=lambda: self._gw_q_ans("success")).pack(anchor="w")
        ttk.Button(fq, text="答失败", command=lambda: self._gw_q_ans("fail")).pack(anchor="w", pady=2)
        ttk.Button(fq, text="丢弃", command=lambda: self._gw_q_ans("drop")).pack(anchor="w")

    def _gw_apply(self):
        mode = self.gw_mode.get()
        try:
            delay = max(int(self.gw_delay.get()), 0)
        except Exception:
            delay = 0
        try:
            custom = json.loads(self.gw_custom.get("1.0", "end")) if self.gw_custom.get("1.0", "end").strip() else {}
        except Exception:
            custom = {}
        success = mode != "auto_fail"
        self.gw.set_rule(mode=("auto" if mode in ("auto", "auto_fail") else ("custom" if mode == "custom" else "manual")),
                         success=success, delay_ms=delay,
                         custom_body=json.dumps(custom, ensure_ascii=False))
        self.store.add_event(f"WMS网关应答策略: {self.gw_mode.get()} delay={delay}ms")

    def _gw_q_ans(self, act):
        sel = self.gw_q_list.curselection()
        if not sel:
            return
        item = self.gw_q_list.get(sel[0])
        try:
            idx = int(item.split("#")[1].split("]")[0])
        except Exception:
            return
        if act == "drop":
            with self.gw.pending_lock:
                self.gw.pending = [p for p in self.gw.pending if p["id"] != idx]
            self.store.add_event(f"已丢弃人工应答队列#{idx}")
        else:
            ok, msg = self.gw.manual_respond(idx, act)
            self.store.add_event(msg)

    # ------------------------------------------------------------------ 页: RFID查询
    def _pg_query(self, parent):
        left = ttk.LabelFrame(parent, text="EPC ↔ SKU(barcode) 映射表", padding=6)
        left.pack(side="left", fill="both", expand=True, padx=4, pady=4)
        cols = ("epc", "sku")
        self.q_tree = ttk.Treeview(left, columns=cols, show="headings", height=14)
        self.q_tree.heading("epc", text="EPC")
        self.q_tree.heading("sku", text="SKU(barcode=波次inco)")
        self.q_tree.column("epc", width=300)
        self.q_tree.column("sku", width=230)
        self.q_tree.pack(fill="both", expand=True)
        f = ttk.Frame(left)
        f.pack(fill="x", pady=4)
        ttk.Label(f, text="EPC:").pack(side="left")
        self.q_epc = ttk.Entry(f, width=26)
        self.q_epc.pack(side="left")
        ttk.Label(f, text="SKU:").pack(side="left", padx=(6, 0))
        self.q_sku = ttk.Entry(f, width=18)
        self.q_sku.pack(side="left")
        ttk.Button(f, text="＋增", command=self._q_add).pack(side="left", padx=4)
        ttk.Button(f, text="－删选中", command=self._q_del).pack(side="left")
        f2 = ttk.Frame(left)
        f2.pack(fill="x", pady=2)
        ttk.Button(f2, text="载入:干净波次映射", command=lambda: self._q_load("clean")).pack(side="left")
        ttk.Button(f2, text="载入:用户样例单行", command=lambda: self._q_load("sample")).pack(side="left", padx=6)
        ttk.Button(f2, text="清空", command=self._q_clear).pack(side="left")
        self.lbl_q_note = tk.Label(f2, text="", fg="#666")
        self.lbl_q_note.pack(side="left", padx=8)

        right = ttk.LabelFrame(parent, text="应答策略与请求观察", padding=6)
        right.pack(side="left", fill="both", expand=True, padx=4, pady=4)
        self.q_mode = tk.StringVar(value="normal")
        for txt, val in (("正常:命中即回显(未映射不回显)", "normal"),
                         ("空结果(模拟全部查无/超时链)", "empty"),
                         ("HTTP非200", "error")):
            ttk.Radiobutton(right, text=txt, value=val, variable=self.q_mode,
                            command=self._q_apply).pack(anchor="w")
        f3 = ttk.Frame(right)
        f3.pack(fill="x")
        ttk.Label(f3, text="延迟ms:").pack(side="left")
        self.q_delay = ttk.Entry(f3, width=7)
        self.q_delay.insert(0, "0")
        self.q_delay.pack(side="left")
        tk.Label(f3, text="(1300 可测 1s 发送超时；>5000 测查询超时重试)", fg="#999").pack(side="left")
        ttk.Button(f3, text="应用", command=self._q_apply).pack(side="left", padx=6)
        self.q_st = tk.Label(right, text="", justify="left", font=("Consolas", 9), fg="#222")
        self.q_st.pack(anchor="w", pady=4)
        ttk.Label(right, text="最近一次查询请求：").pack(anchor="w")
        self.q_last = tk.Text(right, height=8, state="disabled", font=("Consolas", 9))
        self.q_last.pack(fill="both", expand=True)
        tk.Label(right, text="应答结构: HTTP200 {data:{data:[{epc,barcode,...}]},...}（WCS 只认此嵌套；Authorization 头须与配置一致）",
                 fg="#999", justify="left", wraplength=460).pack(anchor="w")

    def _q_apply(self):
        try:
            delay = max(int(self.q_delay.get()), 0)
        except Exception:
            delay = 0
        self.query.set_rule(mode=self.q_mode.get(), delay_ms=delay,
                            http_code=500 if self.q_mode.get() == "error" else 200)
        self.store.add_event(f"RFID查询策略: {self.q_mode.get()} delay={delay}ms")

    def _q_add(self):
        epc = self.q_epc.get().strip()
        sku = self.q_sku.get().strip()
        if not epc or not sku:
            messagebox.showwarning("输入", "EPC 与 SKU 均必填")
            return
        self.query.add_mapping(epc, sku)
        self._q_apply()
        self._q_refresh_tree()

    def _q_del(self):
        sel = self.q_tree.selection()
        if not sel:
            return
        epc = self.q_tree.item(sel[0], "values")[0]
        with self.query.mapping_lock:
            self.query.mapping.pop(epc, None)
        self._q_refresh_tree()

    def _q_clear(self):
        self.query.set_mapping({})
        self._q_refresh_tree()

    def _q_load(self, kind):
        if kind == "clean":
            _w, m, _p = mock_proto.build_count_wave("PP2026TMP", grids=3, skus_per_grid=2, qty=2)
        else:
            m = mock_proto.sample_epc_map()
        self.query.set_mapping(m)
        self._q_refresh_tree()
        self.lbl_q_note.config(text=f"已载入 {len(m)} 条映射")

    def _q_refresh_tree(self):
        self.q_tree.delete(*self.q_tree.get_children())
        for epc, sku in sorted(self.query.get_mapping().items()):
            self.q_tree.insert("", "end", values=(epc, sku))

    # ------------------------------------------------------------------ 页: S7锁格
    def _pg_s7(self, parent):
        top = ttk.Frame(parent, padding=4)
        top.pack(fill="x")
        self.s7_st = tk.Label(top, text="", justify="left", font=("Consolas", 9), fg="#222")
        self.s7_st.pack(side="left")
        self.s7_st2 = tk.Label(top, text="", font=("Consolas", 9), fg="#00695c")
        self.s7_st2.pack(side="right")

        mid = ttk.LabelFrame(parent, text="格口锁格位图（点按=锁格/解锁，DB77 bit=格口号，WCS 1s轮询检测上升沿→H7满箱回传）", padding=6)
        mid.pack(fill="both", expand=True, padx=4, pady=2)
        self.s7_grid_frame = ttk.Frame(mid)
        self.s7_grid_frame.pack(fill="both", expand=True)
        self.s7_btns = {}
        for g in range(1, 67):
            b = tk.Button(self.s7_grid_frame, text=f"{g:02d}", width=4, relief="ridge",
                          command=lambda gg=g: self._s7_toggle(gg))
            b.grid(row=(g - 1) // 12, column=(g - 1) % 12, padx=2, pady=2, sticky="we")
            self.s7_btns[g] = b
        bottom = ttk.LabelFrame(parent, text="工具", padding=6)
        bottom.pack(fill="x", padx=4)
        f = ttk.Frame(bottom)
        f.pack(fill="x")
        ttk.Label(f, text="格口号:").pack(side="left")
        self.s7_spin = ttk.Spinbox(f, from_=1, to=66, width=5)
        self.s7_spin.set(1)
        self.s7_spin.pack(side="left")
        ttk.Button(f, text="锁格", command=lambda: self._s7_spin_act(True)).pack(side="left", padx=4)
        ttk.Button(f, text="解锁", command=lambda: self._s7_spin_act(False)).pack(side="left")
        ttk.Button(f, text="全部解锁", command=self._s7_unlock_all).pack(side="left", padx=12)
        ttk.Button(f, text="清空已锁(内存仅显示)", command=self._s7_clear_display).pack(side="left")
        self.s7_hex = tk.Label(bottom, text="", font=("Consolas", 8), fg="#333")
        self.s7_hex.pack(anchor="w", pady=2)
        tk.Label(bottom, text="说明：WCS 只读 DB77 从不回写；满箱后 WCS 禁用该格口，直到 WMS 重新 H6 绑定(自动恢复)或新波次开始。",
                 fg="#999").pack(anchor="w")

    def _s7_toggle(self, g):
        cur = self.s7.grid_state(g)
        self.s7.set_grid(g, not cur)

    def _s7_spin_act(self, lock):
        try:
            g = int(self.s7_spin.get())
        except Exception:
            g = 1
        self.s7.set_grid(g, lock)

    def _s7_unlock_all(self):
        for g in range(1, 200):
            self.s7.set_grid(g, False)

    def _s7_clear_display(self):
        # 仅解锁展示用（等同全解锁）
        self._s7_unlock_all()

    # ------------------------------------------------------------------ 页: 场景
    def _pg_scen(self, parent):
        top = ttk.LabelFrame(parent, text="标准全流程（H6→H4→人工开始分拣→RFID→PLC→锁格H7→人工结束任务H8）", padding=6)
        top.pack(fill="x", padx=4, pady=4)
        f = ttk.Frame(top)
        f.pack(fill="x")
        ttk.Label(f, text="格口数:").pack(side="left")
        self.sc_grids = ttk.Entry(f, width=4)
        self.sc_grids.insert(0, "3")
        self.sc_grids.pack(side="left", padx=2)
        ttk.Label(f, text="每格SKU:").pack(side="left")
        self.sc_skus = ttk.Entry(f, width=4)
        self.sc_skus.insert(0, "2")
        self.sc_skus.pack(side="left", padx=2)
        ttk.Label(f, text="每SKU件数:").pack(side="left")
        self.sc_qty = ttk.Entry(f, width=4)
        self.sc_qty.insert(0, "2")
        self.sc_qty.pack(side="left", padx=2)
        ttk.Label(f, text="orderCode:").pack(side="left")
        self.sc_oc = ttk.Entry(f, width=18)
        self.sc_oc.insert(0, "PP2026MOCK%03d" % (int(time.time()) % 1000))
        self.sc_oc.pack(side="left", padx=2)
        ttk.Label(f, text="节奏ms:").pack(side="left")
        self.sc_pace = ttk.Entry(f, width=5)
        self.sc_pace.insert(0, "350")
        self.sc_pace.pack(side="left", padx=2)
        self.btn_sc_run = ttk.Button(f, text="▶ 运行标准全流程", command=self._scen_start_std)
        self.btn_sc_run.pack(side="left", padx=8)
        tk.Label(f, text="（流程中每一步都在下方与报文总览呈现；需要你在 WCS 界面配合点按钮时会弹检查点）",
                 fg="#666").pack(side="left")

        d = ttk.LabelFrame(parent, text="异常演练（各自独立，可随时运行；部分需活动波次/分拣上下文）", padding=6)
        d.pack(fill="x", padx=4, pady=4)
        fd = ttk.Frame(d)
        fd.pack(fill="x")
        self.sc_drill = ttk.Combobox(fd, state="readonly", width=52)
        self.sc_drill.configure(values=[
            ("NOREAD 空读帧", "NOREAD"),
            ("未映射EPC(查询0命中→重试→SKU查询超时)", "unknown_epc"),
            ("查询延迟1300ms(>1s发送窗口拒发)", "query_delay"),
            ("落格反馈状态码=2(异常)", "fb_status23"),
            ("H7应答失败→Outbox重试(需活动波次,约40s观察)", "h7_fail"),
        ])
        self.sc_drill.current(0)
        self.sc_drill.pack(side="left")
        ttk.Button(fd, text="运行选中演练", command=self._scen_start_drill).pack(side="left", padx=6)
        self.lbl_drill_ctx = tk.Label(fd, text="", fg="#666")
        self.lbl_drill_ctx.pack(side="left")

        logf = ttk.LabelFrame(parent, text="场景日志 / 检查点", padding=6)
        logf.pack(fill="both", expand=True, padx=4, pady=4)
        self.scen_log = tk.Text(logf, height=10, state="disabled", font=("Consolas", 9))
        self.scen_log.pack(fill="both", expand=True)
        self.scen_cp = tk.Label(logf, text="（无进行中的场景）", fg="#00695c", wraplength=1200, justify="left", anchor="w")
        self.scen_cp.pack(fill="x", pady=4)
        cf = ttk.Frame(logf)
        cf.pack(fill="x")
        self.btn_cp_go = ttk.Button(cf, text="✔ 我已操作完成，继续", command=self._cp_go, state="disabled")
        self.btn_cp_go.pack(side="left")
        self.btn_cp_abort = ttk.Button(cf, text="✘ 中止场景", command=self._cp_abort, state="disabled")
        self.btn_cp_abort.pack(side="left", padx=8)
        self.lbl_scen_fin = tk.Label(cf, text="", fg="#888")
        self.lbl_scen_fin.pack(side="left", padx=10)

    def _scen_ctx(self):
        app = self

        class Ctx:
            cfg = app.cfg
            store = app.store

            def log(self, text, warn=False):
                app.store.add_event(("[场景] " + ("⚠" if warn else "") + text))
                app.ui_queue.append(("scen_log", text, warn))

            def checkpoint(self, text):
                app._scen_abort.clear()
                app._cp_text = text
                app._cp_ev.clear()
                app.ui_queue.append(("checkpoint", text))
                while not app._cp_ev.wait(0.2):
                    if app._scen_abort.is_set():
                        app.ui_queue.append(("checkpoint_off",))
                        return False
                app.ui_queue.append(("checkpoint_off",))
                return not app._scen_abort.is_set()

            def aborted(self):
                return app._scen_abort.is_set()

            def sleep(self, sec):
                end = time.time() + sec
                while time.time() < end:
                    if app._scen_abort.is_set():
                        raise mock_scenario.Abort()
                    time.sleep(0.1)

            def wait_until(self, pred, timeout, desc):
                end = time.time() + timeout
                while time.time() < end:
                    if app._scen_abort.is_set():
                        raise mock_scenario.Abort()
                    try:
                        if pred():
                            return True
                    except Exception:
                        pass
                    time.sleep(0.15)
                return False

            def http_post(self, path, obj):
                url = f"http://{app.wcs_ip}:{app.cfg.wms_listen_port}{path}"
                req = urllib.request.Request(url, data=json.dumps(obj, ensure_ascii=False).encode("utf-8"),
                                             headers={"Content-Type": "application/json"}, method="POST")
                app.store.add("WMS下发", "发", f"POST {path} (场景)", raw_text=json.dumps(obj, ensure_ascii=False, indent=1))
                t0 = time.time()
                try:
                    r = urllib.request.urlopen(req, timeout=8)
                    text = r.read().decode("utf-8", errors="replace")
                    code = r.status
                    r.close()
                except urllib.error.HTTPError as e:
                    text = e.read().decode("utf-8", errors="replace")
                    code = e.code
                except Exception as e:
                    app.store.add_event(f"[场景] HTTP异常 {path}: {e}")
                    return -1, str(e)
                ms = int((time.time() - t0) * 1000)
                app.store.add("WMS下发", "收", f"HTTP {code} ({ms}ms) (场景)", raw_text=text)
                return code, text

            def rfid_connected(self):
                return app.rfid.connected_client_count() > 0

            def set_auto_ack(self, on, status):
                app.auto_ack_scenario = on     # 场景级控制，不受界面刷新覆盖
                app.auto_ack_on = on
                app.auto_ack_status = status

            def rfid_send(self, epc, seq, note=""):
                raw = mock_proto.make_rfid_frame(seq, "01", epc, "LIT0D")
                return app.rfid.send_frame(raw, note=note)

            def plc_epc_seen(self, epc, since_ts):
                return any(t >= since_ts and e == epc for (e, t, _x) in app.plc_seen)

            def gw_last(self):
                return app.gw.last()

            def gw_set_rule(self, **kw):
                app.gw.set_rule(**kw)

            def query_set_rule(self, **kw):
                app.query.set_rule(**kw)

            def query_mapping(self, m):
                app.query.set_mapping(m)
                app.ui_queue.append(("q_refresh",))

            def s7_set_grid(self, g, lock):
                app.s7.set_grid(g, lock)

            def pick_mapped_piece(self):
                m = app.query.get_mapping()
                for epc, sku in m.items():
                    g = app.piece_grid.get(epc)
                    if g:
                        return epc, sku, g
                for epc, sku in list(m.items())[:1]:
                    return epc, sku, None
                return None, None, None
        return Ctx()

    def _scen_run_in_thread(self, fn):
        if getattr(self, "_scen_thread", None) and self._scen_thread.is_alive():
            messagebox.showinfo("场景进行中", "已有场景在运行，请先中止或等待完成")
            return
        self._scen_abort.clear()
        self.lbl_scen_fin.config(text="运行中…")
        t = threading.Thread(target=self._scen_wrap, args=(fn,), daemon=True)
        self._scen_thread = t
        t.start()

    def _scen_wrap(self, fn):
        try:
            out = fn()
            self.ui_queue.append(("scen_fin", str(out)))
        except mock_scenario.Abort:
            self.ui_queue.append(("scen_fin", "已中止"))
        except Exception as e:
            self.store.add_event(f"[场景] 异常终止: {e!r}")
            self.ui_queue.append(("scen_fin", f"异常终止: {e!r}"))
        finally:
            # ★ 场景结束（无论成功/中止/异常）复位场景级自动回执，避免影响后续手动操作
            self.auto_ack_scenario = False

    def _scen_start_std(self):
        try:
            plan = mock_scenario.StdPlan(grids=int(self.sc_grids.get()),
                                         skus_per_grid=int(self.sc_skus.get()),
                                         qty=int(self.sc_qty.get()),
                                         order_code=self.sc_oc.get().strip() or "PP2026MOCK001",
                                         pace_s=max(int(self.sc_pace.get()), 100) / 1000.0)
        except Exception as e:
            messagebox.showwarning("参数", f"参数错误: {e}")
            return
        self._scen_run_in_thread(lambda: self._scen_std_glue(plan))

    def _scen_std_glue(self, plan):
        ctx = self._scen_ctx()
        # 生成与登记映射/格口上下文，供演练使用
        _w, _m, pieces = mock_proto.build_count_wave(plan.order_code, grids=plan.grids,
                                                     skus_per_grid=plan.skus_per_grid, qty=plan.qty)
        self.piece_grid = {epc: g for (epc, _s, g) in pieces}
        self.store.add_event(f"[场景] 本波次件表已登记 {len(pieces)} 件（EPC→格口 供演练使用）")
        return mock_scenario.run_standard(ctx, plan)

    def _scen_start_drill(self):
        name = self.sc_drill.get().split("(")[0].strip()
        key = self.sc_drill.current()
        vals = ["NOREAD", "unknown_epc", "query_delay", "fb_status23", "h7_fail"]
        drill = vals[key] if 0 <= key < len(vals) else "NOREAD"
        grids = sorted({int(g) for g in self.piece_grid.values()})[:1]
        self._scen_run_in_thread(lambda: self._drill_glue(drill, grids))

    def _drill_glue(self, drill, grids):
        ctx = self._scen_ctx()
        return mock_scenario.run_drill(ctx, drill, context_grids=grids)

    # ------------------------------------------------------------------ 页: 配置
    def _pg_cfg(self, parent):
        left = ttk.LabelFrame(parent, text="配置文件（WCS 固定读取 exe 同目录 config/http_server.xml）", padding=6)
        left.pack(side="left", fill="both", expand=True, padx=4, pady=4)
        f = ttk.Frame(left)
        f.pack(fill="x")
        self.cfg_kind = tk.Label(f, text="", font=("Microsoft YaHei UI", 10, "bold"))
        self.cfg_kind.pack(side="left")
        ttk.Button(f, text="重新检测/重载", command=self._cfg_reload).pack(side="right")
        self.cfg_addr = tk.Text(left, height=20, state="disabled", font=("Consolas", 9))
        self.cfg_addr.pack(fill="both", expand=True, pady=4)
        f2 = ttk.Frame(left)
        f2.pack(fill="x")
        ttk.Button(f2, text="① 一键启用 Mock 配置", command=self._cfg_enable).pack(side="left")
        ttk.Button(f2, text="② 还原正式配置", command=self._cfg_restore).pack(side="left", padx=8)
        tk.Label(f2, text="操作后需重启 WCS_httpServer.exe", fg="#b26a00").pack(side="left")

        ref = ttk.LabelFrame(left, text="端口预期参考（run_mock.bat 指定 vs 当前配置文件 —— 手工改 http_server.xml 对照用）",
                             padding=6)
        ref.pack(fill="x", pady=(8, 0))
        self.ref_txt = tk.Text(ref, height=10, state="disabled", font=("Consolas", 9), wrap="none")
        self.ref_txt.pack(fill="x")
        self.ref_txt.tag_configure("head", foreground="#1b5e20",
                                   font=("Microsoft YaHei UI", 9, "bold"))
        self.ref_txt.tag_configure("ok", foreground="#0a7d32")
        self.ref_txt.tag_configure("bad", foreground="#b00020")
        self.ref_txt.tag_configure("gray", foreground="#777777")
        self.ref_note = tk.Label(ref, text="", fg="#888", justify="left", anchor="w",
                                 wraplength=760)
        self.ref_note.pack(anchor="w", pady=(3, 0))

        right = ttk.LabelFrame(parent, text="说明", padding=6)
        right.pack(side="left", fill="both", expand=True, padx=4, pady=4)
        info = (
            "地址均按 release_WcsHttpServer\\config\\http_server.xml 生效值工作；\n"
            "『① 一键启用 Mock 配置』= 把 mock_env\\config_mock\\http_server.xml（可自定义）\n"
            "备份当前文件后原样复制过去，内容以你自己维护的这份配置为准。\n\n"
            "模板当前指向本机仿真台的地址类节点（以实际文件为准）：\n"
            "  rfidPushServerIp=127.0.0.1:rfidPushServerPort(当前2010) ← 仿真台监听\n"
            "  rfidQueryUrl=http://127.0.0.1:9100/... ← 仿真台监听\n"
            "  feedbackTestUrl/EndTestUrl=http://127.0.0.1:8099/... (H7/H8) ← 仿真台监听\n"
            "  plcS7Ip=127.0.0.1（S7 端口固定 102，两侧不可配）\n"
            "  wmsListenPort(当前8191)/plcListenPort(当前102) ← WCS 监听\n"
            "  rfidHeartbeatEnable 与配置一致（当前0）；useTestEnv=1\n"
            "  binding 预绑定 001~065 随模板原样写入（WCS 启动即按此绑格口容器）\n\n"
            "⚠ plcListenPort 若与 S7 同为 102，会与仿真台 S7 监听互斥，联调建议改非 102\n\n"
            "备份位置：release_WcsHttpServer\\config\\config_backup\\*.bak（脚本自动产生，不删任何原文件）\n\n"
            "【调试换端口（仿真台绝不自动改写配置）】\n"
            "  1) 用 run_mock.bat 指定本次端口，如: run_mock.bat wms=9191 plc=2500 rfid=3010 gw=9099 query=9100\n"
            "  2) 看左侧『端口预期参考』表格（期望 vs 配置文件当前值）\n"
            "  3) 按表格手工修改 http_server.xml 对应节点（先备份），重启 WCS 与本台后生效\n"
            "  4) WCS 在另一台机器时加网络参数: run_mock.bat wcs=<WCS机IP> bind=0.0.0.0\n"
            "     (wcs=PLC/软件下发出站目标; bind=监听绑定地址, 跨机必填 0.0.0.0)\n"
            "  S7 锁格端口固定 102（snap7 协议），两侧均不可配")
        tk.Label(right, text=info, justify="left", fg="#333",
                 font=("Microsoft YaHei UI", 9)).pack(anchor="nw", fill="x")

    def _cfg_reload(self):
        self.cfg = mock_cfg.WcsConfig().load()
        self._refresh_cfg_view()
        self.store.add_event(f"配置已重新加载: {self.cfg.path} ok={self.cfg.ok}")

    def _cfg_enable(self):
        if not messagebox.askyesno("启用Mock配置", "将备份当前正式配置并替换为 mock 配置（不改动其它任何文件）。\n执行后需重启 WCS_httpServer.exe。\n继续？"):
            return
        code, msg = mock_swap_config.enable()
        messagebox.showinfo("结果", msg)
        self._cfg_reload()

    def _cfg_restore(self):
        if not messagebox.askyesno("还原正式配置", "将从 config_backup 最近备份还原正式配置。\n执行后需重启 WCS_httpServer.exe。\n继续？"):
            return
        code, msg = mock_swap_config.restore()
        messagebox.showinfo("结果", msg)
        self._cfg_reload()

    def _refresh_cfg_view(self):
        kind = mock_swap_config.current_kind()
        color = {"mock": "#00695c", "production": "#b26a00", "missing": "red"}.get(kind, "#333")
        self.cfg_kind.config(text=f"当前配置状态: {kind}", fg=color)
        if self.cfg.ok:
            self._txt(self.cfg_addr, self.cfg.addresses_summary() +
                      "\n\n全部地址是否指向本机: " + ("是 ✓" if self.cfg.all_local() else "否 ✗（请先『一键启用Mock配置』）"))
        # 端口预期参考（run_mock.bat 环境变量 → 对照当前配置文件）
        exp = self.expected_ports
        self.ref_txt.config(state="normal")
        self.ref_txt.delete("1.0", "end")
        for tag, line in port_reference_lines(self.cfg, exp):
            self.ref_txt.insert("end", line + "\n", tag)
        self.ref_txt.config(state="disabled")
        cur = self.cfg.port_summary() if self.cfg.ok else {}
        diffs = [k for k in exp if cur.get(k) != exp[k]]
        exp_txt = "  ".join("%s=%s" % (k, v) for k, v in exp.items()) or "（未指定）"
        remote = []
        if self.wcs_ip != "127.0.0.1":
            remote.append("wcs(目标WCS/PLC通道连它)=" + self.wcs_ip)
        if self.bind_host:
            remote.append("bind(监听绑定)=" + self.bind_host)
        net_txt = ("；远程: " + "  ".join(remote)) if remote else ""
        if diffs:
            self.ref_note.config(
                text=f"本次指定: {exp_txt}{net_txt} —— 与配置文件不一致项: {'、'.join(diffs)}。"
                     "仿真台仍按当前配置文件运行：如需按本次期望联调，请停止 WCS 后手工修改 "
                     "release_WcsHttpServer\\config\\http_server.xml 对应节点（备份留档），再重启 WCS 与本台。"
                     "S7=102 固定。",
                fg="#b00020")
        else:
            self.ref_note.config(
                text=f"本次指定: {exp_txt}{net_txt}。修改 http_server.xml 后需重启 WCS_httpServer.exe 才生效；"
                     "S7=102 固定。本台绝不自动改写配置文件，全部端口请手工修改。",
                fg="#888")

    # ======================================================================
    # 工具方法
    # ======================================================================
    @staticmethod
    def _txt(w, text):
        w.config(state="normal")
        w.delete("1.0", "end")
        w.insert("1.0", text)
        w.config(state="disabled")

    @staticmethod
    def _txt_ed(w, text):
        w.delete("1.0", "end")
        w.insert("1.0", text)

    def _apply_initial_mapping(self):
        w, m, p = mock_proto.build_count_wave("PP2026TMP", grids=3, skus_per_grid=2, qty=2)
        self.query.set_mapping(m)
        self.piece_grid = {epc: g for (epc, _s, g) in p}
        self._q_refresh_tree()

    def _all_start(self):
        for name, svc in self.services:
            try:
                svc.start()
            except Exception as e:
                self.store.add_event(f"{name} 启动异常: {e}")

    def _all_stop(self):
        for name, svc in self.services:
            try:
                svc.stop()
            except Exception:
                pass
        self.store.add_event("全部服务已停止")

    def _checkpoint_set(self, text):
        self.scen_cp.config(text="【检查点】" + text, fg="#b26a00")
        self.btn_cp_go.config(state="normal")
        self.btn_cp_abort.config(state="normal")

    def _checkpoint_off(self):
        self.scen_cp.config(text="（无进行中的检查点）", fg="#00695c")
        self.btn_cp_go.config(state="disabled")
        self.btn_cp_abort.config(state="disabled")

    def _cp_go(self):
        self._cp_ev.set()

    def _cp_abort(self):
        self._scen_abort.set()
        self._cp_ev.set()
        self._checkpoint_off()

    # ======================================================================
    # 周期刷新
    # ======================================================================
    def _tick(self):
        if self._closing:
            return
        try:
            self._drain_ui_queue()
            self._refresh_status()
            self._refresh_msg()
            self._refresh_s7()
            self._refresh_rfid()
            self._refresh_gw()
            self._refresh_plc()
            self._refresh_query()
            if time.time() - self._ov_at > 2.0:
                self._ov_at = time.time()
                self._refresh_overview()
        except Exception as e:
            self.store.add_event(f"UI刷新异常: {e!r}")
        self._tick_after = self.after(300, self._tick)

    def _drain_ui_queue(self):
        n = 0
        while self.ui_queue and n < 200:
            n += 1
            try:
                item = self.ui_queue.popleft()
            except IndexError:
                break
            kind = item[0]
            if kind == "wm_resp":
                self._txt(self.wm_resp, item[1])
            elif kind == "bind_done":
                self.lbl_bind_st.config(text=item[1])
            elif kind == "scen_log":
                tag = "warn" if item[2] else None
                self.scen_log.config(state="normal")
                self.scen_log.insert("end", ("⚠ " if item[2] else "") + item[1] + "\n", tag)
                self.scen_log.config(state="disabled")
                self.scen_log.see("end")
            elif kind == "checkpoint":
                self._checkpoint_set(item[1])
            elif kind == "checkpoint_off":
                self._checkpoint_off()
            elif kind == "scen_fin":
                self.lbl_scen_fin.config(text=item[1])
                self._checkpoint_off()
                self.last_scen_report = item[1]
            elif kind == "q_refresh":
                self._q_refresh_tree()

    def _wcs_running(self):
        now = time.time()
        if now - self.wcs_proc_check_at < 3.0:
            return self.wcs_proc_detect
        self.wcs_proc_check_at = now
        try:
            out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq WCS_httpServer.exe", "/NH"],
                                 capture_output=True, text=True, timeout=5,
                                 creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            self.wcs_proc_detect = "WCS_httpServer.exe" in (out.stdout or "")
        except Exception:
            pass
        return self.wcs_proc_detect

    def _refresh_status(self):
        wcs = self._wcs_running()
        parts = [f"WCS进程: {'● 运行中' if wcs else '○ 未运行'}"]
        for name, svc in self.services:
            st = svc.status() if hasattr(svc, "status") else None
            if st is None:
                parts.append(f"{name}:--")
                continue
            run = "●" if st["running"] else "○"
            det = ""
            if name == "RFID推送服务端":
                with svc.clients_lock:
                    n = len(svc.clients)
                det = f" 已连WCS:{n}"
            elif name == "PLC客户端":
                det = " 已连接" if svc.connected else " 未连接"
            elif name == "S7锁格模拟":
                snap = svc.snapshot()
                det = f" 读:{snap['counters']['reads']}"
            elif name == "WMS网关接收":
                lk = svc.last()
                det = f" H7:{lk.get('h7_count', 0)} H8:{lk.get('h8_count', 0)}"
            parts.append(f"{run}{name}{det}")
        fg = "#0a7d32" if (wcs and self._svcs_all_ok()) else "#333333"
        self.lbl_status.config(text=" | ".join(parts), fg=fg)

    def _svcs_all_ok(self):
        want = {"RFID推送服务端", "PLC客户端", "S7锁格模拟", "WMS网关接收", "RFID查询服务"}
        for name, svc in self.services:
            if name not in want:
                continue
            st = svc.status() if hasattr(svc, "status") else None
            if not st or not st["running"]:
                return False
        return True

    def _refresh_msg(self):
        try:
            new_rows, _earliest = self.store.rows_since(self._msg_applied_seq)
        except Exception:
            return
        if not new_rows:
            return
        for r in new_rows:
            if self._msg_pass_filter(r):
                self._msg_insert(r)
        self._msg_applied_seq = max(r["seq"] for r in new_rows)
        self._msg_cnt_label()

    def _msg_pass_filter(self, r):
        ch = self.f_ch.get()
        if ch != "全部" and r["channel"] != ch:
            return False
        dr = self.f_dir.get()
        if dr != "全部" and r["dir"] != dr:
            return False
        kw = self.f_kw.get().strip().lower()
        if kw:
            blob = (r["summary"] + r["raw_text"] + r["parsed"] + r["ref"]).lower()
            if kw not in blob:
                return False
        return True

    def _refresh_rfid(self):
        svc = self.rfid
        st = svc.status()
        with svc.clients_lock:
            n = len(svc.clients)
        counter = st["counters"]
        age = ""
        if st["last_io"]:
            age = f" 最近IO {time.time() - st['last_io']:.1f}s前"
        text = (f"运行: {'是' if st['running'] else '否'}  监听: {svc.host}:{svc.port}\n"
                f"WCS连接: {n}  累计接入: {counter.get('accepts', 0)}  收帧: {counter.get('recv', 0)}{age}")
        self.rfid_st.config(text=text, fg="#0a7d32" if st["running"] else "#b00020")

    def _refresh_plc(self):
        st = self.plc.status()
        conn = self.plc.connected
        text = (f"连接: {'已连接 WCS' if conn else '未连接(自动重连中)'}  {self.plc.host}:{self.plc.port}\n"
                f"收帧: {st['counters'].get('recv', 0)}  发帧: {st['counters'].get('sent', 0)}")
        if conn and self.plc.connected_since:
            text += f"\n保持连接: {int(time.time() - self.plc.connected_since)}s"
        self.plc_st.config(text=text, fg="#0a7d32" if conn else "#b00020")
        # 最近指令显示（取最近一条3字段帧）
        for e, t, raw in reversed(self.plc_seen):
            if raw.count("|") == 2:
                f3 = raw.split("|")
                self._txt(self.plc_last, f"{time.strftime('%H:%M:%S', time.localtime(t))} 指令: {{EPC={f3[0]} | 格口={f3[1]} | 小车={f3[2]}}}")
                break
        else:
            if self.plc_seen:
                e, t, raw = self.plc_seen[-1]
                self._txt(self.plc_last, f"{time.strftime('%H:%M:%S', time.localtime(t))} 反馈: {{{raw}}}")
        self._plc_apply_auto()

    def _refresh_gw(self):
        st = self.gw.status()
        lk = self.gw.last()
        rule = self.gw.get_rule()
        mode_txt = {"auto": "自动-成功", "auto_fail": "自动-失败", "custom": "自定义", "manual": "人工队列"}.get(
            rule["mode"], rule["mode"])
        self.gw_st.config(text=f"运行: {'是' if st['running'] else '否'}  监听 {self.gw.host}:{self.gw.port}\n"
                               f"策略: {mode_txt}  delay={rule.get('delay_ms', 0)}ms")
        self.gw_h78.config(text=f"H7 满箱回传: {lk.get('h7_count', 0)}    H8 完结回传: {lk.get('h8_count', 0)}")
        body = lk.get("h7") or lk.get("h8")
        if body and body != getattr(self, "_gw_last_body_shown", None):
            self._gw_last_body_shown = body
            self._txt(self.gw_last_body, body[:4000])
        # 人工应答队列
        with self.gw.pending_lock:
            pend = list(self.gw.pending)
        cur = list(self.gw_q_list.get(0, "end"))
        newval = [f"#{p['id']} [{p['kind']}] {p['time']} len={len(p['body'])}" for p in pend]
        if cur != newval:
            self.gw_q_list.delete(0, "end")
            for v in newval:
                self.gw_q_list.insert("end", v)

    def _refresh_s7(self):
        snap = self.s7.snapshot()
        run = snap["running"]
        age_txt = ""
        if snap["last_read_at"]:
            age_txt = f"  距上次WCS读DB77: {time.time() - snap['last_read_at']:.1f}s（每1s一次=通道活跃）"
        self.s7_st.config(text=f"运行: {'是' if run else '否'}  监听 {self.s7.host}:102  接入: {snap['counters']['accepts']}  "
                               f"读请求: {snap['counters']['reads']}  写: {snap['counters']['writes']}{age_txt}")
        locked = []
        db = snap["db77"]
        for g in range(1, 67):
            locked.append(bool(db[g // 8] & (1 << (g % 8))))
        for g, b in self.s7_btns.items():
            if locked[g - 1]:
                b.config(bg="#e57373", fg="white", text=f"{g:02d}锁")
            else:
                b.config(bg="#e8f5e9", fg="#1b5e20", text=f"{g:02d}")
        nlock = sum(locked)
        self.s7_st2.config(text=f"已锁格口: {nlock}")
        self.s7_hex.config(text="DB77(25B)=" + db.hex(" ").upper())

    def _refresh_query(self):
        st = self.query.status()
        rule = self.query.get_rule()
        stats = self.query.stats()
        qlast = (stats["last_q"] or "")[:400]
        self.q_st.config(text=f"运行: {'是' if st['running'] else '否'}  监听 {self.query.host}:{self.query.port}\n"
                              f"请求数: {stats['qcount']}  策略: {rule.get('mode')} delay={rule.get('delay_ms', 0)}ms\n"
                              f"映射条目: {len(self.query.get_mapping())}  期望Authorization: {self.cfg.rfid_appkey or '(空,不校验)'}")
        if qlast and qlast != getattr(self, "_q_last_shown", None):
            self._q_last_shown = qlast
            self._txt(self.q_last, qlast)

    def _refresh_overview(self):
        cfg_txt = self.cfg.addresses_summary() if self.cfg.ok else self.cfg.error
        if cfg_txt != getattr(self, "_ov_cfg_last", None):
            self._ov_cfg_last = cfg_txt
            self._txt(self.ov_cfg, cfg_txt)
        svc_lines = []
        wcs = self._wcs_running()
        svc_lines.append(f"WCS_httpServer 进程: {'运行中' if wcs else '未运行（请先启动 WCS 或检查路径）'}")
        for name, svc in self.services:
            if name == "RFID推送服务端":
                n = svc.connected_client_count()
                svc_lines.append(f"RFID推送服务端: {'运行' if svc.status()['running'] else '停止'} (WCS已连:{n}) {svc.host}:{svc.port}")
            elif name == "PLC客户端":
                svc_lines.append(f"PLC客户端: {'已连接' if svc.connected else '未连接(自动重连)'} {svc.host}:{svc.port}")
            elif name == "S7锁格模拟":
                snap = svc.snapshot()
                svc_lines.append(f"S7锁格模拟: {'运行' if snap['running'] else '停止'} (接入{snap['counters']['accepts']} 读{snap['counters']['reads']}) {svc.host}:102")
            elif name == "WMS网关接收":
                lk = svc.last()
                svc_lines.append(f"WMS网关接收: {'运行' if svc.status()['running'] else '停止'} H7:{lk.get('h7_count', 0)} H8:{lk.get('h8_count', 0)} {svc.host}:{svc.port}")
            else:
                svc_lines.append(f"{name}: {'运行' if svc.status()['running'] else '停止'} {svc.host}:{svc.port}")
        self._txt(self.ov_svc, "\n".join(svc_lines))

    def _on_close(self):
        self._closing = True
        if self._tick_after:
            self.after_cancel(self._tick_after)
        self._scen_abort.set()
        self._cp_ev.set()
        for name, svc in self.services:
            try:
                svc.stop()
            except Exception:
                pass
        try:
            self.store.add_event("仿真台退出")
        except Exception:
            pass
        try:
            self.store.close()
        except Exception:
            pass
        self.destroy()


def main():
    if any(a in ("check", "--check") for a in sys.argv[1:]):
        # 无界面模式: 打印端口预期 vs 当前配置文件 对照（供 run_mock.bat check 使用）
        cfg = mock_cfg.WcsConfig().load()
        for _tag, line in port_reference_lines(cfg, resolve_expected_ports()):
            print(line)
        return
    exp = resolve_expected_ports()
    if exp:
        print("[run_mock] 端口参数: %s  （仅对照参考，见『配置』页；配置文件请手工修改）"
              % "  ".join("%s=%s" % (k, exp[k]) for k in ("wms", "plc", "rfid", "gw", "query") if k in exp))
    app = MockConsole()
    app.mainloop()


if __name__ == "__main__":
    main()
