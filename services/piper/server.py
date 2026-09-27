"""Minimal Piper TTS HTTP server: POST /synthesize {"text": "..."} -> WAV."""

import io
import os
import wave

from fastapi import FastAPI, Response
from fastapi.concurrency import run_in_threadpool
from piper import PiperVoice
from pydantic import BaseModel, Field

VOICE_NAME = os.getenv("PIPER_VOICE", "en_US-lessac-medium")
voice = PiperVoice.load(f"/voices/{VOICE_NAME}.onnx")
app = FastAPI(title="piper")


class SynthRequest(BaseModel):
    text: str = Field(min_length=1, max_length=1000)


def _synthesize(text: str) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wav_file:
        if hasattr(voice, "synthesize_wav"):  # piper-tts >= 1.3
            voice.synthesize_wav(text, wav_file)
        else:
            voice.synthesize(text, wav_file)
    return buf.getvalue()


@app.get("/health")
async def health():
    return {"status": "ok", "voice": VOICE_NAME}


@app.post("/synthesize")
async def synthesize(req: SynthRequest):
    return Response(await run_in_threadpool(_synthesize, req.text), media_type="audio/wav")
