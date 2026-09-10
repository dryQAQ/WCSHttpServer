# -*- coding: utf-8 -*-
"""
mock_channels.py — 仿真台全部通道服务
  RfidServer       : RFID 推送服务端（TCP 监听，WCS 主动连入；帧 {SN|设备码|EPC}0D）
  PlcClient        : PLC 客户端（TCP 连接 WCS plcListenPort；收发 {} 帧）
  HttpWmsPush      : 软件/WMS 下发客户端（POST WCS:8191 三个接口）
  HttpGatewayServer: 软件/WMS 网关（接收 H7/H8 回传，按样例应答 + appkey/method 校验）
  RfidQueryServer  : RFID 查询服务（接收 WCS 的 EPC→barcode(SKU) 查询）
所有通道收发经 store.add() 统一留存；UI 通过 service.status() 轮询。
"""
import json
import re
import socket
import socketserver
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler

import mock_proto as proto

try:
    import queue
except Exception:
    import Queue as queue

BUFSIZE = 65536


def _hexs(b):
    return b.hex(" ").upper() if b else ""


class _Base:
    """公共服务骨架：线程管理 + 状态快照"""
    def __init__(self, store):
        self.store = store
        self._stop = threading.Event()
        self._threads = []
        self._lock = threading.Lock()
        self._counters = {"accepts": 0, "recv": 0, "sent": 0}
        self._status = {"running": False, "detail": ""}
        self._last_io = 0.0

    # ---- 子类实现 ----
    def _run(self):
        raise NotImplementedError

    def _teardown(self):
        pass

    # ---- 公共服务 ----
    def start(self):
        if self._status.get("running"):
            return
        self._stop.clear()
        self._status["running"] = True
        t = threading.Thread(target=self._run, daemon=True)
        self._threads.append(t)
        t.start()

    def stop(self):
        self._stop.set()
        self._status["running"] = False
        self._teardown()

    def _bump(self, key):
        with self._lock:
            self._counters[key] = self._counters.get(key, 0) + 1
            self._last_io = time.time()

    def status(self):
        with self._lock:
            return {"running": self._status["running"],
                    "detail": self._status["detail"],
                    "counters": dict(self._counters),
                    "last_io": self._last_io}


# ============================================================================
# 1) RFID 推送服务端
# ============================================================================
class RfidServer(_Base):
    def __init__(self, host, port, store):
        super().__init__(store)
        self.host, self.port = host, port
        self.clients = []            # list[socket]
        self.clients_lock = threading.Lock()
        self._srv = None

    def _run(self):
        try:
            srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            srv.bind((self.host, self.port))
            srv.listen(4)
            srv.settimeout(0.5)
            self._srv = srv
        except OSError as e:
            self.store.add_event(f"RFID服务端 监听失败 {self.host}:{self.port} -> {e}")
            self._status["running"] = False
            return
        self._status["detail"] = f"监听 {self.host}:{self.port} 等待WCS连入"
        self.store.add_event(f"RFID推送服务端已启动，监听 {self.host}:{self.port}（等待 WCS 连入）")
        while not self._stop.is_set():
            try:
                conn, addr = srv.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            with self.clients_lock:
                self.clients.append(conn)
            self._bump("accepts")
            self.store.add(f"RFID", "事件", f"WCS 已连入 远端={addr[0]}:{addr[1]} 当前连接数={len(self.clients)}")
            t = threading.Thread(target=self._client_loop, args=(conn, addr), daemon=True)
            self._threads.append(t)
            t.start()
        self._teardown()

    def _client_loop(self, conn, addr):
        conn.settimeout(1.0)
        buf = b""
        while not self._stop.is_set():
            try:
                data = conn.recv(BUFSIZE)
            except socket.timeout:
                continue
            except OSError:
                break
            if not data:
                break
            self._bump("recv")
            buf += data
            # 按 } 抽帧展示（心跳帧整行识别），内容 ASCII
            while True:
                i = buf.find(b"}")
                if i < 0:
                    if len(buf) > 65536:
                        buf = b""
                    break
                seg = buf[: i + 1]
                buf = buf[i + 1:]
                # 剥掉上一帧遗留的帧尾字符（0D/\r/\n，与 WCS 侧容忍一致）
                while seg[:2] == b"0D" or seg[:1] in (b"\r", b"\n"):
                    seg = seg[2:] if seg[:2] == b"0D" else seg[1:]
                    if not seg:
                        break
                if not seg:
                    continue
                try:
                    text = seg.decode("ascii", errors="replace")
                except Exception:
                    text = repr(seg)
                is_heart = "HEARTBEAT" in text.upper()
                summary = f"收到WCS帧: {text.strip()}" if not is_heart else f"WCS心跳 {text.strip()}"
                parsed = "" if is_heart else (proto.describe_rfid_frame(text) or "")
                self.store.add("RFID", "收", summary, raw_text=text,
                               raw_hex=_hexs(seg), parsed=parsed,
                               ref="HEARTBEAT" if is_heart else "")
        with self.clients_lock:
            if conn in self.clients:
                self.clients.remove(conn)
        try:
            conn.close()
        except OSError:
            pass
        self.store.add("RFID", "事件", f"WCS 连接断开 远端={addr[0]}:{addr[1]}（WCS 约3s后自动重连）")

    def send_frame(self, raw_bytes, note=""):
        with self.clients_lock:
            peers = list(self.clients)
        if not peers:
            return False, "无已连接的 WCS（请先启动 WCS 且其自动连入本服务）"
        ok = 0
        for c in peers:
            try:
                c.sendall(raw_bytes)
                ok += 1
            except OSError as e:
                self.store.add_event(f"RFID发送失败: {e}")
        self._bump("sent")
        try:
            text = raw_bytes.decode("ascii", errors="replace")
        except Exception:
            text = repr(raw_bytes)
        self.store.add("RFID", "发", (note + " " if note else "") + text.strip(),
                       raw_text=text, raw_hex=_hexs(raw_bytes),
                       parsed=proto.describe_rfid_frame(text) or "")
        return ok > 0, f"已发送给 {ok} 个连接"

    def connected_client_count(self):
        with self.clients_lock:
            return len(self.clients)

    def _teardown(self):
        with self.clients_lock:
            for c in list(self.clients):
                try:
                    c.close()
                except OSError:
                    pass
            self.clients.clear()
        if self._srv:
            try:
                self._srv.close()
            except OSError:
                pass
            self._srv = None


# ============================================================================
# 2) PLC 客户端（连接 WCS plcListenPort）
# ============================================================================
class PlcClient(_Base):
    def __init__(self, host, port, store, reconnect_s=3):
        super().__init__(store)
        self.host, self.port = host, port
        self.reconnect_s = reconnect_s
        self._sock = None
        self._send_lock = threading.Lock()
        self._kick = threading.Event()
        self.on_cmd = None          # callable(fields)->list[str] 可返回应答
        self.connected_since = 0.0

    @property
    def connected(self):
        return self._sock is not None

    def restart(self):
        """立即触发一次重连尝试（不停止服务）"""
        if not self.connected:
            self._kick.set()

    def _nap(self, sec):
        """等待 sec 秒或收到 kick/stop；返回 False 表示应退出"""
        end = time.time() + sec
        while time.time() < end:
            if self._stop.is_set():
                return False
            if self._kick.is_set():
                self._kick.clear()
                return True
            time.sleep(0.1)
        return True

    def _run(self):
        self.store.add_event(f"PLC 客户端将连接 WCS {self.host}:{self.port}（每 {self.reconnect_s}s 自动重连）")
        while not self._stop.is_set():
            sock = None
            try:
                sock = socket.create_connection((self.host, self.port), timeout=3)
            except OSError as e:
                self._status["detail"] = f"连接失败: {e}（{self.reconnect_s}s后重试）"
                if not self._nap(self.reconnect_s):
                    break
                continue
            self._sock = sock
            self.connected_since = time.time()
            sock.settimeout(1.0)
            self._status["detail"] = f"已连接 WCS {self.host}:{self.port}"
            self.store.add("PLC", "事件", f"PLC(仿真) 已连接 WCS TCP {self.host}:{self.port}")
            buf = ""
            while not self._stop.is_set():
                try:
                    data = sock.recv(BUFSIZE)
                except socket.timeout:
                    continue
                except OSError:
                    break
                if not data:
                    break
                self._bump("recv")
                buf += data.decode("ascii", errors="replace")
                # 按 } 逐帧抽离（pos 推进，支持粘包/多帧）
                pos = 0
                while True:
                    j = buf.find("}", pos)
                    if j < 0:
                        break
                    self._on_frame(buf[pos: j + 1])
                    pos = j + 1
                buf = buf[pos:]
                if len(buf) > 65536:
                    buf = buf[-65536:]
            self._sock = None
            try:
                sock.close()
            except OSError:
                pass
            self.store.add("PLC", "事件", "PLC(仿真) 与 WCS 连接断开，准备重连")
            if self._stop.is_set():
                break
            if not self._nap(self.reconnect_s):
                break
        self._teardown()

    def _on_frame(self, seg):
        self._bump("recv")
        text = seg.strip()
        frames = proto.parse_braced_frames(text)
        for fields in frames:
            desc = proto.describe_plc_frame(fields) if fields else "空帧"
            self.store.add("PLC", "收", text, raw_text=text,
                           raw_hex=_hexs(seg.encode("ascii", errors="replace")),
                           parsed=desc, ref=fields[0] if fields else "")
            if self.on_cmd:
                try:
                    replies = self.on_cmd(fields) or []
                except Exception as e:
                    replies = []
                    self.store.add_event(f"PLC 应答回调异常: {e}")
                for r in replies:
                    self.send_text(r)

    def send_bytes(self, raw, note=""):
        sock = self._sock
        if sock is None:
            self.store.add("PLC", "发", "【未连接，发送失败】" + (note or ""))
            return False
        with self._send_lock:
            try:
                sock.sendall(raw)
            except OSError as e:
                self.store.add_event(f"PLC发送失败: {e}")
                return False
        self._bump("sent")
        try:
            text = raw.decode("ascii", errors="replace")
        except Exception:
            text = repr(raw)
        self.store.add("PLC", "发", (note + " " if note else "") + text.strip(),
                       raw_text=text, raw_hex=_hexs(raw))
        return True

    def send_text(self, text, note=""):
        return self.send_bytes(text.encode("ascii", errors="replace"), note)

    def _teardown(self):
        if self._sock:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None


# ============================================================================
# 3) 软件/WMS 下发客户端（POST WCS HTTP）
# ============================================================================
class HttpWmsPush(_Base):
    """不常驻线程；每次 send() 在独立线程执行，结果回写 store + 回调"""
    def __init__(self, base_url, store, timeout=10):
        super().__init__(store)
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout
        self.on_result = None      # callable(text)

    def _run(self):
        pass  # 无常驻线程

    def send(self, path, body_str, headers=None, note=""):
        t = threading.Thread(target=self._do, args=(path, body_str, headers or {}, note),
                             daemon=True)
        self._threads.append(t)
        t.start()

    def _do(self, path, body_str, headers, note):
        url = self.base_url + path
        data = body_str.encode("utf-8") if isinstance(body_str, str) else body_str
        hdrs = {"Content-Type": "application/json"}
        hdrs.update(headers)
        req = urllib.request.Request(url, data=data, headers=hdrs, method="POST")
        self.store.add("WMS下发", "发", f"POST {path} len={len(data)} {(note or '')}".strip(),
                       raw_text=f"{url}\n{body_str}" if isinstance(body_str, str) else repr(body_str))
        t0 = time.time()
        try:
            resp = urllib.request.urlopen(req, timeout=self.timeout)
            code = resp.status
            resp_headers = dict(resp.headers.items())
            rbody = resp.read().decode("utf-8", errors="replace")
            resp.close()
            parsed = ""
            try:
                parsed = proto.pretty_json(rbody)
            except Exception:
                pass
            elapsed = int((time.time() - t0) * 1000)
            summary = f"HTTP {code} ({elapsed}ms) {path}"
            self.store.add("WMS下发", "收", summary, raw_text=rbody,
                           parsed=parsed, ref=path)
            if self.on_result:
                self.on_result(f"{summary}\n\n{rbody}")
        except urllib.error.HTTPError as e:
            elapsed = int((time.time() - t0) * 1000)
            rbody = e.read().decode("utf-8", errors="replace")
            parsed = ""
            try:
                parsed = proto.pretty_json(rbody)
            except Exception:
                pass
            self.store.add("WMS下发", "收", f"HTTP {e.code} ({elapsed}ms) {path}", raw_text=rbody, parsed=parsed)
            if self.on_result:
                self.on_result(f"HTTP {e.code} ({elapsed}ms)\n\n{rbody}")
        except Exception as e:
            elapsed = int((time.time() - t0) * 1000)
            self.store.add("WMS下发", "事件", f"请求异常({elapsed}ms): {e}")
            if self.on_result:
                self.on_result(f"请求异常: {e}")


# ============================================================================
# HTTP 服务公共基座
# ============================================================================
class _ThreadingHttp(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def handle_error(self, request, client_address):
        # 客户端(如 Qt)主动断开的 keep-alive 连接属常态，静默处理
        pass


class _JsonHttpServer(_Base):
    Handler = None

    def _make_handler(self):
        srv = self

        class H(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *a):
                pass

            def _handle(self):
                srv.handle_request(self)

            do_POST = _handle
            do_GET = _handle
            do_PUT = _handle
            do_DELETE = _handle
        return H

    def _run(self):
        # ★ 端口占用预检（Windows SO_REUSEADDR 会静默双绑，先探测避免“假启动”）
        try:
            probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 0)
            probe.bind((self.host, self.port))
            probe.close()
        except OSError as e:
            self.store.add_event(f"{self._name} 端口被占用 {self.host}:{self.port}（未接管；若同时运行了其它占用该端口的程序如 wms_mock_gui，请二选一）: {e}")
            self._status["running"] = False
            self._status["detail"] = f"端口被占用 {self.host}:{self.port}"
            return
        try:
            h = self._make_handler()
            srv = _ThreadingHttp((self.host, self.port), h)
        except OSError as e:
            self.store.add_event(f"{self._name} 监听失败 {self.host}:{self.port}: {e}")
            self._status["running"] = False
            return
        self._srv = srv
        self._status["detail"] = f"监听 {self.host}:{self.port}"
        self.store.add_event(f"{self._name}已启动，监听 {self.host}:{self.port}")
        try:
            srv.serve_forever(poll_interval=0.3)
        except Exception:
            pass
        self._status["running"] = False

    def start(self):
        self._name = self.__class__.__name__
        super().start()

    def _teardown(self):
        srv = getattr(self, "_srv", None)
        if srv:
            try:
                srv.shutdown()
            except Exception:
                pass
            try:
                srv.server_close()
            except Exception:
                pass
            self._srv = None


def _read_body(h):
    try:
        n = int(h.headers.get("Content-Length") or 0)
    except Exception:
        n = 0
    if n <= 0:
        return b""
    return h.rfile.read(n)


# ============================================================================
# 4) WMS 网关接收端（H7/H8）
# ============================================================================
class HttpGatewayServer(_JsonHttpServer):
    def __init__(self, host, port, store, cfg):
        super().__init__(store)
        self.host, self.port = host, port
        self.cfg = cfg
        self._rule_lock = threading.Lock()
        self.rule = {"mode": "auto",          # auto | custom | manual
                     "success": True,          # auto 时 success true/false
                     "http_code": 200,         # auto 时使用
                     "delay_ms": 0,
                     "custom_body": ""}        # custom 时使用（原文）
        self.pending = []                      # manual 模式待人工应答 [(seq, kind, order)]
        self.pending_lock = threading.Lock()
        self._last = {"h7": None, "h8": None, "h7_count": 0, "h8_count": 0}

    def set_rule(self, **kw):
        with self._rule_lock:
            self.rule.update(kw)

    def get_rule(self):
        with self._rule_lock:
            return dict(self.rule)

    def handle_request(self, h):
        length = int(h.headers.get("Content-Length") or 0)
        body = h.rfile.read(length) if length else b""
        raw_url = h.path
        method = h.command
        parsed_q = {}
        m = re.match(r"^([^?]*)(?:\?(.*))?$", raw_url)
        path = m.group(1) if m else raw_url
        if m and m.group(2):
            for kv in m.group(2).split("&"):
                if "=" in kv:
                    k, v = kv.split("=", 1)
                    parsed_q[k] = urllib.parse.unquote_plus(v)
        headers = {k.lower(): v for k, v in h.headers.items()}
        body_text = body.decode("utf-8", errors="replace")

        # 判定 H7/H8
        meth = parsed_q.get("method", "")
        if meth == self.cfg.method_h8:
            kind = "H8"
        elif meth == self.cfg.method_h7:
            kind = "H7"
        else:
            kind = "未知"
        # 报文留存
        parsed = ""
        try:
            parsed = proto.pretty_json(body_text) if body_text else ""
        except Exception:
            pass
        appkey = self.cfg.active_appkey
        q_appkey = parsed_q.get("appkey", "")
        h_appkey = headers.get("appkey", "")
        checks = []
        if meth:
            checks.append(f"method={meth} OK" if meth in (self.cfg.method_h7, self.cfg.method_h8)
                          else f"method={meth} ⚠非预期")
        else:
            checks.append("⚠缺少 method 参数")
        if q_appkey:
            checks.append(f"URL appkey={'一致' if q_appkey == appkey else '不一致:' + q_appkey + '(期望 ' + appkey + ')'}")
        else:
            checks.append("⚠URL 缺少 appkey 参数")
        if h_appkey:
            checks.append(f"Header AppKey={'一致' if h_appkey == appkey else '不一致:' + h_appkey}")
        else:
            checks.append("⚠Header 缺少 AppKey")
        chk_txt = "；".join(checks)
        self.store.add("WMS网关", "收",
                       f"{kind}回传 {method} {path} body={len(body)}B | {chk_txt}",
                       raw_text=f"{method} {raw_url}\nAppKey-Header: {h.headers.get('AppKey') or h.headers.get('appkey') or '(无)'}\n\n{body_text}",
                       parsed=parsed, ref=kind)
        self._last[kind] = body_text
        if kind == "H7":
            self._last["h7_count"] += 1
        elif kind == "H8":
            self._last["h8_count"] += 1
        with self._rule_lock:
            rule = dict(self.rule)
        if rule["mode"] == "manual":
            with self.pending_lock:
                self.pending.append({"kind": kind, "body": body_text,
                                     "time": time.strftime("%H:%M:%S"), "id": len(self.pending) + 1})
            self._respond(h, kind, 200, proto.to_json_bytes({"success": True,
                                                             "body": "已入人工应答队列",
                                                             "ts": proto.ts_now()}))
            return
        delay = max(int(rule.get("delay_ms") or 0), 0)
        if delay:
            time.sleep(delay / 1000.0)
        if rule["mode"] == "custom":
            try:
                resp_body = rule.get("custom_body", "").encode("utf-8")
                code = int(rule.get("http_code") or 200)
            except Exception:
                resp_body = b"{}"
                code = 200
        else:
            code = int(rule.get("http_code") or 200)
            if kind == "H8":
                obj = proto.reply_end_success()
            else:
                obj = proto.reply_fullbox_success()
            if not rule.get("success", True):
                obj["success"] = False
                obj["body"] = "模拟失败: " + obj["body"]
            resp_body = proto.to_json_bytes(obj)
        self._respond(h, kind, code, resp_body)

    def _respond(self, h, kind, code, resp_body):
        h.send_response(code)
        h.send_header("Content-Type", "application/json; charset=UTF-8")
        h.send_header("Content-Length", str(len(resp_body)))
        h.end_headers()
        try:
            h.wfile.write(resp_body)
        except OSError:
            pass
        text = resp_body.decode("utf-8", errors="replace")
        self.store.add("WMS网关", "发",
                       f"应答 {kind} HTTP {code} len={len(resp_body)}",
                       raw_text=text, parsed=proto.pretty_json(text))
        self._bump("sent")

    def manual_respond(self, index, mode, code=200, custom_body=None):
        """mode: success|fail|custom；用于 manual 模式队列"""
        with self.pending_lock:
            item = None
            for p in self.pending:
                if p["id"] == index:
                    item = p
                    self.pending.remove(p)
                    break
        if not item:
            return False, "队列中无此请求"
        body = custom_body
        if mode == "success":
            obj = proto.reply_end_success() if item["kind"] == "H8" else proto.reply_fullbox_success()
            body = proto.to_json_bytes(obj)
        elif mode == "fail":
            obj = proto.reply_end_success() if item["kind"] == "H8" else proto.reply_fullbox_success()
            obj["success"] = False
            body = proto.to_json_bytes(obj)
        else:
            body = (body or b"{}")
        self.store.add("WMS网关", "事件",
                       f"人工应答 队列#{item['id']} {item['kind']} mode={mode}")
        return True, f"已应答 #{item['id']}（原报文已在总览中，此处不再重复留存）"

    def last(self):
        return dict(self._last)


# ============================================================================
# 5) RFID 查询服务（EPC→barcode/SKU）
# ============================================================================
class RfidQueryServer(_JsonHttpServer):
    def __init__(self, host, port, store, cfg):
        super().__init__(store)
        self.host, self.port, self.cfg = host, port, cfg
        self.mapping = {}          # epc -> barcode(sku)，UI/场景可写
        self.mapping_lock = threading.Lock()
        self.rule = {"mode": "normal",     # normal | empty | error
                     "delay_ms": 0,
                     "http_code": 200}
        self._rule_lock = threading.Lock()
        self._last_q = None
        self._qcount = 0

    def set_mapping(self, m):
        with self.mapping_lock:
            self.mapping = dict(m)

    def add_mapping(self, epc, sku):
        with self.mapping_lock:
            self.mapping[epc] = sku

    def get_mapping(self):
        with self.mapping_lock:
            return dict(self.mapping)

    def set_rule(self, **kw):
        with self._rule_lock:
            self.rule.update(kw)

    def get_rule(self):
        with self._rule_lock:
            return dict(self.rule)

    def stats(self):
        return {"qcount": self._qcount, "last_q": self._last_q}

    def handle_request(self, h):
        body = _read_body(h)
        raw_url = h.path
        m = re.match(r"^([^?]*)", raw_url)
        path = m.group(1) if m else raw_url
        body_text = body.decode("utf-8", errors="replace")
        with self._rule_lock:
            rule = dict(self.rule)
        auth = h.headers.get("Authorization", "")
        cfg_auth = self.cfg.rfid_appkey
        auth_note = ("Authorization 一致" if auth and (not cfg_auth or auth == cfg_auth)
                     else ("⚠Authorization 缺失" if not auth else "⚠Authorization 不一致"))
        self._qcount += 1
        self._last_q = body_text
        parsed = ""
        try:
            parsed = proto.pretty_json(body_text)
        except Exception:
            pass
        try:
            req = json.loads(body_text) if body_text else {}
            epc_list = req.get("epcList", []) or []
        except Exception:
            epc_list = []
        self.store.add("RFID查询", "收",
                       f"POST {path} epcCount={len(epc_list)} | {auth_note}",
                       raw_text=f"{raw_url}\nAuthorization: {auth or '(无)'}\n\n{body_text}",
                       parsed=parsed, ref=",".join(epc_list[:5]))
        delay = max(int(rule.get("delay_ms") or 0), 0)
        if delay:
            time.sleep(delay / 1000.0)
        mode = rule.get("mode", "normal")
        http_code = 500 if mode == "error" else int(rule.get("http_code") or 200)
        with self.mapping_lock:
            mapping = dict(self.mapping)
        rows = []
        if mode == "normal":
            for epc in epc_list:
                if epc in mapping:
                    rows.append({"barcode": mapping[epc], "epc": epc, "tid": "",
                                 "uniqueCode": "", "productContainsMetal": False})
                else:
                    # 未映射 → 不回显（与 WCS 解析一致：不回显=该 EPC 不再被重查）
                    rows.append(None)
            rows = [r for r in rows if r]
        resp = {"data": {"data": rows}, "errorCode": "", "msg": "",
                "requestId": 0, "status": 200, "success": True}
        resp_body = proto.to_json_bytes(resp)
        self._bump("sent")
        try:
            h.send_response(http_code)
            h.send_header("Content-Type", "application/json; charset=UTF-8")
            h.send_header("Content-Length", str(len(resp_body)))
            h.end_headers()
            h.wfile.write(resp_body)
        except OSError:
            pass
        hit = len(rows)
        self.store.add("RFID查询", "发",
                       f"应答 {path} 命中{hit}/{len(epc_list)} mode={mode}",
                       raw_text=resp_body.decode("utf-8", errors="replace"),
                       parsed=proto.pretty_json(resp_body.decode("utf-8", errors="replace")))
