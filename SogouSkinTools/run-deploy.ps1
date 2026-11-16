$deployScript = Join-Path $PSScriptRoot "deploy-installed.ps1"
$p = Start-Process -FilePath powershell.exe `
    -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$deployScript`"") `
    -Verb RunAs -Wait -PassThru
Write-Output ("ExitCode=" + $p.ExitCode)
