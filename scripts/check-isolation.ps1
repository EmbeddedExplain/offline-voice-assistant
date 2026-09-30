# Episode 4: check (and optionally enforce) that nothing leaves for the Internet.
#
#   .\scripts\check-isolation.ps1            # tests + 20 s packet capture during real requests
#   .\scripts\check-isolation.ps1 -Block     # add a firewall rule: the gateway can no longer reach the Internet
#   .\scripts\check-isolation.ps1 -Unblock   # remove that rule
#
# How it works:
#   - Outbound test: each container tries a TCP connection to 1.1.1.1:443 and a DNS lookup.
#   - Firewall: an iptables rule in the DOCKER-USER chain of the Docker Desktop VM drops anything
#     the `lan` network (the gateway) forwards to a non-private address. Not persistent: it disappears
#     when Docker Desktop restarts, so run -Block again after a restart.
#   - Capture: tcpdump on the bridges of both Docker networks while /ask (and /voice) run. The pcap files
#     land in captures\ and can be opened in Wireshark.
# Needs the nicolaka/netshoot image (tcpdump + iptables), pulled once while online.
param([switch]$Block, [switch]$Unblock, [int]$CaptureSeconds = 20, [string]$Gateway = "http://localhost:8000")

Set-Location (Split-Path $PSScriptRoot -Parent)
$Tag = "offline-assistant-egress-block"
$Private = "172.16.0.0/12"  # Docker bridges; replies to the Core2 go back through 172.19.0.1

function Get-Bridge([string]$Network) {
    "br-" + (docker network inspect "offline-assistant_$Network" --format '{{.Id}}').Substring(0, 12)
}
function Invoke-VM([string]$Cmd) {
    # Runs a shell command in the Docker Desktop VM's network namespace
    docker run --rm --net=host --privileged -v "${PWD}\captures:/cap" nicolaka/netshoot sh -c $Cmd
}

$lanBridge = Get-Bridge "lan"
$internalBridge = Get-Bridge "internal"
$rule = "DOCKER-USER -i $lanBridge ! -d $Private -m comment --comment $Tag -j DROP"

if ($Block) {
    Invoke-VM "iptables -C $rule 2>/dev/null || iptables -I $rule"
    Write-Host "Egress from the gateway ($lanBridge) to the Internet is now dropped." -ForegroundColor Green
    exit
}
if ($Unblock) {
    Invoke-VM "while iptables -D $rule 2>/dev/null; do :; done"
    Write-Host "Egress block removed." -ForegroundColor Yellow
    exit
}

Write-Host "==> 1. Can each container reach the Internet? (TCP 1.1.1.1:443, DNS example.com)"
$py = "import socket;s=socket.socket();s.settimeout(3);r=s.connect_ex(('1.1.1.1',443));print('TCP '+('REACHABLE' if r==0 else 'blocked'));" +
      "exec('try:\n socket.gethostbyname(\'example.com\');print(\'DNS REACHABLE\')\nexcept Exception:print(\'DNS blocked\')')"
$services = docker compose ps --services
foreach ($svc in "gateway", "whisper", "piper") {
    if ($services -notcontains $svc) { continue }
    $out = (docker compose exec -T $svc python -c $py) -join ", "
    $color = if ($out -match "REACHABLE") { "Yellow" } else { "Green" }
    Write-Host ("  {0,-8} {1}" -f $svc, $out) -ForegroundColor $color
}
docker compose exec -T ollama timeout 3 bash -c 'exec 3<>/dev/tcp/1.1.1.1/443' *> $null
$out = if ($LASTEXITCODE -eq 0) { "TCP REACHABLE" } else { "TCP blocked" }
Write-Host ("  {0,-8} {1}" -f "ollama", $out) -ForegroundColor $(if ($out -match "REACHABLE") { "Yellow" } else { "Green" })

$blocked = (Invoke-VM "iptables -S DOCKER-USER | grep -c $Tag") -as [int]
Write-Host ("  Gateway egress firewall rule: " + $(if ($blocked -gt 0) { "ON" } else { "OFF (run with -Block)" }))

Write-Host "`n==> 2. Capturing packets on both Docker networks for $CaptureSeconds s while requests run"
New-Item -ItemType Directory -Force captures | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$capture = Start-Job -ArgumentList $PWD, $lanBridge, $internalBridge, $stamp, $CaptureSeconds -ScriptBlock {
    param($dir, $lan, $internal, $stamp, $secs)
    docker run --rm --net=host --privileged -v "${dir}\captures:/cap" nicolaka/netshoot sh -c `
        "timeout $secs tcpdump -i $lan -nn -w /cap/lan-$stamp.pcap 2>/dev/null & timeout $secs tcpdump -i $internal -nn -w /cap/internal-$stamp.pcap 2>/dev/null & wait"
}
Start-Sleep -Seconds 3
$body = @{ question = "Why is local AI good for privacy?" } | ConvertTo-Json
Invoke-RestMethod -Method Post -Uri "$Gateway/ask" -ContentType "application/json" -Body $body | Out-Null
Write-Host "  /ask done"
if ((Test-Path captures\question.wav) -and ($services -contains "whisper")) {
    Invoke-WebRequest -Method Post -Uri "$Gateway/voice" -ContentType "audio/wav" -InFile captures\question.wav -UseBasicParsing | Out-Null
    Write-Host "  /voice done"
}
Wait-Job $capture | Out-Null
Remove-Job $capture

# Count packets whose destination is outside the private ranges (i.e. headed for the Internet)
$public = "not (dst net 10.0.0.0/8 or dst net 172.16.0.0/12 or dst net 192.168.0.0/16 or multicast or broadcast)"
foreach ($net in "lan", "internal") {
    $file = "$net-$stamp.pcap"
    $total = (Invoke-VM "tcpdump -nn -r /cap/$file 2>/dev/null | wc -l").Trim()
    $out = (Invoke-VM "tcpdump -nn -r /cap/$file '$public' 2>/dev/null | wc -l").Trim()
    $color = if ([int]$out -eq 0) { "Green" } else { "Red" }
    Write-Host ("  {0,-9} {1,5} packets captured, {2} to a public address   (captures\{3})" -f $net, $total, $out, $file) -ForegroundColor $color
    if ([int]$out -gt 0) { Invoke-VM "tcpdump -nn -r /cap/$file '$public' 2>/dev/null | head -5" }
}
