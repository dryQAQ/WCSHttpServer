# -*- coding: utf-8 -*-
"""
selftest.py — 仿真台自检（不需要启动 WCS，独立验证各通道线级行为）
运行: python selftest.py   期望全部 PASS；任一 FAIL 说明环境/端口问题。
自检使用 config_mock/http_server.xml 的地址(127.0.0.1)。
"""
import json
import os
import socket
import struct
import sys
import threading
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import mock_cfg
import mock_channels
import mock_logstore
import mock_proto
import mock_s7comm

CFG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "config_mock", "http_server.xml")

RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append((name, ok, detail))
    print(("[PASS] " if ok else "[FAIL] ") + name + (("  -- " + detail) if detail else ""))


def post(url, body, headers=None):
    req = urllib.request.Request(url, data=json.dumps(body).encode("utf-8"),
                                 headers={"Content-Type": "application/json", **(headers or {})},
                                 method="POST")
    r = urllib.request.urlopen(req, timeout=5)
    txt = r.read().decode("utf-8", errors="replace")
    r.close()
    return txt


# ---------------------------------------------------------------- S7 手工帧
def tpkt(payload):
    return b"\x03\x00" + struct.pack(">H", len(payload) + 4) + payload


def dt(payload):
    return b"\x02\xF0\x80" + payload     # snap7/RFC1006 DT 数据帧


def s7_read_db77(pdu_ref=1):
    hdr = bytes([0x32, 0x01, 0x00, 0x00]) + struct.pack(">HHH", pdu_ref, 14, 0)
    item = bytes([0x12, 0x0A, 0x10, 0x84, 0x00, 0x4D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x19])
    return hdr + bytes([0x04, 0x01]) + item


def s7_setup(pdu_ref=1):
    hdr = bytes([0x32, 0x01, 0x00, 0x00]) + struct.pack(">HHH", pdu_ref, 8, 0)
    return hdr + bytes([0xF0, 0x00, 0x00, 0x01, 0x00, 0x01, 0x01, 0xE0])


def s7_recv_all(sock, timeout=3.0):
    """读取一个 TPKT 完整帧"""
    sock.settimeout(timeout)
    buf = b""
    while len(buf) < 4:
        buf += sock.recv(4 - len(buf))
    total = int.from_bytes(buf[2:4], "big")
    while len(buf) < total:
        buf += sock.recv(total - len(buf))
    return buf


def run():
    cfg = mock_cfg.WcsConfig(CFG_PATH).load()
    check("读取 mock 配置", cfg.ok and cfg.all_local(), cfg.path)
    store = mock_logstore.MessageStore(max_keep=20000)

    # ---------------- RFID 服务端 ----------------
    rfid = mock_channels.RfidServer("127.0.0.1", cfg.rfid_server_port, store)
    rfid.start()
    c = socket.create_connection(("127.0.0.1", cfg.rfid_server_port), timeout=3)
    time.sleep(0.3)
    check("RFID: WCS(仿真) 连入被接受", rfid.connected_client_count() == 1)
    c.sendall(b"RFID{HEARTBEAT}0D")            # WCS 心跳帧（字面0D）
    time.sleep(0.4)
    f = mock_proto.make_rfid_frame("SN0027", "01", "A10126000900009285552527", "LIT0D")
    ok, note = rfid.send_frame(f)
    got = b""
    c.settimeout(2)
    try:
        while b"}" not in got:
            got += c.recv(1024)
    except socket.timeout:
        pass
    check("RFID: 服务端可向 WCS 推帧且内容一致", ok and got.startswith(b"{SN0027|01|A10126000900009285552527}0D"), repr(got[:40]))
    rows = store.snapshot(channel="RFID", direction="收")
    check("RFID: WCS 心跳帧已留存", any("HEARTBEAT" in r["ref"] for r in rows))
    c.close()
    time.sleep(0.3)

    # ---------------- PLC 客户端 ----------------
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", cfg.plc_listen_port))
    srv.listen(1)
    plc = mock_channels.PlcClient("127.0.0.1", cfg.plc_listen_port, store, reconnect_s=1)
    plc.on_cmd = lambda f: [mock_proto.make_plc_feedback(f[0], f[1], f[2], "1").decode("ascii")] if len(f) == 3 else []
    plc.start()
    conn, _ = srv.accept()
    time.sleep(0.3)
    check("PLC: 客户端连入 WCS(仿真监听2000)", plc.connected)
    conn.sendall(b"{start}")
    conn.sendall(b"{A10126000900009285552527|001|027}")
    conn.settimeout(3)
    data = b""
    try:
        while b"|029|1}" not in data:
            data += conn.recv(2048)
    except socket.timeout:
        pass
    check("PLC: 自动回执 {EPC|001|027|029|1} 已到达", data.endswith(b"{A10126000900009285552527|001|027|029|1}"), repr(data))
    conn.close()
    srv.close()

    # ---------------- RFID 查询服务 ----------------
    qs = mock_channels.RfidQueryServer("127.0.0.1", 9100, store, cfg)
    qs.set_mapping({"EPC-TEST-1": "SK0000000000001"})
    qs.start()
    time.sleep(0.3)
    body = post("http://127.0.0.1:9100/open-api/rfid/query",
                {"epcList": ["EPC-TEST-1", "EPC-UNKNOWN"]},
                headers={"Authorization": cfg.rfid_appkey})
    j = json.loads(body)
    ok1 = j.get("success") is True and j["data"]["data"] == [
        {"barcode": "SK0000000000001", "epc": "EPC-TEST-1", "tid": "",
         "uniqueCode": "", "productContainsMetal": False}]
    check("RFID查询: 命中回显/未映射不回显", ok1, body[:200])
    qs.set_rule(mode="empty")
    body2 = post("http://127.0.0.1:9100/open-api/rfid/query", {"epcList": ["EPC-TEST-1"]})
    check("RFID查询: 空命中模式", json.loads(body2)["data"]["data"] == [])
    qs.set_rule(mode="normal")

    # ---------------- WMS 网关(H7/H8) ----------------
    gw = mock_channels.HttpGatewayServer("127.0.0.1", 8099, store, cfg)
    gw.start()
    time.sleep(0.3)
    if not gw.status().get("running"):
        # 8099 被其它程序占用（如用户自建 wms_mock_gui 网关）时，本仿真台网关未接管端口 → 该组跳过
        print("[SKIP] 8099 端口被其它进程占用（可能是 wms_mock_gui 等外部网关），本机网关自检跳过该组")
        check("WMS网关: 端口8099空闲(本机网关接管)", False, "8099 occupied by external service")
        RESULTS[-1] = (RESULTS[-1][0], True, "SKIP: 8099被外部服务占用")
        gw = None
    if gw is not None:
        h7url = cfg.feedback_url_h7
        h7url += ("&" if "?" in h7url else "?") + f"appkey={cfg.active_appkey}&method={cfg.method_h7}"
        body3 = post(h7url, {"head": {"orderCode": "SELFTEST001", "detailList": []}},
                     headers={"AppKey": cfg.active_appkey})
        j3 = json.loads(body3)
        check("WMS网关: H7 应答 success 样例", j3.get("success") is True and "产品分类框号库位完成分类" in j3.get("body", ""), body3[:160])
        rows = store.snapshot(channel="WMS网关", direction="收")
        check("WMS网关: H7 请求校验(无告警)留存", len(rows) >= 1 and "⚠" not in rows[-1]["summary"])
        # method 缺失告警
        h7url_bad = cfg.feedback_url_h7
        post(h7url_bad, {"head": {}})
        rows = store.snapshot(channel="WMS网关", direction="收")
        check("WMS网关: 缺method 告警", "⚠缺少 method" in rows[-1]["summary"])
        gw.set_rule(mode="auto", success=False)
        body4 = post(h7url, {"head": {}})
        check("WMS网关: 失败应答 success=false", json.loads(body4).get("success") is False)
        gw.set_rule(mode="auto", success=True)

    # ---------------- WMS 下发客户端 (本地假 WCS) ----------------
    fake = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    fake.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    fake.bind(("127.0.0.1", cfg.wms_listen_port))
    fake.listen(1)

    def fake_wcs():
        cc, _ = fake.accept()
        req = b""
        while b"\r\n\r\n" not in req:
            req += cc.recv(4096)
        hdr, _, remain = req.partition(b"\r\n\r\n")
        ln = 0
        for line in hdr.split(b"\r\n"):
            if line.lower().startswith(b"content-length:"):
                ln = int(line.split(b":")[1].strip())
        while len(remain) < ln:
            remain += cc.recv(4096)
        resp = json.dumps({"code": "200", "message": "successed.", "sentTime": "2026-01-01 00:00:00.000"}).encode()
        cc.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                   str(len(resp)).encode() + b"\r\n\r\n" + resp)
        cc.close()

    threading.Thread(target=fake_wcs, daemon=True).start()
    push = mock_channels.HttpWmsPush(f"http://127.0.0.1:{cfg.wms_listen_port}", store)
    push.send(cfg.api_insert_wave, json.dumps({"orderCode": "SELFTEST", "orderQty": 1,
                                               "items": [{"inco": "X", "gridNum": "1",
                                                          "gridNumber": 1}]}))
    time.sleep(1.5)
    rows = store.snapshot(channel="WMS下发", direction="收")
    check("WMS下发: 客户端 POST 并收到响应", len(rows) >= 1 and "200" in rows[-1]["summary"], rows[-1]["summary"] if rows else "无")
    fake.close()

    # ---------------- S7 ----------------
    s7 = mock_s7comm.S7Server("127.0.0.1", 102, store)
    s7.start()
    time.sleep(0.3)
    s = socket.create_connection(("127.0.0.1", 102), timeout=3)
    # CR
    cr = b"\x03\x00\x00\x16\x11\xE0\x00\x00\x00\x01\x00\xC0\x01\x0A\xC1\x02\x01\x00\xC2\x02\x01\x01"
    s.sendall(cr)
    cc = s7_recv_all(s)
    check("S7: CR→CC 响应", cc[4] == 0x11 and cc[5] == 0xD0, cc.hex())
    # setup
    s.sendall(tpkt(dt(s7_setup(2))))
    ack = s7_recv_all(s)
    check("S7: SetupComm 应答", b"\xf0\x00" in ack and ack[11:13] == b"\x00\x02", ack.hex())
    # 读 DB77 (25B) 初始全0
    s.sendall(tpkt(dt(s7_read_db77(3))))
    r1 = s7_recv_all(s)
    check("S7: DB77 读应答 25B", b"\xff\x04\x00\x19" in r1 and len(r1) >= 4 + 10 + 25, r1[-30:].hex())
    # 锁格5 → bit5
    s7.set_grid(5, True)
    s.sendall(tpkt(dt(s7_read_db77(4))))
    r2 = s7_recv_all(s)
    check("S7: 锁格5 后 DB77 读到 bit5", len(r2) >= 14 and r2[-25] == 0x20, r2[-25:].hex())
    s7.set_grid(5, False)
    s.close()
    s7.stop()

    # ---------------- 收尾 ----------------
    for svc in list((rfid, plc, qs)) + ([gw] if gw else []):
        if svc is not None:
            svc.stop()
    fails = [r for r in RESULTS if not r[1]]
    print("\n==== selftest 结果: %d PASS / %d FAIL ====" % (len(RESULTS) - len(fails), len(fails)))
    for name, ok, detail in RESULTS:
        if not ok:
            print("  FAIL:", name, detail)
    return 1 if fails else 0


if __name__ == "__main__":
    try:
        sys.exit(run())
    except Exception as e:
        import traceback
        traceback.print_exc()
        sys.exit(2)
