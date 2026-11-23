# Downloads the XeSS SDK release and copies what the package needs out of it: the headers and licenses into
# Native~/xess, and the runtime DLLs into Plugins/x86_64 with the import settings kept in Native~/Plugins.
# Pass -Zip to use a copy of the release zip that is already on disk.
param(
    [string]$Zip
)

$ErrorActionPreference = 'Stop'
$version = '3.0.2'
$sha256 = '88B8A373F30E33F3558A77A93E634F11B8132FC3047EA1A8EDEEAD32B8471990'
$url = "https://github.com/intel/xess/releases/download/v$version/XeSS_SDK_$version.zip"

$native = $PSScriptRoot
$sdk = Join-Path $native 'xess'
$plugins = Join-Path $native '../Plugins/x86_64'
$temp = Join-Path ([IO.Path]::GetTempPath()) "xess-sdk-$version"

if (-not $Zip) {
    New-Item -ItemType Directory -Force $temp | Out-Null
    $Zip = Join-Path $temp "XeSS_SDK_$version.zip"
    if (-not (Test-Path $Zip)) {
        Write-Host "Downloading $url"
        Invoke-WebRequest $url -OutFile "$Zip.part"
        Move-Item "$Zip.part" $Zip
    }
}

$actual = (Get-FileHash $Zip -Algorithm SHA256).Hash
if ($actual -ne $sha256) {
    throw "$Zip has SHA-256 $actual, but XeSS SDK $version has $sha256."
}

$extracted = Join-Path $temp 'extracted'
Remove-Item -Recurse -Force $extracted -ErrorAction SilentlyContinue
Expand-Archive $Zip $extracted

Remove-Item -Recurse -Force $sdk -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $sdk 'inc') | Out-Null
Copy-Item -Recurse (Join-Path $extracted 'inc/xess') (Join-Path $sdk 'inc')
Copy-Item (Join-Path $extracted 'LICENSE.txt'), (Join-Path $extracted 'third-party-programs.txt') $sdk

foreach ($dll in 'libxess.dll', 'libxess_dx11.dll') {
    Copy-Item (Join-Path $extracted "bin/$dll") $plugins -Force
    Copy-Item (Join-Path $native "Plugins/x86_64/$dll.meta") $plugins -Force
}

Remove-Item -Recurse -Force $extracted
Write-Host "Copied XeSS SDK $version into $(Resolve-Path $sdk) and $(Resolve-Path $plugins)"
