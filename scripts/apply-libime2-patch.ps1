#Requires -Version 5.1
<#
.SYNOPSIS
  Apply the external-preedit display-attribute fix to the libIME2 submodule.

.DESCRIPTION
  This repository vendors a small patch for libIME2's TextService.cpp:
  clear TF_ATTR_INPUT in external-preedit mode and set a zero-length caret
  after commit.  It prevents the blue text background seen in Windows Search
  and Task Manager after committing Chinese text.

  The script is idempotent: if the fix is already present (for example after
  switching libIME2 to a fork that contains it), it exits successfully without
  modifying anything.

.PARAMETER RepoRoot
  Repository root. Defaults to the parent directory of this script.
#>
param(
  [string] $RepoRoot = ""
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
  $RepoRoot = Split-Path -Parent $PSScriptRoot
}
$RepoRoot = [System.IO.Path]::GetFullPath($RepoRoot)

$libimeRoot = Join-Path $RepoRoot "libIME2"
$sourceFile = Join-Path $libimeRoot "src\TextService.cpp"
$patchFile = Join-Path $RepoRoot "patches\libIME2-external-preedit-attr.patch"

if (-not (Test-Path -LiteralPath $sourceFile)) {
  throw "libIME2 submodule is missing: $sourceFile. Run 'git submodule update --init --recursive' first."
}
if (-not (Test-Path -LiteralPath $patchFile)) {
  throw "Patch file is missing: $patchFile"
}

$marker = "post-clear input attribute"
if (Select-String -LiteralPath $sourceFile -Pattern $marker -Quiet) {
  Write-Host "[libIME2] external-preedit attribute fix already present; skipping patch."
  return
}

Write-Host "[libIME2] applying $patchFile"
& git -C $libimeRoot apply --whitespace=nowarn $patchFile
if ($LASTEXITCODE -ne 0) {
  throw "git apply failed with exit code $LASTEXITCODE. Resolve libIME2 changes manually."
}

Write-Host "[libIME2] patch applied."
