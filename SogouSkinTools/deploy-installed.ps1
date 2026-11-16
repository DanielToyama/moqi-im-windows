# Deploy the Sogou-skin-enabled MoqiTextService.dll to an installed Moqi.
# Run elevated. Backs up every replaced file before touching it.
#
# Usage (from the repository root):
#   powershell -ExecutionPolicy Bypass -File .\SogouSkinTools\deploy-installed.ps1
#   powershell -ExecutionPolicy Bypass -File .\SogouSkinTools\deploy-installed.ps1 `
#       -X86Dll .\build-vs32\MoqiTextService\Release\MoqiTextService.dll `
#       -X64Dll .\build-vs64\MoqiTextService\Release\MoqiTextService.dll
#
# The script never downloads anything. It stops MoqiLauncher, replaces the
# four runtime DLL locations, installs SogouSkinSettings.exe, and restarts the
# launcher if it was running.
param(
    [string] $RepoRoot = "",
    [string] $InstallDir = "C:\Program Files (x86)\MoqiIM",
    [string] $Configuration = "Release",
    [string] $X86Dll = "",
    [string] $X64Dll = "",
    [string] $SettingsExe = ""
)

$ErrorActionPreference = "Stop"

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "This script must run from an elevated PowerShell (Run as administrator)."
}

if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = Split-Path -Parent $PSScriptRoot
}
$RepoRoot = [System.IO.Path]::GetFullPath($RepoRoot)

function Find-BuildArtifact {
    param(
        [string] $ExplicitPath,
        [string[]] $Candidates,
        [string] $Label
    )
    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) {
        $full = if ([System.IO.Path]::IsPathRooted($ExplicitPath)) {
            [System.IO.Path]::GetFullPath($ExplicitPath)
        } else {
            [System.IO.Path]::GetFullPath((Join-Path $RepoRoot $ExplicitPath))
        }
        if (Test-Path -LiteralPath $full) { return $full }
        throw "$Label not found: $full"
    }
    foreach ($candidate in $Candidates) {
        $full = [System.IO.Path]::GetFullPath((Join-Path $RepoRoot $candidate))
        if (Test-Path -LiteralPath $full) { return $full }
    }
    throw "$Label not found. Build first or pass -X86Dll/-X64Dll/-SettingsExe. Tried: $($Candidates -join ', ')"
}

$is64Os = [Environment]::Is64BitOperatingSystem
$srcX86 = Find-BuildArtifact $X86Dll @(
    "build-vs32-local\MoqiTextService\$Configuration\MoqiTextService.dll",
    "build-vs32\MoqiTextService\$Configuration\MoqiTextService.dll"
) "x86 MoqiTextService.dll"
$srcX64 = $null
if ($is64Os) {
    $srcX64 = Find-BuildArtifact $X64Dll @(
        "build-vs64-local\MoqiTextService\$Configuration\MoqiTextService.dll",
        "build-vs64\MoqiTextService\$Configuration\MoqiTextService.dll"
    ) "x64 MoqiTextService.dll"
}
$srcSettings = Find-BuildArtifact $SettingsExe @(
    "build-vs32-local\SogouSkinTools\$Configuration\SogouSkinSettings.exe",
    "build-vs32\SogouSkinTools\$Configuration\SogouSkinSettings.exe",
    "build-vs64-local\SogouSkinTools\$Configuration\SogouSkinSettings.exe",
    "build-vs64\SogouSkinTools\$Configuration\SogouSkinSettings.exe"
) "SogouSkinSettings.exe"

$install = [System.IO.Path]::GetFullPath($InstallDir)
$system32 = Join-Path $env:WINDIR "System32\MoqiTextService.dll"
$syswow64 = Join-Path $env:WINDIR "SysWOW64\MoqiTextService.dll"
$logDir = Join-Path $RepoRoot "artifacts"
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$backup = Join-Path $install "sogou-skin-backup-$stamp"
$log = Join-Path $logDir "deploy-$stamp.log"

New-Item -ItemType Directory -Force -Path $logDir | Out-Null
New-Item -ItemType Directory -Force -Path $backup | Out-Null

function Log([string] $message) {
    $line = "$(Get-Date -Format 'HH:mm:ss') $message"
    Add-Content -LiteralPath $log -Value $line -Encoding UTF8
    Write-Host $line
}

function Replace-File([string] $target, [string] $source, [string] $label) {
    if (-not (Test-Path -LiteralPath $source)) {
        throw "source missing: $source"
    }
    if (-not (Test-Path -LiteralPath $target)) {
        throw "target missing: $target"
    }
    $backupTarget = Join-Path $backup ("$label-" + (Split-Path $target -Leaf))
    Copy-Item -LiteralPath $target -Destination $backupTarget -Force
    Log "backed up $label -> $backupTarget"
    $old = "$target.sogou-old-$stamp"
    try {
        Move-Item -LiteralPath $target -Destination $old -Force
        Log "renamed $label to $old"
    } catch {
        throw "cannot rename $label (is it locked?): $_"
    }
    try {
        Copy-Item -LiteralPath $source -Destination $target -Force
        Log "installed $label <- $source"
    } catch {
        Move-Item -LiteralPath $old -Destination $target -Force
        throw "copy failed for $label, restored original: $_"
    }
    Remove-Item -LiteralPath $old -Force -ErrorAction SilentlyContinue
    Log "removed old file $old"
}

try {
    $launcher = Get-Process MoqiLauncher -ErrorAction SilentlyContinue
    $wasRunning = $null -ne $launcher
    if ($wasRunning) {
        $launcher | Stop-Process -Force
        Start-Sleep -Milliseconds 800
        Log "stopped MoqiLauncher"
    }

    Replace-File (Join-Path $install "MoqiTextService.dll") $srcX86 "x86"
    if ($is64Os) {
        Replace-File (Join-Path $install "x64\MoqiTextService.dll") $srcX64 "x64"
        Replace-File $syswow64 $srcX86 "system-x86"
        Replace-File $system32 $srcX64 "system-x64"
    } else {
        Replace-File $system32 $srcX86 "system-x86"
    }

    $settingsTarget = Join-Path $install "SogouSkinSettings.exe"
    if (Test-Path -LiteralPath $settingsTarget) {
        Copy-Item -LiteralPath $settingsTarget -Destination (Join-Path $backup "SogouSkinSettings.exe") -Force
        Log "backed up SogouSkinSettings.exe"
    }
    Copy-Item -LiteralPath $srcSettings -Destination $settingsTarget -Force
    Log "installed native settings UI <- $srcSettings"

    if ($wasRunning) {
        Start-Process -FilePath (Join-Path $install "MoqiLauncher.exe") -WorkingDirectory $install
        Log "restarted MoqiLauncher"
    }
    Log "DONE. Some already-running apps may keep the old DLL until restarted."
} catch {
    Log "FAILED: $_"
    throw
}
