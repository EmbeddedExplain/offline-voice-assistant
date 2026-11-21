# Offline Voice Assistant — M5Stack Core2 × Local LLM

📺 Built episode by episode on YouTube: https://www.youtube.com/@EmbeddedExplain

A fully offline AI assistant: an **M5Stack Core2** (ESP32) talks over local Wi-Fi to a PC running
**Ollama**, **faster-whisper** and **Piper** in Docker. Once installed, no answer goes through the cloud — and we measure it.

```
┌──────────────┐   local Wi-Fi (HTTP)   ┌──────────────────────────────────────────┐
│ M5Stack Core2│ ─────────────────────► │ PC — Docker Compose                      │
│              │   POST /ask   (text)   │  gateway (FastAPI) :8000  ← only LAN port │
│ mic / screen │   POST /voice (WAV)    │   ├── whisper (speech → text)   internal │
│ speaker      │ ◄───────────────────── │   ├── ollama  (text → answer)   network, │
└──────────────┘   JSON / WAV reply     │   └── piper   (answer → speech) no Internet
                                        └──────────────────────────────────────────┘
```

## Status

| Episode | Feature | Status |
|---|---|---|
| 1 | Core2 asks preset questions → answer on screen (`/ask`) | ✅ firmware + gateway |
| 2 | Push-to-talk voice loop (`/voice`): hold the screen, speak, release | ✅ firmware + gateway/whisper/piper |
| 3 | Per-stage latency, model comparison, power | ✅ `data/timings.csv` + `scripts/benchmark.ps1` |
| 4 | Prove zero bytes leave the network (`tcpdump`) | ✅ `scripts/check-isolation.ps1` |

## Requirements

- Docker Desktop (WSL2 backend). For NVIDIA GPUs: recent driver; GPU passthrough is built into Docker Desktop.
- VS Code + the **PlatformIO IDE** extension (recommended extensions are suggested on open).
- An M5Stack Core2 and a 2.4 GHz Wi-Fi network shared with the PC.

## Quick start

```powershell
# 1. Configuration
copy .env.example .env
copy firmware\include\secrets.example.h firmware\include\secrets.h   # then edit Wi-Fi + PC IP

# 2. Start the stack (text only) and download the model once (needs Internet just for this step)
docker compose up -d --build
.\scripts\pull-model.ps1

# 3. Check Ollama is on the GPU, not the CPU
docker compose logs ollama | Select-String -Pattern "gpu|cuda|compute"

# 4. Test without the Core2, twice: the 1st call loads the model (cold), the 2nd is the real latency (warm)
.\scripts\ask.ps1 "What is an ESP32?"
.\scripts\ask.ps1 "What is an ESP32?"

# 5. Test from another device on the LAN (e.g. your phone): http://<PC-IP>:8000/docs
#    If this works, firewall + port are fine and the Core2 will work too.

# 6. Only then flash the Core2: open offline-assistant.code-workspace, then PlatformIO → firmware → Upload
```

Windows Firewall: allow inbound TCP 8000 on your *private* network so the Core2 can reach the gateway.

### Troubleshooting

- **Core2 won't join Wi-Fi**: the ESP32 is 2.4 GHz only. If your router broadcasts 2.4 and 5 GHz under one
  name, try a dedicated 2.4 GHz SSID.
- **Worked yesterday, not today**: the PC's IP changed. Reserve it (DHCP reservation) in your router.
- **No COM port when flashing**: install the USB driver for your Core2's chip (CH9102 or CP2104, depending on revision).
- **Slow first answer**: normal, Ollama loads the model. It then stays loaded for `OLLAMA_KEEP_ALIVE` (30 min).

Voice loop (episode 2): `docker compose --profile voice up -d --build`, then `.\scripts\voice.ps1 question.wav`
(16-bit PCM WAV). On the Core2, hold a finger on the screen, speak, release: the reply plays on the speaker.

Benchmark (episode 3): `.\scripts\benchmark.ps1` measures cold and warm latency for every installed model
and prints mean ± standard deviation. Pull extra models first, e.g. `.\scripts\pull-model.ps1 llama3.2:1b`.

VS Code tasks (`Ctrl+Shift+P` → *Run Task*) wrap all of the above.

## Choosing a model

| Machine | LLM (4-bit) | Whisper |
|---|---|---|
| GPU ≥ 8 GB VRAM / Mac ≥ 16 GB | 7–8B | `small` |
| **GPU 6 GB (e.g. RTX 3050 Laptop)** | **3B default**, 8B fits alone | `base` on CPU |
| Laptop without GPU | 1–3B | `base` / `tiny` on CPU |

No NVIDIA GPU? Delete the `deploy:` block of the `ollama` service in `docker-compose.yml`.

## Network isolation

The `ollama`, `whisper` and `piper` containers only join the `internal` Docker network
(`internal: true`, no route out). Models are baked in at **build** time (whisper, piper) or pulled via a
temporary override (`compose.pull.yml`, used by `scripts/pull-model.ps1`). Only the gateway publishes a port.

**Caveat:** Docker can't publish a port from a container that is *only* on an internal network, so the
gateway also sits on the normal `lan` network, which has Internet access. Two extra measures close that gap:

- `dns: [127.0.0.1]` on the gateway: Docker's embedded DNS still resolves the service names, but external
  lookups go nowhere (Docker's resolver would otherwise bypass any firewall rule).
- `.\scripts\check-isolation.ps1 -Block` adds an iptables rule (`DOCKER-USER` chain, Docker Desktop VM) that
  drops anything the gateway sends to a public address. It is not persistent: re-run after restarting Docker.

`.\scripts\check-isolation.ps1` then tests outbound TCP and DNS from every container and captures packets
with `tcpdump` on both Docker networks during real `/ask` and `/voice` requests (pcap files in `captures/`).

## Timing data

Each request appends a row to `data/timings.csv`: STT / LLM / TTS / total milliseconds, Ollama load,
prompt and generation times, token count, audio sizes. That's the raw material for episode 3.

All measurements use a fixed context of 2048 tokens (`LLM_NUM_CTX`) so 1B / 3B / 8B are compared fairly.
A larger context uses more memory and changes latency, so your numbers will differ if you raise it.

## License

MIT
