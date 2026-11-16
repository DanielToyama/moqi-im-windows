param(
    [Parameter(Mandatory = $true)]
    [string] $SkinPath
)

$ErrorActionPreference = "Stop"
$skin = (Resolve-Path -LiteralPath $SkinPath).Path
$moqi = Join-Path $env:APPDATA "Moqi"
New-Item -ItemType Directory -Force -Path $moqi | Out-Null

if (Test-Path -LiteralPath $skin -PathType Container) {
    $target = Join-Path $moqi "sogou_skin"
    if (Test-Path -LiteralPath $target) {
        Remove-Item -LiteralPath $target -Recurse -Force
    }
    Copy-Item -LiteralPath $skin -Destination $target -Recurse
    Write-Host "Installed extracted skin to $target"
} else {
    $target = Join-Path $moqi "sogou_skin.ssf"
    Copy-Item -LiteralPath $skin -Destination $target -Force
    Write-Host "Installed skin archive to $target"
}

Write-Host "Restart Moqi (or restart the text service) to pick up the skin."
