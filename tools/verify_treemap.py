"""Run existing regression checks and capture treemap evidence, restoring user settings."""

import argparse
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--dry-run", action="store_true")
    mode.add_argument("--apply", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = root / "work" / "treemap-validation"
    settings = Path(os.environ["APPDATA"]) / "DiskCleaner" / "DiskCleaner.ini"
    if args.dry_run:
        print("Run CTest and tools/smoke.ps1; capture light/dark/tree/files images.")
        print(f"Temporarily replace and finally restore: {settings}")
        print(f"Evidence output: {output}")
        return 0
    output.mkdir(parents=True, exist_ok=True)
    original = settings.read_bytes() if settings.exists() else None
    if original is not None:
        (output / "settings-before.ini").write_bytes(original)
    env = os.environ.copy()
    env["PATH"] = "C:/Qt/6.11.2/mingw_64/bin;" + env["PATH"]
    env["QT_FORCE_STDERR_LOGGING"] = "1"
    exe = root / "build" / "DiskCleaner.exe"

    def run(name, command, timeout=90):
        result = subprocess.run(command, cwd=root, env=env, shell=False,
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

    try:
        run("ctest", ["C:/Qt/Tools/CMake_64/bin/ctest.exe", "--test-dir", "build", "--output-on-failure"])
        (output / "treemap-tests.log").write_bytes(
            (root / "build" / "Testing" / "Temporary" / "LastTest.log").read_bytes())
        run("smoke", ["pwsh", "-NoProfile", "-File", str(root / "tools" / "smoke.ps1")])
        for name, theme, tab, classes in [("light", "light", "0", "false"),
                                          ("dark-classes", "dark", "0", "true"),
                                          ("files", "light", "1", "false")]:
            settings.write_text("[General]\nfilterscanresults=false\ntreemapDetail=3\n"
                                f"treemapClasses={classes}\n", encoding="utf-8")
            command = [str(exe), "--scan", str(root / "work" / "sample"), "--lang", "zh_CN",
                       "--theme", theme, "--tab", tab, "--shot", str(output / f"{name}.png"),
                       "--delay", "2200"]
            if name == "light":
                command += ["--treemap-png", str(output / "treemap.png"),
                            "--export", str(output / "scan.csv")]
            run(name, command, 30)
            if not (output / f"{name}.png").is_file():
                raise RuntimeError(f"Missing screenshot: {name}")
        print(f"Evidence: {output}")
    finally:
        if original is not None:
            settings.write_bytes(original)
        elif settings.exists():
            settings.unlink()
        print("Restored original user settings.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
