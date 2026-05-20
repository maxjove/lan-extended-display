param(
    [string]$RepoRoot = (Resolve-Path "$PSScriptRoot\..\..\..\..").Path,
    [string]$BuildDir = "build-idd",
    [string]$VsDevCmd = "F:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\Tools\VsDevCmd.bat",
    [string]$CMake = "F:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    [string]$WdkRoot = "F:\Windows Kits\10",
    [string]$WdkVersion = "10.0.26100.0",
    [string]$UmdfVersion = "2.25",
    [string]$CertSubject = "CN=LAN Extended Display Test Driver"
)

$ErrorActionPreference = "Stop"

$buildPath = Join-Path $RepoRoot $BuildDir
$packagePath = Join-Path $buildPath "driver\idd-virtual-display"
$inf2cat = Join-Path $WdkRoot "bin\$WdkVersion\x86\Inf2Cat.exe"
$signtool = Join-Path $WdkRoot "bin\$WdkVersion\x64\signtool.exe"

cmd /s /c "`"$VsDevCmd`" -arch=x64 && `"$CMake`" -S `"$RepoRoot`" -B `"$buildPath`" -G `"Visual Studio 18 2026`" -A x64 -DLED_BUILD_CLIENT=OFF -DLED_BUILD_TOOLS=OFF -DLED_BUILD_IDD_DRIVER=ON -DLED_WDK_ROOT=`"$WdkRoot`" -DLED_WDK_VERSION=$WdkVersion -DLED_UMDF_VERSION=$UmdfVersion && `"$CMake`" --build `"$buildPath`" --config Debug --target led_idd_package"
if ($LASTEXITCODE -ne 0) {
    throw "Driver build failed with exit code $LASTEXITCODE"
}

$catPath = Join-Path $packagePath "led_idd.cat"
Remove-Item -LiteralPath $catPath -ErrorAction SilentlyContinue
& $inf2cat /driver:$packagePath /os:10_X64,10_GE_X64
if ($LASTEXITCODE -ne 0) {
    throw "Inf2Cat failed with exit code $LASTEXITCODE"
}

$cert = Get-ChildItem Cert:\LocalMachine\My |
    Where-Object { $_.Subject -eq $CertSubject } |
    Select-Object -First 1

if (-not $cert) {
    $cert = New-SelfSignedCertificate `
        -Subject $CertSubject `
        -Type CodeSigningCert `
        -KeyUsage DigitalSignature `
        -KeyAlgorithm RSA `
        -KeyLength 2048 `
        -HashAlgorithm SHA256 `
        -CertStoreLocation "Cert:\LocalMachine\My" `
        -NotAfter (Get-Date).AddYears(5)
}

foreach ($storeName in @("Root", "TrustedPublisher")) {
    $store = New-Object System.Security.Cryptography.X509Certificates.X509Store($storeName, "LocalMachine")
    $store.Open("ReadWrite")
    $store.Add($cert)
    $store.Close()
}

& $signtool sign /sm /fd SHA256 /sha1 $cert.Thumbprint $catPath
if ($LASTEXITCODE -ne 0) {
    throw "signtool sign failed with exit code $LASTEXITCODE"
}

& $signtool verify /pa /v $catPath
if ($LASTEXITCODE -ne 0) {
    throw "signtool verify failed with exit code $LASTEXITCODE"
}

Write-Host "Driver package ready: $packagePath"
