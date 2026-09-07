# -*- coding: utf-8 -*-
"""
dev_e2e_act.py — E2E 执行助手（在前台按阶段调用）
  bind        绑定格口1..N（H6，同步逐条）
  wave        下发干净波次（H4）并打印应答
  post <body> 通用 POST（body 为 JSON 文件或直接传 JSON 字符串）
  cmdfile <text>  向 e2e_cmd.txt 写入控制命令（供常驻服务执行 RFID推送/锁格）
  tail <n>    输出常驻服务统计文件/留存末尾
"""
import json
import os
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mock_proto as P

HERE = os.path.dirname(os.path.abspath(__file__))
CFG_PATH = os.path.join(HERE, "..", "release_WcsHttpServer", "config", "http_server.xml")
import mock_cfg
CFG = mock_cfg.WcsConfig(CFG_PATH).load()
BASE = f"http://127.0.0.1:{CFG.wms_listen_port}"
CMD = os.path.join(HERE, "logs", "e2e_cmd.txt")
STAT = os.path.join(HERE, "logs", "e2e_srv.txt")
STORE_F = os.path.join(HERE, "logs", "报文留存.txt")


def post(path, obj, tag=""):
    req = urllib.request.Request(BASE + path,
                                 data=json.dumps(obj, ensure_ascii=False).encode("utf-8"),
                                 headers={"Content-Type": "application/json"}, method="POST")
    t0 = time.time()
    try:
        r = urllib.request.urlopen(req, timeout=8)
        text = r.read().decode("utf-8", errors="replace")
        code = r.status
        r.close()
    except urllib.error.HTTPError as e:
        text = e.read().decode("utf-8", errors="replace")
        code = e.code
    ms = int((time.time() - t0) * 1000)
    print(f"[{tag or path}] HTTP {code} ({ms}ms) {text[:400]}")
    return code, text


def cmd(text):
    with open(CMD, "w", encoding="utf-8") as f:
        f.write(text)
    print("CMD:", text)


if __name__ == "__main__":
    act = sys.argv[1] if len(sys.argv) > 1 else ""
    if act == "bind":
        n = int(sys.argv[2]) if len(sys.argv) > 2 else 3
        for g in range(1, n + 1):
            post(CFG.api_binding, {"boxcode": "H-01-2%03d" % g, "latticehole": str(g)}, f"bind{g}")
            time.sleep(0.15)
    elif act == "wave":
        oc = sys.argv[2] if len(sys.argv) > 2 else "PP2026E2E001"
        w, m, pieces = P.build_count_wave(oc, grids=3, skus_per_grid=1, qty=1)
        post(CFG.api_insert_wave, w, "H4")
    elif act == "post":
        post(sys.argv[2], json.loads(sys.argv[3]), "post")
    elif act == "cmd":
        cmd(sys.argv[2])
    elif act == "stat":
        try:
            print(open(STAT, encoding="utf-8").read())
        except OSError as e:
            print("stat 不可读:", e)
    elif act == "tail":
        n = int(sys.argv[2]) if len(sys.argv) > 2 else 12
        try:
            lines = open(STORE_F, encoding="utf-8").read().splitlines()
            print("\n".join(lines[-n:]))
        except OSError as e:
            print("store 不可读:", e)
    elif act == "cmd_help":
        print("rfid <epc> [seq]\nlock <g>\nunlock <g>")
    else:
        print(__doc__)
