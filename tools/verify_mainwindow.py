"""Capture and verify the native GPUI client area at the current Windows display scale."""

import argparse
from pathlib import Path
import subprocess

from PIL import Image, ImageDraw


def compare_images(reference: Path, actual: Path, output: Path) -> None:
    source = Image.open(reference).convert("RGB")
    implementation = Image.open(actual).convert("RGB")
    if source.size != implementation.size:
        implementation = implementation.resize(source.size, Image.Resampling.LANCZOS)
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
        draw.text((left.width + 24, 7), "GPUI IMPLEMENTATION", fill="#18263b")
        canvas.paste(left, (0, 28))
        canvas.paste(right, (left.width + 16, 28))
        canvas.save(output / f"comparison-{name}.png")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--dry-run", action="store_true")
    mode.add_argument("--apply", action="store_true")
    parser.add_argument("--output", type=Path, help="Evidence directory (relative to project or absolute)")
    parser.add_argument("--reference", type=Path, help="Compare against a supplied client-area screenshot")
    parser.add_argument("--skip-comparison", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--extended", action="store_true", help="Also capture English and a compact file list at the current display scale")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = root / args.output if args.output else root / "work/ui-redesign"
    shots = [
        ("light", "light", "1486x1024", 0, "sample", "zh_CN"),
        ("files", "light", "1486x1024", 1, "dups", "zh_CN"),
        ("compact", "light", "1080x680", 0, "sample", "zh_CN"),
        ("dark", "dark", "1280x800", 0, "sample", "zh_CN"),
        ("empty", "light", "1080x680", 0, None, "zh_CN"),
    ]
    if args.extended:
        shots += [
            ("english", "light", "1080x680", 1, "sample", "en"),
            ("files-compact", "dark", "800x600", 1, "dups", "zh_CN"),
        ]
    if args.dry_run:
        print("Capture " + ", ".join(s[0] for s in shots))
        print(f"Evidence directory: {output}")
        return 0
    output.mkdir(parents=True, exist_ok=True)
    for name, theme, geometry, tab, fixture, lang in shots:
        target = output / f"{name}.png"
        command = [str(root / "dist/DiskCleaner.exe"), "--geometry", geometry,
                   "--theme", theme, "--lang", lang, "--tab", str(tab),
                   "--shot", str(target), "--delay", "600"]
        if fixture:
            command.extend(["--scan", str(root / "work" / fixture)])
        if fixture == "dups":
            command.extend(["--dup", "1", "--dup-only"])
        result = subprocess.run(command, cwd=root, shell=False, capture_output=True,
                                text=True, encoding="utf-8", errors="replace", timeout=30)
        (output / f"{name}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        result.check_returncode()
        with Image.open(target) as shot:
            if shot.convert("RGB").getcolors(8) is not None:
                raise ValueError(f"Window screenshot has too few colors: {name}")
            print(f"{name}: {shot.size}")

    if args.reference and not args.skip_comparison:
        compare_images(args.reference, output / "light.png", output)

    print(f"Evidence: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
