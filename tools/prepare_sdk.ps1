[CmdletBinding()]
param(
    [string]$SdkRoot = 'D:\RT-ThreadStudio\repo\Extract\Board_Support_Packages\Infineon\PSOC_E84-EDGI-TALK\1.4.0',
    [string]$ModelRoot
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
$SdkRoot = (Resolve-Path -LiteralPath $SdkRoot).Path
$Descriptor = Join-Path $SdkRoot 'sdk-bsp-psoc_e84-edgi-talk.yaml'
if (!(Test-Path -LiteralPath $Descriptor -PathType Leaf) -or
    !(Select-String -LiteralPath $Descriptor -Pattern '^\s*version:\s*1\.4\.0\s*$' -Quiet)) {
    throw 'Select the installed Edgi-Talk 1.4.0 BSP directory with -SdkRoot.'
}

function Copy-SdkTree([string]$Source, [string]$Destination) {
    $From = Join-Path $SdkRoot $Source
    $To = Join-Path $RepoRoot $Destination
    if (!(Test-Path -LiteralPath $From -PathType Container)) {
        throw "SDK directory is missing: $From"
    }
    & robocopy $From $To /E /XC /XN /XO /XJ /XD .git __pycache__ build Debug /R:1 /W:1 /NFL /NDL /NJH /NJS /NP
    if ($LASTEXITCODE -ge 8) {
        throw "Could not copy $Source (robocopy exit $LASTEXITCODE)."
    }
}

Copy-SdkTree 'libraries' 'libraries'
Copy-SdkTree 'projects/libs' 'libs'
Copy-SdkTree 'rt-thread' 'rt-thread'
Copy-SdkTree 'tools/edgeprotecttools/bin' 'tools/edgeprotecttools/bin'
Copy-SdkTree 'tools/edgeprotecttools/cm33_s_signed_fw' 'tools/edgeprotecttools/cm33_s_signed_fw'

foreach ($Resource in @('55500A1.clm_blob', '55500A1.trxcse', 'cyw55513modpse84som_rev3.txt')) {
    $Relative = "resources/$Resource"
    $Destination = Join-Path $RepoRoot $Relative
    if (!(Test-Path -LiteralPath $Destination)) {
        Copy-Item -LiteralPath (Join-Path $SdkRoot $Relative) -Destination $Destination
    }
}

if ($ModelRoot) {
    $Destination = Join-Path $RepoRoot 'libraries/Common/deepcraft_ai/model'
    $ModelFiles = @('model.c', 'model.h')
    foreach ($File in $ModelFiles) {
        if (!(Test-Path -LiteralPath (Join-Path $ModelRoot $File) -PathType Leaf)) {
            throw "The model directory must contain $File."
        }
    }
    foreach ($File in $ModelFiles) {
        Copy-Item -LiteralPath (Join-Path $ModelRoot $File) -Destination $Destination
    }
}

Write-Host 'Edgi-Talk SDK ready. Run .\scons.bat or .\build_dualcore.ps1 from the project directory.'
