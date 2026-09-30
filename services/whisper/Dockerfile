FROM python:3.12-slim

ARG WHISPER_MODEL=base
WORKDIR /srv
# requests: imported by faster-whisper 1.1 but no longer pulled in by recent huggingface_hub
RUN pip install --no-cache-dir "faster-whisper==1.1.*" "requests==2.*" "fastapi==0.115.*" "uvicorn==0.32.*"

# Download the model at build time (build has Internet); runtime runs offline.
RUN python -c "from faster_whisper import WhisperModel; WhisperModel('${WHISPER_MODEL}', device='cpu', compute_type='int8', download_root='/models')"
ENV HF_HUB_OFFLINE=1

COPY server.py .
EXPOSE 9000
CMD ["uvicorn", "server:app", "--host", "0.0.0.0", "--port", "9000"]
