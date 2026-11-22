<#
.SYNOPSIS
    Checks that every synced asset matches its JSON file, by running the JsonBlueprintSync commandlet with -verify.

.DESCRIPTION
    Starts the editor headless (about a minute) and prints what is out of sync: a JSON that differs from its asset, a missing
    JSON, or a JSON with no asset. Exits with 0 if everything matches and 1 otherwise, so it can gate a push or a build.

    Run it before you push. The editor binaries must be built; the editor itself may be open, but unsaved changes are not
    checked (the commandlet reads the saved assets).

.PARAMETER Project
    The .uproject. Default: the one in the project this plugin is in (three folders up from this script).

.PARAMETER EngineDir
    The engine folder (the one that contains Engine\). Default: $env:UE_ENGINE_DIR, else the engine the .uproject is
    associated with, from the registry.

.PARAMETER Dir
    Only check assets under this content path, e.g. /Game/Features. Default: /Game.

.PARAMETER JsonDir
    Overrides the JsonDir setting.

.PARAMETER FullLog
    Print the whole editor log, not only the plugin's lines.

.EXAMPLE
    .\Plugins\JsonBlueprintSync\Scripts\Verify.ps1
#>
[CmdletBinding()]
param(
    [string]$Project,
    [string]$EngineDir,
    [string]$Dir,
    [string]$JsonDir,
    [switch]$FullLog
)

$ErrorActionPreference = 'Stop'

if (-not $Project) {
    $ProjectDir = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
    $Found = @(Get-ChildItem -Path $ProjectDir -Filter '*.uproject' -File)
    if ($Found.Count -ne 1) {
        Write-Error "Cannot find exactly one .uproject in $ProjectDir. Pass -Project."
    }
    $Project = $Found[0].FullName
}
$Project = (Resolve-Path $Project).Path

if (-not $EngineDir) {
    $EngineDir = $env:UE_ENGINE_DIR
}
if (-not $EngineDir) {
    $Association = (Get-Content -Raw -Path $Project | ConvertFrom-Json).EngineAssociation
    if ($Association) {
        # An installed (Launcher) engine is registered by version, a source build by GUID.
        $Installed = "HKLM:\SOFTWARE\EpicGames\Unreal Engine\$Association"
        $Builds = 'HKCU:\SOFTWARE\Epic Games\Unreal Engine\Builds'
        if (Test-Path $Installed) {
            $EngineDir = (Get-ItemProperty -Path $Installed).InstalledDirectory
        }
        elseif ((Test-Path $Builds) -and (Get-ItemProperty -Path $Builds).PSObject.Properties[$Association]) {
            $EngineDir = (Get-ItemProperty -Path $Builds).$Association
        }
    }
}
if (-not $EngineDir) {
    Write-Error 'Cannot find the engine. Pass -EngineDir or set UE_ENGINE_DIR.'
}

$Editor = Join-Path $EngineDir 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
if (-not (Test-Path $Editor)) {
    Write-Error "Cannot find $Editor."
}

$Arguments = @($Project, '-run=JsonBlueprintSync', '-verify', '-unattended', '-nullrhi', '-nosplash', '-nop4')
if ($Dir) { $Arguments += "-dir=$Dir" }
if ($JsonDir) { $Arguments += "-jsondir=$JsonDir" }

Write-Host "Verifying JSON of $Project (takes about a minute)..."
& $Editor @Arguments | ForEach-Object {
    if ($FullLog -or $_ -match 'LogJsonBlueprintSync') {
        $_
    }
}
$Code = $LASTEXITCODE

if ($Code -eq 0) {
    Write-Host 'Every synced asset matches its JSON.' -ForegroundColor Green
}
else {
    Write-Host ("JSON and assets differ (exit code $Code). Open the listed assets in the editor and save them, " +
        'or apply their JSON in Window > JSON Sync, then run this again. Use -FullLog if no asset is listed.') -ForegroundColor Red
}
exit $Code
