param(
    [ValidateSet("0", "1")]
    [string] $StatusBar,
    [ValidateSet("0", "1")]
    [string] $Horizontal,
    [ValidateSet("0", "1")]
    [string] $KeyRepeat,
    [string] $StatusBarHide = ""
)

$ErrorActionPreference = "Stop"
$dir = Join-Path $env:APPDATA "Moqi"
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$path = Join-Path $dir "sogou_skin_options.ini"

$currentStatusBar = "1"
$currentHorizontal = "0"
$currentKeyRepeat = "1"
$currentStatusBarHide = ""
if (Test-Path -LiteralPath $path) {
    foreach ($line in Get-Content -LiteralPath $path) {
        if ($line -match '^\s*statusbar\s*=\s*([01])') { $currentStatusBar = $Matches[1] }
        if ($line -match '^\s*horizontal\s*=\s*([01])') { $currentHorizontal = $Matches[1] }
        if ($line -match '^\s*key_repeat\s*=\s*([01])') { $currentKeyRepeat = $Matches[1] }
        if ($line -match '^\s*statusbar_hide\s*=\s*(.*)$') { $currentStatusBarHide = $Matches[1].Trim() }
    }
}
if ($PSBoundParameters.ContainsKey("StatusBar")) { $currentStatusBar = $StatusBar }
if ($PSBoundParameters.ContainsKey("Horizontal")) { $currentHorizontal = $Horizontal }
if ($PSBoundParameters.ContainsKey("KeyRepeat")) { $currentKeyRepeat = $KeyRepeat }
if ($PSBoundParameters.ContainsKey("StatusBarHide")) { $currentStatusBarHide = $StatusBarHide }

@"
[Options]
statusbar=$currentStatusBar
horizontal=$currentHorizontal
key_repeat=$currentKeyRepeat
statusbar_hide=$currentStatusBarHide
"@ | Set-Content -LiteralPath $path -Encoding ASCII

Write-Host "Wrote $path"
Write-Host "statusbar=$currentStatusBar horizontal=$currentHorizontal key_repeat=$currentKeyRepeat"
Write-Host "Focus another app (or start a new one) to apply status bar visibility; horizontal applies to the next candidate draw."
