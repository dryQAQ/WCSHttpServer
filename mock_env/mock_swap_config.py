# -*- coding: utf-8 -*-
"""
mock_swap_config.py — 一键切换 WCS 正式/mock 配置文件
  python mock_swap_config.py --enable    备份正式配置并启用 mock 配置
  python mock_swap_config.py --restore   从备份还原正式配置
  python mock_swap_config.py --status    查看当前状态
策略：任何切换前都把当前 config/http_server.xml 备份到 config_backup/（时间戳），
从不删除任何用户文件；--restore 选择最近一份备份。
"""
import os
import re
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
RELEASE = os.path.abspath(os.path.join(HERE, "..", "release_WcsHttpServer"))
CFG_DIR = os.path.join(RELEASE, "config")
LIVE = os.path.join(CFG_DIR, "http_server.xml")
MOCK = os.path.join(HERE, "config_mock", "http_server.xml")
BAK_DIR = os.path.join(CFG_DIR, "config_backup")

# mock 配置的关键特征值（用于校验/提示）
MOCK_MARKS = {
    "rfidPushServerIp": "127.0.0.1",
    "plcS7Ip": "127.0.0.1",
    "rfidQueryUrl": "http://127.0.0.1:9100/open-api/rfid/query",
    "feedbackTestUrl": "http://127.0.0.1:8099/gwms5/service/openapi/product/skuClassificationTask/gwisSubProductClassifyOrder",
    "feedbackEndTestUrl": "http://127.0.0.1:8099/gwms5/service/openapi/productClasTask/gwisSubProductClassifyEndOrder",
}


def _read(p):
    try:
        with open(p, "r", encoding="utf-8") as f:
            return f.read()
    except Exception:
        return ""


def text_matches(text, key, val):
    m = re.search(r"<%s>\s*([^<]*?)\s*</%s>" % (key, key), text)
    return bool(m and m.group(1) == val)


def current_kind():
    if not os.path.isfile(LIVE):
        return "missing"
    t = _read(LIVE)
    if all(text_matches(t, k, v) for k, v in MOCK_MARKS.items()):
        return "mock"
    return "production"


def backup_live(tag):
    os.makedirs(BAK_DIR, exist_ok=True)
    ts = time.strftime("%Y%m%d_%H%M%S")
    dst = os.path.join(BAK_DIR, f"http_server.xml.{tag}.{ts}.bak")
    shutil.copy2(LIVE, dst)
    return dst


def enable(verbose=True):
    if not os.path.isfile(MOCK):
        return 1, f"找不到 mock 配置模板: {MOCK}"
    if current_kind() == "missing":
        os.makedirs(CFG_DIR, exist_ok=True)
    elif current_kind() == "mock":
        return 0, "当前已是 mock 配置（未重复覆盖）"
    bak = backup_live("正式备份-启用mock前")
    shutil.copy2(MOCK, LIVE)
    return 0, f"已启用 MOCK 配置。\n备份: {bak}\n当前: {LIVE}\n\n请重启 WCS_httpServer.exe 生效；正式配置用「还原正式配置」恢复。"


def restore(verbose=True):
    if not os.path.isdir(BAK_DIR):
        return 1, "没有找到任何备份（config_backup 不存在），不执行还原"
    baks = [f for f in os.listdir(BAK_DIR) if f.endswith(".bak")]
    if not baks:
        return 1, "没有找到任何备份文件，不执行还原"
    baks.sort()
    chosen = os.path.join(BAK_DIR, baks[-1])
    if current_kind() != "mock":
        return 0, "当前不是 mock 配置，无需还原（如确需换回更早备份请手工处理）"
    backup_live("mock备份-还原正式前")
    shutil.copy2(chosen, LIVE)
    return 0, f"已还原正式配置。\n来源备份: {chosen}\n当前: {LIVE}\n\n请重启 WCS_httpServer.exe 生效。"


def status():
    kind = current_kind()
    lines = [f"当前配置: {kind}"]
    lines.append(f"文件: {LIVE}")
    if os.path.isdir(BAK_DIR):
        baks = sorted(f for f in os.listdir(BAK_DIR) if f.endswith(".bak"))
        lines.append(f"备份数: {len(baks)}")
        if baks:
            lines.append("最近: " + baks[-1])
    return "\n".join(lines)


if __name__ == "__main__":
    act = sys.argv[1] if len(sys.argv) > 1 else "--status"
    if act == "--enable":
        code, msg = enable()
    elif act == "--restore":
        code, msg = restore()
    else:
        code, msg = 0, status()
    print(msg)
    sys.exit(code)
