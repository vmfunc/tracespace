# Runs the Neovim plugin test on Windows.
#
#   powershell -File tests\nvim\run-windows.ps1 [-Binary build\tracespace.exe]
#
# Neovim is started in its own (hidden) console and reports through a log file:
# when Neovim's stdout is a pipe, terminal jobs write to that pipe instead of the
# terminal buffer, and the test could not read the diagram.
param(
    [string]$Binary = (Join-Path $PSScriptRoot "..\..\build\tracespace.exe")
)

$repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$log = Join-Path $env:TEMP "tracespace-plugin-test.log"
if (Test-Path $log) { Remove-Item $log -Confirm:$false }

$env:TRACESPACE_TEST_LOG = $log
$env:TRACESPACE_BIN = (Resolve-Path $Binary).Path -replace '\\', '/'

$nvimArgs = '--headless', '--clean', '-c', '"set rtp+=nvim"', '-c', '"luafile tests/nvim/plugin_test.lua"'
$p = Start-Process -FilePath "nvim" -ArgumentList $nvimArgs -WorkingDirectory $repo -WindowStyle Hidden -PassThru
if (-not $p.WaitForExit(180000)) {
    $p.Kill()
    Write-Output "timed out"
    exit 1
}
if (Test-Path $log) { Get-Content $log -Encoding UTF8 }
exit $p.ExitCode
