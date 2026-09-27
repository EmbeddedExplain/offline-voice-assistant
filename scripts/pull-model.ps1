# Download an Ollama model, then put ollama back on the isolated network.
# Usage: .\scripts\pull-model.ps1 [model]    (default: LLM_MODEL from .env, else llama3.2:3b)
param([string]$Model)

# Not "Stop": Windows PowerShell 5.1 treats docker's progress output on stderr as an error.
# Check $LASTEXITCODE after each native command instead.
Set-Location (Split-Path $PSScriptRoot -Parent)

function Invoke-Checked([scriptblock]$Cmd) {
    & $Cmd
    if ($LASTEXITCODE -ne 0) { throw "Command failed (exit $LASTEXITCODE): $Cmd" }
}

if (-not $Model) {
    $line = if (Test-Path .env) { Select-String -Path .env -Pattern '^LLM_MODEL=(.+)$' | Select-Object -First 1 } else { $null }
    $Model = if ($line) { $line.Matches[0].Groups[1].Value.Trim() } else { "llama3.2:3b" }
}

Write-Host "==> Starting ollama WITH Internet access to pull $Model"
Invoke-Checked { docker compose -f docker-compose.yml -f compose.pull.yml up -d ollama }
Invoke-Checked { docker compose exec ollama ollama pull $Model }

Write-Host "==> Restarting ollama on the internal (offline) network only"
Invoke-Checked { docker compose up -d --force-recreate ollama }
Invoke-Checked { docker compose exec ollama ollama list }
