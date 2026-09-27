"""Gateway between the M5Stack Core2 and the local model containers.

POST /ask    JSON {"question": "..."}  -> JSON {"answer": "...", "timings_ms": {...}}
POST /voice  body = WAV (16 kHz mono)  -> WAV reply, with X-Transcript / X-Answer / X-Timings headers

Every request appends one row to TIMINGS_FILE (CSV), which is the raw data for episode 3.
"""

import csv
import json
import logging
import os
import time
from datetime import datetime, timezone
from pathlib import Path
from urllib.parse import quote

import httpx
from fastapi import FastAPI, HTTPException, Request, Response
from pydantic import BaseModel, Field

OLLAMA_URL = os.getenv("OLLAMA_URL", "http://ollama:11434")
WHISPER_URL = os.getenv("WHISPER_URL", "http://whisper:9000")
PIPER_URL = os.getenv("PIPER_URL", "http://piper:9001")
LLM_MODEL = os.getenv("LLM_MODEL", "llama3.2:3b")
LLM_MAX_TOKENS = int(os.getenv("LLM_MAX_TOKENS", "80"))
LLM_NUM_CTX = int(os.getenv("LLM_NUM_CTX", "2048"))
SYSTEM_PROMPT = os.getenv(
    "SYSTEM_PROMPT",
    "You are a helpful voice assistant running fully offline on a local computer. "
    "Answer in one or two short sentences. Plain text only: no markdown, no lists, no emoji.",
)
TIMINGS_FILE = Path(os.getenv("TIMINGS_FILE", "/data/timings.csv"))
TIMING_FIELDS = [
    "timestamp", "endpoint", "model", "stt_ms", "llm_ms", "tts_ms", "total_ms",
    "llm_load_ms", "llm_prompt_ms", "llm_gen_ms", "llm_tokens",
    "audio_in_bytes", "audio_out_bytes", "question", "answer",
]

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("gateway")

app = FastAPI(title="Offline assistant gateway")
client = httpx.AsyncClient(timeout=httpx.Timeout(180.0, connect=5.0))


class AskRequest(BaseModel):
    question: str = Field(min_length=1, max_length=500)


def ms_since(start: float) -> int:
    return round((time.perf_counter() - start) * 1000)


def record_timings(row: dict) -> None:
    row = {"timestamp": datetime.now(timezone.utc).isoformat(timespec="seconds"), "model": LLM_MODEL, **row}
    log.info("timings %s", {k: v for k, v in row.items() if k.endswith("_ms")})
    try:
        TIMINGS_FILE.parent.mkdir(parents=True, exist_ok=True)
        new_file = not TIMINGS_FILE.exists()
        with TIMINGS_FILE.open("a", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=TIMING_FIELDS, extrasaction="ignore")
            if new_file:
                writer.writeheader()
            writer.writerow(row)
    except OSError as exc:
        log.warning("could not write timings: %s", exc)


async def ask_llm(question: str) -> tuple[str, dict]:
    start = time.perf_counter()
    try:
        r = await client.post(
            f"{OLLAMA_URL}/api/chat",
            json={
                "model": LLM_MODEL,
                "stream": False,
                "messages": [
                    {"role": "system", "content": SYSTEM_PROMPT},
                    {"role": "user", "content": question},
                ],
                "options": {"num_predict": LLM_MAX_TOKENS, "num_ctx": LLM_NUM_CTX},
            },
        )
        r.raise_for_status()
    except httpx.HTTPError as exc:
        raise HTTPException(502, f"ollama error: {exc}") from exc
    data = r.json()
    ns = 1_000_000
    timings = {
        "llm_ms": ms_since(start),
        "llm_load_ms": data.get("load_duration", 0) // ns,
        "llm_prompt_ms": data.get("prompt_eval_duration", 0) // ns,
        "llm_gen_ms": data.get("eval_duration", 0) // ns,
        "llm_tokens": data.get("eval_count", 0),
    }
    return data["message"]["content"].strip(), timings


@app.get("/health")
async def health():
    return {"status": "ok", "model": LLM_MODEL}


@app.post("/ask")
async def ask(req: AskRequest):
    start = time.perf_counter()
    answer, timings = await ask_llm(req.question)
    timings["total_ms"] = ms_since(start)
    record_timings({"endpoint": "ask", "question": req.question, "answer": answer, **timings})
    return {"answer": answer, "timings_ms": timings}


@app.post("/voice")
async def voice(request: Request):
    start = time.perf_counter()
    audio_in = await request.body()
    if len(audio_in) < 1000:
        raise HTTPException(400, "expected a WAV body")

    t = time.perf_counter()
    try:
        r = await client.post(f"{WHISPER_URL}/transcribe", content=audio_in, headers={"Content-Type": "audio/wav"})
        r.raise_for_status()
    except httpx.HTTPError as exc:
        raise HTTPException(502, f"whisper error (is the 'voice' profile running?): {exc}") from exc
    transcript = r.json()["text"].strip()
    stt_ms = ms_since(t)

    if transcript:
        answer, timings = await ask_llm(transcript)
    else:
        answer, timings = "Sorry, I didn't catch that.", {"llm_ms": 0}

    t = time.perf_counter()
    try:
        r = await client.post(f"{PIPER_URL}/synthesize", json={"text": answer})
        r.raise_for_status()
    except httpx.HTTPError as exc:
        raise HTTPException(502, f"piper error: {exc}") from exc
    audio_out = r.content
    tts_ms = ms_since(t)

    timings.update(stt_ms=stt_ms, tts_ms=tts_ms, total_ms=ms_since(start))
    record_timings({
        "endpoint": "voice", "question": transcript, "answer": answer,
        "audio_in_bytes": len(audio_in), "audio_out_bytes": len(audio_out), **timings,
    })
    # Headers must be ASCII: percent-encode the text, the Core2 decodes it.
    return Response(
        content=audio_out,
        media_type="audio/wav",
        headers={
            "X-Transcript": quote(transcript),
            "X-Answer": quote(answer),
            "X-Timings": json.dumps(timings),
        },
    )
