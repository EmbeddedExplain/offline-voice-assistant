# Quick test of the gateway without the Core2.
# Usage: .\scripts\ask.ps1 "Why is the sky blue?"
param([string]$Question = "What is an ESP32?", [string]$Gateway = "http://localhost:8000")

$body = @{ question = $Question } | ConvertTo-Json
$res = Invoke-RestMethod -Method Post -Uri "$Gateway/ask" -ContentType "application/json" -Body $body
Write-Host "`n$($res.answer)`n" -ForegroundColor Cyan
$res.timings_ms | Format-List
