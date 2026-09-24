param(
    [Parameter(Mandatory=$true)][string]$Log,
    [Parameter(Mandatory=$true)][string]$Reference,
    [int]$ExpectedFrames = 600
)
$ErrorActionPreference = 'Stop'
$actual = @(Get-Content -LiteralPath $Log | ForEach-Object {
    if ($_ -match '^FRAME (\d+) (\d+) ([0-9a-f]{32})$') {
        [pscustomobject]@{ Index = [int]$Matches[1]; Hash = $Matches[3] }
    }
})
$expected = @(Get-Content -LiteralPath $Reference | ForEach-Object {
    if ($_ -match '^\s*0,.*?,\s*3110400,\s*([0-9a-f]{32})\s*$') { $Matches[1] }
})
if ($ExpectedFrames -lt 1 -or $actual.Count -ne $ExpectedFrames -or $expected.Count -lt $ExpectedFrames) {
    throw "Frame count mismatch: actual=$($actual.Count), reference=$($expected.Count), required=$ExpectedFrames"
}
for ($i = 0; $i -lt $ExpectedFrames; $i++) {
    if ($actual[$i].Index -ne $i -or $actual[$i].Hash -ne $expected[$i]) {
        throw "Frame $i mismatch: actual=$($actual[$i].Hash), reference=$($expected[$i])"
    }
}
"PASS: $ExpectedFrames production-path 1920x1080 NV12 frames match the software reference; unique hashes=$(@($actual.Hash | Sort-Object -Unique).Count)"
