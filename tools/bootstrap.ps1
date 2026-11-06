# Downloads the open-source build tools into .local\ inside the project. Nothing
# is written to the registry, PATH or any folder outside the project. The MSVC
# compiler and the Windows SDK come separately from tools\fetch-msvc.ps1.
#
# -WithNvidia fetches the NVIDIA Streamline SDK, which is optional: without it the
# engine builds and runs the same, only with no DLSS or Reflex. Using the SDK means
# accepting NVIDIA's terms: https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt
param(
    [switch] $WithValidation,   # also fetch the Vulkan validation layers, for debugging
    [switch] $WithNvidia
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$Local = Join-Path (Split-Path $PSScriptRoot -Parent) '.local'
$Cache = Join-Path $Local 'cache'
New-Item -ItemType Directory -Force $Cache | Out-Null

$Tools = @(
    @{ Name = 'cmake'; Url = 'https://github.com/Kitware/CMake/releases/download/v4.4.5/cmake-4.4.5-windows-x86_64.zip'; Check = 'bin\cmake.exe' }
    @{ Name = 'ninja'; Url = 'https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip'; Check = 'ninja.exe' }
    @{ Name = 'dxc'; Url = 'https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/dxc_2026_09_29.zip'; Check = 'bin\x64\dxc.exe' }
    @{ Name = 'vulkan-headers'; Url = 'https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.363.0.zip'; Check = 'include\vulkan\vulkan.h' }
    @{ Name = 'physx'; Url = 'https://github.com/NVIDIA-Omniverse/PhysX/archive/refs/tags/110.3-omni-and-physx-5.9.1.zip'; Check = 'physx\include\PxPhysicsAPI.h' }
)
if ($WithValidation) {
    $Tools += @{ Name = 'vulkan-validation'; Url = 'https://github.com/KhronosGroup/Vulkan-ValidationLayers/releases/download/nightly/Validation_Layers_Windows_64.zip'; Check = '' }
}

if ($WithNvidia) {
    $Tools += @{ Name = 'streamline'; Url = 'https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.14.16/streamline-sdk-v2.14.16.zip'; Check = 'include\sl.h' }
}

foreach ($Tool in $Tools) {
    $Target = Join-Path $Local $Tool.Name
    if ($Tool.Check -and (Test-Path (Join-Path $Target $Tool.Check))) {
        Write-Host "$($Tool.Name): already installed"
        continue
    }

    $Archive = Join-Path $Cache (Split-Path $Tool.Url -Leaf)
    if (-not (Test-Path $Archive)) {
        Write-Host "$($Tool.Name): downloading $($Tool.Url)"
        Invoke-WebRequest $Tool.Url -OutFile "$Archive.part"
        Move-Item "$Archive.part" $Archive
    }

    $Unpacked = Join-Path $Cache "$($Tool.Name)-unpacked"
    if (Test-Path $Unpacked) { Remove-Item -Recurse -Force $Unpacked }
    Expand-Archive $Archive $Unpacked

    # Most archives wrap everything in one versioned folder; strip it.
    $Entries = @(Get-ChildItem $Unpacked)
    $Source = if ($Entries.Count -eq 1 -and $Entries[0].PSIsContainer) { $Entries[0].FullName } else { $Unpacked }
    if (Test-Path $Target) { Remove-Item -Recurse -Force $Target }
    Move-Item $Source $Target
    if (Test-Path $Unpacked) { Remove-Item -Recurse -Force $Unpacked }
    Write-Host "$($Tool.Name): installed to $Target"
}
