# -*- coding: utf-8 -*-
"""
mock_proto.py — 帧编解码、样例报文模板、EPC↔SKU 示例数据
（线级格式依据 WCS_httpServer 源码与《鞋服窄带项目协议26.8.20.pdf》）
"""
import copy
import json
import re
from datetime import datetime

# ============================================================================
# 1) RFID 推送通道（TCP ASCII，{} 包裹，| 分隔）
# ============================================================================
HEARTBEAT_ASCII = "RFID{HEARTBEAT}0D"          # 字面 '0D' 两个字符，无回车（WCS 发送）
HEARTBEAT_BYTES = HEARTBEAT_ASCII.encode("ascii")

TAIL_CHOICES = [("字面0D(现场)", "LIT0D"), ("回车CR(0x0D)", "CR"),
                ("回车换行CRLF", "CRLF"), ("无帧尾", "NONE")]


def frame_tail_bytes(tail_kind):
    if tail_kind == "CR":
        return b"\r"
    if tail_kind == "CRLF":
        return b"\r\n"
    if tail_kind == "NONE":
        return b""
    return b"0D"          # 字面字符 '0' 'D'


def make_rfid_frame(seq="SN0027", dev="01", epc="A10126000900009285552527", tail_kind="LIT0D"):
    """组 RFID 帧: {流水号|设备码|EPC}+帧尾"""
    body = "{%s|%s|%s}" % (seq, dev, epc)
    return body.encode("ascii") + frame_tail_bytes(tail_kind)


def car_num_from_seq(seq):
    """SN0027 -> '27'（跳过非数字前缀→int→去前导零；无数字返回 ''）"""
    m = re.search(r"(\d+)", str(seq or ""))
    if not m:
        return ""
    v = int(m.group(1))
    return str(v) if v > 0 else ""


def pad3(n):
    try:
        return "%03d" % int(n)
    except Exception:
        return str(n)


def gen_seq_list(start=1, count=99):
    """SN0001~SN0099"""
    out = []
    for i in range(count):
        n = (start - 1 + i) % 99 + 1
        out.append("SN%04d" % n)
    return out


# ============================================================================
# 2) PLC TCP 通道
# ============================================================================
def make_plc_frame(fields):
    """fields: list[str] -> {a|b|c}（无帧尾，与 WCS 一致）"""
    return ("{" + "|".join(str(f) for f in fields) + "}").encode("ascii")


def make_plc_feedback(epc, grid, car, status="1"):
    """落格反馈 5 字段 {EPC|格口号|小车号|小车号+2|状态码}
    (源码口径: {epc|grid|firstCar|lastCar|status}, 第4字段=尾车, car=首车=下发小车号)"""
    car3 = pad3(car)
    try:
        last3 = pad3(int(car) + 2)
    except Exception:
        last3 = car3
    return make_plc_frame([epc, grid, car3, last3, status])


def parse_braced_frames(raw_text):
    """从原始文本中抽取全部 {..} 帧并按 | 拆分"""
    frames = []
    for m in re.finditer(r"\{([^{}]*)\}", raw_text):
        content = m.group(1).strip()
        if content:
            frames.append([p.strip() for p in content.split("|")])
    return frames


# ============================================================================
# 3) 样例模板
# ============================================================================
# 用户提供的任务下发原始样例（原样保留）
SAMPLE_WAVE_ITEMS = [
    {"inco": "106301209506204", "gridNum": "1", "gridNumber": 3, "gridType": "0"},
    {"inco": "1142150980902", "gridNum": "1", "gridNumber": 1, "gridType": "0"},
    {"inco": "115215009601804", "gridNum": "1", "gridNumber": 10, "gridType": "0"},
    {"inco": "1223103260403", "gridNum": "1", "gridNumber": 7, "gridType": "0"},
    {"inco": "1232122271904", "gridNum": "1", "gridNumber": 3, "gridType": "0"},
    {"inco": "1234010650903", "gridNum": "1", "gridNumber": 9, "gridType": "0"},
    {"inco": "1234010660406", "gridNum": "1", "gridNumber": 14, "gridType": "0"},
    {"inco": "1234016425701", "gridNum": "1", "gridNumber": 2, "gridType": "0"},
    {"inco": "1242122034601", "gridNum": "1", "gridNumber": 12, "gridType": "0"},
    {"inco": "12421220411503", "gridNum": "1", "gridNumber": 4, "gridType": "0"},
    {"inco": "1242125480303", "gridNum": "1", "gridNumber": 5, "gridType": "0"},
    {"inco": "125110022800302", "gridNum": "1", "gridNumber": 6, "gridType": "0"},
    {"inco": "125351024111903", "gridNum": "1", "gridNumber": 15, "gridType": "0"},
    {"inco": "1283102011403", "gridNum": "1", "gridNumber": 8, "gridType": "0"},
    {"inco": "13430717361814", "gridNum": "1", "gridNumber": 13, "gridType": "0"},
    {"inco": "16404442202200", "gridNum": "1", "gridNumber": 11, "gridType": "0"},
    {"inco": "1123103160402", "gridNum": "2", "gridNumber": 20, "gridType": "0"},
    {"inco": "116215084703302", "gridNum": "2", "gridNumber": 18, "gridType": "0"},
    {"inco": "1222200260438", "gridNum": "2", "gridNumber": 17, "gridType": "0"},
    {"inco": "12330602504803", "gridNum": "2", "gridNumber": 19, "gridType": "0"},
    {"inco": "1242141461802", "gridNum": "2", "gridNumber": 21, "gridType": "0"},
    {"inco": "126214031601802", "gridNum": "2", "gridNumber": 22, "gridType": "0"},
    {"inco": "1142150780907", "gridNum": "3", "gridNumber": 24, "gridType": "0"},
    {"inco": "1213100280903", "gridNum": "3", "gridNumber": 25, "gridType": "0"},
    {"inco": "12431539061803", "gridNum": "3", "gridNumber": 23, "gridType": "0"},
    {"inco": "106101097601802", "gridNum": "4", "gridNumber": 29, "gridType": "0"},
    {"inco": "1242151080903", "gridNum": "5", "gridNumber": 1, "gridType": "2"},
    {"inco": "1242151080906", "gridNum": "5", "gridNumber": 1, "gridType": "2"},
    {"inco": "166025072411500", "gridNum": "5", "gridNumber": 1, "gridType": "2"},
    {"inco": "1242151080903", "gridNum": "6", "gridNumber": 27, "gridType": "1"},
    {"inco": "1242151080906", "gridNum": "6", "gridNumber": 25, "gridType": "1"},
    {"inco": "166025072411500", "gridNum": "6", "gridNumber": 26, "gridType": "1"},
]


def sample_wave(order_code="PP202600000030"):
    qty = sum(int(it.get("gridNumber", 0)) for it in SAMPLE_WAVE_ITEMS)
    return {"orderCode": order_code, "orderQty": qty,
            "sobi": "H-01-AB", "items": copy.deepcopy(SAMPLE_WAVE_ITEMS)}


def sample_epc_map():
    """示例 EPC↔SKU：把样例 EPC 映射到波次中的第一个 SKU（可编辑）"""
    sku = SAMPLE_WAVE_ITEMS[0]["inco"]
    return {"A10126000900009285552527": sku}


def build_clean_wave(order_code="PP202600000099", grids=6, per_grid=2):
    """生成“干净”波次：单 SKU 单格口、可指定 格口数×每格口SKU数，
    orderQty 自动 = ΣgridNumber，附 EPC↔SKU 表。返回 (wave_dict, epc_map)"""
    items = []
    epc_map = {}
    n = 1
    for g in range(1, grids + 1):
        for k in range(per_grid):
            sku = "SK%013d" % (g * 1000 + k + 1)      # 13位 SKU 码
            items.append({"inco": sku, "gridNum": str(g),
                          "gridNumber": 1, "gridType": "0"})
            epc_map["A1012600090000928555%05d" % n] = sku
            n += 1
    wave = {"orderCode": order_code,
            "orderQty": sum(int(it["gridNumber"]) for it in items),
            "sobi": "H-01-AB", "items": items}
    return wave, epc_map


def build_count_wave(order_code, grids=3, skus_per_grid=2, qty=3, sobi="H-01-AB"):
    """按 格口数×每格SKU数×每SKU件数 生成波次与 EPC↔SKU 表。
    返回 (wave, epc_map, pieces): pieces=[(epc, sku, grid), ...] 按件展开、按格口分组顺序排列"""
    items = []
    epc_map = {}
    pieces = []
    n = 1
    for g in range(1, grids + 1):
        for s in range(1, skus_per_grid + 1):
            sku = "SK%013d" % (g * 1000 + s)
            grid_type = "1" if s == skus_per_grid and g == grids else "0"
            items.append({"inco": sku, "gridNum": str(g), "gridNumber": qty, "gridType": grid_type})
            for _ in range(qty):
                epc = "A1012600090000928555%05d" % n
                n += 1
                epc_map[epc] = sku
                pieces.append((epc, sku, str(g)))
    wave = {"orderCode": order_code,
            "orderQty": sum(int(it["gridNumber"]) for it in items),
            "sobi": sobi, "items": items}
    return wave, epc_map, pieces


def binding_payloads(box_prefix="H-01-2", start_hole=1, end_hole=6):
    """H6 容器绑定模板: boxcode 与格口号一一对应（示例风格 H-01-2001 ↔ 格口1）"""
    out = []
    for h in range(start_hole, end_hole + 1):
        out.append({"boxcode": "%s%03d" % (box_prefix, h), "latticehole": str(h)})
    return out


def cancel_payload(order_code="PP202600000030", reason="测试取消"):
    return {"orderCode": order_code, "cancelReason": reason}


def ts_now(ms=True):
    s = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    return s + (":000" if ms else "")


# WMS 网关应答样例（客户样例；ts 格式 yyyy-MM-dd HH:mm:ss:000）
def reply_fullbox_success():
    return {"success": True, "body": "产品分类框号库位完成分类--同步成功!", "ts": ts_now(True)}


def reply_end_success():
    return {"success": True, "body": "产品分类任务完结--同步成功!", "ts": ts_now(True)}


def pretty_json(obj_or_str):
    try:
        if isinstance(obj_or_str, str):
            obj_or_str = json.loads(obj_or_str)
        return json.dumps(obj_or_str, ensure_ascii=False, indent=2)
    except Exception:
        return str(obj_or_str)


def to_json_bytes(obj):
    return json.dumps(obj, ensure_ascii=False).encode("utf-8")


# ============================================================================
# 4) 帧解析展示辅助
# ============================================================================
def describe_rfid_frame(text):
    """返回解析描述文本；解析失败返回 None"""
    m = re.search(r"\{([^{}]*)\}", text or "")
    if not m:
        return None
    parts = [p.strip() for p in m.group(1).split("|")]
    if len(parts) == 3:
        seq, dev, epc = parts
        car = car_num_from_seq(seq)
        return f"流水号={seq} 设备码={dev} EPC={epc} → 小车号={car or '?'}(无效) | {epc}|{car}"
    if len(parts) == 2:
        seq, epc = parts
        car = car_num_from_seq(seq)
        return f"流水号={seq}(无设备码,兼容2字段) → 小车号={car or '?'}"
    return f"字段数={len(parts)}(非法帧)"


def describe_plc_frame(fields):
    """对 PLC 通道帧给出解析描述"""
    if len(fields) == 1:
        if fields[0].lower() == "start":
            return "PLC 批次开始信号 {start}"
        if fields[0].lower() == "stop":
            return "PLC 批次停止信号 {stop}"
        return f"单字段帧: {fields[0]}"
    if len(fields) == 3:
        return f"3字段反馈: EPC/条码={fields[0]} 格口={fields[1]} 小车号={fields[2]}"
    if len(fields) == 5:
        return (f"5字段反馈: EPC={fields[0]} 格口={fields[1]} 首车={fields[2]} "
                f"尾车={fields[3]} 状态码={fields[4]}(1=成功/2=无格口/3=信息不全)")
    return f"{len(fields)}字段(格式异常, WCS将告警)"
