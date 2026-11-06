# Starts the Unreal project as a game, keeping its caches inside the project. The first
# run compiles shaders, which takes a while; tools\build-unreal.ps1 must have run before.
param(
    [switch] $Editor,                # open the editor instead of the game
    [string[]] $Arguments = @()      # passed on, such as -vulkan
)
$ErrorActionPreference = 'Stop'

$Root = Split-Path $PSScriptRoot -Parent
$Binary = Join-Path $Root 'UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe'
$Project = Join-Path $Root 'Unreal\WindsOfTeinora.uproject'

[Environment]::SetEnvironmentVariable('UE-LocalDataCachePath', (Join-Path $Root '.local\ue-cache'), 'Process')

if ($Editor) {
    & $Binary $Project @Arguments
}
else {
    & $Binary $Project -game -windowed -ResX=1600 -ResY=900 @Arguments
}
