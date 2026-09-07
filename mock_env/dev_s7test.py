# -*- coding: utf-8 -*-
"""dev_s7test.py — 开发期：仅启 S7 模拟，观察 WCS 轮询；有读后置位格口3再复位"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mock_logstore
import mock_s7comm

STORE = mock_logstore.MessageStore(max_keep=30000, log_dir=os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "logs"))


def main():
    s7 = mock_s7comm.S7Server("127.0.0.1", 102, STORE)
    s7.start()
    STORE.add_event("dev_s7test: S7 模拟已启动")
    # 等待 WCS 开始轮询（最多20s）
    t0 = time.time()
    while time.time() - t0 < 20:
        if s7.snapshot()["counters"]["reads"] > 2:
            break
        time.sleep(0.5)
    for g in (3, 6, 10):
        STORE.add_event(f"dev_s7test: 置位 格口{g}")
        s7.set_grid(g, True)
        time.sleep(3.5)
        STORE.add_event(f"dev_s7test: 复位 格口{g}")
        s7.set_grid(g, False)
        time.sleep(3.5)
    time.sleep(5)
    snap = s7.snapshot()
    print("S7 reads:", snap["counters"]["reads"], "accepts:", snap["counters"]["accepts"])
    print("db77:", bytes(snap["db77"]).hex(" ").upper())
    s7.stop()
    STORE.close()


if __name__ == "__main__":
    main()
