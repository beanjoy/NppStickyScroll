param(
    [string]$Generator = 'Visual Studio 17 2022',
    [switch]$HostTests
)
$ErrorActionPreference = 'Stop'
$buildDir = if ($HostTests) { 'build-host' } else { 'build' }
$hostOption = if ($HostTests) { 'ON' } else { 'OFF' }
Push-Location $PSScriptRoot
try {
    cmake -S . -B $buildDir -G $Generator -A x64 "-DSTICKY_HOST_TESTS=$hostOption"
    if ($LASTEXITCODE -ne 0) { throw 'CMake 配置失败' }
    cmake --build $buildDir --config Release --parallel
    if ($LASTEXITCODE -ne 0) { throw '编译失败' }
    ctest --test-dir $buildDir -C Release --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw '测试失败' }
    if (-not $HostTests) {
        New-Item -ItemType Directory -Force -Path dist | Out-Null
        Copy-Item -LiteralPath "$buildDir/Release/NppStickyScroll.dll" -Destination dist/NppStickyScroll.dll
        Get-FileHash -Algorithm SHA256 -LiteralPath dist/NppStickyScroll.dll
    }
} finally { Pop-Location }
