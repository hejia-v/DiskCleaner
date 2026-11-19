"""Run Rust regressions and capture GPUI treemap evidence."""

import argparse

from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--dry-run", action="store_true")
    mode.add_argument("--apply", action="store_true")
    parser.add_argument("--output", type=Path, default=Path("work/treemap-validation"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = root / args.output
    if args.dry_run:
        print("Run Rust CTest and CLI smoke tests, then render the sample treemap and CSV.")
        print(f"Evidence directory: {output}")
        return 0
    output.mkdir(parents=True, exist_ok=True)
    exe = root / "dist/DiskCleaner.exe"

    def run(name, command, timeout=90):
        result = subprocess.run(command, cwd=root, shell=False,
                                capture_output=True, text=True, encoding="utf-8",
                                errors="replace", timeout=timeout)
        text = result.stdout + result.stderr
        (output / f"{name}.log").write_text(text, encoding="utf-8")
        print(f"{name}: exit={result.returncode}")
        for line in text.splitlines():
            if "PASS" in line or "FAIL" in line or "passed" in line or "RESULT" in line:
                print("  " + line)
        if result.returncode:
            raise RuntimeError(f"{name} failed; see {output / (name + '.log')}")

    run("ctest", ["ctest", "--test-dir", "build", "-C", "Release", "--output-on-failure"])
    run("smoke", ["pwsh", "-NoProfile", "-File", str(root / "tools/smoke.ps1")])
    for name, theme, tab in [("light", "light", "0"), ("dark", "dark", "0"), ("files", "light", "1")]:
        command = [str(exe), "--scan", str(root / "work/sample"), "--lang", "zh_CN",
                   "--theme", theme, "--tab", tab, "--shot", str(output / f"{name}.png"),
                   "--delay", "600"]
        if name == "light":
            command += ["--treemap-png", str(output / "treemap.png"), "--export", str(output / "scan.csv")]
        run(name, command, 30)
        if not (output / f"{name}.png").is_file():
            raise RuntimeError(f"Missing screenshot: {name}")
    print(f"Evidence directory: {output}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
