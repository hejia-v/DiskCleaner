"""Exercise deletion notifications and F5 in the deployed GPUI window on an isolated fixture."""
import argparse
import ctypes
from ctypes import wintypes
from pathlib import Path
import subprocess
import tempfile
import time

from PIL import Image

user32 = ctypes.WinDLL("user32", use_last_error=True)
enum_callback = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
user32.EnumWindows.argtypes = [enum_callback, wintypes.LPARAM]
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user32.IsWindowVisible.argtypes = [wintypes.HWND]
user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
user32.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]


def window_for(pid):
    matches = []

    @enum_callback
    def inspect(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            matches.append(hwnd)
        return True

    user32.EnumWindows(inspect, 0)
    return matches[0] if matches else None


def wait_for(probe, description, seconds=10):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        value = probe()
        if value:
            return value
        time.sleep(0.12)
    raise RuntimeError(f"Timed out waiting for {description}")


def capture(hwnd):
    """Request the composited client area, including the DirectX surface."""
    import struct
    gdi = ctypes.WinDLL("gdi32")
    handle = wintypes.HANDLE
    gdi.CreateCompatibleDC.argtypes = [handle]
    gdi.CreateCompatibleDC.restype = handle
    gdi.CreateDIBSection.argtypes = [handle, ctypes.c_void_p, wintypes.UINT,
                                     ctypes.POINTER(ctypes.c_void_p), handle, wintypes.DWORD]
    gdi.CreateDIBSection.restype = handle
    gdi.SelectObject.argtypes = [handle, handle]
    gdi.SelectObject.restype = handle
    gdi.DeleteObject.argtypes = gdi.DeleteDC.argtypes = [handle]
    user32.GetClientRect.argtypes = [handle, ctypes.POINTER(wintypes.RECT)]
    user32.PrintWindow.argtypes = [handle, handle, wintypes.UINT]
    rect = wintypes.RECT()
    if not user32.GetClientRect(hwnd, ctypes.byref(rect)):
        raise ctypes.WinError()
    width, height = rect.right, rect.bottom
    header = struct.pack("<IiiHHIIiiII", 40, width, -height, 1, 32, 0, 0, 0, 0, 0, 0)
    dc = gdi.CreateCompatibleDC(None)
    data = ctypes.c_void_p()
    bitmap = gdi.CreateDIBSection(dc, header, 0, ctypes.byref(data), None, 0)
    old = gdi.SelectObject(dc, bitmap)
    try:
        if not bitmap or not data.value or not user32.PrintWindow(hwnd, dc, 3):
            raise RuntimeError("Window capture failed")
        return Image.frombytes("RGB", (width, height), ctypes.string_at(data, width * height * 4),
                               "raw", "BGRX")
    finally:
        gdi.SelectObject(dc, old)
        gdi.DeleteObject(bitmap)
        gdi.DeleteDC(dc)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--dry-run", action="store_true")
    mode.add_argument("--apply", action="store_true")
    args = parser.parse_args()
    if args.dry_run:
        print("Create one temporary file, scan its directory, remove that file and send F5 to the test window.")
        return 0
    root = Path(__file__).resolve().parents[1]
    exe = root / "dist/Release/DiskCleaner.exe"
    with tempfile.TemporaryDirectory(prefix="diskcleaner-snapshot-") as tmp:
        folder = Path(tmp)
        victim = folder / "fixture.txt"
        victim.write_text("snapshot fixture", encoding="utf-8")
        process = subprocess.Popen([str(exe), "--scan", str(folder), "--geometry", "1080x680", "--theme", "light", "--lang", "en"],
                                   cwd=root, shell=False)
        try:
            hwnd = wait_for(lambda: window_for(process.pid), "the GPUI window")

            def colors():
                frame = capture(hwnd)
                return dict((rgb, count) for count, rgb in frame.getcolors(frame.width * frame.height))

            wait_for(lambda: colors().get((226, 235, 255), 0) > 100, "selected snapshot root")
            victim.unlink()
            removed = (252, 232, 228)
            wait_for(lambda: colors().get(removed, 0) > 100, "deleted row marking")
            user32.PostMessageW(hwnd, 0x0100, 0x74, 1)  # WM_KEYDOWN, VK_F5
            user32.PostMessageW(hwnd, 0x0101, 0x74, 0xC0000001)
            wait_for(lambda: colors().get(removed, 0) == 0, "refreshed snapshot")
            if not user32.SendMessageW(hwnd, 0x007F, 1, 0):
                raise RuntimeError("Missing large window icon")
            if not user32.SendMessageW(hwnd, 0x007F, 0, 0):
                raise RuntimeError("Missing small window icon")
            print("PASS deletion marking, F5 snapshot refresh and both window icons")
        finally:
            hwnd = window_for(process.pid)
            if hwnd:
                user32.PostMessageW(hwnd, 0x0010, 0, 0)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
