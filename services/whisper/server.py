"""Minimal faster-whisper HTTP server: POST /transcribe (WAV body) -> {"text": "..."}."""

import io
import os
import wave

import numpy as np
from fastapi import FastAPI, HTTPException, Request
from fastapi.concurrency import run_in_threadpool
from faster_whisper import WhisperModel

MODEL = os.getenv("WHISPER_MODEL", "base")
DEVICE = os.getenv("WHISPER_DEVICE", "cpu")
COMPUTE_TYPE = os.getenv("WHISPER_COMPUTE_TYPE", "int8")
LANGUAGE = os.getenv("WHISPER_LANGUAGE") or None

model = WhisperModel(MODEL, device=DEVICE, compute_type=COMPUTE_TYPE, download_root="/models")
app = FastAPI(title="whisper")


def _load_wav(audio: bytes) -> np.ndarray:
    """16-bit PCM WAV -> mono float32 at 16 kHz, the format Whisper expects.

    Decoding here (instead of letting faster-whisper call PyAV/ffmpeg) avoids a version
    mismatch between the two and keeps the input format explicit.
    """
    with wave.open(io.BytesIO(audio)) as w:
        if w.getsampwidth() != 2:
            raise ValueError("expected 16-bit PCM WAV")
        rate, channels = w.getframerate(), w.getnchannels()
        pcm = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float32) / 32768.0
    if channels > 1:
        pcm = pcm.reshape(-1, channels).mean(axis=1)
    if rate != 16000:  # linear resampling is enough for speech
        pcm = np.interp(np.arange(0, len(pcm), rate / 16000), np.arange(len(pcm)), pcm).astype(np.float32)
    return pcm


def _transcribe(audio: bytes) -> dict:
    try:
        samples = _load_wav(audio)
    except (wave.Error, ValueError, EOFError) as exc:
        raise HTTPException(400, f"invalid WAV: {exc}") from exc
    segments, info = model.transcribe(samples, language=LANGUAGE, beam_size=1, vad_filter=True)
    text = " ".join(s.text.strip() for s in segments)
    return {"text": text, "language": info.language, "duration_s": round(info.duration, 2)}


@app.get("/health")
async def health():
    return {"status": "ok", "model": MODEL, "device": DEVICE}


@app.post("/transcribe")
async def transcribe(request: Request):
    return await run_in_threadpool(_transcribe, await request.body())
