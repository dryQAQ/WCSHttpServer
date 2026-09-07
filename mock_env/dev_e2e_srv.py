# -*- coding: utf-8 -*-
"""
dev_e2e_srv.py — E2E 联调常驻服务：
RFID/S7/WMS网关/RFID查询 + PLC客户端(收到 {EPC|格口|小车} 自动回执 5字段状态1)
运行至 logs/e2e_stop.flag 出现；每5s把计数写入 logs/e2e_srv.txt
配合 dev_ui.py 点击 WCS 界面按钮完成全流程验收。
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import mock_cfg
import mock_channels
import mock_logstore
import mock_proto
import mock_s7comm

HERE = os.path.dirname(os.path.abspath(__file__))
CFG = mock_cfg.WcsConfig(os.path.join(HERE, "..", "release_WcsHttpServer", "config", "http_server.xml")).load()
STORE = mock_logstore.MessageStore(max_keep=60000, log_dir=os.path.join(HERE, "logs"))
FLAG = os.path.join(HERE, "logs", "e2e_stop.flag")
STAT = os.path.join(HERE, "logs", "e2e_srv.txt")
CMDF = os.path.join(HERE, "logs", "e2e_cmd.txt")
_seen = None

# 干净波次 3格口×1SKU×1件
WAVE, EPC_MAP, PIECES = mock_proto.build_count_wave(
    "PP2026E2E001", grids=3, skus_per_grid=1, qty=1)
GRIDS = [str(g) for g in range(1, 4)]


def write_stat(extra=""):
    snap = S7.snapshot()
    lines = [
        f"time={time.strftime('%H:%M:%S')}",
        f"rfid_clients={RFID.connected_client_count()} rfid_hb={len(STORE.snapshot(channel='RFID', direction='收', keyword='心跳'))}",
        f"s7_accepts={snap['counters']['accepts']} s7_reads={snap['counters']['reads']}",
        f"plc_connected={PLC.connected} plc_cmds={len(STORE.snapshot(channel='PLC', direction='收', keyword='3字段反馈'))}",
        f"gw_h7={GW.last().get('h7_count', 0)} gw_h8={GW.last().get('h8_count', 0)}",
        f"query_req={QRY.stats()['qcount']}",
    ]
    if extra:
        lines.append(extra)
    with open(STAT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


def main():
    global RFID, PLC, GW, QRY, S7
    RFID = mock_channels.RfidServer("127.0.0.1", CFG.rfid_server_port, STORE)
    PLC = mock_channels.PlcClient("127.0.0.1", CFG.plc_listen_port, STORE, reconnect_s=1)
    GW = mock_channels.HttpGatewayServer("127.0.0.1", 8099, STORE, CFG)
    QRY = mock_channels.RfidQueryServer("127.0.0.1", 9100, STORE, CFG)
    S7 = mock_s7comm.S7Server("127.0.0.1", 102, STORE)
    QRY.set_mapping(EPC_MAP)

    def on_cmd(fields):
        if len(fields) == 3:
            epc, grid, car = fields
            STORE.add("PLC", "事件", f"[E2E] 收到WCS指令 epc={epc} grid={grid} car={car} → 自动回执")
            return [mock_proto.make_plc_feedback(epc, grid, car, "1").decode("ascii")]
        return []

    PLC.on_cmd = on_cmd
    for svc in (RFID, PLC, GW, QRY, S7):
        svc.start()

    STORE.add_event(f"[E2E] 服务就绪 order={WAVE['orderCode']} items={len(WAVE['items'])} qty={WAVE['orderQty']} 映射={len(EPC_MAP)}")
    write_stat("phase=READY")
    for f in (FLAG, CMDF):
        try:
            os.remove(f)
        except OSError:
            pass
    seq_no = 0
    deadline = time.time() + 2400
    while time.time() < deadline:
        if os.path.exists(CMDF):
            try:
                with open(CMDF, encoding="utf-8") as f:
                    cmds = [ln.strip() for ln in f.read().splitlines() if ln.strip()]
                os.remove(CMDF)
                for c in cmds:
                    parts = c.split()
                    if not parts:
                        continue
                    if parts[0] == "rfid" and len(parts) >= 2:
                        seq_no += 1
                        seq = parts[2] if len(parts) > 2 else "SN%04d" % seq_no
                        raw = mock_proto.make_rfid_frame(seq, "01", parts[1], "LIT0D")
                        ok, note = RFID.send_frame(raw, note=f"[E2E]{seq}")
                        STORE.add_event(f"[E2E] rfid推送 {seq} {parts[1]} → {'OK' if ok else note}")
                    elif parts[0] == "lock" and len(parts) >= 2:
                        S7.set_grid(int(parts[1]), True)
                        STORE.add_event(f"[E2E] S7锁格 {parts[1]}")
                    elif parts[0] == "unlock" and len(parts) >= 2:
                        S7.set_grid(int(parts[1]), False)
                        STORE.add_event(f"[E2E] S7解锁 {parts[1]}")
                    elif parts[0] == "map" and len(parts) >= 3:
                        QRY.add_mapping(parts[1], parts[2])
                        STORE.add_event(f"[E2E] map {parts[1]} -> {parts[2]}")
                    elif parts[0] == "mapdel" and len(parts) >= 2:
                        with QRY.mapping_lock:
                            QRY.mapping.pop(parts[1], None)
                        STORE.add_event(f"[E2E] mapdel {parts[1]}")
            except Exception as e:
                STORE.add_event(f"[E2E] cmd处理异常 {e!r}")
        write_stat()
        if os.path.exists(FLAG):
            break
        time.sleep(0.5)
    STORE.add_event("[E2E] 服务退出")
    for svc in (RFID, PLC, GW, QRY, S7):
        svc.stop()
    write_stat("phase=EXIT")
    STORE.close()


if __name__ == "__main__":
    main()
