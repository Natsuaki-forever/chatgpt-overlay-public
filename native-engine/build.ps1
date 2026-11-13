param(
    [string]$Output = 'build\native-prototype',
    [string]$MsvcRoot,
    [string]$SdkRoot,
    [switch]$VisualCaptureProbe,
    [Alias('EditorPrototype')][switch]$Editor
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $projectRoot
if ($VisualCaptureProbe -and $Editor) { throw 'Choose either the capture probe or the editor' }
if (($VisualCaptureProbe -or $Editor) -and $Output -eq 'build\native-prototype') { throw 'Choose a separate -Output for the selected target' }
$sourceFile = if ($VisualCaptureProbe) { 'tests\native_visual_capture.cpp' } elseif ($Editor) {
    'native-engine\editor\panel.cpp'
} else { 'native-engine\src\main.cpp' }
$executableName = if ($VisualCaptureProbe) { 'OverlayVisualCaptureProbe.exe' } elseif ($Editor) {
    'OverlayNativeEditorPrototype.exe'
} else { 'OverlayNative.exe' }
$extraLibraries = if ($Editor) { @('comdlg32.lib', 'imm32.lib', 'comctl32.lib', 'gdiplus.lib') } else { @() }
if (-not $MsvcRoot) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'MSVC not found; specify -MsvcRoot or install Visual Studio C++ Build Tools' }
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $installation) { throw 'No x64 MSVC installation found' }
    $versionFile = Join-Path $installation 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt'
    $version = (Get-Content -LiteralPath $versionFile -Raw).Trim()
    $MsvcRoot = Join-Path $installation "VC\Tools\MSVC\$version"
}
$msvc = (Resolve-Path -LiteralPath $MsvcRoot).Path
if (-not $SdkRoot) { $SdkRoot = Join-Path $projectRoot 'tools\windows-sdk' }
$sdk = (Resolve-Path -LiteralPath $SdkRoot).Path
$includeRoot = Join-Path $sdk 'Include'
$sdkVersion = Get-ChildItem -LiteralPath $includeRoot -Directory | Where-Object {
    Test-Path -LiteralPath (Join-Path $_.FullName 'um\Windows.h')
} | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $sdkVersion) { throw 'Windows SDK headers missing; run tools/prepare-sdk.ps1 first' }
$sdkInclude = $sdkVersion.FullName
if (-not (Test-Path -LiteralPath (Join-Path $sdkInclude 'um\Windows.h'))) { throw 'Run tools/prepare-sdk.ps1 first' }
New-Item -ItemType Directory -Path $Output -Force | Out-Null
$oldInclude, $oldLib, $oldPath = $env:INCLUDE, $env:LIB, $env:PATH
try {
    $env:INCLUDE = "$msvc\include;$sdkInclude\ucrt;$sdkInclude\shared;$sdkInclude\um"
    $env:LIB = "$msvc\lib\x64;$sdk\ucrt\x64;$sdk\um\x64"
    $env:PATH = "$msvc\bin\Hostx64\x64;$sdk\bin\$($sdkVersion.Name)\x64;" + $oldPath
    & "$msvc\bin\Hostx64\x64\cl.exe" /nologo /std:c++17 /O2 /EHsc /utf-8 /MT /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN $sourceFile "/Fo$Output\main.obj" "/Fe$Output\$executableName" /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:app.manifest user32.lib gdi32.lib dwmapi.lib d2d1.lib dwrite.lib windowscodecs.lib ole32.lib shell32.lib advapi32.lib psapi.lib @extraLibraries
    if ($LASTEXITCODE -ne 0) { throw 'Native build failed' }
    $licenseDirectory = Join-Path $Output 'licenses'
    New-Item -ItemType Directory -Path $licenseDirectory -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'third_party\LICENSE-json.MIT') -Destination (Join-Path $licenseDirectory 'nlohmann-json.txt')
} finally { $env:INCLUDE, $env:LIB, $env:PATH = $oldInclude, $oldLib, $oldPath }
