param(
  [string]$BaseUrl = "http://opentaptox-esp32c6.local",
  [string]$OutputDir = "data\captures\live",
  [string]$Prefix = "",
  [double]$IntervalSeconds = 2.0,
  [int]$StatusIntervalSeconds = 15,
  [switch]$Clear,
  [switch]$Passive
)

$ErrorActionPreference = "Continue"

if ([string]::IsNullOrWhiteSpace($Prefix)) {
  $Prefix = "tigo_support_delta_" + (Get-Date -Format "yyyy-MM-dd_HH-mm-ss")
}

if (-not [System.IO.Path]::IsPathRooted($OutputDir)) {
  $OutputDir = Join-Path (Get-Location) $OutputDir
}
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null

$basePath = Join-Path $OutputDir $Prefix
$metaFile = "$basePath.meta.json"
$framesFile = "$basePath.interesting.jsonl"
$statusFile = "$basePath.status.jsonl"
$stopFile = "$basePath.stop"
$start = Get-Date
$sinceSeq = 0
$sequenceEpoch = 0
$lastFrameMs = $null
$lastControllerUptimeMs = $null
$forcedResetReason = ""
$lastStatus = Get-Date "2000-01-01T00:00:00"

function Get-ApiJson {
  param([string]$Path)
  try {
    return Invoke-RestMethod -Uri ($BaseUrl.TrimEnd("/") + $Path) -TimeoutSec 5
  } catch {
    return [ordered]@{ error = $_.Exception.Message; path = $Path }
  }
}

function Write-JsonLine {
  param([string]$Path, [string]$Kind, $Data)
  $now = Get-Date
  $line = [ordered]@{
    ts = $now.ToString("o")
    elapsed_s = [int](($now - $start).TotalSeconds)
    capture_epoch = $sequenceEpoch
    kind = $Kind
    data = $Data
  }
  Add-Content -Encoding UTF8 -Path $Path -Value ($line | ConvertTo-Json -Depth 40 -Compress)
}

if ($Passive) {
  Get-ApiJson "/api/polling/set?enabled=0" | Out-Null
}
if ($Clear) {
  Get-ApiJson "/api/interesting-frames/clear" | Out-Null
}

[ordered]@{
  started_at = $start.ToString("o")
  base_url = $BaseUrl
  frames_file = $framesFile
  status_file = $statusFile
  stop_file = $stopFile
  clear_requested = [bool]$Clear
  passive_requested = [bool]$Passive
  note = "Polls /api/interesting-frames and records only non-standard TAP/CCA frames."
} | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 $metaFile

Write-Host "Interesting-frame logger started"
Write-Host "  Frames: $framesFile"
Write-Host "  Status: $statusFile"
Write-Host "  Stop:   $stopFile"

while (-not (Test-Path -LiteralPath $stopFile)) {
  $now = Get-Date
  if (($now - $lastStatus).TotalSeconds -ge $StatusIntervalSeconds) {
    $status = Get-ApiJson "/api/status"
    Write-JsonLine $statusFile "status" $status
    if (-not $status.error -and $null -ne $status.uptime_ms) {
      $uptimeMs = [uint64]$status.uptime_ms
      if ($null -ne $lastControllerUptimeMs -and $uptimeMs -lt [uint64]$lastControllerUptimeMs) {
        $forcedResetReason = "ESP uptime moved backwards"
      }
      $lastControllerUptimeMs = $uptimeMs
    }
    $lastStatus = $now
  }

  $batch = Get-ApiJson ("/api/interesting-frames?since_seq={0}" -f $sinceSeq)
  $frameClockMovedBack = $false
  if (-not $batch.error -and $batch.frames -and $null -ne $lastFrameMs) {
    foreach ($candidate in $batch.frames) {
      if ($null -ne $candidate.ms -and [uint64]$candidate.ms -lt [uint64]$lastFrameMs) {
        $frameClockMovedBack = $true
        break
      }
    }
  }
  $headMovedBack = -not $batch.error -and $null -ne $batch.head_seq -and
                   [uint64]$batch.head_seq -lt [uint64]$sinceSeq
  if ($headMovedBack -or $frameClockMovedBack -or -not [string]::IsNullOrEmpty($forcedResetReason)) {
    $previousSeq = $sinceSeq
    $resetReason = if (-not [string]::IsNullOrEmpty($forcedResetReason)) {
      $forcedResetReason
    } elseif ($headMovedBack) {
      "ESP interesting-frame sequence moved backwards"
    } else {
      "ESP frame uptime moved backwards"
    }
    $sequenceEpoch++
    $sinceSeq = 0
    $lastFrameMs = $null
    $forcedResetReason = ""
    Write-JsonLine $statusFile "sequence_reset" ([ordered]@{
      previous_seq = [uint64]$previousSeq
      new_head_seq = [uint64]$batch.head_seq
      capture_epoch = $sequenceEpoch
      reason = "$resetReason; refetching current ring"
    })
    $batch = Get-ApiJson "/api/interesting-frames?since_seq=0"
  }
  if ($batch.frames) {
    foreach ($frame in $batch.frames) {
      Write-JsonLine $framesFile "interesting_frame" $frame
      if ($frame.seq -gt $sinceSeq) {
        $sinceSeq = [uint32]$frame.seq
      }
      if ($null -ne $frame.ms) {
        $lastFrameMs = [uint64]$frame.ms
      }
    }
  } elseif ($batch.error) {
    Write-JsonLine $statusFile "api_error" $batch
  }

  Start-Sleep -Milliseconds ([int]($IntervalSeconds * 1000))
}

$end = Get-Date
[ordered]@{
  stopped_at = $end.ToString("o")
  duration_s = [int](($end - $start).TotalSeconds)
  last_seq = $sinceSeq
  capture_epoch = $sequenceEpoch
} | ConvertTo-Json -Compress | Add-Content -Encoding UTF8 $metaFile
