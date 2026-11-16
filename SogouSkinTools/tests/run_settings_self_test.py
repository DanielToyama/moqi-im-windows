#!/usr/bin/env python3
"""Headless dropdown/self-test driver for SogouSkinSettings.

Creates an isolated %APPDATA%/%USERPROFILE% containing three .ssf variants
(same file name in two folders, to exercise the disambiguation label), a
recent-skin file and a Downloads copy, then runs:

    SogouSkinSettings.exe --self-test <artifacts dir>

The settings UI itself drives CB_SETCURSEL + CBN_SELCHANGE, records preview
dimensions/hashes and writes PNG screenshots into the artifacts directory.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import time
import zipfile
from pathlib import Path


def make_skin(source: Path, destination: Path, font_size: int) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(source, "r") as src, zipfile.ZipFile(
        destination, "w", zipfile.ZIP_DEFLATED
    ) as dst:
        for info in src.infolist():
            data = src.read(info.filename)
            if info.filename.lower() == "skin.ini":
                if data.startswith(b"\xff\xfe"):
                    text = data[2:].decode("utf-16-le")
                    text = text.replace("font_size=15", f"font_size={font_size}")
                    data = b"\xff\xfe" + text.encode("utf-16-le")
                else:
                    text = data.decode("utf-8", errors="replace")
                    text = text.replace("font_size=15", f"font_size={font_size}")
                    data = text.encode("utf-8")
            dst.writestr(info, data)


def main() -> int:
    repo = Path(__file__).resolve().parents[2]
    exe_override = None
    if len(sys.argv) >= 3 and sys.argv[1] == "--exe":
        exe_override = Path(sys.argv[2])
    artifacts = repo / "artifacts" / "settings_dropdown_selftest"
    if artifacts.exists():
        shutil.rmtree(artifacts)
    artifacts.mkdir(parents=True)

    sample = Path(r"E:\SogouSkin\samples\win7_repacked.ssf")
    if not sample.is_file():
        print(f"missing sample: {sample}", file=sys.stderr)
        return 2

    work = artifacts / "isolated_profile"
    appdata = work / "appdata"
    profile = work / "profile"
    localappdata = work / "localappdata"
    program_files = work / "program_files_x86"
    for path in (appdata / "Moqi" / "skins", profile / "Downloads",
                 localappdata, program_files):
        path.mkdir(parents=True, exist_ok=True)

    installed = appdata / "Moqi" / "sogou_skin.ssf"
    config_skin = appdata / "Moqi" / "skins" / "win7.ssf"
    downloads_skin = profile / "Downloads" / "win7.ssf"
    make_skin(sample, installed, 15)
    make_skin(sample, config_skin, 20)
    make_skin(sample, downloads_skin, 26)

    recent_text = "\r\n".join(
        [str(downloads_skin), str(work / "missing" / "gone.ssf"), str(config_skin)]
    ) + "\r\n"
    (appdata / "Moqi" / "sogou_skin_recent.txt").write_bytes(
        b"\xff\xfe" + recent_text.encode("utf-16-le")
    )
    # Options owned by another screen must survive a settings apply.
    (appdata / "Moqi" / "sogou_skin_options.ini").write_text(
        "[Options]\nstatusbar=1\nhorizontal=0\nkey_repeat=0\nstatusbar_hide=\n",
        encoding="ascii",
    )

    exe = exe_override
    if exe is None:
        exe = repo / "build-vs64-local" / "SogouSkinTools" / "Release" / "SogouSkinSettings.exe"
        if not exe.is_file():
            exe = repo / "build-vs32-local" / "SogouSkinTools" / "Release" / "SogouSkinSettings.exe"
    if not exe.is_file():
        print("SogouSkinSettings.exe not built", file=sys.stderr)
        return 2

    env = os.environ.copy()
    env["APPDATA"] = str(appdata)
    env["USERPROFILE"] = str(profile)
    env["LOCALAPPDATA"] = str(localappdata)
    # Python exposes these variables with upper-case names; overwrite the
    # canonical key so the child process sees the isolated values.
    env.pop("ProgramFiles(x86)", None)
    env.pop("PROGRAMFILES(X86)", None)
    env["PROGRAMFILES(X86)"] = str(program_files)
    env.pop("ProgramFiles", None)
    env.pop("PROGRAMFILES", None)
    env["PROGRAMFILES"] = str(program_files)
    env.pop("MOQI_SOGOU_SKIN", None)
    env.pop("MOQI_SOGOU_STATUSBAR", None)

    started = time.time()
    result = subprocess.run(
        [str(exe), "--self-test", str(artifacts)],
        cwd=str(repo),
        env=env,
        timeout=90,
    )
    elapsed = time.time() - started

    log_path = artifacts / "self-test.log"
    log = ""
    if log_path.is_file():
        raw = log_path.read_bytes()
        log = raw[2:].decode("utf-16-le") if raw.startswith(b"\xff\xfe") else raw.decode("utf-8", errors="replace")
    print(log)
    options_path = appdata / "Moqi" / "sogou_skin_options.ini"
    options_text = options_path.read_text(encoding="ascii", errors="replace") if options_path.is_file() else ""
    key_repeat_preserved = "key_repeat=0" in options_text
    print(f"key_repeat_preserved={'PASS' if key_repeat_preserved else 'FAIL'}")
    print(f"exit_code={result.returncode} elapsed={elapsed:.1f}s")
    expected = [
        "combo_count=3",
        "distinct_previews=3",
        "capacity_ok=1",
        "recent_apply_ok=1",
        "result=pass",
        "item=0 label=win7.ssf  (profile\\Downloads)",
    ]
    ok = (
        result.returncode == 0
        and all(item in log for item in expected)
        and "gone.ssf" not in log
        and key_repeat_preserved
    )
    print("settings_self_test=PASS" if ok else "settings_self_test=FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
