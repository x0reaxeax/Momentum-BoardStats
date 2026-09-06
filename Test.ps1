param([ValidateSet('Debug','Release')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
foreach ($test in @('CoreTests','LifecycleTests','RendererTests')) {
    & (Join-Path $PSScriptRoot "bin/x64/$Configuration/$test.exe")
    if ($LASTEXITCODE -ne 0) { throw "${test} failed: $LASTEXITCODE" }
}
Write-Host 'All three tests passed.'
