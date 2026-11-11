param([string]$PortableZip)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $PortableZip) { $PortableZip = Join-Path $root '.tools/npp.8.9.1.portable.x64.zip' }
$PortableZip = (Resolve-Path -LiteralPath $PortableZip).Path
$expected = '96D39A00D8264BA652A519063313CF3F9F3A1E6CB99F74C7E9C3CECD9FFAE989'
if ((Get-FileHash -Algorithm SHA256 -LiteralPath $PortableZip).Hash -ne $expected) {
    throw '此测试脚本只接受官方 Notepad++ 8.9.1 x64 便携 ZIP，文件校验失败。'
}
& (Join-Path $root 'build.ps1') -HostTests
$testDir = Join-Path $root ('.tools/host-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testDir | Out-Null
Expand-Archive -LiteralPath $PortableZip -DestinationPath $testDir
$pluginDir = Join-Path $testDir 'plugins/NppStickyScroll'
New-Item -ItemType Directory -Path $pluginDir | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'build-host/Release/NppStickyScroll.dll') -Destination (Join-Path $pluginDir 'NppStickyScroll.dll')
$oldTestDir = $env:NPP_STICKY_TEST_DIR
$env:NPP_STICKY_TEST_DIR = $testDir
try {
    $testProcess = Start-Process -FilePath (Join-Path $testDir 'notepad++.exe') -ArgumentList '-multiInst','-nosession','-noUpdater' -WindowStyle Hidden -PassThru
} finally {
    $env:NPP_STICKY_TEST_DIR = $oldTestDir
}
$result = Join-Path $testDir 'host-results.txt'
$deadline = [DateTime]::UtcNow.AddSeconds(60)
$complete = $false
while ([DateTime]::UtcNow -lt $deadline -and -not $testProcess.HasExited) {
    if (Test-Path -LiteralPath $result) {
        $stream = [IO.FileStream]::new($result, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
        $reader = [IO.StreamReader]::new($stream)
        try { $text = $reader.ReadToEnd() } finally { $reader.Dispose() }
        if ($text -match 'RESULT \d+ checks, (\d+) failures') { $complete = $true; break }
    }
    Start-Sleep -Milliseconds 300
}
if (-not $complete) { throw "宿主测试超时或意外退出。诊断目录：$testDir" }
$failures = [int]$Matches[1]
Write-Output $text
$null = $testProcess.CloseMainWindow()
if (-not $testProcess.WaitForExit(5000)) { Write-Warning "测试窗口尚未退出：$testDir" }
if ($failures -ne 0) { throw "宿主测试有 $failures 项失败。" }
Write-Output "测试结果与绘制快照保存在：$testDir"
