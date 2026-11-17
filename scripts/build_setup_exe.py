"""Builds what players download: dist/DAORecomp.exe, the launcher with its Install view
and no game code, carrying the installer (dist/installer-engine/, built first by
build_installer_engine.py). Run from a git checkout on Windows or Linux:
    python scripts/build_setup_exe.py [--skip-installer] [--llvm auto|<bin folder>]
Releases use the pinned LLVM download (the default); --llvm auto takes the system's, for a
local test build where that download doesn't run (it needs libxml2.so.2).
Running it accepts the Microsoft build tools license for their download, as
install.py --accept-msvc-license does.
"""
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
sys.path.insert(0, str(REPO / "scripts"))

import build_installer_engine  # noqa: E402
from installer import pipeline  # noqa: E402
from installer.util import InstallError, Reporter  # noqa: E402


def main():
    if "--skip-installer" not in sys.argv:
        build_installer_engine.main()
    engine = build_installer_engine.OUT
    if not (engine / "python.exe").exists() or not (engine / build_installer_engine.BUNDLE_NAME).exists():
        sys.exit(f"{engine} isn't built.")
    try:
        if sys.argv[-1] == "--llvm":
            sys.exit("--llvm needs a value: auto or LLVM's bin folder.")
        llvm = sys.argv[sys.argv.index("--llvm") + 1] if "--llvm" in sys.argv else "download"
        pipeline.build_setup(Reporter(), engine, REPO / "dist" / "DAORecomp.exe", llvm=llvm)
    except InstallError as e:
        sys.exit(f"Error: {e}")


if __name__ == "__main__":
    main()
