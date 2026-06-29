#!/usr/bin/env python3
"""Structural audit for Development Version 105 branding and documentation."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
errors: list[str] = []
passes: list[str] = []


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8", errors="replace")


def check(name: str, condition: bool) -> None:
    (passes if condition else errors).append(name)

cmake = read("CMakeLists.txt")
build_info = read("src/core/build-info.h")
plugin_main = read("src/obs/plugin-main.h")
locale = read("data/locale/en-US.ini")
readme = read("README.md")
about = read("src/editor/title-editor/signal-handlers.inc")
window = read("src/editor/title-editor/window-session.inc")
assets = read("src/editor/title-assets.h")
transition = read("src/transitions/transition-editor-dialog.cpp")
gitignore = read(".gitignore")

check(
    "public and development versions are synchronized",
    "project(broadcast-graphics-live VERSION 0.8.7)" in cmake
    and 'set(OBS_BGS_PRERELEASE "alpha")' in cmake
    and 'set(OBS_BGS_DEVELOPMENT_VERSION "105")' in cmake
    and '#define PLUGIN_VERSION "0.8.7-alpha"' in build_info
    and '#define BGL_DEVELOPMENT_VERSION "105"' in build_info
    and '#define PLUGIN_VERSION "0.8.7-alpha"' in plugin_main
    and "v0.8.7-alpha" in readme
    and "Development Version 105" in readme,
)

check(
    "OmniaTV About branding is localized and linked",
    'OBSTitles.DevelopedBy="Developed by: omniatv"' in locale
    and 'OBSTitles.OmniaTvWebsiteTooltip="Open omniatv.com"' in locale
    and "new OmniaTvLogo" in about
    and 'https://omniatv.com' in window
    and "QDesktopServices::openUrl" in window,
)

check(
    "theme-aware OmniaTV logo variants are installed",
    (ROOT / "data/icons/omnianormal.svg").is_file()
    and (ROOT / "data/icons/omniainvert.svg").is_file()
    and 'qApp->palette().color(QPalette::Window).lightnessF() < 0.5' in window
    and 'load_svg("icons/omnianormal.svg")' in window
    and 'load_svg("icons/omniainvert.svg")' in window,
)

check(
    "Broadcast Graphics Live application icon replaces inherited OBS icon",
    (ROOT / "data/icons/broadcast-graphics-live-app-icon.svg").is_file()
    and 'bgl_brand_icon()' in assets
    and 'bgl_apply_brand_icon(this);' in window
    and 'bgl_apply_brand_icon(this);' in transition
    and sum(1 for p in (ROOT / "src").rglob("*") if p.is_file() and "bgl_apply_brand_icon" in p.read_text(encoding="utf-8", errors="replace")) >= 4,
)

canonical_docs = {
    "README.md",
    "USER_GUIDE.md",
    "EDITOR_WORKFLOW.md",
    "TEXT_AND_LIVE_DATA.md",
    "RENDERING_AND_CACHE.md",
    "EFFECTS_AND_EXTENSIONS.md",
    "ARCHITECTURE_AND_BUILD.md",
    "CHANGELOG.md",
}
actual_docs = {p.name for p in (ROOT / "docs").iterdir() if p.is_file()}
check("documentation is consolidated to eight canonical files", actual_docs == canonical_docs)
check(
    "machine-readable module map moved out of documentation",
    (ROOT / "tools/modular-source-map.json").is_file()
    and 'ROOT / "tools/modular-source-map.json"' in read("tools/audit_modularity_performance.py"),
)

all_text = "\n".join(
    p.read_text(encoding="utf-8", errors="replace")
    for p in ROOT.rglob("*")
    if p.is_file() and p != Path(__file__).resolve() and p.suffix.lower() in {".md", ".txt", ".ini", ".h", ".cpp", ".inc", ".py", ".json", ".cmake"}
)
check(
    "previous personal-credit wording is removed",
    all(token.lower() not in all_text.lower() for token in ("Ant" + "onios", "Dimo" + "poulos", "Vibe" + " coder")),
)

required_ignore_tokens = (
    "/build-*/", "CMakeCache.txt", "*.dll", "*.so", "*.zip", ".vs/", ".vscode/",
    ".idea/", "__pycache__/", ".flatpak-builder/", "*.log", ".DS_Store", "Thumbs.db",
)
check("gitignore covers build, IDE, package, cache, log, and OS artifacts",
      all(token in gitignore for token in required_ignore_tokens))

print("Development Version 105 branding/documentation audit")
for item in passes:
    print(f"  PASS: {item}")
for item in errors:
    print(f"  FAIL: {item}")
print(f"RESULT: {'PASS' if not errors else 'FAIL'} ({len(passes)} passed, {len(errors)} failed)")
sys.exit(0 if not errors else 1)
