"""Keep the five most recent completed DiskCleaner smoke runs."""
import argparse
import msvcrt
from pathlib import Path
import re


RESULTS = Path(__file__).resolve().parents[1] / "work" / "smoke"
ARTIFACTS = {
    "export.csv", "scan.png", "full.csv", "full.png", "synthetic.mft",
    "dups.png", "filter.png", "dark.png", "light.png",
}


def generated_files(run):
    """Return only known regular outputs. Leave customized directories alone."""
    files = []
    for entry in run.iterdir():
        if entry.resolve() != entry:
            return None
        if entry.name == "http" and entry.is_dir():
            for child in entry.iterdir():
                if child.name != "ver.txt" or child.resolve() != child or not child.is_file():
                    return None
                files.append(child)
        elif entry.name in ARTIFACTS and entry.is_file():
            files.append(entry)
        else:
            return None
    return files


def prune(results, apply):
    completed = []
    for run in results.iterdir():
        if not re.fullmatch(r"[0-9a-f]{32}", run.name) or run.resolve() != run or not run.is_dir():
            continue
        try:
            stamp = run.stat().st_mtime_ns
            files = generated_files(run)
            if files:
                completed.append((stamp, run, files))
        except FileNotFoundError:
            continue  # Another successful run finished its cleanup.
    expired = sorted(completed, reverse=True)[5:]
    for _, run, files in expired:
        print(f"{'Prune' if apply else 'Would prune'} {run}")
        if apply:
            # Delete only the inspected files. An unexpected new file prevents rmdir.
            for file in files:
                if file.resolve() != file:
                    raise ValueError(f"Unexpected artifact path: {file}")
                file.unlink(missing_ok=True)
            for directory in (run / "http", run):
                try:
                    directory.rmdir()
                except FileNotFoundError:
                    pass
    print(f"Expired smoke runs: {len(expired)}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--dry-run", action="store_true")
    mode.add_argument("--apply", action="store_true")
    apply = parser.parse_args().apply
    if RESULTS.resolve() != RESULTS:
        raise ValueError(f"Unexpected smoke results path: {RESULTS}")
    if RESULTS.exists() and apply:
        # Closing the file releases the Windows lock, including on process exit.
        with (RESULTS / ".prune.lock").open("a+b") as lock:
            lock.seek(0)
            msvcrt.locking(lock.fileno(), msvcrt.LK_LOCK, 1)
            prune(RESULTS, True)
    elif RESULTS.exists():
        prune(RESULTS, False)
