param(
    [string]$WdkRoot = "F:\Windows Kits\10",
    [string]$WdkVersion = "10.0.26100.0"
)

$ErrorActionPreference = "Stop"

$devcon = Join-Path $WdkRoot "Tools\$WdkVersion\x64\devcon.exe"
if (-not (Test-Path $devcon)) {
    throw "devcon.exe not found: $devcon"
}

& $devcon remove "Root\LedIdd"
if ($LASTEXITCODE -ne 0) {
    Write-Warning "devcon remove returned $LASTEXITCODE. The virtual display device may already be absent."
}

Write-Host "LED IddCx virtual display remove requested."
