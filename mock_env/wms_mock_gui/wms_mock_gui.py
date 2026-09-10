# -*- coding: utf-8 -*-
"""
wms_mock_gui.py — WMS 报文 Mock 测试台（鞋服窄带分拣 WCS<->WMS 接口联调用）
================================================================================
纯 Python 标准库，无第三方依赖；整个目录复制到任意 Windows 机器即可运行。

功能：
  1. 报文编辑·发送（可扮演两个方向）
       WCS→WMS : H7 满箱回传 / H8 波次完结回传（gwisSubProductClassifyOrder/EndOrder）
       WMS→WCS : H4 波次下发 InsertWaveInfo / H6 格口容器绑定 BindingLatticePort
     输入 SKU 码与格口号等 → 自动生成与生产一致的报文 JSON（可修改）→
     发送到可配置的接口地址（本机模拟网关 / WMS 测试环境 / WMS 正式环境 / WCS 服务），
     显示生产口径的响应与判定。
  2. 内置模拟 WMS 网关（默认 127.0.0.1:8099，路径任意，与 WCS 正式配置的
     feedbackTestUrl 一致）：接收真实 WCS 发来的 H7/H8，按可配置业务规则引擎校验，
     报文正确 → 应答 {"success":true,"body":"产品分类框号库位完成分类--同步成功!","ts":"…"}
     错误 → 应答 {"success":false,"body":"<原因>","ts":"…"}（与生产网关响应字段一致，
     WCS 侧只认 success==true）。
  3. 校验规则与主数据（期望AppKey/method、格口编码、绑定表、波次计划库、SKU主档）
     可在界面维护，自动保存到同目录 wms_mock_state.json。

用法:
  python wms_mock_gui.py                  # 启动并自动打开浏览器
  python wms_mock_gui.py --gui-port 9000 --gw-port 8099 --no-browser
运行后: GUI 页 http://127.0.0.1:<gui_port>；模拟网关监听 <gw_host>:<gw_port>。
"""
import json
import os
import re
import socket
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import webbrowser
from collections import deque
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

APP_NAME = "WMS 报文 Mock 测试台"

# ── 运行目录解析：源码运行 / PyInstaller 单文件 EXE 均可用 ──
FROZEN = bool(getattr(sys, "frozen", False))
if FROZEN:
    APP_DIR = os.path.dirname(os.path.abspath(sys.executable))   # EXE 所在目录（状态文件放这里）
    RES_DIR = getattr(sys, "_MEIPASS", APP_DIR)                   # 打包资源解压目录
else:
    APP_DIR = os.path.dirname(os.path.abspath(__file__))
    RES_DIR = APP_DIR
BASE_DIR = APP_DIR
STATE_FILE = os.path.join(APP_DIR, "wms_mock_state.json")
LOG_FILE = os.path.join(APP_DIR, "wms_mock_gui.log") if FROZEN else None
if os.path.exists(os.path.join(APP_DIR, "webui.html")):           # EXE 旁可放自定义 webui.html
    WEBUI_FILE = os.path.join(APP_DIR, "webui.html")
else:
    WEBUI_FILE = os.path.join(RES_DIR, "webui.html")
LOG_MAX = 300

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass


def _frozen_excepthook(tp, val, tb):
    """EXE(无控制台)模式下把未捕获异常写入日志，便于客户机排查"""
    import traceback
    try:
        if LOG_FILE:
            with open(LOG_FILE, "a", encoding="utf-8") as f:
                f.write("\n[未捕获异常]\n")
                traceback.print_exception(tp, val, tb, file=f)
    except Exception:
        pass
    try:
        if sys.__excepthook__ is not None:
            sys.__excepthook__(tp, val, tb)
    except Exception:
        pass


if FROZEN:
    sys.excepthook = _frozen_excepthook
    threading.excepthook = _frozen_excepthook

# ────────────────────────────────────────────────────────────────
# 常量（与生产/测试环境接口一致，见 release config http_server.xml / define.h）
# ────────────────────────────────────────────────────────────────
PATH_H7 = "/gwms5/service/openapi/product/skuClassificationTask/gwisSubProductClassifyOrder"
PATH_H8 = "/gwms5/service/openapi/productClasTask/gwisSubProductClassifyEndOrder"
PATH_H4 = "/api/DispatchSortingCommand/InsertWaveInfo"
PATH_H6 = "/api/DispatchSortingCommand/BindingLatticePort"
APPKEY_TEST = "dz_bxh_dmwcs_cs"      # 测试环境（gwms5 openapi 账号）
APPKEY_PROD = "dz_bxh_wcs_zs"        # 正式环境
METHOD_H7 = "gwisSubProductClassifyOrder"
METHOD_H8 = "gwisSubProductClassifyEndOrder"

REPLY_H7_OK_BODY = "产品分类框号库位完成分类--同步成功!"
REPLY_H8_OK_BODY = "产品分类任务完结--同步成功!"

RULES = [
    ("r_structure", "报文结构与必填字段",
     "head/orderCode/detailList(或sumLocation)缺失、sku为空、qty非正整数等 → 拒绝", True),
    ("r_method", "接口方法 method 校验",
     "URL 查询参数 method 必须与 H7/H8 期望值一致", True),
    ("r_appkey", "身份鉴权 AppKey",
     "URL 参数与 Header 的 AppKey 必须都存在且等于「期望AppKey」", True),
    ("r_wave", "波次计划校验",
     "orderCode 必须是本模拟WMS已下发过的波次（计划库中存在，H4 下发或手工导入）", True),
    ("r_grid", "格口号校验",
     "格口号须在 1~66 内且属于该波次明细（H4 格口号自动记录）", True),
    ("r_bind", "容器绑定校验",
     "格口已绑定容器，且明细 targetLocation = 绑定容器号（H6 绑定或手工维护）", True),
    ("r_sku", "SKU 计划校验",
     "明细 SKU 必须在该格口的波次计划中", True),
    ("r_cap", "数量上限校验",
     "明细 qty 不得超过该格口该 SKU 的计划数量（模拟生产拒超收）", True),
    ("r_skumaster", "SKU 主档校验",
     "明细 SKU 必须在自动汇总的 SKU 主档中（默认关闭）", False),
]


# ────────────────────────────────────────────────────────────────
# 工具
# ────────────────────────────────────────────────────────────────
def now_ts(ms=True):
    s = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    return s + (":000" if ms else "")


def clock_ms():
    return time.strftime("%H:%M:%S") + ".%03d" % (int(time.time() * 1000) % 1000)


def ts_stamp():
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]


def p(*a):
    text = " ".join(str(x) for x in a)
    try:
        print(text, flush=True)
    except Exception:
        pass
    if LOG_FILE:
        try:
            with open(LOG_FILE, "a", encoding="utf-8") as f:
                f.write(ts_stamp() + "  " + text + "\n")
        except Exception:
            pass


def default_presets():
    return [
        {"name": "本机模拟WMS网关(默认8099)", "base": "http://127.0.0.1:8099",
         "role": "wms", "appkey": APPKEY_TEST, "env": "本地模拟"},
        {"name": "WMS测试环境 wmstest.pelliot.com.cn:9090", "base": "https://wmstest.pelliot.com.cn:9090",
         "role": "wms", "appkey": APPKEY_TEST, "env": "测试"},
        {"name": "WMS正式环境 wms.pelliot.com.cn", "base": "https://wms.pelliot.com.cn",
         "role": "wms", "appkey": APPKEY_PROD, "env": "正式"},
        {"name": "本机WCS服务(8191)", "base": "http://127.0.0.1:8191",
         "role": "wcs", "appkey": "", "env": "本地WCS"},
        {"name": "自定义", "base": "http://127.0.0.1:8099", "role": "wms",
         "appkey": APPKEY_TEST, "env": "自定义"},
    ]


def default_bindings():
    return [{"grid": g, "box": "H-01-2%03d" % g} for g in range(1, 7)]


def demo_wave_items():
    """与 mock_env 示例一致的干净波次：格口1~6 × 每格2个SKU × 每SKU1件"""
    items = []
    for g in range(1, 7):
        for k in range(2):
            sku = "SK%013d" % (g * 1000 + k + 1)
            items.append({"inco": sku, "gridNum": str(g), "gridNumber": 1, "gridType": "0"})
    return items


def default_state():
    return {
        "config": {
            "appkey_expected": APPKEY_TEST,   # 模拟WMS网关鉴权期望值（WCS mock 配置 appkeyTest 同值）
            "method_h7": METHOD_H7,
            "method_h8": METHOD_H8,
            "grid_prefix": "22",              # 对外格口编码前缀（5 → "22005"；留空=仅补零）
            "grid_width": 3,
            "grid_max": 66,
            "warehouse": "H",
            "goods_owner": "BXH_CS",
            "order_type": "01",
            "operuser_code": "admin",
            "operuser_name": "管理员",
            "from_location_default": "A-01-02",
            "gw_host": "127.0.0.1",
            "gw_port": 8099,
            "gui_port": 8765,
            "send_timeout_s": 15,
        },
        "rules": [[rid, label, default_on, desc]
                  for (rid, label, desc, default_on) in RULES],
        "gateway_mode": "auto",               # auto | force_success | force_fail
        "bindings": default_bindings(),
        "waves": [{"orderCode": "PP202600000099", "sobi": "H-01-AB",
                   "items": demo_wave_items(), "pushed_at": ""}],
        "presets": default_presets(),
    }


class App(object):
    """全局共享状态（线程安全）"""

    def __init__(self):
        self.lock = threading.RLock()
        self.state = default_state()
        self.log_gw = deque(maxlen=LOG_MAX)     # 模拟网关收到的请求
        self.log_sent = deque(maxlen=LOG_MAX)   # 发送历史
        self.gw_server = None
        self.gw_thread = None
        self.gw_last_error = ""
        self.gw_h7 = 0
        self.gw_h8 = 0
        self.gw_unknown = 0
        self.load_state()

    # ---------------- 持久化 ----------------
    def load_state(self):
        try:
            if os.path.exists(STATE_FILE):
                with open(STATE_FILE, "r", encoding="utf-8") as f:
                    saved = json.load(f)
                d = default_state()
                for sec in ("config", "presets"):
                    if isinstance(saved.get(sec), dict if sec == "config" else list):
                        d[sec] = saved[sec]
                if isinstance(saved.get("rules"), list) and saved["rules"]:
                    d["rules"] = saved["rules"]
                if isinstance(saved.get("bindings"), list):
                    d["bindings"] = [b for b in saved["bindings"]
                                     if isinstance(b, dict) and b.get("grid") is not None]
                if isinstance(saved.get("waves"), list):
                    d["waves"] = [w for w in saved["waves"]
                                  if isinstance(w, dict) and w.get("orderCode")]
                if isinstance(saved.get("gateway_mode"), str):
                    d["gateway_mode"] = saved["gateway_mode"]
                self.state = d
                p("[状态] 已从 %s 加载配置" % os.path.basename(STATE_FILE))
        except Exception as e:
            p("[状态] 加载失败，使用默认配置:", e)

    def save_state(self):
        try:
            with self.lock:
                with open(STATE_FILE, "w", encoding="utf-8") as f:
                    json.dump(self.state, f, ensure_ascii=False, indent=1)
        except Exception as e:
            p("[状态] 保存失败:", e)

    def upsert_wave(self, wave):
        """H4 下发成功（或手工导入）后记录波次到计划库（同 orderCode 覆盖）"""
        order = str((wave or {}).get("orderCode") or "").strip()
        if not order:
            return
        items = []
        for it in (wave.get("items") or []):
            if not isinstance(it, dict):
                continue
            items.append({
                "inco": str(it.get("inco") or "").strip(),
                "gridNum": str(it.get("gridNum") or "").strip(),
                "gridNumber": int_safe(it.get("gridNumber"), 1),
                "gridType": str(it.get("gridType") or "0"),
            })
        rec = {"orderCode": order, "sobi": str(wave.get("sobi") or "").strip(),
               "items": items, "pushed_at": ts_stamp()}
        with self.lock:
            for idx, w in enumerate(self.state["waves"]):
                if str(w.get("orderCode", "")).strip() == order:
                    self.state["waves"][idx] = rec
                    break
            else:
                self.state["waves"].append(rec)
        self.save_state()

    # ---------------- 数据辅助 ----------------
    def cfg(self, key, default=None):
        return self.state["config"].get(key, default)

    def rule_on(self, rid):
        for r in self.state["rules"]:
            if r[0] == rid:
                return bool(r[2])
        return False

    def find_wave(self, order_code):
        for w in self.state["waves"]:
            if str(w.get("orderCode", "")).strip() == str(order_code).strip():
                return w
        return None

    def binding_map(self):
        m = {}
        for b in self.state["bindings"]:
            try:
                m[int(b["grid"])] = str(b.get("box", "")).strip()
            except Exception:
                pass
        return m

    def wave_plans(self, wave):
        """(格口int, sku) -> 计划数量；同时返回该波次的格口集合"""
        plans = {}
        grids = set()
        prefix = self.cfg("grid_prefix", "")
        for it in wave.get("items", []):
            gi = parse_grid_code(it.get("gridNum", ""), prefix)
            sku = str(it.get("inco", "")).strip()
            if gi and sku:
                plans[(gi, sku)] = plans.get((gi, sku), 0) + int_safe(it.get("gridNumber"), 1)
                grids.add(gi)
        return plans, grids

    def sku_master(self):
        s = set()
        for w in self.state["waves"]:
            for it in w.get("items", []):
                v = str(it.get("inco", "")).strip()
                if v:
                    s.add(v)
        return sorted(s)

    def grid_encode(self, n):
        cfg = self.state["config"]
        w = int(cfg.get("grid_width") or 3)
        return str(cfg.get("grid_prefix") or "") + str(int(n)).zfill(w)

    def add_log_gw(self, e):
        with self.lock:
            self.log_gw.append(e)

    def add_log_sent(self, e):
        with self.lock:
            self.log_sent.append(e)

    def snapshot(self):
        with self.lock:
            cfg = dict(self.state["config"])
            cfg["gw_running"] = self.gw_server is not None
            cfg["gw_last_error"] = self.gw_last_error
            cfg["gw_h7"], cfg["gw_h8"], cfg["gw_unknown"] = self.gw_h7, self.gw_h8, self.gw_unknown
            return {
                "config": cfg,
                "rules": self.state["rules"],
                "gateway_mode": self.state["gateway_mode"],
                "bindings": self.state["bindings"],
                "waves": self.state["waves"],
                "presets": self.state["presets"],
                "sku_master": self.sku_master(),
                "log_gw": list(self.log_gw),
                "log_sent": list(self.log_sent),
            }


def int_safe(v, default=None):
    try:
        return int(float(str(v).strip()))
    except Exception:
        return default


def parse_grid_code(s, prefix=""):
    """WMS 格口编码 → 内部格口号 int；兼容 "22005"/"005"/"5"（与 WCS WmsGridCode.h 口径一致）"""
    if s is None:
        return None
    t = str(s).strip()
    if not t:
        return None
    if prefix and t.startswith(prefix):
        t = t[len(prefix):]
    if not t:
        return None
    try:
        v = int(t)
        return v if v > 0 else None
    except Exception:
        return None


def grid_display(raw, prefix):
    t = str(raw).strip()
    if prefix and t.startswith(prefix):
        t = t[len(prefix):]
    return t.lstrip("0") or "0"


def grid_code_safe(s, cfg):
    """UI 辅助：任意写法归一为对外编码"""
    g = parse_grid_code(s, cfg.get("grid_prefix", ""))
    if g is None:
        g = int_safe(str(s).strip().lstrip("0"))
    if g is None:
        return s
    w = int(cfg.get("grid_width") or 3)
    return str(cfg.get("grid_prefix") or "") + str(g).zfill(w)


def make_html_escape(text):
    if not isinstance(text, str):
        text = json.dumps(text, ensure_ascii=False)
    return (text.replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;").replace('"', "&quot;"))


def pretty_json(text):
    try:
        return json.dumps(json.loads(text), ensure_ascii=False, indent=2)
    except Exception:
        return text


def trunc(text, n=2000):
    text = str(text or "")
    return text if len(text) <= n else text[:n] + "…[截断]"


# ════════════════════════════════════════════════════════════════
# 模拟 WMS 网关（接收 H7/H8，按规则引擎应答）
# ════════════════════════════════════════════════════════════════
def gateway_respond(app, path, query, headers_lc, body_text):
    """返回 (resp_obj, checks, kind, verdict_text)"""
    cfg = app.state["config"]
    q = {k: v for k, v in query.items()}
    qk = q.get("appkey", "")
    hk = (headers_lc.get("appkey") or "").strip()
    meth = q.get("method", "")
    mh7, mh8 = cfg.get("method_h7"), cfg.get("method_h8")
    kind = "H8" if meth == mh8 else ("H7" if meth == mh7 else "未知")
    mode = app.state.get("gateway_mode", "auto")
    checks = []
    resp = None

    def rule_mark(rid, ok, msg=""):
        label = next((r[1] for r in app.state["rules"] if r[0] == rid), rid)
        enabled = app.rule_on(rid)
        checks.append({"rule": rid, "label": label, "on": enabled,
                       "ok": ok if enabled else None, "msg": msg})

    # 强制模式：先于规则
    if mode in ("force_success", "force_fail"):
        ok_body = REPLY_H7_OK_BODY if kind == "H7" else REPLY_H8_OK_BODY
        if kind == "未知":
            ok_body = REPLY_H7_OK_BODY
        success = mode == "force_success"
        body = ok_body if success else "模拟失败: " + ok_body
        resp = {"success": success, "body": body, "ts": now_ts(True)}
        note = "应答策略=强制%s（跳过规则校验）" % ("成功" if success else "失败")
        rule_mark("r_structure", True, note)
        return resp, checks, kind, note

    # ---- 1) JSON 结构 ----
    obj = None
    parse_err = ""
    try:
        obj = json.loads(body_text) if body_text.strip() else None
    except Exception as e:
        obj = None
        parse_err = str(e)
    if not isinstance(obj, dict):
        note = "报文格式错误：body 非合法 JSON（%s）" % (parse_err or "空body")
        rule_mark("r_structure", False, note)
        resp = {"success": False, "body": "同步失败：" + note, "ts": now_ts(True)}
        return resp, checks, kind, note

    head = obj.get("head")
    fail = []
    if not isinstance(head, dict):
        fail.append("缺少 head 对象")
    else:
        if not str(head.get("orderCode") or "").strip():
            fail.append("head.orderCode 缺失/为空")
    if kind == "H7":
        dl = head.get("detailList") if isinstance(head, dict) else None
        if not isinstance(dl, list) or not dl:
            fail.append("head.detailList 缺失或为空")
        else:
            for i, it in enumerate(dl, 1):
                if not isinstance(it, dict):
                    fail.append("detailList[%d] 非对象" % i)
                    continue
                if not str(it.get("num") or "").strip():
                    fail.append("detailList[%d] num(格口号)缺失" % i)
                if not str(it.get("targetLocation") or "").strip():
                    fail.append("detailList[%d] targetLocation(容器号)缺失" % i)
                if not str(it.get("sku") or "").strip():
                    fail.append("detailList[%d] sku 缺失" % i)
                if int_safe(it.get("qty"), -1) < 1:
                    fail.append("detailList[%d] qty 非法(须为正整数)" % i)
    else:  # H8 / 未知按H8宽松处理
        if isinstance(head, dict) and str(head.get("sumLocation") or "").strip():
            if int_safe(head.get("sumLocation"), None) is None:
                fail.append("head.sumLocation 非数字")
        else:
            fail.append("head.sumLocation 缺失")
    if fail:
        note = "结构校验失败：" + "；".join(fail)
        rule_mark("r_structure", False, note)
        resp = {"success": False, "body": "同步失败：" + note, "ts": now_ts(True)}
        return resp, checks, kind, note
    rule_mark("r_structure", True, "结构完整")

    # ---- 2) method ----
    if kind == "未知":
        note = "method=%r 与期望接口不符（H7=%s / H8=%s）" % (meth, mh7, mh8)
        rule_mark("r_method", False, note)
        resp = {"success": False, "body": "同步失败：" + note, "ts": now_ts(True)}
        return resp, checks, kind, note
    rule_mark("r_method", True, "method=%s" % meth)

    # ---- 3) appkey ----
    exp = cfg.get("appkey_expected", "")
    appkey_bad = []
    if not qk:
        appkey_bad.append("URL缺少appkey参数")
    elif qk != exp:
        appkey_bad.append("URL appkey=%s 与期望 %s 不一致" % (qk, exp))
    if not hk:
        appkey_bad.append("Header缺少AppKey")
    elif hk != exp:
        appkey_bad.append("Header AppKey=%s 与期望 %s 不一致" % (hk, exp))
    if appkey_bad:
        note = "鉴权失败：" + "；".join(appkey_bad)
        rule_mark("r_appkey", False, note)
        resp = {"success": False, "body": "同步失败：" + note, "ts": now_ts(True)}
        return resp, checks, kind, note
    rule_mark("r_appkey", True, "URL/Header AppKey 一致")

    # ---- 4) 波次计划 ----
    order_code = str(head.get("orderCode") or "").strip()
    wave = app.find_wave(order_code)
    if app.rule_on("r_wave") and wave is None:
        note = "波次 %s 不存在：本模拟WMS未下发该波次（先在「报文编辑」用H4下发，或到主数据导入）" % order_code
        rule_mark("r_wave", False, note)
        resp = {"success": False, "body": "同步失败：" + note, "ts": now_ts(True)}
        return resp, checks, kind, note
    rule_mark("r_wave", True, "波次 %s 存在" % order_code)

    plans, wave_grids = app.wave_plans(wave) if wave else ({}, set())
    total_plan = sum(plans.values())

    if kind == "H7":
        dl = head["detailList"]
        prefix = cfg.get("grid_prefix", "")
        for i, it in enumerate(dl, 1):
            gi = parse_grid_code(it.get("num", ""), prefix)
            gd = grid_display(str(it.get("num") or ""), prefix)
            sku = str(it.get("sku") or "").strip()
            qty = int_safe(it.get("qty"), 0)
            tl = str(it.get("targetLocation") or "").strip()
            if app.rule_on("r_grid"):
                if gi is None or gi < 1 or gi > int(cfg.get("grid_max") or 66):
                    note = "明细[%d] 格口号 %s 非法/越界（有效 1~%s）" % (i, it.get("num"), cfg.get("grid_max"))
                    rule_mark("r_grid", False, note)
                    resp = {"success": False, "body": "同步失败：" + note, "ts": now_ts(True)}
                    return resp, checks, kind, note
                if wave is not None and gi not in wave_grids:
                    gs = "、".join(sorted(str(x) for x in wave_grids))
                    note = "明细[%d] 格口号 %s(%s) 不在波次 %s 明细中（波次格口：%s）" % (
                        i, it.get("num"), gd, order_code, gs)
                    rule_mark("r_grid", False, note)
                    resp = {"success": False, "body": "同步失败：" + note, "ts": now_ts(True)}
                    return resp, checks, kind, note
            rule_mark("r_grid", True, "格口号 %s(%s) 有效" % (it.get("num"), gd))
            if app.rule_on("r_bind"):
                bmap = app.binding_map()
                bbox = bmap.get(gi) if gi is not None else None
                if not bbox:
                    note = "明细[%d] 格口 %s(%s) 未绑定容器：需先发 H6 绑定或维护绑定表" % (i, it.get("num"), gd)
                    rule_mark("r_bind", False, note)
                    resp = {"success": False, "body": "同步失败：" + note, "ts": now_ts(True)}
                    return resp, checks, kind, note
                if tl != bbox:
                    note = ("明细[%d] 格口 %s(%s) targetLocation=%s 与绑定容器 %s 不一致"
                            % (i, it.get("num"), gd, tl, bbox))
                    rule_mark("r_bind", False, note)
                    resp = {"success": False, "body": "同步失败：" + note, "ts": now_ts(True)}
                    return resp, checks, kind, note
            rule_mark("r_bind", True, "格口 %s(%s) 容器 %s 一致" % (it.get("num"), gd, tl))
            plan_qty = plans.get((gi, sku), 0) if gi is not None else 0
            if app.rule_on("r_sku") and plan_qty <= 0:
                grid_skus = sorted({s for (g, s) in plans if g == gi}) if gi is not None else []
                note = ("明细[%d] SKU %s 不在格口 %s(%s) 的波次计划中（该格口SKU：%s）"
                        % (i, sku, it.get("num"), gd, "、".join(grid_skus[:8]) or "无"))
                rule_mark("r_sku", False, note)
                resp = {"success": False, "body": "同步失败：" + note, "ts": now_ts(True)}
                return resp, checks, kind, note
            rule_mark("r_sku", True, "SKU %s 在格口计划内" % sku)
            if app.rule_on("r_cap") and qty > plan_qty:
                note = ("明细[%d] SKU %s qty=%d 超过格口 %s(%s) 计划数量 %d（模拟生产拒超收）"
                        % (i, sku, qty, it.get("num"), gd, plan_qty))
                rule_mark("r_cap", False, note)
                resp = {"success": False, "body": "同步失败：" + note, "ts": now_ts(True)}
                return resp, checks, kind, note
            rule_mark("r_cap", True, "qty=%d ≤ 计划 %d" % (qty, plan_qty))
            if app.rule_on("r_skumaster") and sku not in app.sku_master():
                note = "明细[%d] SKU %s 不在 SKU 主档中" % (i, sku)
                rule_mark("r_skumaster", False, note)
                resp = {"success": False, "body": "同步失败：" + note, "ts": now_ts(True)}
                return resp, checks, kind, note
        rule_mark("r_skumaster", True, "SKU 均在主档")
    else:  # H8
        if app.rule_on("r_cap"):
            sl = int_safe(head.get("sumLocation"), 0)
            if sl > total_plan:
                note = "完结件数 sumLocation=%d 超过波次计划总数 %d" % (sl, total_plan)
                rule_mark("r_cap", False, note)
                resp = {"success": False, "body": "同步失败：" + note, "ts": now_ts(True)}
                return resp, checks, kind, note
        rule_mark("r_cap", True, "sumLocation 在计划总数内")

    # ---- 全部通过 ----
    ok_body = REPLY_H7_OK_BODY if kind == "H7" else REPLY_H8_OK_BODY
    resp = {"success": True, "body": ok_body, "ts": now_ts(True)}
    return resp, checks, kind, "通过（%d 项规则）" % len(checks)


class GatewayServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


class GatewayHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    app = None

    def log_message(self, fmt, *args):
        pass

    def _read_body(self):
        length = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(length) if length else b""

    def _answer(self, code, obj):
        raw = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=UTF-8")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        try:
            self.wfile.write(raw)
        except OSError:
            pass

    def do_POST(self):
        app = GatewayHandler.app
        started = time.time()
        parsed = urllib.parse.urlparse(self.path)
        query = {k: v for k, v in urllib.parse.parse_qsl(parsed.query)}
        headers_lc = {k.lower(): v for k, v in self.headers.items()}
        body = self._read_body()
        body_text = body.decode("utf-8", errors="replace")
        # 若走强制/手动之外：规则引擎
        try:
            resp, checks, kind, verdict = gateway_respond(app, parsed.path, query,
                                                          headers_lc, body_text)
        except Exception as e:
            resp = {"success": False, "body": "网关内部错误: %s" % e, "ts": now_ts(True)}
            checks, kind, verdict = [], "未知", "网关内部错误"
        ms = int((time.time() - started) * 1000)
        entry = {
            "t": ts_stamp(), "kind": kind, "path": parsed.path,
            "url_appkey": query.get("appkey", ""),
            "hdr_appkey": headers_lc.get("appkey", ""),
            "method_param": query.get("method", ""),
            "body": trunc(body_text, 6000),
            "checks": checks, "verdict": verdict,
            "ok": bool(resp.get("success")),
            "resp": resp, "resp_status": 200, "ms": ms,
            "mode": app.state.get("gateway_mode", "auto"),
        }
        if kind == "H7":
            app.gw_h7 += 1
        elif kind == "H8":
            app.gw_h8 += 1
        else:
            app.gw_unknown += 1
        app.add_log_gw(entry)
        p("[网关 %s] %s  %s  →  success=%s (%dms) %s"
          % (time.strftime("%H:%M:%S"), kind, parsed.path, resp.get("success"), ms,
             "" if resp.get("success") else resp.get("body", "")[:120]))
        self._answer(200, resp)

    def do_GET(self):
        if urllib.parse.urlparse(self.path).path == "/health":
            self._answer(200, {"ok": True, "name": APP_NAME,
                               "ts": now_ts(True), "expect_appkey":
                                   GatewayHandler.app.cfg("appkey_expected", "")})
        else:
            self._answer(404, {"success": False, "body": "not found"})

    def do_PUT(self):
        self._answer(405, {"success": False, "body": "method not allowed"})

    def do_DELETE(self):
        self._answer(405, {"success": False, "body": "method not allowed"})


# ════════════════════════════════════════════════════════════════
# HTTP 发送（模拟 WCS 客户端 / WMS 客户端）
# ════════════════════════════════════════════════════════════════
def http_send(url, method="POST", body_text=None, headers=None, timeout=15):
    headers = dict(headers or {})
    has_ct = any(k.lower() == "content-type" for k in headers)
    if body_text and not has_ct:
        headers["Content-Type"] = "application/json; charset=UTF-8"
    headers.setdefault("User-Agent", "wms-mock-gui/1.0")
    data = body_text.encode("utf-8") if body_text is not None else b""
    req = urllib.request.Request(url, data=data, method=method, headers=headers)
    started = time.time()
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            raw = r.read()
            return r.status, dict(r.headers), raw.decode("utf-8", errors="replace"), started
    except urllib.error.HTTPError as e:
        raw = e.read()
        return e.code, dict(e.headers), raw.decode("utf-8", errors="replace"), started
    except urllib.error.URLError as e:
        raise RuntimeError("网络错误(URLError): %s" % e.reason)
    except socket.timeout:
        raise RuntimeError("请求超时(%ss)" % timeout)
    except Exception as e:
        raise RuntimeError("请求失败: %s" % e)


def judge_response(kind, status, text):
    """生产口径判定：H7/H8 只认 JSON 顶层 success==true；H4/H6 认 code==200"""
    obj = None
    try:
        obj = json.loads(text) if text and text.strip() else None
    except Exception:
        obj = None
    if kind in ("H7", "H8"):
        if isinstance(obj, dict) and obj.get("success") is True:
            return True, "成功（success=true）"
        if isinstance(obj, dict):
            return False, "失败（success=%s）：%s" % (obj.get("success"), obj.get("body", ""))
        return False, "失败（响应非 JSON，HTTP %s）" % status
    else:  # H4/H6（WCS 入站应答）
        if status == 200 and isinstance(obj, dict):
            code = obj.get("code")
            if str(code) in ("200",):
                return True, "成功（code=200）：%s" % obj.get("message", "")
            return False, "失败（code=%s）：%s" % (code, obj.get("message", ""))
        return False, "失败（HTTP %s）：%s" % (status, trunc(text, 300))


def do_send_impl(app, payload):
    """/api/send 核心；返回日志条目 dict（不落日志由调用方决定? 落日志在此完成）"""
    url = str(payload.get("url") or "").strip()
    kind = str(payload.get("kind") or "H7").upper()
    if not url:
        raise RuntimeError("目标地址为空")
    body_text = payload.get("body")
    if isinstance(body_text, dict):
        body_text = json.dumps(body_text, ensure_ascii=False)
    headers = dict(payload.get("headers") or {})
    if kind in ("H7", "H8"):
        appkey = str(payload.get("appkey") or "").strip()
        if appkey:
            headers.setdefault("AppKey", appkey)
    timeout = int_safe(payload.get("timeout"), app.cfg("send_timeout_s", 15))
    status, resp_headers, resp_text, started = http_send(url, "POST", body_text,
                                                         headers, timeout=timeout)
    ms = int((time.time() - started) * 1000)
    ok, label = judge_response(kind, status, resp_text)
    entry = {
        "t": ts_stamp(), "kind": kind, "url": trunc(url, 600),
        "appkey": str(payload.get("appkey") or "").strip(),
        "body": trunc(body_text or "", 6000), "status": status, "ms": ms,
        "ok": ok, "label": label, "resp": trunc(resp_text, 6000),
        "error": None,
    }
    # 成功后按需记录到模拟WMS底账（H4→波次计划库，H6→绑定表）
    if ok:
        try:
            if payload.get("record_wave"):
                wave = payload["record_wave"]
                if isinstance(wave, dict) and wave.get("orderCode") and \
                        isinstance(wave.get("items"), list):
                    app.upsert_wave(wave)
                    entry["note"] = "已记录到波次计划库: %s" % wave["orderCode"]
            if payload.get("record_binding"):
                b = payload["record_binding"]
                grid = int_safe(b.get("grid"), None)
                box = str(b.get("box") or "").strip()
                if grid and box:
                    with app.lock:
                        bs = app.state["bindings"]
                        hit = None
                        for x in bs:
                            if int_safe(x.get("grid"), -1) == grid:
                                hit = x
                                break
                        if hit:
                            hit["box"] = box
                        else:
                            bs.append({"grid": grid, "box": box})
                    app.save_state()
                    entry["note"] = "已记录到格口绑定表: %s→%s" % (grid, box)
        except Exception as e:
            entry["note"] = "记录失败: %s" % e
    return entry, ok, label


# ════════════════════════════════════════════════════════════════
# Web GUI 服务
# ════════════════════════════════════════════════════════════════
class WebServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


class WebHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    app = None

    def log_message(self, fmt, *args):
        pass

    def _json(self, code, obj):
        raw = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=UTF-8")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        try:
            self.wfile.write(raw)
        except OSError:
            pass

    def _html(self, code, raw):
        self.send_response(code)
        self.send_header("Content-Type", "text/html; charset=UTF-8")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        try:
            self.wfile.write(raw)
        except OSError:
            pass

    def _read_json(self):
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length else b""
        try:
            return json.loads(raw.decode("utf-8")) if raw else {}
        except Exception as e:
            raise RuntimeError("请求体非合法JSON: %s" % e)

    def do_GET(self):
        app = WebHandler.app
        path = urllib.parse.urlparse(self.path).path
        if path in ("/", "/index.html"):
            try:
                with open(WEBUI_FILE, "rb") as f:
                    return self._html(200, f.read())
            except Exception as e:
                return self._html(500, ("内部错误: %s" % e).encode("utf-8"))
        if path == "/api/state":
            return self._json(200, app.snapshot())
        if path == "/api/health":
            return self._json(200, {"ok": True, "name": APP_NAME, "ts": now_ts(True)})
        return self._json(404, {"error": "not found"})

    def do_POST(self):
        app = WebHandler.app
        path = urllib.parse.urlparse(self.path).path
        try:
            body = self._read_json()
            if path == "/api/config":
                with app.lock:
                    if isinstance(body.get("config"), dict):
                        app.state["config"].update(body["config"])
                    if isinstance(body.get("gateway_mode"), str):
                        app.state["gateway_mode"] = body["gateway_mode"]
                    if isinstance(body.get("rules"), list):
                        app.state["rules"] = body["rules"]
                    if isinstance(body.get("bindings"), list):
                        app.state["bindings"] = [b for b in body["bindings"]
                                                 if isinstance(b, dict) and b.get("grid") is not None]
                    if isinstance(body.get("presets"), list):
                        app.state["presets"] = [p2 for p2 in body["presets"]
                                                if isinstance(p2, dict) and p2.get("name")]
                app.save_state()
                return self._json(200, {"ok": True})
            if path == "/api/waves_add":
                wave = body.get("wave") or {}
                app.upsert_wave(wave)
                return self._json(200, {"ok": True})
            if path == "/api/waves_delete":
                code = str(body.get("orderCode") or "").strip()
                with app.lock:
                    app.state["waves"] = [w for w in app.state["waves"]
                                          if str(w.get("orderCode", "")).strip() != code]
                app.save_state()
                return self._json(200, {"ok": True})
            if path == "/api/waves_clear":
                with app.lock:
                    app.state["waves"] = []
                app.save_state()
                return self._json(200, {"ok": True})
            if path == "/api/reset_demo":
                with app.lock:
                    d = default_state()
                    keep = app.state["config"]
                    d["config"].update({k: v for k, v in keep.items()
                                        if k in ("gw_port", "gui_port", "gw_host")})
                    app.state = d
                app.save_state()
                return self._json(200, {"ok": True})
            if path == "/api/log_clear":
                scope = str(body.get("scope") or "all")
                with app.lock:
                    if scope in ("gw", "all"):
                        app.log_gw.clear()
                    if scope in ("sent", "all"):
                        app.log_sent.clear()
                return self._json(200, {"ok": True})
            if path == "/api/send":
                entry, ok, label = do_send_impl(app, body)
                app.add_log_sent(entry)
                return self._json(200, {"ok": True, "entry": entry})
            if path == "/api/gw_restart":
                port = int_safe(body.get("port"), app.cfg("gw_port", 8099))
                result = start_gateway(app, port)
                return self._json(200, result)
            if path == "/api/gw_stop":
                stop_gateway(app)
                return self._json(200, {"ok": True})
            if path == "/api/quit":
                # 让浏览器按钮能退出服务（EXE 无控制台时唯一退出途径）
                threading.Thread(target=lambda: (time.sleep(0.5), os._exit(0)),
                                 daemon=True).start()
                return self._json(200, {"ok": True, "bye": True})
            return self._json(404, {"error": "not found"})
        except RuntimeError as e:
            return self._json(400, {"error": str(e)})
        except Exception as e:
            return self._json(500, {"error": str(e)})

    def do_PUT(self):
        self._json(405, {"error": "method not allowed"})

    def do_DELETE(self):
        self._json(405, {"error": "method not allowed"})


# ════════════════════════════════════════════════════════════════
# 网关启停
# ════════════════════════════════════════════════════════════════
def start_gateway(app, port=None):
    stop_gateway(app)
    host = app.cfg("gw_host", "127.0.0.1")
    port = port or int(app.cfg("gw_port", 8099))
    GatewayHandler.app = app
    try:
        srv = GatewayServer((host, port), GatewayHandler)
    except Exception as e:
        app.gw_last_error = "监听 %s:%s 失败: %s" % (host, port, e)
        p("[网关] " + app.gw_last_error)
        return {"ok": False, "error": app.gw_last_error}
    app.gw_server = srv
    app.gw_last_error = ""
    with app.lock:
        app.state["config"]["gw_port"] = port
    app.save_state()
    t = threading.Thread(target=srv.serve_forever, daemon=True,
                         name="gw-serve")
    t.start()
    app.gw_thread = t
    p("[网关] 模拟WMS网关已启动 → http://%s:%d （接收H7/H8，路径任意）" % (host, port))
    return {"ok": True, "host": host, "port": port}


def stop_gateway(app):
    srv = app.gw_server
    app.gw_server = None
    if srv:
        try:
            srv.shutdown()
            srv.server_close()
        except Exception:
            pass
        p("[网关] 模拟WMS网关已停止")


# ════════════════════════════════════════════════════════════════
# 主入口
# ════════════════════════════════════════════════════════════════
def main():
    args = sys.argv[1:]
    gui_port, gw_port, no_browser, no_gateway = 0, 0, False, False
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--gui-port" and i + 1 < len(args):
            gui_port = int_safe(args[i + 1], 0) or 0
            i += 1
        elif a == "--gw-port" and i + 1 < len(args):
            gw_port = int_safe(args[i + 1], 0) or 0
            i += 1
        elif a == "--no-browser":
            no_browser = True
        elif a == "--no-gateway":
            no_gateway = True
        i += 1

    app = App()
    if gui_port:
        app.state["config"]["gui_port"] = gui_port
    if gw_port:
        app.state["config"]["gw_port"] = gw_port

    # 防双开：配置端口上已有本程序实例 → 直接打开浏览器后退出
    if probe_running(int(app.cfg("gui_port", 8765))):
        url = "http://127.0.0.1:%d" % int(app.cfg("gui_port", 8765))
        p("检测到 %s 已在运行 → 打开 %s" % (APP_NAME, url))
        if not no_browser:
            _open(url)
        sys.exit(0)

    if not no_gateway:
        start_gateway(app)

    # Web GUI 端口：从配置端口起，被占用则顺延
    web = None
    base = int(app.cfg("gui_port", 8765))
    for port in range(base, base + 10):
        try:
            WebHandler.app = app
            web = WebServer(("127.0.0.1", port), WebHandler)
            app.state["config"]["gui_port"] = port
            break
        except OSError as e:
            p("[GUI] 端口 %s 被占用，尝试 %s ..." % (port, port + 1))
    if web is None:
        p("[GUI] 无法找到可用端口，退出")
        sys.exit(1)

    url = "http://127.0.0.1:%s" % port
    p("=" * 62)
    p("  %s" % APP_NAME)
    p("  GUI 界面     : %s" % url)
    p("  模拟WMS网关  : %s:%s%s" % (app.cfg("gw_host"), app.cfg("gw_port"),
                                    "" if app.gw_server else "  (未运行: " + app.gw_last_error + ")"))
    p("  状态文件     : %s" % os.path.basename(STATE_FILE))
    if FROZEN:
        p("  退出方式     : 页面右上角「退出服务」按钮（无控制台窗口）")
    else:
        p("  关闭本窗口或 Ctrl+C 退出")
    p("=" * 62)
    if not no_browser:
        threading.Timer(0.8, lambda: _open(url)).start()
    try:
        web.serve_forever()
    except KeyboardInterrupt:
        p("\n已退出")


def _open(url):
    try:
        webbrowser.open(url)
    except Exception:
        pass


def probe_running(port):
    """配置端口上是否已有本程序实例在运行（防双开）"""
    try:
        req = urllib.request.Request("http://127.0.0.1:%d/api/health" % port)
        with urllib.request.urlopen(req, timeout=1.5) as r:
            j = json.loads(r.read().decode("utf-8", errors="replace"))
        return bool(j.get("ok")) and APP_NAME in str(j.get("name", ""))
    except Exception:
        return False


if __name__ == "__main__":
    main()
