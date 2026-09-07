# -*- coding: utf-8 -*-
"""dev_s7fmt.py — 对真实 snap7.dll 找出能被正确解析的 读DB77 应答格式（用 x64 Python 运行）"""
import ctypes
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mock_logstore
import mock_s7comm as M

DB = [0] * 25
DB[0] = 0x48  # bit3+bit6 已置位，用于判定数据是否真的到达客户端


def run_case(fmt, hdr12=False, plen_bytes=b"\x04\x00"):
    store = mock_logstore.MessageStore(max_keep=500)
    srv = M.S7Server("127.0.0.1", 102, store)
    srv.set_bitmap_bytes(bytes(DB))
    srv.start()
    time.sleep(0.2)

    class Patched(M.S7Server):
        pass

    # 覆写 _on_read：使用指定格式应答
    def on_read(self, conn, pdu_ref, params):
        items = M.S7Server._parse_items(self, params)
        data = bytes(self.db77[:25])
        item = bytes([0xFF, 0x04]) + struct.pack(">H", 25) + data
        plen = len(plen_bytes)
        hdr = M.s7_header(0x03, pdu_ref, plen, len(item))
        if hdr12:
            hdr = hdr + b"\x00\x00"
        ack = hdr + plen_bytes + item
        self._send(conn, M.tpkt_wrap(M.cotp_dt(ack)))

    srv._on_read = lambda conn, ref, params: on_read(srv, conn, ref, params)
    dll = ctypes.CDLL(r"D:\WCS\WCSApp\WCS_httpServer\release_WcsHttpServer\snap7.dll")
    dll.Cli_Create.restype = ctypes.c_void_p
    dll.Cli_ConnectTo.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
    dll.Cli_ConnectTo.restype = ctypes.c_int
    dll.Cli_DBRead.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
    dll.Cli_DBRead.restype = ctypes.c_int
    dll.Cli_Disconnect.argtypes = [ctypes.c_void_p]
    dll.Cli_Destroy.argtypes = [ctypes.c_void_p]
    cli = dll.Cli_Create()
    rc = dll.Cli_ConnectTo(cli, b"127.0.0.1", 0, 1)
    out = None
    if rc == 0:
        buf = (ctypes.c_ubyte * 25)()
        rc2 = dll.Cli_DBRead(cli, 77, 0, 25, ctypes.cast(buf, ctypes.c_void_p))
        out = bytes(buf)
        dll.Cli_Disconnect(cli)
    dll.Cli_Destroy(cli)
    srv.stop()
    got = None
    if out is not None:
        got = "bits" if out[0] == DB[0] else out.hex(" ")
    return rc, got


if __name__ == "__main__":
    cases = [
        ("A hdr10 plen2[04,00]", False, b"\x04\x00"),
        ("B hdr10 plen2[04,01]", False, b"\x04\x01"),
        ("C hdr10 plen1[04]", False, b"\x04"),
        ("D hdr12 plen2[04,00]", True, b"\x04\x00"),
        ("E hdr12 plen2[04,01]", True, b"\x04\x01"),
        ("F hdr10 plen2[FF,00]?", False, b"\xff\x00"),
    ]
    for name, h12, pb in cases:
        try:
            rc, got = run_case(name, h12, pb)
            print(f"{name:28s} connect={rc} dbread={rc} data={got}")
        except Exception as e:
            print(f"{name:28s} EXC {e!r}")
