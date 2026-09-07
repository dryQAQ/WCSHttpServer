# -*- coding: utf-8 -*-
"""
field_rfid_sender.py — 现场旁路工具: RFID 推送模拟器(服务端)
作用: 扮演 "RFID 推送服务端", 等 WCS_httpServer 连入后, 向 WCS 推送
      {SNxxxx|01|EPC}0D 帧 (字面 0D 结尾, 与现场协议一致), 模拟读码器读到物件。

角色说明: WCS 是 TCP 客户端, 主动连接 配置中 rfidPushServerIp:rfidPushServerPort。
  因此本工具在客户机监听该地址; 测试时把配置里 rfidPushServerIp 临时指向运行本工具的机器
  (同机则 127.0.0.1), 重启 WCS 即可; 真 RFID 服务端不需要停(它只是暂时没有 WCS 连入)。

用法:
  python field_rfid_sender.py                       # 交互模式(输入EPC回车即推一帧)
  python field_rfid_sender.py --port 2010
  python field_rfid_sender.py --auto EPC1,EPC2,EPC3 --interval 500   # 连上后自动逐条推
  python field_rfid_sender.py --listen 0.0.0.0      # WCS 在别的机器时监听所有网卡

交互命令:
  直接输入 EPC        推一帧(流水号自动 SN0001~SN0099 递增)
  n <EPC>             推 NOREAD 帧  {SN|01|NOREAD}0D
  h                   查看统计(WCS连接数/已推帧/收到心跳)
  q                   退出
"""
import argparse
import re
import socket
import sys
import threading
import time

TAIL = b"0D"   # 字面 '0D' (0x30 0x44), 与现场协议一致; 想改回车字节可用 --tail cr


def now():
    return time.strftime("%H:%M:%S")


class RfidSender:
    def __init__(self, host, port, no_input=False):
        self.host, self.port = host, port
        self.no_input = no_input
        self.clients = []
        self.lock = threading.Lock()
        self.seq = 1
        self.sent = 0
        self.hb = 0
        self.stop = False

    def serve(self):
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            srv.bind((self.host, self.port))
        except OSError as e:
            print(f"[{now()}] bind {self.host}:{self.port} 失败: {e} (端口被占或IP不是本机)")
            sys.exit(2)
        srv.listen(4)
        srv.settimeout(0.5)
        print(f"[{now()}] RFID推送模拟器监听 {self.host}:{self.port}  (等 WCS 连入; 输入EPC回车=推一帧, q=退出)")
        while not self.stop:
            try:
                c, addr = srv.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            with self.lock:
                self.clients.append(c)
            print(f"[{now()}] WCS 已连入 {addr[0]}:{addr[1]}  当前连接数={len(self.clients)}")
            threading.Thread(target=self.recv_loop, args=(c,), daemon=True).start()
        for c in list(self.clients):
            try:
                c.close()
            except OSError:
                pass

    def recv_loop(self, c):
        """收 WCS 侧发来的字节(心跳 RFID{HEARTBEAT}0D 等), 只统计不处理"""
        try:
            while not self.stop:
                d = c.recv(4096)
                if not d:
                    break
                if b"HEARTBEAT" in d:
                    self.hb += 1
                else:
                    print(f"[{now()}] 收到WCS: {d[:80]!r}")
        except OSError:
            pass
        finally:
            with self.lock:
                if c in self.clients:
                    self.clients.remove(c)
            print(f"[{now()}] WCS 断开 (当前连接数={len(self.clients)}; WCS 约3s后会重连, 无需操作)")

    def send(self, epc, note=""):
        with self.lock:
            peers = list(self.clients)
        if not peers:
            print(f"[{now()}] 没有已连接的 WCS, 帧未发送 (请确认WCS已启动且 rfidPushServerIp 指向本机)")
            return False
        seq = "SN%04d" % self.seq
        raw = ("{%s|01|%s}" % (seq, epc)).encode("ascii") + TAIL
        for c in peers:
            try:
                c.sendall(raw)
            except OSError:
                pass
        self.seq = self.seq % 99 + 1
        self.sent += 1
        m = re.search(r"(\d+)", seq)
        car = int(m.group(1)) if m else 0
        print(f"[{now()}] 推送 {note}{raw.decode()}  (WCS识别: 小车号={car})")
        return True

    def auto(self, epcs, interval_ms):
        def run():
            t0 = time.time()
            while time.time() - t0 < 60:
                with self.lock:
                    n = len(self.clients)
                if n > 0:
                    break
                time.sleep(0.5)
            if not self.clients:
                print(f"[{now()}] 60s内无WCS连入, 自动推送取消")
                return
            print(f"[{now()}] WCS已连入, 开始自动推送 {len(epcs)}帧")
            for epc in epcs:
                if self.stop:
                    break
                self.send(epc)
                time.sleep(interval_ms / 1000.0)
            print(f"[{now()}] 自动推送完成 共{len(epcs)}帧 (仍可手动输入EPC继续)")
        threading.Thread(target=run, daemon=True).start()

    def input_loop(self):
        while not self.stop:
            try:
                line = input("EPC> ").strip()
            except (EOFError, KeyboardInterrupt):
                break
            if not line:
                continue
            if line.lower() in ("q", "quit", "exit"):
                self.stop = True
                break
            if line.lower() == "h":
                with self.lock:
                    n = len(self.clients)
                print(f"连接数={n} 已推={self.sent} 收到心跳={self.hb}")
                continue
            if line.lower().startswith("n "):
                self.send("NOREAD", note="NOREAD帧 ")
                continue
            self.send(line)
        self.stop = True
        print("退出")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--listen", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=2010)
    ap.add_argument("--auto", default="")
    ap.add_argument("--interval", type=int, default=300)
    ap.add_argument("--no-input", action="store_true")
    a = ap.parse_args()
    s = RfidSender(a.listen, a.port, a.no_input)
    if not a.no_input:
        threading.Thread(target=s.input_loop, daemon=True).start()
    if a.auto:
        s.auto([e.strip() for e in a.auto.split(",") if e.strip()], a.interval)
    try:
        s.serve()
    except KeyboardInterrupt:
        pass
