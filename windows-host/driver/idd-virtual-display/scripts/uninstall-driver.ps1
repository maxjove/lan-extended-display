param(
    [string]$WdkRoot = "F:\Windows Kits\10",
    [string]$WdkVersion = "10.0.26100.0"
)

$ErrorActionPreference = "Stop"

$devcon = Join-Path $WdkRoot "Tools\$WdkVersion\x64\devcon.exe"

& $devcon remove "Root\LedIdd"

$drivers = pnputil /enum-drivers /class Display
$publishedNames = @()
for ($i = 0; $i -lt $drivers.Count; ++$i) {
    if ($drivers[$i] -match "Published Name:\s+(oem\d+\.inf)") {
        $publishedName = $matches[1]
        $block = ($drivers[$i..([Math]::Min($i + 8, $drivers.Count - 1))] -join "`n")
        if ($block -match "Original Name:\s+led_idd\.inf") {
            $publishedNames += $publishedName
        }
    }
}

foreach ($name in $publishedNames) {
    pnputil /delete-driver $name /uninstall /force
}

Write-Host "LED IddCx driver uninstall attempted."
