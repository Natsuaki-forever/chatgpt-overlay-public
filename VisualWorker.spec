# -*- mode: python ; coding: utf-8 -*-
"""Separate no-Qt visual worker; dependencies are explicit build inputs only."""
from pathlib import Path
import os

dependency_path=os.environ.get('OVERLAY_VISION_BUILD_DEPS')
if not dependency_path or not Path(dependency_path).is_dir():
    raise RuntimeError('Set OVERLAY_VISION_BUILD_DEPS to the installed headless OpenCV directory')

a=Analysis(['tools/run_visual_worker.py'],pathex=[dependency_path],binaries=[],datas=[],hiddenimports=[],
    hookspath=[],hooksconfig={},runtime_hooks=[],excludes=['PySide6','shiboken6','tkinter','matplotlib','scipy','pandas'],
    noarchive=False,optimize=1)
# The worker processes window frames and still-image templates. OpenCV loads
# this optional plugin only for video input/output.
video_plugin = 'opencv_videoio_ffmpeg'
a.binaries = [entry for entry in a.binaries if not Path(entry[0]).name.lower().startswith(video_plugin)]
a.datas = [entry for entry in a.datas if not Path(entry[0]).name.lower().startswith(video_plugin)]
pyz=PYZ(a.pure)
exe=EXE(pyz,a.scripts,[],name='OverlayVision',debug=False,bootloader_ignore_signals=False,
    strip=False,upx=False,console=False,disable_windowed_traceback=False,
    manifest='app.manifest',exclude_binaries=True)
collect=COLLECT(exe,a.binaries,a.datas,strip=False,upx=False,name='OverlayVision')

