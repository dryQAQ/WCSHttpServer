# -*- coding: utf-8 -*-
"""
dev_ui.py — 开发期：对 WCS_httpServer.exe 窗口做 置前/截图(纯Python写PNG)/鼠标点击
用法:
  python dev_ui.py prime <pid> <x> <y>     置前+还原+移动到 (x,y)
  python dev_ui.py shot  <pid> <png路径>    截图窗口（PrintWindow，黑屏则回退屏幕拷贝）
  python dev_ui.py rect <pid>               打印窗口矩形
  python dev_ui.py click <x> <y>            在屏幕物理坐标单击
"""
import ctypes
import ctypes.wintypes as wt
import struct
import sys
import time
import zlib

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32
kernel32 = ctypes.windll.kernel32

# ── DPI 感知（物理像素坐标） ──
try:
    user32.SetProcessDPIAware()
except Exception:
    pass

SM_CMONITORS = 80
INPUT_MOUSE = 0
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
SW_RESTORE = 9
HWND_TOP = 0


def find_hwnd(pid):
    out = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, lparam):
        p = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(hwnd):
            out.append(hwnd)
        return True

    user32.EnumWindows(cb, 0)
    return out[0] if out else None


def rect_of(hwnd):
    r = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    return r.left, r.top, r.right, r.bottom


def prime(pid, x, y):
    hwnd = find_hwnd(pid)
    if not hwnd:
        print("NO_WINDOW")
        return 1
    user32.ShowWindow(hwnd, SW_RESTORE)
    user32.SetWindowPos(hwnd, HWND_TOP, int(x), int(y), 0, 0,
                        0x0001 | 0x0002 | 0x0040)   # SWP_NOSIZE|SHOWWINDOW|NOACTIVATE
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.6)
    print("OK", hwnd)
    return 0


def click(x, y):
    user32.SetCursorPos(int(x), int(y))
    time.sleep(0.15)
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.08)
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    time.sleep(0.3)
    print("CLICKED", x, y)


def _png_chunk(tag, data):
    c = struct.pack(">I", len(data)) + tag + data
    c += struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    return c


def write_png(path, w, h, rgb_rows):
    raw = b""
    for row in rgb_rows:
        raw += b"\x00" + row
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(_png_chunk(b"IHDR", ihdr))
        f.write(_png_chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(_png_chunk(b"IEND", b""))


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG),
                ("biPlanes", wt.WORD), ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", wt.LONG),
                ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD),
                ("biClrImportant", wt.DWORD)]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", wt.DWORD * 3)]


def capture_window(pid, path):
    hwnd = find_hwnd(pid)
    if not hwnd:
        print("NO_WINDOW")
        return 1
    l, t, r, b = rect_of(hwnd)
    w, h = r - l, b - t
    if w <= 0 or h <= 0:
        print("BAD_RECT", l, t, r, b)
        return 1

    def from_dib(hdc, bmp):
        bmi = BITMAPINFO()
        bmi.bmiHeader.biSize = 40
        bmi.bmiHeader.biWidth = w
        bmi.bmiHeader.biHeight = -h
        bmi.bmiHeader.biPlanes = 1
        bmi.bmiHeader.biBitCount = 32
        bmi.bmiHeader.biCompression = 0
        buf = ctypes.create_string_buffer(w * h * 4)
        gdi32.GetDIBits(hdc, bmp, 0, h, buf, ctypes.byref(bmi), 0)
        rows = []
        stride = w * 4
        for y in range(h):
            row = buf.raw[y * stride:(y + 1) * stride]
            rgb = b""
            for x in range(w):
                b_, g_, r_ = row[x * 4], row[x * 4 + 1], row[x * 4 + 2]
                rgb += bytes((r_, g_, b_))
            rows.append(rgb)
        return rows

    ok = False
    rows = None
    # 方式1: PrintWindow(PW_RENDERFULLCONTENT=2)
    hwnd_dc = user32.GetWindowDC(hwnd)
    mem_dc = gdi32.CreateCompatibleDC(hwnd_dc)
    bmp = gdi32.CreateCompatibleBitmap(hwnd_dc, w, h)
    old = gdi32.SelectObject(mem_dc, bmp)
    try:
        rc = user32.PrintWindow(hwnd, mem_dc, 2)
        if rc:
            rows = from_dib(mem_dc, bmp)
            nonblack = sum(1 for r_ in rows[::max(1, h // 200)] for px in r_[::97]
                           if px not in (0, 255) or px == 0 and False)
            # 黑色检测：采样中 纯黑像素比例 < 60% 视为有效
            total = 0
            black = 0
            for rr in rows[::max(1, h // 200)]:
                for i in range(0, len(rr), 3 * 97):
                    total += 1
                    if rr[i] < 12 and rr[i + 1] < 12 and rr[i + 2] < 12:
                        black += 1
            ok = total and (black / total) < 0.6
    finally:
        gdi32.SelectObject(mem_dc, old)
        gdi32.DeleteObject(bmp)
        gdi32.DeleteDC(mem_dc)
        user32.ReleaseDC(hwnd, hwnd_dc)
    if not ok:
        # 方式2: 屏幕拷贝窗口区域（窗口需置前且未被遮挡）
        srcdc = user32.GetDC(0)
        mem_dc = gdi32.CreateCompatibleDC(srcdc)
        bmp = gdi32.CreateCompatibleBitmap(srcdc, w, h)
        old = gdi32.SelectObject(mem_dc, bmp)
        gdi32.BitBlt(mem_dc, 0, 0, w, h, srcdc, l, t, 0x00CC0020)
        rows = from_dib(mem_dc, bmp)
        gdi32.SelectObject(mem_dc, old)
        gdi32.DeleteObject(bmp)
        gdi32.DeleteDC(mem_dc)
        user32.ReleaseDC(0, srcdc)
    write_png(path, w, h, rows)
    print("SHOT", path, w, "x", h)
    return 0


def rect(pid):
    hwnd = find_hwnd(pid)
    if not hwnd:
        print("NO_WINDOW")
        return 1
    print("RECT", *rect_of(hwnd))
    return 0


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "prime":
        sys.exit(prime(int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])))
    if cmd == "shot":
        sys.exit(capture_window(int(sys.argv[2]), sys.argv[3]))
    if cmd == "rect":
        sys.exit(rect(int(sys.argv[2])))
    if cmd == "click":
        sys.exit(click(int(sys.argv[2]), int(sys.argv[3])))
    print("usage: dev_ui.py prime|shot|rect|click ...")
