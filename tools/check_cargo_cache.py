"""Report NTFS file allocation under build/cargo; never modify the cache."""
from collections import Counter
import ctypes
from ctypes import wintypes
from pathlib import Path
import stat
import sys


CACHE = Path(__file__).resolve().parents[1] / "build" / "cargo"
GIB = 1024 ** 3
LIMIT = 3 * GIB
SHARED = "跨项硬链接（仅计一次）"


class FileStandardInfo(ctypes.Structure):
    _fields_ = [("allocation", ctypes.c_int64), ("eof", ctypes.c_int64),
                ("links", wintypes.DWORD), ("deleted", ctypes.c_ubyte),
                ("directory", ctypes.c_ubyte)]


class FileCompressionInfo(ctypes.Structure):
    _fields_ = [("compressed_size", ctypes.c_int64), ("format", wintypes.WORD),
                ("unit_shift", ctypes.c_ubyte), ("chunk_shift", ctypes.c_ubyte),
                ("cluster_shift", ctypes.c_ubyte), ("reserved", ctypes.c_ubyte * 3)]


kernel = ctypes.WinDLL("kernel32", use_last_error=True)
kernel.CreateFileW.argtypes = (wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                               wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE)
kernel.CreateFileW.restype = wintypes.HANDLE
kernel.GetFileInformationByHandleEx.argtypes = (wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD)
kernel.CloseHandle.argtypes = (wintypes.HANDLE,)


def allocated_bytes(path, attributes):
    # FILE_READ_ATTRIBUTES, share read/write/delete, OPEN_EXISTING.
    handle = kernel.CreateFileW(str(path), 0x80, 7, None, 3, 0, None)
    if handle == wintypes.HANDLE(-1).value:
        raise ctypes.WinError(ctypes.get_last_error(), f"无法打开 {path}")
    try:
        if attributes & (stat.FILE_ATTRIBUTE_COMPRESSED | stat.FILE_ATTRIBUTE_SPARSE_FILE):
            info = FileCompressionInfo()
            if not kernel.GetFileInformationByHandleEx(handle, 8, ctypes.byref(info), ctypes.sizeof(info)):
                raise ctypes.WinError(ctypes.get_last_error(), f"无法读取压缩或稀疏文件占用 {path}")
            return info.compressed_size
        info = FileStandardInfo()
        if not kernel.GetFileInformationByHandleEx(handle, 1, ctypes.byref(info), ctypes.sizeof(info)):
            raise ctypes.WinError(ctypes.get_last_error(), f"无法读取文件占用 {path}")
        return info.allocation
    finally:
        kernel.CloseHandle(handle)


def measure(root):
    sizes = Counter()
    links = {}
    skipped = 0
    pending = [root]
    while pending:
        for path in pending.pop().iterdir():
            meta = path.lstat()
            if meta.st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT:
                skipped += 1
                continue
            if stat.S_ISDIR(meta.st_mode):
                pending.append(path)
                continue
            parts = path.relative_to(root).parts[:-1]
            # Explicit Cargo targets add a target-triple directory before the profile.
            depth = 3 if len(parts) >= 2 and parts[1] in ("debug", "release") else 2
            group = "/".join(parts[:depth]) or "根目录文件"
            sizes.setdefault(group, 0)
            if meta.st_nlink > 1:
                identity = (meta.st_dev, meta.st_ino)
                if identity in links:
                    original, size = links[identity]
                    if original != group and original != SHARED:
                        sizes[original] -= size
                        sizes[SHARED] += size
                        links[identity] = (SHARED, size)
                    continue
            size = allocated_bytes(path, meta.st_file_attributes)
            sizes[group] += size
            if meta.st_nlink > 1:
                links[identity] = (group, size)
    return sizes, skipped


def report(sizes, skipped):
    total = sum(sizes.values())
    print(f"build/cargo 文件实际占用 {total / GIB:.2f} GiB（{total:,} 字节），提示阈值 3 GiB。")
    if skipped:
        print(f"已跳过 {skipped} 个重解析点，其目标未计入占用。")
    if total <= LIMIT:
        print("未超过阈值，继续复用现有缓存。")
        return
    print("超过阈值。各项实际占用（MiB）")
    for group, size in sizes.most_common():
        print(f"  {size / 1024**2:10.2f}  {group}")
    print("可清理范围（仅供预览）")
    if any(group == "debug" or group.startswith("debug/") for group in sizes):
        print("  已结束调试时，可预览 dev 配置：cargo clean --profile dev --target-dir build/cargo --dry-run")
    print("  仅本项目 Release 产物：cargo clean --release -p disk-cleaner --target-dir build/cargo --dry-run")
    print("  保留 Release 依赖、构建脚本和指纹。其他配置或目标仅在确认废弃后单独处理。")


def main():
    try:
        if CACHE.resolve() != CACHE:
            raise OSError(f"缓存根路径指向其他位置：{CACHE}")
        try:
            CACHE.stat()
        except FileNotFoundError:
            print("build/cargo 尚未创建，占用 0 字节。")
            return 0
        report(*measure(CACHE))
    except OSError as error:
        print(f"统计未完成，请在编译结束后重试：{error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    sys.exit(main())
