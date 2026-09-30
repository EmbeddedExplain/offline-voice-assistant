# Episode 3: measure cold and warm latency for several models, then print mean +/- standard deviation.
#
# Usage: .\scripts\benchmark.ps1 [-Models llama3.2:1b,llama3.2:3b] [-ColdRuns 5] [-WarmRuns 5] [-Wav captures\question.wav]
#
# "Cold" = ollama restarted before the request, so the model is loaded from disk into VRAM.
# Note: the Docker VM keeps the model file in its page cache, so this is a *warm-disk* cold start.
# For a true cold start (nothing cached) run `wsl --shutdown` first; that is too slow to repeat 5 times.
#
# Raw rows go to data\benchmark-<date>.csv (and, via the gateway, to data\timings.csv).
param(
    [string[]]$Models,
    [int]$ColdRuns = 5,
    [int]$WarmRuns = 5,
    [string]$Question = "What is an ESP32?",
    [string]$Wav,
    [string]$Gateway = "http://localhost:8000"
)

Set-Location (Split-Path $PSScriptRoot -Parent)

if (-not $Models) {
    # Default: every model installed in ollama, smallest first
    $Models = docker compose exec ollama ollama list | Select-Object -Skip 1 | ForEach-Object { ($_ -split '\s+')[0] } | Where-Object { $_ } | Sort-Object
}

function Wait-Ollama {
    foreach ($i in 1..60) {
        docker compose exec ollama ollama list *> $null
        if ($LASTEXITCODE -eq 0) { return }
        Start-Sleep -Milliseconds 500
    }
    throw "ollama did not come back after restart"
}

function Invoke-Ask([string]$Model) {
    $body = @{ question = $Question; model = $Model } | ConvertTo-Json
    (Invoke-RestMethod -Method Post -Uri "$Gateway/ask" -ContentType "application/json" -Body $body).timings_ms
}

$rows = New-Object System.Collections.Generic.List[object]
function Add-Row([string]$Model, [string]$Kind, $t) {
    $tokPerS = 0
    if ($t.llm_gen_ms -gt 0) { $tokPerS = [math]::Round($t.llm_tokens * 1000.0 / $t.llm_gen_ms, 1) }
    $row = [pscustomobject]@{
        model = $Model; kind = $Kind; total_ms = $t.total_ms; upload_ms = $t.upload_ms; stt_ms = $t.stt_ms; llm_ms = $t.llm_ms; tts_ms = $t.tts_ms
        llm_load_ms = $t.llm_load_ms; llm_prompt_ms = $t.llm_prompt_ms; llm_gen_ms = $t.llm_gen_ms
        llm_tokens = $t.llm_tokens; tokens_per_s = $tokPerS
    }
    $rows.Add($row)
    Write-Host ("  {0,-14} {1,-6} total {2,6} ms   load {3,6} ms   {4,5} tok/s" -f $Model, $Kind, $t.total_ms, $t.llm_load_ms, $tokPerS)
}

foreach ($model in $Models) {
    Write-Host "==> $model"
    foreach ($i in 1..$ColdRuns) {
        docker compose restart ollama *> $null
        Wait-Ollama
        Add-Row $model "cold" (Invoke-Ask $model)
    }
    foreach ($i in 1..$WarmRuns) { Add-Row $model "warm" (Invoke-Ask $model) }
}

if ($Wav) {
    Write-Host "==> voice loop (default model), $WarmRuns warm runs"
    foreach ($i in 1..$WarmRuns) {
        $res = Invoke-WebRequest -Method Post -Uri "$Gateway/voice" -ContentType "audio/wav" -InFile $Wav -UseBasicParsing
        Add-Row "voice" "warm" ($res.Headers['X-Timings'] | ConvertFrom-Json)
    }
}

New-Item -ItemType Directory -Force data | Out-Null
$csv = "data\benchmark-$(Get-Date -Format 'yyyyMMdd-HHmmss').csv"
$rows | Export-Csv -NoTypeInformation -Encoding UTF8 $csv

function Stat($values) {
    $v = @($values | Where-Object { $_ -ne $null } | ForEach-Object { [double]$_ })
    if ($v.Count -eq 0) { return "-" }
    $mean = ($v | Measure-Object -Average).Average
    $sd = 0
    if ($v.Count -gt 1) { $sd = [math]::Sqrt((($v | ForEach-Object { ($_ - $mean) * ($_ - $mean) }) | Measure-Object -Sum).Sum / ($v.Count - 1)) }
    "{0:N0} +/- {1:N0}" -f $mean, $sd
}

Write-Host "`nSummary (mean +/- standard deviation, ms):"
$rows | Group-Object model, kind | ForEach-Object {
    $g = $_.Group
    [pscustomobject]@{
        model = $g[0].model; kind = $g[0].kind; n = $g.Count
        total = Stat $g.total_ms; load = Stat $g.llm_load_ms; prompt = Stat $g.llm_prompt_ms
        upload = Stat $g.upload_ms; stt = Stat $g.stt_ms; tts = Stat $g.tts_ms; "tok/s" = Stat $g.tokens_per_s
    }
} | Format-Table -AutoSize | Out-String | Write-Host
Write-Host "Raw data: $csv"
