# -*- coding: utf-8 -*-
"""
dev_probe.py — 开发期联调用：以无界面方式启动部分仿真服务（RFID/S7/查询/网关），
供真实 WCS_httpServer.exe 局部链路验证（S7 连接+轮询、RFID 连接+心跳+SKU查询）。
用法: python dev_probe.py [--seconds N] [--push-epc EPC] 
"""
import json
import os
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import mock_cfg
import mock_channels
import mock_logstore
import mock_s7comm

HERE = os.path.dirname(os.path.abspath(__file__))
CFG = mock_cfg.WcsConfig(os.path.join(HERE, "..", "release_WcsHttpServer", "config", "http_server.xml")).load()
STORE = mock_logstore.MessageStore(max_keep=50000, log_dir=os.path.join(HERE, "logs"))


def main():
    secs = 20
    push_epc = None
    args = sys.argv[1:]
    for i, a in enumerate(args):
        if a == "--seconds" and i + 1 < len(args):
            secs = int(args[i + 1])
        if a == "--push-epc" and i + 1 < len(args):
            push_epc = args[i + 1]

    rfid = mock_channels.RfidServer("127.0.0.1", CFG.rfid_server_port, STORE)
    s7 = mock_s7comm.S7Server("127.0.0.1", 102, STORE)
    qs = mock_channels.RfidQueryServer("127.0.0.1", 9100, STORE, CFG)
    gw = mock_channels.HttpGatewayServer("127.0.0.1", 8099, STORE, CFG)
    for svc in (rfid, s7, qs, gw):
        svc.start()

    qs.set_mapping({"A10126000900009285552527": "106301209506204",
                    "A10126000900009285559999": "SK9999999999999"})
    STORE.add_event("dev_probe: 服务已启动，等待 WCS 连入...")

    deadline = time.time() + secs
    pushed = False
    while time.time() < deadline:
        snap = s7.snapshot()
        n_rfid = rfid.connected_client_count()
        # 到达后推一帧 EPC 触发查询（波次未开始也查 SKU）
        if push_epc and n_rfid and not pushed:
            import mock_proto as P
            rfid.send_frame(P.make_rfid_frame("SN0027", "01", push_epc, "LIT0D"), note="probe")
            pushed = True
        if n_rfid and snap["counters"]["reads"] > 0 and pushed:
            break
        time.sleep(0.5)

    time.sleep(1)
    snap = s7.snapshot()
    hb = STORE.snapshot(channel="RFID", direction="收", keyword="心跳")
    print("RFID clients:", rfid.connected_client_count())
    print("RFID heartbeat rows:", len(hb))
    print("S7 reads:", snap["counters"]["reads"], " accepts:", snap["counters"]["accepts"])
    print("Query requests:", qs.stats()["qcount"])
    print("Gateway H7/H8:", gw.last().get("h7_count"), gw.last().get("h8_count"))
    for svc in (rfid, s7, qs, gw):
        svc.stop()
    STORE.close()


if __name__ == "__main__":
    main()
