# Builds RetooledXeSS.dll and copies it into Plugins/x86_64. Needs CMake, Ninja, LLVM's clang-cl, the Windows SDK and
# the Vulkan headers, from the Vulkan SDK or from a Vulkan-Headers checkout passed as -VulkanInclude.
# A Unity editor that has loaded the plugin keeps the DLL open, so close it before building.
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [string]$VulkanInclude
)

$ErrorActionPreference = 'Stop'
$native = $PSScriptRoot
$build = Join-Path $native "build/$Configuration"
$plugins = Join-Path $native '../Plugins/x86_64'

if (-not (Test-Path (Join-Path $native 'xess/inc/xess/xess.h'))) {
    throw "The XeSS SDK headers are missing from $native/xess/inc. Run fetch-sdk.ps1 first."
}

$vulkan = if ($VulkanInclude) { "-DVulkan_INCLUDE_DIR=$VulkanInclude" }
cmake -S $native -B $build -G Ninja `
    "-DCMAKE_BUILD_TYPE=$Configuration" `
    -DCMAKE_CXX_COMPILER=clang-cl `
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON `
    $vulkan
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }

cmake --build $build
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }

Copy-Item (Join-Path $build 'RetooledXeSS.dll') $plugins -Force
Write-Host "Copied RetooledXeSS.dll to $(Resolve-Path $plugins)"
