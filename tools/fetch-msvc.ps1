# Unpacks the MSVC compiler and the Windows SDK from Microsoft's official
# manifests into .local\msvc. Nothing is installed or registered, and no admin
# rights are needed. cmake\msvc-local.cmake picks the result up.
#
#   tools\fetch-msvc.ps1 -List             show what would be downloaded
#   tools\fetch-msvc.ps1 -AcceptLicense    download and unpack
#
# -AcceptLicense means you accept the Visual Studio Build Tools and Windows SDK
# licence terms: https://go.microsoft.com/fwlink/?LinkId=2179911
param(
    [switch] $AcceptLicense,
    [switch] $List,
    [string] $MsvcVersion = '',    # package prefix such as 14.44; empty picks the newest
    [string] $SdkBuild = ''        # Windows 11 SDK build such as 26100; empty picks the newest
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

if (-not ($List -or $AcceptLicense)) {
    throw 'Pass -List to preview, or -AcceptLicense to accept the licence terms and install.'
}

$Local = Join-Path (Split-Path $PSScriptRoot -Parent) '.local'
$Cache = Join-Path $Local 'cache\msvc'
$Root = Join-Path $Local 'msvc'

$Channel = Invoke-RestMethod 'https://aka.ms/vs/17/release/channel'
$ManifestItem = $Channel.channelItems | Where-Object id -eq 'Microsoft.VisualStudio.Manifests.VisualStudio'
$Manifest = Invoke-RestMethod $ManifestItem.payloads[0].url -TimeoutSec 300

$Packages = @{}
foreach ($Package in $Manifest.packages) {
    if (-not $Package.language -or $Package.language -eq 'en-US') {
        $Packages[$Package.id.ToLowerInvariant()] = $Package
    }
}

# --- MSVC -------------------------------------------------------------------
$Suffix = '.tools.hostx64.targetx64.base'
$Versions = $Packages.Keys |
    Where-Object { $_ -match "^microsoft\.vc\.(\d+\.\d+\.\d+\.\d+)$([regex]::Escape($Suffix))$" } |
    ForEach-Object { $_ -replace '^microsoft\.vc\.', '' -replace [regex]::Escape($Suffix), '' } |
    Where-Object { $_.StartsWith($MsvcVersion) } |
    Sort-Object { [version]$_ } -Descending
if (-not $Versions) { throw "No MSVC toolset matches '$MsvcVersion'." }
$Pv = @($Versions)[0]

$MsvcPayloads = foreach ($Id in @(
    "microsoft.vc.$Pv.tools.hostx64.targetx64.base"
    "microsoft.vc.$Pv.tools.hostx64.targetx64.res.base"
    "microsoft.vc.$Pv.premium.tools.hostx64.targetx64.base"
    "microsoft.vc.$Pv.crt.headers.base"
    "microsoft.vc.$Pv.crt.x64.desktop.base"
    "microsoft.vc.$Pv.crt.x64.store.base"     # also holds msvcrt.lib, vcruntime.lib and oldnames.lib
)) {
    if (-not $Packages.ContainsKey($Id)) { throw "Package not found: $Id" }
    $Packages[$Id].payloads
}

# --- Windows SDK ------------------------------------------------------------
$SdkBuilds = $Packages.Keys |
    Where-Object { $_ -match '^win11sdk_10\.0\.(\d+)$' } |
    ForEach-Object { [int]($_ -replace '^win11sdk_10\.0\.', '') } |
    Where-Object { -not $SdkBuild -or $_ -eq [int]$SdkBuild } |
    Sort-Object -Descending
if (-not $SdkBuilds) { throw "Windows SDK build '$SdkBuild' is not in the manifest." }
$Sdk = @($SdkBuilds)[0]

$SdkPayloads = $Packages["win11sdk_10.0.$Sdk"].payloads
$SdkMsiNames = @(
    'Windows SDK Desktop Headers x64-x86_en-us.msi'
    'Windows SDK Desktop Headers x86-x86_en-us.msi'
    'Windows SDK Desktop Libs x64-x86_en-us.msi'
    'Windows SDK Desktop Tools x64-x86_en-us.msi'
    'Windows SDK OnecoreUap Headers x64-x86_en-us.msi'
    'Windows SDK OnecoreUap Headers x86-x86_en-us.msi'
    'Windows SDK for Windows Store Apps Headers OnecoreUap-x86_en-us.msi'
    'Windows SDK for Windows Store Apps Headers-x86_en-us.msi'
    'Windows SDK for Windows Store Apps Libs-x86_en-us.msi'
    'Windows SDK for Windows Store Apps Tools-x86_en-us.msi'
    'Windows SDK Signing Tools-x86_en-us.msi'
    'Universal CRT Headers Libraries and Sources-x86_en-us.msi'
)
$SdkMsis = @($SdkPayloads | Where-Object { $SdkMsiNames -contains (Split-Path $_.fileName -Leaf) })
if ($SdkMsis.Count -ne $SdkMsiNames.Count) { throw "Windows SDK $Sdk has only $($SdkMsis.Count) of $($SdkMsiNames.Count) installers." }

$MsvcBytes = ($MsvcPayloads | Measure-Object size -Sum).Sum
$SdkBytes = ($SdkPayloads | Measure-Object size -Sum).Sum
Write-Host "MSVC package: $Pv, $([math]::Round($MsvcBytes / 1MB)) MB"
Write-Host "Windows SDK: 10.0.$Sdk, at most $([math]::Round($SdkBytes / 1MB)) MB (only the cabinets of $($SdkMsis.Count) installers are fetched)"
if ($List) { return }

function Get-Payload($Payload, $Folder) {
    New-Item -ItemType Directory -Force $Folder | Out-Null
    $Path = Join-Path $Folder (Split-Path $Payload.fileName -Leaf)
    if (-not (Test-Path $Path) -or (Get-Item $Path).Length -ne $Payload.size) {
        Write-Host "  $(Split-Path $Path -Leaf)"
        Invoke-WebRequest $Payload.url -OutFile $Path
    }
    return $Path
}

Add-Type -AssemblyName System.IO.Compression.FileSystem

# VSIX files are zip archives; everything needed sits under Contents\VC.
Write-Host 'Unpacking MSVC'
foreach ($Payload in $MsvcPayloads) {
    $Vsix = Get-Payload $Payload (Join-Path $Cache 'vsix')
    $Zip = [IO.Compression.ZipFile]::OpenRead($Vsix)
    try {
        foreach ($Entry in $Zip.Entries) {
            $Name = [Uri]::UnescapeDataString($Entry.FullName)
            if ($Name -match '^Contents/(VC/.+[^/])$') {
                $Destination = Join-Path $Root ($Matches[1] -replace '/', '\')
                # A re-run skips what is there: compiler servers may hold these files open.
                if ((Test-Path $Destination) -and (Get-Item $Destination).Length -eq $Entry.Length) { continue }
                New-Item -ItemType Directory -Force (Split-Path $Destination) | Out-Null
                [IO.Compression.ZipFileExtensions]::ExtractToFile($Entry, $Destination, $true)
            }
        }
    }
    finally {
        $Zip.Dispose()
    }
}

# Administrative extraction unpacks an MSI without installing or registering it.
Write-Host 'Unpacking Windows SDK'
$SdkCache = Join-Path $Cache "sdk-$Sdk"
$SdkTarget = Join-Path $Cache "sdk-$Sdk-unpacked"
foreach ($Payload in $SdkMsis) {
    $Msi = Get-Payload $Payload $SdkCache
    $Text = [Text.Encoding]::Latin1.GetString([IO.File]::ReadAllBytes($Msi))
    $Cabs = [regex]::Matches($Text, '[0-9a-f]{32}\.cab') | ForEach-Object Value | Sort-Object -Unique
    foreach ($Cab in $Cabs) {
        $CabPayload = $SdkPayloads | Where-Object { (Split-Path $_.fileName -Leaf) -eq $Cab } | Select-Object -First 1
        if ($CabPayload) { Get-Payload $CabPayload $SdkCache | Out-Null }
    }
    $Process = Start-Process msiexec.exe -Wait -PassThru -ArgumentList "/a `"$Msi`" /quiet /qn TARGETDIR=`"$SdkTarget`""
    if ($Process.ExitCode -ne 0) { throw "msiexec failed for $Msi with exit code $($Process.ExitCode)." }
}

# The toolchain file needs a path without spaces, so drop "Windows Kits\10".
$WinSdk = Join-Path $Root 'winsdk'
if (Test-Path $WinSdk) { Remove-Item -Recurse -Force $WinSdk }
Move-Item (Join-Path $SdkTarget 'Windows Kits\10') $WinSdk
Remove-Item -Recurse -Force $SdkTarget

# Unreal's build rules insist on mscoree from the .NET Framework SDK, even for what they do not compile.
Write-Host 'Unpacking .NET Framework SDK'
$NetFxCache = Join-Path $Cache 'netfxsdk-4.6.2'
$NetFxTarget = "$NetFxCache-unpacked"
$NetFxPaths = foreach ($Payload in $Packages['microsoft.net.4.6.2.sdk'].payloads) { Get-Payload $Payload $NetFxCache }
$NetFxMsi = $NetFxPaths | Where-Object { $_ -like '*.msi' }
$Process = Start-Process msiexec.exe -Wait -PassThru -ArgumentList "/a `"$NetFxMsi`" /quiet /qn TARGETDIR=`"$NetFxTarget`""
if ($Process.ExitCode -ne 0) { throw "msiexec failed for $NetFxMsi with exit code $($Process.ExitCode)." }

$NetFx = Join-Path $Root 'netfxsdk'
if (Test-Path $NetFx) { Remove-Item -Recurse -Force $NetFx }
New-Item -ItemType Directory -Force $NetFx | Out-Null
Move-Item (Join-Path $NetFxTarget 'Program Files\Windows Kits\NETFXSDK\4.6.2') (Join-Path $NetFx '4.6.2')
Remove-Item -Recurse -Force $NetFxTarget

Write-Host "Done. Toolchain is in $Root"
