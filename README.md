# ChatGPT Overlay 0.01α

Windows 11 x64 的本地遮盖工具。可为不同应用和窗口设置矩形遮盖，自定义文字、颜色和效果，并选择窗口锚定或视觉定位。正常使用时，鼠标可穿透遮盖。

## 使用

1. 从 Releases 下载并完整解压 ZIP，启动 `ChatGPTOverlay.exe`。请保留同级的 `editor` 和 `vision` 目录。
2. 按 **Ctrl+Shift+E** 打开面板，在顶部选择应用和窗口。
3. 点击「新建」框选区域，再调整属性。点击「完成」保存。视觉定位可在「位置」中设置参考区域和遮盖关系。

应用设置和识别模板保存在本机 `%APPDATA%\ChatGPTOverlay`。视觉组件按需读取所选窗口的参考区域，识别在本机进行。此版本为 Alpha。

## 构建

需要 MSVC、Windows SDK 和 Python。默认编辑器使用 Win32。Windows SDK 放在 `tools/windows-sdk`，或使用 `tools/prepare-sdk.ps1 -DownloadMissing` 准备。

```powershell
python -m pip install -r requirements.txt
python -m pip install -r visual_tracking/requirements.txt --target build/vision-deps
.\build-native.ps1 -Output dist\candidate -Work build\candidate -VisionDeps build\vision-deps -StageOnly
```

第三方组件许可见源码对应目录和发布包的 `licenses`。
