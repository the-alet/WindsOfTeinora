# Builds the Unreal project with the compiler and SDK inside the project, so no Visual
# Studio installation is involved. Unreal finds them through AutoSDK, which only needs
# them in a particular layout: that is made of junctions, under .local\ue-sdks.
#
# It needs UE_5.8 in the project folder, tools\fetch-msvc.ps1 and
# tools\build-physx.ps1 -Crt dynamic.
param(
    [ValidateSet('Development', 'DebugGame')] [string] $Configuration = 'Development',
    [switch] $Materials # remake the materials even if they exist
)
$ErrorActionPreference = 'Stop'

$Root = Split-Path $PSScriptRoot -Parent
$Local = Join-Path $Root '.local'
$Engine = Join-Path $Root 'UE_5.8\Engine'
$Project = Join-Path $Root 'Unreal\WindsOfTeinora.uproject'

$Toolset = Get-ChildItem (Join-Path $Local 'msvc\VC\Tools\MSVC') -Directory | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $Toolset) { throw 'MSVC is not in .local: run tools\fetch-msvc.ps1 first' }
if (-not (Test-Path (Join-Path $Local 'physx-sdk\dynamic\lib\PhysX_static_64.lib'))) { throw 'PhysX is not built for Unreal: run tools\build-physx.ps1 -Crt dynamic' }

$Sdks = Join-Path $Local 'ue-sdks'
$Win64 = Join-Path $Sdks 'HostWin64\Win64'
$Links = @{
    (Join-Path $Win64 "VS2022\$($Toolset.Name)") = $Toolset.FullName
    (Join-Path $Win64 'Windows Kits\10')         = Join-Path $Local 'msvc\winsdk'
    (Join-Path $Win64 'Windows Kits\NETFXSDK\4.6.2') = Join-Path $Local 'msvc\netfxsdk\4.6.2'
}
foreach ($Link in $Links.Keys) {
    if (-not (Test-Path $Link)) {
        New-Item -ItemType Directory -Force (Split-Path $Link) | Out-Null
        New-Item -ItemType Junction -Path $Link -Target $Links[$Link] | Out-Null
    }
}
$env:UE_SDKS_ROOT = $Sdks
[Environment]::SetEnvironmentVariable('UE-LocalDataCachePath', (Join-Path $Local 'ue-cache'), 'Process') # the name has a dash in it

& (Join-Path $Engine 'Build\BatchFiles\Build.bat') WindsOfTeinoraEditor Win64 $Configuration "-Project=$Project" -WaitMutex -NoHotReloadFromIDE
if ($LASTEXITCODE -ne 0) { throw "Unreal build failed with exit code $LASTEXITCODE" }

# The materials are made by a script rather than kept as binary files.
$Content = Join-Path $Root 'Unreal\Content\Teinora'
if ($Materials -or -not (Test-Path (Join-Path $Content 'M_Focus.uasset'))) {
    $Script = Join-Path $Root 'Unreal\Scripts\make_content.py'
    & (Join-Path $Engine 'Binaries\Win64\UnrealEditor-Cmd.exe') $Project -run=pythonscript "-script=$Script" -unattended -nosplash -nullrhi -nop4 -stdout -FullStdOutLogOutput
    if ($LASTEXITCODE -ne 0) { throw "Making the materials failed with exit code $LASTEXITCODE" }
}
