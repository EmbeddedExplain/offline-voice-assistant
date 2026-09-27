FROM python:3.12-slim

ARG PIPER_VOICE=en_US-lessac-medium
WORKDIR /srv
RUN pip install --no-cache-dir "piper-tts==1.3.*" "fastapi==0.115.*" "uvicorn==0.32.*"

# Download the voice at build time (build has Internet); runtime runs offline.
COPY download_voice.py .
RUN python download_voice.py "${PIPER_VOICE}"

COPY server.py .
EXPOSE 9001
CMD ["uvicorn", "server:app", "--host", "0.0.0.0", "--port", "9001"]
