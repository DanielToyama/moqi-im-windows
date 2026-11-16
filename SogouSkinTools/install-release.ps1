#Requires -Version 5.1
<#
.SYNOPSIS
  Install the four release DLLs into an existing Moqi installation.

.DESCRIPTION
  Put the four release DLLs, SogouSkinSettings.exe and this script in one
  folder, then run install.bat (or this script) as administrator.  The script
  stops MoqiLauncher, backs up every replaced file, copies the DLLs to the
  four TSF load locations, installs the settings tool and restarts MoqiLauncher.

.PARAMETER SourceDir
  Folder containing the release files. Defaults to this script's folder.

.PARAMETER InstallDir
  Moqi installation directory. Defaults to C:\Program Files (x86)\MoqiIM.

.PARAMETER NoPause
  Do not wait for Enter at the end. Used by install.bat.
#>
param(
  [string] $SourceDir = "",
  [string] $InstallDir = "C:\Program Files (x86)\MoqiIM",
  [switch] $NoPause
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($SourceDir)) {
  $SourceDir = $PSScriptRoot
}
$SourceDir = [System.IO.Path]::GetFullPath($SourceDir)
$InstallDir = [System.IO.Path]::GetFullPath($InstallDir)

function Test-Administrator {
  $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
  $principal = New-Object Security.Principal.WindowsPrincipal($identity)
  return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-Administrator)) {
  $argLine = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -SourceDir `"$SourceDir`" -InstallDir `"$InstallDir`""
  if ($NoPause) {
    $argLine += " -NoPause"
  }
  $elevated = Start-Process -FilePath "powershell.exe" -Verb RunAs -Wait -PassThru -ArgumentList $argLine
  exit $elevated.ExitCode
}

$is64 = [Environment]::Is64BitOperatingSystem
$system32 = Join-Path $env:WINDIR "System32\MoqiTextService.dll"
$syswow64 = Join-Path $env:WINDIR "SysWOW64\MoqiTextService.dll"
$settingsSource = Join-Path $SourceDir "SogouSkinSettings.exe"
$settingsTarget = Join-Path $InstallDir "SogouSkinSettings.exe"

$files = @()
if ($is64) {
  $files += @{ Source = "MoqiTextService-ProgramFiles-x86.dll"; Target = (Join-Path $InstallDir "MoqiTextService.dll"); Label = "ProgramFiles-x86" }
  $files += @{ Source = "MoqiTextService-ProgramFiles-x64.dll"; Target = (Join-Path $InstallDir "x64\MoqiTextService.dll"); Label = "ProgramFiles-x64" }
  $files += @{ Source = "MoqiTextService-System32.dll"; Target = $system32; Label = "System32" }
  $files += @{ Source = "MoqiTextService-SysWOW64.dll"; Target = $syswow64; Label = "SysWOW64" }
} else {
  $files += @{ Source = "MoqiTextService-ProgramFiles-x86.dll"; Target = (Join-Path $InstallDir "MoqiTextService.dll"); Label = "ProgramFiles-x86" }
  $files += @{ Source = "MoqiTextService-ProgramFiles-x86.dll"; Target = $system32; Label = "System32" }
}

foreach ($item in $files) {
  $path = Join-Path $SourceDir $item.Source
  if (-not (Test-Path -LiteralPath $path)) {
    throw "缺少文件：$($item.Source)。请把 Release 里的四个 DLL 和 install.bat/install.ps1 放到同一个文件夹。"
  }
}

if (-not (Test-Path -LiteralPath (Join-Path $InstallDir "MoqiLauncher.exe"))) {
  throw "没有找到 MoqiLauncher.exe。请先安装官方墨奇输入法，或使用 -InstallDir 指定安装目录。"
}

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$backupDir = Join-Path $InstallDir "sogou-skin-backup-$stamp"
New-Item -ItemType Directory -Path $backupDir -Force | Out-Null

$launcher = Get-Process MoqiLauncher -ErrorAction SilentlyContinue
$wasRunning = $null -ne $launcher
if ($wasRunning) {
  Write-Host "正在停止 MoqiLauncher..."
  $launcher | Stop-Process -Force
  Start-Sleep -Milliseconds 900
}

function Backup-AndReplace {
  param(
    [string] $Source,
    [string] $Target,
    [string] $Label
  )
  $parent = Split-Path -Parent $Target
  if (-not (Test-Path -LiteralPath $parent)) {
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
  }
  if (Test-Path -LiteralPath $Target) {
    $backupPath = Join-Path $backupDir ("$Label-" + (Split-Path $Target -Leaf))
    Copy-Item -LiteralPath $Target -Destination $backupPath -Force
  }
  $oldPath = "$Target.sogou-old-$stamp"
  if (Test-Path -LiteralPath $Target) {
    Move-Item -LiteralPath $Target -Destination $oldPath -Force
  }
  Copy-Item -LiteralPath $Source -Destination $Target -Force
  if (Test-Path -LiteralPath $oldPath) {
    Remove-Item -LiteralPath $oldPath -Force
  }
  Write-Host "已替换：$Target"
}

Write-Host "安装目录：$InstallDir"
Write-Host "备份目录：$backupDir"
foreach ($item in $files) {
  $source = Join-Path $SourceDir $item.Source
  Backup-AndReplace -Source $source -Target $item.Target -Label $item.Label
}

if (Test-Path -LiteralPath $settingsSource) {
  Backup-AndReplace -Source $settingsSource -Target $settingsTarget -Label "SogouSkinSettings"
} else {
  Write-Host "未找到 SogouSkinSettings.exe，跳过设置程序（不影响输入法运行）。"
}

if ($wasRunning) {
  $launcherPath = Join-Path $InstallDir "MoqiLauncher.exe"
  if (Test-Path -LiteralPath $launcherPath) {
    Start-Process -FilePath $launcherPath -WorkingDirectory $InstallDir
    Write-Host "已重新启动 MoqiLauncher。"
  }
}

Write-Host ""
Write-Host "安装完成。请关闭并重新打开正在使用输入法的应用（浏览器、微信、编辑器等）。"
Write-Host "不需要重启系统。"

if (-not $NoPause) {
  Read-Host "按 Enter 退出" | Out-Null
}
