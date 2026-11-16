#!/usr/bin/env python3
"""Isolated font-settings test using the 达妮娅 reference skin.

Creates a throw-away %APPDATA%/%USERPROFILE%/%LOCALAPPDATA% tree with the
reference skin installed as the current skin plus a second copy at a
different font size (so the existing distinct-preview assertions still run).
It then runs:

    SogouSkinSettings.exe --self-test <artifacts>/font_settings_test

The UI self-test loads 达妮娅, switches the Chinese face to 微软雅黑 and the
English face to Arial, captures before/after previews, applies, and verifies
that sogou_skin_overrides.ini is UTF-16LE and contains the exact font names.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import time
import zipfile
from pathlib import Path


REFERENCE_SKIN = Path(r"C:\Users\Daniel\AppData\Roaming\Moqi\sogou_skin.ssf")


def make_variant(
    source: Path,
    destination: Path,
    font_size: int | None,
    font_ch: str | None = None,
    font_en: str | None = None,
) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(source, "r") as src, zipfile.ZipFile(
        destination, "w", zipfile.ZIP_DEFLATED
    ) as dst:
        for info in src.infolist():
            data = src.read(info.filename)
            if info.filename.lower() == "skin.ini" and (
                font_size is not None or font_ch is not None or font_en is not None
            ):
                if data.startswith(b"\xff\xfe"):
                    text = data[2:].decode("utf-16-le")
                    if font_size is not None:
                        text = text.replace("font_size=24", f"font_size={font_size}")
                    if font_ch is not None:
                        text = text.replace("font_ch=荆南麦圆体", f"font_ch={font_ch}")
                    if font_en is not None:
                        text = text.replace("font_en=荆南麦圆体", f"font_en={font_en}")
                    data = b"\xff\xfe" + text.encode("utf-16-le")
                else:
                    text = data.decode("utf-8-sig", errors="replace")
                    if font_size is not None:
                        text = text.replace("font_size=24", f"font_size={font_size}")
                    if font_ch is not None:
                        text = text.replace("font_ch=荆南麦圆体", f"font_ch={font_ch}")
                    if font_en is not None:
                        text = text.replace("font_en=荆南麦圆体", f"font_en={font_en}")
                    data = text.encode("utf-8")
            dst.writestr(info, data)


def read_utf16(path: Path) -> str:
    raw = path.read_bytes()
    if raw.startswith(b"\xff\xfe"):
        return raw[2:].decode("utf-16-le")
    return raw.decode("utf-8", errors="replace")


def main() -> int:
    repo = Path(__file__).resolve().parents[2]
    exe_override = None
    if len(sys.argv) >= 3 and sys.argv[1] == "--exe":
        exe_override = Path(sys.argv[2])

    artifacts = repo / "artifacts" / "font_settings_test"
    if artifacts.exists():
        shutil.rmtree(artifacts)
    artifacts.mkdir(parents=True)

    if not REFERENCE_SKIN.is_file():
        print(f"missing reference skin: {REFERENCE_SKIN}", file=sys.stderr)
        return 2

    work = artifacts / "isolated_profile"
    appdata = work / "appdata"
    profile = work / "profile"
    localappdata = work / "localappdata"
    program_files = work / "program_files_x86"
    for path in (
        appdata / "Moqi" / "skins",
        profile / "Downloads",
        localappdata,
        program_files,
    ):
        path.mkdir(parents=True, exist_ok=True)

    installed = appdata / "Moqi" / "sogou_skin.ssf"
    second = appdata / "Moqi" / "skins" / "dania_20.ssf"
    missing_font = appdata / "Moqi" / "skins" / "missing_font_20.ssf"
    make_variant(REFERENCE_SKIN, installed, None)
    make_variant(REFERENCE_SKIN, second, 20)
    # Not installed anywhere: the font combos must still add and select this
    # exact name so an applied override never silently changes it.
    make_variant(
        REFERENCE_SKIN,
        missing_font,
        20,
        font_ch="不存在的测试字体 A",
        font_en="不存在的测试字体 A",
    )

    recent_text = (
        str(installed) + "\r\n" + str(second) + "\r\n" + str(missing_font) + "\r\n"
    )
    (appdata / "Moqi" / "sogou_skin_recent.txt").write_bytes(
        b"\xff\xfe" + recent_text.encode("utf-16-le")
    )
    (appdata / "Moqi" / "sogou_skin_options.ini").write_text(
        "[Options]\nstatusbar=1\nhorizontal=0\nkey_repeat=1\nstatusbar_hide=\n",
        encoding="ascii",
    )
    # A hand-edited section that the settings UI does not own must survive.
    manual_override = "[CustomManual]\nkeep_me=1\n"
    (appdata / "Moqi" / "sogou_skin_overrides.ini").write_bytes(
        b"\xff\xfe" + manual_override.encode("utf-16-le")
    )

    exe = exe_override
    if exe is None:
        exe = (
            repo
            / "build-vs64-local"
            / "SogouSkinTools"
            / "Release"
            / "SogouSkinSettings.exe"
        )
        if not exe.is_file():
            exe = (
                repo
                / "build-vs32-local"
                / "SogouSkinTools"
                / "Release"
                / "SogouSkinSettings.exe"
            )
    if not exe.is_file():
        print("SogouSkinSettings.exe not built", file=sys.stderr)
        return 2

    env = os.environ.copy()
    env["APPDATA"] = str(appdata)
    env["USERPROFILE"] = str(profile)
    env["LOCALAPPDATA"] = str(localappdata)
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
        timeout=120,
    )
    elapsed = time.time() - started

    log_path = artifacts / "self-test.log"
    log = read_utf16(log_path) if log_path.is_file() else ""
    print(log)

    overrides_path = appdata / "Moqi" / "sogou_skin_overrides.ini"
    overrides = read_utf16(overrides_path) if overrides_path.is_file() else ""

    expected = [
        "combo_count=3",
        "distinct_previews=3",
        "font_ch_before=荆南麦圆体",
        "font_en_before=荆南麦圆体",
        "font_selected_ch=微软雅黑",
        "font_selected_en=Arial",
        "font_preview_changed=1",
        "font_apply_ok=1",
        "font_reload_ok=1",
        "font_dropdown_ok=1",
        "font_layout_overlap_ok=1",
        "font_ch=不存在的测试字体 A",
        "result=pass",
    ]
    checks = {item: item in log for item in expected}
    checks["overrides_font_ch"] = "font_ch=微软雅黑" in overrides
    checks["overrides_font_en"] = "font_en=Arial" in overrides
    checks["overrides_font_size_24"] = "font_size=24" in overrides
    checks["overrides_utf16le"] = overrides_path.is_file() and overrides_path.read_bytes().startswith(b"\xff\xfe")
    checks["manual_field_preserved"] = "keep_me=1" in overrides

    for name, ok in checks.items():
        print(f"{name}={'PASS' if ok else 'FAIL'}")
    print(f"exit_code={result.returncode} elapsed={elapsed:.1f}s")
    print(f"artifacts={artifacts}")

    ok = result.returncode == 0 and all(checks.values())
    print("font_settings_test=PASS" if ok else "font_settings_test=FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
