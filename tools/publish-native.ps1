function Write-OverlayUpdateIssue([string]$Directory, [string]$Code, [string]$State = 'failed') {
    if (-not $Directory) { return }
    try {
        [IO.Directory]::CreateDirectory($Directory) | Out-Null
        $path = Join-Path $Directory 'update.jsonl'
        if ([IO.File]::Exists($path) -and (Get-Item -LiteralPath $path).Length -ge 128KB) {
            [IO.File]::Move($path, (Join-Path $Directory 'update.previous.jsonl'), $true)
        }
        $line = @{ time = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ'); source = 'update'; code = $Code; state = $State } | ConvertTo-Json -Compress
        [IO.File]::AppendAllText($path, $line + "`n", [Text.UTF8Encoding]::new($false))
    } catch {
        # A diagnostic write must never interrupt recovery.
    }
}

function Assert-OverlayDeliveryPath([string]$Path, [string]$Root) {
    $rootPath = [IO.Path]::GetFullPath($Root).TrimEnd('\')
    $candidate = [IO.Path]::GetFullPath($Path)
    if (-not $candidate.StartsWith($rootPath + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Delivery path is outside the project: $candidate"
    }
    while ($candidate.Length -ge $rootPath.Length) {
        $item = Get-Item -LiteralPath $candidate -Force -ErrorAction SilentlyContinue
        if ($item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Delivery path must not use a link: $candidate"
        }
        if ($candidate.Length -eq $rootPath.Length) { break }
        $candidate = Split-Path -Parent $candidate
    }
}

function Assert-OverlayDelivery([string]$Path) {
    $manifest = Get-Content -Raw -LiteralPath (Join-Path $Path 'build.json') | ConvertFrom-Json
    $files = @{
        'ChatGPTOverlay.exe' = $manifest.engine_sha256
        'editor\OverlayEditor.exe' = $manifest.editor_sha256
    }
    if ($manifest.optional_visual_worker -or $manifest.vision_sha256) {
        $files['vision\OverlayVision.exe'] = $manifest.vision_sha256
    }
    foreach ($relative in $files.Keys) {
        $binary = Join-Path $Path $relative
        if (-not $files[$relative] -or (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash -ne $files[$relative]) {
            throw "Delivery integrity check failed: $relative"
        }
    }
}

function Restore-InterruptedOverlayDelivery([string]$Current, [string]$Root, [string]$IssueDirectory) {
    $previous = "$Current.previous"
    Assert-OverlayDeliveryPath $Current $Root
    Assert-OverlayDeliveryPath $previous $Root
    if (-not (Test-Path -LiteralPath $previous)) { return }
    if (Test-Path -LiteralPath $Current) {
        throw "A previous delivery is still retained at $previous. Review it before another update."
    }
    try {
        Assert-OverlayDelivery $previous
        [IO.Directory]::Move($previous, $Current)
    } catch {
        Write-OverlayUpdateIssue $IssueDirectory 'update_restore_failed'
        throw
    }
    Write-OverlayUpdateIssue $IssueDirectory 'update_restored' 'recovered'
    Write-Host "Restored the previous delivery: $Current"
}

function Switch-OverlayDelivery(
    [string]$Staged, [string]$Current, [string]$Root,
    [string]$IssueDirectory,
    [scriptblock]$VerifyInstalled = { param($Path) Assert-OverlayDelivery $Path }
) {
    $previous = "$Current.previous"
    Assert-OverlayDeliveryPath $Staged $Root
    Assert-OverlayDeliveryPath $Current $Root
    Assert-OverlayDeliveryPath $previous $Root
    if (Test-Path -LiteralPath $previous) { throw "Previous delivery still exists: $previous" }
    try { Assert-OverlayDelivery $Staged } catch {
        Write-OverlayUpdateIssue $IssueDirectory 'update_verification_failed'
        throw
    }
    $hadPrevious = [IO.Directory]::Exists($Current)
    $previousMoved = $false
    $newInPlace = $false
    try {
        if ($hadPrevious) {
            [IO.Directory]::Move($Current, $previous)
            $previousMoved = $true
        }
        [IO.Directory]::Move($Staged, $Current)
        $newInPlace = $true
        & $VerifyInstalled $Current
    } catch {
        $failure = $_
        Write-OverlayUpdateIssue $IssueDirectory 'update_switch_failed'
        try {
            if ($newInPlace) { [IO.Directory]::Move($Current, $Staged) }
            if ($previousMoved) {
                [IO.Directory]::Move($previous, $Current)
                Write-OverlayUpdateIssue $IssueDirectory 'update_restored' 'recovered'
            }
        } catch {
            Write-OverlayUpdateIssue $IssueDirectory 'update_restore_failed'
            throw "Update failed ($failure). Automatic restore failed ($_). Previous delivery: $previous"
        }
        throw $failure
    }
    if ($hadPrevious) {
        try {
            Assert-OverlayDeliveryPath $previous $Root
            Remove-Item -LiteralPath $previous -Recurse -Force -ErrorAction Stop
        } catch {
            Write-OverlayUpdateIssue $IssueDirectory 'update_cleanup_failed'
            Write-Warning "New delivery is active. Previous directory cleanup failed: $previous"
        }
    }
}
