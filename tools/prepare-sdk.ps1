param([switch]$DownloadMissing)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$sdkRoot = Join-Path $PSScriptRoot 'windows-sdk'
New-Item -ItemType Directory -Path $sdkRoot -Force | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem
$sdkAbsolute = (Resolve-Path -LiteralPath $sdkRoot).Path
$packages = @{
    'sdk-headers' = @('microsoft.windows.sdk.cpp', '8C17F5BEB7616CBFB665F76F2E1AD74ECEAF7B97C8EF1D0BBC4CA2EB9E525413')
    'sdk-x64' = @('microsoft.windows.sdk.cpp.x64', '8150A2F0DC69FD2B2A3AFD6D66B38E3DBAB2B901CC5DC65DE42BE4836C3953F3')
}
$packageDirectory = Join-Path $PSScriptRoot 'sdk-packages'
New-Item -ItemType Directory -Path $packageDirectory -Force | Out-Null
foreach ($packageName in @('sdk-headers', 'sdk-x64')) {
    $packagePath = Join-Path $packageDirectory "$packageName.nupkg"
    $packageId, $expectedHash = $packages[$packageName]
    if (-not (Test-Path -LiteralPath $packagePath)) {
        if (-not $DownloadMissing) { throw "Missing $packagePath; use -DownloadMissing to fetch the pinned official NuGet packages" }
        $temporaryDownload = $packagePath + '.download'
        $packageVersion = '10.0.26100.4948'
        Invoke-WebRequest -Uri "https://api.nuget.org/v3-flatcontainer/$packageId/$packageVersion/$packageId.$packageVersion.nupkg" -OutFile $temporaryDownload
        if ((Get-FileHash -LiteralPath $temporaryDownload -Algorithm SHA256).Hash -ne $expectedHash) { throw "SDK package checksum mismatch: $packageName" }
        Move-Item -LiteralPath $temporaryDownload -Destination $packagePath
    }
    if ((Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash -ne $expectedHash) { throw "SDK package checksum mismatch: $packageName" }
    $archive = [System.IO.Compression.ZipFile]::OpenRead($packagePath)
    try {
        foreach ($entry in $archive.Entries) {
            $include = $entry.FullName -match '^c/Include/10\.0\.26100\.0/(um|shared|ucrt)/'
            $library = $entry.FullName -match '^c/(um|ucrt)/x64/'
            $buildTool = $entry.FullName -match '^c/bin/10\.0\.26100\.0/x64/(mt\.exe|rc\.exe|rcdll\.dll|MidlrtMd\.dll)$'
            if (($include -or $library -or $buildTool) -and $entry.Name) {
                $relative = $entry.FullName.Substring(2).Replace('/', '\')
                $destination = [System.IO.Path]::GetFullPath((Join-Path $sdkAbsolute $relative))
                if (-not $destination.StartsWith($sdkAbsolute + '\', [System.StringComparison]::OrdinalIgnoreCase)) { throw 'SDK entry escaped target directory' }
                New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
                [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $destination, $true)
            }
        }
    } finally { $archive.Dispose() }
}
Write-Output "Project-local SDK ready: $sdkAbsolute"
