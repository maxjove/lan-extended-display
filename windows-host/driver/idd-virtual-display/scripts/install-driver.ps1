param(
    [string]$RepoRoot = (Resolve-Path "$PSScriptRoot\..\..\..\..").Path,
    [string]$BuildDir = "build-idd",
    [string]$WdkRoot = "F:\Windows Kits\10",
    [string]$WdkVersion = "10.0.26100.0"
)

$ErrorActionPreference = "Stop"

$packagePath = Join-Path (Join-Path $RepoRoot $BuildDir) "driver\idd-virtual-display"
$infPath = Join-Path $packagePath "led_idd.inf"
$devcon = Join-Path $WdkRoot "Tools\$WdkVersion\x64\devcon.exe"

if (-not (Test-Path $infPath)) {
    throw "Driver INF not found: $infPath"
}

if (-not (Test-Path $devcon)) {
    throw "devcon.exe not found: $devcon"
}

$existingDevices = & $devcon find "Root\LedIdd"
if (($LASTEXITCODE -eq 0) -and ($existingDevices -match "LED LAN Virtual Display Adapter")) {
    Write-Host "LED IddCx virtual display device already exists."
    $existingDevices
    exit 0
}

pnputil /add-driver $infPath
if ($LASTEXITCODE -ne 0) {
    throw "pnputil add-driver failed with exit code $LASTEXITCODE"
}

& $devcon install $infPath "Root\LedIdd"
if ($LASTEXITCODE -ne 0) {
    Write-Warning "devcon returned $LASTEXITCODE. Check pnputil /enum-devices /instanceid ROOT\DISPLAY\0002 /properties."
}

pnputil /enum-devices /class Display | Select-String -Pattern "LED LAN Virtual Display|ROOT\\DISPLAY|Status|Problem|Driver Name" -Context 1,2
