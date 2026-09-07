# -*- coding: utf-8 -*-
"""
mock_logstore.py — 全通道报文留存（线程安全）
任何通道（RFID/PLC/WMS下发/WMS网关/RFID查询/S7/系统事件）的收发都经 add() 记录，
GUI 报文总览与导出共用；支持过滤、增量文件归档。
"""
import os
import threading
import time
from datetime import datetime

CHANNELS = ("RFID", "PLC", "WMS下发", "WMS网关", "RFID查询", "S7", "系统")


class MessageStore:
    def __init__(self, max_keep=50000, log_dir=None):
        self._lock = threading.RLock()
        self._rows = []          # list[dict]
        self._seq = 0
        self.max_keep = max_keep
        self._file = None
        self._file_lock = threading.Lock()
        if log_dir:
            os.makedirs(log_dir, exist_ok=True)
            self._file = open(os.path.join(log_dir, "报文留存.txt"), "a", encoding="utf-8")

    # ---- 写入 ----
    def add(self, channel, direction, summary, raw_text="", raw_hex="", parsed="", ref=None):
        """channel 见 CHANNELS；direction: '→收'/'←发' 语义统一为 本仿真台视角:
           发=仿真台发送, 收=仿真台收到。ref 可选关联 id（如 msgId/EPC）"""
        with self._lock:
            self._seq += 1
            row = {
                "seq": self._seq,
                "time": time.time(),
                "time_str": datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3],
                "channel": channel,
                "dir": direction,        # "发"/"收"/"事件"
                "summary": summary,
                "raw_text": raw_text,
                "raw_hex": raw_hex,
                "parsed": parsed,
                "ref": ref or "",
            }
            self._rows.append(row)
            if len(self._rows) > self.max_keep:
                del self._rows[: len(self._rows) - self.max_keep]
        if self._file:
            with self._file_lock:
                try:
                    self._file.write(self._fmt_line(row) + "\n")
                    self._file.flush()
                except Exception:
                    pass
        return row

    @staticmethod
    def _fmt_line(r):
        hexs = (" [" + r["raw_hex"] + "]") if r["raw_hex"] else ""
        return f"[{r['time_str']}] [{r['channel']}] {r['dir']} {r['summary']}{hexs}\n    raw: {r['raw_text']}"

    def add_event(self, summary):
        return self.add("系统", "事件", summary)

    # ---- 读取 ----
    def snapshot(self, channel=None, direction=None, keyword=None, last_n=None):
        """返回按时间正序的副本；keyword 匹配 summary/raw_text/parsed/ref"""
        with self._lock:
            rows = list(self._rows)
        if channel and channel != "全部":
            rows = [r for r in rows if r["channel"] == channel]
        if direction and direction != "全部":
            rows = [r for r in rows if r["dir"] == direction]
        if keyword:
            k = keyword.lower()
            rows = [r for r in rows
                    if k in r["summary"].lower() or k in r["raw_text"].lower()
                    or k in r["parsed"].lower() or k in r["ref"].lower()]
        if last_n:
            rows = rows[-last_n:]
        return rows

    def count(self):
        with self._lock:
            return len(self._rows)

    def last_seq(self):
        with self._lock:
            return self._rows[-1]["seq"] if self._rows else 0

    def rows_since(self, since_seq):
        """增量读取：返回 (新行列表[seq>since_seq], 当前最早seq)。用于 GUI 定时增量刷新。"""
        with self._lock:
            n = len(self._rows)
            i = 0
            while i < n and self._rows[i]["seq"] <= since_seq:
                i += 1
            return self._rows[i:], (self._rows[0]["seq"] if n else 0)

    def clear(self):
        with self._lock:
            self._rows.clear()
            self._seq = 0

    def close(self):
        with self._file_lock:
            if self._file:
                self._file.close()
                self._file = None
