$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$strictUtf8 = [Text.UTF8Encoding]::new($false, $true)
$failed = $false
Push-Location $root
try {
    $files = rg --files --hidden -g '!.git/**' -g '!.tools/**' -g '!build*/**' -g '!dist/**' -g '!docs/*-results.txt' -g '!docs/*.png' -g '!docs/*.bmp'
    foreach ($file in ($files | Sort-Object)) {
        $bytes = [IO.File]::ReadAllBytes((Join-Path $root $file))
        $content = $strictUtf8.GetString($bytes)
        $bom = $bytes.Length -ge 3 -and $bytes[0] -eq 239 -and $bytes[1] -eq 187 -and $bytes[2] -eq 191
        $crlf = [regex]::Matches($content, "`r`n").Count
        $lf = [regex]::Matches($content, "(?<!`r)`n").Count
        $cr = [regex]::Matches($content, "`r(?!`n)").Count
        $mixed = (@($crlf, $lf, $cr | Where-Object { $_ -gt 0 }).Count -gt 1)
        if ($mixed -or ($file -notlike 'vendor*' -and ($bom -or $crlf -or $cr))) { $failed = $true }
        Write-Output "$file : UTF8 BOM=$bom CRLF=$crlf bareLF=$lf bareCR=$cr mixed=$mixed"
    }
} finally { Pop-Location }
if ($failed) { throw '存在不符合预期的文件编码或换行。' }
