# -*- coding: utf-8 -*-
"""
mock_s7comm.py — 极简 S7 (ISO-on-TCP / S7comm) 服务端
用途：模拟 PLC 的 S7 接口供 WCS 锁格检测：
  - 监听 TCP 102
  - COTP CR→CC（TSAP/TPDU 回显协商）
  - Setup communication (0xF0)
  - Read Var (0x04)：返回 DB77(面积0x84) 25 字节位图
  - Write Var (0x05)：应答成功（WCS 现网不回写 DB77，仅为健壮性）
位图约定：格口号 N(1..66) ↔ DB77 bit N（字节 N//8 的第 N%8 位，LSB位序），
与 WCS PlcManager 轮询代码（gridNum = byte*8+bit，从0计）一致：bit=1 → "001"。
所有收发 PDU 原样留存到报文总览（S7 通道）。
"""
import socket
import struct
import threading
import time

S7_PORT = 102
DB77_SIZE = 25
MAX_GRID = 200


def hexdump(b):
    return b.hex(" ").upper() if b else ""


def tpkt_wrap(payload):
    return b"\x03\x00" + struct.pack(">H", len(payload) + 4) + payload


def cotp_dt(payload):
    """snap7/RFC1006 Data TPDU 封装: LI=2, 类型 0xF0(DT), EOT+编号 0x80, 后随 S7 PDU"""
    return b"\x02\xF0\x80" + payload


def s7_header(rosctr, pdu_ref, plen, dlen):
    return bytes([0x32, rosctr, 0x00, 0x00]) + struct.pack(">HHH", pdu_ref, plen, dlen)


def s7_ack_header(pdu_ref, plen, dlen):
    """ACK/ACK_DATA 应答头 = 10B 标准头 + ErrorClass/ErrorCode 2B（官方 snap7 解析依赖此12B头）"""
    return s7_header(0x03, pdu_ref, plen, dlen) + b"\x00\x00"


class S7Server:
    def __init__(self, host, port, store):
        self.host = host
        self.port = port
        self.store = store
        self._stop = threading.Event()
        self._srv = None
        self._threads = []
        self._lock = threading.Lock()
        self.db77 = bytearray(DB77_SIZE)
        self.counters = {"accepts": 0, "reads": 0, "writes": 0, "setups": 0, "frames_in": 0, "frames_out": 0}
        self._last_read_at = 0.0
        self._running = False
        self.on_grid_change = None        # callable(grid, locked)

    # ---------- 状态 ----------
    def start(self):
        if self._running:
            return
        self._stop.clear()
        self._running = True
        t = threading.Thread(target=self._accept_loop, daemon=True)
        self._threads.append(t)
        t.start()

    def stop(self):
        self._stop.set()
        self._running = False
        if self._srv:
            try:
                self._srv.close()
            except OSError:
                pass
            self._srv = None

    @property
    def running(self):
        return self._running

    def status(self):
        snap = self.snapshot()
        return {"running": snap["running"],
                "detail": f"DB77读请求:{snap['counters']['reads']}",
                "counters": snap["counters"],
                "last_io": snap["last_read_at"]}

    def is_connected(self):
        return self.counters["accepts"] > 0 and not self._stop.is_set()

    def snapshot(self):
        with self._lock:
            return {"running": self._running,
                    "counters": dict(self.counters),
                    "db77": bytes(self.db77),
                    "last_read_at": self._last_read_at}

    # ---------- DB77 位图操作 ----------
    def grid_state(self, grid):
        """grid: 1..199；DB 位 = grid"""
        with self._lock:
            if not 0 <= grid < MAX_GRID:
                return None
            return bool(self.db77[grid // 8] & (1 << (grid % 8)))

    def set_grid(self, grid, locked, note=""):
        if not 0 < grid < MAX_GRID:
            return False
        with self._lock:
            byte = grid // 8
            mask = 1 << (grid % 8)
            old = bool(self.db77[byte] & mask)
            if old == locked:
                return False
            if locked:
                self.db77[byte] |= mask
            else:
                self.db77[byte] &= ~mask
        tag = "锁格" if locked else "解锁"
        self.store.add("S7", "事件",
                       f"仿真PLC DB77 {'置位' if locked else '复位'} 格口{grid:03d}(bit={grid}) {note}".strip(),
                       ref=f"G{grid:03d}")
        if self.on_grid_change:
            try:
                self.on_grid_change(grid, locked)
            except Exception:
                pass
        return True

    def set_bitmap_bytes(self, data25):
        with self._lock:
            for i, b in enumerate(data25[:DB77_SIZE]):
                self.db77[i] = b

    # ---------- 网络 ----------
    def _accept_loop(self):
        try:
            srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            srv.bind((self.host, self.port))
            srv.listen(4)
            srv.settimeout(0.5)
            self._srv = srv
        except OSError as e:
            self.store.add_event(f"S7模拟服务 监听失败 {self.host}:{self.port}: {e}")
            self._running = False
            return
        self.store.add_event(f"S7 模拟 PLC 已启动，监听 {self.host}:{self.port}（DB77 锁格位图）")
        while not self._stop.is_set():
            try:
                conn, addr = srv.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            with self._lock:
                self.counters["accepts"] += 1
            self.store.add("S7", "事件", f"WCS S7客户端连入 {addr[0]}:{addr[1]}（ISO-on-TCP）")
            t = threading.Thread(target=self._conn_loop, args=(conn, addr), daemon=True)
            self._threads.append(t)
            t.start()
        try:
            srv.close()
        except OSError:
            pass
        self._srv = None
        self.store.add_event("S7 模拟服务已停止")

    def _conn_loop(self, conn, addr):
        conn.settimeout(1.0)
        buf = b""
        while not self._stop.is_set():
            try:
                data = conn.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                break
            if not data:
                break
            buf += data
            while len(buf) >= 4:
                total = int.from_bytes(buf[2:4], "big")
                if total < 4 or total > 8192:
                    self.store.add_event(f"S7 收到非法 TPKT 长度 {total}，丢弃缓冲")
                    buf = b""
                    break
                if len(buf) < total:
                    break
                frame, buf = buf[:total], buf[total:]
                self._on_frame(conn, frame)
        try:
            conn.close()
        except OSError:
            pass
        self.store.add("S7", "事件", f"WCS S7连接断开 {addr[0]}:{addr[1]}（WCS心跳线程约2s后重连）")

    # ---------- 协议处理 ----------
    def _send(self, conn, tpkt):
        try:
            conn.sendall(tpkt)
            with self._lock:
                self.counters["frames_out"] += 1
            # 留存出站原始帧
            self.store.add("S7", "发", f"响应帧 {len(tpkt)}B",
                           raw_hex=hexdump(tpkt),
                           parsed=self._describe(tpkt))
        except OSError as e:
            self.store.add_event(f"S7 发送失败: {e}")

    def _on_frame(self, conn, tpkt):
        with self._lock:
            self.counters["frames_in"] += 1
        parsed = self._describe(tpkt)
        self.store.add("S7", "收", f"请求帧 {len(tpkt)}B", raw_hex=hexdump(tpkt), parsed=parsed)
        payload = tpkt[4:]
        if len(payload) < 2:
            return
        li = payload[0]
        if li > len(payload) - 1:
            return
        tpdu = payload[1:1 + li]
        ttype = tpdu[0] if tpdu else None
        if ttype == 0xE0:            # Connection Request
            self._on_cr(conn, tpdu)
        elif ttype in (0xF0, 0x0F):  # Data TPDU -> S7 PDU
            # snap7 标准: LI=2, DT=0xF0, [EOT/编号], 其后为 S7 PDU
            # 兼容旧式(测试用): LI=len+3, DT=0x0F, [EOT/编号 2B]
            if ttype == 0xF0:
                s7pdu = payload[1 + li:]
            else:
                s7pdu = tpdu[3:]
            self._on_s7pdu(conn, s7pdu)
        # 其它（CC 等）忽略

    def _on_cr(self, conn, tpdu):
        # CR: E0 dst-ref(2) src-ref(2) class(1) params...
        src_ref = tpdu[2:4]
        params = tpdu[5:] if len(tpdu) > 5 else b""
        c1 = c2 = None
        tpdsize = 0x0A
        i = 0
        while i < len(params):
            code = params[i]
            ln = params[i + 1] if i + 1 < len(params) else 0
            val = params[i + 2: i + 2 + ln]
            if code == 0xC0 and ln == 1:
                tpdsize = val[0]
            elif code == 0xC1 and ln == 2:
                c1 = val
            elif code == 0xC2 and ln == 2:
                c2 = val
            i += 2 + ln
        c1 = c1 or b"\x01\x00"
        c2 = c2 or b"\x01\x01"
        # CC: D0 dst-ref=src_ref src-ref=0000 class=00 参数(TSAP对调)
        cc = bytes([0xD0]) + src_ref + b"\x00\x00\x00"
        cc += bytes([0xC0, 0x01, tpdsize])
        cc += bytes([0xC1, 0x02]) + c2
        cc += bytes([0xC2, 0x02]) + c1
        cc = bytes([len(cc)]) + cc
        self._send(conn, tpkt_wrap(cc))

    def _on_s7pdu(self, conn, pdu):
        if len(pdu) < 10 or pdu[0] != 0x32:
            self.store.add_event(f"S7 PDU 头非法 {len(pdu)}B，忽略")
            return
        rosctr = pdu[1]
        if rosctr != 0x01:           # 只处理 Job
            return
        pdu_ref = int.from_bytes(pdu[4:6], "big")
        plen = int.from_bytes(pdu[6:8], "big")
        if 10 + plen > len(pdu):
            return
        params = pdu[10:10 + plen]
        fn = params[0] if params else 0xFF
        if fn == 0xF0:               # Setup communication
            req_pdu_len = int.from_bytes(params[6:8], "big") if len(params) >= 8 else 480
            with self._lock:
                self.counters["setups"] += 1
            pdu_len = min(max(req_pdu_len, 240), 960)
            ack_params = bytes([0xF0, 0x00, 0x00, 0x01, 0x00, 0x01]) + struct.pack(">H", pdu_len)
            ack = s7_ack_header(pdu_ref, len(ack_params), 0) + ack_params
            self._send(conn, tpkt_wrap(cotp_dt(ack)))
        elif fn == 0x04:             # Read Var
            self._on_read(conn, pdu_ref, params)
        elif fn == 0x05:             # Write Var
            self._on_write(conn, pdu_ref, params, pdu)
        else:
            self.store.add_event(f"S7 未知功能码 0x{fn:02X}，忽略")

    def _parse_items(self, params, offset=2):
        """read/write 参数区 items：每12字节；返回 dict(raw, area, db, addr, length, count)"""
        items = []
        try:
            count = params[1]
        except Exception:
            return items
        i = offset
        for _ in range(count):
            if i + 12 > len(params):
                break
            raw = params[i:i + 12]
            if raw[0] != 0x12:
                i += 12
                continue
            # 兼容两种地址规格：
            #   A) [12 0A 10][area][db2][addr3][00][len2]        (Siemens/snap7 官方)
            #   B) [12 0A 10][WL][count2][db2][area][addr3]      (带传输规格)
            area = raw[3]
            db = int.from_bytes(raw[4:6], "big")
            addr = int.from_bytes(raw[6:9], "big")
            length = int.from_bytes(raw[10:12], "big")
            items.append({"raw": raw, "area": area, "db": db, "addr": addr,
                          "length": length,
                          "count": int.from_bytes(raw[4:6], "big")})
            i += 12
        return items

    def _db77_like(self, item):
        """宽松识别“读 DB77 锁格位图”的请求（不同 snap7 变体布局）"""
        raw = item["raw"]
        db77 = b"\x00\x4d"
        has_db77 = db77 in raw
        area_ok = raw[3] == 0x84 or raw[8] == 0x84
        wl_byte = raw[3] in (0x02, 0x01)
        if has_db77 and (area_ok or wl_byte):
            return True
        if raw[3] == 0x84 and item["db"] == 77:
            return True
        return False

    def _read_data_for(self, area, db, addr, length):
        """返回 (return_code, data)；DB77 返回位图切片，其它区域按不存在处理"""
        if area == 0x84 and db == 77:
            with self._lock:
                self._last_read_at = time.time()
                self.counters["reads"] += 1
            if addr + length <= DB77_SIZE:
                with self._lock:
                    return 0xFF, bytes(self.db77[addr:addr + length])
            return 0x05, b""
        if area == 0x84:             # 其它 DB 返回全 0
            return 0xFF, bytes(length)
        return 0x05, b""             # 非DB区：不存在

    def _on_read(self, conn, pdu_ref, params):
        items = self._parse_items(params)
        data_parts = []
        for item in items:
            with self._lock:
                self._last_read_at = time.time()
                self.counters["reads"] += 1
            if self._db77_like(item):
                # WCS 每次轮询读 25 字节 DB77（0..25）；无论其 count 字段如何编码均回 25B
                with self._lock:
                    data = bytes(self.db77[:DB77_SIZE])
                data_parts.append(bytes([0xFF, 0x04]) + struct.pack(">H", len(data)) + data)
            else:
                rc, data = self._read_data_for(item["area"], item["db"], item["addr"], item["length"])
                if rc == 0xFF:
                    data_parts.append(bytes([0xFF, 0x04]) + struct.pack(">H", len(data)) + data)
                else:
                    data_parts.append(bytes([rc, 0x00, 0x00, 0x00]))
        dlen = sum(len(p) for p in data_parts)
        ack = s7_ack_header(pdu_ref, 2, dlen) + bytes([0x04, 0x00]) + b"".join(data_parts)
        self._send(conn, tpkt_wrap(cotp_dt(ack)))

    def _on_write(self, conn, pdu_ref, params, pdu):
        items = self._parse_items(params)
        dlen0 = int.from_bytes(pdu[8:10], "big")
        # 数据区逐 item: 4字节头(00 transport len2)+值
        data = pdu[10 + len(params): 10 + len(params) + dlen0]
        applied = []
        pos = 0
        for (area, db, addr, length) in items:
            rc = 0xFF
            if pos + 4 <= len(data):
                tr, ln = data[pos + 1], int.from_bytes(data[pos + 2:pos + 4], "big")
                val = data[pos + 4: pos + 4 + ln]
                pos += 4 + ln
                if area == 0x84 and db == 77 and tr == 0x04:
                    with self._lock:
                        self.counters["writes"] += 1
                        for j, b in enumerate(val):
                            if addr + j < DB77_SIZE:
                                self.db77[addr + j] = b
                    applied.append((addr, val))
            else:
                rc = 0x05
            # data 应答无需逐项；统一成功即可
        ack = s7_ack_header(pdu_ref, 2, 0) + bytes([0x05, 0x00])
        self._send(conn, tpkt_wrap(cotp_dt(ack)))
        if applied:
            self.store.add("S7", "事件", f"WCS 写 DB77 offset={applied[0][0]} len={len(applied[0][1])}（当前WCS不回写，仅记录）")

    def _describe(self, tpkt):
        """帧摘要（供报文总览 parsed 列）"""
        payload = tpkt[4:]
        if len(payload) < 2:
            return "TPKT 空载荷"
        li = payload[0]
        if li > len(payload) - 1:
            return f"COTP LI越界 li={li}"
        tpdu = payload[1:1 + li]
        t = tpdu[0] if tpdu else -1
        if t == 0xE0:
            return f"COTP 连接请求 CR (srcTSAP={tpdu[-4:-2].hex() if len(tpdu) >= 4 else '?'} dstTSAP={tpdu[-2:].hex() if len(tpdu) >= 2 else '?'})"
        if t == 0xD0:
            return "COTP 连接确认 CC"
        if t in (0xF0, 0x0F):
            if t == 0xF0:
                pdu = payload[1 + li:]
            else:
                pdu = tpdu[3:]
            if len(pdu) < 10 or pdu[0] != 0x32:
                return f"COTP DT {len(tpdu)}B"
            ros = pdu[1]
            plen = int.from_bytes(pdu[6:8], "big")
            fn = pdu[10] if plen >= 1 and len(pdu) > 10 else None
            kind = {0x01: "Job", 0x03: "ACK"}.get(ros, f"ROS{ros}")
            fname = {0xF0: "SetupComm", 0x04: "ReadVar", 0x05: "WriteVar"}.get(fn, f"fn=0x{fn:02X}" if fn is not None else "?")
            return f"S7 {kind} {fname} ref={int.from_bytes(pdu[4:6], 'big')}"
        return f"COTP type=0x{t:02X}"
