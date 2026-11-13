# -*- mode: python ; coding: utf-8 -*-
from pathlib import Path
import sys
import os
import pefile
import PySide6

qt_dir = Path(PySide6.__file__).parent
editor_build = os.environ.get('OVERLAY_ENTRY') == 'editor'
entry_file = 'editor_main.py' if editor_build else 'main.py'
product_name = 'OverlayEditor' if editor_build else 'ChatGPTOverlay'
runtime_names = {p.name.lower() for pattern in ('msvcp140*.dll', 'vcruntime140*.dll', 'concrt140.dll') for p in qt_dir.glob(pattern)}


def version(path):
    with pefile.PE(str(path), fast_load=True) as pe:
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_RESOURCE']])
        info = pe.VS_FIXEDFILEINFO[0]
        return info.FileVersionMS, info.FileVersionLS


a = Analysis(
    [entry_file],
    pathex=[],
    binaries=[],
    datas=[('overlay/assets/chevron-down.png','overlay/assets'),('overlay/assets/chevron-down-light.png','overlay/assets')],
    hiddenimports=[],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
    optimize=0,
)
# Qt 6.11 uses the Windows 11 system ICU ABI. A Poppler/Conda DLL with the same
# basename on PATH has version-suffixed exports and must never be bundled.
a.binaries = [entry for entry in a.binaries if Path(entry[0]).name.lower() not in {'icuuc.dll', 'icuin.dll', 'icudt.dll'}]
# Windows reuses the first loaded DLL basename. A newer nested copy cannot
# replace an older runtime already loaded by Python at bootloader startup.
for name in sorted(runtime_names):
    candidates = [Path(source) for dest, source, kind in a.binaries if Path(dest).name.lower() == name]
    candidates += [p for p in qt_dir.glob('*.dll') if p.name.lower() == name]
    python_copy = Path(sys.base_prefix) / name
    if python_copy.exists():
        candidates.append(python_copy)
    newest = max(candidates, key=version)
    a.binaries = [entry for entry in a.binaries if Path(entry[0]).name.lower() != name]
    a.binaries.append((name, str(newest), 'BINARY'))

pyz = PYZ(a.pure)

onefile = not editor_build and os.environ.get('OVERLAY_BUNDLE_MODE', 'onedir') == 'onefile'
bundle = [a.binaries, a.datas, []] if onefile else [[]]
exe = EXE(
    pyz, a.scripts, *bundle,
    name=product_name,
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    manifest='app.manifest',
    exclude_binaries=not onefile,
)
if not onefile:
    collect = COLLECT(exe, a.binaries, a.datas, strip=False, upx=False, name=product_name)
