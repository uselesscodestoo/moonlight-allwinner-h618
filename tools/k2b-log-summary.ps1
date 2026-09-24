param([Parameter(Mandatory=$true)][string]$Log)
$ErrorActionPreference = 'Stop'
$lines = Get-Content -LiteralPath $Log
$samples = @($lines | ForEach-Object {
    if ($_ -match 'K2B: received=') {
        $fields = @{}
        foreach ($match in [regex]::Matches($_, '([a-z_]+)=(\d+(?:\.\d+)?)')) {
            $fields[$match.Groups[1].Value] = [double]::Parse($match.Groups[2].Value, [cultureinfo]::InvariantCulture)
        }
        if (-not $fields.ContainsKey('elapsed_ms')) { throw 'Log lacks measured elapsed_ms; do not assume five-second intervals.' }
        $fields
    }
})
if ($samples.Count -lt 2) { throw 'Need at least two timestamped statistics samples.' }
$intervals = @(for ($i = 1; $i -lt $samples.Count; $i++) {
    $a = $samples[$i - 1]; $b = $samples[$i]
    $seconds = ($b.elapsed_ms - $a.elapsed_ms) / 1000
    if ($seconds -le 0 -or $b.decoded -lt $a.decoded) { throw 'Counters reset or timestamps did not increase; split separate runs.' }
    [pscustomobject]@{
        StartMs = $a.elapsed_ms; EndMs = $b.elapsed_ms
        ReceivedFPS = [math]::Round(($b.received - $a.received) / $seconds, 3)
        DecodedFPS = [math]::Round(($b.decoded - $a.decoded) / $seconds, 3)
        DisplaySubmittedFPS = [math]::Round(($b.display_submitted - $a.display_submitted) / $seconds, 3)
        Recoveries = $b.recoveries - $a.recoveries
        QueueDiscarded = $b.discarded - $a.discarded
    }
})
$first = $samples[0]; $last = $samples[-1]
$duration = ($last.elapsed_ms - $first.elapsed_ms) / 1000
[pscustomobject]@{
    Evidence = 'Software counters only; not measured HDMI presentation FPS. First interval starts at the first logged sample.'
    WindowSeconds = $duration
    WindowDecoded = $last.decoded - $first.decoded
    WindowDecodedFPS = [math]::Round(($last.decoded - $first.decoded) / $duration, 3)
    TotalReceived = $last.received; TotalDecoded = $last.decoded
    TotalDisplaySubmitted = $last.display_submitted
    TotalRecoveries = $last.recoveries; TotalQueueDiscarded = $last.discarded
    ReportedNetworkDropEvents = @($lines | Select-String 'Network dropped \d+ frame').Count
    Intervals = $intervals
} | ConvertTo-Json -Depth 4
