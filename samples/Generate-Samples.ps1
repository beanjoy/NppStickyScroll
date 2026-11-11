param([switch]$Large)
$ErrorActionPreference = 'Stop'
$utf8 = [System.Text.UTF8Encoding]::new($false)
$json = [System.Collections.Generic.List[string]]::new()
$json.Add('{')
$json.Add('  "DeviceLog": {')
$json.Add('    "Windows": [')
$xml = [System.Collections.Generic.List[string]]::new()
$xml.Add('<?xml version="1.0" encoding="UTF-8"?>')
$xml.Add('<DeviceLog>')
$xml.Add('  <Windows>')
for ($i = 0; $i -lt 100; ++$i) {
    $json.Add('      {')
    $json.Add(('        "Index": {0},' -f $i))
    $json.Add('        "Details": {')
    $json.Add('          "Name": "示例设备",')
    $json.Add('          "Samples": [')
    $json.Add('            10,')
    $json.Add('            20,')
    $json.Add('            30')
    $json.Add('          ]')
    $json.Add('        }')
    $json.Add($(if ($i -eq 99) { '      }' } else { '      },' }))
    $xml.Add(('    <Window index="{0}">' -f $i))
    $xml.Add('      <Details>')
    $xml.Add('        <Name>示例设备</Name>')
    $xml.Add('        <Samples>')
    $xml.Add('          <Value>10</Value>')
    $xml.Add('          <Value>20</Value>')
    $xml.Add('          <Value>30</Value>')
    $xml.Add('        </Samples>')
    $xml.Add('      </Details>')
    $xml.Add('    </Window>')
}
$json.Add('    ],')
$json.Add('    "NextSection": {')
$json.Add('      "Done": true')
$json.Add('    }')
$json.Add('  }')
$json.Add('}')
$xml.Add('  </Windows>')
$xml.Add('  <NextSection>')
$xml.Add('    <Done>true</Done>')
$xml.Add('  </NextSection>')
$xml.Add('</DeviceLog>')
[IO.File]::WriteAllText((Join-Path $PSScriptRoot 'DeviceLog.json'), (($json -join "`n") + "`n"), $utf8)
[IO.File]::WriteAllText((Join-Path $PSScriptRoot 'DeviceLog.xml'), (($xml -join "`n") + "`n"), $utf8)
$deep = [System.Collections.Generic.List[string]]::new()
$deep.Add('{')
for ($i = 1; $i -le 10; ++$i) { $deep.Add(('  ' * $i) + ('"Level{0}": {{' -f $i)) }
for ($i = 0; $i -lt 60; ++$i) { $deep.Add(('  ' * 11) + ('"value{0}": {0},' -f $i)) }
$deep.Add(('  ' * 11) + '"last": true')
for ($i = 10; $i -ge 0; --$i) { $deep.Add(('  ' * $i) + '}') }
[IO.File]::WriteAllText((Join-Path $PSScriptRoot 'Deep.json'), (($deep -join "`n") + "`n"), $utf8)
$longKey = 'LongParent_' + ('x' * 6000)
$wrapped = "{`n  `"$longKey`": {`n"
for ($i = 0; $i -lt 80; ++$i) { $wrapped += ('    "item{0}": "{1}",' -f $i, ('abcdefghij' * 30)) + "`n" }
$wrapped += "    `"last`": true`n  }`n}`n"
[IO.File]::WriteAllText((Join-Path $PSScriptRoot 'Wrapped.json'), $wrapped, $utf8)
[IO.File]::WriteAllText((Join-Path $PSScriptRoot 'Empty.json'), '', $utf8)
if ($Large) {
    $largePath = Join-Path $PSScriptRoot 'Large.json'
    $writer = [IO.StreamWriter]::new($largePath, $false, $utf8)
    $writer.NewLine = "`n"
    try {
        $writer.WriteLine('{')
        $writer.WriteLine('  "DeviceLog": {')
        $writer.WriteLine('    "Windows": [')
        for ($i = 0; $i -lt 150000; ++$i) { $writer.WriteLine(('      {"Index":' + $i + ',"Value":42},')) }
        $writer.WriteLine('      {"Last":true}')
        $writer.WriteLine('    ]')
        $writer.WriteLine('  }')
        $writer.WriteLine('}')
    } finally { $writer.Dispose() }
}
