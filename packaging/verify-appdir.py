#!/usr/bin/env python3
"""Reject missing dependencies and accidental use of host application libraries."""
import os
from pathlib import Path
import re
import subprocess
import sys


def verify(appdir):
    appdir = appdir.resolve()
    env = os.environ.copy()
    env.pop("LD_PRELOAD", None)
    env["LD_LIBRARY_PATH"] = str(appdir / "usr/lib")
    env["LC_ALL"] = "C"
    failures = []
    count = 0
    for path in sorted((appdir / "usr").rglob("*")):
        if not path.is_file() or path.is_symlink():
            continue
        with path.open("rb") as stream:
            if stream.read(4) != b"\x7fELF":
                continue
        count += 1
        result = subprocess.run(["ldd", str(path)], env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if result.returncode:
            failures.append(f"{path.relative_to(appdir)}: ldd failed: {result.stdout.strip()}")
        for line in result.stdout.splitlines():
            if "=> not found" in line:
                failures.append(f"{path.relative_to(appdir)}: {line.strip()}")
            match = re.match(r"\s*((?:libQt6|libpipewire-0\.3\.so|libpulse|libwebrtc\.so|libavcodec\.so|libavutil\.so|libSM\.so|libICE\.so)\S*) => (\S+)", line)
            if match and not Path(match[2]).resolve().is_relative_to(appdir):
                failures.append(f"{path.relative_to(appdir)}: host application dependency: {line.strip()}")
    if not count:
        failures.append("No ELF files found in AppDir")
    if failures:
        raise SystemExit("AppDir dependency verification failed:\n" + "\n".join(failures))
    print(f"Verified {count} ELF files: dependencies resolve and application libraries are bundled.")


if __name__ == "__main__":
    verify(Path(sys.argv[1]))
