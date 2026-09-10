# -*- coding: utf-8 -*-
"""
mock_cfg.py — 读取 WCS 的 http_server.xml（依据配置文件里的地址工作）
仅做只读解析；若缺节点给默认值并提示。
"""
import os
import re
import xml.etree.ElementTree as ET

DEFAULT_CFG = os.path.abspath(os.path.join(
    os.path.dirname(__file__), "..", "release_WcsHttpServer", "config", "http_server.xml"))

LOCAL_IPS = ("127.0.0.1", "localhost", "0.0.0.0", "::1")


def _txt(v):
    return (v or "").strip()


def _int(v, default=0):
    try:
        return int(_txt(v))
    except Exception:
        return default


class WcsConfig:
    def __init__(self, path=None):
        self.path = path or DEFAULT_CFG
        self.ok = False
        self.error = ""
        self.fields = {}

    def load(self):
        self.ok = False
        self.error = ""
        if not os.path.isfile(self.path):
            self.error = f"配置文件不存在: {self.path}"
            return self
        try:
            root = ET.parse(self.path, parser=ET.XMLParser(encoding="utf-8")).getroot()
        except Exception as e:
            self.error = f"XML 解析失败: {e}"
            return self
        if root.tag != "config":
            self.error = "根节点不是 <config>"
            return self
        for child in root:
            self.fields[child.tag] = _txt(child.text)
        self.ok = True
        return self

    def f(self, key, default=""):
        v = self.fields.get(key)
        return v if v not in (None, "") else default

    def i(self, key, default=0):
        return _int(self.fields.get(key), default)

    # ──── 常用聚合视图 ────
    @property
    def wms_listen_port(self):
        return self.i("wmsListenPort", 8191)

    @property
    def plc_listen_port(self):
        return self.i("plcListenPort", 102)

    @property
    def rfid_server_ip(self):
        return self.f("rfidPushServerIp", "127.0.0.1")

    @property
    def rfid_server_port(self):
        return self.i("rfidPushServerPort", 2010)

    @property
    def rfid_heartbeat_enable(self):
        return self.f("rfidHeartbeatEnable", "1") == "1"

    @property
    def rfid_heartbeat_ms(self):
        return max(self.i("rfidHeartbeatIntervalMs", 2000), 200)

    @property
    def rfid_query_url(self):
        return self.f("rfidQueryUrl", "http://172.31.10.201:9521/open-api/rfid/query")

    @property
    def rfid_appkey(self):
        return self.f("rfidAppkey", "")

    @property
    def appkey(self):
        return self.f("appkey", "")

    @property
    def appkey_test(self):
        return self.f("appkeyTest", "")

    @property
    def use_test_env(self):
        return self.f("useTestEnv", "1") == "1"

    @property
    def feedback_url_h7(self):
        return (self.f("feedbackTestUrl") if self.use_test_env else self.f("feedbackUrl")) or \
               self.f("feedbackUrl", "http://127.0.0.1:8099/h7")

    @property
    def feedback_url_h8(self):
        return (self.f("feedbackEndTestUrl") if self.use_test_env else self.f("feedbackEndUrl")) or \
               self.f("feedbackEndUrl", "http://127.0.0.1:8099/h8")

    @property
    def method_h7(self):
        return self.f("feedbackMethod", "gwisSubProductClassifyOrder")

    @property
    def method_h8(self):
        return self.f("feedbackEndMethod", "gwisSubProductClassifyEndOrder")

    @property
    def active_appkey(self):
        return self.appkey_test if self.use_test_env else self.appkey

    @property
    def api_insert_wave(self):
        return self.f("apiInsertWaveInfo", "/api/DispatchSortingCommand/InsertWaveInfo")

    @property
    def api_binding(self):
        return self.f("apiBindingLatticePort", "/api/DispatchSortingCommand/BindingLatticePort")

    @property
    def api_cancel(self):
        return self.f("apiInsertWaveIn", "/api/DispatchSortingCommand/InsertWaveIn")

    @property
    def plc_s7_ip(self):
        return self.f("plcS7Ip", "192.168.100.10")

    @property
    def warehouse_code(self):
        return self.f("warehouseCode", "H")

    @property
    def goods_owner(self):
        return self.f("goodsOwner", "BXH_CS")

    @property
    def fullbox_default_target(self):
        return self.f("h7DefaultTargetLocation", "66")

    def port_summary(self):
        """当前生效端口速查（仿真台/WCS 共用同一份 http_server.xml）。
        S7(102) 为 snap7 协议固定端口，不在 xml 中配置。"""
        fb = self.f("feedbackTestUrl") if self.use_test_env else self.f("feedbackUrl")
        gw_url = fb or self.f("feedbackUrl")
        return {
            "wms": self.i("wmsListenPort", 8191),
            "plc": self.i("plcListenPort", 102),
            "rfid": self.i("rfidPushServerPort", 2010),
            "gw": self.parse_url_port(gw_url, 8099),
            "query": self.parse_url_port(self.f("rfidQueryUrl"), 9100),
        }

    def addresses_summary(self):
        """生成“依据配置文件”的角色地址清单（文本）"""
        lines = [
            f"配置文件        : {self.path}  [{'已加载' if self.ok else '未加载'}]",
            f"WMS下发端口     : {self.wms_listen_port}   (软件→WCS, 仿真台作客户端 POST)",
            f"PLC监听端口     : {self.plc_listen_port}   (WCS监听, 仿真台作PLC客户端连接)",
            f"RFID推送服务    : {self.rfid_server_ip}:{self.rfid_server_port}   (WCS主动连入, 仿真台监听)",
            f"RFID心跳        : {'开启' if self.rfid_heartbeat_enable else '关闭'}  每{self.rfid_heartbeat_ms}ms",
            f"RFID查询URL     : {self.rfid_query_url}   (WCS→仿真台查询服务)",
            f"RFID Authorization: {self.rfid_appkey or '(空)'}",
            f"H7 满箱回传URL  : {self.feedback_url_h7}",
            f"H8 完结回传URL  : {self.feedback_url_h8}",
            f"method H7/H8    : {self.method_h7} / {self.method_h8}",
            f"appkey(测试/正式): {self.active_appkey}   useTestEnv={'1(测试)' if self.use_test_env else '0(正式)'}",
            f"S7 PLC地址      : {self.plc_s7_ip}:102  (DB77 锁格位图, 仿真台监听)",
            f"仓库/货主       : {self.warehouse_code} / {self.goods_owner}",
        ]
        return "\n".join(lines)

    def is_address_local(self, host):
        if not host:
            return True
        return host.strip().lower() in LOCAL_IPS

    def all_local(self):
        return all([
            self.is_address_local(self.rfid_server_ip),
            self.is_address_local(self.plc_s7_ip),
            re.match(r"^https?://(127\.0\.0\.1|localhost|0\.0\.0\.0)", self.rfid_query_url) is not None,
            re.match(r"^https?://(127\.0\.0\.1|localhost|0\.0\.0\.0)", self.feedback_url_h7) is not None,
            re.match(r"^https?://(127\.0\.0\.1|localhost|0\.0\.0\.0)", self.feedback_url_h8) is not None,
        ])

    def parse_url_port(self, url, default=8099):
        m = re.match(r"^https?://([^/:]+)(?::(\d+))?", url or "")
        if not m:
            return default
        return int(m.group(2)) if m.group(2) else default

    def parse_url_host(self, url, default="127.0.0.1"):
        m = re.match(r"^https?://([^/:]+)(?::(\d+))?", url or "")
        return m.group(1) if m else default

    def parse_url_path(self, url, default="/"):
        m = re.search(r"^https?://[^/]+(/.*)$", url or "")
        return m.group(1) if m else default


if __name__ == "__main__":
    c = WcsConfig().load()
    print(c.error or c.addresses_summary())
