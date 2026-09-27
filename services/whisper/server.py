"""Minimal faster-whisper HTTP server: POST /transcribe (WAV body) -> {"text": "..."}."""

import io
import os

from fastapi import FastAPI, Request
from fastapi.concurrency import run_in_threadpool
from faster_whisper import WhisperModel

MODEL = os.getenv("WHISPER_MODEL", "base")
DEVICE = os.getenv("WHISPER_DEVICE", "cpu")
COMPUTE_TYPE = os.getenv("WHISPER_COMPUTE_TYPE", "int8")
LANGUAGE = os.getenv("WHISPER_LANGUAGE") or None

model = WhisperModel(MODEL, device=DEVICE, compute_type=COMPUTE_TYPE, download_root="/models")
app = FastAPI(title="whisper")


def _transcribe(audio: bytes) -> dict:
    segments, info = model.transcribe(io.BytesIO(audio), language=LANGUAGE, beam_size=1, vad_filter=True)
    text = " ".join(s.text.strip() for s in segments)
    return {"text": text, "language": info.language, "duration_s": round(info.duration, 2)}


@app.get("/health")
async def health():
    return {"status": "ok", "model": MODEL, "device": DEVICE}


@app.post("/transcribe")
async def transcribe(request: Request):
    return await run_in_threadpool(_transcribe, await request.body())
