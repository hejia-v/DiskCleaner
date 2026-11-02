"""Capture main-window design evidence and source diffs, restoring user settings."""

import argparse
import difflib
import os
from pathlib import Path
import subprocess

from PIL import Image, ImageDraw


def compare_images(reference: Path, actual: Path, output: Path) -> None:
    source = Image.open(reference).convert("RGB")
    # The design includes a Windows title bar; QWidget.grab captures client content.
    source = source.crop((1, 34, source.width, source.height))
    implementation = Image.open(actual).convert("RGB")
    if source.size != implementation.size:
        raise ValueError(f"Comparison requires equal client sizes: {source.size} != {implementation.size}")
    for name, box in {
        "full": (0, 0, source.width, source.height),
        "scan-summary": (0, 50, 744, 330),
        "directory": (0, 380, 744, 750),
        "treemap": (744, 380, source.width, 850),
    }.items():
        left, right = source.crop(box), implementation.crop(box)
        canvas = Image.new("RGB", (left.width * 2 + 16, left.height + 28), "#e8edf4")
        draw = ImageDraw.Draw(canvas)
        draw.text((8, 7), "REFERENCE", fill="#18263b")
        draw.text((left.width + 24, 7), "QT IMPLEMENTATION", fill="#18263b")
        canvas.paste(left, (0, 28))
        canvas.paste(right, (left.width + 16, 28))
        canvas.save(output / f"comparison-{name}.png")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--dry-run", action="store_true")
    mode.add_argument("--apply", action="store_true")
    parser.add_argument("--output", type=Path, help="Evidence directory (relative to project or absolute)")
    parser.add_argument("--skip-comparison", action="store_true", help="Skip the historical design comparison")
    parser.add_argument("--extended", action="store_true", help="Also capture 125/200 percent DPI, English and dense results")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = root / args.output if args.output else root / "work/ui-redesign"
    settings = Path(os.environ["APPDATA"]) / "DiskCleaner/DiskCleaner.ini"
    shots = [
        ("light", "light", "1486x1024", 0, True, "1"),
        ("files", "light", "1486x1024", 1, True, "1"),
        ("compact", "light", "1080x680", 0, True, "1"),
        ("dark", "dark", "1280x800", 0, True, "1"),
        ("empty", "light", "1080x680", 0, False, "1"),
        ("dpi150", "light", "1080x680", 0, True, "1.5"),
    ]
    if args.extended:
        shots += [
            ("dpi125", "light", "1080x680", 0, True, "1.25"),
            ("dpi200", "light", "1080x680", 0, True, "2"),
            ("english", "light", "1080x680", 1, True, "1"),
            ("project", "light", "1486x1024", 0, True, "1"),
            ("files-compact", "light", "1080x680", 1, True, "1"),
        ]
    if args.dry_run:
        print(f"Temporarily replace and finally restore: {settings}")
        print("Capture: " + ", ".join(name for name, *_ in shots))
        print(f"Evidence and source diffs: {output}")
        return 0
    output.mkdir(parents=True, exist_ok=True)
    original = settings.read_bytes() if settings.exists() else None
    env = os.environ.copy()
    env["PATH"] = "C:/Qt/6.11.2/mingw_64/bin;C:/Qt/Tools/mingw1310_64/bin;" + env["PATH"]
    env["QT_FORCE_STDERR_LOGGING"] = "1"
    try:
        settings.parent.mkdir(parents=True, exist_ok=True)
        for name, theme, geometry, tab, scan, scale in shots:
            settings.write_text("[General]\nfilterscanresults=false\ntreemapDetail=3\n"
                                "treemapClasses=false\ntreemapAllocated=true\n", encoding="utf-8")
            command = [str(root / "dist/Release/DiskCleaner.exe"), "-qwindowgeometry", geometry + "+0+0",
                       "--theme", theme, "--lang", "en" if name == "english" else "zh_CN", "--tab", str(tab),
                       "--shot", str(output / f"{name}.png"), "--delay", "2000"]
            if scan:
                command += ["--scan", str(root if name == "project" else root / "work/sample")]
            env["QT_SCALE_FACTOR"] = scale
            result = subprocess.run(command, cwd=root, env=env, shell=False, capture_output=True,
                                    text=True, encoding="utf-8", errors="replace", timeout=30)
            (output / f"{name}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
            if result.returncode or not (output / f"{name}.png").is_file():
                raise RuntimeError(f"{name} failed: exit {result.returncode}; see {output / (name + '.log')}")
            print(f"{name}: {Image.open(output / (name + '.png')).size}")
    finally:
        if original is not None:
            settings.write_bytes(original)
        elif settings.exists():
            settings.unlink()
        print("Restored user settings.")
    reference = root / "work/ux-audit-2026-11-02/main-light-v1.png"
    if reference.is_file() and not args.skip_comparison:
        compare_images(reference, output / "light.png", output)
    diffs = []
    for baseline in sorted((output / "baseline").glob("*")):
        current = root / ("tests" if baseline.name == "treemap_tests.cpp" else "src") / baseline.name
        before = baseline.read_text(encoding="utf-8").splitlines(keepends=True)
        after = current.read_text(encoding="utf-8").splitlines(keepends=True)
        if before != after:
            diffs.extend(difflib.unified_diff(before, after, fromfile=f"before/{baseline.name}",
                                             tofile=str(current.relative_to(root)), n=3))
    (output / "source.diff").write_text("".join(diffs), encoding="utf-8")
    print(f"Evidence: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
