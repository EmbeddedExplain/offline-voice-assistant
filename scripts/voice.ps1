# Test the full voice loop with a WAV file (16 kHz mono recommended). Requires the 'voice' profile.
# Usage: .\scripts\voice.ps1 question.wav   -> writes reply.wav and prints transcript/answer/timings
param([Parameter(Mandatory)][string]$Wav, [string]$Gateway = "http://localhost:8000", [string]$Out = "reply.wav")

$res = Invoke-WebRequest -Method Post -Uri "$Gateway/voice" -ContentType "audio/wav" -InFile $Wav -OutFile $Out -PassThru
Write-Host "You said : $([uri]::UnescapeDataString($res.Headers['X-Transcript']))"
Write-Host "Answer   : $([uri]::UnescapeDataString($res.Headers['X-Answer']))" -ForegroundColor Cyan
Write-Host "Timings  : $($res.Headers['X-Timings'])"
Write-Host "Reply audio saved to $Out"
