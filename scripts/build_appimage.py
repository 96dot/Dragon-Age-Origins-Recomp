"""Builds the Linux AppImage around a built DAORecomp.exe (what Windows players download):
    python3 scripts/build_appimage.py dist/DAORecomp.exe dist/DAORecomp-x86_64.AppImage
It holds the exe, umu-launcher (which runs it with Proton; with its license and a
README), linux/appimage/AppRun, the script that starts it (see there), the .desktop file
and icon, and the project's LICENSE and README. Runs on Linux (x86-64); its tools are pinned
downloads (installer/deps.py), run without FUSE.
"""
import os
import shutil
import struct
import subprocess
import sys
import tarfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))

from installer import deps, util  # noqa: E402

WORK = REPO / "build" / "appimage"
SOURCE = REPO / "linux" / "appimage"


def icon_png(ico):
    """The largest image of a .ico whose images are PNGs (DAORecomp/res/daorecomp.ico)."""
    data = Path(ico).read_bytes()
    count = struct.unpack_from("<H", data, 4)[0]
    best = None
    for i in range(count):
        width, size, offset = data[6 + 16 * i], *struct.unpack_from("<II", data, 6 + 16 * i + 8)
        image = data[offset:offset + size]
        if image.startswith(b"\x89PNG") and (best is None or (width or 256) > best[0]):
            best = (width or 256, image)
    if best is None:
        sys.exit(f"{ico} has no PNG image")
    return best[1]


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    exe, out = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    if sys.platform != "linux":
        sys.exit("The AppImage is built on Linux.")
    if not exe.is_file():
        sys.exit(f"No {exe}.")
    reporter = util.Reporter()
    downloads = WORK / "downloads"
    umu_tar = util.download(reporter, deps.UMU_ZIPAPP, downloads)
    umu_license = util.download(reporter, deps.UMU_LICENSE, downloads)
    tool = util.download(reporter, deps.APPIMAGETOOL, downloads)
    runtime = util.download(reporter, deps.APPIMAGE_RUNTIME, downloads)

    app = WORK / "AppDir"
    if app.exists():
        shutil.rmtree(app)
    (app / "umu").mkdir(parents=True)
    shutil.copyfile(SOURCE / "AppRun", app / "AppRun")
    (app / "AppRun").chmod(0o755)
    shutil.copyfile(SOURCE / "daorecomp.desktop", app / "daorecomp.desktop")
    (app / "daorecomp.png").write_bytes(icon_png(REPO / "DAORecomp" / "res" / "daorecomp.ico"))
    os.symlink("daorecomp.png", app / ".DirIcon")
    shutil.copyfile(exe, app / "DAORecomp.exe")
    # umu-run, the one program in the zipapp's tar (its name checked, not trusted).
    with tarfile.open(umu_tar) as tar:
        member = tar.getmember("umu/umu-run")
        if not member.isfile():
            sys.exit(f"{umu_tar.name}: umu/umu-run isn't a file")
        (app / "umu" / "umu-run").write_bytes(tar.extractfile(member).read())
    shutil.copyfile(umu_license, app / "umu" / "LICENSE")
    (app / "umu" / "README.txt").write_text(
        f"umu-launcher {deps.UMU_VERSION} (GPL-3.0, see LICENSE), unchanged: {deps.UMU_ZIPAPP.url}\n"
        f"Its source: https://github.com/Open-Wine-Components/umu-launcher/tree/{deps.UMU_VERSION}\n")
    for name in ("LICENSE", "README.md"):
        shutil.copyfile(REPO / name, app / name)

    tool.chmod(0o755)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.unlink(missing_ok=True)
    env = dict(os.environ, ARCH="x86_64", APPIMAGE_EXTRACT_AND_RUN="1")
    subprocess.run([str(tool), "--no-appstream", "--runtime-file", str(runtime), str(app), str(out)],
                   check=True, env=env, cwd=WORK)
    out.chmod(0o755)
    print(f"built {out} ({util.human_size(out.stat().st_size)})")


if __name__ == "__main__":
    main()
