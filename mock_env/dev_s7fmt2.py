# -*- coding: utf-8 -*-
"""dev_s7fmt2.py — 干净版应答格式矩阵实验（x64 Python + 真实 snap7.dll）"""
import ctypes
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mock_logstore
import mock_s7comm as M

DLL = r"D:\WCS\WCSApp\WCS_httpServer\release_WcsHttpServer\snap7.dll"
FLAG = 0x48  # bit3+bit6


def make_ack(fmt, pdu_ref, payload25):
    item = bytes([0xFF, 0x04]) + struct.pack(">H", 25) + payload25
    dlen = len(item)
    if fmt == "A":          # hdr10, param[04 00]
        hdr = M.s7_header(0x03, pdu_ref, 2, dlen)
        return hdr + b"\x04\x00" + item
    if fmt == "D":          # hdr12(err 00 00), param[04 00]
        hdr = M.s7_header(0x03, pdu_ref, 2, dlen) + b"\x00\x00"
        return hdr + b"\x04\x00" + item
    if fmt == "E":          # hdr12, param[04 01]
        hdr = M.s7_header(0x03, pdu_ref, 2, dlen) + b"\x00\x00"
        return hdr + b"\x04\x01" + item
    if fmt == "G":          # hdr12, param 仅[04] (plen=1)
        hdr = M.s7_header(0x03, pdu_ref, 1, dlen) + b"\x00\x00"
        return hdr + b"\x04" + item
    if fmt == "H":          # hdr10, param[04 01]
        hdr = M.s7_header(0x03, pdu_ref, 2, dlen)
        return hdr + b"\x04\x01" + item
    if fmt == "I":          # hdr12, 无param(plen=0), 数据直接放 data 区
        hdr = M.s7_header(0x03, pdu_ref, 0, dlen) + b"\x00\x00"
        return hdr + item
    if fmt == "J":          # hdr10 无param, item in data
        hdr = M.s7_header(0x03, pdu_ref, 0, dlen)
        return hdr + item
    raise ValueError(fmt)


def one_case(fmt):
    store = mock_logstore.MessageStore(max_keep=200)
    srv = M.S7Server("127.0.0.1", 102, store)
    srv.set_bitmap_bytes(bytes([FLAG] + [0] * 24))
    srv.start()
    time.sleep(0.25)
    orig = srv._on_read

    def on_read(self_, conn, pdu_ref, params):
        payload = bytes(srv.db77[:25])
        srv._send(conn, M.tpkt_wrap(M.cotp_dt(make_ack(fmt, pdu_ref, payload))))

    srv._on_read = lambda conn, ref, params: on_read(srv, conn, ref, params)
    dll = ctypes.WinDLL(DLL)
    dll.Cli_Create.restype = ctypes.c_void_p
    for name, at, rt in [("Cli_ConnectTo", [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int], ctypes.c_int),
                         ("Cli_DBRead", [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_void_p], ctypes.c_int),
                         ("Cli_Disconnect", [ctypes.c_void_p], ctypes.c_int),
                         ("Cli_Destroy", [ctypes.c_void_p], None)]:
        f = getattr(dll, name)
        f.argtypes = at
        if rt is not None:
            f.restype = rt
    cli = dll.Cli_Create()
    if not cli:
        srv.stop()
        return "Cli_Create failed"
    rc = dll.Cli_ConnectTo(cli, b"127.0.0.1", 0, 1)
    if rc != 0:
        srv.stop()
        return f"connect rc={rc}"
    buf = (ctypes.c_ubyte * 25)()
    rc2 = dll.Cli_DBRead(cli, 77, 0, 25, ctypes.cast(buf, ctypes.c_void_p))
    dll.Cli_Disconnect(cli)
    h = ctypes.c_void_p(cli)
    dll.Cli_Destroy(ctypes.byref(h))      # Cli_Destroy(S7Object*)
    srv.stop()
    data = bytes(buf)
    ok = data[0] == FLAG
    return f"rc={rc2} data={'BITS-OK' if ok else data.hex(' ')}"


if __name__ == "__main__":
    for fmt in "ADEHGIJ":
        try:
            print(fmt, "->", one_case(fmt))
        except Exception as e:
            print(fmt, "-> EXC", repr(e))
        time.sleep(0.4)
