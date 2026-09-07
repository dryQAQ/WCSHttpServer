# -*- coding: utf-8 -*-
"""
field_plc_sim.py — 现场旁路工具: PLC 落格反馈模拟器(客户端)
作用: 扮演 PLC, 以 TCP 客户端连入 WCS_httpServer 的 plcListenPort(默认2000),
  接收 WCS 发来的落格指令 {EPC|格口|小车}, 并按现场格式回 5 字段落格反馈
  {EPC|格口|小车|小车+2|状态码} (状态码默认1=成功), 模拟"PLC执行落格成功"。

角色说明: WCS 是 TCP 服务端(监听2000), 真 PLC 是客户端。本工具=另一个客户端。
  ⚠ WCS 会把落格指令广播给所有已连客户端: 若真 PLC 仍连接且机械可动,
    请先断开真PLC或切断机械使能, 否则模拟RFID推送会触发真机动作!

用法:
  python field_plc_sim.py                        # 连 127.0.0.1:2000, 收到指令自动回 状态1
  python field_plc_sim.py --host 192.168.1.50    # WCS 在其他机器
  python field_plc_sim.py --status 2             # 自动回 状态2(无格口, 练异常)
  python field_plc_sim.py --manual               # 手动模式: 收到指令后按回车才回执
交互(任意模式可用):
  直接输入 原文          按原文原样发给 WCS (例如 {start} / {stop} / {EPC|001|027|029|1})
  s<码>                  切换自动回执状态码(1/2/3/0)
  a / m                  自动回执 <-> 手动回车回执 切换
  h                      统计
  q                      退出
断线后每2秒自动重连。
"""
import argparse
import re
import socket
import sys
import threading
import time

STATUS_DESC = {"1": "成功", "2": "无格口(异常)", "3": "信息不全(异常)", "0": "其它(按成功)"}


def now():
    return time.strftime("%H:%M:%S")


def pad3(n):
    try:
        return "%03d" % int(n)
    except Exception:
        return str(n)


class PlcSim:
    def __init__(self, host, port, status="1", manual=False, no_input=False):
        self.host, self.port = host, port
        self.status = status
        self.manual = manual
        self.no_input = no_input
        self.sock = None
        self.recv_cnt = 0
        self.send_cnt = 0
        self.last_cmd = None
        self.stop = False

    def run(self):
        if not self.no_input:
            threading.Thread(target=self.input_loop, daemon=True).start()
        while not self.stop:
            try:
                s = socket.create_connection((self.host, self.port), timeout=5)
            except OSError as e:
                print(f"[{now()}] 连接 {self.host}:{self.port} 失败: {e}  (2s后重试)")
                time.sleep(2)
                continue
            self.sock = s
            print(f"[{now()}] 已连接 WCS {self.host}:{self.port}  (回执状态码={self.status} 模式={'手动回车回执' if self.manual else '自动回执'})")
            buf = ""
            try:
                while not self.stop:
                    s.settimeout(0.5)
                    try:
                        d = s.recv(4096)
                    except socket.timeout:
                        continue
                    except OSError:
                        break
                    if not d:
                        break
                    buf += d.decode("ascii", errors="replace")
                    pos = 0
                    while True:
                        j = buf.find("}", pos)
                        if j < 0:
                            break
                        self.on_frame(buf[pos:j + 1])
                        pos = j + 1
                    buf = buf[pos:]
            except OSError:
                pass
            try:
                s.close()
            except OSError:
                pass
            self.sock = None
            print(f"[{now()}] 连接断开, 2s后重连...")
            time.sleep(2)
        print("退出")

    def on_frame(self, seg):
        text = seg.strip()
        m = re.fullmatch(r"\{([^{}]*)\}", text)
        if not m:
            return
        parts = [p.strip() for p in m.group(1).split("|")]
        self.recv_cnt += 1
        if parts == ["start"]:
            print(f"[{now()}] 收到 {{start}} (批次开始信号)")
            return
        if parts == ["stop"]:
            print(f"[{now()}] 收到 {{stop}} (批次停止信号)")
            return
        if len(parts) >= 3:
            self.last_cmd = parts
            print(f"[{now()}] 收到落格指令(第{self.recv_cnt}条): EPC={parts[0]} 格口={parts[1]} 小车={parts[2]}")
            if self.manual:
                print(f"[{now()}]   (手动模式: 按 回车 回执该条)")
        else:
            print(f"[{now()}] 收到 {text} (字段数={len(parts)}, 非指令帧)")
        if not self.manual:
            self.reply()

    def reply(self):
        if not self.last_cmd:
            return
        epc, grid, car = self.last_cmd[0], self.last_cmd[1], self.last_cmd[2]
        car3 = pad3(car)
        try:
            last3 = pad3(int(car) + 2)
        except Exception:
            last3 = car3
        raw = ("{%s|%s|%s|%s|%s}" % (epc, grid, car3, last3, self.status)).encode("ascii")
        self.send(raw, f"落格反馈({STATUS_DESC.get(self.status, self.status)})")

    def send(self, raw, note=""):
        if not self.sock:
            print(f"[{now()}] 未连接, 未发送")
            return False
        try:
            self.sock.sendall(raw)
        except OSError as e:
            print(f"[{now()}] 发送失败: {e}")
            return False
        self.send_cnt += 1
        print(f"[{now()}] 发送 {note} {raw.decode()}")
        return True

    def input_loop(self):
        while not self.stop:
            try:
                line = input("PLC> ").strip()
            except (EOFError, KeyboardInterrupt):
                break
            if not line:
                if self.manual and self.last_cmd:
                    self.reply()
                continue
            if line.lower() in ("q", "quit", "exit"):
                self.stop = True
                break
            if line.lower() == "h":
                print(f"连接={'在' if self.sock else '断'} 收={self.recv_cnt} 发={self.send_cnt} "
                      f"状态码={self.status} 回执={'手动' if self.manual else '自动'} 最后指令={self.last_cmd}")
                continue
            if line.lower() == "a":
                self.manual = False
                print("切换为 自动回执")
                continue
            if line.lower() == "m":
                self.manual = True
                print("切换为 手动回车回执")
                continue
            if line.startswith("s") and line[1:].isdigit():
                self.status = line[1:]
                print(f"自动回执状态码={self.status} ({STATUS_DESC.get(self.status, '?')})")
                continue
            if line.startswith("{"):
                self.send(line.encode("ascii", errors="replace"), "原文")
            else:
                self.send(line.encode("ascii", errors="replace"), "原文(无花括号)")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=2000)
    ap.add_argument("--status", default="1")
    ap.add_argument("--manual", action="store_true")
    ap.add_argument("--no-input", action="store_true")
    a = ap.parse_args()
    p = PlcSim(a.host, a.port, a.status, a.manual, a.no_input)
    try:
        p.run()
    except KeyboardInterrupt:
        pass
