# Builds the PhysX SDK from .local\physx into .local\physx-sdk\<Crt>, using the compiler
# inside the project. It is a project of its own: PhysX fixes its own language standard
# and warning flags, which do not mix with this one's. Run it after tools\fetch-msvc.ps1.
#
# -Crt static is what the standalone game links. -Crt dynamic is for the Unreal project,
# which always uses the dynamic runtime. Neither needs the CUDA toolkit: rigid bodies
# run on the CPU.
param(
    [ValidateSet('static', 'dynamic')] [string] $Crt = 'static'
)
$ErrorActionPreference = 'Stop'

$Root = Split-Path $PSScriptRoot -Parent
$Local = Join-Path $Root '.local'
$Source = Join-Path $Local 'physx\physx'
$Build = Join-Path $Local "build\physx-$Crt"
$Install = Join-Path $Local "physx-sdk\$Crt"

if (-not (Test-Path (Join-Path $Source 'CMakeLists.txt'))) {
    throw 'PhysX is not in .local\physx: run tools\bootstrap.ps1 first'
}

$env:Path = "$Local\cmake\bin;$Local\ninja;$env:Path"
$env:CL = '/FS' # PhysX compiles with /Zi, and Ninja runs many compilers against one PDB

cmake -S $Source -B $Build -G Ninja "--toolchain=$Root\cmake\msvc-local.cmake" `
    -DCMAKE_BUILD_TYPE=Release `
    "-DCMAKE_INSTALL_PREFIX=$Install" `
    -DPX_GENERATE_GPU_PROJECTS=OFF `
    -DPX_GENERATE_STATIC_LIBRARIES=ON `
    "-DNV_USE_STATIC_WINCRT=$(if ($Crt -eq 'static') { 'ON' } else { 'OFF' })" `
    -DNV_USE_DEBUG_WINCRT=OFF `
    -DPX_BUILDSNIPPETS=OFF `
    -DPX_BUILDPVDRUNTIME=OFF
if ($LASTEXITCODE -ne 0) { throw 'PhysX configure failed' }

cmake --build $Build
if ($LASTEXITCODE -ne 0) { throw 'PhysX build failed' }

cmake --install $Build
if ($LASTEXITCODE -ne 0) { throw 'PhysX install failed' }

Write-Host "PhysX ($Crt runtime) installed to $Install"
